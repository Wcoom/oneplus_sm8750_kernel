#!/usr/bin/env node
/**
 * DSH 媒体生成 MCP 服务器（fastaitoken 中转）。
 *
 * 把中转的生成类模型暴露成两个 MCP 工具，供 DSH 的 dsh-mcp-client 挂载：
 *   - generate_image：POST /v1/images/generations（OpenAI 兼容），返回图片 URL 后下载到本地
 *   - generate_video：POST /v1/videos 建任务（Sora 风格），轮询 GET /v1/videos/<id>，
 *     完成后从任务对象的 download_url 取产物
 *
 * 协议要点（均经真实调用验证，2026-09-11）：
 *   - 视频是异步任务：建任务返回 202 + {id, status, progress}，终态对象带
 *     video_url / result_url / download_url（火山 VOD 签名链接，约 24 小时有效）。
 *   - GET /v1/videos/<id>/content 不可用（404 "Videos API is not supported for this platform"），
 *     只能走 download_url。
 *   - 生成耗时以分钟计（实测 4 秒视频约 8 分钟），故调用方必须放宽工具超时
 *     （dsh-mcp-client 的 toolCallTimeoutMs 默认仅 60 秒）。
 *
 * 凭据来源：环境变量 FASTAI_API_KEY 优先，否则读 DSH 凭据库的 FASTAI_OPENAI_API_KEY。
 * 本文件不含任何密钥。
 *
 * stdout 只写 JSON-RPC（MCP stdio 传输按行分隔），日志一律走 stderr。
 */

import { createInterface } from 'node:readline'
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { randomUUID } from 'node:crypto'
import { join, resolve } from 'node:path'

const BASE_URL = (process.env.FASTAI_MEDIA_BASE_URL ?? 'https://www.fastaitoken.com/v1').replace(/\/+$/, '')
const OUT_DIR = resolve(process.env.FASTAI_MEDIA_OUT ?? join(process.cwd(), 'media-out'))
const VIDEO_TIMEOUT_MS = Number(process.env.FASTAI_VIDEO_TIMEOUT_MS ?? 25 * 60 * 1000)
const VIDEO_POLL_MS = Number(process.env.FASTAI_VIDEO_POLL_MS ?? 10_000)
const IMAGE_TIMEOUT_MS = Number(process.env.FASTAI_IMAGE_TIMEOUT_MS ?? 5 * 60 * 1000)

/** 写一行诊断到 stderr；stdout 留给 JSON-RPC。 */
function log(message) {
  process.stderr.write(`[media-mcp] ${message}\n`)
}

/** 发一条 JSON-RPC 响应。 */
function send(message) {
  process.stdout.write(`${JSON.stringify(message)}\n`)
}

/**
 * 解析 API key：优先环境变量，其次 DSH 凭据库（单一事实源，避免密钥进配置文件）。
 * @returns {string} 可用的 API key。
 */
function apiKey() {
  if (process.env.FASTAI_API_KEY) return process.env.FASTAI_API_KEY
  const credentialsPath = process.env.DSH_CREDENTIALS ?? '/root/.dsh/.credentials.yaml'
  if (existsSync(credentialsPath)) {
    const matched = readFileSync(credentialsPath, 'utf8').match(/^\s*FASTAI_OPENAI_API_KEY:\s*(\S+)\s*$/m)
    if (matched) return matched[1]
  }
  throw new Error(
    `没有可用的凭据：请设置环境变量 FASTAI_API_KEY，或在 ${credentialsPath} 中配置 FASTAI_OPENAI_API_KEY`,
  )
}

/**
 * 带超时的 JSON 请求。
 * @param {string} path 相对 BASE_URL 的路径。
 * @param {{method?: string, body?: unknown, timeoutMs?: number}} options 请求选项。
 * @returns {Promise<{status: number, json: any, text: string}>} 状态码与解析结果。
 */
