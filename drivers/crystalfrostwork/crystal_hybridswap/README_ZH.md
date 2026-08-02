# Crystal Hybridswap 模块说明

语言：[English](README.md) | 简体中文

Crystal Hybridswap 是一个兼容 zram 用户态 ABI 的 Hybridswap 实现。它保留标准 zram 的用户态入口，同时使用 Crystal 自己的控制面和私有 zram 数据面来实现写回、批量读回、memcg 策略、压力通知和诊断能力。

一句话概括：用户态仍然看到 zram 设备和 zram sysfs 节点，内部则由 Crystal Hybridswap 管理策略、写回和诊断。

---

## 1. 模块简介

Crystal Hybridswap 面向内存压力场景，适合希望继续使用标准 zram 工作流，同时增加受控页写回能力的系统。

模块提供：

- 标准 zram 块设备，例如 `/dev/zramX`；
- 标准 zram 控制和配置路径，例如 `/sys/class/zram-control` 和 `/sys/block/zramX`；
- 带 ZMS 压缩对象打包写回和 batch-in 能力的私有 zram 数据面；
- Crystal 扩展的 sysfs、memcg、eventfd 和 debugfs 控制接口；
- 自动和显式的 swapout / swapin 风格操作；
- quota、压力、写回、batch-in 和最近操作快照等诊断信息。

Crystal Hybridswap 将标准 zram ABI 作为稳定外部契约。Crystal 专属行为通过新增节点暴露，而不是改变标准 zram 节点语义。

---

## 为什么需要 Crystal Hybridswap

Crystal Hybridswap 的目标，是保留 Hybridswap 对用户态有价值的外部使用契约，同时把强耦合的 vendor 内部状态机改写为更容易审计和维护的 zram slot 写回/读回设计。

OPPO 官方 Hybridswap 本身具有明确的功能价值。它提供了完整功能、成熟用户态 API、自动策略、per-memcg 控制，以及成体系的写回/读回能力。对于使用原始 vendor 内核的设备，这些能力是重要的，不应被简单否定。

问题主要在内部架构，而不是功能目标。旧架构把 extent 分配、反向映射、zram slot 状态、memcg 归属、backing storage、reclaim-in、batch-out、pre-out 和 fault-out 放在同一套共享状态机里。一个被写出的页面不只是带 backing storage 的 zram slot，同时还关联 extent 生命周期、rmap 条目、memcg 记账、LRU 关系以及不同场景的恢复规则。

在通用内核移植和长期维护场景下，继续局部修补这套模型成本很高。readback、writeback、delete、失败恢复或 memcg 记账中的局部改动，都可能同时影响多个隐式不变量。错误定位也会变得困难，因为可见问题可能出现在与触发状态迁移不同的场景中。

Crystal 因此保留有利于部署和维护的用户可见部分，但重写内部实现。设计原则包括：

- 尽量保留标准 zram ABI 和常见 Hybridswap 风格控制接口；
- 将兼容节点视为外部契约，而不是旧内部对象身份的承诺；
- 使用 zram slot writeback/readback，并通过 ZMS 将压缩对象打包到 backing device，不迁移旧 extent/rmap/fault-out 框架；
- batch-in 提交前进行页级 slot 快照校验；
- 将 core worker、memcg、pressure、zram bridge 和 stats 职责拆成可审计层；
- 通过明确计数器、操作快照和诊断报告暴露策略结果。

这不是否定 OPPO 官方 Hybridswap 的功能价值，而是面向通用内核移植和长期维护时，对可维护性与可审计性的取舍：保留 Hybridswap 使用体验，避免继承完整 vendor 内部 extent/rmap 状态机。

---

## 2. 架构设计

### 2.1 构建与配置模型

主配置符号是 `CONFIG_CRYSTAL_HYBRIDSWAP`。

典型配置要求包括：

