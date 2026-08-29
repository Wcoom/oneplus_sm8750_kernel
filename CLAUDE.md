# /home/wcoom 工作区记忆（CLAUDE.md）

> 本文件是 Claude Code 会话启动时**自动读取**的工作区持久记忆（项目级 `./CLAUDE.md`）。
> 目的：让每个新会话快速恢复上下文 —— 目录结构、Claude Code 运行环境与操作惯例、内核工程工作流。
> 记忆读取层级（全部自动加载）：① `/root/.claude/CLAUDE.md`（全局）→ ② `/root/.claude/projects/-home-wcoom/CLAUDE.md`（项目记忆）→ ③ 本文件 → ④ 子项目 CLAUDE.md（如 `oplus13/CLAUDE.md`、`oplus13/ACK维护任务书.md`）。

## 1. 本工作区是什么

WSL2 Ubuntu 环境下的开发工作区，核心工作方向：

- **Google GKI Linux 内核工程**：Android 内核源码维护、定制、编译、打包、刷机（OnePlus 13 / SM8750）
- **DeepSeek Harness（DSH）**：位于 `deepseek-harness/`，agent 编排框架（Cordis 插件体系）
- 其余：内核模块/补丁实验、CC-Switch 等工具

## 2. 目录地图

| 路径 | 内容 |
|---|---|
| `6.6/` | GKI 6.6 ACK 内核树（`MODULE.bazel`/`launch_cvd.sh`/`flash_device.sh`，含 `kernel`、`common-modules`、`external`、`prebuilts`） |
| `oplus13/` | **OnePlus 13 (SM8750) 定制内核工作区**：`android_kernel_common_oneplus_sm8750`（内核源码）、`clang-19/`（工具链）、`AnyKernel3-6.6.112-NOKSU-OnePlus8Elite/`（刷机包模板）、`op_mods/`、`docs/`、`bpf.sh`、`CLAUDE.md`（项目详细记忆）、`ACK维护任务书.md`（月度维护任务书） |
| `pddump/` | **原创 payload.bin 解包工具**（v1.0.0，Go，对标 payload-dumper-go）：多核并行解压（12 核实测 5.3 倍提速）、顺序写盘、稀疏输出；交付物 `pddump/dist/pddump-windows-amd64.zip`（Windows exe + 说明）；项目记忆见 `pddump/CLAUDE.md`；构建需 `export PATH=/usr/local/go/bin:$PATH GOPROXY=https://goproxy.cn,direct`（本机网络受限，仅 goproxy.cn/dl.google.com/github git 可用） |
| `deepseek-harness/` | DeepSeek Harness（DSH）源码 checkout |
| `mihomo-ebpf-smart-export/` | **mihomo 透明代理移植项目**（ColorOS 15 设备，Magisk /data/adb/box 部署）：`repo/`（源码 git 仓库，分支 official-20260828 = 官方 metacubex/Alpha 061966e7 + 5 个本地定制 commit：ebpf 移植、smart LightGBM 移植、bypass 提前启动、Model.bin 加固、vernesong nodes filter 同步）、`deploy/`（部署资产）、`tools/`（bpf 工具 + git-gh-proxy.sh）、`MEMORY.md`（项目详细记忆，排障根因在此）、`README.md`；项目记忆在项目内，本文件只放指针 |
| `内核构建.sh` | 内核构建脚本（clang-19 + ccache 伪装，增量编译） |
| `dabao.sh` | AnyKernel3 刷机包打包脚本 |
| `android-ndk-r25c/` | Android NDK |
| `CC-Switch-v3.19.1-Linux-x86_64.deb` | CC-Switch（Claude Code 配置切换工具）安装包 |
| `mattpocock-skills/` | mattpocock/skills 仓库本地副本（**仅作参考/自定义**，不参与加载；技能已通过 Claude Code 插件提供，见 §4） |

## 3. Claude Code 运行环境

- **本体**：`/root/.local/bin/claude`（native 安装，v2.1.233；`claude --version` 可查）
- **Go 工具链**：`/usr/local/go/bin/go`（go1.24.6，2026-08 安装）；本机外网受限，Go 模块必须用 `GOPROXY=https://goproxy.cn,direct`；pddump 项目的 Linux/Windows 构建与测试均依赖此工具链
- **用户级配置**：`/root/.claude/settings.json`；全局状态：`/root/.claude.json`
- **项目级权限白名单**：`/home/wcoom/oplus13/.claude/settings.local.json`（已授权 `Bash(git *)`、`Bash(curl *)`、`Bash(gh *)`、`Bash(python3 *)` 等，免确认执行）
- **API 路由**：DeepSeek 中转（`ANTHROPIC_BASE_URL=https://api.deepseek.com/anthropic`，key 在 settings.json 的 `ANTHROPIC_AUTH_TOKEN`）
- **模型映射**：`haiku → deepseek-v4-flash`；`sonnet/opus → deepseek-v4-pro[1M]`；`CLAUDE_CODE_EFFORT_LEVEL=max`；自动压缩窗口 786432
- **默认语言**：简体中文（全局偏好，见 `/root/.claude/CLAUDE.md`）

