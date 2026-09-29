# OnePlus 13 (SM8750) 内核项目

基于 `whitewhale0612/android_kernel_common_oneplus_sm8750-C16` (Linux 6.6.118)，为 OnePlus 13 定制内核。

## 构建与打包

- **构建脚本**: `/home/wcoom/桌面/内核构建.sh`（clang-19 + ccache 伪装，增量编译，禁止运行 `make clean` 除非明确要求；内含 `export LOCALVERSION=""` 以抑制 setlocalversion 追加 `+`）
- **工具链**: `/home/wcoom/桌面/oplus13/clang-19/`
- **gki内核源码目录**: `/home/wcoom/桌面/oplus13/android_kernel_common_oneplus_sm8750`
- **打包**: `/home/wcoom/桌面/dabao.sh` 将 `out/arch/arm64/boot/Image` 复制进 `AnyKernel3-6.6.112-NOKSU-OnePlus8Elite/` 并打 zip
  - 命名规则：`AnyKernel3-<Image镜像时间>.zip`（`date -r $IMAGE +%Y%m%d-%H%M`，如 `AnyKernel3-20260811-1231.zip`；2026-08-11 起弃用提交哈希）
- **刷机到实体机（本机直接 adb，2026-09-29 更正）**：`adb -s 5d6d4090 ...`，用 `/usr/bin/adb`（Debian android-sdk 34.0.5）。本机现为**原生 Ubuntu**：`/mnt` 为空、无 Windows 侧可调用，旧记 `/mnt/d/刷机/platform-tools/adb.exe`（以及更早的 `/mnt/c/WINDOWS/system32`）均已失效。⚠️ **手机重启会让 USB 掉线**（内核日志 `USB disconnect` 后不再枚举、`lsusb` 与 `adb devices` 双双为空），需**拔插数据线**才恢复——2026-09-29 刷机时即遇到，勿误判为刷机失败。
  - 设备：OnePlus 13 PJZ110，序列号 `5d6d4090`，Android 16，KernelSU root（`ksud 3.2.5-63-ga33dbca3`，LKM 不受 boot kernel 替换影响）
  - 刷入法（无 TWRP/管理器 App 时）：zip 推送到 `/data/local/tmp/` 后解包；`su -c` 的 `cd; export PATH=$PWD/tools:$PATH; sh anykernel.sh` 必须作为整体交给 root shell，避免 Windows adb 重组参数后只让 `cd` 进入 root shell、继而报 `magiskboot: inaccessible or not found`；设置 `OUTFD=1` 后由 AnyKernel3 仅替换当前 slot boot 中的内核。
  - anykernel.sh 逻辑：有 init_boot → `split_boot`（仅换内核）+ `flash_boot`（dd 写回当前 slot boot 分区）；刷完 `adb reboot` 生效
  - **设备侧 tools 无执行位坑（2026-08-28 已修模板）**：模板目录打包进 zip 时权限丢失（magiskboot 等 0644），设备端 `/data/local/tmp` 解包后 `magiskboot: can't execute: Permission denied`——需 `chmod -R 755 tools`；模板源目录已 `chmod a+x` 修复，后续打包不再丢位
  - **新旧内核判别（版本串固定时）**：uname 无法区分，以 boot 分区内核段 md5 与本地 Image 对比为准——`dd if=/dev/block/by-name/boot<slot> bs=4096 skip=1 count=12456 | head -c 39258624 | md5sum`（boot 头 4096 字节，KERNEL_SZ 51016896/4096=12456 块）
  - 真机验证成功记录：2026-08-16（ACK 第四轮）；2026-08-17（ACK 第五轮 + seccomp 兼容修复，内核 #31，连续 50 次 `su` 全部成功）；2026-08-28（ACK 第六轮，verify PASS=5，boot 内核 md5 核对）
- **git 远程**: `origin` = 上游 whitewhale0612（只拉取，勿推送）；`ack` = `https://android.googlesource.com/kernel/common`（官方源）；`github` = 个人仓库 `Wcoom/oneplus_sm8750_kernel`（推送目标，SSH 认证）
- **当前分支**: `6.6.118-13T`

## 当前稳定基线（2026-09-11 更新）

