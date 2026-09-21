# /home/wcoom 工作区记忆（AGENTS.md）

> 本文件是 Codex 会话启动时**自动读取**的工作区持久记忆（项目级 `./AGENTS.md`）。
> 目的：让每个新会话快速恢复上下文 —— 目录结构、Codex 运行环境与操作惯例、内核工程工作流。
> 记忆读取层级（全部自动加载）：① `/root/.Codex/AGENTS.md`（全局）→ ② `/root/.Codex/projects/-home-wcoom/AGENTS.md`（项目记忆）→ ③ 本文件 → ④ 子项目 AGENTS.md（如 `oplus13/AGENTS.md`、`oplus13/ACK维护任务书.md`）。

## 1. 本工作区是什么

WSL2 Ubuntu 环境下的开发工作区，核心工作方向：

- **Google GKI Linux 内核工程**：Android 内核源码维护、定制、编译、打包、刷机（OnePlus 13 / SM8750）
- **DeepSeek Harness（DSH）**：位于 `deepseek-harness/`，agent 编排框架（Cordis 插件体系）
- 其余：内核模块/补丁实验、CC-Switch 等工具

## 2. 目录地图

| 路径 | 内容 |
|---|---|
| `oplus13/` | **OnePlus 13 (SM8750) 定制内核工作区**：`android_kernel_common_oneplus_sm8750`（内核源码）、`clang-19/`（工具链）、`AnyKernel3-6.6.112-NOKSU-OnePlus8Elite/`（刷机包模板）、`op_mods/`、`docs/`、`bpf.sh`、`AGENTS.md`（项目详细记忆）、`ACK维护任务书.md`（月度维护任务书） |
| `pddump/` | **原创 payload.bin 解包工具**（v1.0.0，Go，对标 payload-dumper-go）：多核并行解压（12 核实测 5.3 倍提速）、顺序写盘、稀疏输出；交付物 `pddump/dist/pddump-windows-amd64.zip`（Windows exe + 说明）；项目记忆见 `pddump/AGENTS.md`；构建需 `export PATH=/usr/local/go/bin:$PATH GOPROXY=https://goproxy.cn,direct`（本机网络受限，仅 goproxy.cn/dl.google.com/github git 可用） |
| `deepseek-harness/` | DeepSeek Harness（DSH）源码 checkout |
| `mihomo-ebpf-smart-export/` | **mihomo 透明代理移植项目**（ColorOS 15 设备，Magisk /data/adb/box 部署）：`repo/`（源码 git 仓库，分支 official-20260828 = 官方 metacubex/Alpha 061966e7 + 5 个本地定制 commit：ebpf 移植、smart LightGBM 移植、bypass 提前启动、Model.bin 加固、vernesong nodes filter 同步）、`deploy/`（部署资产）、`tools/`（bpf 工具 + git-gh-proxy.sh）、`MEMORY.md`（项目详细记忆，排障根因在此）、`README.md`；项目记忆在项目内，本文件只放指针 |
| `内核构建.sh` | 内核构建脚本（clang-19 + ccache 伪装，增量编译） |
| `dabao.sh` | AnyKernel3 刷机包打包脚本 |
| `android-ndk-r25c/` | Android NDK |
| `CC-Switch 3.20.3` | CC-Switch（Codex/Claude Code 配置切换工具），已安装（dpkg ii）；官方安装包 deb 已于 2026-09-21 校验安装后清理删除 |
| `mattpocock-skills/` | mattpocock/skills 仓库本地副本（**仅作参考/自定义**，不参与加载；技能已通过 Codex 插件提供，见 §4） |

## 3. Codex 运行环境