async function request(path, options = {}) {
  const { method = 'GET', body, timeoutMs = 120_000, retries = 2 } = options
  let lastFailure = ''
  for (let attempt = 0; attempt <= retries; attempt += 1) {
    try {
      const response = await fetch(`${BASE_URL}${path}`, {
        method,
        headers: {
          Authorization: `Bearer ${apiKey()}`,
          ...(body === undefined ? {} : { 'Content-Type': 'application/json' }),
        },
        ...(body === undefined ? {} : { body: JSON.stringify(body) }),
        signal: AbortSignal.timeout(timeoutMs),
      })
      const text = await response.text()
      let json
      try {
        json = JSON.parse(text)
      } catch {
        json = undefined
      }
      return { status: response.status, json, text }
    } catch (error) {
      // 只重试网络层失败：该中转经实测会偶发中断连接，而 HTTP 错误码是明确答复，重试无益。
      const cause = error?.cause?.code ?? error?.cause?.message ?? ''
      lastFailure = `${error?.message}${cause ? `（${cause}）` : ''}`
      log(`${method} ${path} 第 ${attempt + 1} 次失败：${lastFailure}`)
      if (attempt < retries) await new Promise((wake) => setTimeout(wake, 2000))
    }
  }
  throw new Error(`${method} ${path} 网络请求失败：${lastFailure}`)
}

/** 从错误响应里抽出可读信息。 */
function errorMessage(result, fallback) {
  const error = result.json?.error
  if (typeof error === 'string') return error
  if (error?.message) return error.message
  return `${fallback}（HTTP ${result.status}）${result.text.slice(0, 200)}`
}

/** 生成产物文件名前缀：本地时间戳 + 模型名。 */
function fileStem(model) {
  const now = new Date()
  const pad = (value) => String(value).padStart(2, '0')
  const stamp = `${now.getFullYear()}${pad(now.getMonth() + 1)}${pad(now.getDate())}`
    + `-${pad(now.getHours())}${pad(now.getMinutes())}${pad(now.getSeconds())}`
  return `${stamp}-${String(model).replace(/[^\w.-]+/g, '_')}`
}

/**
 * 下载产物到 OUT_DIR。
 * @param {string} url 上游返回的产物地址。
 * @param {string} stem 文件名主体（不含扩展名）。
 * @param {string} fallbackExt 无法从响应推断扩展名时使用的后缀。
 * @returns {Promise<{path: string, bytes: number}>} 落盘路径与字节数。
 */
async function download(url, stem, fallbackExt) {
  const response = await fetch(url, { signal: AbortSignal.timeout(10 * 60 * 1000) })
  if (!response.ok) throw new Error(`下载产物失败：HTTP ${response.status}`)
  const bytes = Buffer.from(await response.arrayBuffer())
  const contentType = response.headers.get('content-type') ?? ''
  const fromUrl = new URL(url).pathname.match(/\.([a-z0-9]{2,5})$/i)?.[1]
  const ext = contentType.includes('mp4') ? 'mp4'
    : contentType.includes('webm') ? 'webm'
      : contentType.includes('png') ? 'png'
        : contentType.includes('jpeg') ? 'jpg'
          : (fromUrl ?? fallbackExt)
  mkdirSync(OUT_DIR, { recursive: true })
  const path = join(OUT_DIR, `${stem}.${ext}`)
  writeFileSync(path, bytes)
  return { path, bytes: bytes.length }
}

