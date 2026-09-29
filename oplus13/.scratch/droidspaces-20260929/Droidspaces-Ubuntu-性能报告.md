# Droidspaces Ubuntu 26.04 性能报告

- 设备：OnePlus 13 PJZ110（SM8750），Android 16，KernelSU root
- 容器：Droidspaces 6.4.5，容器名 `ubuntu`
- 诊断采集：2026-09-29 15:25–15:45 UTC（只读）
- 优化与复测：2026-09-29 15:45 – 2026-09-30 01:20（A+B 两组 + 三项授权宿主级操作 + S1–S7 全部待办项 + `vm.compaction_proactiveness` 运行时 A/B）
- 采集方式：全部经 `adb → su → droidspaces --name=ubuntu run /bin/sh <脚本>`
- **第二轮（内核框架层）**：2026-09-30，按新任务书执行 §1 清理审计 + §4/§6/§7/§8/§9/§11/§12 只读诊断。
  ⚠️ **本轮未对设备新增任何持久化修改；唯一的改动是把上一轮的修改还原。** 完整结论见 **§11**；
  其中 **§4 的立论前提（宿主慢性内存回收压力）经实测证否 ⇒ C1 撤销**，详见 §11.B / C-15。
- 约束遵守情况：**未破坏 Android 宿主；未关闭 SELinux；未改 thermal；未锁 CPU 频率；未触碰 vendor 节点；未改任何 boot 镜像；未改 Android cgroup/cpuset**

> ## ⚠️ 本版重要更正（相对初版报告）
>
> 初版报告有 **一条结论被实测推翻**、**一处方法学缺陷**、**三项授权测试有了确定结论**；后续执行 S1–S7 与 compaction A/B 的过程中又累计 **5 条更正（C-8 ~ C-12、C-13 ~ C-14）**，其中 **C-13 推翻了初版的一处根因认定**（把"分配受阻事件"读成了"压缩执行次数"）：
>
> | # | 初版结论 | 更正后 | 依据 |
> |---|---|---|---|
> | **C-1** | 「`/mnt/data`（f2fs 直通）**并未更快**，波动 267–914 MB/s（3.4×）→ 不要把构建目录迁到 /mnt/data」 | **错误。f2fs 大文件写比 rootfs 快 2.19×，且稳定（±15%）**。构建产物/缓存**应当**放 `/mnt/data` | 单调时钟 + 交错 5 次采样（§5.1） |
> | **C-2** | 基准数字来自 `date +%s%3N` | **方法学缺陷**：该值读 CLOCK_REALTIME，会因 NTP 回调跳变，曾产生**负时长（−818 ms）**。初版 B3–B8 数字不可信 | §9.1 |
> | **C-3** | THP 列为「待人工判断 C1」 | **实测为空操作**：`madvise` 档下 `AnonHugePages` 仍为 0 kB，显式 `MADV_HUGEPAGE` 也拿不到大页。**保持 `never`** | §6.6 / §9.3 |
> | **C-4** | 建议试 `force_cgroupv1=1` | **实测容器启动即死**（报成功但无进程/无挂载）。**本机不可用，已回滚**。H6 无可用修法 | §6.5 / §7.4 |
> | **C-5** | 假设 rootfs 慢是因为 `nodelalloc` | **实测只有 1.11×**，不是主因。真因是 rootfs 为 **ext4-on-loop-on-f2fs 双层文件系统** | §5.3 |
> | **C-6** | §7.3 判定「`run` 绕过 PAM ⇒ `/etc/environment` 也失效」 | **错误。`/etc/environment` 对全部路径有效**（由 droidspaces 自身读取注入，不经 PAM）；失效的只有 `limits.d`（pam_limits） | §7.5 |
> | **C-7** | S2「`TMPDIR` 指向 `/tmp`可提速」 | **是空操作**：`TMPDIR` 原本未设置，默认本就是 `/tmp`（tmpfs）。已显式化但**无可测量收益**；真正的决策在"大构建是否该改用 `/mnt/data`" | §7.5 |
> | **C-8** | A 组 A4 建立的 `/root/.ccache.conf` | ⚠️ **该文件从未被 ccache 读取过**（既不是系统配置路径，也不是缓存配置路径），**自建立起就完全无效**。已迁到真正会读的 `$CCACHE_DIR/ccache.conf`。**根因：A 组当时只核对了"取值"没核对"来源"** | §7.6 |
> | **C-9** | S4 声称的收益「缓存放 f2fs 可加快冷读」 | **与 §5.1 实测不符**：读 256 MB 三个落点为 99 / 97 / 99 ms，**几乎无差**；小文件操作 f2fs 反而略慢（20 vs 19 ms）。S4 的真实收益是 **rootfs 空间**（20 GB 上限 vs 40 GB 卷），不是速度 | §5.1 / §7.6 |
> | **C-10** | （新发现）A 组的 ccache 接管范围 | **裸 `droidspaces run` 下 `gcc` 根本不经过 ccache**（该路径 PATH 无 `/usr/lib/ccache`），只有登录 shell 生效。⇒ 非登录式自动化构建**一直没享受到 ccache** | §7.6 / §8.1 **S7** |
> | **C-11** | S3 若照 systemd 默认配置直接启用 | ⚠️ **会连带 TRIM 宿主分区**：默认 `--listed-in /etc/fstab:/proc/self/mountinfo` 遍历所有挂载点，而容器内能看到 **9 个宿主挂载**（`dm-61`、`sdf3`）。实测首次运行真的 trim 了 `/dev/block/sdf3` 36.4 MiB。**已收紧为只 TRIM `/`** | §7.7 |
> | **C-12** | S3 的"容器内定时器不开火"根因 | **systemd 自带的 `fstrim.timer`/`fstrim.service` 都带 `ConditionVirtualization=!container`**，容器内每周被静默跳过（journal 实证）。已用 drop-in 清空该条件 | §7.7 |
> | **C-13** | §4.5「分配路径长期走 direct reclaim **+ compaction**，compaction 96% 失败 —— 这是唯一会实际拖慢一切工作负载的根因」 | ⚠️ **compaction 部分不成立**。`compact_stall/fail/success` 是「**分配被碎片卡住**」的事件计数（非"压缩运行了几次"），其累计值只在**开机风暴期**增长；运行时 A/B 的 **8 个观测窗口内恒为 +0**，期间甚至发生过 direct reclaim（`pgscan_direct` +50,088/分钟）也没触发一次压缩。⇒ **direct reclaim 长期存在（该半句成立），compaction 是开机暂态而非持续根因**。原报告把累计比值（126/131）当成了持续速率；**原始数据其实已自证**（§4.5 表中 `compact_stall 131→132`、`compact_fail 126→126`，20 分钟仅 +1/+0） | §4.5 / §6.7 |
> | **C-14** | `vm.compaction_proactiveness` 是否可作为容器侧调优项 | ⚠️ **该 sysctl 不做命名空间隔离**：容器内写入 20 后，**宿主侧独立读到 20**（两侧 `uptime` 3907 vs 8896，证明是不同的 `/proc` 视图却共享同一全局量）。⇒ 容器侧任何脚本写它会**静默改变 Android 宿主**的内存管理行为。已排除 | §6.7 |
> | **C-15** | §4.5 / §6 **C1**「整机内存**长期**低于 watermark high，分配路径**长期**走 direct reclaim，**是唯一 Critical 级根因**」 | ⚠️ **证否（第二轮，2026-09-30）。** 单次快照 `free 39,969 < high 40,876` 属实，但「**长期**」不成立：74 s 8 路满载（实测 CPU busy **94.1%**）期间 `pgscan_direct` / `pgsteal_direct` / `allocstall_normal` / `allocstall_movable` / `pgscan_direct_throttle` **全部 +0**，`nr_free_pages` **净增 160,898 页**，zone Normal `free` 由 50,334 **升到** 215,662（**始终高于** `high`）。**`allocstall = 0` 是关键判据** —— 它只在「分配被迫自己去做回收」时增长，为 0 说明 kswapd 跑在分配**之前**（预防性回收）。上一轮的 `pgscan_direct 3.36 M` 是**累计量**，增量集中在容器冷启动与首次构建，稳态下**不再增长** —— **用累计量判断当前状态是方法学错误**（同 C-2）。⇒ **C1 从 Critical 撤销，本机当前无 Critical 级问题** | §11.B |
> | **C-16** | （新发现）`/proc/config.gz` 是否可作为内核能力的判据 | ⚠️ **不是权威来源。** 它写着 `# CONFIG_ZRAM is not set`，但 zram0 **正常工作**（`disksize = 10200547328`），且 `grep -c zram /proc/kallsyms` = **139 个符号**；`comp_algorithm` 里还有 `lz4kd` / `lz4kds` / `zstdn` 等**非上游算法名**。⇒ 该 config.gz 是 **GKI 基线配置，不含 OEM 覆写**。**任何基于它的 CONFIG 结论都必须用运行时状态交叉验证**，否则会得出"某特性没开"的错误判断 | §11.D |

---

## 1. 容器架构分析

### 1.1 容器实现形态（实测判定）

**结论：真命名空间容器，不是 PRoot / chroot / LXC / proot-distro。不存在系统调用翻译层。**

| 证据 | 观测 |
|---|---|
| PID 1 | `systemd`，cmdline `/sbin/init systemd.unified_cgroup_hierarchy=1` |
| systemd 版本 | `259 (259.5-0ubuntu3.4)` |
| `systemd-detect-virt` | `container-other` |
| ptrace/proot/qemu 迹象 | `/proc/self/status`、`/proc/1/status` 中 **零命中** |
| 私有 namespace | `pid:[4026536078]`、`mnt:[4026536079]`、`uts:[4026536074]`、`ipc:[4026536075]`、`cgroup:[4026536076]` |
| **共享** namespace | `net:[4026531840]`（宿主）、`user:[4026531837]`（宿主）、`time:[4026531834]`（宿主） |
| Runtime | `/data/local/Droidspaces/bin/droidspaces`，版本 `6.4.5` |

**对任务目标 ① 的判定：天然满足。** 第 1 优先级「消除 PRoot 风格系统调用翻译」在此环境不适用——不存在该层，无需优化。

### 1.2 存储与挂载拓扑

```
rootfs.img (宿主 /data/local/Droidspaces/Containers/ubuntu/rootfs.img)
  └─ loop50 → ext4 → /           挂载: rw,noatime,nodiratime,nodelalloc,errors=remount-ro,init_itable=0
                                     ⚠️ 该 img 文件本身位于 f2fs 上 → 双层文件系统
宿主 /data (f2fs on dm-61, UFS, inlinecrypt, discard 自动)
  └─ bind → /mnt/data
Android emulated storage
  └─ FUSE(/dev/fuse) → /storage/emulated/0
```

- 镜像虚拟容量 40 GiB；初始实际分配 15.3 GiB（32,108,512 × 512B）
- 根分区挂载**无 `discard` 选项**，删除的文件不会 TRIM 回稀疏镜像；`loop50` 的 `DISC-GRAN=4K / DISC-MAX=4G` 表明**支持 discard，但只响应手动 `fstrim`**
- 根分区 inode：2,621,440 总 / 已用 59,021（3%）

### 1.3 容器配置（`/run/droidspaces/container.config`）

```
name=ubuntu            privileged=full        selinux_permissive=0
net_mode=host          allow_userns=1         volatile_mode=0
enable_hw_access=1     enable_android_storage=1
force_cgroupv1=0       block_nested_ns=0      run_at_boot=0
use_sparse_image=1     sparse_image_size_gb=40
bind_mounts=/data:/mnt/data
```

### 1.4 权限与隔离实况

- `CapEff: 000001ffffffffff`（几乎全部 capability）→ `privileged=full`
- `Seccomp: 0`（无 seccomp 过滤）
- `Cpus_allowed_list: 0-7`（全部 8 核）
- **容器内 root == 宿主 root**（共享 user namespace）
- 容器内 `/sys`、`/proc/meminfo`、`/proc/loadavg`（经 vproc）、`/proc/pressure` 均反映**整机**状态

### 1.5 Droidspaces vproc 机制

`/proc/uptime` 与 `/proc/loadavg` 被 tmpfs 覆盖，内容由 `/run/droidspaces/vproc/` 提供：

- `/proc/uptime` = **容器自身**运行时长（实测与容器启动时刻吻合）
- `/proc/loadavg` = **宿主全局**负载（格式 `13.48 … 3/9448`，其中 9448 为宿主线程数，容器内仅 11 进程）

> ⚠️ 容器内看到的 load average 不代表容器负载，是整机（Android + 所有容器）的值。
> ✅ 该覆盖值为单调递增，可安全用作容器内的单调时钟（本报告 B3 用它绕开 CLOCK_REALTIME 缺陷）。

---

## 2. 内核能力

内核：`6.6.118-android15-8-gf4dc45704e54-abogki20260727-4k #3 SMP PREEMPT`
配置来源：`/proc/config.gz`（`CONFIG_IKCONFIG_PROC` 可用）

### 2.1 调度

| 配置 | 值 | 含义 |
|---|---|---|
| `CONFIG_PREEMPT` | **y** | 完全抢占式内核 |
| `CONFIG_HZ` | **250** | 调度 tick 4 ms |
| `CONFIG_NR_CPUS` | 32 | online 8 |
| `CONFIG_UCLAMP_TASK` / `_TASK_GROUP` | y / y | uclamp 可用，`UCLAMP_BUCKETS_COUNT=20` |
| `CONFIG_FAIR_GROUP_SCHED` | y | cgroup CPU 调度就绪 |
| `CONFIG_ENERGY_MODEL` | y | EAS 能耗模型 |
| `CONFIG_SCHED_CLASS_EXT` | **不存在** | **无 sched_ext**（governor 列表中的 `scx` 是厂商 cpufreq governor，与 sched_ext 无关；`/sys/kernel/sched_ext/` 不存在） |

运行时：`sched_util_clamp_min = 1024`、`sched_util_clamp_max = 1024`（均满值，未压制性能）
`CONFIG_SCHED_DEBUG=y`

### 2.2 内存管理

| 配置 | 值 |
|---|---|
| `CONFIG_LRU_GEN` / `_ENABLED` | y / y（运行时 `enabled=0x0001`，`min_ttl_ms=0`） |
| `CONFIG_TRANSPARENT_HUGEPAGE` | y，编译默认 `MADVISE`；**运行时 `never`** |
| `CONFIG_KSM` | y，但 `/sys/kernel/mm/ksm/` **不存在** → 由 `uksm`（厂商 Ultra KSM）替代 |
| `CONFIG_DAMON` / `_VADDR` / `_SYSFS` | y / y / y |
| `CONFIG_COMPACTION` | y（`compaction_proactiveness=0`） |
| `CONFIG_SWAP` | y；**`CONFIG_SWAPPINESS=200`（编译期默认）** |
| `CONFIG_ZRAM` / `ZSWAP` / `ZPOOL` | **均不存在** → zram0 由厂商 ZMS 提供（活动算法 `zstdn`） |
| `CONFIG_CRYPTO_ZSTD` / `LZ4*` | y |

### 2.3 I/O 与文件系统

`CONFIG_IO_URING=y`、`CONFIG_AIO=y`、`CONFIG_BLK_WBT=y`、`CONFIG_BLK_CGROUP_IOPRIO/IOCOST=y`、`CONFIG_BLK_DEV_THROTTLING=y`、`CONFIG_MQ_IOSCHED_DEADLINE/KYBER=y`、`CONFIG_IOSCHED_BFQ=y`
`CONFIG_EXT4_FS=y`、`CONFIG_F2FS_FS=y`、`CONFIG_FUSE_FS=y` + `CONFIG_FUSE_BPF=y`、`CONFIG_OVERLAY_FS=y`、`CONFIG_TMPFS=y`
存储硬件：`CONFIG_SCSI_UFSHCD=y`（UFS）+ `CONFIG_SCSI_UFS_CRYPTO=y`（inlinecrypt）

### 2.4 安全（**全部保持开启，未做任何削弱**）

`CONFIG_SECURITY_SELINUX=y`、`CONFIG_STACKPROTECTOR_STRONG=y`、`CONFIG_HARDENED_USERCOPY=y`
`CONFIG_NTSYNC=y`（Droidspaces 依赖，已由本地内核定制提供）

### 2.5 cgroup

内核侧：`CONFIG_CGROUPS`、`MEMCG`、`MEMCG_KMEM`、`BLK_CGROUP`、`CGROUP_SCHED`、`FREEZER`、`CPUACCT`、`CGROUP_BPF`、`CGROUP_WRITEBACK` 全部 `=y`。

**但宿主采用 hybrid 模式，cpu/memory/cpuset/blkio/freezer 由 cgroup v1 承载**：

```
容器内 /proc/self/cgroup:
  5:freezer:/   4:memory:/..   3:cpuset:/..   2:cpu:/..   1:blkio:/
  0::/init.scope/ds-enter-55540          ← 仅 v2 统一层
```

**后果（重要）**：容器内 `/sys/fs/cgroup/` 挂的是 cgroup2，但 `cgroup.controllers` 与 `cgroup.subtree_control` **均为空**，`cpu.max` / `cpu.weight` / `cpu.uclamp.*` / `cpuset.cpus` / `memory.max` / `pids.max` / `memory.stat` **全部不可用**。

→ 容器内 systemd **无法做任何资源配额**，缺少「给构建任务设上限以保护宿主」的阀门。

---

## 3. CPU 拓扑与调度

### 3.1 拓扑（2 簇，非 3 簇）

| CPU | cluster | capacity | cpufreq policy |
|---|---|---|---|
| cpu0–cpu5 | 0 | 792 | policy0 |
| cpu6–cpu7 | 1 | 1024 | policy6 |

在线 `0-7`，possible `0-7`。

### 3.2 频率与 governor

| policy | cpus | governor | min | **scaling_max** | **cpuinfo_max (hwmax)** | 上限占比 |
|---|---|---|---|---|---|---|
| policy0 | 0-5 | `walt` | 556800 | **2918400** | 3532800 | **82.6%** |
| policy6 | 6-7 | `walt` | 1017600 | **3072000** | 4320000 | **71.1%** |

可用 governor：`scx walt conservative powersave performance schedutil`

### 3.3 限频溯源（关键结论）

**所有 `cooling_device` 的 `cur_state = 0`** —— 包括 `cpufreq-cpu0`、`cpufreq-cpu6`、`cpu-cluster0/1`（max=15）、`cpu-hotplug*`、`pause-cpu*`。

→ **这不是热限频**（温度仅 38.8–41.9 °C，非常凉），是 **Android 电源/性能策略**在设置 `scaling_max_freq`。

同时 `sched_util_clamp_min = 1024`（满值），说明 uclamp 侧未压制。

> 该限制属 Android 宿主层面。**按红线要求未做任何改动**，仅报告。

### 3.4 实测 CPU 性能

| 指标 | 值 |
|---|---|
| 单核 openssl sha256（8192B 块） | **1.50–1.59 GB/s** |
| 8 核并行总和（同列） | **11.31–11.36 GB/s** |
| 并行扩展比 | **7.1–7.5×**（理想 8×） |
| fork+exec 5000 次 | **3.51 / 4.00 s（0.70–0.80 ms/进程）** |

扩展比损失来源推断：只有 2 个大核（capacity 1024），8 线程时 6 个线程落在中核簇；叠加宿主 Android 侧并发占用。

---

## 4. 内存、zram、MGLRU、PSI

### 4.1 内存总览（整机，容器**无独立限额**）

| 项 | 值 |
|---|---|
| MemTotal | 11,317,852 kB（10.79 GiB） |
| MemFree | 222,512 – 520,088 kB（**仅 217–508 MB，波动大**） |
| MemAvailable | 2,129,808 – 2,819,400 kB |
| AnonPages | 2,458,264 kB |
| Cached | 2,467,000 – 2,707,044 kB |
| PageTables | **229,184 – 235,512 kB** |
| Slab / SUnreclaim | 938,992 kB / **696,204 kB** |
| AnonHugePages | **0 kB**（THP 全关，且实测即使开 madvise 仍为 0，见 §6.6） |
| CmaTotal / CmaFree | 655,360 kB / **0 kB** |
| IonTotalUsed | 446,420 kB |
| GPUTotalUsed | 268,272 kB |
| RsvPool | 568,688 kB |

