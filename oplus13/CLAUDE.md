# OnePlus 13 (SM8750) 内核项目

基于 `whitewhale0612/android_kernel_common_oneplus_sm8750-C16` (Linux 6.6.118)，为 OnePlus 13 定制内核。

## 构建与打包

- **构建脚本**: `/home/wcoom/内核构建.sh`（clang-19 + ccache 伪装，增量编译，禁止运行 `make clean` 除非明确要求；内含 `export LOCALVERSION=""` 以抑制 setlocalversion 追加 `+`）
- **工具链**: `/home/wcoom/oplus13/clang-19/`
- **gki内核源码目录**: `/home/wcoom/oplus13/android_kernel_common_oneplus_sm8750`
- **打包**: `/home/wcoom/dabao.sh` 将 `out/arch/arm64/boot/Image` 复制进 `AnyKernel3-6.6.112-NOKSU-OnePlus8Elite/` 并打 zip
  - 命名规则：`AnyKernel3-<Image镜像时间>.zip`（`date -r $IMAGE +%Y%m%d-%H%M`，如 `AnyKernel3-20260811-1231.zip`；2026-08-11 起弃用提交哈希）
- **刷机到实体机（WSL 调用 Windows 侧 ADB）**：使用 `/mnt/d/刷机/platform-tools/adb.exe -s 5d6d4090 ...`（2026-08-30 核实实际路径，旧记录 `/mnt/c/WINDOWS/system32` 已失效）；Windows 端设备直连，WSL 无 USB 直通。
  - 设备：OnePlus 13 PJZ110，序列号 `5d6d4090`，Android 16，KernelSU root（`ksud 3.2.5-63-ga33dbca3`，LKM 不受 boot kernel 替换影响）
  - 刷入法（无 TWRP/管理器 App 时）：zip 推送到 `/data/local/tmp/` 后解包；`su -c` 的 `cd; export PATH=$PWD/tools:$PATH; sh anykernel.sh` 必须作为整体交给 root shell，避免 Windows adb 重组参数后只让 `cd` 进入 root shell、继而报 `magiskboot: inaccessible or not found`；设置 `OUTFD=1` 后由 AnyKernel3 仅替换当前 slot boot 中的内核。
  - anykernel.sh 逻辑：有 init_boot → `split_boot`（仅换内核）+ `flash_boot`（dd 写回当前 slot boot 分区）；刷完 `adb reboot` 生效
  - **设备侧 tools 无执行位坑（2026-08-28 已修模板）**：模板目录打包进 zip 时权限丢失（magiskboot 等 0644），设备端 `/data/local/tmp` 解包后 `magiskboot: can't execute: Permission denied`——需 `chmod -R 755 tools`；模板源目录已 `chmod a+x` 修复，后续打包不再丢位
  - **新旧内核判别（版本串固定时）**：uname 无法区分，以 boot 分区内核段 md5 与本地 Image 对比为准——`dd if=/dev/block/by-name/boot<slot> bs=4096 skip=1 count=12456 | head -c 39258624 | md5sum`（boot 头 4096 字节，KERNEL_SZ 51016896/4096=12456 块）
  - 真机验证成功记录：2026-08-16（ACK 第四轮）；2026-08-17（ACK 第五轮 + seccomp 兼容修复，内核 #31，连续 50 次 `su` 全部成功）；2026-08-28（ACK 第六轮，verify PASS=5，boot 内核 md5 核对）
- **git 远程**: `origin` = 上游 whitewhale0612（只拉取，勿推送）；`ack` = `https://android.googlesource.com/kernel/common`（官方源）；`github` = 个人仓库 `Wcoom/oneplus_sm8750_kernel`（推送目标，SSH 认证）
- **当前分支**: `6.6.118-13T`

## 当前稳定基线（2026-08-30 更新）

- **HEAD**: `c60d286c556c1`（**BBG 已整体移除**：先 revert `1268a1a` 放行提交 `2b6eea3`，再删除符号链接/挂载/配置 `c60d286`；子仓库 bundle 备份 `oplus13/Baseband-guard-backup-20260902.bundle`；**已推送 github**，2026-08-30）
- **版本**: 固定名 `6.6.118-android15-8-gf4dc45704e54-abogki20260727-4k`（SUBLEVEL 118；`CONFIG_LOCALVERSION` 写死 + `LOCALVERSION_AUTO` 关闭，不再随提交哈希变化）
- **产物**: `out/arch/arm64/boot/Image` 39,258,624 字节，md5 `965e04aff87aa07125291a5040801bff`（2026-08-30 构建，无 BBG）；上一版 md5 `0e54d8b8...`（2026-08-30 含 BBG）
- **构建脚本**（2026-08-30 修复）：`内核构建.sh` 自包含 `cd`（不依赖 cwd）+ `PAHOLE=/usr/bin/pahole`（原 6.6/prebuilts 路径随 6.6/ 删除失效；clang-19/bin/pahole 悬空链接已改指 /usr/bin/pahole v1.25）
- **ccache**: 4.38G / 5G
- `ahead origin 10728` 属正常现象（ACK 合并带入大量上游历史）
- 推送认证：GitHub PAT 权限不足（403），已改用 ed25519 SSH key（`wcoom@wsl2`）

**四项定制均已挂载并在 `out/.config` 中生效**（非仅 defconfig 声明；BBG 已于 2026-08-30 整体移除）：

| 定制 | 代码位置 | 配置项 |
|---|---|---|
| fq_guard | `net/sched/fq_guard.c` | `CONFIG_NET_SCH_FQ_GUARD=y` + `CONFIG_DEFAULT_FQ=y` |
| ReKernel-X | `drivers/rekernel_x/rkx*.c` | `CONFIG_REKERNEL_X=y` |
| Droidspaces | `drivers/misc/ntsync.c` + `include/uapi/linux/ntsync.h` | `CONFIG_NTSYNC/SYSVIPC/PID_NS/IPC_NS/USER_NS/NAMESPACES/POSIX_MQUEUE=y` |
| 温度偏移 | `kernel/temp_offset_sysctl.c` + `include/linux/temp_offset_sysctl.h` | `/proc/sys/kernel/temperature_offset_celsius`（sysctl，obj-y 无条件编译） |

**ABI 红线守点**：`include/linux/sched.h:1535-1536` 用 `ANDROID_KABI_USE(6, sysv_sem)` + `_ANDROID_KABI_REPLACE(7,8, sysv_shm)` 占预留槽位；`kernel/pid.c` ghost_task 12 处引用完好。