- `CONFIG_CRYSTAL_HYBRIDSWAP=y`；
- 关闭普通 upstream/vendor `ZRAM` 实现，因为 Crystal 提供自己的 zram 兼容设备实现；
- 启用 `MEMCG` 和 `CGROUPS`；
- 启用或由配置自动选择 `ZSMALLOC`；
- 启用或由配置自动选择 `EVENTFD`，用于压力通知；
- 如需更完整诊断信息，启用 `DEBUG_FS`。

相关可选配置包括：

- `CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_EMPTY_APIS`：为旧用户态探测保留兼容占位接口。默认关闭，且不会恢复旧 Hybridswap 数据路径。
- `CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM`：暴露并启用旧 Hybridswap `memory.swapd_memcgs_param` memcg swapd 策略控制逻辑。该选项默认关闭。这套旧参数同时包含兼容/保存展示字段，以及启用后仍可能影响 Crystal 自动 memcg 写回的字段。
- `CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK`：启用私有 zram 写回数据面。
- `CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_MEMORY_TRACKING`：在具备 debugfs 支持时启用更详细的内存跟踪。
- `CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_MULTI_COMP`：在平台支持时启用多压缩流或多压缩器能力。
- `CONFIG_CRYSTAL_HYBRIDSWAP_SDDC`：启用按设备的相似性索引、精确 alias 和驻留 delta 表示；Crystal Hybridswap 开启时默认启用，运行时仍采用 best-effort 策略。
- `CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_LZ4KD`：构建 SDDC 使用的私有 4 KiB LZ4KD 普通/delta 后端，不向全局 Crypto API 注册算法；arm64 4 KiB page 构建默认启用。
- `CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_ZMS_NATIVE`：可选地将经过校验的 SDDC DELTA wire 对象原样写入 ZMS，并 pin 驻留的不可变 reference。设备验证完成前默认关闭；不支持的对象继续走 flatten 回退。
- `CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_KUNIT_TEST`：构建私有 codec 和 SDDC 状态机 KUnit 测试。
- `CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_DEF_COMP`：选择默认压缩算法。

### 2.2 组件分层

| 组件 | 职责 |
|---|---|
| zram driver | 提供 `/dev/zramX`、`/sys/block/zramX`、压缩、slot 状态、读写、写回、batch-in 和标准 zram ABI 兼容性。 |
| zram bridge | 提供 Crystal 专属 zram sysfs 控制、运行统计、摘要报告、loop-backed 设备绑定支持和操作快照。 |
| core worker | 管理异步工作、自动策略、quota、force swapout、force swapin、batch-in 调度和多 zram 目标选择。 |
| memcg interface | 提供 cgroup 级 force 操作、策略参数、压力节点和 per-memcg 状态输出。 |
| pressure interface | 提供基于 eventfd 的压力注册和 low / medium / critical 压力通知。 |
| debugfs stats | 提供面向开发和深度排障的扩展统计与诊断报告。 |
| compression layer | 管理私有 zram 数据面使用的压缩后端和压缩流。 |

高层数据流如下：

```text
user space / cgroup / eventfd
        -> bridge interfaces
        -> Crystal core worker
        -> private zram data plane
        -> backing device
```

### 2.3 私有 zram 数据面

Crystal Hybridswap 使用 zram slot 状态，而不是 extent 级对象模型。写回 slot 指向内存中的 ZMS handle，ZMS 将压缩后的 zram 对象打包进 PAGE_SIZE backing block。重要状态包括：

| 状态 | 含义 |
|---|---|
| `ZRAM_WB` | 页面压缩对象已经由 ZMS 写回到 backing device。 |
| `ZRAM_UNDER_WB` | 页面正在写回或 batch-in，不能被其他操作并发接管。 |
| `ZRAM_IDLE` | 页面被标记为 idle，可被写回策略选中。 |
| `ZRAM_HUGE` | 页面被视为 huge 或难压缩候选。 |
| `ZRAM_INCOMPRESSIBLE` | 当前压缩器无法有效压缩该页面。 |

所有页状态迁移都受 slot 级锁保护。写回和 batch-in 使用显式所有权规则，避免读、写、回收、reset 和设备移除路径并发破坏 slot 状态。