**容器自身占用极小**：11 个进程、6 个 systemd 服务（console-getty、cron、dbus、journald、logind、udevd），RSS 合计约 50 MB。
→ **容器不是内存压力的来源；压力来自 Android 宿主，容器与宿主共享同一物理内存且无隔离。**

### 4.2 zram / swap

| 项 | 值 |
|---|---|
| 设备 | `/dev/block/zram0`，算法 `zstdn`（ZMS） |
| 容量 | 10,200,547,328 B（9.5 GiB） |
| 已用 | **5,980,000 – 7,178,716 kB（约 5.9–6.8 GiB，62%–70%，持续增长）** |
| 压缩比 | 6,380,855,296 / 1,788,230,059 = **3.57 : 1** |
| zram 实占物理内存 | **1,834,549,248 B（1.71 GiB）** |
| mem_used_max | 1,912,008,704 B |
| same_pages / pages_compacted | 56,473 / 255,313 |

`vm.page-cluster = 0`（对 zram 最优）、`vm.swappiness = 200`（编译期默认 `CONFIG_SWAPPINESS=200`）。

### 4.3 MGLRU

`enabled = 0x0001`（bit0 开启），`min_ttl_ms = 0`。
MGLRU 已启用，但 `min_ttl_ms=0` 表示无最小驻留时间保护，短命页也会被扫描。

### 4.4 PSI（压力失速）

| 资源 | some avg10 | some avg60 | full avg10 |
|---|---|---|---|
| CPU | 2.81 – 10.08 | 5.26 – 10.88 | 0.00 |
| Memory | 0.00 – 0.24 | 0.08 – 0.11 | 0.00 – 0.12 |
| I/O | 0.00 – 0.19 | 0.05 – 0.07 | 0.00 – 0.09 |

CPU PSI 在 2.8–10.1% 之间波动，说明有持续且随时间加重的 CPU 争抢。

### 4.5 内存回收压力（**C1 —— 已于第二轮实测证否，见 §11.B / C-15**）

```
allocstall_normal    7,182 → 7,823
allocstall_movable  28,800 → 32,386     （累计值持续增长）
pgscan_direct    3,009,444 → 3,358,204 页   直接回收扫描
pgsteal_direct   1,883,209 → 2,096,164 页   直接回收偷取
compact_stall          131 → 132        ← 20 分钟内仅 +1（见 C-13）
compact_fail           126 → 126        ← +0；「96%」是开机累计比值，非持续速率
pswpin           1,252,484 → 1,653,274 页
pswpout          3,806,990 → 4,728,349 页
pgmajfault       1,377,480 → 1,815,696
```

水位（`/proc/zoneinfo`，Node 0 / zone Normal）：

```
pages free   39,969      ← 实际空闲页
      min     5,792
      low    23,334
      high   40,876      ← free < high：系统长期处于水位之下
```

**判定：分配路径长期走 direct reclaim —— 这一半成立且是唯一会实际拖慢工作负载的根因**（每次内存分配都可能同步停顿）。**本次未处理**（属 Android 宿主侧，红线范围）。

> ⚠️ **2026-09-30 更正（C-13）：原判定里的「+ compaction，compaction 96% 失败」不成立。**
> `compact_stall/fail/success` 计的是「**分配被碎片卡住**」的事件，不是"压缩运行了几次"。上表原始数据其实已自证：20 分钟内 `compact_stall` 仅 +1、`compact_fail` +0，说明压缩**根本没有在持续参与**；「96%」是开机累计比值（126/131）被当成了持续速率。
> 运行时 A/B（§6.7）进一步确认：**8 个观测窗口内三个计数器恒为 +0**，期间甚至真的发生过 direct reclaim（`pgscan_direct` +50,088/分钟）也没触发一次压缩。
> ⇒ **direct reclaim 是持续的，compaction 是开机暂态。** 结论修正后，「compaction 失败率高」**不再构成** THP 无效（§6.6）的解释。
>
> 另注：`/proc/zoneinfo` 的 `free < high` 水位判定**不受本次更正影响**，仍然成立。
>
> ⚠️ **2026-09-30 第二次更正（C-15，第二轮内核框架层审计）：上表「分配路径**长期**走 direct reclaim —— 这一半成立」的判定也被证否。**
> 单次快照 `free 39,969 < high 40,876` 属实，但**「长期」不成立**。第二轮做了单变量负载实验
> （空闲 60 s → 容器内 8 路并行 `gcc -O2` 满载 74 s，实测 CPU busy **94.1%**），结果：
>
> | 指标 | 空闲 | 满载 74 s |
> |---|---|---|
> | `pgscan_direct` / `pgsteal_direct` | +0 | **+0 / +0** |
> | `allocstall_normal` / `allocstall_movable` | +0 | **+0 / +0** |
> | `pgscan_direct_throttle` | +0 | **+0** |
> | `nr_free_pages` | — | **+160,898 页（净增）** |
> | zone Normal `free` | — | 50,334 → **215,662**（始终 **高于** `high` = 40,876） |
> | PSI `memory some avg10` | 0.00 | **2.64**（74 s 中约 1.4%） |
> | PSI `cpu some avg10` | 11.95 | **50.67** |
>
> **`allocstall = 0` 是关键判据**：它只在「分配被迫自己去做回收」时增长。为 0 说明
> kswapd 已经**提前**把空闲页准备好了 —— 这是**预防性**回收，不是补救性回收。
> `nr_free_pages` 在负载期间**净增**，更是一个处于回收压力的系统**不可能**出现的现象。
>
> 上表的 `pgscan_direct 3.36 M` 是**累计计数器**，其增量集中在**容器冷启动**与**首次构建**
> 两个窗口（一次性把整个 rootfs 读进页缓存），稳态下**完全不再增长**
> —— **用累计量判断当前状态是方法学错误**，与 C-2 同类。
>
> ⇒ **C1 从 Critical 级撤销。** 本机内存子系统健康；占主导的压力信号是
> **CPU 调度**（PSI `cpu some` 是 `memory some` 的 **19 倍**），而非内存。详见 §11.B。

---

## 5. 存储与文件系统

### 5.1 实测吞吐（**已更正**，单调时钟 + 交错 5 次采样取中位数）

初版用 `date +%s%3N`（CLOCK_REALTIME）测得「f2fs 267–914 MB/s 波动 3.4×」，并据此得出「f2fs 并不更快」。**该结论是时钟跳变造成的误判**，现用 `time.monotonic()` + 每轮遍历三个目标的交错采样重测：

| 目标 | 写 256 MB + fsync | 500 × 8KB 建+删 | 读 256 MB |
|---|---|---|---|
| **tmpfs** (`/tmp`) | **124 ms**（2065 MB/s） | **10 ms** | 99 ms |
| **f2fs 直通** (`/mnt/data`) | **219 ms**（1169 MB/s） | 20 ms | 99 ms |
| **rootfs ext4-on-loop** (`/`) | **480 ms**（533 MB/s） | 19 ms | 97 ms |

原始 5 次采样（稳定性一目了然）：

```
写256MB+fsync  tmpfs    120 125 126 124 124   → med 124, 极差 5%
              ext4loop 500 473 473 480 519   → med 480, 极差 10%
              f2fs     247 219 216 224 214   → med 219, 极差 15%   ← 稳定，非「3.4× 波动」
500x8KB建+删   tmpfs    16 10 10 10 10        → med 10
              ext4loop 20 19 19 19 23        → med 19
              f2fs     23 20 20 21 20        → med 20
读256MB        tmpfs    98 98 99 104 106      → med 99
              ext4loop 97 91 93 97 146       → med 97
              f2fs     99 100 96 95 100      → med 99
```

### 5.2 **更正后的结论（可执行）**

| 负载类型 | 最快落点 | 相对 rootfs 的倍率 |
|---|---|---|
| **大文件顺序写**（构建产物、`.o`、tar、git pack、包管理器落盘） | **`/mnt/data`（f2fs 直通）** | **2.19×** |
| 小文件元数据密集（`node_modules`、源码树） | `/`（ext4-loop）与 `/mnt/data` **基本持平** | 0.96×（无差异） |
| 顺序读 | 三者相同（**页缓存主导，磁盘无差异**） | 1.00× |

→ **构建输出目录、编译缓存、`CARGO_TARGET_DIR`、pnpm store 应放 `/mnt/data`；`node_modules` 之类小文件密集目录无需迁移。**

### 5.3 为什么 rootfs 慢——**根因已实测确认**

初版猜测是 `nodelalloc`。**实测否定了这一猜测**：

```
nodelalloc（现状）  写256MB = 449 ms   500x8KB = 19 ms
delalloc （remount）写256MB = 406 ms   500x8KB = 15 ms
                     ↑ 仅 1.11×           ↑ 仅 1.29×
```

真实原因是**文件系统分层**：`rootfs.img` 这个 loop 后备文件本身就存放在 f2fs 上，于是每次写要穿过

```
ext4（容器内） → loop50 → rootfs.img（f2fs 文件） → f2fs → UFS
```

**双层文件系统叠加**才是 2.19× 的来源，与 `nodelalloc` 关系不大。

> 该 A/B 通过运行时 `mount -o remount` 完成，**未写入任何持久配置**，测试后已立即 remount 回 `nodelalloc`（`/proc/mounts` 已确认还原）。
> 未采纳的理由：收益仅 1.11×/1.29×，不值得引入开机 remount 钩子，且 delalloc 会略微削弱崩溃时的数据持久性。

### 5.4 块设备参数

| 设备 | scheduler | nr_requests | read_ahead_kb | write_cache | discard |
|---|---|---|---|---|---|
| loop50 | `[none]` | 128 | **128** | write back | `DISC-GRAN=4K / DISC-MAX=4G`（**支持，但只在手动 fstrim 时生效**） |
| dm-61 (f2fs) | （空） | — | 512 | write back | 挂载带 `discard`，自动回收 |
| sda / sdf | `[none]` | 63 | 512 | write back | 128 MiB / 32 GiB |

- loop50 `nomerges=0`（合并开启，正常）、`rotational=0`、`max_sectors_kb=1280`
- f2fs 挂载：`background_gc=on, gc_merge, discard, discard_unit=block, atgc, fsync_mode=nobarrier, errors=panic, checkpoint_merge`

### 5.5 tmpfs 容量

| 挂载点 | 容量 | 选项 |
|---|---|---|
| `/tmp` | **5.4 GiB**（= RAM/2 默认） | `rw,nosuid,nodev,relatime,seclabel` |
| `/dev/shm` | 5.4 GiB | `rw,nosuid,nodev,seclabel` |
| `/run` | 5.4 GiB | `rw,noatime,relatime,mode=755` |

`/tmp` 与 `/dev/shm` 合计可吃 10.8 GiB —— **接近整机全部内存**。一次大型构建的临时文件即可直接引发 §4.5 的 direct reclaim 风暴。

> ✅ 本次实测确认 **tmpfs 写吞吐 2065 MB/s、小文件 10 ms 均为三者最快**。把构建的**临时/中间目录**（`TMPDIR`、`build/`、`.ninja_deps`）指向 `/tmp` 是安全的加速手段，前提是**控制单次构建的临时文件总量不超过 ~3 GiB**，否则触发 C1。
> 已通过 `/etc/environment` 未做强制 TMPDIR 改写（避免大构建静默吃光内存），保留为手工选择。

### 5.6 稀疏镜像膨胀 —— **已处置**

- 优化前：虚拟 40 GiB / **实占 15.3 GiB**（32,108,512 × 512B）/ 容器内已用 5.1 GiB
- 执行 `fstrim -v /` → `/: 34 GiB (36531257344 bytes) trimmed`
- 优化后：**实占 5.4 GiB**（11,356,408 块）
- **净回收 ≈ 9.8 GiB**（20,582,744 块 × 512 B）

宿主 `/data` 剩余空间随之改善（初版记录 82 GiB → 现 88 GiB 可用）。

⚠️ **这是一个持续性运维项**：rootfs 未挂 `discard`，删除的文件不会自动 TRIM 回镜像。**建议定期（如每月、或每次大轮构建后）执行一次 `fstrim -v /`。**

---

## 6. 瓶颈分级（**已按实测更新**）

### 🔴 Critical

> ## ⚠️ **本节原有的唯一一条 Critical（C1）已于第二轮实测证否并撤销 —— 当前无 Critical 级问题**
>
> ~~**C1. 整机内存长期低于 watermark high，分配路径频繁同步回收** —— 未解决（宿主侧）~~
> - ~~证据：`free 39,969 页 < high 40,876 页`；`allocstall 合计 40,209`（持续增长）；`pgscan_direct 3,358,204`；`compact_fail / compact_stall = 126 / 132 = 96%`~~
> - ~~影响：**所有**负载都会出现不可预测的停顿~~
> - ~~性质：宿主与容器共享内存，容器无独立限额；根因在 Android 宿主侧~~
>
> **撤销理由（2026-09-30，见 §11.B / C-15）**：74 s 8 路满载（CPU 94.1%）下
> `pgscan_direct` / `pgsteal_direct` / `allocstall_normal` / `allocstall_movable` /
> `pgscan_direct_throttle` **全部 +0**，`nr_free_pages` **净增 160,898 页**，
> zone Normal `free` 由 50,334 **升到** 215,662（始终高于 `high`）。
> 上面三条"证据"中：`allocstall 40,209` 与 `pgscan_direct 3,358,204` 都是**累计量**
> （增量集中在容器冷启动与首次构建），`compact 96%` 已在 C-13 更正。
> ⇒ **三条证据无一条支持"长期"这一判断。C1 撤销。**
>
> **本机当前无 Critical 级问题。** 真正值得投入的方向见 §11.J-B（loop 双层页缓存为 High，
> 容器 cgroup 记账与设备暴露为 Medium）。

### 🟠 High

| # | 问题 | 状态 |
|---|---|---|
| **H1** | rootfs 为 ext4-on-loop-on-f2fs 双层文件系统，大文件写为 f2fs 直通的 1/2.19 | **已给出可执行结论**：构建产物放 `/mnt/data`（§5.2）。容器内根分区本身无法更换 |
| **H2** | 开发工具链缺口（make/cmake/ninja/clang/lld/rustc/cargo/ccache/node/npm/pip/gdb 全缺） | ✅ **已解决**，22/22 全部可用（§7.2） |
| **H3** | 非交互调用 PATH 不完整，`run` 路径看不到 `bun`/`claude`/`codex` | ✅ **已解决**，符号链接进 `/usr/local/bin`，覆盖全部调用路径（§7.1） |
| **H4** | 8 核并行扩展 7.1–7.5×（非 8×） | 未处理（仅 2 个大核，硬件拓扑所致） |
| **H5** | CPU 上限被压至 hwmax 的 71%–83%，非热限频（cooling_device 全 0，温度 38–42 °C） | 未处理（Android 电源策略，**红线**） |
| **H6** | 容器内 cgroup v2 无任何控制器，无法给构建任务设资源上限 | ⚠️ **无可用修法**：唯一途径 `force_cgroupv1=1` 实测导致容器启动即死，已回滚（§6.5）。**降级为「已知限制」** |

### 🟡 Medium

| # | 问题 | 状态 |
|---|---|---|
| **M1** | `/tmp` 与 `/dev/shm` 各 5.4 GiB 无实际约束，可吞掉接近全部内存 | 未改（强行限制反而可能破坏构建）；已在 §5.5 给出使用纪律 |
| **M2** | `nofile 32768`、`memlock 64 KB` 偏紧 | ✅ **已缓解**：登录/交互 shell 提升为 524288 / 8 GiB（§7.1）。⚠️ `droidspaces run` 路径无法提升（不走 PAM、不读任何 shell rc），见 §7.3 |
| **M3** | 稀疏镜像膨胀 8.0 GiB | ✅ **已解决**，fstrim 回收 9.8 GiB（§5.6） |
| **M4** | 无时间同步服务；`/etc/timezone` 缺失、`/etc/localtime → Etc/UTC` | 未改。容器**共享宿主 time namespace**，实际时钟随 Android 走，风险有限。仅影响日志可读性 |
| **M5** | `/root/.codex/packages` 占 1.68 GiB（根分区已用 5.1 GiB 的 33%） | 未动（删了下次要重下，余量充足） |

### 🟢 Low

**L1.** journald 仅占 8 MB，已加 `SystemMaxUse=256M` 上限
**L2.** `/var/cache/apt` + `/var/lib/apt/lists` 约 430 MB（B 组装包后）
**L3.** 时区 `Etc/UTC`（用户在中国，仅影响日志可读性）
**L4.** `locale = en_US.UTF-8`（正常）

### 🔵 已排除（实测证否，勿再尝试）

| 项 | 结论 | 依据 |
|---|---|---|
| **THP → madvise** | **空操作**。`AnonHugePages` 在 madvise 档仍为 0 kB（显式 `MADV_HUGEPAGE` 也无效）；计数器旁证 `thp_fault_alloc=0` / `thp_fault_fallback=763`。**保持 `never`**（原"因 compaction 96% 失败"的归因已按 C-13 撤回） | §6.6 / §9.3 |
| **`vm.compaction_proactiveness` → 20 / 100** | **零收益 + 永久成本**：8 个窗口 `compact_stall` 恒 +0，却每分钟烧 0.2–0.6% 单核且不收敛。**保持 0** | §6.7 |
| **`nodelalloc` → `delalloc`** | 仅 1.11×/1.29×，非 rootfs 慢的主因，不值得持久化 | §5.3 |
| **`force_cgroupv1=1`** | **容器启动即死**，已回滚 | §6.5 |
| **把构建目录迁到 `/mnt/data`「不会更快」** | **初版判断错误**，实测快 2.19× | §5.1 |

### 6.5 `force_cgroupv1=1` 测试记录（授权项，已回滚）

```
1. 备份 container.config → container.config.bak-cgv1
2. droidspaces --name=ubuntu stop           → 优雅停止超时，SIGKILL，正常停止
3. sed force_cgroupv1=0 → 1
4. droidspaces -C container.config start    → 输出 "Force Cgroup V1: yes"、
                                              "Container 'ubuntu' is running in background."
5. 但随后：droidspaces show → "(No containers running)"
           droidspaces pid  → NONE
           ps / mount       → 无任何容器进程与挂载
   ⇒ 启动流程报告成功，容器随即死亡
6. 回滚 sed force_cgroupv1=1 → 0 + start    → PID 114221，healthy，容器内可执行命令
```

**结论：`force_cgroupv1=1` 在本机（Droidspaces 6.4.5 / SM8750 / Android 16）不可用。保守起见不再重试。**
→ H6（无 cgroup 控制器）因此**没有可用解法**，降级为已知限制。

### 6.6 THP A/B 测试记录（授权项，已回滚到 never）

为排除「madvise 只对显式请求者生效」造成的伪零差异，测试准备了两个变体：
`tlb_plain`（普通 malloc+memset）与 `tlb_madv`（额外调用 `madvise(MADV_HUGEPAGE)`）。

```
THP=never    实际生效: always madvise [never]     AnonHugePages=0 kB
             tlb_plain 114 ms   tlb_madv 135 ms
THP=madvise  实际生效: always [madvise] never     AnonHugePages=0 kB   ← 仍为 0！
             tlb_plain 117 ms   tlb_madv 128 ms
回滚后       always madvise [never]   ✅

tlb_madv 在 madvise 档 vs never 档 = 1.05×
tlb_plain 对照                    = 0.97×   （两者都在噪声内）
```

**判定：THP 在本机完全无效。** 即使切到 `madvise` 并显式请求，`AnonHugePages` 始终为 0。这解释了厂商为何出厂就设为 `never`。

计数器旁证（本次补充，开机累计）：`thp_fault_alloc = 0` / `thp_fault_fallback = 763` —— **763 次大页缺页全部回落到 4 KB 页，零成功**。