### ⚠️ ZRAM 配置是刻意如此，勿"修正"

`CONFIG_ZRAM=n` 与 `ZRAM_MEMORY_TRACKING=y` / `ZRAM_WRITEBACK=y` / `CRYSTAL_HYBRIDSWAP_ZRAM_*=y` 并存**不是错配**——crystal_hybridswap 走自有 SDDC 通路（代码在 `drivers/crystalfrostwork/crystal_hybridswap/`，非 `drivers/block/zram/`），不用原生 zram 驱动。这几行是每次上游合并的冲突高发区，**冲突时一律保留本地 `CONFIG_ZRAM=n`**。

### 内存/回收默认值（2026-08-06 复核）

| 项 | 值 | 说明 |
|---|---|---|
| swappiness | **200** | `CONFIG_SWAPPINESS=200`（`CONFIG_SET_SWAPPINESS_IN_KERNEL=y` 原有机制；来自 ACK 基线，非本地新增） |
| zram 默认容量 | 未定制 | 原 1:2 已移除，默认容量完全交给用户空间 |
| watermark_scale_factor | 未定制 | 原 100 已移除，恢复内核默认 10 |

> 2026-08-06 提交 `522eb9730e2bc` 移除 zram 1:2 与 watermark 100 两项本地调优，保留 swappiness=200（ACK 基线自带，勿当本地改动回退）。


## 本地修改（截至 2026-08-16 的 24 个提交；后续维护见第 13 项）

1. **fq_guard** (`net/sched/fq_guard.c`, commit `d4050a049` + 稳定性/低功耗优化, `CONFIG_NET_SCH_FQ_GUARD=y`)
   - 内核源码级守护：监听 NETDEV_UP/CHANGE/REGISTER，延迟后强制替换数据接口 root qdisc 为 fq
   - 黑名单前缀优先（默认 `rmnet_ims`），白名单前缀匹配
   - 参数: `enable/delay_ms/recheck_ms/retry_burst/blacklist`（启动参数 + `/sys/module/fq_guard/parameters/`）
   - `sch_api.c` 中 `qdisc_create` 去 static（built-in 链接期解析）；defconfig 默认 qdisc 已由 fq_codel 改为 fq
   - **低功耗重构（2026-08-05）**：
     - **修复 net 引用泄漏**（正确性缺陷）：原实现每次事件都 `get_net()` 而只在 UNREGISTER 时 `put_net()` 一次，`NETDEV_CHANGE` 链路抖动时高频触发会累积泄漏，导致 net namespace 无法销毁。改为仅首次分配 ctx 时取一次引用
     - **无限周期轮询 → 有限复查**：原 `period_ms=15000` 每 15 秒唤醒一次且永不停止；改为仅在"检测到被覆盖并改回"后复查 `retry_burst`(3) 次，连续发现已是 fq 即停止，回到纯事件驱动零开销。netd 若再覆盖会伴随 NETDEV_CHANGE 重新触发
     - **去掉自建 workqueue**：改用 `system_power_efficient_wq`（带 `WQ_POWER_EFFICIENT`，允许调度器避免唤醒空闲小核），省下一个常驻 kworker
     - **事件去抖**：`delayed_work_pending()` 已排队则不重排，避免事件风暴导致频繁抢 rtnl_lock（全局锁）
     - **`INIT_DELAYED_WORK` 移至 init 一次性执行**：原实现每次复用槽位都重新 INIT，会破坏 timer/work 内部状态


2. **ReKernel-X built-in 移植** (`drivers/rekernel_x/`, +1903 行, commit `3eb91d7cace`)
   - 源文件为 `rkx*` 前缀（15 个）：`rkx.c/h`、`rkx_binder.c`、`rkx_binder_kp.c`、`rkx_binder_alloc.c/h`、`rkx_genl.c`、`rkx_netfilter.c`、`rkx_netuid.c`、`rkx_signal.c`、`rkx_free_async.c`、`rkx_frozen.c`、`rkx_log.h` + `Kconfig`/`Makefile`
   - 配置项 `CONFIG_REKERNEL_X=y`（gki_defconfig:102）
   - 原 LKM 移植为编入内核的 obj-y，Generic Netlink family `rekernel_x2`，事件语义与 LKM 版一致
   - **binder/signal 事件用 kprobe 替代 `android_vh_*` tracepoint**（注：2026-08-15 核实 `gki_defconfig` 与 `out/.config` 中 `CONFIG_ANDROID_VENDOR_HOOKS=y`——vendor hooks 实际已启用且 ACK 合并时按"保留本地"处理；kprobe 方案维持不变，是否改回 vendor hook 另议）
   - 含 netfilter 网络事件、free-async 异步清理、frozen 检测；零轮询/零常驻线程/零 wakelock
   - 另移除 `kernel/module/module_overlay/modules/qcom-scm.ko`

3. **Baseband-guard (BBG)** (`security/baseband-guard`, commit `cc7887d802`)——**已于 2026-08-30 整体移除，见第 17 条**
   - 经 vc-teahouse/Baseband-guard `setup.sh` 接入：符号链接 + `security/Makefile/Kconfig` 挂载
   - `CONFIG_BBG=y`；`CONFIG_LSM` 末尾追加 `baseband_guard`（selinux 之后）；`BBG_BLOCK_BOOT/RECOVERY` 保持 n

4. **Droidspaces 容器支持** (commit `605e6859e4`, 2026-08-05)
   - 来源：cctv18/oppo_oplus_realme_sm8750 `.github/workflows/fastbuild_6.6.118.yml`「启用 Droidspaces 容器支持」步骤
   - 补丁下载自 `droidspaces_patch`（raw.githubusercontent.com，5 个补丁存于内核根目录，被 `.gitignore:40` `*.patch` 排除不入库）：
     - `fix_sysvipc_kabi_6_7_8.patch`：`task_struct` 的 `sysvsem/sysvshm` 移入 `ANDROID_KABI_USE(6, ...)` 保留槽位（`include/linux/sched.h`），`CONFIG_SYSVIPC=y` 不破坏 GKI ABI
     - `fix_oplus_bsp_midas.patch`：`find_task_by_vpid()` 对 `oplus_bsp_midas` 模块查询缺失任务返回 ghost sentinel task（`kernel/pid.c`），规避开机崩溃
     - `ntsync_base.patch` + `ntsync_compat_android15-6.6.patch`：NTSync 驱动（`drivers/misc/ntsync.c` +28KB、`include/uapi/linux/ntsync.h`），Kconfig/Makefile 注册 `CONFIG_NTSYNC`
     - `evdi_drm.patch`（extend 模式备用，未应用）
   - gki_defconfig 追加：`PID_NS/IPC_NS/USER_NS/SYSVIPC/DEVTMPFS/NAMESPACES/POSIX_MQUEUE/NETFILTER_XT_TARGET_LOG/NETFILTER_XT_MATCH_RECENT/NTSYNC`（均 =y；ADDRTYPE 原已开启）
   - 仅删除 `android/abi_gki_protected_exports_aarch64`（ABI 导出保护清单，避免 SYSVIPC 开启后校验失败）；`android/` 下其余 38 个 ABI 文件（`abi_gki_aarch64.stg`、`abi_gki_aarch64_oplus` 等）**均保留**
   - **extend 可选项未开**：`BT_HCIVHCI`、`STATIC_USERMODEHELPER=n`、`DRM_LINDROID_EVDI`（上游标注测试性）