- **本体**：`/root/.local/bin/codex`（符号链接 → `codex-official-0.147.0` → `/root/.codex/packages/standalone/releases/0.147.0-.../bin/codex`，官方原版；`codex --version` 可查）
- **用户级配置**：`/root/.codex/config.toml`（model = `gpt-5.6-sol`、reasoning high、approvals_reviewer user）+ `/root/.codex/auth.json`（OpenAI 原生登录）；`/root/.codex/sessions/` 存会话 jsonl，`history.jsonl` 存用户输入摘要
- **项目级权限白名单**：`/home/wcoom/oplus13/.Codex/settings.local.json`（已授权 `Bash(git *)`、`Bash(curl *)`、`Bash(gh *)`、`Bash(python3 *)` 等，免确认执行）；`/root/.codex/rules/default.rules`（prefix_rule 白名单）
- **模型/API**：OpenAI 原生（auth.json），非 DeepSeek 中转；`/root/.codex/models_cache.json` 为模型目录缓存
- **汉化已清理（2026-08-30）**：曾于 08-30 由 Codex 会话源码级汉化（`/root/codex-0.147.0-zh` 18G 源码 + 1.2G 二进制 + cargo/rustup 工具链）并切换符号链接；已全部删除、恢复官方版、config.toml 中文指令行已移除、汉化会话记录已清理
- **Windows 桌面版**：`/mnt/c/Users/34073/.codex/`（桌面应用，经 WSL interop 起 app-server；不用时需手动终止）
- **默认语言**：与用户交流使用简体中文（工作区惯例），但 Codex CLI 本体为官方英文版

## 4. Skills 插件：mattpocock-skills（已引入并启用）

- **状态**：`mattpocock-skills@Codex-plugins-official` **v1.2.3 已安装且 enabled**（`Codex plugins list` 确认）
- **来源**：https://github.com/mattpocock/skills（本地副本：`/home/wcoom/mattpocock-skills/`）
- **安装位置**：`/root/.Codex/plugins/cache/Codex-plugins-official/mattpocock-skills/1.2.3/`（官方 marketplace 托管，自动随上游更新）
- **25 个技能**（`Codex plugin details mattpocock-skills` 可查清单）：
  - **入口/路由器**：`ask-matt`（先问它，按任务路由到具体技能）
  - **需求对齐**：`grill-with-docs`（代码类）、`grill-me`（非代码类）—— 动手前先"拷问"对齐需求
  - **工程流程**：`to-spec`、`to-tickets`（规格/工单化）、`tdd`、`code-review`、`diagnosing-bugs`、`resolving-merge-conflicts`
  - **建模与设计**：`domain-modeling`、`codebase-design`、`improve-codebase-architecture`、`wayfinder`、`wizard`
  - **执行类**：`implement`、`prototype`、`research`、`triage`、`handoff`、`teach`、`wait-what`、`to-questionnaire`、`writing-for-agents`、`setup-matt-pocock-skills`
- **常驻成本**：约 1,163 tok/会话（always-on）
- **管理命令**：`Codex plugins list` / `Codex plugin details <name>` / `Codex plugin disable|enable <name>` / `Codex plugin update`（如上游发布新版本）
- **新项目首次使用**：运行 `/setup-matt-pocock-skills` 配置（问题追踪器、triage 标签、文档保存位置）
- ⚠️ 勿再通过 `npx skills add` 或 `~/.Codex/skills` 链接重复安装（README 明确：两种方式只选其一，否则技能重复）

### 记忆与技能的分工边界（不冲突原则）

- **记忆 = 工作区事实与配置**：目录地图、命令、基线、决策、红线、技能配置位置（`docs/agents/*`）、会话惯例
- **技能 = 流程**：如何做某类事的步骤，由各 `SKILL.md` 自身定义，经 skill 工具加载
- 本文件链**不复制、不改写任何技能正文**，只引用技能名与配置位置；修改技能行为时改技能文件本身
- 技能消费记忆的方式：`docs/agents/issue-tracker.md`（issue 落盘约定）、`triage-labels.md`（标签映射）、`domain.md`（领域文档规则）——二者接口就是这三个文件，改动需两边一致

## 5. 常用操作速查（内核工程）

- **构建内核**：`bash /home/wcoom/内核构建.sh`（增量编译，**禁止随意 `make clean`**；ccache 位于 `oplus13/.ccache`，容量 5G）
- **打包刷机包**：`bash /home/wcoom/dabao.sh` → `AnyKernel3-<Image时间戳>.zip`（如 `AnyKernel3-20260811-1523.zip`）
- **ACK 月度维护**：按 `/home/wcoom/oplus13/ACK维护任务书.md` 执行（`.maintenance/state.json` 幂等断点续跑；红线：GKI ABI/KMI 不可破坏，禁止 `-X ours/-X theirs`）
- **内核源码**：`/home/wcoom/oplus13/android_kernel_common_oneplus_sm8750`，分支 `6.6.118-13T`
  - 远程：`origin`（上游 whitewhale0612，只拉勿推）、`ack`（google googlesource 官方源）、`github`（个人仓库 Wcoom/oneplus_sm8750_kernel，SSH ed25519 推送）