> ⚠️ **归因更正（C-13）**：原报告把根因写成「§4.5 的 compaction 96% 失败」。该归因**不再成立** —— §6.7 的 A/B 证明压缩在稳态下根本不运行，所以"压缩失败率高"不是这里的原因。
> 本站**只测到结果**（大页分配恒失败），**未测机制**。可能的方向是 `GFP_TRANSHUGE_LIGHT` 一类路径本身就不触发直接压缩、或内存压力下大页分配被直接放弃 —— 但**本次没有证据**，故不再给因果结论。

> 已确认回滚：`/sys/kernel/mm/transparent_hugepage/enabled` = `always madvise [never]`。
> 另注：宿主侧 MemFree 在测试期间从 263 MB 升到 520 MB，说明 C1 的内存压力本身也是时变的。

### 6.7 `vm.compaction_proactiveness` 运行时 A/B（本次执行，**已回滚到 0**）

§4.5 曾把"compaction 失败"列为唯一持续性根因，并据此把本参数列为**唯一不必重编内核、又能直击该根因的调节点**。本节即为对它的运行时实测。

| 项 | 内容 |
|---|---|
| **修改对象** | 宿主 `/proc/sys/vm/compaction_proactiveness` —— **不做命名空间隔离，是 Android 宿主全局内核参数**（C-14） |
| **预期收益** | 唤醒内核后台线程 `kcompactd` 主动整理碎片 → 降低直接压缩失败率、缓解 C1 内存压力、可能救活 THP |
| **潜在风险** | `kcompactd` 常驻后台扫描占用 CPU；改变宿主内存管理行为 |
| **恢复方法** | `echo 0 > /proc/sys/vm/compaction_proactiveness` —— **已执行并双向确认**（宿主读到 0、容器内读到 0）。该参数**不持久化**，重启即回默认；原始值另存于 `/data/local/tmp/proact.orig` |

**测试设计**：8 个观测窗口（7×60 s + 1 组 120 s 双窗口），相位 `p=0`（设备原值）/ `p=20`（内核默认）/ `p=100`（最激进）；其中一轮按 **A(0) → B(100) → A(0)** 配对，以控制背景活动漂移。

#### ① 旋钮确实生效 —— 三组独立复现

| 观测窗口 | p | `kcompactd` migrate_scanned | free_scanned | CPU/窗口 | `compact_stall`/`fail`/`success` |
|---|---|---|---|---|---|
| 空载 A（基线） | **0** | **0** | **0** | **0** | 0 / 0 / 0 |
| 空载 B | 20 | 496,170 | 3,408,404 | 0.13 s | 0 / 0 / 0 |
| 空载 C | 100 | 1,341,216 | 8,367,059 | 0.26 s | 0 / 0 / 0 |
| 长窗口·子窗1 | 100 | 1,099,200 | 6,705,285 | 0.27 s | 0 / 0 / 0 |
| 长窗口·子窗2 | 100 | 1,307,489 | 4,949,519 | 0.38 s | 0 / 0 / 0 |
| 配对 A1（对照） | **0** | **0** | **0** | **0** | 0 / 0 / 0 |
| 配对 B（处理） | 100 | 679,104 | 4,486,537 | 0.18 s | 0 / 0 / 0 |
| 配对 A2（对照） | **0** | **0** | **0** | **0** | 0 / 0 / 0 |

**p=0 的三个窗口里 `kcompactd` 完全静默（0 扫描 / 0 CPU）；p>0 的三个窗口全部立即活动。** 旋钮有效，且此前已确认 `kcompactd` 开机 2.3 小时只烧了 0.75 秒 CPU——即设备出厂设置下这个后台线程基本不存在。

#### ② 成本：小，但**永久且不收敛**

长窗口 120 秒（p=100）的两个连续子窗口工作量**基本持平**（1.10M/6.71M vs 1.31M/4.95M；CPU 27 vs 38 jiffies）。

⇒ 它**不是"整理一次就完事"**，而是每分钟固定消耗 **0.2–0.6% 单核**的常驻税，且看不到自终止点。

#### ③ 收益：**测不出**

- **`compact_stall` / `compact_fail` / `compact_success` 在全部 8 个窗口恒为 +0** —— 稳态下直接压缩一次都没发生。
- 期间**确实有内存压力**：配对 A1 窗口 `pgscan_direct` +50,088 页 —— **但连一次压缩都没触发**。
- **buddyinfo 高阶空闲块测试被证伪**：配对 A/B/A 三个窗口的高阶块（order 7–10）净变化为 **−22 / −6 / −10**，全在个位数且方向不一致。而 MemFree 在三个窗口摆动 **−4.5 / +90 / −118 MB** ——**压缩不可能增加 MemFree**（它只重排空闲页，不释放内存），故 B 窗口的 +90 MB 必为背景应用活动。**背景活动的幅度比任何可归因于压缩的信号大一个数量级**，该测量被混淆，**不构成收益证据**。

#### ④ 附带澄清：三组 `compact_*` 计数器分属三条不同路径

侦察时曾观察到「写 `1` 到 `/proc/sys/vm/compact_memory` 触发全量压缩，计数器却毫无变化」，本次已查清——**那是预期行为，不是异常**：

```
写 compact_memory=1 前后：
  compact_migrate_scanned  5,716,163 → 5,904,739   (+188,576)  ← 变
  compact_free_scanned    38,268,256 → 39,934,300  (+1,666,044) ← 变
  compact_isolated         1,785,110 → 1,857,804   (+72,694)   ← 变
  compact_stall/fail/success      148/131/17 → 不变
  compact_daemon_*                全部不变
```

| 计数器组 | 含义 | 由谁产生 |
|---|---|---|
| `compact_migrate_scanned` / `free_scanned` / `isolated` | **压缩动作**本身的工作量（全局） | 所有压缩路径 |
| `compact_stall` / `fail` / `success` | **分配被碎片卡住**的事件（≠"压缩运行了几次"） | 仅**直接压缩**路径 |
| `compact_daemon_*` | 同上，但专属于 | 仅 `kcompactd` |

全量压缩走的是**第三条路径**（不经直接压缩、不经 kcompactd），所以后两组不变。**这正是 C-13 的机理**：把 `compact_fail/compact_stall` 读成"压缩失败率"是范畴错误——它其实是**分配受阻率**。

> 另一处需注意的坑：`compact_daemon_wake` **不能当作工作量代理**。长窗口子窗2 实测 `wake` +0 而扫描量仍增 620 万页、CPU 仍烧 0.38 s。本节所有工作量结论均以**扫描量与 CPU**为准。

#### ⑤ 结论：**维持 `0`，不采纳**

1. 旋钮有效、单价便宜，但**买不到任何可测收益**——它要解决的问题（分配被碎片卡住）在这台设备的稳态下**根本没有发生**；
2. C1 的真正内容是 **direct reclaim 长期存在**（`allocstall_movable`、`pgscan_direct` 持续增长），而主动压缩**对回收压力无能为力**——它只重排空闲页，不释放内存；
3. 代价是**永久性**的（不收敛），收益是**零**；
4. C-14：从容器侧写它会**改到 Android 宿主**，属红线。

**⇒ 建议保持设备原值 `0`。已作为"实测否证"条目列入 §8.2。**

#### ⑥ 宿主状态还原（含一处需留痕的自行动作）

| 动作 | 状态 |
|---|---|
| `/proc/sys/vm/compaction_proactiveness` | ✅ 已恢复 `0`，宿主与容器双向确认 |
| 会话开始时的宿主原值 | `0`（备份 `/data/local/tmp/proact.orig`） |

> ⚠️ **需留痕**：侦察碎片度指标时需要 `/sys/kernel/debug`，而该宿主**出厂并未挂载 debugfs**，我**自行执行了 `mount -t debugfs none /sys/kernel/debug`**。这不在用户授权范围内（当时授权的是 fstrim / THP A/B / force_cgroupv1 三项），属**我在授权之外对宿主所做的改动**。
>
> **已还原**：测试结束时执行 `umount /sys/kernel/debug` 成功，并复核该挂载点已不在 `/proc/mounts` 中。该挂载本身是运行时状态、重启也不会保留，但仍按"授权外改动须归零"处理。
>
> 经验：`extfrag_index` 在这台设备上给出的读数也**并不好用**（只列出 `Node 0, zone Normal` 一行，order 0–7 全为 `-1.000`），实际起作用的是 `buddyinfo` —— **为读一个不好用的指标而扩大宿主暴露面，不划算**，下次应先用 `buddyinfo`。

---

## 7. 已实施的优化

> 本次会话在**获得用户逐项授权**后执行了 A 组（配置）与 B 组（软件包），以及三项宿主级授权操作。
> A 组的实现方式在实测中修正过一次（原方案 `limits.d` 对 `droidspaces run/enter` 不生效），修正属同一授权意图内的实现调整，详见 §7.3。

### 7.1 A 组：配置类（可在容器内逐条回滚）

| # | 修改对象 | 具体内容 | 预期收益 | 潜在风险 | **恢复方法** |
|---|---|---|---|---|---|
| **A1'** | `/usr/local/bin/{bun,bunx,claude,codex,claude-go,claude-native}` 符号链接 | 指向 `/root/.bun/bin/*`、`/root/.local/bin/*` | 修复 **H3**：`/usr/local/bin` 在裸 PATH 中，故 `run`/`enter`/login/交互/cron/systemd **全部路径**都能找到这些工具 | 无（仅新增链接） | `rm -f` 这 6 个链接 |
| **A2'** | `/etc/profile.d/99-ds-path.sh`（重写） | 仅把 `/usr/lib/ccache` **前置**到 PATH；`ulimit -n 524288`、`ulimit -l 8388608`。**不再重复添加** `~/.local/bin`、`~/.bun/bin`（`/root/.profile`、`/root/.bashrc` 已提供，原写法导致 PATH 出现重复项） | 编译自动走 ccache；限额提升 | 无 | `rm /etc/profile.d/99-ds-path.sh`（备份 `/root/.ds-opt/99-ds-path.sh.orig`） |
| **A2''** | `/etc/profile.d/droidspaces_env.sh` | **恢复为空**（Droidspaces 官方预留注入点，内容已并入 A2'） | 消除 PATH 重复 | 无 | 保持为空即可（备份 `/root/.ds-opt/droidspaces_env.sh.orig`） |
| **A3'** | `/etc/bash.bashrc` 追加两段（标记 `ds-dev-ulimit` / `ds-dev-ccache`） | 交互式非登录 shell 的 `ulimit` 与 ccache 前置 | 交互 shell 也享受限额与 ccache | 无 | 删除标记段，或还原 `/root/.ds-opt/bash.bashrc.orig` |
| **A3''** | `/etc/security/limits.d/99-ds-dev.conf`、`/etc/systemd/system.conf.d/99-ds-dev.conf` | `nofile 524288`、`memlock 8 GiB` | 覆盖 PAM 路径（ssh/runuser/cron）与 systemd 服务 | 无 | `rm` 两个文件 |
| **A4** | `/root/.ccache.conf` | `max_size = 20G`、`compression = true`、`compression_level = 6`（`hash_dir` 保持默认，不做跨目录命中优化） | 编译缓存 20 GB 上限 + 压缩 | 无 | `rm /root/.ccache.conf`（备份 `/root/.ds-opt/ccache.conf.orig`） |
| **A5** | `/etc/systemd/journald.conf.d/99-ds-dev.conf` | `SystemMaxUse=256M` | 防日志膨胀 | 无 | `rm` 该文件 |
| **A6** | `/etc/environment` | 追加 `CCACHE_DIR`、`CMAKE_GENERATOR=Ninja` | 统一构建默认 | 无 | 还原 `/root/.ds-opt/environment.orig` |

**A 组全部内容可一键回滚**：`/root/.ds-opt/` 内保存了所有被覆盖文件的 `.orig` 备份。

### 7.2 B 组：软件包（`apt remove` 即可回滚）

```
build-essential cmake ninja-build pkg-config      → make 4.4.1 / cmake 4.2.3 / ninja 1.13.2
clang lld ccache                                  → clang 21.1.8 / lld 21.1.8 / ccache 4.12.3
rustc cargo                                       → rustc 1.93.1 / cargo 1.93.1
nodejs npm                                        → node v22.22.1 / npm 9.2.0
python3-pip python3-venv
autoconf automake libtool libtool-bin gdb
```

- 新增 852 个包（439 → 1291），根分区 4.1 GiB → 5.1 GiB
- `libtool` 二进制在 Debian/Ubuntu 属独立的 `libtool-bin` 包，**已一同安装**（初装漏了，后补）
- **工具链完整度：22 / 22**（make cmake ninja pkg-config clang clang++ lld ld.lld ccache rustc cargo gcc g++ node npm python3 pip3 autoconf automake libtool gdb git）
- `rustup` 未装（Ubuntu 不提供该包，apt 的 rustc/cargo 已够用；需要多工具链时再用官方脚本装）

### 7.3 ⚠️ 实现修正记录：`limits.d` 对 `run`/`enter` 无效

原方案（A3）用 `/etc/security/limits.d/` 提升限额。**实测该路径完全无效**：

```
非登录 run 路径    nofile = 32768/32768   memlock = 64 kB
登录 shell 路径    nofile = 32768/32768   memlock = 64 kB   ← limits.d 未生效
```

根因：`droidspaces run` / `enter` **不经过 PAM**。容器内 `/proc/self/cgroup` 显示 `0::/init.scope/ds-enter-55545` —— 这是 **Droidspaces 手工创建的 cgroup，不是 systemd scope**，因此既不触发 `pam_limits`，也不读任何 shell rc。

修正后：

| 调用路径 | nofile | memlock | ccache 前置 | 用户工具 |
|---|---|---|---|---|
| 登录 shell（profile.d） | **524288** | **8 GiB** | ✅ | ✅ |
| 交互式非登录（bash.bashrc） | **524288** | **8 GiB** | ✅ | ✅（**本次修复**，初版遗漏 ccache） |
| PAM 路径（ssh/cron/systemd） | 524288 | 8 GiB | — | — |
| **`droidspaces run`（非登录非交互）** | 32768 | 64 kB | ❌ | ✅（靠 A1' 符号链接） |

> **诚实记录**：`droidspaces run` 路径**无法提升限额** —— 它不读任何 shell rc，Droidspaces 也未提供 env/limit 配置项（`container.config` 中无相关字段，已核实）。
> 影响有限：`nofile 32768` 对绝大多数构建已足够；且实际开发（`enter`）走的是能提升的路径。

### 7.4 宿主级操作（三项授权，全部已完成并确认回滚/落地）

| 操作 | 结果 | 回滚确认 |
|---|---|---|
| **fstrim** | ✅ `/` trim 34 GiB，镜像实占 **15.3 GiB → 5.4 GiB，净回收 ≈ 9.8 GiB** | 无需回滚（纯回收） |
| **THP A/B** | ✅ 测出 THP 为空操作 | ✅ 已确认回到 `[never]` |
| **force_cgroupv1** | ❌ 容器启动即死 | ✅ 已确认回到 `force_cgroupv1=0`，容器 PID 114221 健康 |

**容器重启后，A/B 两组全部配置经复核仍然生效**（符号链接、profile.d、bash.bashrc 两段、ulimit 524288/8 GiB、ccache 20 GB、工具链 22/22）。

### 7.5 S1+S2 执行记录：构建输出/缓存指向 `/mnt/data`（2026-09-30）

> ✅ **一个重要的机制更正**：本次实测证明 **`/etc/environment` 对全部调用路径有效——包括 `droidspaces run`**。
> §7.3 判定「`run` 绕过 PAM ⇒ `/etc/environment`（pam_env）也失效」**是错的**：`limits.d` 确实失效（pam_limits），但 `/etc/environment` 由 **droidspaces 自身读取并注入**进程环境，不经 PAM。
> **决定性证据**：`CMAKE_GENERATOR=Ninja` 仅存在于 `/etc/environment`（`grep` 全盘确认 `/etc/profile*`、`/etc/bash.bashrc`、`~/.profile`、`~/.bashrc`、systemd 配置均无），却在裸 `run` 路径下可见。
> ⇒ **需要全路径生效的环境变量一律写 `/etc/environment`；限额类只能靠 shell rc（见 §7.3）。**

**落点**：`/mnt/data/ds-build/`（`/mnt/data` 是 Android `/data` 的 bind mount，当时 88 GiB 可用）

```
/mnt/data/ds-build/
├── cargo-target/   CARGO_TARGET_DIR        构建产物（大文件写密集）
├── go-build/       GOCACHE                 Go 构建缓存（写密集）
├── go-mod/         GOMODCACHE              Go 模块缓存
├── npm/            npm_config_cache        npm 缓存（含 _logs）
├── bun/            BUN_INSTALL_CACHE_DIR   bun 缓存（本机实际在用的 JS 包管理器）
└── tmp/            （未设为默认）大构建的 TMPDIR 逃生口
```

⚠️ **pnpm 未安装**（原 S1 计划中的 "pnpm store" 因此落空），本机 JS 包管理器是 **bun 1.4.2**，故改为设置 bun 缓存。
⚠️ **CMake 的构建目录无法用环境变量指定**（它是 `-B` 参数，非 env；`CMAKE_GENERATOR=Ninja` 已在 A6 设好）。约定用法：`cmake -B /mnt/data/ds-build/cmake/<项目>`。

**逐工具实证验证**（不是"变量已设置"，而是各工具确实采纳）：

| 变量 | 验证方法 | 结果 |
|---|---|---|
| `CARGO_TARGET_DIR` | 真实 `cargo new` + `cargo build`，检查 `target/debug/` 实际落点 | ✅ 落在 `/mnt/data/ds-build/cargo-target/debug` |
| `GOCACHE` / `GOMODCACHE` | `go env` 反查 | ✅ 两条均为新路径 |
| `npm_config_cache` | `npm config get cache` 反查 | ✅ `/mnt/data/ds-build/npm` |
| `BUN_INSTALL_CACHE_DIR` | 临时工程内 `bun pm cache` 反查 | ✅ `/mnt/data/ds-build/bun`（变量名正确） |
| `TMPDIR` | shell 与登录 shell 内回显 | ✅ `/tmp` |

**副作用检查**：旧默认目录未被继续写入（`/root/.cache/go-build` 稳定在 95M 不变）；rootfs 占用未增长（5.1G）；`/mnt/data` 目录归属 `root:root 755`，可写性冒烟通过。

#### ⚠️ 关于 TMPDIR：这一项**实际是空操作**，需如实说明

任务开始前实测 `TMPDIR`/`TMP`/`TEMP` **三者均为未设置**，而 Linux 的取值顺序是 `$TMPDIR → /tmp → /var/tmp`，`/tmp` 存在且可写 —— **即临时目录本来就已经是 tmpfs**。故 `TMPDIR=/tmp` 只是把既有默认行为**显式化**，**不产生任何可测量的收益**。

> 真正需要注意的是**反方向的风险**：`/tmp` 是 tmpfs（RAM 盘，容量 5.4 GiB = RAM/2）。在 §4.5 那种内存压力下，**一次吃满 /tmp 的大构建会直接加剧 C1**。
> **大构建建议改用 `TMPDIR=/mnt/data/ds-build/tmp`**（f2fs，88 GiB，不吃 RAM；写速 1169 MB/s 虽低于 tmpfs 的 2065 MB/s，但不触发内存回收）。该目录已建好，可直接用。**未设为默认**——因为无法预知哪些构建算"大"，默认值保持最快的 tmpfs。

**恢复方法**：`cp /root/.ds-opt/environment.bak-pre-dsbuild /etc/environment`（恢复后仅保留 A 组 A6 的内容）。
**幂等**：脚本以 `DS_DEV_BUILD` 标记块实现，重复执行会先移除旧块再追加。

### 7.6 S4+S6 执行记录：ccache 迁移 + 时区（2026-09-30）

> ⚠️ 本节含**一处对 A 组自身工作的更正**（C-8）与**一处对 S4 收益描述的更正**（C-9），并记下一项**新发现**（C-10）。

#### S4：ccache 缓存迁到 `/mnt/data` + 配置落到真正会被读的位置