## 4. Skills 插件：mattpocock-skills（已引入并启用）

- **状态**：`mattpocock-skills@claude-plugins-official` **v1.2.3 已安装且 enabled**（`claude plugins list` 确认）
- **来源**：https://github.com/mattpocock/skills（本地副本：`/home/wcoom/mattpocock-skills/`）
- **安装位置**：`/root/.claude/plugins/cache/claude-plugins-official/mattpocock-skills/1.2.3/`（官方 marketplace 托管，自动随上游更新）
- **25 个技能**（`claude plugin details mattpocock-skills` 可查清单）：
  - **入口/路由器**：`ask-matt`（先问它，按任务路由到具体技能）
  - **需求对齐**：`grill-with-docs`（代码类）、`grill-me`（非代码类）—— 动手前先"拷问"对齐需求
  - **工程流程**：`to-spec`、`to-tickets`（规格/工单化）、`tdd`、`code-review`、`diagnosing-bugs`、`resolving-merge-conflicts`
  - **建模与设计**：`domain-modeling`、`codebase-design`、`improve-codebase-architecture`、`wayfinder`、`wizard`
  - **执行类**：`implement`、`prototype`、`research`、`triage`、`handoff`、`teach`、`wait-what`、`to-questionnaire`、`writing-for-agents`、`setup-matt-pocock-skills`
- **常驻成本**：约 1,163 tok/会话（always-on）
- **管理命令**：`claude plugins list` / `claude plugin details <name>` / `claude plugin disable|enable <name>` / `claude plugin update`（如上游发布新版本）
- **新项目首次使用**：运行 `/setup-matt-pocock-skills` 配置（问题追踪器、triage 标签、文档保存位置）
- ⚠️ 勿再通过 `npx skills add` 或 `~/.claude/skills` 链接重复安装（README 明确：两种方式只选其一，否则技能重复）

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
- **详细内核项目上下文**（五项定制、ABI 红线守点、ZRAM 配置注意事项等）见 `/home/wcoom/oplus13/CLAUDE.md`

### mihomo 工程速查（2026-08-29 起）

- **网络链路（重要）**：WSL/Windows 直连 GitHub 超时、gh-proxy.com 被 fake-ip（198.18.0.111）污染；可行链路 = WSL → 手机 mihomo HTTP 代理（192.168.1.177:7890，wlan0 同网段）→ gh-proxy.com → GitHub 全链路 200。git 用法：`git -c http.proxy=http://192.168.1.177:7890 ls-remote https://gh-proxy.com/https://github.com/<repo>.git`；包装器 `mihomo-ebpf-smart-export/tools/git-gh-proxy.sh`（手机 IP 为 DHCP 动态，变了要改 PHONE_PROXY）
- **mihomo 构建/部署**：`GOOS=android GOARCH=arm64 CGO_ENABLED=0 go build -tags with_ebpf`（GOOS=android 才能读 Android 系统 CA 池；缺 with_ebpf 则 bpf fd=0 无劫持）；部署到 `/data/adb/box/bin/mihomo`，chown root:net_admin 后必须再 chmod 6755（chown 会清 setuid 位）；必须 setsid 启动；Windows adb 位于 `/mnt/c/WINDOWS/system32/adb.exe`（WSL 内无 adb）

## 6. 会话惯例

- 始终使用简体中文交流、解释、写注释与提交信息
- 涉及内核 ABI/KMI 红线、冲突合并时，严格执行 `ACK维护任务书.md` 的规则，宁可停下等人工确认
- 修改技能/工作流文件前先读取对应 CLAUDE.md / 任务书，保持记忆文件与现状同步

### Git 纪律（所有代码用 git 管理，2026-08-15 起）

- 所有代码/脚本/文档变更都必须纳入 git；发现未纳入 git 的代码先 `git init` 或并入合适仓库再动手
- 工作流：确认干净起点 → 按仓库惯例开分支 → 变更 → 细粒度 add → 提交（简体中文信息，主题 ≤72 字符）→ 推送（按远程约定）
- 不入库：构建产物（out/、*.zip、Image）、ccache、密钥（遵守 .gitignore；内核仓库 `.maintenance/` 需 `git add -f`）
- 破坏性命令（`push --force`、`reset --hard`、`clean -fd`、`branch -D`、`checkout .` 等）执行前必须向用户说明并获得确认
- 里程碑打 tag；大轮合并前建备份分支；ABI/KMI 相关提交先经 `subagent_review` 审核
- **工作区根仓库**：`/home/wcoom` 已 `git init`（2026-08-15），管理根目录脚本与记忆文档（`内核构建.sh`、`dabao.sh`、`CLAUDE.md` 等），`.gitignore` 排除各子项目与构建产物；子项目各有自己的仓库（内核、harness、mattpocock-skills）

## 7. DSH 接管（Claude 继任者 preset）

