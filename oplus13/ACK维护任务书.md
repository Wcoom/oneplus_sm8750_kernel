# 任务书：GKI 6.6 个人源码 ACK 月度维护

> 用途：每月让 Claude Code 依据本任务书自动"补上最新提交"，可持续维护个人 GKI 6.6 内核源码（含 Droidspaces 等定制）。
> 上游：**仅 Google ACK `android15-6.6`**（官方 googlesource）。
> **红线（任何操作不得触碰）：GKI ABI 与 KMI。** 红线不过，宁可停下来等人工，绝不强行提交。

---

## 0. 红线（每次执行前先读）

1. **ABI 红线**：GKI 核心结构体布局（task_struct、file、inode、sock、net_device 等）不得改变；只允许使用 Android 预留 KABI/Vendor 槽位扩展。ABI 基线文件为 `android/abi_gki_aarch64.stg`（**本仓库无 `.xml` 格式**，故不用 `diff_abi`，改用第 4.5 节的定制点抽查）。
2. **KMI 红线**：GKI 导出符号集合、vendor hook（`trace_android_vh_*`）**签名**不得修改——第三方 vendor 模块编译期依赖这些签名。
3. **Hook 接口不可改，只能挪触发点**：上游重构导致 hook 调用点变化时，禁止改 hook 原型；在合并后新等价调用点重新触发，并确保形参能从新上下文取值。
4. **安全修复必须吸收**：CVE 修复不能被冲突吞掉。
5. **禁止 `-X ours` / `-X theirs`**：整片取舍会静默丢弃上游 CVE 或本地定制，必须逐个冲突判断融合。
6. **`Makefile` SUBLEVEL 保持 118**：版本号变动影响用户感知与模块匹配，如需调整先向用户确认。

---

## 1. 维护机制

固定流程：**准备检查 → 状态对比 → 合并 → 红线校验 → 构建验证 → 更新状态 → 推送打包**。

**幂等性**：合并前先比对 `.maintenance/state.json` 里的 `last_merged`，无新提交自动跳过 → 重复执行安全。
**断点续跑**：失败阶段写入 `maintenance.log`，下次从失败点继续。

### 1.1 仓库内维护目录

```
.maintenance/
├── state.json           # 维护状态（ACK last_merged 基线）—— 可持续的核心
├── maintenance.log      # 每批操作日志（追加）
└── reports/             # 月度报告 archive（report-YYYY-MM.md）
```

> ⚠️ `.maintenance/` 被 `.gitignore` 的 `.*` 规则忽略，**提交必须 `git add -f .maintenance/`**，否则状态文件不入库、下月失去基线。

---

## 2. 仓库拓扑与远程配置（已配置完毕，核对即可）

```bash
cd /home/wcoom/oplus13/android_kernel_common_oneplus_sm8750

git remote -v    # 维护相关三个：
# ack     https://android.googlesource.com/kernel/common     （官方源，勿用 GitHub 镜像）
# origin  https://github.com/whitewhale0612/...-C16.git       （只拉取，勿推送）
# github  git@github.com:Wcoom/oneplus_sm8750_kernel.git      （推送目标，SSH）
```

**⚠️ ack 必须用官方 googlesource**：曾误用 GitHub 镜像 `aosp-mirror/kernel_common`，其 `android15-6.6` 滞后 9 个月，导致整轮合并作废重做。合并前务必核对分支最新提交：

```bash
curl -s "https://android.googlesource.com/kernel/common/+log/refs/heads/android15-6.6?format=JSON" | head -20
```

## 3. 分支策略

```
6.6.118-13T      # 唯一长期维护分支（Claude 只动它）
```

- 合并用 `--no-ff` 提交，message 用简体中文，注明上游 commit 区间；
- **大轮合并前先建备份分支**（`git branch backup/<描述>`），便于整轮 `git reset --hard` 撤销。

---

## 4. 月度执行流程

### 4.1 准备检查

```bash
cd /home/wcoom/oplus13/android_kernel_common_oneplus_sm8750
git branch --show-current                      # 必须 6.6.118-13T
git status --short                             # 必须干净，禁止带脏合并
test -f .maintenance/state.json || echo "!! 缺状态文件"
df -h .                                        # 全量编译需 ≥ 80GB 空闲
ls android/abi_gki_aarch64.stg arch/arm64/configs/gki_defconfig
```

### 4.2 拉取 ACK 最新（幂等）

```bash
# googlesource TLS 不稳定（GnuTLS recv error），必须加这两个参数：
git -c http.version=HTTP/1.1 -c http.postBuffer=524288000 fetch ack android15-6.6
```

> **浅克隆陷阱**：若 `git merge-base` 找不到共同祖先，先 `git fetch origin --unshallow`（约 35 分钟，已完成过一次，正常无需重做）。

### 4.3 合并