5. **Merge ACK android15-6.6 安全补丁（两轮合并）**（2026-08-05）
   - 上游：`ack/android15-6.6`（官方 googlesource）
   - **重要前置**：本地仓库原是浅克隆（26 提交），`git fetch origin --unshallow`（~35 分钟）后才找到与 ACK 的共同祖先 `dd8fcb53983c`（2025-05-30）；googlesource TLS 传输不稳定（GnuTLS 错误），fetch 用 `-c http.version=HTTP/1.1 -c http.postBuffer=524288000` 可成功
   - **第一轮（commit `be9610f4683`）**：3964 提交（2025-05→2025-11），87 冲突；**基于 GitHub 镜像 aosp-mirror/kernel_common，其 android15-6.6 滞后 9 个月**，发现后纠正重做
   - **第二轮（commit `4860642a0474`，官方最新）**：增量 6425 提交（2025-10→2026-08-04 `742616e50d04`），98 冲突（3 代理 + 手工）
     - 吸收上游 CVE/修复：btrfs/ext4/ntfs3/nfc/sco/esp offload/rtl8150/hid/extract-cert 等
     - 保留本地：vendor hooks、crystal_hybridswap、f2fs 锁追踪、dma-buf 记账链、unix 新 GC、ZRAM=n、Makefile SUBLEVEL=118（上游 142）、mmu.c fixmap 清零
   - **编译修复**：`init/main.c` 与 `errata.rst` 残留 `>>>>>>>` 标记；`include/trace/hooks/mm.h` 6 处 hook 缺参数行（workingset_active/folio_remove_rmap/put_refs_direct_free_extent/folio_end_writeback/folio_start_writeback/mm_init/swap_device_swapoff）；`certs/extract-cert.c` 上游 key_pass 依赖外部 `-DUSE_PKCS11_ENGINE` 而本地构建不传 → 恢复无条件声明；`arch/arm64/mm/mmu.c` 补回 `void *ptr;` 声明
   - 验证：KABI 槽位、ghost_task、NTSYNC ×88、Droidspaces 配置全 =y、ZRAM 关闭、`6.6.118-4k` 版本保留、编译无错误
   - **经验教训**：合并 ACK 前必须用官方源核对分支最新提交（`curl -s ".../+log/refs/heads/android15-6.6?format=JSON"`），勿信任镜像时效

6. **Merge whitewhale0612 上游 crystal_hybridswap 更新** (commit `656ece04bd3`, 2026-08-05, 合并提交)
   - 同步 `origin/6.6.118-13T`（whitewhale0612 2026-08-05 发布）3 个 crystal_hybridswap 提交（`767dfa1d765be` speed up SDDC match scoring 等）
   - **零冲突**自动合并（4 文件 +653/-125：Kconfig、crystal_sddc.c/.h、zram_drv.c）
   - 至此本地已**吸收 origin 全部提交**（behind 0）

7. **2026-08-11 月度维护（origin 同步 + ACK 合并）**（`703de0b75390b` + `c48bd3e91e0ee` + `e17a2557d8474`）
   - **origin 同步**（`703de0b75390b`）：上游重写 `6.6.118-13T` 历史（forced update），旧同步点 `767dfa1d765be` 被移除，共同祖先 `11c4627b685c5`；合并 8 提交 → `cdb8a029f30b8`，crystal_hybridswap SDDC/LZ4KD 重构
     - 20 冲突（全在 crystal_hybridswap，本地无定制）：采用 origin 版本；`crystal_sddc.c` +1103 行、SDDC 完整性诊断、LZ4KD delta 解码并入 `lz4kd_decode.c`（本地独有 `lz4kd_decode_delta.c` 保留但不链接）
     - 吸收 kernel/fork.c sched/fork 失败清理修复、fs/erofs 移除 ARM64 NEON LZ4 分支
   - **ACK 合并**（`c48bd3e91e0ee`）：`742616e50d04` → `6b2ea17f1a2fb` 2 提交，L0 零冲突（UPSTREAM HID playstation CVE + ANDROID ublk 16K `__PAGE_SIZE`）
   - **维护状态**（`e17a2557d8474`）：state.json last_merged=`6b2ea17f1a2fb`、report-2026-08.md
   - 验证：红线全过、编译成功 `6.6.118-4k-gc48bd3e91e0e`、ntsync×88、已推送 github、打包 `AnyKernel3-6.6.118-4k-e17a2.zip`

8. **温度偏移 sysctl（temperature_offset_celsius）** (commit `623b8c1edf2ef`, 2026-08-11)
   - 移植自 whitewhale0612 上游 **v2.5 release 线**提交 `0527d017ec0f0`（2026-03-26）——该功能是 v2.5 线（6.6.89-whitewhale）独有，6.6.x-13T 主线从未合并，须自行移植；当前 6.6.118 上下文与 v2.5 逐字一致，零冲突
   - 新增 `kernel/temp_offset_sysctl.c`（sysctl 注册：`-100~100`℃，0644，`proc_dointvec_minmax`）+ `include/linux/temp_offset_sysctl.h`（偏移换算辅助）
   - `drivers/thermal/thermal_helpers.c` `__thermal_zone_get_temp()`：非电池热区温度 `-offset*1000`（m℃）；`drivers/power/supply/power_supply_core.c` `power_supply_get_property()`：电池 `TEMP` `-offset*10`（0.1℃）
   - `kernel/Makefile`：obj-y 追加 `temp_offset_sysctl.o`
   - 接口：`/proc/sys/kernel/temperature_offset_celsius`（写入偏移即对所有热区/电池温度读数生效；root 可写）

