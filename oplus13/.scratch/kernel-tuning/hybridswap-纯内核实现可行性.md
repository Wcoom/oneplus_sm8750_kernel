# 研究 · crystal_hybridswap_webui 模块能否纯内核层实现

> 日期：2026-08-15。方法：一手证据——设备端模块文件（`/data/adb/modules/crystal_hybridswap_webui/`）、`chswapctl` 二进制 strings、内核树源码 `drivers/crystalfrostwork/crystal_hybridswap/`。
> 结论分级：参数默认值固化「完全可行」；防 vendor 反写「可行（fq_guard 先例）」；WebUI「不可内核化」。

## 1. 模块在做什么（一手证据）

- 模块定位："WebUI, boot owner, and ColorOS background policy for crystal_hybridswap"（`module.prop` description）。
- 开机三阶段应用 profile：`post-fs-data.sh`、`service.sh`、`boot-completed.sh` 均调用 `chswapctl api POST /v1/boot/apply`（三个脚本原文）。
- 常驻守护：`service.sh` 注释 "a persistent daemon that reconciles the profile onto the kernel whenever an external actor drifts a managed node"；由 `daemon.sh`（supervisor）拉活。
- 防 vendor 反写：`oplus-policy.sh` 用模块自带文件替换 6 个 ColorOS 策略 XML（`sys_osense_appmng_decisionmaker_config.xml`、`sys_osense_memory_config.xml`、`sys_memory_nirvana_config.xml`、`sys_osense_feature_common_config.xml`、`sys_mm_swap_config.xml`、`sys_osense_memory_decisionmaker_config.xml`，清单在该脚本 `CHSWAP_OPLUS_POLICY_FILES`）。

## 2. profile 键 → 内核落点（全部有出处）

| profile 键 | 值 | 内核落点 | 出处 |
|---|---|---|---|
| crystal.enable / core_enable | 1 | `chs.enabled`/`chs.core_enabled`（默认 0=关闭） | `core.c:3306-3307`（默认）、`core.c:2781-2816`（setter） |
| policy.zram_wm_ratio | 100 | cgroup 文件 `zram_wm_ratio`，内核默认宏 `CHS_DEFAULT_ZRAM_WM_RATIO` | `memcg.c:1747-1756`、`core.c:3315` |
| policy.avail_buffers | 2200/1800/2200 | cgroup 文件 `avail_buffers`，内核默认 0/0/0 | `memcg.c:1773-1791`、`memcg.c:2578-2580` |
| policy.erm_avail_buffer_enable | 1 | cgroup 文件，内核默认宏 `CHS_ERM_AVAIL_BUFFER_DEFAULT_ENABLE` | `memcg.c:1797-1807`、`core.c:3309` |
| zram.disksize / comp_algorithm | 22G / lz4 | zram sysfs（userspace 设置，内核无默认 disksize） | `chswapctl` strings：`zram0`、`recomp_algorithm` |
| backing.mode | block | 绑定 `/dev/block/by-name/hybridswaps`（loop_device 节点） | `zram_bridge.c:829-861`、strings |
| swap.enabled / priority | true / 32767 | swapon 动作 + 优先级（userspace 语义） | strings：`swap.swapoff`、`profile.swap.priority is out of range` |
| aging.* | 900s/64M/4096M/1800s/1 | per-memcg `aging_anon` 控制文件 | `memcg.c:1507-1520`、strings：`aging_anon`、`fs/cgroup/top-app/cgroup.procs` |

设备 cgroup 布局：cgroup2 挂载于 `/sys/fs/cgroup`（`/proc/mounts` 实测）；`chswapctl` strings 显示其写 `/sys/fs/cgroup/memory`、监控 `fs/cgroup/top-app/cgroup.procs`（游戏前台探测，strings：`aging: game foreground`）。

## 3. 可行性分级

### ✅ 完全可行：参数默认值固化（树内）
- 改 `core.c` 初值（enabled/core_enabled=1、avail 2200/1800/2200、wm_ratio 100、erm=1），或加 Kconfig/module_param 控制。
- 内部 atomic 状态，不涉及导出符号与结构布局，**KMI 零风险**。

### ⚠️ 可行但需自举逻辑：zram 初始化 + swapon
- 默认 disksize：给 zram/zram_bridge 加默认值参数（vendor 驱动无默认）；自动 swapon（priority 32767）需要驱动自举或保留一次 userspace 动作。**建议保留 userspace 做 swapon**（最稳）。

### ⚠️ 可行（中等工作量）：防 vendor 反写的内核守护
- 树内先例：`net/sched/fq_guard.c`（事件驱动 + 有限复查强制守护）。
- 方案：驱动内置"managed 节点守护"，检测 ColorOS 漂移即恢复。
- 副作用：参数固定死 → vendor 按场景动态调节（appmng 场景触发）失效——与模块 daemon 行为等效，但 XML 替换本身无法内核化（那是 userspace 文件）。

### ❌ 不可内核化：WebUI 监控面板
- 监控/展示/配置 UI 属 userspace；纯内核替代的是"boot owner + 参数守护"两部分，WebUI 可保留或舍弃。

## 4. 推荐路径

1. **保守**（推荐起步）：固化"参数默认值"（使能 + avail/wm/erm），zram disksize 加内核默认参数；swapon 与监控保留轻量 userspace（可用 service.d 脚本替代整个 Go 模块）。
2. **激进**：再加 fq_guard 式内核守护（约 100-200 行）。
3. **维持现状**：继续用模块。

## 5. 保守档实现要点（精确到行，2026-08-15 预研）

- **Kconfig 先例**：`drivers/crystalfrostwork/crystal_hybridswap/Kconfig` 已有 `CRYSTAL_HYBRIDSWAP_ERM_AVAIL_BUFFER_DEFAULT_ON`（default y，控制 `crystal_hybridswap_internal.h:92-96` 的 erm 默认）——照此模式新增同类选项即可，defconfig 现有 crystal 选项在 `gki_defconfig:822-828`。
- **使能固化**：`core.c:3306-3307` `atomic_set(&chs.enabled/core_enabled, 0)` → 1（或改由新 Kconfig 控制）。
- **wm_ratio**：`crystal_hybridswap_internal.h:35` `CHS_DEFAULT_ZRAM_WM_RATIO 75` → 100（模块 profile 值）。
- **avail_buffers**：`memcg.c:2578-2580` 初值 0/0/0 → 2200/1800/2200（模块 profile 值）。
- **aging 是空转**：`CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_EMPTY_APIS` 未开（defconfig 无此条目）时 `memory.aging_anon` 为无功能占位（Kconfig help 原文）——模块的 aging 配置在本构建不生效，**纯内核化时可直接忽略 aging 项**。
- **zram disksize**：驱动无默认值接口（仅 `zram_drv.c:6175` `module_param(num_devices)` 一个先例），需新增 `module_param`/Kconfig 默认值；lz4kd 默认压缩已在 defconfig（`CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_DEF_COMP="lz4kd"`），与模块的 lz4 请求略有出入（SDDC 通路用 lz4kd）。
- **不动的部分**：swapon + priority 32767（保留一次 userspace 动作）、WebUI 监控（userspace）。
- **KMI 安全**：全部为驱动内部 atomic/宏，不涉及导出符号与结构布局。