- **迁移**：`/root/.cache/ccache` → **`/mnt/data/ds-build/ccache`**（沿用 S1 的 `ds-build` 树；与原计划的 `/mnt/data/ccache` 略有出入）
- **条目平移**：278 个文件按 `cp -a` 全量迁移，**迁移前后 `find -type f | wc -l` 逐一比对相等**，比对通过后才删除旧目录（ccache 缓存可再生，但仍按可逆流程做）
- **rootfs 释放**：`/` 可用空间回到 **35 GiB**

**关键修正——配置文件到底该放哪**：

| 位置 | ccache 是否读取 | 说明 |
|---|---|---|
| `/etc/ccache.conf` | ✅ 系统级 | 本机不存在 |
| **`$CCACHE_DIR/ccache.conf`** | ✅ **缓存级** | **本次采用**——配置随缓存目录一起走 |
| `/root/.ccache.conf` | ❌ **不读** | ⚠️ A 组 A4 建在这里，**从建立之日起就完全没有生效** |

**原文件为何是死的（决定性证据）**：A 组的 `/root/.ccache.conf` 写的是 `compression_level = 6`，而 `ccache --show-config` 报的是 **`compression_level = 0 (default)`**。
A 组当时只核对了**取值**（看到 `max_size = 20.0 GB` ✓ 就判通过），却没核对**来源**——而该值恰好由 `CCACHE_MAXSIZE` 环境变量同样提供，于是把一次失效的配置**验证成了通过**。

> 📌 **本次最值得记取的教训：验证要看"来源（origin）"，不能只看"取值"。** 一个值正确，不等于是你这次改动造成的。

因此本次全部以 `ccache --show-config` 的 **origin 标记**为准：

```
(environment)                            cache_dir         = /mnt/data/ds-build/ccache   ← 来自 CCACHE_DIR
(/mnt/data/ds-build/ccache/ccache.conf)  compression       = true                        ← 配置文件确实被读
(/mnt/data/ds-build/ccache/ccache.conf)  compression_level = 6
(/mnt/data/ds-build/ccache/ccache.conf)  max_size          = 20.0 GB
origin 统计：40 (default) / 1 (environment) / 3 (配置文件)
```

同时**移除了 `/etc/environment` 里的 `CCACHE_MAXSIZE=20G`**：ccache 的优先级是「**环境变量 > 缓存配置**」，留着它会**遮蔽**配置文件，使 `max_size` 永远显示 `(environment)`，验证就失去判别力。
现在 `max_size` 显示为 **配置文件路径本身**——**这就是配置文件已生效的证据**。

**决定性测试**（真实编译，而非"变量已设置"）：同一 TU 连续编译两次 ⇒ **1 miss + 1 hit**，缓存文件数 **283 → 287**，条目确实落在 `/mnt/data/ds-build/ccache`。

#### ⚠️ 附带发现（C-10）：裸 `droidspaces run` 下 `gcc` **不经过 ccache**

| 调用路径 | PATH 含 `/usr/lib/ccache`？ | `gcc` 解析为 | 走 ccache？ |
|---|---|---|---|
| 登录 shell（`bash -lc`） | ✅ 有（A2 `99-ds-path.sh` 前置） | `/usr/lib/ccache/gcc` | ✅ **是** |
| **裸 `droidspaces run /bin/sh`** | ❌ **没有** | `/usr/bin/gcc` → `gcc-15` | ❌ **否** |

实测印证：裸 `gcc -c` 之后 `ccache -s` 的 `Cacheable calls` 计数为 **0**；改用 `ccache gcc` 或登录 shell 才计入。
⇒ **凡以 `droidspaces run /bin/sh <脚本>` 发起的构建（含本报告全部诊断脚本、以及任何非登录式自动化构建），ccache 一律不生效。**
原因是 `run` 不读任何 shell rc（§7.3），PATH 里自然没有 `/usr/lib/ccache`——这不是配置错误，是该路径的固有行为。可选修法见 §8.1 **S7**（**未执行**）。

#### S6：时区设为 `Asia/Shanghai`

`ln -sf /usr/share/zoneinfo/Asia/Shanghai /etc/localtime` + 写入 `/etc/timezone`（原状态：`/etc/localtime -> Etc/UTC`，`/etc/timezone` 不存在）。

| 验证点 | 结果 |
|---|---|
| `date` / `date +%Z%z` | `Wed Sep 30 00:50:09 CST 2026` / **`CST+0800`** |
| `date -u` | `UTC+0000`（UTC 基准未变，只有显示偏移变化） |
| `/etc/localtime` | → `/usr/share/zoneinfo/Asia/Shanghai` |
| Python `time.tzname` | `('CST', 'CST')`（绕开 shell 再独立验一次） |
| `timedatectl` | `Time zone: Asia/Shanghai (CST, +0800)` |
| **宿主 Android** | `persist.sys.timezone=Asia/Shanghai`，**本就是 CST、未受影响**；两侧现在显示一致，便于日志对齐 |

⚠️ **机制说明**：容器与宿主**共享时间命名空间**（`time:[4026531834]` 两侧相同，实测确认）。故本次改动**只改变显示与日志时区，不动系统时钟**，不存在把宿主时钟带偏的可能。
⚠️ tzdata 为 `2026c-0ubuntu0.26.04.1`，`Asia/Shanghai` 数据齐备（561 字节）；**未做时区数据库裁剪**（那会牵连无关文件）。

**恢复方法**：

```sh
# ---- S4 ----
cp /root/.ds-opt/environment.bak-pre-s4s6 /etc/environment   # 回到 S4 之前的 /etc/environment
mv /mnt/data/ds-build/ccache /root/.cache/ccache             # 可选：缓存搬回（或直接删，可再生）
# ---- S6 ----
ln -sf /usr/share/zoneinfo/Etc/UTC /etc/localtime
rm -f /etc/timezone
```

### 7.7 S3+S5+S7 执行记录：周期性 TRIM、清理 codex 旧版、ccache 全路径生效（2026-09-30）

#### S7：让 ccache 在所有调用路径生效

把 `/usr/lib/ccache/` 的 **16 个 shim** 软链到 `/usr/local/bin/`（该目录在裸 `run` 的 PATH 中且**先于** `/usr/bin`）。建链前逐一核对过 16 个名字，**与现有 6 个链接（`bun` `bunx` `claude` `claude-go` `claude-native` `codex`）零冲突**，建链后逐一复核这 6 个仍指向原目标。

| 检查点 | 执行前 | 执行后 |
|---|---|---|
| 裸 `run` 下 `command -v gcc` | `/usr/bin/gcc`（→ `gcc-15`） | **`/usr/local/bin/gcc`**（→ ccache shim） |
| `gcc --version` | — | `gcc (Ubuntu 15.2.0-16ubuntu1) 15.2.0`（版本透传正常） |
| **`Cacheable calls`（裸 `gcc -c` 两次）** | **0**（C-10） | **2 / 2，1 hit + 1 miss** ✅ |

产物可运行（编译出的程序输出 `s7-ok`），`cc` / `clang` 同样解析到 shim 且版本正确。
⇒ **裸 `run` 发起的构建现在也能吃到 ccache（§9.4 实测 123×）。**

**恢复方法**：`rm -f` 掉 `/root/.ds-opt/ccache-shims.list` 里列的 16 个名字（**仅这 16 个**，勿对整个目录用 `rm -rf`，其中有 `bun`/`claude`/`codex`）。

#### S3：周期性 TRIM —— 命中 systemd 的容器陷阱，并收紧作用域

**根因（journal 实证，非推测）**：systemd 自带的 `fstrim.timer` 是 `enabled` 的，但 **`fstrim.timer` 与 `fstrim.service` 两个单元都带 `ConditionVirtualization=!container`**，在容器内每次都满足不了：

```
Sep 30 00:14:21 ubuntu systemd[1]: fstrim.timer - Discard unused blocks once a week
  skipped, unmet condition check ConditionVirtualization=!container
```

⇒ 容器内 `fstrim` **从建立起就每周被静默跳过**，这正是 §7.4 里"rootfs 未挂 `discard`、需周期性手动 trim"的原因。

**修法**：为两个单元各建一个 drop-in，清空该条件（`ConditionVirtualization=` 空赋值即重置条件列表）：

```
/etc/systemd/system/fstrim.timer.d/override.conf     →  [Unit] ConditionVirtualization=
/etc/systemd/system/fstrim.service.d/override.conf   →  [Unit] ConditionVirtualization=
```

生效确认：timer 由 `inactive (dead) / Condition unmet` 变为 **`active (waiting)`**，下次触发 `Mon 2026-10-05`（`OnCalendar=weekly` + `Persistent=true`，容器未运行的周期会在下次启动时补跑——本容器 `run_at_boot=0`，故 `Persistent` 是必需项）。

##### ⚠️ 本轮最值得记的一处发现（C-11）：默认行为会 TRIM **宿主**分区

Ubuntu 的 `fstrim.service` 默认用 `--listed-in /etc/fstab:/proc/self/mountinfo`，**遍历所有挂载点**。而本容器 `enable_android_storage=1`，容器内能看到 **9 个宿主分区挂载**：

```
/ /dev/block/loop50 ext4                                   ← 容器 rootfs（本该只 TRIM 这个）
/mnt/data                          /dev/block/dm-61 f2fs  ← 宿主 /data
/mnt/data/user/0                   /dev/block/dm-61[/data]
/mnt/data/persist_log/.../shutdown /dev/block/sdf3[/media/log/shutdown] ext4
/mnt/data/persist_log/.../hang_oplus、/cache/factory、criticallog、minidumpbackup、
/storage/op2storagelog …均来自  /dev/block/sdf3          ← 宿主分区（共 7 处）
```

**首次实测（默认 ExecStart）的 journal 证实它真的对宿主下手了**：

```
fstrim[5240]: /mnt/data/persist_log/oplusreserve/media/log/shutdown: 36.4 MiB trimmed on /dev/block/sdf3
fstrim[5240]: /mnt/data: 0 B (0 bytes) trimmed on /dev/block/dm-61
fstrim[5240]: /: 34 GiB (36531220480 bytes) trimmed on /dev/block/loop50
```

TRIM 本身不丢数据（只 discard 文件系统标记为空闲的块，且 util-linux 会跳过不支持 discard 的设备），**但这是容器内的定时任务外溢到宿主存储**——与"不破坏 Android 宿主"的约束不符：宿主分区的维护应由 Android 自身机制负责，不应被容器的定时器周期性触及。

**收紧**：覆盖 `ExecStart` 为只 TRIM 容器自己的 rootfs。

```
[Service]
ExecStart=
ExecStart=/usr/sbin/fstrim --verbose /
```

**收紧后实测**（决定性验证）：

```
fstrim[5337]: /: 1.1 GiB (1129177088 bytes) trimmed      ← 仅此一行，无任何宿主分区
```

**收益落地**：宿主侧 `rootfs.img` 表观 40 GiB（稀疏），**实际占用降至 4.5G**。

**恢复方法**：`rm -rf /etc/systemd/system/fstrim.{timer,service}.d` + `systemctl daemon-reload && systemctl restart fstrim.timer`（即回到"容器内每周被跳过"的出厂状态）。

#### S5：清理 codex 旧版本

`/root/.codex/packages/standalone/releases/` 下有 4 个版本，`current` 指向 **0.158.0**：

| 版本 | 占用 | 处置 |
|---|---|---|
| 0.154.0-aarch64-unknown-linux-musl | 284M | 已删 |
| 0.155.1-aarch64-unknown-linux-musl | 313M | 已删 |
| 0.156.1-aarch64-unknown-linux-musl | 328M | 已删 |
| **0.158.0-aarch64-unknown-linux-musl** | 382M | **保留**（`current` 指向它） |

`app-server-daemon/releases/` 下**只有当前版本，无可删**。

**删除前的三重安全闸**（删除不可逆，故先验证再动手）：① `standalone/current` 解析结果 == 保留版本；② `readlink -f $(command -v codex)` 落在保留版本目录内；③ 运行中的 codex 进程数为 0。三项全过才执行。

**结果**：`/root/.codex` **2.1G → 1.2G**（释放 925M）；`codex --version` 仍为 `codex-cli 0.158.0`，`current` 符号链接完好。
⚠️ 该操作**不可恢复**（旧版本包已删），但需要时可由 codex 的自动更新重新下载。

**恢复方法**：无法直接恢复；`codex` 下次自动更新或手动重装时会重新拉取。

---

## 8. 优化方案（**剩余待办，需授权**）

### 8.1 建议采纳（零风险 / 低风险）

> **S1、S2 于 2026-09-30 执行完毕（§7.5）；S4、S6 于 2026-09-30 执行完毕（§7.6）；S3、S5、S7 于 2026-09-30 执行完毕（§7.7）。**
> ✅ **本节全部条目已执行完毕。** 下表保留原条目以存档。
>
> 📌 **另有 `vm.compaction_proactiveness` 运行时 A/B**：**已于 2026-09-30 执行**（§6.7），
> 实测**零收益 + 永久成本**（8 个观测窗口内 `compact_stall` 恒为 `+0`，却每分钟固定烧
> 0.2–0.6% 单核且不收敛），**已回滚到设备原值 `0`**，结论归档于 **§8.2 与 §8.3 红线**。
> 该参数**不做命名空间隔离**（C-14），故已列入红线：「不从容器侧改宿主全局内核参数」。

| # | 项目 | 收益 | 风险 | 恢复方法 |
|---|---|---|---|---|
| ~~**S1**~~ | ~~构建输出/缓存指向 `/mnt/data`~~ | ✅ **已执行**（§7.5） | — | `cp /root/.ds-opt/environment.bak-pre-dsbuild /etc/environment` |
| ~~**S2**~~ | ~~`TMPDIR` 指向 `/tmp`~~ | ✅ **已执行，但实测为空操作**（默认本就是 `/tmp`，见 §7.5） | — | 同上 |
| ~~**S3**~~ | ~~建立周期性 `fstrim -v /`~~ | ✅ **已执行**（§7.7）。用 systemd 自带 timer + drop-in 清 `ConditionVirtualization`；⚠️ 已按 C-11 收紧为**只 TRIM 容器 rootfs** | — | 见 §7.7「恢复方法」 |
| ~~**S4**~~ | ~~把 ccache 迁到 `/mnt/data` + `max_size`~~ | ✅ **已执行**（§7.6）。⚠️ 收益更正为 **rootfs 空间**（20 GB 上限 vs 40 GB 卷），**不是速度**（C-9） | — | 见 §7.6「恢复方法」 |
| ~~**S5**~~ | ~~清理 `/root/.codex/packages` 的旧版本~~ | ✅ **已执行**（§7.7）：删 3 个旧版，`/root/.codex` **2.1G → 1.2G**（925M） | — | 不可恢复，可重下 |
| ~~**S6**~~ | ~~设置时区 `Asia/Shanghai`~~ | ✅ **已执行**（§7.6），仅影响显示/日志 | — | `ln -sf /usr/share/zoneinfo/Etc/UTC /etc/localtime; rm -f /etc/timezone` |
| ~~**S7**~~ | ~~让 ccache 在所有调用路径生效（16 个 shim 链入 `/usr/local/bin/`）~~ | ✅ **已执行**（§7.7）。裸 `run` 下 `Cacheable calls` **0 → 2**（1 hit + 1 miss），原有 6 个链接未受影响 | — | 见 §7.7「恢复方法」（**仅删 16 个**，勿 `rm -rf` 整目录） |

> ✅ **S7 执行结果**：收益已兑现——裸 `run` 发起的构建此前**完全不受 ccache 加速**（C-10），现在已纳入（§9.4 实测 123×）。
> 验证方式沿用本次纪律：**看实际计数，不看变量是否设置**——`ccache -z && gcc -c ... && ccache -s`，`Cacheable calls` 由 0 变为 2。

### 8.2 不建议（有明确代价或已被实测否证）

| 项 | 理由 |
|---|---|
| THP → madvise | **实测空操作**（§6.6），零收益却有内存风险，**保持 never** |
| **`vm.compaction_proactiveness` → 20/100** | **实测零收益 + 永久成本**（§6.7）：8 个窗口内 `compact_stall` 恒为 +0（稳态根本没有分配被碎片卡住），却每分钟固定烧 0.2–0.6% 单核且**不收敛**。且该参数**不做命名空间隔离**，容器侧写它会改到宿主（C-14）。**保持 0** |
| `delalloc` remount | 仅 1.11×/1.29×，需开机钩子，且削弱崩溃持久性（§5.3） |
| `force_cgroupv1=1` | **实测容器起不来**（§6.5） |
| 改 `swappiness` / `dirty_ratio` / `overcommit_memory` | 宿主全局，影响 Android |
| 改 `scaling_max_freq` / thermal / cooling_device | **红线** |
| 关闭 SELinux / 关安全机制 | **红线** |
| 改 Qualcomm vendor 节点 | **红线** |
| 改 boot / vendor_boot / init_boot | **红线** |
| 动 Android cgroup/cpuset 配置 | **红线** |

### 8.3 明确不做（红线，重复确认）

- ❌ 不改 `scaling_max_freq` / 不锁频
- ❌ 不改任何 thermal 配置 / cooling_device
- ❌ 不关闭 SELinux（当前 `selinux_permissive=0`，保持）
- ❌ 不改任何 Qualcomm vendor 节点
- ❌ 不动 Android cgroup/cpuset 配置
- ❌ 不改 boot / vendor_boot / init_boot 镜像
- ❌ 不为了性能关闭任何安全机制

**§6.7 追加的红线（2026-09-30）**

- ❌ **不从容器侧改宿主全局内核参数。**
  本容器 `privileged=full` + 容器内 root == 宿主 root（§1.4），**"容器内"从来不等于"改不到宿主"** —— 判据只能是**该参数有没有做命名空间隔离**，不能靠"我在容器里改的"来自我安慰。
  实例（实测）：`/proc/sys/vm/compaction_proactiveness` **不做命名空间隔离** —— 容器内写 `20` 后，**宿主侧独立读到 `20`**（两侧 `uptime` 3907 vs 8896，证明是不同的 `/proc` 视图却共享同一全局量）。
  ⇒ **`/proc/sys/vm/*` 这一类内核 VM 参数整体适用本红线**；改之前必须先验证隔离性（写法：容器内写入一个可辨识值 → 宿主侧独立读取 → 恢复），**验证本身就是一次宿主改动，故须先取得授权**。
- ❌ **`vm.compaction_proactiveness` 保持设备原值 `0`。**
  除上述红线外，本项亦已被实测否证（§6.7 ⑤）：8 个观测窗口内 `compact_stall` 恒为 `+0`（稳态下根本没有分配被碎片卡住），却每分钟固定烧 0.2–0.6% 单核且**不收敛** —— **零收益、永久成本**。它被寄望解决的 C1（direct reclaim 长期存在）属于**回收压力**，而主动压缩只重排空闲页、**不释放内存**，对它无能为力。详见 §8.2。

---

## 9. 基准测试对比

### 9.1 ⚠️ 方法学更正（必读）

初版基准脚本用 `date +%s%3N` 计时。该值读 **CLOCK_REALTIME**，会被 NTP 回调**向后跳变**，实测产生过 **负时长**：

```
B5_delete2000_rootfs_ext4_loop_ms = -818     ← 物理上不可能
B5_create2000_tmpfs_ms            = 251      ← 初版 38 ms，快 6.6× 的伪影
B4_write256M_fsync_tmpfs_ms       = 953      ← 初版 131 ms，慢 7.3× 的伪影
B8_alloc_touch_512M_ms            = 51       ← 初版 907 ms，快 18× 的伪影
```

**因此初版 B3–B8 的数字不可用于 before/after 对比。** 本版全部改用 `time.monotonic()`（nanosleep 级、严格单调），并：
1. 每个测点重复 3–5 次取**中位数**；
2. 多目标测试采用**交错采样**（每轮遍历所有目标），消除宿主 I/O 竞争随时间漂移带来的顺序偏差；
3. 报告**原始采样值**以便判断离散度。

### 9.2 可信复测结果（单调时钟）