启用 SDDC 且 primary compressor 支持 delta 时，普通 zram 对象会被异步观察。完全相同的 stream 可以作为无 handle alias 共享一个 immutable reference，相似 stream 则可以保存为相对该 reference 的 LZ4KD delta。SDDC 在 commit、read 和 writeback 边界同时校验 slot mutation 与 reference generation。alias/delta 在进入 ZMS 前会 flatten 回普通 zram stream，因此驻留 reference graph 不会持久化。representation、wire format、所有权、锁序、记账和失败恢复不变量详见 [sddc/DESIGN.md](sddc/DESIGN.md)。

### 2.4 控制面

控制面负责协调：

- 自动写回策略；
- force swapout 请求；
- force swapin 和 batch-in 请求；
- 日写回 quota；
- 设备寿命相关控制；
- 基于 zram 压力的目标选择；
- 多 zram 目标选择；
- 重配置或 teardown 期间的暂停/恢复行为；
- 最近操作快照和计数器。

自动策略是 best-effort。它会结合内存压力、swap 可用性、zram 状态、quota、退避窗口和 memcg 候选情况，再决定是否派发写回工作。默认情况下，来自 `memory.swapd_memcgs_param` 的旧 Hybridswap memcg score 策略处于关闭状态，因此自动 memcg 写回不会被这套旧参数选择或加权；只有启用 `CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM` 后才会使用。

### 2.5 关键操作路径

#### 普通 zram I/O

用户态读写仍然通过 zram 块设备进行。页面可以继续压缩保存在内存中，也可以在已经写回时从 backing device 读取。

#### writeback

writeback 会按请求模式扫描 zram slot，例如 idle pages、huge pages、huge idle pages、incompressible pages 或指定 page index。符合条件的压缩对象会被标记为写回中，由 ZMS 打包写入 backing device，然后更新 slot 状态。

#### batch-in

batch-in 会从 backing device 读回页面并恢复到 zram。提交前会校验 slot 快照，避免操作期间已经变化的页面被旧数据覆盖。

#### force swapout

force swapout 是 memcg 或全局 best-effort 操作，用于将符合条件的页面写回 backing device。它可能部分成功，也可能因为无匹配页面、quota 用尽、压力检查、目标过滤或写回失败而提前结束。

#### force swapin

force swapin 是 best-effort batch-in 操作。它选择目标设备和页面，读回已写出的页面，并在替换 zram 条目前验证 slot 状态。

---

## 3. 主要功能

| 功能 | 说明 |
|---|---|
| 标准 zram ABI | 保留 `/dev/zramX`、`/sys/class/zram-control` 以及 `disksize`、`reset`、`compact`、`comp_algorithm`、`writeback`、`writeback_limit`、`bd_stat` 等标准 `/sys/block/zramX` 节点。 |
| backing device 支持 | 支持用于页面写回的后端块设备。Crystal 还提供 loop-device bridge，供需要晚绑定或更清晰诊断的部署使用。 |
| 自动写回策略 | 根据压力、zram 状态、quota、设备寿命控制和节流窗口决定是否写回页面。 |
| force swapout | 允许从 memcg 或全局控制路径显式触发 best-effort 页面写回。 |
| force swapin / batch-in | 允许将已写回页面通过快照校验读回 zram。 |
| force shrink | 在平台支持时提供 memcg 级匿名页和文件页收缩控制。 |
| per-memcg 策略与统计 | 暴露 cgroup 级压力、参数、操作结果和 per-app 摘要。 |
| multi-zram 处理 | 多个 zram 设备同时存在时选择合适目标。 |
| SDDC 与 LZ4KD delta | 可选地去重完全相同的驻留 stream，并将相似 stream 保存为经过校验的 delta，同时保持普通 zram 和 ZMS 行为。 |
| 压力通知 | 通过 eventfd 报告 low、medium、critical 三档内存压力。 |
| 诊断能力 | 提供轻量 sysfs 统计、详细 Crystal 统计、操作快照、debugfs 报告和内核日志。 |

---

## 4. 用户使用说明

### 4.1 启用模块

