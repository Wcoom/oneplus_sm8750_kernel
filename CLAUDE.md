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
| `deepseek-harness/` | DeepSeek Harness（DSH）源码 checkout |
| `内核构建.sh` | 内核构建脚本（clang-19 + ccache 伪装，增量编译） |
| `dabao.sh` | AnyKernel3 刷机包打包脚本 |
| `android-ndk-r25c/` | Android NDK |
| `CC-Switch-v3.19.1-Linux-x86_64.deb` | CC-Switch（Claude Code 配置切换工具）安装包 |
| `mattpocock-skills/` | mattpocock/skills 仓库本地副本（**仅作参考/自定义**，不参与加载；技能已通过 Claude Code 插件提供，见 §4） |

## 3. Claude Code 运行环境

- **本体**：`/root/.local/bin/claude`（native 安装，v2.1.233；`claude --version` 可查）
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
- **含创造模式全部特性**（2026-08-15）：`tool-cordis` 自修改工具集（inspect/define/run/stop/undefine）+ 两个 Cordis 技能（`cordis-plugin-development`、`editing-cordis-compositions`，在 preset `skills/` 内）+ 双平面创作规则——可创作/校验其他 preset，但绝不改动部署自带 shipped preset（升级会覆盖）
- ⚠️ **tool-cordis 是进程单例**：其 Host Inspect Provider（Service/Event/Builtin/Tool）注册进进程级 `cordisInspect` 注册表，同一进程只允许一份。`cordis` 与 `claude-successor` 两个带 tool-cordis 的 preset 不能在同一进程共存——**切换默认 preset 后需重启 Web 进程**，claude-successor 才会成为唯一创造模式挂载；此后 cordis 会话不再需要（功能已全部并入 claude-successor）
- 技能配置（issue 追踪器 / triage 标签 / 领域文档）以 `oplus13/docs/agents/*` 为准，两个 agent 共用，改动需两边生效
- Claude Code 仍可用（`claude` 命令），但默认工作由 DSH 的 `claude-successor` 预设会话承担

### 模型分工（2026-08-15 起生效）

- **编码 = DeepSeek-V4-Pro**：`claude-successor` 会话（部署默认模型已改为 `deepseek-v4-pro`），主 agent 负责全部代码编写与内核工程
- **审核 = DeepSeek-V4-Flash**：主 agent 用 `subagent_review` 工具后台委派 flash 子代理独立审核变更（重大变更合并/提交前执行）
- **记忆优化 = DeepSeek-V4-Flash**：主 agent 用 `subagent_memory` 工具后台委派 flash 子代理把新事实写入本记忆链（重要工作完成或会话收尾时执行）
- 记忆与技能不冲突：见 §4「记忆与技能的分工边界」——记忆只存事实与配置，技能流程以各 SKILL.md 为准