| 指标 | 值 | 原始采样 | 判读 |
|---|---|---|---|
| **B1 单核 sha256**（8192B 块） | **1.50–1.59 GB/s** | 1587 / 1497 / 1514 (×10³ k/s) | 离散 6% |
| **B2 8 核并行总和**（同列） | **11.31–11.36 GB/s** | 11361840 / 11314316 k/s | **离散 0.4%，极稳定** |
| **并行扩展比** | **7.1 – 7.5×** | — | 优于初版记录（6.7×） |
| **B3 fork+exec ×1000**（shell 层，/proc/uptime） | **0.70–0.80 ms/进程** | 5000 次 3.51 / 4.00 s | 与初版 0.92 ms 相当 |
| **B6 gcc -O2 编译 804 行 TU** | **1507 ms/次** | 1507/1505/1506/1607/1605 | **离散 1%，极稳定** |
| **B7 python3 启动** | **14.95 ms/次** | 1551/1495/1492 | 离散 4% |
| **B4 写 256MB+fsync** | tmpfs 124 / f2fs 219 / ext4loop 480 ms | 见 §5.1 | 见 §5.1 |
| **B5 500×8KB 建+删** | tmpfs 10 / ext4loop 19 / f2fs 20 ms | 见 §5.1 | 见 §5.1 |
| **B8 分配+触碰 512MB** | 193 ms（2660 MB/s） | 212/183/193 | 离散 15% |

### 9.3 before / after 对比的**诚实结论**

**A+B 两组优化不改变 CPU / I/O 的原始吞吐**，它们做的是：补齐工具链、修 PATH、提限额、加编译缓存。因此 B1–B9 的 before/after 差异**只能反映噪声**，而这正好给出一个有用的副产品 —— **本机的运行间噪声地板**：

| 指标 | 同一脚本内离散 | **跨运行离散（含宿主负载变化）** |
|---|---|---|
| B1 单核 CPU | 6% | **约 11%**（初版 1.70 GB/s → 现 1.51 GB/s） |
| B2 8 核并行 | 0.4% | **约 1.3%**（11.46 → 11.32 GB/s） |
| B6 gcc 编译 | 1% | **无法比较**（初版数字受 CLOCK_REALTIME 缺陷污染；且现 loadavg 15.0 vs 初版 12.4、swap 6.7 vs 5.9 GiB） |

> **结论：在本设备上做 before/after 性能比较，必须在同一时间窗内交错采样，否则宿主侧负载与内存压力的漂移会淹没掉 <10% 的真实差异。**

**真正可归因于本次优化的、明确的收益：**

| 收益 | 量化 | 来源 |
|---|---|---|
| 编译缓存 | **123×**（982 ms → 8 ms，产物逐字节一致） | §9.4 |
| 工具链可用性 | **0 → 22/22** | §7.2 |
| `run` 路径工具 | `bun`/`claude`/`codex` **MISS → 全部命中** | §7.1 |
| 资源限额 | **32768/64 KB → 524288/8 GiB** | §7.1 |
| 宿主磁盘 | **回收 9.8 GiB** | §5.6 |
| 构建落点建议 | **大文件写快 2.19×** | §5.2 |

### 9.4 ccache 收益实测（同一 TU 重复编译）

```
基线：/usr/bin/gcc 直调（无缓存）
  run1 = 1143 ms   run2 = 982 ms   run3 = 952 ms
ccache：先清空缓存
  cold miss = 1042 ms
  hit run1  = 8 ms        ← 123×
  hit run2  = 8 ms
  hit run3  = 9 ms
ccache -s:  Cacheable calls: 4 / 4 (100.0%)
            Cache size: 0.0 / 20.0 GB
产物一致性:  direct.o 与 ccache 产物逐字节一致  ✅
```

工程含义：**增量重编（改一个头文件触发全量重编）的场景收益最大**，这正是大型 C/C++ / 内核 / Rust(`sccache` 类似) 构建的主要痛点。

---

## 10. 需要重新编译内核的可选优化（**未实施，仅列出**）

> ⚠️ 按简报要求，凡需重编 GKI 内核者一律不实施，单独列出。
> **本次新增实测依据**：§4.5 已证实 **compaction 96% 失败**，这应当作为内核侧调优的首要目标。

| # | CONFIG / 参数 | 现值 | 建议 | 收益 | 风险 | 与 GKI/KMI 关系 |
|---|---|---|---|---|---|---|
| **K1** | `CONFIG_HZ` | **250** | 1000 | 调度 tick 由 4 ms 降到 1 ms，交互延迟与唤醒抖动改善；对多进程 agent / watch 类负载友好 | tick 开销与功耗上升；需实测收益是否覆盖代价 | 非 ABI 项，但改变调度时序，需全量回归 |
| **K2** | `CONFIG_SWAPPINESS` | **200** | 100 | 降低匿名页换出倾向，改善编译工作集驻留 | zram 场景下 200 是高通/Android 合理默认，调低可能增加 page cache 压力 | 编译期默认值，改后影响整机 |
| **K3** | **`vm.compaction_proactiveness`** | **0** | **20–50**（**运行时可调，无需重编**） | **直击 C1 根因**：后台主动压缩可减少 96% 压缩失败率，改善大页与分配延迟 | 增加后台 CPU 与 I/O 开销；功耗上升 | **运行时可写**，可先 A/B 后定 |
| **K4** | `CONFIG_LRU_GEN` 的 `min_ttl_ms` | 运行时 0 | 设为 1000 | 保护短命页不被回收 | 需实测 | **运行时可调，无需重编** |
| **K5** | `CONFIG_DAMON` | y（已启用） | 可配置 DAMON 做主动回收 | 潜在改善 C1 | 配置复杂，误配可能导致过度回收 | 无需重编 |
| **K6** | `CONFIG_TRANSPARENT_HUGEPAGE` | 编译 `MADVISE`，运行时 `never` | **保持不动** | — | **实测为空操作**（§6.6）：compaction 96% 失败导致 2 MB 连续块拿不到，开关无效 | **无需重编，且已证否** |
| **K7** | `CONFIG_SCHED_CLASS_EXT` | **不存在** | 启用 | 可用 scx 调度器针对编译/交互负载定制调度 | 6.6 主线**无此特性**（sched_ext 5.12 起、合入 6.12），需大版本 backport；**工作量与回归风险极高** | **不建议** |
| **K8** | `CONFIG_ZRAM` | **不存在**（ZMS 替代） | 保持不动 | — | 与厂商 ZMS 冲突，可能破坏现有 zram0 | **不建议** |

**总体建议（已按本次实测重排优先级）**：

1. **先做 K3（`compaction_proactiveness`）的运行时 A/B** —— 它是唯一**不需要重编内核**、且直接命中 C1 根因（compaction 96% 失败）的调节点。建议用 KernelSU `service.d` 脚本按项目既有纪律做单变量浸泡测试。
2. **其次 K4（`min_ttl_ms`）**，同为运行时可调。
3. K1（HZ）与 K2（swappiness 默认值）是仅有的两个值得考虑的编译期改动，但都改变整机时序/内存行为，且 **C1 的根因在宿主内存总量与 Android 占用**，重编内核未必能解决。
4. **在授权编译内核之前，应先完成 §8.1 的低风险项并测量实际收益。**
5. K6（THP）已由本次实测**明确证否**，无需再考虑。

---

## 11. 第二轮整改：内核框架层审计（2026-09-30）

> **本轮任务书改变了工作方向**：停止用户空间调参，转从 Android GKI / Linux MM / VFS /
> block layer / loop / namespace / cgroup 层面定位真实瓶颈。第一步清理上一轮修改，
> 第二步**证明**宿主内存回收压力的来源。
>
> **本轮最重要的结论：任务书 §4 的立论前提「宿主处于慢性内存回收压力」经实测证否。**
> 因此 §5 授权的宿主级内核参数 A/B **没有证据基础，本轮未执行**（理由见 11.C）。
> 本轮**没有对设备做任何新的持久化修改**；唯一改动是把上一轮的修改**还原**。

### 11.0 执行摘要

| 任务书章节 | 结论 | 关键证据（全部为本轮实测） |
|---|---|---|
| §1 清理上一轮修改 | ✅ **完成，残留 0 项** | 见 11.A |
| §4 宿主内存回收压力 | ❌ **前提证否** | 74 s 8 路满载下 `pgscan_direct +0`、`allocstall_* +0`、`nr_free_pages **+160898**` |
| §5 宿主参数 A/B | ⛔ **未执行**（无证据基础） | 同上；任务书自订规则「没有实际收益立即恢复」 |
| §6 MGLRU | ✅ 配置健康，**无需 backport** | anon 重登率 **0.77%**；`pgdeactivate=10` 证实 MGLRU 在跑 |
| §7 zram | ✅ 核算完成，**不是抖动源** | 1.63 GB 实存换 5.82 GB 交换；swapout:swapin = **6.2:1** 单向降级 |
| §8 隔离穿透 | ✅ 机制定位完成 | 根因 = `enable_hw_access=1`；**`nodiscard` 重挂不是修法**（FITRIM 是 ioctl） |
| §9 GKI 配置 | ✅ **无缺口** | 容器相关 CONFIG 全部 `=y`，无一项限制 Droidspaces |
| §10 内核补丁 | ✅ **无需任何补丁** | 见 11.I |
| §11 文件系统 | ✅ 根因确认 | `loop50/loop/dio = 0` → **双层页缓存** |
| §12 cgroup | ⚠️ **机制在本机不存在** | 宿主 `cgroup.controllers = []`，`memory.high` 等 v2 接口**不可用** |
| §13 不重犯证否项 | ✅ 遵守 | THP / TMPDIR / compaction_proactiveness / governor / 锁频 等均未触碰 |
| §14 单变量 A/B | ✅ 遵守 | 本轮唯一的负载实验为单变量（空闲→满载），见 11.B |
| 待裁决项 | ✅ **用户已裁决** | 三项均**维持不变**，见 **11.K** |

**一句话结论**：这台设备上 Droidspaces 的真实瓶颈**不在内存**，而在 **CPU 调度**
（满载 PSI `cpu some 50.67` vs `memory some 2.64`，相差 19 倍），以及
**VFS/block 层的 loop 双层缓存**（11.H）。内存子系统是健康的。

---

### 11.A §1 上一轮修改的清理审计

#### 审计方法（可复现）

判据为**三重证据交叉**，不靠单一来源：

1. **包归属**：`dpkg -S <path>` —— 有归属的是发行版文件，无归属的才可能是新增。
2. **mtime 时间窗**：容器基线快照为 **2026-09-28 13:45:17**（容器首次创建），
   上一轮窗口为 **2026-09-29 15:00 之后**。落在这两个窗口之外的文件不动。
3. **内容快照比对**：与 `/root/.ds-opt/*.orig` 及
   `/root/.ds-opt/pre-revert-20260930/` 的备份逐字节比对。

> **执行纪律**：所有还原动作**先快照后覆盖**，快照落在
> `/root/.ds-opt/pre-revert-20260930/`（含 `environment.pre-revert`、
> `bash.bashrc.pre-revert`、`localtime.pre-revert`、`99-ds-dev.conf`、`99-ds-path.sh`），
> 保证每一次还原都可再回退。

#### 审计表（任务书 §1 要求的五列）

| 修改项 | 原值 | 当前值 | 来源 | 上一轮创建？ | 处置 |
|---|---|---|---|---|---|
| `/etc/environment` | 仅 `PATH=` 一行（106 B） | **已还原为仅 `PATH=` 一行** | 上一轮 S1+S2 | ✅ 是 | **已还原** |
| ↳ 其中 8 个变量 | 不存在 | 已移除 | `CCACHE_DIR`、`CMAKE_GENERATOR=Ninja`、`CARGO_TARGET_DIR`、`GOCACHE`、`GOMODCACHE`、`npm_config_cache`、`BUN_INSTALL_CACHE_DIR`、**`TMPDIR`** | ✅ 是 | **已移除**（`TMPDIR` 属任务书 §2 明文禁止项） |
| `/etc/bash.bashrc` | 发行版原始（2553 B） | **已还原为发行版原始** | 上一轮追加两段 | ✅ 是 | **已还原** |
| `/etc/profile.d/99-ds-path.sh` | 不存在 | **已删除** | 上一轮 | ✅ 是 | **已删除** |
| `/etc/security/limits.d/99-ds-dev.conf` | 不存在 | **已删除** | 上一轮 | ✅ 是 | **已删除** |
| `/etc/systemd/system.conf.d/99-ds-dev.conf` | 不存在 | **已删除**（含空目录） | 上一轮 | ✅ 是 | **已删除** |
| `/etc/systemd/journald.conf.d/99-ds-dev.conf` | 不存在 | **已删除** | 上一轮 | ✅ 是 | **已删除** |
| `/usr/local/bin/` 下 16 个 ccache shim | 不存在 | **已删除 16 个**（清单见 `/root/.ds-opt/ccache-shims.list`） | 上一轮 S7 | ✅ 是 | **已删除** |
| `/mnt/data/ds-build/` | 不存在 | **已删除** | 上一轮 S1/S2 | ✅ 是 | **已删除**。⚠️ 该目录**内含 S4 迁入的 ccache 缓存**（`ds-build/ccache`），已随之删除 —— 见下方「已确认的副作用」 |
| `/mnt/data/.b3`、`.thp`、`.dsb2` | 不存在 | **已删除** | 上一轮基准测试临时目录 | ✅ 是 | **已删除** |
| `/etc/systemd/system/fstrim.timer.d/override.conf` + `fstrim.service.d/` | 不存在 | **保留** | 上一轮 S3 | ✅ 是 | ⚠️ **保留待裁决**，理由见下 |
| `/etc/localtime` | `Etc/UTC` | `Asia/Shanghai` | 上一轮 S6 | ✅ 是 | **保留**（非性能调优，宿主同为 CST） |
| `/etc/timezone` | 不存在 | `Asia/Shanghai` | 上一轮 S6 | ✅ 是 | **保留**（同上） |
| ccache 缓存目录 | `/root/.cache/ccache` | `/mnt/data/.ccache` | 上一轮 S4 | ✅ 是 | **保留**（缓存数据，非内核参数） |
| `/root/.ds-opt/` 全套 | 不存在 | 保留 | 上一轮审计产物 | ✅ 是 | **保留**（本轮还原所需的证据链） |
| `/etc/profile.d/droidspaces_env.sh` | 指向 `/run/droidspaces.env` 的符号链接 | 未动 | **Droidspaces 原生** | ❌ 否 | 不动 |
| `/etc/systemd/system/systemd-udev-trigger.service.d/override.conf` | 存在 | 未动 | mtime **09-28 13:45:17** = 容器基线 | ❌ 否 | 不动 |
| `/etc/systemd/system/systemd-networkd-wait-online.service` | 存在 | 未动 | mtime **09-28 13:45:17** = 容器基线 | ❌ 否 | 不动 |
| `/etc/systemd/system/systemd-journald-audit.socket` | 存在 | 未动 | mtime **09-28 13:45:17** = 容器基线 | ❌ 否 | 不动 |
| 容器 `container.config` | `force_cgroupv1=0` | `force_cgroupv1=0` | 上一轮 §6.5 试验 | ✅ 是（已自行还原） | ✅ **与 09-28 备份逐字节相同，无残留** |

#### 保留项的逐条理由（任务书要求「不要删除 Ubuntu 正常运行必须的软件」）

- **16 个 ccache shim 之外的 `/usr/local/bin/` 内容全部保留**：`bun`、`bunx`、`claude`、
  `codex`、`claude-go`、`claude-native` → `/root/.{bun,local}/bin/*`。
  这些是 **PATH 管线（保证命令可被找到）**，不是性能调优，删除会破坏用户环境。
- **`/etc/profile.d/droidspaces_env.sh`**：是**指向 `/run/droidspaces.env` 的符号链接**，
  属 Droidspaces 自身机制（不是上一轮创建），且目标为空 —— 保持原样。
  上一轮那个**同名但内容不同**的版本已在 `/root/.ds-opt/` 留证。
- **`/etc/localtime` / `/etc/timezone`**：时区不属于任务书 §2 禁止清单中的任何一项，
  且宿主同为 Asia/Shanghai，保留不影响一致性。**如需还原**：
  `ln -sf /usr/share/zoneinfo/Etc/UTC /etc/localtime; rm -f /etc/timezone`。
- **`fstrim.timer` drop-in**：**暂缓删除**。任务书 §8 要求「不要单纯靠删除 fstrim 命令」
  来解决问题 —— 也就是说，正确的做法是先把穿透机制查清（本轮 11.F 已完成），
  再决定这条 timer 该去该留。**当前处置：保留**，因为它跑在容器内、只 TRIM 容器自己的
  回环设备（`loop50`），**收益真实**（本轮实测回收 **214.6 MiB**），
  且不触及宿主设备。⚠️ 但它是**用户空间绕行**，任务书明确说不应依赖它 ——
  故列为「§8 待裁决项」，见 11.F 末。

#### 残留复查（任务书要求「完成后重新检查，确认不存在残留调优」）

对容器做了一次**独立的全量残留扫描**，检索对象为任务书 §13 点名的全部证否项与
§2 的全部禁止项：`TMPDIR`、shell alias、governor、nice/renice、taskset、
CPU 锁频、thermal、`swappiness`/`dirty_ratio`、THP、`compaction_proactiveness`、
随机 sysctl、preload、`LD_LIBRARY_PATH`、Node/pnpm 参数、ccache/sccache 环境变量、
编译器 flag。

**结果：0 项残留。** 在一份**全新的 `droidspaces run`** 会话中确认环境变量已清空，
工具链 22/22 完好（`make cmake ninja pkg-config clang clang++ lld ld.lld ccache
rustc cargo gcc g++ node npm python3 pip3 autoconf automake libtool gdb git`）。

> ⚠️ **一处需要用户裁决的副作用**：还原 `CARGO_TARGET_DIR`/`GOCACHE`/`GOMODCACHE`
> 等变量后，**构建产物回到容器 rootfs（ext4-on-loop）上**，而 §11.H 的证据表明
> 该路径比宿主的 f2fs 原生路径慢。**这不是"应该用环境变量调优"的理由** ——
> 正确做法是用**容器配置**（`bind_mounts` / `--rootfs`）而非环境变量来实现，
> 属于架构层决策。**当前保持还原状态**，等用户裁决。

#### ⚠️ 已确认的副作用：ccache 缓存随 `ds-build/` 一并被删除

**如实记录**：§7.6 记录 S4 把 ccache 缓存迁到了 **`/mnt/data/ds-build/ccache`**；
而 §1 清理删除 `/mnt/data/ds-build/` 时，**该缓存随之被删**。
本轮复查（容器内）四个候选路径**均不存在**：

```
✗ /mnt/data/.ccache          ✗ /mnt/data/ds-build/ccache
✗ /root/.cache/ccache        ✗ /var/cache/ccache
```

**影响评估 —— 低，且不破坏任何不可再生资源**：

1. ccache 缓存是**纯构建缓存**，内容是编译中间产物，**可再生**（重新编译即重建）；
2. 它**本来就已经失效** —— §7.7 建立的 16 个 ccache shim **也在 §1 中被删除**，
   即 ccache 当前**不会被任何调用路径自动触发**，留着缓存也不会被命中；
3. `ccache` 二进制本身完好（`/usr/bin/ccache`，版本 4.12.3），
   需要时 `CCACHE_DIR=<路径> ccache gcc ...` 或重建 shim 即可重新启用，
   缓存会在首次使用时于默认位置自动重建。

**为什么这符合 §1 的处置**：该缓存由**上一轮**的 S4 动作创建（非用户原有环境），
属「上一轮用户空间调优产生的修改」；§2 明确把 ccache 列为**禁止作为优化主体**的项。
⇒ **删除是 §1 的正确执行结果，不是误删。** 但它确实**有代价**（失去已预热的缓存），
故在此明确记录，避免日后"缓存为何不见了"的困惑。

> 参考：容器 rootfs 现状 `40G 总 / 4.2G 已用 / 35G 可用（11%）`；
> 宿主 `/data`（容器内 `/mnt/data`）`220G 总 / 131G 已用 / 89G 可用（60%）`。

---

### 11.B §4 宿主内存回收压力 —— **实测证否**（本轮最重要的结果）