典型配置如下：

```text
CONFIG_CRYSTAL_HYBRIDSWAP=y
CONFIG_MEMCG=y
CONFIG_CGROUPS=y
CONFIG_ZSMALLOC=y
CONFIG_EVENTFD=y
```

普通 `ZRAM` 实现不应同时启用。如需详细 debugfs 诊断，请启用 `DEBUG_FS`。

### 4.2 推荐初始化顺序

下面是通用顺序。请将 `<zramX>`、`<backing_dev>`、`<loop_dev>` 和 `<cg_path>` 替换成具体平台值。

```bash
# 1. 创建 zram 设备。部分平台会通过 init 脚本完成。
echo 1 > /sys/class/zram-control/hot_add

# 2. 选择压缩算法并设置容量。
echo lz4 > /sys/block/<zramX>/comp_algorithm
echo 4G  > /sys/block/<zramX>/disksize

# 3. 绑定 backing device。
echo /dev/<backing_dev> > /sys/block/<zramX>/backing_dev

# 可选：部署需要时使用 Crystal loop-device bridge。
echo /dev/<loop_dev> > /sys/block/<zramX>/hybridswap_loop_device

# 4. 作为 swap 使用。
mkswap /dev/<zramX>
swapon /dev/<zramX>

# 5. 启用 Crystal 控制。
echo 1 > /sys/block/<zramX>/hybridswap_enable
echo 1 > /sys/block/<zramX>/hybridswap_core_enable
echo 0 > /sys/block/<zramX>/hybridswap_swapd_pause

# 6. 可选：调整自动策略。
echo 1          > /sys/block/<zramX>/hybridswap_dev_life
echo 1000000000 > /sys/block/<zramX>/hybridswap_quota_day
echo 75         > /sys/block/<zramX>/hybridswap_zram_increase

# 7. 可选：在启用 CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM 时，
# 配置旧 memcg swapd 策略。
# 请先读取参数节点，并按内核返回的格式写入。
echo '...' > /sys/fs/cgroup/memory/<cg_path>/memory.swapd_memcgs_param
echo '...' > /sys/fs/cgroup/memory/<cg_path>/memory.swapd_single_memcg_param
```

### 4.3 常用 zram sysfs 节点

| 节点 | 用途 |
|---|---|
| `disksize` | 设置 zram 设备容量。 |
| `reset` | 重置 zram 设备。 |
| `compact` | 请求 zsmalloc compact。 |
| `comp_algorithm` | 选择压缩算法。 |
| `backing_dev` | 绑定用于写回的后端块设备。 |
| `writeback` | 触发页面写回。 |
| `writeback_limit` | 设置写回限制。 |
| `writeback_limit_enable` | 启用或关闭写回限制。 |
| `sddc_stat` | 显示 SDDC manager 状态、候选活动、驻留 reference/alias/delta、节省字节和失败计数；SDDC 不可用时报告全零。 |
| `bd_stat` | 三字段 data-path backing-device 统计，使用 ZMS 物理 4K block 口径。 |
| `zms_stat` | 显示 Crystal ZMS backing-store 状态、打包情况、block 分配、dirty 数据和 read-merge 诊断。 |
| `writeback_cold_stat` | 显示启用 entry access-time tracking 时自动写回使用的 age/cold-page 选择计数。 |

### 4.4 Crystal zram bridge 节点

| 节点 | 用途 |
|---|---|
| `hybridswap_enable` | Crystal Hybridswap 总开关。 |
| `hybridswap_core_enable` | 启用或关闭核心策略，不改变总开关状态。 |
| `hybridswap_swapd_pause` | 暂停或恢复自动 swapd 风格策略。 |
| `hybridswap_loglevel` | 控制模块日志详细程度。 |
| `hybridswap_vmstat` | 显示轻量运行状态和最近操作信息。 |
| `hybridswap_crystal_stat` | 显示详细 zram I/O、quota、multi-zram 和压力统计。 |
| `hybridswap_report` | 显示模块能力和诊断摘要。 |
| `hybridswap_stat_snap` | 显示最近 swapout、swapin、writeback 和 batch-in 快照。 |
| `hybridswap_meminfo` | 显示 zram 与 memcg 相关内存摘要。 |
| `hybridswap_loop_device` | 在支持时记录或绑定 loop-backed 设备路径。 |
| `hybridswap_dev_life` | 控制设备寿命相关策略行为。 |
| `hybridswap_quota_day` | 控制每日写回 quota。 |
| `hybridswap_zram_increase` | 控制 zram 压力或增长阈值。 |