```bash
LAST=$(python3 -c "import json;print(json.load(open('.maintenance/state.json'))['sources']['ack']['last_merged'])")
git log --oneline $LAST..ack/android15-6.6 | wc -l     # 0 → 跳过并记"无更新"
git branch backup/pre-ack-$(date +%Y%m)               # 备份
git merge ack/android15-6.6 --no-commit --no-ff
git diff --name-only --diff-filter=U                  # 列出冲突文件
```

### 4.4 冲突分级处理

| 级别 | 判定 | 处理方式 | 可否自动 |
|---|---|---|---|
| **L0** | 无 unmerged 文件 | 直接提交 | ✅ |
| **L1** | 机械冲突：`gki_defconfig` 重复 CONFIG、NTSYNC 重复段、ABI 文件 vendor 段追加 | 按第 6 节规则去重，`git add` 后提交 | ✅（处理后必须再过红线校验） |
| **L2** | 需判断：`sched.h` KABI、`include/trace/hooks/*` 调用点迁移、`kernel/pid.c` 定制段、hybridswap/dma-buf 链 | 逐处解决，**每处写入 maintenance.log 记录取舍** | ⚠️ 必须逐处记录；拿不准 → 停 |
| **L3** | 红线冲突：hook 签名被改、核心结构体布局变化、KABI 槽位被占 | **立即停止**，输出人工报告，禁止提交 | ❌ |

**停止规则**：出现 L3，或 L2 超过 3 处拿不准，中止并报告用户。不强行继续。

**冲突量大时可派并行代理**（每个约 20 文件）。但**代理产出必须逐文件复核语法完整性**——曾出现融合损坏：函数括号缺失、变量声明丢失、调用点参数数不匹配。**修不动就整文件回退** `git checkout HEAD -- <file>`。

### 4.5 红线校验

```bash
# a) 冲突标记检查（必须同时查三种标记，只查 <<<<<<< 会漏）
git grep -nE "^<<<<<<<|^=======|^>>>>>>>" -- . ':!*.rst' || echo "OK"

# b) vendor hook 签名不变（只允许新增/迁移，不允许改删）
git diff --stat HEAD@{1} -- include/trace/hooks drivers/android/vendor_hooks.c

# c) 本地定制关键点仍在
grep -n "ANDROID_KABI_USE(6\|sysv_sem\|sysv_shm" include/linux/sched.h   # 应有 2 处 KABI 行
grep -c "ghost_task" kernel/pid.c                                        # 应为 12
grep -n "CONFIG_NTSYNC" drivers/misc/Kconfig drivers/misc/Makefile
grep -E "^CONFIG_(NTSYNC|SYSVIPC|PID_NS|IPC_NS|USER_NS|NAMESPACES|POSIX_MQUEUE|BBG|NET_SCH_FQ_GUARD|REKERNEL_X)=y" arch/arm64/configs/gki_defconfig
grep -n "^CONFIG_ZRAM=n" arch/arm64/configs/gki_defconfig                # 必须保持 n（刻意）
grep -n "baseband_guard" arch/arm64/configs/gki_defconfig                # LSM 串末尾
grep -n "^SUBLEVEL" Makefile                                             # 应为 118
```

### 4.6 构建验证

```bash
bash /home/wcoom/内核构建.sh     # clang-19 + ccache 伪装，增量编译，勿加 make clean
```

编译后核验：

```bash
cd /home/wcoom/oplus13/android_kernel_common_oneplus_sm8750
strings out/arch/arm64/boot/Image | grep -m1 "6\.6\."      # 版本号（应 6.6.118-4k-g<sha>）
strings out/vmlinux | grep -c ntsync                        # 当前应为 97（含 ntsync_fixup）
grep -E "FQ_GUARD|REKERNEL_X|NTSYNC|^CONFIG_BBG|SYSVIPC" out/.config
```

> **WSL 会在 BTF/vmlinux 链接阶段偶发杀进程**（内存压力），**重跑即过**，不是代码错误。

### 4.7 更新状态 + 提交 + 推送 + 打包

```bash
# 1) 更新 .maintenance/state.json（ack last_merged / last_maintenance）+ 追加 maintenance.log
# 2) 生成 .maintenance/reports/report-YYYY-MM.md（模板见 4.8）
git add -f .maintenance/           # ⚠️ 必须 -f
git commit -m "月度维护 YYYY-MM：ACK 合并 <n> 提交"
git push github 6.6.118-13T        # 回退场景用 --force-with-lease
bash /home/wcoom/dabao.sh          # 打包刷机包
```

**收尾必做**：更新 `/home/wcoom/oplus13/CLAUDE.md`（本次维护记录）。

### 4.8 月度报告模板