9. **内核名称固定 GKI 版本串 + zip 时间命名** (commit `bee7957f6bb87`, 2026-08-11)
   - `gki_defconfig`：`CONFIG_LOCALVERSION="-android15-8-gf4dc45704e54-abogki20260727-4k"` + `# CONFIG_LOCALVERSION_AUTO is not set` → `kernel.release` 精确为该串（`f4dc45704e54` 为官方 GKI 风格串，非本仓库提交）
   - **注意**：`LOCALVERSION_AUTO` 关闭后，setlocalversion 在未设 `LOCALVERSION` 环境变量时执行 `scm_version --short` 追加 `+`（`scripts/setlocalversion` 197-205 行）；构建脚本 `内核构建.sh` 已加 `export LOCALVERSION=""` 抑制（设空串即可，AUTO=y 时无副作用）
   - `dabao.sh`：zip 命名改为 Image 镜像时间（`AnyKernel3-<YYYYMMDD-HHMM>.zip`），弃用提交哈希

10. **架构深化改造（8 候选，5 提交，2026-08-11，`fd90bdb39a73b`~`751e0e1c77022`）**
    - 由 `/improve-codebase-architecture` 审查 5 个本地定制模块后实施，行为等价红线（genl 消息格式/错误码/init 顺序/日志逐字一致）
    - **C1 ntsync_fixup**（`fd90bdb39a73b`）：SELinux 权限修复独立为 `drivers/misc/ntsync_fixup.c`（late_initcall + 2s delayed work 执行 `__vfs_setxattr_noperm` + chmod 0666）；`ntsync.c` 移除自建 worker 38 行；Makefile 追加 `ntsync_fixup.o`
    - **C2+C7 rkx kprobe 收敛**（`5922e88c7c539`）：新增 `drivers/rekernel_x/rkx_kprobe.c`（`rkx_register_kprobes` 失败整体回滚 / `rkx_unregister_kprobes` / `rkx_send_event` 统一事件出口，netlink 未就绪静默返回）；signal/binder/binder_kp 改 kprobe 表；**删除 `rkx_binder_alloc.c/.h`**（`binder_alloc_copy_from_buffer` 解析失败降级 pr_warn）；rkx_netfilter 保留自有样板（per-netns hook，第二刀）（第二刀已于 `b4f77338a9bbc` 完成：err/exit 注销序列收敛为 `rkx_teardown_all()`，netfilter 定论保留自有注册、不再收敛）
    - **C3 fq_guard 正式接口**（`37ed9c5138e66`）：`sch_api.c` 导出 `qdisc_create_by_kind()`（`EXPORT_SYMBOL_GPL`，原型入 `include/net/pkt_sched.h:110`），`qdisc_create` 恢复 static wrapper（nla_strscpy 转发）；fq_guard 删除 extern hack 改正式调用 + IS_ERR 检查；notifier 不再重置 `rechecks_left`（recheck 所有权收敛到 fqg_work，**修掉 retry_burst 计数在事件驱动下永不复位的缺陷**）；Kconfig help 更新为事件驱动 + 有限复查语义
    - **C4 温度偏移单一入口**（`ab5924867424c`）：`include/linux/temp_offset_sysctl.h` 提供 `apply_temperature_offset(zone_type, raw, unit)`（battery 排除 `strncasecmp` 7/4 前缀内聚其中，`TEMP_OFFSET_MILLI_C/DECI_C` 枚举）；thermal_helpers（m℃）与 power_supply_core（0.1℃）两调用点各剩一行
    - **C5+C6 BBG 锁安全**（`751e0e1c77022`，gitlink 指向嵌套 repo `Baseband-guard` 的 `6e32d81`）：`bbg_check_blockdev_access()` 收敛 S_ISBLK→write_op→trusted 判定为单一入口（三个 hook 复用，前置链不变）；`allow_has/allow_add` 改 irqsave 自旋锁 + 内部 `allow_has_locked()` 防自死锁；可睡眠的 `blkdev_get_no_open` 解析保持在锁外
    - **C8 脚本清理（无内核提交）**：`verify_kernel.sh` 179→104 行（删 zram 1:2 / watermark 100 过期段）、`bpf.sh` 变量化 `KERNEL_ROOT/DEFCONFIG`、删除仓库根旧版 `dabao.sh`（519B 硬编码残留）——三脚本均在 `/home/wcoom/oplus13/`，**不属内核 git 仓库**，无提交内容
    - 验证：编译退出码 0、`OBJCOPY arch/arm64/boot/Image` 39,258,624 字节、error 0、13 个改动文件全部重编译（fq_guard/sch_api/ntsync_fixup/rkx_kprobe/baseband_guard/temp_offset_sysctl 等）；打包 `AnyKernel3-20260811-1523.zip`（31M）；**已真机验证**（2026-08-11：开机正常、`/proc/sys/kernel/temperature_offset_celsius` 可写生效、Droidspaces 容器可用）

11. **第二轮架构深化（crystal_hybridswap 重点，5 提交，2026-08-16，`674cf5a571c45`~`8f42d16655f2f`）**
    - **背景**：当日先撤销 8-15 两条提交（用户确认后 `reset --hard` 到 `751e0e1c77022`，被撤销内容保留在备份分支 `backup-6.6.118-13T-20260816-pre-reset`：ADR-0002 激进档守护 `f4a1c67dc87cc` + rkx 注销收尾 `b4f77338a9bbc`）；随后跑第二轮 `/improve-codebase-architecture` 审查（重点 crystal_hybridswap，报告 `/tmp/architecture-review-20260816-102757.html`），用户选定 C1-C6 并实施
    - **`674cf5a` rekernel_x**：err/exit 注销序列收敛为 `rkx_teardown_all()`（恢复被撤销提交的收敛，定义前置）
    - **`7d95970` mm/vmscan**：`scan_balance` 枚举上移至新增 `include/linux/vmscan_balance.h`，消除 memcg.c 副本（hooks/vmscan.h 官方前向声明零改动；hook 头走 `TRACE_HEADER_MULTI_READ` 不能承载完整枚举）
    - **`5340f23` crystal_hybridswap SDDC 记账接口**：4 函数归位 `sddc/crystal_sddc.c`；`zram_drv.h` 共享 5 个去 static 基础设施（`zram_memcg_stats_find_locked/update/add_current/sub_current`、`update_used_max`）
    - **`c39637b` crystal_hybridswap param.c**：新建 `chs_param_store(dev,id,buf,len)` 单一写入口，8 个 sysfs RW 属性瘦身为转发；ADR-0002 守护的唯一拦截点预留位（守护实现未恢复）
    - **`8f42d16` crystal_hybridswap native_wb**：finish/abort 合并为 `crystal_sddc_native_wb_release`（以 abort 为基准，finish 靠 private=NULL 退化）+ 3 个 KUnit 用例（release NULL/零值/未发布 reservation）
    - **验证与审核**：两次增量构建 exit 0（Image 39,258,624 字节不变）；DeepSeek-V4-Flash 独立审核 PASS（KMI/ABI 红线过：vmscan_balance.h 纯编译期、crystal_hybridswap 零 EXPORT_SYMBOL；行为等价逐字验证过），3 条低严重度建议中弱断言已修（xa_load→xa_empty）并 amend
    - **范围说明**：C1 完整版（SDDC 接口 32→3 事务收敛）与 C5 完整版（状态机纯函数化）超出单会话安全范围，本轮交付第一阶段（接口-2、KUnit+3），完整版待 DESIGN-IT-TWICE 设计流程
    - **注意**：备份分支 `backup-6.6.118-13T-20260816-pre-reset`（撤销内容）与 `backup-6.6.118-13T-arch-C1-C6-baseline`（重构前基线）仍在；KUnit 在 defconfig 中 `CONFIG_KUNIT=m`，KUnit 用例未在真机构建中编译（Kconfig:148 要求 `KUNIT=y`）