### 4.5 memcg 节点

常见 memcg 节点包括：

- `memory.force_swapout`
- `memory.force_swapin`
- `memory.force_shrink_anon_percent`
- `memory.force_shrink_anon`
- `memory.force_shrink_file`
- `memory.swapd_pressure`
- `memory.avail_buffers`
- `memory.erm_avail_buffer_enable`
- `memory.erm_avail_buffer`
- `memory.swapd_policy_stat`
- `memory.total_info_per_app`
- `memory.app_uid`

`memory.force_shrink_anon_percent` 接受 1 到 100 的百分比，
按当前 memcg 本地匿名页加 zram/writeback 页计算目标换出比例，
只对尚未达到目标的差额发起匿名页回收。现有
`memory.force_shrink_file` 路径会保留全局 inactive file 低水位，
当继续回收可能低于 768 MiB 时拒绝或提前停止。

`memory.erm_avail_buffer_enable` 控制 OSvelte/ERM 可用内存水位覆盖
是否参与自动策略，默认值由
`CONFIG_CRYSTAL_HYBRIDSWAP_ERM_AVAIL_BUFFER_DEFAULT_ON` 决定，该选项
默认启用。`memory.erm_avail_buffer` 接受两个 MiB 值：
`min_avail high_avail`，并要求 `high_avail - min_avail >= 64`。
启用且写入有效值后，Crystal 只用它们替换自动策略中的 effective
`min_avail_buffers` 和 `high_avail_buffers`；`memory.avail_buffers`
保存的 base 四元组、`free_swap_threshold`、`zram_wm_ratio`、quota、
dev_life、zram gate、backoff/window throttle 和 `force_*` 接口均不被
ERM 覆盖。`memory.swapd_policy_stat` 会显示 base、ERM override 和
effective 条件来源。

`memory.app_uid` 为兼容旧用户态保留可写权限，但 Crystal 会忽略写入值，
显示值仍根据当前挂在该 memcg 下的 task 自动推导：`-1` 表示未知或空
memcg，`-2` 表示存在多个 UID，非负数表示真实 Linux UID。

启用 `CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM` 时，Crystal 还会暴露旧 `memory.swapd_memcgs_param` 根 cgroup 节点和 `memory.swapd_single_memcg_param` per-memcg 节点。关闭该选项时，这些节点不暴露，自动 memcg 写回也不受旧 score/ratio 策略控制。

启用 `CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_EMPTY_APIS` 时，可能额外出现面向旧用户态探测的占位节点。这些占位节点不会恢复旧内部 Hybridswap 行为。

### 4.6 诊断方法

建议排查顺序：

```bash
cat /sys/block/<zramX>/hybridswap_vmstat
cat /sys/block/<zramX>/hybridswap_crystal_stat
cat /sys/block/<zramX>/hybridswap_stat_snap
cat /sys/block/<zramX>/sddc_stat
cat /sys/kernel/debug/crystal_hybridswap/stats
cat /sys/kernel/debug/crystal_hybridswap/report
dmesg | grep -i hybridswap
```

确认是否发生写回或 batch-in 时，可检查：

- `last_writeback_*` 字段；
- `force_swapout_*` 字段；
- `force_swapin_*` 字段；
- `zram_bd_*` 字段；
- `batchin_*` 字段；
- 压力原因字段；
- debugfs 计数器；
- 相关内核日志。

---

## 5. API / ABI

### 5.1 标准 zram ABI

Crystal Hybridswap 保留标准 zram 对外 ABI：