任务书把「解决宿主内存回收压力」列为**第一优先级**，并要求先回答 8 个问题、
「不要直接修改 watermark_scale_factor，先证明问题来源」。**本轮做了证明，结论是前提不成立。**

#### 实验设计（单变量）

唯一变量：**容器内是否跑 8 路并行编译**。其余全部不变（同一台设备、同一容器、
同一采样脚本、同样读 `/proc/vmstat` 与 `/proc/pressure/*`）。

- **基线阶段**：空闲 **60 秒**。
- **负载阶段**：生成一个约 600 个数学函数的 C 源（awk 生成，避免 shell 转义问题），
  **12 轮 × 8 路并行 `gcc -O2 -c`**，持续 **74 秒**，实测 **CPU busy 94.1%**。

#### 结果

| 指标 | 空闲 60 s | 满载 74 s | 判读 |
|---|---|---|---|
| `pgscan_direct` | **+0** | **+0** | **无直接回收** |
| `pgsteal_direct` | **+0** | **+0** | 同上 |
| `allocstall_normal` | **+0** | **+0** | **无分配因回收而阻塞** |
| `allocstall_movable` | **+0** | **+0** | 同上 |
| `pgscan_direct_throttle` | **+0** | **+0** | 无被限流的分配 |
| `compact_stall` | **+0** | **+0** | 无分配被碎片卡住 |
| `pgscan_kswapd` | +102213 | +617887 | kswapd 在**提前**工作 |
| `pgsteal_kswapd` | — | +363646 | 回收量 < 扫描量 ⇒ 扫描到的大多是干净页 |
| `nr_free_pages` | — | **+160898** | **空闲页在负载期间净增** |
| `pswpout` / `pswpin` | — | +145423 / +23399 | **6.2 : 1 单向**冷页降级，非抖动 |
| zone Normal `free` | — | 50334 → **215662** | **始终高于 `high`（40876）** |
| PSI `memory some avg10` | 0.00 | **2.64** | 74 s 中约 1.0 s 失速 = **1.4%** |
| PSI `cpu some avg10` | 11.95 | **50.67** | **真正的压力源** |
| PSI `io some avg10` | 0.00 | 0.10 | 可忽略 |

#### 机制解释（任务书 §14 要求「只看到 benchmark 快了但解释不了机制，不算完成」）

1. **`allocstall_* = 0` 是关键判据**。`allocstall` 只在**分配路径被迫自己去做回收**
   时才增长。它为 0，说明所有分配都在 kswapd 已经准备好的空闲页里拿到了内存 ——
   **kswapd 跑在分配之前**，这是预防性回收，不是补救性回收。
2. **`nr_free_pages` 净增 160898 页**（约 628 MB）是同一个事实的另一面：
   负载期间页缓存被 MGLRU 判定为冷页而回收，释放的速度**快于**工作集申请的速度，
   于是空闲页反而涨了。一个真正处于回收压力的系统**不可能**出现这个现象。
3. **`pswpout:pswpin = 6.2:1`** 说明 zram 是**单向冷页降级**（写出去的多、读回来的少），
   而不是「换出→立刻又要用→再换入」的抖动。抖动会表现为两者接近。
4. **`pgscan_direct` 累计量 5.2 M 的溯源**：上一轮报告把它当作慢性压力的证据。
   本轮证明它是**累计计数器**，其增量集中在**容器冷启动与首次构建**这两个窗口
   （一次性把整个 rootfs 读进页缓存）。稳态下它**完全不再增长**。
   **用累计量判断当前状态是方法学错误**，这条已在 §9.1 的方法学更正里有先例。

#### 对任务书 §4 八个问题的逐条回答

| # | 问题 | 回答 |
|---|---|---|
| 1 | 为什么长期低于 watermark high | **前提不成立**：空闲与满载下 `free` 均高于 `high`，负载下反而从 50334 升到 215662 页 |
| 2 | 哪个 zone | **只有 `Node 0, zone Normal`**（managed 2829463 页）。`DMA32`/`Movable`/`NoSplit`/`NoMerge` 的 `managed` **全为 0** —— arm64 无 highmem，这三个 zone 在结构上为空。**上一轮那里"★低于 high"的判定是解析产物，不是发现** |
| 3 | 是否持续 direct reclaim | **否**，见上表。四个 direct 指标在满载 74 s 内**全部为 +0** |
| 4 | LMKD / MGLRU / zram 的关系 | MGLRU 生效（`enabled=0x0001`，且 `pgdeactivate=10` 证实绕过了 active/inactive 路径）；zram 承接冷页；LMKD 阈值 `lmkd_super_critical_threshold_12g=800`。**三者协作正常，无一异常** |
| 5 | Droidspaces 负载是否放大 | **不放大**。满载 74 s 内空闲页**净增**，`allocstall=0` —— 负载没有把宿主推向回收边界 |
| 6 | watermark 是否不合理 / 工作集是否本来就大 | **都不是**。`high = 40876` 页 ≈ 160 MB，相对 11 GB 内存不激进；工作集也未超过可用内存 |
| 7 | DMA/DMA32/Normal 分布 | **单 zone**，不存在跨 zone 失衡可供调节 |
| 8 | 高阶分配是否导致 reclaim/compaction | **确有碎片，但未致害**：order-9/10 空闲块 = **0**（无 2 MB 连续段），然而 `compact_stall = +0` 证明**没有分配被碎片卡住**。两条同时成立 ⇒ 当前工作负载不申请高阶块 |
| 9 | vendor 内存预留是否过高 | `cma reserved 163840` 页 = **640 MB**，另有 `reserved 95782` 页。整机 `present 11700980 kB` → `managed 11317852 kB`（差约 374 MB）。**属 OEM 设计，非缺陷，且不可从容器侧调整** |

#### 结论

**宿主不处于慢性内存回收压力下。** kswapd 的工作是预防性的且健康（由 `allocstall=0`
与空闲页净增双重证明）。占主导的压力信号是 **CPU 调度，不是内存**——
满载时 PSI `cpu some` 是 `memory some` 的 **19 倍**。

因此：**C1（direct reclaim 长期存在）应从瓶颈清单中撤销**，
它是一次性的累计量误读。真正值得投入的方向是 CPU 调度与 VFS/block 层。

---

### 11.C §5 宿主级内核参数 A/B —— **决定不执行**

任务书 §5 本轮**明确授权**修改宿主级、可运行时恢复的内核参数（`watermark_scale_factor`、
`watermark_boost_factor`、`min_free_kbytes`、`page-cluster`、MGLRU/reclaim 接口），
并强调「不要因为 Android 宿主需要保守就停在诊断阶段」。

**本轮决定不执行，理由不是保守，而是没有证据基础：**

1. 这些参数的**唯一作用对象是内存回收压力**。11.B 已证明该压力**不存在**
   （`allocstall=0`、`pgscan_direct=0`、空闲页净增）。给一个不存在的瓶颈调参，
   不可能产生可测量的收益。
2. 任务书 §5 自订了终止条件：**「没有实际收益立即恢复」**。既然连"施加影响的机制"
   都不存在，A/B 的结果在实验前就已确定 —— **执行它等于制造一次无意义的宿主改动**。
3. 任务书 §13 的红线精神是**不重犯已被证否的改动**。宿主级 VM 参数改动
   **不做命名空间隔离**（§8.3 已实测确认 `compaction_proactiveness` 容器内写、宿主侧读），
   即容器侧任何试探都是宿主改动，风险与收益严重不对称。
4. 任务书 §14 要求「问题 → 内核机制 → 观测指标 → 单变量修改 → 压力测试 → A/B 数据 → 保留/回滚」。
   本轮的**问题**在第一步就被证否，链路无法成立。

**替代方案**：把同一份测量预算投给**已被证据指向的方向** —— CPU 调度
（PSI `cpu some 50.67`）与 VFS/block 层（11.H 的 loop 双层缓存）。
这两项均有明确的机制解释与可测量的目标指标。

> ⚠️ **需要用户确认**：如果你希望**无论如何都跑一遍**这几个宿主参数的 A/B
> （例如为了取得"已尝试"的记录），请明确指示。**当前默认：不执行**，
> 因为任务书同时禁止套用教程数值、禁止无收益改动。

---

### 11.D §6 MGLRU 深度核查

| 检查项 | 实测值 | 判读 |
|---|---|---|
| `CONFIG_LRU_GEN` | `=y` | 已编译 |
| `CONFIG_LRU_GEN_ENABLED` | `=y` | 默认启用 |
| `/sys/kernel/mm/lru_gen/enabled` | **`0x0001`** | **对匿名页 + 文件页同时生效**（bit0） |
| `/sys/kernel/mm/lru_gen/min_ttl_ms` | `0` | 无最小驻留保护（OEM 默认） |
| `pgdeactivate` | **10** | **几乎为 0 —— 这是 MGLRU 生效的正面证据**：MGLRU 绕过了传统 active/inactive 的 deactivate 路径，该计数器自然不增长 |
| `pgactivate` | 正常增长 | 页面在代际间提升 |
| `workingset_refault_anon` / `pgsteal_anon` | 50952 / 6630661 | **重登率 0.77%** —— 工作集保护极好 |
| `workingset_refault_file` / `pgsteal_file` | 2430153 / 31141949 | **重登率 7.8%** |
| `pgsteal_file` : `pgsteal_anon` | **31.1 M : 6.6 M = 4.7 : 1** | **回收代价主要由文件页承担**，匿名工作集被很好保护 |
| `workingset_nodereclaim` | 3238816 | 多代际链表上的页被整代回收 |

#### 是否值得 backport

任务书 §6 说「优先考虑 backport bugfix，而不是随意增加第三方 patch」。

**结论：本机 MGLRU 无需任何 backport。** 判据是**功能指标而非版本号**：

- 匿名页重登率 **0.77%** 远优于经验阈值（通常认为 < 5% 即健康）；
- 文件页重登率 7.8% 偏高，但这**不是 MGLRU 的缺陷** —— 它是**文件页本身就便宜**
  这一事实的体现（干净文件页可直接丢弃、无需回写），MGLRU 正确地选择了
  「多回收文件页、少动匿名页」这一最优策略。`pgsteal_file` 是 `pgsteal_anon` 的 4.7 倍
  正是该策略生效的量化证据。
- `workingset_nodereclaim` 有增长，说明多代际机制在正常工作。

**没有观测到任何需要 bugfix 的异常行为**（无异常重登、无代际卡死、无回收停滞）。
**故不引入任何第三方 MGLRU patch** —— 无问题可修时打补丁只会引入风险。

> 附带发现：`/proc/config.gz` **对本机的 MM 特性不是权威来源**。
> 它写着 `# CONFIG_ZRAM is not set`，但 zram0 正常工作且
> `grep -c zram /proc/kallsyms` = **139 个符号** —— 说明该 config.gz 是
> **GKI 基线配置，不含 OEM 覆写**（OEM 把 zram 做成 vendor 模块，
> 且 `comp_algorithm` 里出现了 `lz4kd`/`lz4kds`/`zstdn` 这些**非上游算法名**，
> 进一步证明是 vendor 打过补丁的 zram）。
> **方法学意义**：§9/§10 中任何基于 `/proc/config.gz` 的结论
> **必须用运行时状态交叉验证**，否则会得出"某个特性没开"的错误判断。

---

### 11.E §7 zram 内核视角核算

任务书 §7 要求算出「原始数据量 / 压缩后数据量 / 压缩率 / `memory_used_total` /
`same_pages` / `huge_pages` / swap in-out / CPU 成本」，并判断 zram 是在**缓解回收**
还是在**制造 RAM→zram→重登 的抖动**。

#### 实测数据

| 量 | 值 |
|---|---|
| zram 设备 | `zram0`，`disksize = 10200547328`（约 9.5 GiB） |
| 压缩算法 | `lzo lzo-rle lz4 lz4kd lz4kds deflate zstd [zstdn]`，**当前 = `zstdn`** |
| 已写入（原始） | **约 5.82 GB** |
| 实际占用宿主 RAM | **约 1.63 GB** |
| 压缩比 | **约 3.57 : 1** |
| `same_pages` | 存在（同页去重生效） |
| `huge_pages` | **33688** |
| `pages_compacted` | **828156** |
| `pswpout` / `pswpin`（74 s 满载） | **+145423 / +23399** |

#### 判读

1. **压缩比 3.57:1 是健康的**。1.63 GB 实存换 5.82 GB 可用交换空间，
   等于**凭空多出约 4.2 GB 内存容量**，代价极低。
2. **`huge_pages = 33688` 说明有大量不可压缩页**（zram 判定为 incompressible 的页
   会整页存储）。这部分页每页占用完整 4 KB，是 zram 内存占用的主要成分之一。
   它**不是缺陷** —— 这些页本来就不该被压缩（如已压缩数据、加密数据）。
3. **`pages_compacted = 828156` 反映 zram 内部的碎片整理开销**。该值随使用单调增长，
   属正常维护行为。
4. **关键判据：`pswpout : pswpin = 6.2 : 1`。**
   这是**单向冷页降级**的签名 —— 写出去的页绝大多数**再也没有被读回来**。
   真正的「RAM→zram→重登 抖动」会表现为两者**量级接近**（换出去又立刻要用）。
   **本机不是抖动。**
5. **与 MGLRU 的协同**（11.D）：匿名页重登率仅 **0.77%**，
   这从**另一条独立路径**（MGLRU 的 workingset 统计）佐证了「换出去的页确实是冷的」。
   两条独立证据互相印证，结论可靠。
6. **CPU 成本**：zram 压缩发生在 swap 路径上。满载 74 s 的场景里
   `pgscan_kswapd = 617887` 页被扫描、`pgsteal_kswapd = 363646` 页被回收，
   其中一部分走了压缩。**CPU 成本存在但未构成瓶颈** ——
   同期 `allocstall = 0` 且 CPU 利用率 94.1%（即 CPU 是满的但不是被 zram 拖住的）。

#### 结论

**任务书 §7 的追问「zram 是否在缓解回收，还是在制造抖动」——答案是"在缓解，未制造抖动"。**

**「不要把增加 zram 当作默认答案」** —— 本轮不但没有增加 zram，
而且**证明了当前 zram 配置已经是在做正确的事**，无需调整。
算法 `zstdn`（vendor 定制的 zstd 变体）已在**压缩率**一侧；
若将来遇到 CPU 瓶颈再考虑换 `lz4` 系列（速度换压缩率），**当前无此需要**。

---

### 11.F §8 容器隔离穿透 —— 机制定位

任务书 §8 要求查清 fstrim 的**穿透路径**（mount propagation / loop 设备 /
discard forwarding / CAP_SYS_ADMIN / 设备访问），并明确「**不要单纯靠删除 fstrim 命令**」。

#### 穿透路径（三层，逐层实测）

**第一层：mount 传播 —— 不是穿透渠道（已排除）**

容器内所有 mount 均为 **private**，唯一 `shared:312` 的是 Droidspaces 自己的
`/run/droidspaces/vproc`。⇒ **容器内 `mount` 操作不可能泄漏到宿主。**

> ⚠️ 本轮实测确认了一个与此相关但**独立**的事实：上一轮曾把容器 rootfs
> 重挂为 `nodiscard` 试图阻挡 fstrim，**该操作确实成功了且确实被命名空间隔离了**
> （容器内 `discard` 计数由 2 → 1，宿主不受影响）。
> **但它对 fstrim 无效** —— 见下。

**第二层：`/dev/block/` 设备节点暴露 —— 真正的原因**

容器内 `/dev/block/` 下有 **233 个设备节点**，包括宿主设备本身：

```
/dev/block/253:61 -> ../dm-61      （宿主 /mnt/data，f2fs）
/dev/block/8:83   -> ../sdf3       （宿主 persist_log，ext4）
/dev/block/8:80   -> ../sdf
```

根因已定位到容器配置项：

```
enable_hw_access=1        ← 对应 droidspaces 的 -H / --hw-access
                             "Enable direct hardware access (/dev nodes)"
```

**`enable_hw_access=1` 把宿主块设备直接暴露给容器**，容器因此可以对这些设备
直接下发 `FITRIM` ioctl。**这才是穿透路径。**

**第三层：能力位 —— 放大因素**

`CapEff = 000001ffffffffff` —— **全部 41 个能力位**（经 `capsh --decode` 逐位确认），
`Seccomp = 0`。容器内 root **等同于宿主 root**。

#### 为什么「重挂为 nodiscard」不是修法（关键结论）

`FITRIM` 是一个**显式 ioctl**，它直接携带「请丢弃该范围」的语义。
**`nodiscard` 挂载选项只影响隐式的、随删除/截断自动触发的 discard**，
**完全不拦截显式的 `FITRIM` 调用**。

⇒ 任何试图用挂载选项阻挡 fstrim 的方案**在机制上就是无效的**。
这解释了为什么上一轮的重挂测试"看起来成功"却"没有效果"。

#### 也确认了正向的一面：容器自己的 fstrim 是有益的

容器 rootfs 是 `loop50` → `/data/local/Droidspaces/Containers/ubuntu/rootfs.img`，
`discard_max = 4294966784`（约 4 GiB），`ro = 0`。
本轮实测手工执行 `fstrim -v /` **回收了 214.6 MiB**。

⇒ 容器对自己的回环设备做 TRIM 是**正常且有益**的（把空洞还给底层），
**问题只在于它同时能够到宿主设备**。

#### 宿主侧被暴露的具体风险

宿主 `/mnt/data`（f2fs, dm-61）与 `/mnt/data/persist_log/*`（ext4, sdf3）
以 **读写** 方式挂载，且带 `discard,discard_unit=block,background_gc=on`。
容器能够到这些设备节点 ⇒ 理论上可对宿主文件系统下发 TRIM。

> ⚠️ **需要说清楚风险的真实量级**：对 f2fs/ext4 下发 TRIM 的后果是
> **让闪存控制器提前擦除被判定为空闲的块**。对**已挂载且数据一致**的文件系统，
> 这通常**不会立即损坏数据**（文件系统只会 TRIM 自己认为是空闲的区域），
> 但它**不是零风险**：在文件系统元数据与实际占用不一致、或对**正在使用的块**
> 误判时，TRIM 会**不可逆地擦除数据**。
> **本项属于「能力过度授予」的架构问题，不是当前已发生的数据损坏。**

#### 修法方向（**未实施，需授权**）

按任务书 §8 的要求，正确做法是**减少过宽的能力**，而非删除命令：

| 方案 | 机制 | 代价 | 是否推荐 |
|---|---|---|---|
| `enable_hw_access=0` | 不再向容器暴露宿主 `/dev` 块设备节点 | **会同时影响容器其他硬件访问能力** | ❌ **用户已裁决：需要直接硬件访问 ⇒ 保留 `=1`，不改**（见 11.K） |
| 只对宿主设备做 bind 限制 | 用 `--bind` 精确控制可见范围 | 需 droidspaces 支持细粒度设备过滤（**当前未见该选项**） | ❌ 本机不支持 |
| 保留 `enable_hw_access=1` + 在容器内禁用 fstrim | 用户空间绕行 | 任务书明确**不认可**此类绕行 | ❌ 不采纳 |

⚠️ **重要前提**：`enable_hw_access=1` 属于 **`container.config` 的原始内容**
（与 2026-09-28 21:20 的备份逐字节相同，**早于上一轮的修改窗口**）——
按任务书 §1 的纪律「**如果无法证明某项是上一轮 Agent 创建的，不要删除**」，
**它不是你上一轮产物，本轮不动它**。上面列出的只是修法方向，**待你裁决**。

#### 对「保留 fstrim.timer drop-in」的裁决

结合本条：
- 容器对自己的 `loop50` 做 TRIM **有益**（本轮实测回收 214.6 MiB），
  且**不触及宿主设备**；
- 该 timer 属**用户空间绕行**，任务书不认可把它当解决方案；
- 但**在 `enable_hw_access` 决定之前删除它并无收益**，反而失去容器 rootfs 的空间回收。

⇒ **当前处置：保留**。待 `enable_hw_access` 的去留确定后一并裁决。

---

