# dsh-media-mcp

把 fastaitoken 中转的**生成类模型**（图像、视频）接进 DeepSeek Harness 的 MCP 服务器。

DSH 的 LLM 适配器（`dsh-llm-pi-ai`）只走对话协议（`openai-completions` / `openai-responses` /
`anthropic-messages`），中转上新挂的 `seedance-*` / `veo*` / `wan-*` / `gpt-image-*` 都不是对话模型，
塞进模型列表只会得到 500。因此这里走 **MCP**：一个零依赖的 stdio 服务器，通过
`@deepseek-ai/dsh-mcp-client` 挂载，工具以 `mcp__media__generate_image` /
`mcp__media__generate_video` 出现在会话里。

## 用法

**1. 挂载（profile 的 cordis.patch.yml，或 `--patch` overlay）**

```yaml
- id: mcp-media
  name: '@deepseek-ai/dsh-mcp-client'
  config:
    serverName: media
    transport: stdio
    command: node
    args: ['/home/wcoom/dsh-media-mcp/server.mjs']
    cwd: /home/wcoom
    env:
      FASTAI_MEDIA_OUT: /home/wcoom/media-out
    # 关键：视频生成以分钟计（实测 4 秒视频约 8 分钟），
    # 默认的 60 秒 toolCallTimeoutMs 会在任务完成前掐断调用。
    toolCallTimeoutMs: 1500000
```

**2. 直接手测（不经 DSH）**

```sh
printf '%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"smoke","version":"1"}}}' \
  '{"jsonrpc":"2.0","method":"tools/list"}' \
  | node /home/wcoom/dsh-media-mcp/server.mjs
```

## 凭据

不含任何密钥。解析顺序：

1. 环境变量 `FASTAI_API_KEY`；
2. DSH 凭据库 `/root/.dsh/.credentials.yaml` 里的 `FASTAI_OPENAI_API_KEY`（可用 `DSH_CREDENTIALS` 指向别处）。

## 环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `FASTAI_API_KEY` | — | 优先使用的 key；缺省时读 DSH 凭据库 |
| `FASTAI_MEDIA_BASE_URL` | `https://www.fastaitoken.com/v1` | 中转端点（**必须带 `/v1`**） |
| `FASTAI_MEDIA_OUT` | `<cwd>/media-out` | 产物落盘目录 |
| `FASTAI_VIDEO_TIMEOUT_MS` | `1500000`（25 分钟） | 视频任务轮询上限 |
| `FASTAI_VIDEO_POLL_MS` | `10000` | 视频任务轮询间隔 |
| `FASTAI_IMAGE_TIMEOUT_MS` | `300000` | 单次图像生成超时 |

## 中转协议（2026-09-11 实测）

**图像**：OpenAI 兼容，一次性返回。

```
POST /v1/images/generations
{"model":"gpt-image-2","prompt":"...","n":1,"size":"1024x1024"}
→ 200 {"data":[{"url":"https://fast-img.ns.tisoz.com/...","revised_prompt":"..."}],"usage":{...}}
```

**视频**：Sora 风格的异步任务。产物**只能**从终态任务对象的 URL 字段取。

```
POST /v1/videos  {"model":"seedance-2.0-pro-token","prompt":"...","duration":4}
→ 202 {"id":"<task>","object":"video.generation.task","status":"processing","progress":3}

GET  /v1/videos/<task>
→ 200 {"status":"processing","progress":95}
→ 200 {"status":"completed","progress":100,
       "video_url":"https://...volcvideo.com/...","result_url":"...","download_url":"..."}
```

- `GET /v1/videos/<task>/content` **不可用**：404 `Videos API is not supported for this platform`。
- 产物是火山 VOD 签名链接，约 24 小时有效，需及时下载。
- 上游视频任务的返回体两套后端格式略有差异（有时带 `object`/`progress`，有时只有 `id`/`status`），
  本服务器按字段可选读取。

## 模型可用性（2026-09-11 实测）

| 模型 | 结果 |
|---|---|
| `gpt-image-2` | 可用（默认），约十几秒一张 1024×1024 |
| `gpt-image-2.5` | 未实测（中转对缺参数请求返回 502） |
| `seedance-2.0-pro-token` | 可用（默认），4 秒视频约 8 分钟、约 8.7 万 tokens |
| `veo3.1-time` | 能建任务（缺 prompt 时报 `prompt_required`），未跑完整流程 |
| `seedance-2.0-time` / `seedance-2.0-token` / `seedance-2.5-token` / `wan-3.0-time` / `minimax-h3-time` | 中转侧 400 `MEDIA_COUNT_UNAVAILABLE`（没有可用账号），暂不可用 |

⚠️ 生成调用会真实计费，且视频较贵；工具描述里已提示模型优先用图像。

## 已知限制

- 生成期间不向 MCP 客户端发进度通知，调用方只能等到终态（`toolCallTimeoutMs` 必须够大）。
- 不做任务持久化：进程重启后进行中的视频任务只能用任务 id 手工查
  `GET /v1/videos/<id>` 找回。
- 中转无任务列表端点（`GET /v1/videos` 404），无法枚举历史任务。