/** 生成一张图片。 */
async function generateImage(args) {
  const prompt = String(args.prompt ?? '').trim()
  if (!prompt) throw new Error('prompt 不能为空')
  const model = String(args.model ?? 'gpt-image-2')
  const body = { model, prompt, n: Number(args.n ?? 1) }
  if (args.size) body.size = String(args.size)
  const result = await request('/images/generations', { method: 'POST', body, timeoutMs: IMAGE_TIMEOUT_MS })
  if (result.status !== 200) throw new Error(errorMessage(result, `${model} 图像生成失败`))
  const items = Array.isArray(result.json?.data) ? result.json.data : []
  if (items.length === 0) throw new Error(`${model} 未返回图片数据`)
  const saved = []
  for (const [index, item] of items.entries()) {
    const url = item.url
    if (item.b64_json) {
      mkdirSync(OUT_DIR, { recursive: true })
      const path = join(OUT_DIR, `${fileStem(model)}-${index + 1}.png`)
      writeFileSync(path, Buffer.from(item.b64_json, 'base64'))
      saved.push({ path, bytes: Buffer.from(item.b64_json, 'base64').length })
      continue
    }
    if (!url) continue
    const suffix = items.length > 1 ? `-${index + 1}` : ''
    saved.push(await download(url, `${fileStem(model)}${suffix}`, 'png'))
  }
  if (saved.length === 0) throw new Error(`${model} 返回了响应但没有可下载的图片`)
  const lines = saved.map((item) => `- ${item.path}（${(item.bytes / 1024).toFixed(0)} KB）`)
  const revised = items[0]?.revised_prompt
  return [
    `已用 ${model} 生成 ${saved.length} 张图片：`,
    ...lines,
    ...(revised ? [`上游改写的提示词：${String(revised).split('\n')[0]}`] : []),
  ].join('\n')
}

/** 生成一段视频：建任务、轮询到终态、下载产物。 */
async function generateVideo(args) {
  const prompt = String(args.prompt ?? '').trim()
  if (!prompt) throw new Error('prompt 不能为空')
  const model = String(args.model ?? 'seedance-2.0-pro-token')
  const body = { model, prompt }
  if (args.duration !== undefined) body.duration = Number(args.duration)
  const created = await request('/videos', { method: 'POST', body, timeoutMs: 120_000 })
  const task = created.json ?? {}
  const taskId = task.id ?? task.task_id
  if (!taskId) throw new Error(errorMessage(created, `${model} 建任务失败`))

  const startedAt = Date.now()
  let last = task
  while (Date.now() - startedAt < VIDEO_TIMEOUT_MS) {
    if (last.status === 'completed' || last.status === 'succeeded' || last.status === 'success') break
    if (last.status === 'failed') {
      const reason = last.error_message ?? last.fail_reason ?? last.error_code ?? '未知原因'
      throw new Error(`${model} 任务 ${taskId} 失败：${reason}`)
    }
    await new Promise((wake) => setTimeout(wake, VIDEO_POLL_MS))
    const polled = await request(`/videos/${taskId}`, { timeoutMs: 60_000 })
    if (polled.json) last = polled.json
    log(`任务 ${taskId} 状态=${last.status ?? '?'} 进度=${last.progress ?? '?'}`)
  }
  const url = last.download_url ?? last.video_url ?? last.result_url
  if (!url) {
    throw new Error(
      `任务 ${taskId} 在 ${Math.round(VIDEO_TIMEOUT_MS / 1000)} 秒内未产出可下载地址`
      + `（最后状态：${last.status ?? '未知'}，进度：${last.progress ?? '未知'}）`,
    )
  }
  const saved = await download(url, fileStem(model), 'mp4')
  const seconds = Math.round((Date.now() - startedAt) / 1000)
  const usage = last.usage?.total_tokens ? `，计费 ${last.usage.total_tokens} tokens` : ''
  return [
    `已用 ${model} 生成视频（耗时约 ${seconds} 秒${usage}）：`,
    `- ${saved.path}（${(saved.bytes / 1024 / 1024).toFixed(1)} MB）`,
    `任务 id：${taskId}`,
  ].join('\n')
}