12. **ACK 第四轮合并（2026-08-16）**（合并提交 `8362cdb3fdc8b` + 维护记录 `7ff12fa0ced1a`）
    - 增量 `6b2ea17f1a2fb` → `24c059d9e217f`，5 提交，**L0 零冲突**自动合并（4 文件自动合并：abi_gki_aarch64.stg 仅追加 / abi_gki_aarch64_xiaomi 追加 / kernel/power/process.c / mm/vmalloc.c）
    - 吸收：xt_quota2 UAF 修复（`d388a7c8effc8`）、vendor hook `android_vh_try_to_freeze_abort` 新增（`d2fc65338294c`）、vmalloc 双导出（`c079fb2a08aa3`）、xiaomi/Exynos symbol list（`a7f1fd7885952`/`24c059d9e217f`）
    - 红线校验全过：stg 零删除、hook 仅新增无改删、KABI 槽位/ghost_task×12/NTSYNC/ZRAM=n/BBG/SUBLEVEL 118 全在；构建 exit 0（Image 39,258,624 字节）；打包 `AnyKernel3-20260816-1154.zip`
    - **已刷入真机并验证（2026-08-16）**：设备 `5d6d4090`（OnePlus 13 PJZ110，slot _b）；uname 精确为新版本串、boot_completed=1、KernelSU root 完好（u:r:ksu:s0）、温度偏移 sysctl 可读
    - 备份分支：`backup/pre-ack-202608`；state.json last_merged=`24c059d9e217f`（2026-08-16）

13. **ACK 第五轮 + seccomp 真机修复（2026-08-17）**（`57b888edbb7a7` + `a0f59328855f9` + `9b46d92f3d6f2`）
    - ACK `24c059d9e217f` → `606edb22359b4`，1 个 GenieZone VM/vCPU 创建失败路径引用泄漏修复，L0 零冲突；备份分支 `backup/pre-ack-20260817`。
    - 审计定位到 ACK 回移 `5346453405bf12` 已改变 `seccomp_filter_release()` 契约，但 KernelSU 3.2.5 在 6.6 仍按旧版本判断传入 `sighand=NULL` 的脱离快照，导致每次 `su` 触发 WARN 且过滤器引用无法释放；兼容路径仅接受该脱离快照，真实任务仍保留 `PF_EXITING` 与 siglock 强校验。
    - ABI/KMI、vendor hook、KABI 槽位、ghost_task×12、ZRAM=n、SUBLEVEL=118 全过；增量构建成功，Image 39,258,624 字节，打包并刷入实体机。
    - 新启动内核 build `#31`，boot ID `e68e6185-b5d6-46ac-ae1b-2aa82df22dc6`；连续 50 次 `su -c true` 全成功，`seccomp_filter_release` 告警 0；`verify_kernel.sh` PASS=5/WARN=0/FAIL=0。
    - 剩余启动告警均落在既有 vendor 模块调用边界（shutdown_detect、cnss2、schedinfo 等），未通过削弱通用内核断言掩盖。

14. **ACK 第六轮合并（2026-08-28）**（合并提交 `a5ca55f56defb` + 维护记录 `0db9942686f0a`）
    - 增量 `606edb22359b4` → `5ef17cb58b6e6`，274 提交（LTS 回移大轮 + ANDROID 特性），**2 冲突（1×L1 + 1×L2）**；备份分支 `backup/pre-ack-20260828`
    - **L1** `android/abi_gki_aarch64.stg.allowed_breaks`：本地侧无条目 → 取 ACK 侧 fscrypt_operations/fscrypt_master_key break 记录
    - **L2** `fs/erofs/zdata.c`：融合——保留本地 OPLUS_STORAGE_FS 空 bio WARN 检查（bugid 7760993）+ 吸收上游 `trace_android_vh_erofs_iostat_submit` hook（检查→hook→submit_bio 顺序）
    - 吸收：fuse-bpf verifier bypass 修复；fscrypt backport（mk_users keyring→list、get_devices 动态分配消除，官方 KABI 槽位 `get_devices_new`）；新增 erofs/f2fs iostat vendor hooks（4 个，仅新增）；memcg 限流不计入全局 PSI stall；Bluetooth/USB/serial/netfilter/batman-adv/xfrm/ipv6/ksmbd 等大批 CVE 修复；act_api RCU revert（上游回退）；GKI symbol list 更新
    - 红线校验全过：冲突标记零残留、hook 无改删、KABI 槽位/ghost_task×12/NTSYNC×97/ZRAM=n/BBG/SUBLEVEL 118 全在；构建 exit 0（Image 39,258,624 字节，SHA-256 `c0ed8f0f`）；DeepSeek-V4-Flash 独立审核 PASS
    - **已刷入真机并验证（2026-08-28）**：设备 `5d6d4090` slot _b；boot 分区内核段 md5 与本地 Image 一致（版本串固定，以 md5 区分新旧）；boot_completed=1、KernelSU root 完好、50 次 su 全成功、seccomp WARN=0；verify_kernel.sh PASS=5/WARN=0/FAIL=0；温度偏移 sysctl=0 可读；/dev/ntsync 0666
    - 打包 `AnyKernel3-20260828-1858.zip`（31M）；已推送 github（`9b46d92f3d6f2..0db9942686f0a`）

