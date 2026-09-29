# ddl_guard_ko — 容器作用域的 DDL 动态守护（可加载模块）

**一句话**：把"恒关 Oplus DDL"升级成跟着 Droidspaces 容器生命周期走的被动开关 ——
容器起来就关、容器停了就开回来，全程事件驱动、零轮询、零常驻线程。

对应 `oplus13/CLAUDE.md` 第 22/25 项记录的 DDL 越界写崩溃（厂商永久模块
`oplus_bsp_sched_assist` 的 `update_ddl_hit_history()` 用失效 task 的 pid 索引
`ddl_sdata[]` → UBSAN 陷阱 → `PANIC_ON_OOPS` 重启）的**方案 A 实现**。

## 1. 为什么是"容器作用域"

DDL 关掉只是少一层调度优化，不会崩；DDL 开着在容器任务高频创建/回收时可能崩。
两者结合的自然结论是：**只在有风险的时候关**。而目前观测到的崩溃场景与容器强相关
（多份 minidump 的调用链都从容器/进程回收路径进入），所以作用域取"有容器在跑"。

它不引入任何探测开销：无容器时模块完全静默，只有 3 个探针挂在冷路径上。

## 2. 锚点（2026-09-29 真机 ftrace 实测）

| # | 锚点 | 触发时机 | 实测 |
|---|---|---|---|
| 1 | kretprobe `copy_pid_ns`（entry 过滤 `flags & CLONE_NEWPID`） | 容器 PID 命名空间创建 | 仅容器启动时触发 |
| 2 | kprobe `zap_pid_ns_processes` | 容器 init 退出后的收尸 | 容器停止时恰好 1 次 |
| 3 | kprobe `put_pid_ns` | 兜底：`ns.count` 归零、对象即将销毁 | 基线 82 次/10s（热路径，只在表非空时才细看） |
| 4 | 加载时 `for_each_process()` 对账 | 覆盖"模块加载前容器已在跑" | 一次性 |

⚠️ `create_pid_namespace` / `destroy_pid_namespace` 已被 ThinLTO 内联掉，**不可挂**，
这就是锚点选 `copy_pid_ns` + `zap_pid_ns_processes` 的原因。

## 3. 状态机

```
          容器创建                          容器停止
  无容器 ──────────► 有容器 ──────────────────────────► 无容器
  DDL=1             DDL=0（立即）              DDL=1（linger_ms 后）
                     ▲
       参数变更 / 模块加载 ──┘（立即重估）
```

- **立即关**：容器出现是关键路径，`mod_delayed_work(wq, &work, 0)` 立刻落闸。
- **延迟开**：容器停止后等 `linger_ms`（默认 3s），既躲开收尸抖动，也让厂商模块里
  与 DDL 相关的统计先自己收敛。
- 只用一个 `system_power_efficient_wq` 上的 `delayed_work`，同一时刻至多一个待执行项；
  `mod_delayed_work()` 会把已排队的项按新 delay 重新计时（新容器出现时能正确缩短延迟）。
  **work 内部不会再排自己**，不存在自排队竞态。

## 4. 参数（`/sys/module/ddl_guard/parameters/`，0644）

| 参数 | 默认 | 说明 |
|---|---|---|
| `policy` | 1 | 1 = 容器作用域；**0 = 恒关（只关不开）**，等同旧的保护脚本 |
| `linger_ms` | 3000 | 容器停止后延迟多少毫秒恢复（0 – 600000） |
| `dry_run` | 0 | 1 = 只记录判定不写开关（上线前验证用） |
| `state` | 只读 | `ns=… overflow=… degraded=… policy=… dry_run=… linger_ms=… applied=… ddl=… nmissed=…` |

参数一写就立即重估一次，无需重启。

## 5. 失败即安全（红线）

| 失败 | 结果 |
|---|---|
| 探针注册失败 | `init` 返回错误 → **insmod 失败**；模块不在，DDL 保持开机脚本写的 0 |
| `global_sched_ddl_enabled` 解析不到（厂商模块没加载） | 静态重定位失败 → **insmod 报错** → 同上 |
| pidns 表满（>64）或 kretprobe 漏事件（`nmissed>0`） | 粘性判定"有容器/不可信" → **绝不开** |
| `rmmod` | **不动当前值**，停在卸载瞬间的状态 |