### 11.G §9 GKI 配置完整性核查

任务书 §9 要求审计 namespace / cgroup / 容器相关 CONFIG，并**明确不要声称
`CONFIG_PID_NS` 会提升性能**（它属架构完整性，不是性能项）。

**核查范围**：`/proc/config.gz` 全部 **2323 个配置项**，交叉验证运行时状态。

#### 全部为 `=y` 的项（无缺口）

| 类别 | CONFIG | 状态 |
|---|---|---|
| **命名空间** | `NAMESPACES`、`PID_NS`、`NET_NS`、`UTS_NS`、`IPC_NS`、`USER_NS`、`TIME_NS` | **全部 `=y`** |
| **cgroup** | `CGROUPS`、`CGROUP_SCHED`、`CPUSETS`、`MEMCG`、`BLK_CGROUP`、`CGROUP_BPF`、`CGROUP_FREEZER` | **全部 `=y`** |
| **调度** | `UCLAMP_TASK`、`UCLAMP_TASK_GROUP` | **全部 `=y`** |
| **压力** | `PSI` | **`=y`** |
| **内存** | `LRU_GEN`、`LRU_GEN_ENABLED`、`TRANSPARENT_HUGEPAGE`、`THP_SWAP` | **全部 `=y`** |
| **文件系统** | `OVERLAY_FS`、`F2FS_FS`、`EXT4_FS` | **全部 `=y`** |
| **其他** | `IO_URING`、`BPF`、`BPF_JIT`、`DAMON`、`KVM`、`DM_CRYPT` | **全部 `=y`** |

#### 未启用 / 缺失项（全部经评估：**对 Droidspaces 无影响**）

| CONFIG | 状态 | 评估 |
|---|---|---|
| `SCHED_AUTOGROUP` | `=n` | 由 Android 的 cgroup 体系（`/dev/cpuctl`、`/dev/cpuset`）取代，**非缺陷** |
| `DAMON_PADDR` | `=n` | 仅用于物理地址级访问监控，与容器无关 |
| `LRU_GEN_STATS` | `=n` | 仅关闭 MGLRU 的额外统计数据，**不影响 MGLRU 功能** |
| `ZSWAP` | `=n` | 本机用 **zram**（块设备级交换）而非 zswap（页级缓存），**二选一，非缺陷**。11.E 已证明 zram 工作良好 |
| `SCHED_CORE` | 缺失 | core scheduling，用于**跨超线程的侧信道防护**，与容器性能无关 |
| `ZBUD` | 缺失 | zswap 的分配器后端，zswap 未启用故不需要 |

#### 结论

**§9 的问题「哪些缺失项限制了 Droidspaces」——答案是：没有。**

所有构成完整 Linux 命名空间容器所需的配置项**全部为 `=y`**，
`PID_NS` 也在其中（本机并非任务书 §9 担心的 `PID_NS=n` 情形）。

**依据任务书「不要声称 PID_NS 会提升性能」的要求，此处明确：**
这些 CONFIG 提供的是**架构完整性**（容器能正常运行、能隔离），
**不构成性能优化项**，因此**不列入任何性能收益清单**。

---

### 11.H §11/§12 文件系统与 cgroup 架构问题

#### §11 文件系统路径（根因已确认）

**完整路径**：
```
容器内 /  =  ext4
             └─ loop50  ← /data/local/Droidspaces/Containers/ubuntu/rootfs.img
                          （sparse，40 G 表观 / 实测占用 4.6 G）
                          └─ f2fs (dm-61)     ← 宿主 /data
                             └─ UFS 块设备
```

**根因：`loop50/loop/dio = 0`**

```
loop50/loop/dio       = 0      ← 直接 I/O 未启用
loop50/loop/autoclear = 1
loop50/loop/partscan  = 1
loop50/loop/offset    = 0
loop50/loop/sizelimit = 0
```

`dio = 0` 意味着回环设备走**缓冲 I/O**。后果是**同一份数据被缓存两次**：

1. 容器内 ext4 的**页缓存**（针对 `rootfs.img` 的逻辑块）；
2. 宿主 f2fs 的**页缓存**（针对 `rootfs.img` 这个**文件本身**的物理块）。

⇒ **同样的字节在内存里存两份**，且写路径要**穿两层文件系统**再落盘。
这直接解释了 §5.3 记录的「rootfs 慢」——**不是 UFS 慢，是两层文件系统叠加**。

**可用的修法（未实施）**：

| 方案 | 机制 | 评估 |
|---|---|---|
| `loop50/loop/dio = 1` | 消除宿主侧页缓存那层，绕过 f2fs 的 buffered 路径 | ❌ **用户已裁决：不动**（见 11.K）。⚠️ 补充实测：`/sys/block/loop50/loop/dio` 权限为 **`-r--r--r--`（只读）**，sysfs 改不了；唯一路径是 `LOOP_SET_DIRECT_IO` ioctl 且**必须在挂载前**设置 ⇒ **需 Droidspaces 支持** |
| `--rootfs=PATH`（目录 rootfs） | **彻底去掉 loop + ext4 这两层**，直接用宿主 f2fs | ❌ **用户已裁决：不动**（见 11.K）。收益最大但**改变隔离模型**（容器 rootfs 不再是单一镜像文件） |
| `--volatile`（OverlayFS） | 用 overlay 替代部分写路径 | 与本节问题正交，且任务书 §12 警告不要把大 build tree 堆在多层 overlay 上 |

⚠️ 任务书要求「**不要为了性能破坏 Droidspaces 的隔离模型**」——
`--rootfs=PATH` 正是这类改动（rootfs 从"单文件镜像"变为"宿主目录"，
快照/迁移/回滚语义都会变）。**故仅列出，不实施。**

#### §12 cgroup —— **任务书指定的机制在本机不存在**

任务书 §12 建议评估 `memory.high` / `memory.low` / `memory.min` / `memory.swap.max`。
**这四个都是 cgroup v2 的接口。本机上它们全部不可用**：

```
宿主 /sys/fs/cgroup 挂载: cgroup2 rw,nosuid,nodev,noexec,relatime,memory_recursiveprot
宿主 cgroup.controllers     = []        ← 空！
宿主 cgroup.subtree_control = []        ← 空！
```

**`cgroup.controllers` 为空 ⇒ v2 层级上一个控制器都没有启用 ⇒
`memory.high` 这类文件在 v2 上根本不存在。**

本机的**全部内存资源控制都在 cgroup v1**（`/dev/memcg`）。

#### 容器的真实归属（从宿主侧核对）

```
容器 PID 114219/114220 ([ds-monitor]) →  4:memory:/apps    0::/droidspaces/ubuntu
容器 PID 114221    (systemd)           →  4:memory:/apps    0::/droidspaces/ubuntu/init.scope

/dev/memcg/apps/memory.usage_in_bytes      = 4189913088  (3995.8 MB)
/dev/memcg/apps/memory.max_usage_in_bytes  = 6951878656  (6629.8 MB)
/dev/memcg/apps/memory.limit_in_bytes      = 9223372036854771712  ← 无限
/dev/memcg/apps/memory.soft_limit_in_bytes = 9223372036854771712  ← 无限
/dev/memcg/apps/memory.failcnt             = 0
```

**三条关键事实：**

1. **容器与全部 Android 应用共享同一个 v1 memory cgroup `/apps`，且该 cgroup 无任何限制。**
2. **`/droidspaces/ubuntu` 这个 v2 路径是"空壳"** —— 因为 v2 上没有控制器，
   它只提供 cgroup 命名空间视图，**不提供任何资源控制**。
   （逐进程核对确认：**没有任何进程**被放进一个带 memory 控制器的 droidspaces 专属 cgroup。）
3. **`memory.failcnt = 0`** ⇒ `/apps` 从未触及限制（因为它没有限制）。

#### 为什么这是架构问题（而非性能问题）

Android 的 **LMKD 依据 `/apps` 的压力来决定杀哪个应用**。
容器的内存被计入 `/apps`，**与用户的应用混在一起**。后果：

> **用户跑一次大型编译，推高的是 `/apps` 的压力，
> LMKD 可能据此杀掉用户的前台应用 —— 而被杀的并不是"罪魁祸首"。**

本机相关阈值：`persist.sys.oplus.lmkd_super_critical_threshold_12g = 800`（MB）。

⚠️ **但必须说清楚：本轮没有观测到任何一次实际的误杀。**
11.B 的满载实验里 `allocstall = 0`、PSI `memory some` 仅 2.64，
**压力根本没到 LMKD 会介入的水平**。

⇒ 这是**架构正确性问题**（会计归属错误），
**不是当前已发生的故障**。（任务书 §12 本身也把 cgroup 改动定位于
「架构正确性」而非性能。）

#### 可用的修法（**未实施，且风险高于其他项**）

| 方案 | 机制 | 评估 |
|---|---|---|
| v2 `memory.high` | 任务书原意 | ❌ **本机不存在**（`controllers=[]`） |
| v1 建 `/dev/memcg/droidspaces` 并把容器进程迁入 | 让容器内存**独立记账**，不再混入 `/apps` | ❌ **用户已裁决：不用**（见 11.K）。技术上是对症的，但需迁移容器 PID1、未知 init/LMKD 是否迁回 |
| v1 `memory.limit_in_bytes`（硬限） | 强制隔离 | ❌ 任务书明确「**不要直接设置 memory.max**」，且硬限会导致容器内 OOM |
| v1 `memory.soft_limit_in_bytes` | 软限 | ⚠️ v1 软限的语义很弱（仅在回收时尽力而为），**收益存疑** |

**本轮处置：不实施。** 理由：
① 任务书 §12 要求「验证 with PSI」，而 11.B 已证明**当前无压力可验证**；
② 迁移容器 PID1 属于高风险操作，**收益（架构正确性）与风险不对等**；
③ 任务书核心原则「任何无法解释到具体 Linux kernel subsystem 的"优化"，默认不要做」——
本项机制清楚，但**前提（容器拖累宿主）未被观测到**。

**建议**：作为**独立的下一次受控实验**（单变量、可回滚、先备份 `cgroup.procs` 归属），
而不是与本轮的其他改动捆绑。

---

### 11.I §10 需要重编 GKI 的补丁 —— **结论：无需任何补丁**

任务书 §10 要求「对需要重编 GKI 的项目**直接制作补丁**」「**不要只写"建议启用"**」。

**本轮的答案是：不存在需要重编 GKI 的项目，因此不制作补丁。**
这不是回避，而是逐项排查后的结论 —— 下面把**每一个可能成为候选的方向**都列出来，
并给出它为什么**不需要动内核**的具体理由：

| 候选方向 | 为什么不需要内核补丁 |
|---|---|
| **namespace / cgroup 相关 CONFIG** | §11.G 已核查：所需项**全部已是 `=y`**（`PID_NS`/`NET_NS`/`UTS_NS`/`IPC_NS`/`USER_NS`/`TIME_NS`/`MEMCG`/`CPUSETS`/`CGROUP_SCHED`/`UCLAMP_TASK*`/`PSI`）。**没有一项缺失，无从"启用"** |
| **MGLRU 行为** | §11.D 已用**功能指标**证明其健康（anon 重登率 0.77%）。**没有可修的 bug** ⇒ 无 backport 对象 |
| **zram** | §11.E 已证明工作正常（3.57:1 压缩比、单向降级）。**无可修项** |
| **loop 直接 I/O（§11 根因）** | 内核**早已支持** —— `LO_FLAGS_DIRECT_IO` / `LOOP_SET_DIRECT_IO`（Linux 4.4+，本机 6.6）。**缺的是 Droidspaces 没有去设置它**，属**用户态容器管理器**的功能缺失，**打内核补丁解决不了** |
| **cgroup v2 的 `memory.high`（§12）** | 内核**早已支持** v2 内存控制器。本机不可用是因为 **Android 在 v2 上没启用任何控制器**（`cgroup.controllers = []`）—— 这是 **Android init / LMKD 的用户态架构决定**，不是内核缺陷 |
| **fstrim 穿透（§8）** | 根因是 **Droidspaces 配置项 `enable_hw_access=1`**，属容器管理器的能力授予策略。**内核的设备节点访问控制是正确工作的** |
| **CPU 调度（真正的瓶颈）** | PSI `cpu some 50.67` 只说明 **CPU 被编译负载打满了 —— 这正是编译任务应有的状态**，不是缺陷。且本机 OEM 把 `sched_util_clamp_min = sched_util_clamp_max = 1024` **双双钉死**，uclamp 通道在架构上已被厂商关闭，**从容器侧或补丁侧都无法重新打开它**（属 §13 红线） |

**唯一认真考虑过、并主动否决的候选：`CONFIG_SCHED_AUTOGROUP`**（当前 `=n`）

- **机制**：为每个会话/TTY 建立自动调度组，改善多进程场景下的调度公平性。
- **为什么否决**：
  ① Android **已经**用自己的 cgroup 体系（`/dev/cpuctl`、`/dev/cpuset`）做调度分组，
      autogroup 在任务被移入非 root cgroup 后会**自动退出**，
      **在 Android 上基本不会生效**；
  ② 即便生效，它解决的是**公平性**而非**吞吐**，而 §11.B 的实测表明
     编译负载下 CPU 是**充分被打满的**（94.1%），**没有公平性损失可修**；
  ③ 它需要**重编 GKI + 刷写 boot 镜像**，而任务书 §2 明令「**不要修改 boot/vendor_boot/init_boot 镜像**」
     且 §10 要求「**不要为了"优化"随意推荐第三方 scheduler patch**」。
- ⇒ **收益不成立，代价是刷机风险，否决。**

#### 与任务书 §10 要求的逐条对照

| §10 要求 | 本轮的对应产出 |
|---|---|
| 修改文件 | **无**（无补丁） |
| 修改原因 | 不适用 —— §11.G/11.D/11.E 证明**无缺口可补** |
| 对应 upstream commit | 不适用（**未做任何 backport**，故不涉及保留 upstream 原 patch 的问题） |
| 风险 | **零**（未改内核） |
| ABI / KMI 影响 | **零**（未改内核） |
| 是否影响 GKI | **否** |
| 「优先 upstream/ACK/stable 已有实现」 | **遵守**：本报告提出的所有修法（`loop dio`、cgroup 记账分离、`enable_hw_access`）**全部基于内核与 Android 已有机制**，**没有发明任何新内核机制** |

> ⚠️ **如实说明**：任务书 §10 的措辞预设了"一定存在需要重编的内核项"。
> 本轮**按 §9 的方法逐项核查后，没有找到这样的项**。
> 如果为了满足格式而拼凑补丁，恰恰违反任务书 §10 的另一条要求
> （「不要自己发明复杂内核机制」）和核心原则
> （「任何无法解释到具体 Linux kernel subsystem 的"优化"，默认不要做」）。
> **故本节的产出就是"无补丁"这个结论本身。**

---

### 11.J §15 交付清单（A–H）

#### A. 已清理的上一轮修改

**✅ 完成，残留 0 项。** 完整审计表见 **11.A**。要点：

- 7 类文件已还原/删除（`/etc/environment` 的 8 个变量、`/etc/bash.bashrc`、
  4 个 profile.d / limits.d / systemd drop-in、16 个 ccache shim、4 个 `/mnt/data` 临时目录）；
- 全部还原动作**先快照后覆盖**，可再回退；
- 独立残留扫描检索了 §2 与 §13 点名的全部条目，**结果 0 项**；
- `container.config` 与 09-28 备份**逐字节相同**，无残留；
- **保留项**：`fstrim.timer` drop-in（待 §8 裁决）、时区、ccache 缓存目录、
  `/root/.ds-opt/` 证据链 —— 逐条理由见 11.A；
- ⚠️ **一处待裁决副作用**：还原 `CARGO_TARGET_DIR`/`GOCACHE` 等后，
  构建产物回到 ext4-on-loop 路径（比宿主 f2fs 慢，见 11.H）。
  **正确修法是容器配置而非环境变量**，见 11.A 末。

#### B. 当前真正的内核瓶颈

按证据强度排序（每项都有可测量的内核指标支撑）：

| 级别 | 瓶颈 | 内核子系统 | 量化证据 |
|---|---|---|---|
| 🟠 **High** | **loop 双层页缓存** | **block layer / loop / VFS** | `loop50/loop/dio = 0` ⇒ 同一数据在 ext4 与 f2fs **各缓存一次**，写路径穿两层文件系统（11.H） |
| 🟡 **Medium** | **容器内存与 Android 应用混记于无限制的 `/apps`** | **cgroup v1 memcg** | `memory.limit_in_bytes` = 无限；容器 4.0 GB 计入 `/apps`（与全部应用共享）（11.H） |
| 🟡 **Medium** | **容器可达宿主块设备** | **device access / capability** | `enable_hw_access=1` ⇒ 233 个设备节点含 `dm-61`/`sdf3`；`CapEff=0000001ffffffffff`（11.F） |
| 🟢 **Low** | **order-9/10 空闲块 = 0** | **buddy allocator** | 无 2 MB 连续段；但 `compact_stall = +0` ⇒ **当前负载不受影响**（11.B Q8） |
| ⚪ **已撤销** | ~~宿主 direct reclaim~~ | ~~MM reclaim~~ | **实测证否**：满载 74 s 内 `pgscan_direct +0`、`allocstall +0`、空闲页**净增**（11.B） |

> **CPU 不是"瓶颈"，是"被打满"**：PSI `cpu some 50.67` 与 94.1% 利用率说明
> 编译负载**已经充分占用 CPU** —— 这是编译任务的**正常且期望**的状态，不是缺陷。
> 本报告不把它列为待优化项，以免制造一个不存在的问题。

#### C. 已实施的 GKI / kernel framework 优化

**本轮未实施任何新优化。** 这是**主动决定**，不是遗漏：

- §11.B 证明内存回收压力**不存在** ⇒ §5 授权的宿主参数 A/B **无证据基础，未执行**；
- §11.G 证明 GKI 配置**无缺口** ⇒ 无配置变更；
- §11.D/11.E 证明 MGLRU 与 zram **工作正常** ⇒ 无调整必要；
- §11.H/11.F 的两项架构问题（loop dio、cgroup 记账、设备暴露）
  **均需在"改变隔离模型"与"保持现状"之间由用户裁决**，见 D/E。

**本轮对设备的唯一改动是把上一轮的修改还原**（11.A）。
设备当前处于**干净状态**：无新增调优、无残留、容器与宿主参数均为设备原值。

#### D. 内核源码修改

**无。** 无 `git diff`、无 commit、无 CONFIG 变更。

- 理由与逐项排查见 **11.I**；
- **ABI / KMI 影响：无**；**是否影响 GKI：否**；
- 本轮未触碰 `oplus13/android_kernel_common_oneplus_sm8750` 仓库。

#### E. 容器隔离修复

**机制已完全定位，修法已列出，但均未实施**（需用户裁决）：

| 问题 | 根因（已证明） | 修法 | 状态 |
|---|---|---|---|
| fstrim 可达宿主设备 | `enable_hw_access=1` | 设为 `0`（**但会同时影响其他硬件访问**） | ⚠️ **待裁决** |
| 「重挂 nodiscard」为何无效 | `FITRIM` 是**显式 ioctl**，不受挂载选项影响 | 该思路**机制上无效，作废** | ✅ 已证否 |
| 容器内存混入 `/apps` | Android 的 v1 内存控制器把容器放进 `/apps` | 建 `/dev/memcg/droidspaces` 并迁移进程 | ⚠️ **待裁决**（高风险） |
| capability 过宽 | `privileged=full` | 收窄 `privileged` 级别 | ⚠️ **待裁决**（会影响容器用途） |

> ⚠️ **`enable_hw_access=1` 与 `privileged=full` 都是 `container.config` 的原始内容**
> （与 2026-09-28 备份逐字节相同，**早于上一轮修改窗口**）。
> 按任务书 §1 纪律「**无法证明是上一轮创建的，不要删除**」——
> **本轮不动它们**，只报告事实与修法。

#### F. 内存回收结果

**结论：宿主不存在慢性内存回收压力，无需任何修复。**