15. **v3.5 prebuilt 反推同步（lz4kds 算法拆分 + SDDC 诊断对齐，2026-08-29）**（`ac2523ece303b` + `8f0998ee9052d` + `941a8c58f2f8d`，已推送 github）
    - **背景**：上游 whitewhale0612 仓库已闭源（禁止 git 同步）；用户刷入上游 v3.5 官方 prebuilt，用 adb 日志反推 v3.3→v3.5 增量并应用到本地源码。反推素材与报告在 `oplus13/.scratch/rev-v35/`（findings.md、brief.md、claude-code-analysis.md、dmesg_v35.txt）
    - **反推结论**：本地（08-11 同步版）几乎已是 v3.3+ 完整状态；日志可实锤的增量仅：① lz4kds 算法拆分 ② SDDC 诊断体系重构（sddc_stat 输出删 wb_ref_pin×6+integrity×4 字段、加 delta_proof_failures）；v3.4/v3.5 changelog 的"调度稳定性修复/MGLRU 修复"无法从日志反推（本地 MGLRU config 与 v3.5 一致；context_tracking WARN 两边同码同行）；Droidspaces 本地已有；codex 子代理两次 thread-start 失败（服务不可用），改由 claude-code 子代理完成独立分析
    - **v3.5 激活事实**：真机 comp_algorithm 激活 [lz4kds] 但 ZRAM_DEF_COMP 配置值为 "lz4kd"，且用户空间无 lz4kds 写入点（/data/adb、/vendor、/system、/product init 均无）——v3.5 的 lz4kds 激活来自内核侧默认
    - **`ac2523ece303b` zcomp 拆分**：backends[] 加 lz4kds（SDDC codec 包装 backend 更名 lz4kds_backend_*）；新增 lz4kd_pure_backend_*（直接包装 crystal_lz4kd_encode/decode，delta ops NULL 自动走普通路径）；按名分发；默认 ZRAM_DEF_COMP 保持 lz4kd
    - **`8f0998ee9052d` SDDC 诊断对齐**：快照/内部 atomic/get_stats/sysfs 输出删 wb_ref_pin×6+integrity×4、加 delta_proof_failures；删 writeback ref-pin 统计函数；try_delta_from_source 加 delta_proof round-trip 验证（压缩后立即解码回验，失败计数放弃 delta）；workspace 加 proof 页；**integrity hash 校验逻辑保留**（内部计数仍在，仅输出隐藏——数据完整性防线不删）
    - **`941a8c58f2f8d` 默认算法 lz4kd→lz4kds**（对齐 v3.5 运行时激活 SDDC）
    - **验证（真机，已刷入 2 次）**：最终版 `AnyKernel3-20260829-1020.zip` 刷入：boot 内核 md5 与本地 Image 一致（cedb1217）；boot_completed=1；su×10 全成功；comp_algorithm 激活 [lz4kds]、SDDC enabled=1、deltas≈96K、aliases≈49K、saved_bytes≈180MB、delta_proof_failures=0；sddc_stat 字段与 v3.5 完全一致；pressure 日志新格式（无 wb_ref_pins）；dmesg 25 条 WARNING 与 v3.5 基线一致（context_tracking/proc_register/spmi 等既有 vendor WARN）
    - **红线全过**：KABI 槽位（sched.h 1535-1536）、ghost_task×12、ZRAM=n、NTSYNC/BBG/REKERNEL_X/FQ_GUARD、SUBLEVEL 118
    - **审核**：DeepSeek-V4-Flash 通过（3 条低严重度建议已处理）；发现 pressure.c:169 有上游自带 EXPORT_SYMBOL_GPL（"crystal 目录零 EXPORT_SYMBOL"旧说法不成立，非本次引入）；备份分支 `backup-6.6.118-13T-pre-v35-rev`

16. **BBG 放行 vendor_dlkm 刷写（2026-08-30，重做+定案）**——**内核侧 BBG 已于 2026-08-30 整体移除（见第 17 条），本条的放行修改与模块侧 dm 镜像直写方案随之作废；fopbatt 模块后续刷写不再受内核 BBG 拦截**
    - **背景**：上午 Codex 会话改 BBG 放行 vendor_dlkm（`fd3fd39`+`c1048de`）被回退；用户要求重做且"有根据不要猜测、不放行整个 super"
    - **真机取证链**：① vendor_dlkm_b=dm-18（super 动态分区，无 bd_meta_info，仅加 allowlist 无效）② BBG deny 日志实锤：`deny write dev=8:14 path=/dev/block/sda14 comm=lpadd_auto`（fopbatt 模块 `lpadd_auto --replace super vendor_dlkm_b` 刷写被拦，写 super 不在 allowlist）③ dm-linear 的 BLKROSET ioctl 转发到底层设备，dm 自身 ro=1 清不掉（`blockdev --setrw` 无效）④ 直写 dm-18 被 ro 拒
    - **内核修改**（子仓库 `eba53b9` + 主仓库 `1268a1a`，gitlink 一致）：allowlist 追加 `vendor_dlkm`；`blkdev_helper.c` 新增 `is_allowed_dm_partition_dev()`（dm_get_md/dm_copy_name_and_uuid 解析 dm 名→同一 allowlist，IS_BUILTIN(CONFIG_BLK_DEV_DM) 保护）；判定接入 `reverse_allow_match_and_cache` 单一入口。**不放行 super**
    - **模块侧改造**（fopbatt 电池工具包 8.2.14-beta，`/home/wcoom/fopbatt-bbg-fix/`，根仓库 b92af23）：`kot_run_lpadd_once` 改为 dm 镜像直写——`dmctl table` 读映射 → `dmctl create vendor_dlkm_<非当前slot>`（同名映射、ro=0、BBG allowlist 放行）→ `dd bs=4M conv=fsync` 直写 → `dmctl delete`；重启后 init 重建分区生效。**不写 super 分区表**
    - **验证（真机全通）**：镜像设备写往返与 dm-18 逐字节一致（erofs superblock 恢复）；完整模块安装流程（ksud install）"vendor_dlkm 写入成功"、BBG deny=0；重启后 boot_completed=1、oplus_chg_v2.ko 从新分区加载运行、/data/opbatt 完整
    - **教训**：BBG 判定是 dev_t 级，动态分区刷写必须经 dm 名称解析；lpadd_auto 类工具写 super 与 BBG 保护意图冲突，模块侧绕行是正解