换句话说：**任何异常都退化成"恒关"，永远不会出现"模块以为在管、其实管不着"**。

## 6. 构建

```bash
bash /home/wcoom/桌面/oplus13/ddl_guard_ko/build.sh
```

前置：`oplus13/android_kernel_common_oneplus_sm8750/out/` 必须是跑过 `内核构建.sh`
的完整构建树（要有 `.config` 和 `Module.symvers`）。

构建脚本里三个关键决定（改动前务必读 `build.sh` 头部注释）：

1. **不覆盖 `utsrelease.h`**——目标内核就是本地 `out/` 那一个（设备跑的就是它），
   vermagic 天然一致。这点与 `fq_guard_ko`（目标是上游 prebuilt）不同。
2. **不签名**（`CONFIG_MODULE_SIG_ALL=` 覆盖）——设备 `MODULE_SIG_FORCE` 未开，
   本地密钥签名反而可能验签失败。
3. **`KBUILD_MODPOST_WARN=1`**——Android 树的 modpost 把未定义符号当**错误**，
   而 `global_sched_ddl_enabled` 只能由运行内核在已加载模块的导出表里解析（编不进 .ko），
   也不能靠写死 CRC 补 `Module.symvers`（见下）。降级掉的严格性由 `build.sh` 自己补回：
   日志里出现除它以外的任何未定义符号，直接判构建失败。

⚠️ **不要用 `__symbol_get()`**：本树里它只对 `GPL_ONLY` 符号放行，而
`global_sched_ddl_enabled` 是普通 `EXPORT_SYMBOL`，必然失败。静态 `extern int` 引用
才是可用路径。

⚠️ **不要写死 CRC**：本地 `check_version()` 恒返回 1 是 `84708f314ec5c` 那笔可单独
revert 的 LXC 补丁带来的；依赖它会在 revert 后炸。不带 `__crc_` 条目走的是"无符号版本"
分支，任何内核都放行。

⚠️ **每次重刷内核都必须重新构建并替换设备上的 .ko**（vermagic/CRC 与内核绑定）。

## 7. 部署

```bash
# 1) 推到设备
adb -s 5d6d4090 push ddl_guard.ko            /data/local/tmp/
adb -s 5d6d4090 push install.sh verify-device.sh /data/local/tmp/
adb -s 5d6d4090 push deploy/99-ddl-guard.sh  /data/local/tmp/

# 2) 先 dry-run 验证状态机（不写开关）
adb -s 5d6d4090 shell su -c 'sh /data/local/tmp/install.sh /data/local/tmp/ddl_guard.ko dry'
adb -s 5d6d4090 shell su -c 'sh /data/local/tmp/verify-device.sh'

# 3) 真开关
adb -s 5d6d4090 shell su -c 'sh /data/local/tmp/install.sh /data/local/tmp/ddl_guard.ko real'

# 4) 固化：模块 + 自启脚本放到持久路径
adb -s 5d6d4090 shell su -c 'mkdir -p /data/adb/ddl_guard && cp /data/local/tmp/ddl_guard.ko /data/adb/ddl_guard/ && chmod 644 /data/adb/ddl_guard/ddl_guard.ko && cp /data/local/tmp/99-ddl-guard.sh /data/adb/service.d/ && chmod 755 /data/adb/service.d/99-ddl-guard.sh'
```

自启脚本语义：**先写 0（安全缺省）→ 再 insmod 交给模块动态管理**；模块加载失败就
停在 0，等同旧的恒关行为。想彻底退回恒关：把脚本里的 `POLICY` 改成 0，或换回
`op_mods/deploy/99-oplus-sched-ddl-guard.sh`。

## 8. 验证

`verify-device.sh` 会自动跑 N 轮容器启停并采样：

```bash
adb -s 5d6d4090 shell su -c 'sh /data/local/tmp/verify-device.sh 3'
```