- **HEAD**: `84708f314ec5c`（LXC「模块校验放行」；其上依次为 `bcf51b7db530e` 容器能力、`8aa156f94819e` defconfig 去重、`9caca32213f3e` wrapfd 修复；再往前是维护记录 `e79471cd46638` 与双父合并 `574270a7d4613899504118109097bf9531c6bb98`——其第一父提交为用户指定回退基线 `941a8c58f2f8d1093e1ec722057e55cbe960aa11`，第二父提交为官方 ACK `d645d30475a90d74210e3afe85e9a6ba748019b3`；按该轮回退结果恢复 BBG）
- ✅ **上述 3 个 LXC 提交已推送 `github/6.6.118-13T`**（`9caca32213f3e..84708f314ec5c`），并已打 tag `v6.6.118-13T-20260911` + 发布 GitHub Release（**Latest**，资产 `AnyKernel3-20260911-1317.zip` 32,165,562 字节，SHA-256 `63aa4b85240a576dbdb870ed51e79a80499cc4b9c69ef597b0c7122b448e4443`）
- **版本**: 固定名 `6.6.118-android15-8-gf4dc45704e54-abogki20260727-4k`（SUBLEVEL 118；`CONFIG_LOCALVERSION` 写死 + `LOCALVERSION_AUTO` 关闭，不再随提交哈希变化）
- **产物**: `out/arch/arm64/boot/Image` 39,262,720 字节，md5 `a5f5cd90bc0d339549313044f67291a1`（2026-09-11 13:17 构建，含 LXC 补丁）；打包 `AnyKernel3-20260911-1317.zip`
- **真机状态（2026-09-11）**：已刷入 slot_a，内核 build `#47`，boot 分区内核段 md5 与本地 Image 一致；Wi-Fi/fq_guard/BBRv3/netns/userns/overlayfs/ntsync 验证全通过（详见第 22 项）
- **构建脚本**（2026-08-30 修复）：`内核构建.sh` 自包含 `cd`（不依赖 cwd）+ `PAHOLE=/usr/bin/pahole`（原 6.6/prebuilts 路径随 6.6/ 删除失效；clang-19/bin/pahole 悬空链接已改指 /usr/bin/pahole v1.25）
- **ccache**: 4.32G / 5G
- `ahead origin 10755` 属正常现象（ACK 合并带入大量上游历史）
- 推送认证：GitHub PAT 权限不足（403），已改用 ed25519 SSH key（`wcoom@wsl2`）

**五项定制均已挂载并在 `out/.config` 中生效**（非仅 defconfig 声明；BBG 随 2026-09-09 回退恢复，2026-09-11 核对时确在运行）：

| 定制 | 代码位置 | 配置项 |
|---|---|---|
| fq_guard | `net/sched/fq_guard.c` | `CONFIG_NET_SCH_FQ_GUARD=y` + `CONFIG_DEFAULT_FQ=y` |
| ReKernel-X | `drivers/rekernel_x/rkx*.c` | `CONFIG_REKERNEL_X=y` |
| Droidspaces | `drivers/misc/ntsync.c` + `include/uapi/linux/ntsync.h` | `CONFIG_NTSYNC/SYSVIPC/PID_NS/IPC_NS/USER_NS/NAMESPACES/POSIX_MQUEUE=y` |
| 温度偏移 | `kernel/temp_offset_sysctl.c` + `include/linux/temp_offset_sysctl.h` | `/proc/sys/kernel/temperature_offset_celsius`（sysctl，obj-y 无条件编译） |
| Baseband-guard | `Baseband-guard@6e32d811` + `security/baseband-guard` | `CONFIG_BBG=y` + `CONFIG_LSM` 末尾 `baseband_guard` |

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


## 本地修改与维护记录（截至 2026-09-29；后续维护见第 25 项）

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

3. **Baseband-guard (BBG)** (`security/baseband-guard`, commit `cc7887d802`)——**随 2026-09-09 回退至 `941a8c58` 恢复**
   - 经 vc-teahouse/Baseband-guard `setup.sh` 接入：符号链接 + `security/Makefile/Kconfig` 挂载
   - `CONFIG_BBG=y`；`CONFIG_LSM` 末尾追加 `baseband_guard`（selinux 之后）；`BBG_BLOCK_BOOT/RECOVERY` 保持 n
   - 顶层 gitlink 精确为 `6e32d811ef5072c5454577915c118db5ba2b5c15`；源码由 `Baseband-guard-backup-20260902.bundle` 恢复

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
    - **C8 脚本清理（无内核提交）**：`verify_kernel.sh` 179→104 行（删 zram 1:2 / watermark 100 过期段）、`bpf.sh` 变量化 `KERNEL_ROOT/DEFCONFIG`、删除仓库根旧版 `dabao.sh`（519B 硬编码残留）——三脚本均在 `/home/wcoom/桌面/oplus13/`，**不属内核 git 仓库**，无提交内容
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
    - **模块侧改造**（fopbatt 电池工具包 8.2.14-beta，`/home/wcoom/桌面/fopbatt-bbg-fix/`，根仓库 b92af23）：`kot_run_lpadd_once` 改为 dm 镜像直写——`dmctl table` 读映射 → `dmctl create vendor_dlkm_<非当前slot>`（同名映射、ro=0、BBG allowlist 放行）→ `dd bs=4M conv=fsync` 直写 → `dmctl delete`；重启后 init 重建分区生效。**不写 super 分区表**
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