- `/dev/zramX` 块设备；
- `/sys/class/zram-control` hot-add 和管理接口；
- 标准 `/sys/block/zramX` 配置节点；
- `backing_dev`、`writeback`、`writeback_limit`、`writeback_limit_enable`、`bd_stat` 等标准写回相关节点。

`bd_stat` 有意保持三字段输出，但 Crystal/ZMS 下字段使用 data-path
物理 4K backing block 口径：当前已回写 block、累计物理回读 block、
累计物理回写 block。ZMS compact 等内部维护 I/O 不计入 `bd_stat`；
完整 ZMS 物理 I/O 计数保留在 `zms_stat`。

### 5.2 Crystal 扩展接口分组

Crystal 专属接口分为：

1. zram bridge 节点：`hybridswap_vmstat`、`hybridswap_crystal_stat`、`hybridswap_report`、`hybridswap_stat_snap` 及相关控制节点。
2. zram 数据面诊断节点：只读的 `sddc_stat`、`zms_stat` 和 `writeback_cold_stat`。
3. memcg bridge 节点：`memory.force_swapout`、`memory.force_swapin`、`memory.force_shrink_*`、`memory.swapd_*` 和 per-app 摘要节点。
4. pressure eventfd 接口：注册并通知 low、medium、critical 压力等级。
5. debugfs 接口：详细计数、快照和诊断报告。

### 5.3 兼容占位 API 边界

`CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_EMPTY_APIS` 只用于旧脚本探测 legacy 节点名时的用户态兼容。该选项默认关闭。启用后，这些节点仍是占位或保存态视图，不代表兼容旧内部数据路径。

### 5.4 旧 memcg swapd 策略 ABI

`CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM` 将旧 Hybridswap `memory.swapd_memcgs_param` 策略接口作为独立兼容功能控制。该选项默认关闭。关闭时，`memory.swapd_memcgs_param` 和 `memory.swapd_single_memcg_param` 不暴露，Crystal 自动 memcg 写回不会使用旧 score 和 ratio 策略进行候选选择或权重计算。`memory.app_score` 仍作为基础 per-memcg 数值保留给其他兼容和诊断路径；不启用旧策略选项时，它本身不会开启旧式自动 memcg 写回策略。

启用该选项时，`memory.swapd_memcgs_param` 接受旧 level 格式：level 数量后跟每个 level 的 `min_score`、`max_score`、`ub_mem2zram_ratio`、`ub_zram2ufs_ratio` 和 `refault_threshold`。真正影响 Crystal 自动 memcg zram-to-UFS 写回的是 score 区间和 `ub_zram2ufs_ratio`：score 区间用于匹配 memcg，`ub_zram2ufs_ratio` 参与候选权重和预算分配。`ub_mem2zram_ratio` 和 `refault_threshold` 主要作为旧 ABI 兼容/展示字段保留。

---

## 6. 与 OPPO 官方 Hybridswap 的区别

本节面向 Crystal Hybridswap 用户解释设计差异。核心区别在于范围：OPPO 官方 Hybridswap 是带有完整内部对象模型的 vendor 实现；Crystal Hybridswap 是 zram 兼容的重构实现，保留有用的外部行为，同时替换内部数据路径。