判定标准：

- **dry**：`state` 的 `ns` 随容器 1↔0 迁移，`ddl` 与节点值全程不变；
- **real**：容器运行中 `ns>=1` 且 `DDL=0`；停止 `linger_ms` 后 `ns=0` 且 `DDL=1`；
- 附加：容器运行中反复采样 `ns` 必须保持 ≥1（证明 `put_pid_ns` 兜底没有误摘）。

### 2026-09-29 真机实测记录

OnePlus 13（`5d6d4090`），内核 `6.6.118-android15-8-gf4dc45704e54-abogki20260727-4k`，
dry-run 与真开关各跑 2 轮容器启停，`state` 全程 `overflow=0 degraded=0 nmissed=0`：

| 时刻 | 事件 |
|---|---|
| 12461.89 | `DDL 开启（容器=0）`（`dry_run` 1→0 的写入路径） |
| 12478.703404 | 登记探针 pidns → **同一毫秒** `DDL 关闭（容器=1）`（紧急路径） |
| 12478.703418 | `put` 摘除该探针 pidns（droidspaces 启动时 `unshare(CLONE_NEWPID)` 试探后立刻销毁） |
| 12479.044773 | 真容器 pidns 登记（不重复写开关：`want == applied`） |
| 12490.333643 | `zap` 摘除（容器停止） |
| 12493.544452 | `DDL 开启（容器=0）`，距 zap **3.2s** = `linger_ms`(3000) |

- 容器运行中反复采样 `ns` 恒为 1 —— `put_pid_ns` 兜底**没有**误摘活着的 ns。
  本树 `put_pid_ns` 是级联实现（归零后销毁并继续减父 ns），且 `free_pid()` 不调用它，
  所以 `refcount_read(&ns->ns.count) == 1` 确实等价于"本次调用会销毁该 ns"。
- `policy` 写 0 立即 `ddl=0`；`rmmod` 后节点值保持不变（卸载不改值）。
- 启动瞬间的"探针 pidns"还顺带跑到了设计里的一条路径：先紧急关 → 40μs 后被 `put`
  摘除并排入 linger → 340μs 后真容器登记把它**缩短为立即执行**（`mod_delayed_work` 语义）。
- 原始 dmesg 见 `oplus13/.scratch/ddl-guard-20260929/`。

## 9. 已知边界（务必知晓）

- **容器是崩溃链最可能的触发源，但不是唯一**：minidump 里的命中线程出现过 `kswapd0`，
  说明内存回收路径也能走到 `update_ddl_hit_history()`。本模块只覆盖容器作用域；
  若日后在**无容器**时也复现同类崩溃，把 `policy` 设成 0 退回恒关。
- **"有容器"是全系统判定**：任何进程 `unshare(CLONE_NEWPID)` 都会让 DDL 关闭。
  偏安全方向（DDL 关只是少一层调度优化）。
- **`rmmod` 不改值**：卸载时若正因容器而关闭，DDL 会一直关着，直到有人重置或重启。
- 本方案**不是治本**：治本是用完整 OEM 源码重编含 `a772844` 边界检查的
  `oplus_bsp_sched_assist.ko`（见 `oplus13/CLAUDE.md` 第 22 项）。

## 10. 并入内核（下一步，可选）

`.ko` 路线跑稳之后，可把它按 `fq_guard` 的先例升级为内置：

1. 源码放 `drivers/misc/ddl_guard.c`（或 `kernel/sched/`），Kconfig 加
   `CONFIG_DDL_GUARD`，`gki_defconfig` 置 `=y`；
2. 内置后 `global_sched_ddl_enabled` 走链接期解析（厂商模块在 vendor_boot，
   仍需运行时由模块导出表解析——**验证方式不变：加载失败/解析失败即恒关**）；
3. 需要重刷内核，收益是省掉每次换内核重新推 .ko 的步骤，以及自启脚本的时序依赖。

在 `.ko` 验证到"连续多日容器启停无异常、无容器时 DDL 正常开启"之前，不建议走这一步。