20. **fq_guard_ko 集成进 ReKernel-X 模块 zip（2026-08-30）**（根仓库 `510c85a`，工作目录 `oplus13/ReKernel-X-1.5/`）
    - **背景**：用户要求把 fq_guard_ko 集成进 `/storage/emulated/0/Download/ReKernel-X-1.5.zip`（KernelSU/Magisk 模块载体）并删除独立自启脚本
    - **zip 结构**：`META-INF/`（安装器）+ `customize.sh`（按 `android${AND_VER}-${CORE_VER}` 匹配 kmod/ 的 rkx ko 拷到模块根）+ `module.prop`（id=rekernel_x v1.5）+ `post-fs-data.sh`（开机 insmod）+ `kmod/`（8 个旧版 rkx ko，20260313）
    - **改动**：`kmod/fq_guard_ko.ko` 放入；customize.sh 在 `rm -rf kmod` 前加一行拷出 fq_guard_ko.ko；post-fs-data.sh 在 rkx 循环前加 fq_guard 加载段（卸载残留 → kptr_restrict=0 → 现解析 fq_qdisc_ops 地址 → insmod 传 `fq_ops_addr`，失败退化无参）
    - **新 zip 已推回手机覆盖原文件**（965KB）；**旧自启 `/data/adb/service.d/99-fq-guard.sh` 与 `/data/adb/fq_guard/` 已删除**，加载职责移交模块——**下次重启后生效**，重启前当前加载的 fq_guard_ko 继续运行
    - **注意**：① kmod/ 内旧 rkx ko（20260313）vermagic 与上游 v3.5 不匹配会静默加载失败，不影响 fq_guard_ko（本机内核已 built-in rkx，无需该 ko）；② KASLR 地址每次开机变，post-fs-data 现解析；③ 重新打包命令与集成说明见 `ReKernel-X-1.5/README.md`；④ zip 打包时 .sh/update-binary 必须 0755 权限

21. **2026-09-09 回退并合并 ACK 第七轮**（内核合并 `574270a7d4613` + 维护记录 `e79471cd46638`）
    - 按要求将 `6.6.118-13T` 回退到 `941a8c58f2f8d1093e1ec722057e55cbe960aa11`，建立备份分支 `backup/rollback-20260909-pre-reset`，再以双父合并提交吸收官方 `ack/android15-6.6` 最新 `d645d30475a90d74210e3afe85e9a6ba748019b3`。
    - ACK 增量 21 个提交，共同基线为 `5ef17cb58b6e6ede5281d9950baf0738c9c5f14d`；L0 零冲突；`android/abi_gki_aarch64.stg` 仅新增；vendor hooks 无本地改删。
    - 红线通过：KABI 槽位、ghost_task×12、NTSYNC×97、SUBLEVEL 118、ZRAM=n、Crystal Hybridswap、FQ_GUARD、ReKernel-X、BBG 与命名空间配置均保持。
    - 增量构建通过，Image 39,262,720 字节，SHA-256 `26777f41d73a0fd9fc3e6e15c40c34d71575478ca1acba70355f833667ea5549`；打包 `AnyKernel3-20260909-2343.zip`（32,162,700 字节，SHA-256 `d421eb9face9c43463ed09d784a0c074d208d083ccc509425c50ee442dce141f`），已发布 GitHub Release `v6.6.118-13T-20260909`。