```markdown
# GKI 6.6 月度维护报告 YYYY-MM
- 执行时间：YYYY-MM-DD（Asia/Shanghai）；执行：Claude Code
- 基线分支：6.6.118-13T @ <sha>

## ACK 更新
| 更新数 | 区间(旧→新) | 结果 |
|---|---|---|

## 冲突处理记录
- L1：<文件>，处理方式
- L2：<文件>，保留了什么/吸收了哪些，理由

## 红线校验
- ABI：PASS/FAIL（KABI 槽位 / ghost_task ×12 / NTSYNC ×97〔含 ntsync_fixup〕/ .config 全 y / SUBLEVEL 118）
- vendor hook：无签名变化
- 本地定制点：sched.h SYSVIPC ☑ / pid.c ghost_task ☑ / NTSYNC ☑ / ZRAM=n ☑ / BBG ☑

## 构建
- 版本号 / Image 编译结果 / 编译修复清单
```

### 4.9 断点续跑规则

- 每阶段完成即写 `maintenance.log`（时间戳 + 结果）；中断后先读 log 定位断点；
- 中断在合并中：`git merge --abort` 重跑，或解决冲突后 `git commit --no-edit` 完成；
- 中断在校验/构建：直接重跑对应命令，无需重新合并。

---

## 5. 本地定制保留清单（冲突时一律保留本地侧）

| 定制 | 文件 | 冲突时决策 |
|---|---|---|
| Droidspaces KABI | `include/linux/sched.h` | 保留 `ANDROID_KABI_USE(6, sysv_sem)` + `_ANDROID_KABI_REPLACE(7,8, sysv_shm)` |
| ghost_task | `kernel/pid.c` | 保留全部 12 处引用 |
| NTSync | `drivers/misc/ntsync.c`、`include/uapi/linux/ntsync.h` | 保留，Kconfig/Makefile 注册不丢 |
| ZRAM=n | `arch/arm64/configs/gki_defconfig` | **保留 n**（hybridswap 走 SDDC，非错配） |
| crystal_hybridswap | `drivers/block/zram/crystal_*` | 保留本地（origin 上游同步除外） |
| contpte 体系 | `arch/arm64/include/asm/pgtable.h` 等 | 保留本地 `__ptep_get`/`__set_ptes` 命名（40+ 处调用依赖，改名会破坏语义） |
| dma-buf 记账链 | `include/linux/sched.h` KABI_USE(1) | 保留 `task_dma_buf_info` |
| unix 新 GC | `include/net/af_unix.h`、`net/unix/garbage.c` | 保留 `unix_schedule_gc`/static work，移除上游 `unix_gc(void)` 声明 |
| vendor hooks | `include/trace/hooks/*` | 保留本地 hook，签名一字不改 |
| SUBLEVEL=118 | `Makefile` | 保留 118（上游为 142+） |
| fq_guard / ReKernel-X / BBG | 见 CLAUDE.md | 整体保留 |

---

## 6. 常见机械冲突处理规则（L1）

| 现象 | 处理 |
|---|---|
| `gki_defconfig` 同一 CONFIG 两侧都有 | 去重保留一份；本地刻意项（ZRAM=n、BBG=y）优先 |
| `CONFIG_LSM` 串冲突 | 融合：保留上游新增 LSM + 末尾 `baseband_guard` |
| `include/trace/hooks/*.h` 重复 `DECLARE_HOOK` / 重复前向声明 | 删重复块（曾出现 `android_vh_loop_skip_queue_work`、`struct request;`） |
| `Makefile` SUBLEVEL | 保留本地 118 |
| ABI 文件 vendor 段 | 追加合并，不删任何一侧条目 |

---

## 7. 实战经验（务必先读）

**编译错误高发类型**（上游 API 演进与本地基线错配）：

1. **API 签名错配**：如 `fscrypt_get_devices`/`f2fs_get_devices` 返回类型不一致 → 适配为本地签名，调用点补 `IS_ERR` + `kfree`
2. **结构体缺字段**：新代码引用本地结构体没有的字段 → 补字段 + 对应 init/cleanup
3. **辅助函数未随调用点合入** → 从上游补入对应 helper
4. **代理融合损坏**：括号缺失、声明丢失、参数数不匹配 → **修不动就整文件回退** `git checkout HEAD -- <file>`
5. **KABI 阻挡的上游特性**：新字段会破坏 KABI 的（如 rpmsg `driver_override`、coredump pidfd）→ 回退本地版，记入报告
6. **冲突标记残留**：`init/main.c`、`.rst` 文档残留 `>>>>>>>`（只查 `<<<<<<<` 会漏）
7. **宏内缺参数行**：`include/trace/hooks/*.h` hook 缺 `TP_PROTO/TP_ARGS` 行 → 报 "embedding #include within macro arguments"
8. **条件编译依赖外部宏**：如 `certs/extract-cert.c` 的 `key_pass` 依赖 `-DUSE_PKCS11_ENGINE`，本地构建不传 → 恢复无条件声明

**流程经验**：
- 编译-修复要迭代多轮，每轮只看首个错误块，修完立即重跑
- 代理产出必须逐文件复核，宁可整文件回退也不要带着半损坏代码往下走
- 合并前建备份分支，撤销时 `git reset --hard <基线>` + `git push --force-with-lease`