17. **BBG 整体移除（2026-08-30）**（`2b6eea3d43fc7` + `c60d286c556c1`，已推送 github）
    - **背景**：真机观察 BBG 的 LSM 块设备写拦截导致部分模块安装失败，且每次块写都走 BBG 判定链严重影响 IO——用户决定源码中彻底移除该定制
    - **第一步**（`2b6eea3d43fc7`）：`git revert 1268a1a`（回退 8-30 的 vendor_dlkm 放行修改，gitlink 指回子仓库 `6e32d81`）
    - **第二步**（`c60d286c556c1`）：整体移除——删除顶层 gitlink `Baseband-guard`（160000）与符号链接 `security/baseband-guard`（120000）；`security/Makefile` 删除 `obj-$(CONFIG_BBG)` 行、`security/Kconfig` 删除 source 行；`gki_defconfig` 删除注释 + `CONFIG_BBG=y` + `CONFIG_LSM=...baseband_guard` 三行，**CONFIG_LSM 恢复内核默认列表**（BBG 引入前 defconfig 无显式 CONFIG_LSM）；恢复后三个文件与引入提交 `cc7887d802^` 逐字一致
    - **子仓库**：磁盘 `Baseband-guard/` 目录整体删除；完整历史（含本地提交 `6e32d81` 锁安全、`eba53b9` vendor_dlkm 放行）已打包 `oplus13/Baseband-guard-backup-20260902.bundle`
    - **备份分支**：`backup-6.6.118-13T-pre-bbg-removal`（位于 1268a1a，含 BBG 全部状态）
    - **验证**：主仓库 grep `CONFIG_BBG/baseband_guard/baseband-guard` 零残留；增量构建 exit 0（新 Image md5 见"当前稳定基线"）；**五项定制 → 四项定制**，后续红线校验清单不再含 BBG

18. **真机网络栈核验（2026-08-30）——当前 boot 仍是上游 v3.5 prebuilt，本地内核未刷入**
    - 背景：用户要求验证手机端 BBRv3 与 fq 队列搭配是否成功（精确到每链接/每网卡）；adb 实测（`D:\刷机\platform-tools\adb.exe`，设备 `5d6d4090`）
    - **当前内核身份**：uname 版本串 `6.6.118-android15-8-gf4dc45704e54-abogki20260808-4k #5`（2026-08-24 构建）≠ 本地基线串 `abogki20260727`；boot 分区内核段 md5 `56242631579b38e9761b774dc098f23d` ≠ 本地 Image `965e04af...`——**本地 2026-08-30 无 BBG 内核（md5 965e04af）尚未刷入真机**
    - **BBRv3 验证：成功**。`/proc/bbr_version`=3；dmesg `[0.430659] /proc/bbr_version created, version: 3`；`tcp_available_congestion_control`=reno bbr cubic、全局默认 bbr；**102/102 个 TCP 链接全部 bbr**（ESTAB+CLOSE-WAIT，详情行 `bbr:(bw:...,mrtt:...,pacing_gain:2.77344,cwnd_gain:2)`——pacing_gain 2.77344 为 v3 特征增益）；启用机制为 whitewhale KERN_TUNING（`request_set_tcp_bbr_enable: enable = 1`、`update bbr_uid` uid 10129/10163 is_p2p=1）
    - **fq 验证：未生效**。全网卡 root qdisc 无一是 fq：wlan0=htb（→ppq→htb→tsd/sfq）、rmnet_data0-3=mq（31 子队列 fq_codel）、rmnet_data4=htb、rmnet_ipa0/ifb2=fq_codel、vgate0=mq+fq_codel、lo=noqueue；`/sys/module/fq_guard/` 不存在、dmesg 无 fq_guard 日志
    - **结论与决策**：bbr 半边成功、fq 半边落空，根因 = 当前内核是上游 v3.5 prebuilt（自带 BBR v3 但**不含本地 fq_guard**，默认 qdisc fq_codel），非配置问题；本地内核刷入后预期 fq_guard 生效（白名单前缀 `rmnet_data`/`r_rmnet_data`/`wlan`/`p2p`/`wifi-aware`/`vgate`/`usb`/`rndis`/`eth`/`bt-pan`，黑名单 `rmnet_ims`）；**用户决定暂不刷机**，保留上游 v3.5 prebuilt 继续使用

19. **fq_guard_ko：给上游 v3.5 prebuilt 内核用的 fq 守护可加载模块（2026-08-30）**（根仓库 `c097e1f`，源码 `oplus13/fq_guard_ko/`）
    - **背景**：第 18 条核验后用户要求把本地 fq_guard 做成 .ko 给上游 v3.5 用（不重刷内核）；真机实测全过
    - **上游内核 API 约束与对策**：
      - `qdisc_create_by_kind()` 是本地 C3 新增导出，上游没有 → 改用上游同样导出的 `qdisc_create_dflt()`（sch_generic.c，返回 NULL 非 ERR_PTR）+ 手动 `qdisc_hash_add()`
      - `fq_qdisc_ops` 是 sch_fq.c static（上游本地都无法 extern）→ 用户态解析 `/proc/kallsyms` 经 `fq_ops_addr=` 模块参数传入；**oplus 安全补丁禁止内核态读 kallsyms**（dmesg "kernel read not supported for file /kallsyms"），内核态解析仅作 fallback；`kptr_restrict` 需先写 0（su 可写）
      - vermagic：构建时临时覆盖 `out/include/generated/utsrelease.h` 为 `abogki20260808-4k`（build.sh 自动备份恢复）；SMP/preempt/mod_unload/modversions/HZ250 两边一致
      - 签名：`MODULE_SIG_PROTECT=y` 下**未签名模块可加载**（signing.c 的 PROTECT 分支 return 0），本地密钥签名反而 fatal 验签失败 → 构建用 `CONFIG_MODULE_SIG_ALL=` 覆盖为不签名
      - CRC（MODVERSIONS）：本地 Module.symvers 与上游同源，全部符号 CRC 一致，一次通过
    - **两个新功能（相对本地 built-in）**：
      - `fqg_scan_existing()`：模块事后加载时主动遍历 init_net 现有接口排队 work，**加载即生效**（本地 built-in 开机早期注册不依赖此）
      - `event_recheck` 兜底复查（默认 true）：work 快速路径跳过时额外排一次复查——**真机抓到本地 built-in 同样存在的时序漏洞**：wifi 重连时 netd 在 fq_guard 检查之后才配 htb 且 tc 配置无 NETDEV 事件，导致守护失效；兜底复查 5s 后抓住覆盖换回（dmesg 1338.349 实锤）。**本地内置版待回移植此修复**
    - **真机验证**：insmod exit=0；scan 后 wlan0(17队列)/rmnet_data0-2(31)/vgate0(256)/rmnet_data4 全部 `qdisc fq`；rmnet_data3/4 DOWN 时正确跳过（netif_running 检查）；wifi toggle 事件驱动 + 兜底复查均生效；BBR 48 链接全 bbr 无副作用；模块参数 enable/event_recheck/delay_ms 等 sysfs 可调
    - **持久化**：`/data/adb/fq_guard/fq_guard_ko.ko` + `/data/adb/service.d/99-fq-guard.sh`（bootanim stopped 后现解析地址 + insmod；**KASLR 每次开机地址变，必须现解析**）
    - **构建**：`bash oplus13/fq_guard_ko/build.sh`（需先跑过 内核构建.sh 有 out/）；加载：`sh install.sh`（push 到设备后）