22. **DDL 越界写重启定位与 LXC 补丁重移植（2026-09-11）**（内核 `8aa156f94819e` + `bcf51b7db530e` + `84708f314ec5c`；`op_mods` `a772844`；已刷机验证）
    - **当日异常重启 6 次**（dropbox SYSTEM_BOOT 时间戳 01:17 / 02:40 / 04:20 / 10:00 / 10:44 / 11:50）。⚠️ **pstore 的 `console-ramoops-0` 被 DDR 复位电位翻转损坏（乱码），不可用**；唯一干净的崩溃日志来源是 **qcom minidump**：`/data/persist_log/DCS/de/minidump/SYSTEM_LAST_KMSG@*@*@<时间>.dat.gz`，其中 `minidump.bin` 用 `strings -n 8 | grep -aE "^\[ *<时间戳>"` 可提取完整未损坏的 panic 文本（含寄存器与调用链）。设备上 `oplus_bsp_sched_assist.ko` **不在 vendor_dlkm/vendor 任意目录**（467 个模块文件里没有，`[permanent]`，疑来自 vendor_boot ramdisk），无法直接反汇编
    - **真根因（已完整定位，非本地引入）**：OEM 模块内部命中 UBSAN 越界陷阱 → die → panic 重启。
      - `Internal error: UBSAN: array index out of bounds: 00000000f2005512 [#1] PREEMPT SMP`；`pc : update_ddl_hit_history+0xf8/0x11c [oplus_bsp_sched_assist]`；`lr : oplus_replace_next_task_ddl+0x174/0x1d8`
      - 调用链：`do_swap_page`/`kswapd` → `schedule` → `pick_next_task_fair` → `walt_cfs_replace_next_task_fair [sched_walt]` → `android_rvh_replace_next_task_fair_handler [sched_assist]` → `oplus_replace_next_task_ddl` → `update_ddl_hit_history`
      - 两次崩溃线程/CPU 不同（`kswapd0`、`#APM_light-weig`）但 pc 完全相同
      - 机制：vendor `sa_ddl.c` 的 `update_ddl_hit_history()` 用 `p->pid` 索引 `ddl_sdata[PID_MAX_DEFAULT]` 并 `memset` 24B + `strscpy_pad` 写入；`p = ots->task` 失效时 pid 异常 → 越界写 → 命中模块内固化的 `brk #0x5512`（`UBSAN_BRK_IMM=0x5500`、`MASK=0x00ff`，kind=0x12=18 即 `ubsan_out_of_bounds`）→ `arch/arm64/kernel/traps.c` 的 `ubsan_handler()` 无条件 `die()` → 因 `CONFIG_PANIC_ON_OOPS=y` 必重启。相关配置（UBSAN_TRAP/UBSAN_BOUNDS/PANIC_ON_OOPS）**均为 GKI 官方默认，非本地引入**
    - **vendor 源码级修复已提交**（`op_mods` 独立仓库，HEAD 原 `d50b305` = PJZ110 16.0.9.401 抽取的 sched_assist 源码，新提交 `a772844`）：`update_ddl_hit_history()` 的 `if(p)` 改为 `if (p && p->pid > 0 && p->pid < PID_MAX_DEFAULT)`（异常输入只丢弃本次统计）；`oplus_replace_next_task_ddl()` 的 `IS_ERR_OR_NULL(ots)` 改为 `IS_ERR_OR_NULL(ots) || IS_ERR_OR_NULL(ots->task)`。⚠️ **需重编 `oplus_bsp_sched_assist.ko` 并部署才生效**，当前仅是源码留存——AnyKernel3 刷机链不换模块（`anykernel.sh` 里 `do.modules=0`）
    - ⚠️ **被否的内核兜底方案（务必记住，勿重犯）**：`arch/arm64/kernel/traps.c` 的 `ubsan_handler()`「报告 + `arm64_skip_faulting_instruction` 跳过陷阱继续执行」**机制不成立**。clang 的 `-fsanitize-trap` 把 `brk` 放在函数末尾 out-of-line 冷块里（现场机器码 `Code: a94257f6 a8c47bfd d50323bf d65f03c0 (d42aa240)` = `ldp; ldp; autiasp; ret; brk #0x5512`），跳过 brk 后 PC 落在陷阱块之后，**可能直接进入下一个函数序幕** = 执行野代码；且该越界写本身仍会执行。补丁已回退，留存 `.scratch/crash-20260911/traps_ubsan_recover_REJECTED.patch`，结论见同目录 `README-审核结论.md`。**将来若仍要做内核侧兜底**：① 首选给 `android_rvh_replace_next_task_fair` 该 vendor hook 加开关；② 次选真正的 fall-through——从 brk 地址向前扫描条件分支（b.cond/cbz/cbnz/tbz/tbnz）找到目标为该 brk 的那条，令 `pc = 分支地址 + 4`，扫不到就维持 `die()`；③ **不要用 `panic_on_oops=0`**（崩溃在持 rq lock 的调度路径，die 会在原子上下文调度 → hang）
    - **当前实际处理（持久化关闭 DDL）**：`echo 0 > /proc/oplus_scheduler/sched_assist/sched_ddl_enabled`（0666 可写；DDL 路径整体不被调用 → 既不 panic 也不执行越界写；已验证有效）。2026-09-20 已将 `op_mods/deploy/99-oplus-sched-ddl-guard.sh`（`op_mods` 提交 `53674b4`）部署到 `/data/adb/service.d/`，权限 `0755`，设备当前值为 `0`；每次启动会等待节点出现后自动重写。治本仍是用完整 OEM 源码重编已含 `a772844` 的 `oplus_bsp_sched_assist.ko`。
    - **Droidspaces/LXC 补丁重移植**：先查证上游 `cctv18/oppo_oplus_realme_sm8750` workflow 容器段引用的 5 个补丁（`fix_sysvipc_kabi_6_7_8` / `fix_oplus_bsp_midas` / `ntsync_base` / `ntsync_compat_android15-6.6` / `evdi_drm`）**与本地 8-05 保存版逐字节相同**，且 9-09 回退 + ACK 第七轮后 Droidspaces 落地**依然完整**（sched.h KABI 槽位 1535-1536、pid.c ghost_task×12、ntsync.c/ntsync.h/ntsync_fixup.c、drivers/misc 注册、out/.config 11 项配置全生效）。新发现并移植了上游 `droidspaces_patch/Kernel_6.6.patch`（9.6KB，标题「添加lxc支持并修复KABI兼容性」，**workflow 未引用**），内核仓库 3 个新提交：
      - `8aa156f94819e`：gki_defconfig 删除重复的 `CONFIG_NAMESPACES=y`（补丁 `echo >>` 追加行与第 43 行重复，触发 `override: reassigning` 告警；`out/.config` 不变）
      - `bcf51b7db530e`：容器能力——overlayfs 放宽（case-insensitive 底层不再拒绝挂载，改强制 userxattr=true/index=false/redirect_mode=NOFOLLOW/xino=OFF/metacopy=false；`ovl_dentry_weird()` 去掉 DCACHE_OP_HASH|COMPARE；`ovl_init_fs_context()` 显式设 override_creds）、`net_ext` 承载 nftables pernet 状态、netdevice l3mdev_ops 移入 `ANDROID_KABI_USE(8,...)`、cgroup v1 noprefix 补 `subsys.name` 符号链接
      - `84708f314ec5c`：模块校验放行（`info->sig_ok = true`、`check_version()` bad_version 分支 `return 0`→`return 1`）——**可单独 revert**
      - 备份分支 `backup/pre-lxc-20260911` @ `9caca32213f3e`
    - **KMI/CRC 实测结论（纠正了审核的部分判断）**：
      - pahole 实测 `struct net` **布局不变**（新增的 `net_ext *ext` 落在 `bpf` 之后的 24 字节对齐空洞内，`xfrm@2944`、`sizeof=4160` 均未变）；`struct net_device` 在 `CONFIG_NET_L3_MASTER_DEV=n` 下走 `#else` 分支保留原 `ANDROID_KABI_RESERVE(8)`，布局也不变
      - ⚠️ **但 `__GENKSYMS__` 的 CRC 按类型定义计算**，新增字段改变 `struct net` 定义 → 所有引用它的导出符号 CRC 变化。实测 dmesg `disagrees about version`：刷机前 `#42` 仅 **18 条**（tls/bluetooth/virtio_balloon 等，这些模块因此加载失败），刷机后 `#47` **3443 条**（tipc 87、qca_cld3_peach_v2 75、qca_cld3_peach 75、qca_cld3_kiwi_v2 74、bluetooth 59、ipam 56、mac80211 54、cfg80211 54、tls 53、nfc 52…），全靠 `check_version` 恒返回 1 才放行
      - **实测驱动仍正常工作**：Wi-Fi 在 149 条 CRC 不匹配下连接成功（`魏5G`，11ac，RSSI -53，650Mbps，IP 192.168.1.142）
      - **教训：CRC 不匹配 ≠ 布局不匹配；但 `CONFIG_MODVERSIONS` 事实上被关闭，将来真不兼容的模块也会被静默加载**
    - **刷机结果（已验证）**：构建产物 `out/arch/arm64/boot/Image` 39,262,720 字节、md5 `a5f5cd90bc0d339549313044f67291a1`（13:17）；打包 `AnyKernel3-20260911-1317.zip`，经 `anykernel.sh` 刷入 slot_a 并重启：boot 分区内核段 md5 与本地 Image **完全一致**，build 号 `#42` → **`#47`**，boot_completed=1。验证通过项：关键模块全在（rmnet_core/cnss2/qca_cld3_peach_v2/oplus_bsp_sched_assist/sched_walt/kernelsu，共 670 模块）；Wi-Fi 连接成功；**fq_guard 生效**（`cnss_pci ... wlan0: fq_guard: root qdisc forced to fq (17 tx queues)`）；`/proc/bbr_version`=3；`unshare -n`（netns）与 `unshare -U`（userns，max_user_namespaces=41088）均通；overlayfs 挂载/读 lower/写 upper/umount 全通过；`/dev/ntsync` 0666
    - **证据与脚本**：`oplus13/.scratch/crash-20260911/`（minidump 解出的 SYSTEM_LAST_KMSG、console-ramoops 原始件、两轮独立审核结论、被否补丁）；刷机/验证脚本 `flash_and_verify.sh`（含 Windows adb 引号处理注意事项）
    - ⚠️ **踩坑与运维要点**：
      - **WSL 构建内存**：本次改动触及 `include/net/net_namespace.h` 核心头 → vmlinux + BTF 全量重生成，`pahole` 曾占 6.7GB RSS，把 7.9GB 物理内存 + 4GB swap 打满、触发 OOM kill（`Out of memory: Killed process ... pahole`），表现为构建长时间无进展。诊断：`ps -eo pid,etime,pcpu,rss,comm | grep pahole` + `cat /proc/<pid>/stack`（可见 `folio_wait_bit_common → do_swap_page` 的 swap 抖动栈）+ `vmstat 1`（si/so 上万）。内存充足时同一构建约 1 分钟即过。**改核心头前先确认可用内存**
      - **`pkill -f "<模式>"` 会匹配自身命令行导致 shell 自杀**（本次又踩一次：`pkill -f "make -j8"` 杀掉自己的 bash）。必须用 `pkill -x make` 或 `pkill -f "make [-]j8"` 这类不自匹配写法
      - **Windows adb 引号重组**：`adb.exe shell "su -c '...'"` 里嵌套引号会被拆散（实测 `dmesg | grep "x y"` 变成多个参数、`lsmod | grep -cE "^(a|b)"` 报 `syntax error: unexpected '('`）。可靠做法：脚本 `base64 -w0` 后经 `adb shell "echo <b64> | base64 -d > /data/local/tmp/x.sh"` 传输，再 `su -c 'sh /data/local/tmp/x.sh'` 执行
    - **本次待办**：① ✅ DDL 关闭已持久化（KernelSU `service.d`，`op_mods` 提交 `53674b4`）；长期治本仍待拿到完整 OEM 源码，重编并部署含 `a772844` 边界检查的 `oplus_bsp_sched_assist.ko`，以恢复 DDL 功能。② ✅ **已完成**——内核改动已推送 `github/6.6.118-13T`（`9caca32213f3e..84708f314ec5c`），tag `v6.6.118-13T-20260911` 已推送并获得 GitHub Release（**Latest**，标题「2026-09-11｜6.6.118-13T：移植 LXC 容器支持补丁」，资产 `AnyKernel3-20260911-1317.zip` 32,165,562 字节，SHA-256 `63aa4b85240a576dbdb870ed51e79a80499cc4b9c69ef597b0c7122b448e4443`）。③ LXC 补丁涉及的 KMI 脆弱性：**一旦将来启用 `CONFIG_NF_TABLES=y`，`nft`(16B) 会从 `struct net` 中部移除，届时是真 KMI 破坏**，启用前必须 `pahole -C net out/vmlinux` 复核并评估厂商模块兼容性。④ 审核提出的 overlayfs 残留缺陷（上游原有行为，未修）：降级写在逐项解析的 `ovl_mount_dir_check()` 中，后续显式 `index=on`/`metacopy=on`/`xino=on`/`redirect_dir=on` 会覆盖它；`kernfs_create_link()` 返回值未检查；`net/core/net_namespace.c` 在 KEYS=y + NF_TABLES=n 下有 unused label（仅告警）