- **详细内核项目上下文**（五项定制、ABI 红线守点、ZRAM 配置注意事项等）见 `/home/wcoom/oplus13/AGENTS.md`

### mihomo 工程速查（2026-08-29 起）

- **网络链路（重要）**：WSL/Windows 直连 GitHub 超时、gh-proxy.com 被 fake-ip（198.18.0.111）污染；可行链路 = WSL → 手机 mihomo HTTP 代理（192.168.1.177:7890，wlan0 同网段）→ gh-proxy.com → GitHub 全链路 200。git 用法：`git -c http.proxy=http://192.168.1.177:7890 ls-remote https://gh-proxy.com/https://github.com/<repo>.git`；包装器 `mihomo-ebpf-smart-export/tools/git-gh-proxy.sh`（手机 IP 为 DHCP 动态，变了要改 PHONE_PROXY）
- **mihomo 构建/部署**：`GOOS=android GOARCH=arm64 CGO_ENABLED=0 go build -tags with_ebpf`（GOOS=android 才能读 Android 系统 CA 池；缺 with_ebpf 则 bpf fd=0 无劫持）；部署到 `/data/adb/box/bin/mihomo`，chown root:net_admin 后必须再 chmod 6755（chown 会清 setuid 位）；必须 setsid 启动；Windows adb 位于 `/mnt/d/刷机/platform-tools/adb.exe`（2026-08-30 核实；旧记录 /mnt/c/WINDOWS/system32 已失效；WSL 内无 adb）

## 6. 会话惯例

- 始终使用简体中文交流、解释、写注释与提交信息
- 涉及内核 ABI/KMI 红线、冲突合并时，严格执行 `ACK维护任务书.md` 的规则，宁可停下等人工确认
- 修改技能/工作流文件前先读取对应 AGENTS.md / 任务书，保持记忆文件与现状同步

### Git 纪律（所有代码用 git 管理，2026-08-15 起）

- 所有代码/脚本/文档变更都必须纳入 git；发现未纳入 git 的代码先 `git init` 或并入合适仓库再动手
- 工作流：确认干净起点 → 按仓库惯例开分支 → 变更 → 细粒度 add → 提交（简体中文信息，主题 ≤72 字符）→ 推送（按远程约定）
- 不入库：构建产物（out/、*.zip、Image）、ccache、密钥（遵守 .gitignore；内核仓库 `.maintenance/` 需 `git add -f`）
- 破坏性命令（`push --force`、`reset --hard`、`clean -fd`、`branch -D`、`checkout .` 等）执行前必须向用户说明并获得确认
- 里程碑打 tag；大轮合并前建备份分支；ABI/KMI 相关提交先经 `subagent_review` 审核
- **工作区根仓库**：`/home/wcoom` 已 `git init`（2026-08-15），管理根目录脚本与记忆文档（`内核构建.sh`、`dabao.sh`、`AGENTS.md` 等），`.gitignore` 排除各子项目与构建产物；子项目各有自己的仓库（内核、harness、mattpocock-skills）

## 7. DSH 接管（Codex 继任者 preset）

- **DeepSeek Harness（DSH）agent 已全面接管本工作区的工程工作**，预设：`claude-successor`（位于 `/root/.dsh/.agent-presets/claude-successor/`，由 `standard` 复制而来）
- 该 preset 已移植：① 本记忆链（本文件 + `oplus13/AGENTS.md` + `oplus13/docs/agents/*` 技能配置）② mattpocock-skills 全部 25 个工程技能（位于 preset 的 `skills/`，全部模型可调用，含 ask-matt 路由、grill-with-docs/grill-me 对齐、to-spec/to-tickets、tdd、code-review、triage、wayfinder 等）
- **记忆双向同步**：本文件链由 DSH 的 `dsh-agent-instructions` 自动读取（AGENTS.md/AGENTS.md 候选），DSH 会话开工前读、完工后把重要进展写回本文件链；Codex 与 DSH 共用同一套记忆与技能配置
- **含创造模式特性**（2026-08-15）：两个 Cordis 技能（`cordis-plugin-development`、`editing-cordis-compositions`，在 preset `skills/` 内）+ 双平面创作规则——可创作/校验其他 preset，但绝不改动部署自带 shipped preset（升级会覆盖）
- ⚠️ **tool-cordis 是进程单例**：其 Host Inspect Provider（Service/Event/Builtin/Tool）注册进进程级 `cordisInspect` 注册表，同一进程只允许一份。因此在 claude-successor 中该行**默认 disabled**，使其可与 cordis preset 共存：**动态插件工具集（cordis_inspect/define/run/stop/undefine）在 cordis 预设会话中使用**；claude-successor 保留两个 Cordis 技能与双平面创作规则，可用文件工具直接编辑 preset 组成。若日后彻底退役 cordis，移除该 disabled 标志即可启用 tool-cordis
- 技能配置（issue 追踪器 / triage 标签 / 领域文档）以 `oplus13/docs/agents/*` 为准，两个 agent 共用，改动需两边生效
- Codex 仍可用（`Codex` 命令），但默认工作由 DSH 的 `claude-successor` 预设会话承担