> 2026-08-28 第六轮合并后的 2 个提交（合并 + 维护记录）已推送 `github`（第五轮 3 个提交此前也已推送，2026-08-17 的"尚未推送"记录已过时）。此前 24 个提交的历史统计沿用 2026-08-16 口径。

> 2026-08-06 已清洗全部远程提交正文中的 Claude Code `Co-Authored-By` trailer 并重写历史：3 个定制提交与 3 个合并提交 hash 变更（ReKernel-X `3eb91d7cace`、BBG `cc7887d802`、Droidspaces `605e6859e4`、ACK 两轮 `be9610f4683`/`4860642a0474`、whitewhale 同步 `656ece04bd3`），上游 ack/origin 历史 hash 不变；已强制推送到 `github`。

## 构建优化决策

- **取消 AFDO、LTO、Polly**（`-fauto-profile`/`-flto=thin`/`-mllvm -polly` 已移除）——保 KMI 完整性、缩编译时间
- 保留 `-O2 -mcpu=oryon-1`（Oryon 核心微架构优化，已验证生效）
- `KBUILD_BUILD_TIMESTAMP="Mon May 12 09:09:59 UTC 2025"` 固定，勿改
- ccache：5G 上限、硬链接开启、日志关闭

## 上游合并注意事项

- **只用官方 googlesource 作为 ACK 源**，勿用 GitHub 镜像（时效不可信）
- fetch 需带 `-c http.version=HTTP/1.1 -c http.postBuffer=524288000`（TLS 不稳）
- **禁止 `-X ours` / `-X theirs`**：整片取舍会静默丢弃上游 CVE 或本地定制，必须逐冲突判断
- 冲突标记检查须同时查 `<<<<<<<`、`=======`、`>>>>>>>` 三种（只查第一种会漏）
- 派并行代理处理大批冲突时，**产出必须逐文件复核语法完整性**（曾出现括号缺失、声明丢失、参数数不匹配）；修不动就整文件 `git checkout HEAD -- <file>` 回退
- WSL 会在 BTF/vmlinux 链接阶段偶发杀进程（内存压力），**重跑即过**，非代码错误
- 大轮合并前先建备份分支，便于整轮 `git reset --hard` 撤销
- `.maintenance/` 被 `.gitignore` 的 `.*` 规则忽略，提交需 `git add -f`

## Agent skills

### 内核优化调优进行中（2026-08 起）

- grill-with-docs 会话已定案"泛化压榨"优化共识：spec 与执行包在 `oplus13/.scratch/kernel-tuning/`（根仓库提交 `493c73d`），领域词汇表 `oplus13/CONTEXT.md`、准入 ADR `oplus13/docs/adr/0001-source-layer-entry-policy.md`
- 纪律：单变量、1 天浸泡、三档判定（变好/变坏/无感）、无感即回退；浸泡期用 KernelSU service.d 脚本 A/B，定案值固化内核默认（新增 Kconfig）
- 真机实测（2026-08-15，adb 直连 `5d6d4090`）：运行时 governor 为 vendor `walt`（非 schedutil）且其 up/down_rate_limit_us 出厂即 0、PELT multiplier 出厂已 4——前两个候选被 vendor 出厂配置判死。首攻已重排为 walt `zone_max_util_pct` 80→90（全部 policy），已应用并回读验证，service.d 脚本 `99-sched-tune.sh` v2 持久化（Kconfig 固化对 vendor 模块不适用）；Day-1 双锚点（微信冷启动/桌面多任务滑动）均判定"变好"，**zone_max_util_pct=90 已定案**（service.d 脚本固化）；第二项 hispeed_load 90→70 经 Day-1 判定"变坏"，已回退出厂 90 并判死（脚本降级 v4 仅管理定案项）；下一候选：SCHED_FEAT（HRTICK/NEXT_BUDDY）→ rtg_boost_freq → HZ 250→1000
- 待命候选（写通路已验证）：SCHED_FEAT（debugfs 可挂载，NEXT_BUDDY/TTWU_QUEUE/HRTICK）、rtg_boost_freq 调高、HZ 250→1000 → IO 轴 → 内存轴；网络轴排除；PELT multiplier 已剔除（vendor 已压满）；另用户已选定 crystal_hybridswap 纯内核化激进档（参数默认值固化 + fq_guard 式内核守护，ADR-0002；注：8-16 已撤销激进档提交 `f4a1c67dc87cc`，当前仅保留 `chs_param_store` 拦截点预留位，守护实现未恢复，见第 11 条）

### Issue tracker

Issue 与 spec 以 markdown 文件存放在 `.scratch/<feature>/`（本地追踪器，不走 gh）。See `docs/agents/issue-tracker.md`.

### Triage labels

五个标准标签：`needs-triage`、`needs-info`、`ready-for-agent`、`ready-for-human`、`wontfix`，以 issue 文件中的 `Status:` 行记录。See `docs/agents/triage-labels.md`.

### Domain docs

单上下文：根目录 `CONTEXT.md` + `docs/adr/`。See `docs/agents/domain.md`.
- `CONTEXT.md` 已于 2026-08 由 grill-with-docs 会话创建（首批定案术语：泛化压榨/纯体感、配置层/源码层、稳定优先、KMI 红线、刻意配置），经 `git add -f` 纳入 `/home/wcoom` 根仓库（提交 `5b4cb5e`）；`docs/adr/` 已有 ADR-0001（源码层准入：允许原创内核改动），词汇表增补体感锚点/浸泡期/源码层准入（根仓库提交 `d420b45`）