23. **2026-09-20 第八轮 ACK 合并与 DDL 保护**（内核合并 `3ab6beb853043`，维护记录 `301e1f9058227`；`op_mods` `53674b4`）
    - 官方 ACK 从 `d645d30475a90d74210e3afe85e9a6ba748019b3` 更新到 `448c303366032107c46d39006c8127a5ca967a26`，共 11 个提交；备份分支 `backup/pre-ack-20260920`。
    - 唯一冲突为 `android/abi_gki_aarch64_sunxi` 的 L1 符号追加；红线通过：Droidspaces KABI、`ghost_task` 12 行、NTSYNC×97、`SUBLEVEL=118`、`CONFIG_ZRAM=n`、BBG/FQ_GUARD/ReKernel-X 与命名空间配置均保留；未使用 `-X ours/-X theirs`。
    - 构建 exit 0：Image 39,262,720 字节，SHA-256 `913729fd15323980adb210bcb9f49e1fc949adc0d92433803e05934503fe9233`；刷机包 `AnyKernel3-20260920-2334.zip`，SHA-256 `401ed0bd85db609b4269988b13df12033c40f26db299303ab617af8030fc0164`，ZIP 内 Image 哈希一致；未自动刷机。
    - DroidSpaces panic 保护已部署到当前设备：`/data/adb/service.d/99-oplus-sched-ddl-guard.sh` 权限 `0755`、设备端 SHA-256 与仓库一致、`sched_ddl_enabled=0`；本次未重启，启动以来无新的 UBSAN/Oops/panic。