### 模型分工（2026-08-15 起生效）

- **编码 = DeepSeek-V4-Pro**：`claude-successor` 会话（部署默认模型已改为 `deepseek-v4-pro`），主 agent 负责全部代码编写与内核工程
- **审核 = DeepSeek-V4-Flash**：主 agent 用 `subagent_review` 工具后台委派 flash 子代理独立审核变更（重大变更合并/提交前执行）
- **记忆优化 = DeepSeek-V4-Flash**：主 agent 用 `subagent_memory` 工具后台委派 flash 子代理把新事实写入本记忆链（重要工作完成或会话收尾时执行）
- 记忆与技能不冲突：见 §4「记忆与技能的分工边界」——记忆只存事实与配置，技能流程以各 SKILL.md 为准

### 模型名与官方文档对齐（2026-09-10 起生效）

- **现行模型名只有两个**（来源：官方文档 `api-docs.deepseek.com/zh-cn` 与线上 `GET https://api.deepseek.com/models`，与实际调用三方实测一致）：`deepseek-flash`（模型版本 DeepSeek-V4.1-Flash，1M 上下文、输出上限 384K、支持图像理解与思考模式）；`deepseek-v4-pro`（DeepSeek-V4-Pro-0813，1M 上下文、输出上限 384K、不支持图像）
- **旧名的服务端行为**：`deepseek-v4-flash`、`deepseek-v4-flash-vision-exp`（对应模型已下线）仍可调用，但服务端路由到 V4.1-Flash，并把响应 `model` 字段改写为 `deepseek-flash`（实测确认）；`deepseek-chat`、`deepseek-reasoner` 同样被路由为 `deepseek-flash`。故本文件其余段落里以旧名指代的模型角色（如 §7「模型分工」的后台子代理）不改也能跑，但响应 `model` 字段与线上目录一律按新名理解
- **V4 Pro 下线计划**：自北京时间 2026-09-14 12:00 起，`deepseek-v4-pro` 的请求全部路由到 V4.1-Flash 并按 Flash 计费，V4 Pro 有序下线（至 V4.1 Pro 上线前）
- **价格**（元/百万 token，空闲/高峰）：flash 输入缓存未命中 1/2、输出 4/8；v4-pro 输入 4.5/9、输出 13.5/27
- **本机 DSH 配置改动（已提交）**：`/root/.dsh/settings.yaml` 的 `agent-default-model.model` 原本已是 `deepseek-flash`（正确），`subagent-model-selection.allowedModels` 由 deepseek-v4-flash / deepseek-v4-pro / deepseek-v4-flash-vision-exp 收敛为 `deepseek-flash` 与 `deepseek-v4-pro`（提交 86c1bf4，仓库 `/root/.dsh`）；`/root/.dsh/.agent-presets/claude-successor/agent.cordis.yml`（本文件旧文写作 `Codex-successor`，实际 preset 目录名为 `claude-successor`）的 `subagent_review` 与 `subagent_memory` 两个子代理 `agentOptions.model` 由 `deepseek-v4-flash` 改为 `deepseek-flash`，persona 中「运行在 DeepSeek-V4-Flash 上」等 3 处文本更新为 DeepSeek-V4.1-Flash（提交 760fdb5）；同一仓库另把上一会话遗留的暂存改动提交为 c2baae3（DSH 0.1.5-rc.1 standard 基线重同步：persona text→prefix/suffix、spawn 行 modelSelectionEnabled、补 present 行）
- **harness 源码改动（已提交）**：`deepseek-harness/` 提交 `12d109cbb0 feat(llm)!: default to the documented DeepSeek model names`（39 个文件）；`packages/llm/llm-deepseek/src/index.ts` 的 DEFAULT_MODELS 由 4 条收敛为 2 条（`deepseek-flash` + `deepseek-v4-pro`），`deepseek-flash` 承接原 v4-flash 的描述、图像能力与 `systemPromptUpdate: 'in-history'`；真实默认调用路径改为 `deepseek-flash`（ACP app 行 `packages/bundle/acp-app/cordis.patch.yml`、TypeScript SDK、subagent-dsh-sdk、web-search-deepseek、Python SDK 与示例）
- ⚠️ **与上游方向相反**：上游 2026-09-09 的 `441385fe38 retain V4 models alongside V41 Flash` 与 `0729dbec66 restore V4 Flash Vision Exp catalog entry` 刻意保留旧条目，本次经用户明确确认后移除；将来与上游同步/rebase 时需留意这处冲突。有意保留未改：`packages/client/connection/src/client/fixture.ts`（GUI 演示 fixture）、`apps/cli/tests/profiles/headless/*.patch.yml`（测试夹具）、`snapshots/**`（录制回放内容）、`apps/web/tests/scaffold.ts` 的 REPLAY_PROVIDERS
- **验证**：相关包单测 762 项全通过；`pnpm run test:docs` 16 项文档门禁全通过；`verify-translation-pairing` 789 对全部一致
- **生效方式**：DSH 以 `node --import tsx/esm apps/cli/src/bin.ts` 从源码加载模块、无热重载——DEFAULT_MODELS 等源码改动必须重启 `dsh`（`dsh stop && dsh`）才对运行中的实例生效；settings.yaml 与 preset 的改动同样需重启新会话才可靠生效
- **快照修复（独立审核推翻首次归因）**：`web-search-endpoint-guidance` 场景回放曾失败，最初误判为「上游改默认模型导致录制过时、与本改动无关」；审核用单行对照实验证明是本次改动引入——该场景 composition 里的 `web-search-deepseek` 只配了 `apiKey`/`baseURL`、未配 `model`，走的是 `DEEPSEEK_DEFAULT_MODEL`（本次由旧名改为 `deepseek-flash`），而录制体 `web/deepseek-search-llm-request` 记的是旧名，逐字段比对即红。修法：在 `snapshots/session/web-search-endpoint-guidance/cordis.snapshot.yml` 显式钉 `model: deepseek-v4-flash` 保留录制语义（与 headless profile 夹具、`apps/web/tests/scaffold.ts` 钉录制值的既有做法一致），Agent Note 中「快照不受影响」的错误表述已同步改正
- **Claude Code 侧同步**：`/root/.claude/settings.json` 的 `ANTHROPIC_MODEL` 与 `ANTHROPIC_DEFAULT_HAIKU_MODEL` 由旧名改为 `deepseek-flash`（sonnet/opus/fable 仍为 `deepseek-v4-pro[1M]`，实测该后缀在 Anthropic 兼容端点有效），备份 `settings.json.bak-20260910`；该文件所在目录不在 git 管理下
- **审核后续修复（追加提交）**：录制侧 `snapshots/session/web-search-endpoint-guidance/cordis.yml` 与回放侧 `cordis.snapshot.yml` 同钉 `model: deepseek-v4-flash`；`llm-deepseek` README 的 `models` 默认值表格、`Config.models` JSDoc 与重生成的 `docs/config-catalog` 同步为两条目；`docs/user/guide/python-sdk` 指南的默认模型改为 `deepseek-flash`；`2026-08-20-unified-image-request-pipeline` note 中「默认把 vision-exp 公布为支持图片」改为 `deepseek-flash`（implemented note 须随事实同步）；`adapter.spec.ts` 把编目条目 v4-pro 与未编目透传名拆成两个用例；`ui-model-selection` 词典键名由 `option.deepseekV4Flash.description` 改为 `option.deepseekFlash.description`；`scripts/smoke-python-runtime.py` 的 live smoke 改发 `deepseek-flash`
- ⚠️ **兼容性代价（审核 M4，必须知晓）**：目录之外的 id 按纯文本处理（`llm-deepseek/src/adapter.ts` 的未编目路由），因此把旧名 `deepseek-v4-flash-vision-exp` 留在 settings 或会话选型里的部署，**带图请求会以 `UNSUPPORTED_CONTENT` 硬失败**（此前由服务端路由到 V4.1-Flash 可成功）；纯文本请求仍照常被服务端路由。要继续发图须改选 `deepseek-flash`，且该 id 已不再出现在模型选择弹窗中。此降级已写入 Agent Note 的 Consequences
- ⚠️ **ACP 快照 7 项既存失败（与本次无关）**：`snapshots/acp/acp.snapshot.ts` 的 handshake / cancel / cancel-tool-calls / escalation-approved / escalation-rejected / fs-escalation-approved / image-compaction 共 7 项回放红，差异是多出一条 `session/update` 的 `config_option_update` 通知；对照实验（把 `packages/bundle/acp-app/cordis.patch.yml` 的 `model` 还原为旧名后仍红，且该场景的模型列表来自其自身组成而非 DEFAULT_MODELS）证明与本次改动无关，属上游既有问题，未处理；将来若接手清理，注意这是 `test:snapshot` 的现存红灯