| 维度 | Crystal Hybridswap | OPPO 官方 Hybridswap |
|---|---|---|
| 功能价值 | 保留实用 Hybridswap 用户体验，同时简化内部结构以适配通用内核维护。 | 提供完整且成熟的 Hybridswap 功能，包括自动策略、per-memcg 控制和成体系的写回/读回。 |
| 外部契约 | 保留标准 zram ABI，并增加 Crystal 扩展节点。 | 使用 OPPO 专属 Hybridswap 行为和私有接口。 |
| 内部状态模型 | 使用 zram slot 状态、ZMS 压缩对象打包存储和明确的 writeback/batch-in 所有权。 | 使用 extent / rmap / object 风格模型，并包含 reclaim-in、batch-out、pre-out 和 fault-out 场景。 |
| 耦合关系 | 拆分 zram 数据面、core worker、memcg 策略、pressure 通知和 stats。 | 强耦合 zram slot 状态、backing storage、memcg 映射、rmap 条目和 extent 生命周期。 |
| 写回/读回模型 | 写回选中的 zram 压缩对象，读回时通过 slot 快照校验恢复到 zram。 | 使用围绕 extent 对象和场景状态迁移构建的官方 vendor reclaim 与恢复路径。 |
| 策略引擎 | 使用 Crystal core policy，关注压力、quota、节流和 multi-zram 选择。 | 使用与其内部状态耦合的官方 vendor 策略模型。 |
| 维护模型 | 倾向显式所有权、更小分层、可审计计数器和局部化失败报告。 | 许多 readback、writeback、恢复和 memcg 变更都必须维护共享隐式不变量。 |
| 诊断方式 | 区分标准 zram ABI、Crystal sysfs 统计、操作快照、debugfs 报告和内核日志。 | 使用官方 vendor 诊断模型。 |
| 兼容立场 | 保持 zram 可用性和部分用户态节点兼容，但不保持旧内部实现身份。 | 保持 OPPO 官方实现语义。 |

实际结果是：Crystal Hybridswap 表现为带 Crystal 控制面的 zram 兼容系统，而不是 OPPO 内部 extent / rmap / fault-out 模型的实现。开发者应将二者视为外部目标相关、内部设计不同的实现。

---

## 7. 安全性与限制

- backing device 需要谨慎选择。Crystal Hybridswap 不会让不适合 swap 工作负载的存储设备变得安全。
- force swapout 和 force swapin 都是 best-effort 操作。部分成功、无匹配页面、quota 用尽、压力过滤、快照不匹配和 I/O 失败都属于可能结果。
- 标准 zram ABI 兼容性优先于标准节点扩展。Crystal 专属信息应放在 Crystal 扩展节点或 debugfs。
- debugfs 面向开发和深度诊断，不应视为稳定生产 ABI。
- 兼容占位 API 默认关闭，只应在用户态确实需要时启用。
- 旧 `memory.swapd_memcgs_param` 策略 ABI 默认关闭；只有旧用户态需要该控制面及其 score/`ub_zram2ufs_ratio` 自动 memcg 写回行为时才应启用。
- SDDC 在满足 Kconfig 依赖时默认启用，需要 4 KiB page 和支持 delta 的 primary compressor，其 delta wire format 只在内存中驻留；可通过 `sddc_stat` 确认 manager 是否运行。初始化或运行时资源不足时会回退到普通压缩。
- 模块不提供 OPPO 官方内部 extent / rmap / fault-out 数据路径。
- 自动策略依赖运行时压力、quota、memcg 状态和 backing-device 可用性，应视为自适应策略而非确定性事务。

---

## 8. 故障排查 FAQ

### 8.1 为什么 `backing_dev` 显示 `none`？

请检查：

1. 写入路径是否为有效块设备。
2. zram 设备是否已经初始化并设置非零 `disksize`。
3. 当前设备状态是否允许绑定 backing device。
4. 是否存在阻止晚绑定的旧写回 slot。
5. 内核日志是否报告 backing-device 校验或绑定错误。

### 8.2 为什么 `bd_stat` 只有三字段？

因为 `bd_stat` 保持三字段 ABI 形态。Crystal/ZMS 下这些字段使用
data-path 物理 4K block 口径，并排除 ZMS 内部维护 I/O。完整 ZMS
物理读写 I/O 请查看 `zms_stat`，其他 Crystal 专属详细计数请查看
`hybridswap_crystal_stat`、`hybridswap_stat_snap` 和 debugfs。

### 8.3 为什么 `hybridswap_vmstat` 和 `hybridswap_crystal_stat` 分开？

`hybridswap_vmstat` 是轻量视图，用于查看当前状态和最近操作；`hybridswap_crystal_stat` 是更完整诊断视图，覆盖 zram I/O、quota、multi-zram 行为和压力状态。

### 8.4 为什么 force swapout / force swapin 不是精确 MB？