24. **2026-09-23 第九轮 ACK 合并**（内核合并 9a0664240ae5，维护记录 d1b7b6d3e399）
    - 官方 ACK 从 448c303366032107c46d39006c8127a5ca967a26 更新到 700526826edfa1bcd25d5b8090a793d9b53f8e94，共 3 个提交（Siengine ABI 符号、x86/mm switch_mm_irqs_off 顺序修复、KVM arm64 iommu identity domain 校验）；备份分支 backup/pre-ack-20260923。
    - L0 零冲突：android/abi_gki_aarch64.stg 自动合并仅 +20 行、android/abi_gki_aarch64_siengine 仅 +2 行；未使用 -X ours/-X theirs。红线通过：Droidspaces KABI（sched.h 1535/1536）、ghost_task 12 行、NTSYNC×97、SUBLEVEL=118、CONFIG_ZRAM=n、BBG/FQ_GUARD/ReKernel-X 与命名空间配置均保留，vendor hooks 无改删。
    - 构建 exit 0：Image 39,131,648 字节，SHA-256 31908e677667b600e828ef5dc7a165137770e0931af6f38a3b390dc5fc65a789；刷机包 AnyKernel3-20260923-0228.zip，SHA-256 2f32c4ef1444f2208b73c0bd2fd13f230165f5053f4a1e184cf50995fca514a7，ZIP 内 Image 哈希一致；本次未自动刷机。
    - 环境说明：迁移到 Ubuntu 26.04 后按本任务书完成首轮维护；项目/工具链位于 /home/wcoom/桌面/oplus13，推送经 SSH 到 github/6.6.118-13T（301e1f905822..d1b7b6d3e399）。