### dsh 命令与多端产品子代理（2026-08-28 起生效）

- **全局 `dsh` 命令**：`/usr/local/bin/dsh` → `/home/wcoom/bin/dsh`（脚本在根仓库，git 管理）。`dsh` 一键启动 Web GUI（已在运行则直接开浏览器）；`dsh --bg` 后台启动（日志 `~/.dsh/logs/`）；`dsh stop` 一键停止全部 DSH 实例（SIGTERM 进程组优雅退出，10s 超时强杀）；其余参数透传 DSH CLI（如 `dsh --profile tui`）
- **多端产品子代理**（rc.8 的 profile-bundle 机制）：web profile 已装 `@deepseek-ai/dsh-subagent-claude-code` + `@deepseek-ai/dsh-subagent-codex`（link: 指向本地 checkout，与源码版本一致），bundle 层在 Host 平面注册提供方
- **三个命名实例**（`/root/.dsh/profiles/web/cordis.patch.yml`，非交互权限模式）：`claude-code`（acceptEdits 编码）、`codex`（approve-for-me 自动评审 + workspace-write）、`claude-code-audit`（plan 只读审计）。claude-successor preset 对应暴露 `subagent_claude_code` / `subagent_codex` / `subagent_claude_code_audit` 三个工具，`enableRunInBackground: true` 支持后台并行委派（配合 `job_output`/`job_kill` 做同时多端工作）
- **继任者模式集成**：claude-successor 的 persona 已内置「多端产品子代理」分工段落——编码委派 Claude Code/Codex、审计走 audit 实例、后台并行策略、与 spawn/fork 及 subagent_review 的分工边界、ABI/KMI 红线绝不委派（agent.cordis.yml 的 persona text 与 preset.yml 描述已同步，commit e68bf23）
- **认证**：Claude Code 子代理走 `~/.claude/settings.json` 原生设置（ANTHROPIC_AUTH_TOKEN + DeepSeek 中转 BASE_URL）；Codex 走 `~/.codex/auth.json` 原生登录
- **配置 git 仓库**：`/root/.dsh`（profile 配置、settings.yaml）与 `/root/.dsh/.agent-presets`（preset 组成）均为独立 git 仓库；sessions/storages/凭据已 gitignore。改配置先改对应文件再提交
- **生效方式**：bundle 安装与 preset 工具行变更需重启——`dsh stop && dsh`，新会话即具备三个产品子代理工具