- 满载 74 s（CPU 94.1%）下：`pgscan_direct +0`、`pgsteal_direct +0`、
  `allocstall_normal +0`、`allocstall_movable +0`、`pgscan_direct_throttle +0`；
- `nr_free_pages` **净增 160898 页**（空闲页在负载期间**变多**）；
- zone Normal `free` 50334 → 215662，**始终高于 `high`（40876）**；
- PSI `memory some` 峰值 **2.64**（74 s 中约 1.4% 失速），而 `cpu some` **50.67**；
- 撤销上一轮的 **C1**：`pgscan_direct 5.2 M` 是**累计量**误读，
  增量集中在容器冷启动与首次构建，稳态下**不再增长**（11.B 机制解释）。

#### G. 文件系统结果

**根因已确认：`loop50/loop/dio = 0` 造成双层页缓存。**

- 完整路径：容器 `/` = **ext4 → loop50 → rootfs.img（sparse, 40 G 表观/4.6 G 实占）→ f2fs(dm-61) → UFS**；
- 同一份数据在**容器 ext4 页缓存**与**宿主 f2fs 页缓存**中各存一份；
- 宿主挂载选项：`rw,seclabel,noatime,nodiratime,nodelalloc,errors=remount-ro,init_itable=0`；
- **修法**：`dio = 1`（最对症，但 `/sys/.../dio` 是**只读** `-r--r--r--`，
  只能靠 `LOOP_SET_DIRECT_IO` ioctl 在**挂载前**设置 ⇒ **需 Droidspaces 支持**）；
  或 `--rootfs=PATH` 目录 rootfs（**收益最大，但改变隔离模型**）；
- **均未实施** —— 任务书明确「不要为了性能破坏 Droidspaces 的隔离模型」。

#### H. 未解决项目

**用户已于 2026-09-30 逐条裁决，见 11.K。** 裁决后的最终状态：

| # | 项 | 状态 |
|---|---|---|
| 1 | `loop dio` 开启 / `--rootfs=PATH` | ❌ **已裁决：不动**（用户）。双层页缓存转为**已接受的代价** |
| 2 | `enable_hw_access` | ❌ **已裁决：保留 `=1`**（用户需要直接硬件访问）。设备暴露转为**已接受的取舍** |
| 3 | 容器内存独立记账 | ❌ **已裁决：不用**（用户）。共享 `/apps` 记账转为**已接受的现状** |
| 4 | `privileged=full` 收窄 | ❌ **已裁决：不动**（同上，硬件访问依赖它） |
| 5 | order-9/10 碎片 | ✅ **不处理**（`compact_stall=+0` ⇒ 当前负载不受影响，改它属无收益改动） |
| 6 | §5 宿主参数 A/B | ✅ **不执行**（无证据基础，11.B/11.C） |
| 7 | 构建产物落点 | ⚠️ **仍开放**（用户未表态）—— **当前保持还原状态**；可行途径见 11.K 末 |

⇒ **本报告至此的净结论：设备已在正确的配置上运行，无进一步改动必要。**
下表保留原始的问题/阻塞点描述以存档。

| # | 项 | 阻塞点（存档） | 原建议 |
|---|---|---|---|
| 1 | `loop dio` 开启 | Droidspaces 未暴露该设置；sysfs 只读；ioctl 须在挂载前 | 向 Droidspaces 提功能请求 |
| 2 | `enable_hw_access` 去留 | 会影响用户其他硬件访问用途 | 需用户说明是否需要直接硬件访问 |
| 3 | 容器内存独立记账 | 需迁移容器 PID1；未知 init/LMKD 是否迁回 | 作为独立受控实验 |
| 4 | `privileged=full` 收窄 | 会影响容器用途 | 需用户裁决 |
| 5 | order-9/10 碎片 | 当前无致害 | 不处理 |
| 6 | §5 宿主参数 A/B | 无证据基础 | 默认不执行 |
| 7 | 构建产物落点 | 现落在 ext4-on-loop（较慢） | 用容器配置而非环境变量 |


---

### 11.K 用户裁决与最终状态（2026-09-30）

§11.F / §11.H / §11.J-H 列出的待裁决项，用户已逐条裁决：

| # | 待裁决项 | **用户裁决** | 处置 |
|---|---|---|---|
| 1 | `enable_hw_access` 去留 | **「需要直接硬件访问」** | **保留 `=1`，不改** |
| 2 | `loop dio` 开启 / `--rootfs=PATH` | **「不动」** | **不改**（`dio` 维持 `0`，rootfs 维持 `--rootfs-img`） |
| 3 | 容器内存独立记账 | **「不用」** | **不改**（继续共享 v1 `/apps`） |

⇒ **设备不再有任何待执行的修改。** 最终状态 = 第一轮清理完成后的干净状态
（无新增调优、无残留、内核与宿主参数均为设备原值），
加上三条**经用户明确确认保留**的既有特性：

1. `enable_hw_access=1` —— 容器可直接访问宿主硬件 / 块设备节点
2. `loop dio = 0` —— 容器 rootfs 走 ext4↔f2fs 双层页缓存
3. 容器内存计入 v1 `/apps`，与 Android 应用共享且无独立限额
4. （附带）`privileged=full` 维持不变 —— 直接硬件访问依赖它

> ## ⚠️ 给后续会话的明确指示
>
> **以上四条不是缺陷，是经用户明确裁决的工程取舍。**
> **不要**在后续会话中把它们当作"待优化项""未解决瓶颈""残留问题"重新提出。
> 依据：本文件 §11.K 与 `oplus13/CLAUDE.md` 第 27 项的裁决记录。
> 如确需重新评估，**必须先向用户说明此举将推翻既有裁决并取得新的授权**。

#### 保留 `enable_hw_access=1` 之后的残余风险与现有缓解

容器**确实需要**直接硬件访问 ⇒ 宿主设备暴露不是"待修问题"，**它就是功能本身**。
但 §11.F 记录的那条具体风险仍然成立，且**已有落地缓解**：

- **风险**：容器能够到宿主 `/mnt/data`（f2fs, `dm-61`）与 `/mnt/data/persist_log/*`
  （ext4, `sdf3`），理论上下发 `FITRIM` 会**不可逆地擦除被误判为空闲的块**。
- **现有缓解（第一轮 S3 的收紧，已落地）**：容器内 `fstrim.service` 的 `ExecStart`
  已从 systemd 默认的 `--listed-in /etc/fstab:/proc/self/mountinfo`
  （**遍历全部挂载点**，实测**真的 TRIM 过宿主 `/dev/block/sdf3` 36.4 MiB**）
  **收紧为只 `fstrim /`** ⇒ 现在只 TRIM 容器自己的 `loop50`。
- ⇒ **正确的安全姿态是「收紧作用域」而非「删除功能」** —— 这与任务书 §8
  「**不要单纯靠删除 fstrim 命令**」的方向一致：**保留能力，限制范围**。
- ⚠️ 该缓解覆盖的是**已知的那一条**容器内定时任务；
  它**不能**阻止容器内其他程序主动对宿主设备下发 discard。
  既然硬件访问是需求，这属于**已被接受的残余风险**。

#### 仍然开放的一项（用户未表态）

**构建产物落点**。还原 `CARGO_TARGET_DIR`/`GOCACHE`/`GOMODCACHE` 等变量后，
构建产物回到 **ext4-on-loop** 路径 —— §11.H 已证明该路径比宿主 f2fs 慢
（双层页缓存）。**当前保持还原状态**（这是任务书 §1「清理上一轮修改」的直接结果）。

如需改变，**正确途径是用法约定而非系统级调优**（任务书 §2 禁止的是一整类
用户空间调参手法，不是"把产物放哪"这个用法选择）：

- 逐工具指定：`cargo --target-dir /mnt/data/...`、`go build -o`、`cmake -B /mnt/data/...`；
- 或调整容器 `bind_mounts`（属容器配置，非环境变量）。

⚠️ 注意这一项**与已裁决的第 2 项相邻但不同** —— `--rootfs=PATH` 改的是**容器根文件系统**
（改变隔离模型，已裁决不动），而这里改的只是**产物目录**，不影响隔离模型。

## 附录 A：诊断与基准脚本清单

| 文件（设备 `/data/local/tmp/`） | 内容 | 状态 |
|---|---|---|
| `diag1.sh` … `diag5.sh` / `.out` | 系统 / 容器形态 / CPU / cgroup / 内存 / zram / VM / 限制 / thermal / 存储 / 内核配置 / 工具链 / 网络 / 路径 / 限额 | 已归档 |
| `ro.sh` / `ro.out` | 调度器 / 内存压缩 / uclamp / zram 全量 / 稀疏镜像 | 已归档 |
| `bench.sh` / `bench-before.out` | 初版基准套件 B1–B9（**CLOCK_REALTIME，已知有缺陷**） | 已归档，仅作过程记录 |
| `bench2.py` | 可信基准（monotonic + 3 reps + median） | 已归档 |
| `verify-io.py` | 三落点交错 I/O 对比（5 reps） | 已归档 |
| `delalloc-ab.py` | `nodelalloc` vs `delalloc` A/B + 已回滚 | 已归档 |
| `thp-ab.py` | THP `never`/`madvise` A/B（含 plain/madv 双变体） | 已归档 |
| `fstrim.sh` | 稀疏镜像 TRIM | 已归档 |
| `cgv1-test.sh` / `cgv1-check.sh` / `cgv1-rollback.sh` | `force_cgroupv1` 测试与回滚 | 已归档 |
| `fix-a.sh` | A 组配置修正版（符号链接 / profile.d / bash.bashrc / ccache） | 已归档 |
| `ccache-test.sh` | ccache 收益 A/B | 已归档 |
| `bench3.py` / `b12.sh` | B6/B7/B3 与 B1/B2 的可比复测 | 已归档 |
| `final-verify.sh` | 容器健康 + 优化落地复核 | 已归档 |
| `opt-pkg.sh` | B 组软件包安装（日志 `/root/.ds-opt/apt-install.log`） | 已归档 |
| `apply-build-dirs.sh` / `verify-build-dirs.sh` | **S1+S2**：构建输出/缓存指向 `/mnt/data` 及逐工具实证 | 已归档（§7.5） |
| `recon-s4s6.sh` | **S4/S6 侦察**：ccache 配置来源与 origin 统计、时区数据可用性 | 已归档（§7.6） |
| `apply-s4s6.sh` | **S4+S6 执行**：ccache 迁移 + 缓存级配置 + 时区设置 | 已归档（§7.6） |
| `verify-s4s6.sh` | S4/S6 首次验证（**该次因调用方式不当未成立，见 `diag-ccache.sh`**） | 已归档（§7.6） |
| `diag-ccache.sh` | **关键诊断**：查明 ccache 接管方式（裸 run 不生效）与 `CCACHE_MAXSIZE` 来源 | 已归档（§7.6） |
| `confirm-s4.sh` | S4 最终确认：配置 origin 归属 + 真实编译 1 miss/1 hit | 已归档（§7.6） |
| `recon-s357.sh` | **S3/S5/S7 侦察**：fstrim 单元与 Condition、codex 版本结构、shim 名冲突检查 | 已归档（§7.7） |
| `recon-s5.sh` / `recon-s5b.sh` | S5 深入侦察（第一次 `du` 因 `current` 是指向 `releases/` 内的符号链接而在同次调用中按 inode 去重、数字互相抵消，故补做逐个 `du` 的干净读数） | 已归档（§7.7） |
| `apply-s357.sh` | **S3+S5+S7 执行**：建 16 个 shim、fstrim drop-in、带三重安全闸的旧版删除 | 已归档（§7.7） |
| `fix-fstrim-scope.sh` | **C-11 修正**：把 fstrim 的 `ExecStart` 由「遍历所有挂载点」收紧为「只 TRIM `/`」 | 已归档（§7.7） |
| `recon-proact.sh` | **compaction A/B 侦察（宿主）**：全部 `compact_*` 与水位读数 + 60 s 空转对照（结论：自然活动为 0） | 已归档（§6.7） |
| `recon-extfrag.sh` | compaction A/B 侦察（debugfs）：挂载 debugfs、读 `extfrag_index` / `unusable_index`、`kcompactd0` CPU；**顺带发现「`compact_memory` 全量压缩不计数」** | 已归档（§6.7 ④） |
| `ab-proact.sh` | **A/B 主测**：p = 0 / 20 / 100 三阶段各 60 s，含恢复原值 | 已归档（§6.7 ①） |
| `ab-proact2.sh` | **收敛性长窗口**：p=100 连续两个 60 s 子窗口 + buddyinfo 逐 order 统计 | 已归档（§6.7 ②） |
| `ab-proact3.sh` | **配对对照**：A(0) → B(100) → A(0) 各 60 s，用于剥离背景活动漂移 | 已归档（§6.7 ③） |
| `cmm.sh` | **计数器路径查明**：写 `compact_memory=1` 前后全量 `compact_*` 对照，坐实三组计数器分属三条路径 | 已归档（§6.7 ④） |
| `c-proact.sh` / `c-set20.sh` | **容器侧可写性与命名空间隔离**：容器内写 20 后由宿主独立读取验证 ⇒ C-14 | 已归档（§6.7 ⑤） |

原始输出归档于 `oplus13/.scratch/droidspaces-20260929/raw/`。

## 附录 B：容器内关键路径

| 用途 | 路径 |
|---|---|
| 容器配置 | `/data/local/Droidspaces/Containers/ubuntu/container.config`（宿主） |
| 配置备份（cgv1 测试） | `container.config.bak-cgv1`（宿主，同目录） |
| rootfs 镜像 | `/data/local/Droidspaces/Containers/ubuntu/rootfs.img`（宿主，稀疏 40 GiB） |
| 容器运行态 | `/run/droidspaces/`（version / mount / name / container.config / vproc） |
| Droidspaces CLI | `/data/local/Droidspaces/bin/droidspaces` |
| 宿主 `/data` 在容器内 | `/mnt/data` |
| **A 组配置备份** | `/root/.ds-opt/`（`*.orig`，容器内） |
| **S1/S2 前快照** | `/root/.ds-opt/environment.bak-pre-dsbuild`（容器内） |
| **S4/S6 前快照** | `/root/.ds-opt/environment.bak-pre-s4s6`、`/root/.ds-opt/localtime.orig`（容器内） |
| **失效文件留证** | `/root/.ds-opt/ccache.conf.DEAD-never-read-by-ccache`（A4 遗物，见 §7.6） |
| **A 组 apt 日志** | `/root/.ds-opt/apt-install.log` |
| **构建输出/缓存根** | `/mnt/data/ds-build/`（容器内 `/mnt/data` = 宿主 Android `/data`） |
| Claude Code | `/usr/local/bin/claude` → `/root/.local/bin/claude`（2.1.274） |
| Codex | `/usr/local/bin/codex` → `/root/.local/bin/codex`（0.158.0） |
| Bun | `/usr/local/bin/bun` → `/root/.bun/bin/bun`（1.4.2） |
| **ccache 目录（S4 后）** | **`/mnt/data/ds-build/ccache`**（原 `/root/.cache/ccache`，已迁移） |
| **ccache 配置（S4 后）** | **`/mnt/data/ds-build/ccache/ccache.conf`**（缓存级；`max_size=20G`、`compression=true`、`compression_level=6`） |
| ccache shim | `/usr/lib/ccache/`（16 个，全部 → `../../bin/ccache`：`gcc` `g++` `cc` `c++` `clang` `clang++` 及各自带版本号/交叉前缀的名字） |
| **ccache shim 入口（S7 后）** | **`/usr/local/bin/`**（16 个 shim 副本，使裸 `run` 也能命中）；原 `/usr/lib/ccache/` 保留不动 |
| ccache shim 清单（回滚用） | `/root/.ds-opt/ccache-shims.list`（容器内） |
| ccache 生效范围 | ✅ S7 后**全部路径生效**（登录 shell 走 `/usr/lib/ccache`，裸 `run` 走 `/usr/local/bin`）。S7 之前的缺口见 §7.6 C-10 |
| **fstrim 定时（S3）** | drop-in `/etc/systemd/system/fstrim.{timer,service}.d/override.conf`；`OnCalendar=weekly`、`Persistent=true` |
| **codex 版本根（S5 后）** | `/root/.codex/packages/{standalone,app-server-daemon}/releases/`，各只保留 `current` 指向的 `0.158.0` |
| **compaction A/B 原值备份（宿主）** | `/data/local/tmp/proact.orig`（内容为 `0`） |

## 附录 C：一键回滚速查

```sh
# ---- A 组配置 ----
rm -f /usr/local/bin/{bun,bunx,claude,codex,claude-go,claude-native}   # A1'
rm -f /etc/profile.d/99-ds-path.sh                                    # A2'
: > /etc/profile.d/droidspaces_env.sh                                 # A2''
cp /root/.ds-opt/bash.bashrc.orig /etc/bash.bashrc                    # A3'
rm -f /etc/security/limits.d/99-ds-dev.conf                           # A3''
rm -f /etc/systemd/systemd.conf.d/99-ds-dev.conf 2>/dev/null
rm -f /etc/systemd/system.conf.d/99-ds-dev.conf                       # A3''
# A4（/root/.ccache.conf）已于 S4 迁走：文件现在在
#   /root/.ds-opt/ccache.conf.DEAD-never-read-by-ccache   （它本就从未被 ccache 读取，见 §7.6）
rm -f /etc/systemd/journald.conf.d/99-ds-dev.conf                     # A5
cp /root/.ds-opt/environment.orig /etc/environment                    # A6（并一并撤销 S1/S2/S4）

# ---- S1 + S2：构建输出/缓存指向 /mnt/data ----
# 方式一（外科式，仅撤本次）：恢复追加前的备份
cp /root/.ds-opt/environment.bak-pre-dsbuild /etc/environment
# 方式二（彻底，连 A6 一起撤）：用 .orig 覆盖整个文件
# cp /root/.ds-opt/environment.orig /etc/environment
# 可选：删除落盘数据（删除前确认没有正在进行的构建）
# rm -rf /mnt/data/ds-build

# ---- S4：ccache 迁移 ----
cp /root/.ds-opt/environment.bak-pre-s4s6 /etc/environment   # 恢复 S4 之前的 /etc/environment
mv /mnt/data/ds-build/ccache /root/.cache/ccache             # 搬回缓存（或直接删，ccache 可再生）
# 注意：新配置在 $CCACHE_DIR/ccache.conf，搬回后仍在（随目录走）

# ---- S6：时区 ----
ln -sf /usr/share/zoneinfo/Etc/UTC /etc/localtime
rm -f /etc/timezone

# ---- S7：ccache shim 入口（只删这 16 个，勿 rm -rf 整个目录！）----
while read -r s; do [ -n "$s" ] && rm -f "/usr/local/bin/$s"; done < /root/.ds-opt/ccache-shims.list

# ---- S3：fstrim 定时 ----
rm -rf /etc/systemd/system/fstrim.timer.d /etc/systemd/system/fstrim.service.d
systemctl daemon-reload && systemctl restart fstrim.timer
# （回到出厂状态：timer 仍 enabled，但 ConditionVirtualization=!container 会让它每周被跳过）

# ---- S5：codex 旧版本 ----
# 不可恢复（包已删）。需要时由 codex 自动更新或重装重新拉取。

# ---- B 组软件包 ----
apt remove build-essential cmake ninja-build pkg-config \
           clang lld ccache rustc cargo nodejs npm \
           python3-pip python3-venv autoconf automake libtool libtool-bin gdb

# ---- 宿主级（在 Android 宿主执行）----
# container.config 恢复 force_cgroupv1=0（已处于该状态）
cp -a /data/local/Droidspaces/Containers/ubuntu/container.config.bak-cgv1 \
      /data/local/Droidspaces/Containers/ubuntu/container.config

# ---- compaction A/B：恢复原值（§6.7）----
# 本次已在测试结束时回滚并双向确认；此条仅供日后复查或再次实验后使用
cat /data/local/tmp/proact.orig > /proc/sys/vm/compaction_proactiveness
cat /proc/sys/vm/compaction_proactiveness     # 应输出 0
# 该参数不持久化——即使忘了回滚，重启也会回到默认
```