const TOOLS = [
  {
    name: 'generate_image',
    description: [
      '用中转的图像模型生成图片并保存到本地工作区。',
      '实测可用：gpt-image-2（默认）。返回本地文件路径，可直接用 read_image 查看。',
      '一次调用生成一张约需十几秒。',
    ].join(' '),
    inputSchema: {
      type: 'object',
      properties: {
        prompt: { type: 'string', description: '图片内容描述，建议包含画面、风格、构图等要素。' },
        model: { type: 'string', description: '图像模型 id，默认 gpt-image-2。' },
        size: { type: 'string', description: '图片尺寸，如 1024x1024；不传由中转决定。' },
        n: { type: 'integer', description: '生成张数，默认 1。' },
      },
      required: ['prompt'],
    },
  },
  {
    name: 'generate_video',
    description: [
      '用中转的视频模型生成视频并保存到本地工作区（异步任务：建任务后轮询到完成为止）。',
      '实测可用：seedance-2.0-pro-token（默认，4 秒约 8 分钟）、veo3.1-time。',
      '其余 seedance/wan/minimax 变体在中转侧报 MEDIA_COUNT_UNAVAILABLE，暂不可用。',
      '生成耗时以分钟计，调用方需要把工具超时放宽到 20 分钟以上。',
    ].join(' '),
    inputSchema: {
      type: 'object',
      properties: {
        prompt: { type: 'string', description: '视频内容描述。' },
        model: { type: 'string', description: '视频模型 id，默认 seedance-2.0-pro-token。' },
        duration: { type: 'integer', description: '时长（秒）；不传由中转按模型默认值决定。' },
      },
      required: ['prompt'],
    },
  },
]

/** 执行一次工具调用，返回 MCP 的调用结果。 */
async function callTool(name, args) {
  try {
    const text = name === 'generate_image' ? await generateImage(args)
      : name === 'generate_video' ? await generateVideo(args)
        : undefined
    if (text === undefined) {
      return { content: [{ type: 'text', text: `未知工具：${name}` }], isError: true }
    }
    return { content: [{ type: 'text', text }] }
  } catch (error) {
    log(`工具 ${name} 失败：${error?.message ?? error}`)
    return { content: [{ type: 'text', text: `${name} 失败：${error?.message ?? String(error)}` }], isError: true }
  }
}

const reader = createInterface({ input: process.stdin, terminal: false })
/** 进行中的请求数：stdin 关闭后要等它们收尾再退出，否则会打断生成任务。 */
let inFlight = 0
let stdinClosed = false

/** stdin 已关闭且没有进行中的请求时退出。 */
function maybeExit() {
  if (stdinClosed && inFlight === 0) process.exit(0)
}

reader.on('line', (line) => {
  const trimmed = line.trim()
  if (!trimmed) return
  let message
  try {
    message = JSON.parse(trimmed)
  } catch {
    log('忽略无法解析的行')
    return
  }
  const { id, method, params } = message
  inFlight += 1
  void (async () => {
    switch (method) {
      case 'initialize':
        send({
          jsonrpc: '2.0',
          id,
          result: {
            protocolVersion: params?.protocolVersion ?? '2024-11-05',
            capabilities: { tools: {} },
            serverInfo: { name: 'dsh-media-mcp', version: '1.0.0' },
          },
        })
        return
      case 'notifications/initialized':
      case 'notifications/cancelled':
        return
      case 'ping':
        send({ jsonrpc: '2.0', id, result: {} })
        return
      case 'tools/list':
        send({ jsonrpc: '2.0', id, result: { tools: TOOLS } })
        return
      case 'tools/call':
        send({
          jsonrpc: '2.0',
          id,
          result: await callTool(params?.name, params?.arguments ?? {}),
        })
        return
      default:
        if (id !== undefined) {
          send({ jsonrpc: '2.0', id, error: { code: -32601, message: `不支持的方法：${method}` } })
        }
    }
  })().catch((error) => {
    log(`处理 ${method} 时异常：${error?.message ?? error}`)
    if (id !== undefined) {
      send({ jsonrpc: '2.0', id, error: { code: -32603, message: String(error?.message ?? error) } })
    }
  }).finally(() => {
    inFlight -= 1
    maybeExit()
  })
})

reader.on('close', () => {
  log('stdin 已关闭')
  stdinClosed = true
  maybeExit()
})

log(`已启动：endpoint=${BASE_URL} out=${OUT_DIR} 会话=${randomUUID().slice(0, 8)}`)