### FastAI OpenAI 中转接入（2026-09-11 起生效）

- **中转**：`https://www.fastaitoken.com`，OpenAI 兼容（`/v1/chat/completions` 与 `/v1/responses` 均可用），Codex 风格反代。`/v1/responses` 尊重请求自带的 `instructions`，只有在请求未提供时才注入 Codex 默认人格（实测确认，故不影响 DSH 的 persona）
- **DSH 路由**：`/root/.dsh/settings.yaml` 的 `llm-pi-ai.providers.openai`（显示名 `FastAI OpenAI`）。路由名复用 pi-ai 内置 `openai` 目录，目录内已有的 id 未声明字段继承目录条目（`gpt-5.6` 是唯一例外，见下），只覆盖 `baseURL: https://www.fastaitoken.com/v1` 与 `apiKeyEnv: FASTAI_OPENAI_API_KEY`；协议取目录默认的 `openai-responses`（pi-ai 硬编码发送 `store: false`，正合该中转要求）
- **密钥**：key 存 `/root/.dsh/.credentials.yaml` 的 `FASTAI_OPENAI_API_KEY`（该文件已被 `.gitignore` 忽略，不入库）；settings.yaml 只写引用名，不含密钥
- **收录模型（7 个，全部经 `/v1/responses` 实测可用）**：`gpt-5.5`、`gpt-5.6`、`gpt-5.6-sol`、`gpt-5.6-terra`、`gpt-5.6-luna`、`gpt-6-astra`、`gpt-5.3-codex-spark`。其中 `gpt-5.6` 是中转独有名字（pi-ai 目录无此条目），**字段不继承目录、必须全部显式声明**：272k 上下文 / 128k 输出 / `input: [text, image]` / off·low·medium·high·xhigh·max 六档推理。漏写 `input` 会退化成纯文本模型：发图被拒，且历史里已有图片（含 `read_image` 产物）的会话切到它之后每个请求 `UNSUPPORTED_CONTENT` 硬失败、无法自愈
- **不可用（勿再收录）**：`gpt-5.4`、`gpt-5.5-pro` 返回 404（分组不支持）；`gpt-5.4-mini` 返回 400（Codex/ChatGPT 账号不支持）
- **已验证**：llm-pi-ai 自身的 `Config`/`assertServiceable` 校验通过（`fastai` 与 `openai` 两个路由均无诊断）；`dsh --profile headless` 以 `openai/gpt-5.5` 真实跑通一次工具调用（读取文件首行并原样返回），reasoning 流正常
- **端点必须带 `/v1`**（踩过的坑）：pi-ai 把 `baseURL` 原样交给 OpenAI SDK 再自行拼路径，缺 `/v1` 会打到站点首页——HTTP 200 但响应体是 HTML 不是 JSON。`fastai` 路由原先缺 `/v1`，导致该路由 4 个模型与 `allowedModels` 里两条 fastai 委派全部不可用，2026-09-11 由独立审核发现并修复；修后 `dsh --profile headless` 以 `fastai/deepseek-v4-flash` 端到端跑通
- **协议选择**：新 key 实测 `/v1/chat/completions` 请求 gpt-5.x 会返回 524（Cloudflare 超时，约 126s 无响应体），而 `/v1/responses` 正常，故 `openai` 路由走 responses；`fastai` 路由的 deepseek/glm 模型走 `openai-completions` 正常
- **图像实测**：中转支持 `input_image`（responses + data URI），16×16 纯红 PNG 实测 `gpt-5.6` 答「红色」，故 7 个模型的模态均已含 `image`
- ⚠️ **手工改默认模型的陷阱（审核 S3）**：DSH 对 `reasoningEffort` 不做 clamp，模型未声明的档位会硬失败（`UNSUPPORTED_REASONING_EFFORT`）。现有 `agent-default-model.reasoningEffort: max` 与 gpt-5.x 不兼容（目录里 `gpt-5.5` 的 `max` 为 null），所以手工把默认模型改成 `openai/gpt-5.5` 时必须同时改掉或删掉这一行；在 GUI 模型选择器里切换则安全（GUI 重写该段时不带旧档位）
- **发现模型的边界（审核 S4）**：因为路由名复用了目录 provider，Models 页的「发现模型」会对该路由列出全部 39 个内置 OpenAI 模型（其中 `gpt-5.4` / `gpt-5.5-pro` / `gpt-5.4-mini` 在中转上不可用）；已写死的 7 条不受影响，重配该路由时别误选
- **已知观测**：该中转 `/v1/responses` 偶发长时间无响应（曾一次 120s 收到 0 字节），批量连发请求时更易触发，日常单发未见问题；DSH 侧 `streamIdleTimeoutMs` 默认 300s 可容忍
- **生效方式**：`llm-pi-ai` 段每请求重读，settings.yaml 改动无需重启；新会话即可在模型选择器选到 `FastAI OpenAI` 的模型；`subagent-model-selection.allowedModels` 已同步加入这 7 个模型
- **默认模型未改**：`agent-default-model` 仍是 `deepseek-official/deepseek-flash`；要用 gpt-5.x 做主模型，在 GUI 模型选择器里切换即可
- **提交**：`/root/.dsh` 仓库 `7ee9ad3`（接入；同时纳入此前会话遗留未入库的 `fastai` 路由：deepseek-v4-flash/v4-pro、glm-5.3/glm-5.3-flash，走 `openai-completions`）与 `d14f638`（修复端点缺 `/v1` 与 gpt-5.6 模态漏声明）

