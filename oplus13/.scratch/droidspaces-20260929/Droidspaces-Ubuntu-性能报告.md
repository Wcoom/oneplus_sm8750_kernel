# Droidspaces Ubuntu 26.04 性能报告

- 设备：OnePlus 13 PJZ110（SM8750），Android 16，KernelSU root
- 容器：Droidspaces 6.4.5，容器名 `ubuntu`
- 诊断采集：2026-09-29 15:25–15:45 UTC（只读）
- 优化与复测：2026-09-29 15:45 – 2026-09-30 01:20（A+B 两组 + 三项授权宿主级操作 + S1–S7 全部待办项 + `vm.compaction_proactiveness` 运行时 A/B）
- 采集方式：全部经 `adb → su → droidspaces --name=ubuntu run /bin/sh <脚本>`
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

### 4.5 **内存回收压力（本报告最关键的未解决项）**

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

**C1. 整机内存长期低于 watermark high，分配路径频繁同步回收** —— **未解决（宿主侧）**
- 证据：`free 39,969 页 < high 40,876 页`；`allocstall 合计 40,209`（持续增长）；`pgscan_direct 3,358,204`；`compact_fail / compact_stall = 126 / 132 = 96%`
- 影响：**所有**负载（编译、Node、Rust、多进程 agent）都会出现不可预测的停顿
- 性质：**宿主与容器共享内存，容器无独立限额**；根因在 Android 宿主侧
- **本次未处理**（属红线范围）。**这是本机唯一的 Critical 级问题，也是唯一未被缓解的项。**

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