25. **2026-09-29 第十轮 ACK 合并（6.6.143 LTS 大轮）**（内核合并 `b2008ec65a5d`，维护记录 `599b6aaf0318`）
    - 官方 ACK 从 700526826edfa1bcd25d5b8090a793d9b53f8e94 更新到 55dbf85d9283442cd5a1eaf581ce0464ab45cb71（2026-09-25），共 **219 个提交**，主体为 `Merge tag 'android15-6.6.143_r00'`（`Merge 6.6.143 into android15-6.6-lts` → `Linux 6.6.143`）：netfilter/mptcp/af_unix/rxrpc/sctp/mm-hugetlb/memory-failure/USB serial/typec/drm/mmc/i2c/thunderbolt/ksmbd 等大批 CVE 与稳定性修复，ANDROID 侧含 incfs lockdep 子类、KVM arm64 THP PFN 校验、f2fs `FI_NO_EXTENT` 修复、GKI db845c 符号追加，另有 serdev/hci_qca/genetlink/CIFS-SWN 若干上游自身 Revert；备份分支 `backup/pre-ack-20260929`（@ d1b7b6d3e399）。
    - ⚠️ **任务书的 `curl +log` 尖端核对本轮不可用**：googlesource 的 HTTP 端点返回 Google 503（直连与经 127.0.0.1:7897 均如此），改用**同一官方源**的 `git ls-remote` 取证（git 协议正常），未退回 GitHub 镜像。
    - **唯一冲突**为 L1 机械冲突 `Makefile` 的 `SUBLEVEL`（上游 143 vs 本地 118），按红线第 6 条保留本地 118，处理后 `Makefile` 相对 HEAD 无其他差异；未使用 `-X ours/-X theirs`。红线全过：Droidspaces KABI（sched.h 1535/1536）、ghost_task 12 行（pid.c 唯一 hunk 在 `__pidfd_fget`，未触及 ghost_task 区域）、NTSYNC×97、SUBLEVEL=118、CONFIG_ZRAM=n、BBG（gitlink 6e32d811 + `CONFIG_LSM` 末尾 baseband_guard）/FQ_GUARD/ReKernel-X 与命名空间配置均保留；`include/trace/hooks` 本轮**零改动**；导出符号仅新增 `ib_umem_check_rereg`、`nf_ct_helper_expectfn_destroy`，RDMA 两符号随文件迁移净零；`include/net/sock.h`、`include/linux/mm.h` 仅声明级改动、无结构体布局变化；冲突标记三型零残留。
    - 构建为**全量编译**（工作区迁移后 `out/` 不存在；ccache 2.7G/5G）exit 0、`error:` 0 条：版本 6.6.118-android15-8-gf4dc45704e54-abogki20260727-4k，Image 39,131,648 字节，SHA-256 `2521d004f478cfa69acfb1be04ba5f2dfbd78f203e9e6ea9924b84cd9387d858`；刷机包 `AnyKernel3-20260929-1523.zip`，26,000,413 字节，SHA-256 `641ab312a4bde2a951d1055ee89f9e818b91619a135ecf6f78d398f075cb34e0`，包内 `Image-dtb` 哈希与构建产物一致。
    - **已刷真机并验证通过（2026-09-29）**：刷前备份 `flash-backups/flash-20260929/preflash-20260929/`（boot_b/init_boot_b/vbmeta_b，两侧哈希一致）；AnyKernel3 刷入 slot `_b`，boot_b 由 `8d1f5545…` 变为 `45809b4c…`；重启后 boot 内核段 md5 `1aea1169190b2398f1f54bb297242c4c` 与本地 Image **逐字节一致**、build `#1`、`boot_completed=1`；KernelSU root 完好（`u:r:ksu:s0`）、连续 50 次 `su -c true` 全成功、`/dev/ntsync` 0666、温度偏移 sysctl 可读、bbr v3、netns/userns 与 overlayfs 可用、模块 670 个、`zram0` swap 9.96G 正常（ZMS 原生启用、活动算法 `[zstdn]`）、fq_guard 强制 fq（rmnet_data3/vgate0，wlan0 17 队列）。
    - **刷机验证的取证结论**：dmesg 19 条 WARNING 全部为既有 vendor 噪声（`/proc/task_info`、`/proc/bcl_stat`、`/proc/oplus_mem` 等重复注册、spmi-pmic-arb、context_tracking）与已知模块 CRC `disagrees about version`（靠 `check_version` 放行），**无一条来自本次合并的代码路径**；本轮合并未触碰 `kernel/seccomp.c` 与 crystal_hybridswap 源码（`zram_drv.c` 仅带入上游 `zram_bvec_write_partial()` UAF 修复一条），故 KernelSU 的 `seccomp_filter_release has_call_to_spin_lock = 1` 信息与 zram 的 `zstdn` 均非本次引入。
    - ⚠️ **发现并已处理：DDL 守护脚本不在设备上**。`/data/adb/service.d/` 只剩 4 个脚本（`99-host-exec.sh`/`box-pikproxy`/`boxproxy-box`/`.zn_cleanup.sh`），无 `99-oplus-sched-ddl-guard.sh`，开机 `sched_ddl_enabled=1`（即第 22 项那类越界写崩溃的风险开关处于开启态）。已按第 22 项既定缓解措施把运行时值写回 `0`；**持久化脚本尚未重新部署**（本地副本 `op_mods/deploy/99-oplus-sched-ddl-guard.sh`，是否放回待定）。
    - **adb 环境更正**：本机现为**原生 Ubuntu**（非 WSL；`/mnt` 为空、有真实 `wlp1s0`），手机 USB 直连，直接用 `/usr/bin/adb`（本文件「刷机到实体机」一条的 Windows `adb.exe` 记法已过时）。⚠️ 手机重启后 USB 会掉一次（`lsusb` 无设备、adb 空），需**拔插数据线**才重新枚举。
    - **尚未推送 github**。

> 2026-09-29 第 25 项记录第十轮 ACK 合并（219 提交，含 6.6.143 LTS 合并）与全量构建/打包/真机刷入验证（未推送）；2026-09-23 第 24 项记录第九轮 ACK 合并（3 提交）与构建/打包校验；2026-09-20 第 23 项记录第八轮 ACK 合并、构建/打包校验与 DDL 持久化保护；2026-09-11 第 22 项记录 DDL 越界写重启定位与 LXC 补丁重移植（含刷机验证 `#47`）；此前维护记录中的真机验证结论仍按各自日期有效。

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
- `CONTEXT.md` 已于 2026-08 由 grill-with-docs 会话创建（首批定案术语：泛化压榨/纯体感、配置层/源码层、稳定优先、KMI 红线、刻意配置），经 `git add -f` 纳入 `/home/wcoom/桌面` 根仓库（提交 `5b4cb5e`）；`docs/adr/` 已有 ADR-0001（源码层准入：允许原创内核改动），词汇表增补体感锚点/浸泡期/源码层准入（根仓库提交 `d420b45`）