- **DeepSeek Harness（DSH）agent 已全面接管本工作区的工程工作**，预设：`claude-successor`（位于 `/root/.dsh/.agent-presets/claude-successor/`，由 `standard` 复制而来）
- 该 preset 已移植：① 本记忆链（本文件 + `oplus13/CLAUDE.md` + `oplus13/docs/agents/*` 技能配置）② mattpocock-skills 全部 25 个工程技能（位于 preset 的 `skills/`，全部模型可调用，含 ask-matt 路由、grill-with-docs/grill-me 对齐、to-spec/to-tickets、tdd、code-review、triage、wayfinder 等）
- **记忆双向同步**：本文件链由 DSH 的 `dsh-agent-instructions` 自动读取（AGENTS.md/CLAUDE.md 候选），DSH 会话开工前读、完工后把重要进展写回本文件链；Claude Code 与 DSH 共用同一套记忆与技能配置
- **含创造模式特性**（2026-08-15）：两个 Cordis 技能（`cordis-plugin-development`、`editing-cordis-compositions`，在 preset `skills/` 内）+ 双平面创作规则——可创作/校验其他 preset，但绝不改动部署自带 shipped preset（升级会覆盖）
- ⚠️ **tool-cordis 是进程单例**：其 Host Inspect Provider（Service/Event/Builtin/Tool）注册进进程级 `cordisInspect` 注册表，同一进程只允许一份。因此在 claude-successor 中该行**默认 disabled**，使其可与 cordis preset 共存：**动态插件工具集（cordis_inspect/define/run/stop/undefine）在 cordis 预设会话中使用**；claude-successor 保留两个 Cordis 技能与双平面创作规则，可用文件工具直接编辑 preset 组成。若日后彻底退役 cordis，移除该 disabled 标志即可启用 tool-cordis
- 技能配置（issue 追踪器 / triage 标签 / 领域文档）以 `oplus13/docs/agents/*` 为准，两个 agent 共用，改动需两边生效
- Claude Code 仍可用（`claude` 命令），但默认工作由 DSH 的 `claude-successor` 预设会话承担

### 模型分工（2026-08-15 起生效）

- **编码 = DeepSeek-V4-Pro**：`claude-successor` 会话（部署默认模型已改为 `deepseek-v4-pro`），主 agent 负责全部代码编写与内核工程
- **审核 = DeepSeek-V4-Flash**：主 agent 用 `subagent_review` 工具后台委派 flash 子代理独立审核变更（重大变更合并/提交前执行）
- **记忆优化 = DeepSeek-V4-Flash**：主 agent 用 `subagent_memory` 工具后台委派 flash 子代理把新事实写入本记忆链（重要工作完成或会话收尾时执行）
- 记忆与技能不冲突：见 §4「记忆与技能的分工边界」——记忆只存事实与配置，技能流程以各 SKILL.md 为准

### dsh 命令与多端产品子代理（2026-08-28 起生效）

- **全局 `dsh` 命令**：`/usr/local/bin/dsh` → `/home/wcoom/bin/dsh`（脚本在根仓库，git 管理）。`dsh` 一键启动 Web GUI（已在运行则直接开浏览器）；`dsh --bg` 后台启动（日志 `~/.dsh/logs/`）；`dsh stop` 一键停止全部 DSH 实例（SIGTERM 进程组优雅退出，10s 超时强杀）；其余参数透传 DSH CLI（如 `dsh --profile tui`）
- **多端产品子代理**（rc.8 的 profile-bundle 机制）：web profile 已装 `@deepseek-ai/dsh-subagent-claude-code` + `@deepseek-ai/dsh-subagent-codex`（link: 指向本地 checkout，与源码版本一致），bundle 层在 Host 平面注册提供方
- **三个命名实例**（`/root/.dsh/profiles/web/cordis.patch.yml`，非交互权限模式）：`claude-code`（acceptEdits 编码）、`codex`（approve-for-me 自动评审 + workspace-write）、`claude-code-audit`（plan 只读审计）。claude-successor preset 对应暴露 `subagent_claude_code` / `subagent_codex` / `subagent_claude_code_audit` 三个工具，`enableRunInBackground: true` 支持后台并行委派（配合 `job_output`/`job_kill` 做同时多端工作）
- **继任者模式集成**：claude-successor 的 persona 已内置「多端产品子代理」分工段落——编码委派 Claude Code/Codex、审计走 audit 实例、后台并行策略、与 spawn/fork 及 subagent_review 的分工边界、ABI/KMI 红线绝不委派（agent.cordis.yml 的 persona text 与 preset.yml 描述已同步，commit e68bf23）
- **认证**：Claude Code 子代理走 `~/.claude/settings.json` 原生设置（ANTHROPIC_AUTH_TOKEN + DeepSeek 中转 BASE_URL）；Codex 走 `~/.codex/auth.json` 原生登录
- **配置 git 仓库**：`/root/.dsh`（profile 配置、settings.yaml）与 `/root/.dsh/.agent-presets`（preset 组成）均为独立 git 仓库；sessions/storages/凭据已 gitignore。改配置先改对应文件再提交
- **生效方式**：bundle 安装与 preset 工具行变更需重启——`dsh stop && dsh`，新会话即具备三个产品子代理工具