请求会先转换为页面，再按 slot 状态、目标 cgroup、压力条件、写回限制、快照状态和 I/O 结果过滤。页级 best-effort 执行不是精确字节事务。

### 8.5 自动写回为什么被跳过？

常见原因包括：

- `hybridswap_enable` 关闭；
- `hybridswap_core_enable` 关闭；
- `hybridswap_swapd_pause` 开启；
- 没有可用 backing device；
- zram 压力低于配置阈值；
- 策略处于空结果退避窗口；
- daily quota 或窗口节流阻止操作；
- 没有可用 memcg 候选；
- 选中的设备没有符合条件的页面。

### 8.6 如何确认确实发生了写回或 batch-in？

建议结合多类证据：

- `hybridswap_vmstat` 查看最近操作摘要；
- `hybridswap_crystal_stat` 查看累计计数；
- `hybridswap_stat_snap` 查看最近操作快照；
- debugfs stats 查看详细计数；
- 内核日志查看操作结果和压力消息。

---

## 9. 开发者维护指南

### 9.1 职责地图

| 区域 | 维护重点 |
|---|---|
| Kconfig | 保持功能符号、依赖、默认值和兼容选项清晰。 |
| Makefile | 保持模块对象组成与启用功能一致。 |
| zram driver | 保持标准 zram ABI、slot 状态正确性、压缩行为、写回、batch-in 和设备生命周期安全。 |
| zram bridge | 保持 Crystal sysfs 控制、统计、报告和操作快照准确且便于阅读。 |
| core worker | 维护异步工作顺序、自动策略、quota 记账、force 操作和 multi-zram 选择。 |
| memcg interface | 维护 cgroup 控制语义、参数解析、force 操作和 per-memcg 统计。 |
| pressure interface | 维护 eventfd 注册、通知等级和压力原因报告。 |
| debugfs stats | 维护详细计数和诊断报告，但不要将 debugfs 当作稳定 ABI。 |
| compression layer | 维护压缩器选择、压缩流处理和 zram 数据面预期兼容性。 |
| SDDC layer | 维护 immutable reference 所有权、slot/cookie ABA 防护、驻留 decode/flatten 行为和精确 payload 字节记账。 |

### 9.2 核心不变量

修改模块时应保护这些不变量：

1. zram 设备生命周期在 reset、移除和异步工作并发时必须安全。
2. 页级 slot 状态只能在正确保护下修改。
3. `ZRAM_UNDER_WB` 所有权必须清晰；writeback 或 batch-in 工作的所有者负责清理它。
4. batch-in 提交恢复数据前必须校验 slot 快照。
5. 自动策略必须遵守 pause、disable、teardown 和 reconfiguration 状态。
6. 除非有意重设计队列模型，pending async work 应保持快照式语义。
7. 统计字段应使用清晰单位，例如 pages、bytes、ns、ms、count 或 ratio。
8. 标准 zram 节点必须保持兼容；Crystal 专属数据应放在 Crystal 扩展节点或 debugfs。
9. 兼容占位 API 不应悄悄获得真实旧数据路径语义。
10. 旧 `memory.swapd_memcgs_param` 策略行为必须始终受 `CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM` 控制；关闭该选项时不得影响自动 memcg 写回。
11. SDDC source/target 工作在 commit 前必须校验完整捕获身份，writeback 必须同时校验 zram 与 SDDC snapshot。
12. SDDC reference 必须通过 slot ownership 释放，teardown 不得强制清空仍有 owner 的 reference。

### 9.3 维护建议

- 非标准 zram ABI 的新增用户可见行为，应优先放入 Crystal 扩展节点。
- 保持 `bd_stat` 符合标准 zram 预期。
- debugfs 输出应便于诊断，但不要文档化为稳定 ABI。
- 自动策略决策应可通过统计和最近操作原因观测。
- memcg 参数解析应既能拒绝非法输入，又便于平台脚本读取和写入。
- multi-zram 选择在相同运行状态下应保持可预期。
- 文档应聚焦当前模块行为、受支持接口和维护不变量。