- **2026-09-11 后续变动**：用户在 GUI 把默认模型切成 `openai/gpt-6-astra`（该模型目录条目的 `max` 档可用，与既有 `reasoningEffort: max` 兼容，不像 `gpt-5.5` 的 `max` 为 null），并移除了 `fastai` 路由（旧 key 的 deepseek/glm 模型不再使用）；`subagent-model-selection.allowedModels` 里两条指向 `fastai` 的悬空引用已同步清理，现为 openai 7 条 + deepseek-official 2 条。提交 `a515236`。

### 生成类模型接入（图像 / 视频，2026-09-11 起生效）

- **背景**：中转 `/v1/models` 从 13 个涨到 24 个，新增的 `seedance-2.0/2.5-*`、`veo3.1-time`、`wan-3.0-time`、`minimax-h3-time` 都是生成类模型而非对话模型：实测在 `/v1/responses` 上返回 500，所以**不能**塞进 `llm-pi-ai` 的 models 列表（只会让模型选择器多出必然失败的项）
- **方案**：新增零依赖 stdio MCP 服务器 `/home/wcoom/dsh-media-mcp/server.mjs`（提交 `1d17b0d`，文档见同目录 README.md），经 `@deepseek-ai/dsh-mcp-client` 挂载，暴露 `mcp__media__generate_image` 与 `mcp__media__generate_video` 两个工具；挂载行在 `/root/.dsh/profiles/web/cordis.patch.yml`（提交 `d4f5c72`）
- ⚠️ **patch 层的语法坑**：`- id: <name>` 是**按 id 覆盖已有行**，新增行必须写成 `- insert:` 列表；写成前者会报 `patch: entry "..." not found`
- ⚠️ **必配项**：`toolCallTimeoutMs: 1500000`（25 分钟）。视频是异步任务，实测 4 秒片约 8 分钟，mcp-client 默认的 60 秒会在任务完成前掐断调用
- **凭据**：服务器优先读环境变量 `FASTAI_API_KEY`，否则读 DSH 凭据库 `/root/.dsh/.credentials.yaml` 的 `FASTAI_OPENAI_API_KEY`；配置与仓库里都不含密钥；产物默认落 `/home/wcoom/media-out`（已在根仓库 .gitignore 忽略）
- **中转生成协议（实测）**：图像走 `POST /v1/images/generations`（OpenAI 兼容，一次性返回 `data[].url`，`gpt-image-2` 十几秒出 1024×1024；缺 prompt 时它返回 500 而不是 400）；视频走 Sora 风格异步任务——`POST /v1/videos` 返回 202 + `{id, status, progress}`，轮询 `GET /v1/videos/<id>`，终态对象带 `video_url` / `result_url` / `download_url`（火山 VOD 签名链接，约 24 小时有效）；**`GET /v1/videos/<id>/content` 不可用**（404 `Videos API is not supported for this platform`）；`GET /v1/videos` 无列表端点（404），任务只能按 id 查
- **模型可用性**：`gpt-image-2` 可用（默认）、`seedance-2.0-pro-token` 可用（默认，4 秒片约 8.7 万 tokens）、`veo3.1-time` 可建任务；`seedance-2.0-time`、`seedance-2.0-token`、`seedance-2.5-token`、`wan-3.0-time`、`minimax-h3-time` 在中转侧报 400 `MEDIA_COUNT_UNAVAILABLE`（它自己没有可用账号）
- **网络注意**：该中转会偶发中断连接（表现为 `fetch failed`，重试即好），服务器已对网络层失败自动重试 2 次；`node fetch` 直连正常，无需代理
- **已验证**：MCP 协议握手与工具列表冒烟通过；图像与视频两条链路都实跑出产物（2.6MB MP4 / 1.9MB PNG，且已用 read_image 目视确认内容正确）；headless 会话端到端调用 `mcp__media__generate_image` 成功并返回产物路径
- **生效方式**：web profile 的 `patchReload` 是 `live`，新会话即应具备这两个工具；若模型/工具列表里没出现，执行 `dsh stop && dsh`（会中断当前会话）
- **费用提示**：生成调用真实计费，视频明显贵于图像，工具描述里已提示优先用图像
