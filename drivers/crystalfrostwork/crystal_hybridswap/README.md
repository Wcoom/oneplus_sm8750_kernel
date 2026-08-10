# Crystal Hybridswap

Language: English | [简体中文](README_ZH.md)

Crystal Hybridswap is a zram-compatible hybridswap implementation. It keeps the standard zram user-space ABI while replacing the internal hybridswap data path with a Crystal-managed control plane and a private zram data plane.

In short: user space still sees zram devices and zram sysfs nodes, while Crystal Hybridswap provides its own policy engine, writeback handling, memcg integration, pressure notification, and diagnostics.

---

## 1. Module Overview

Crystal Hybridswap is designed for memory-pressure scenarios where a system wants to keep the normal zram workflow and add controlled page writeback to a backing device.

The module provides:

- standard zram block devices such as `/dev/zramX`;
- standard zram control and configuration paths such as `/sys/class/zram-control` and `/sys/block/zramX`;
- a private zram data path with ZMS packed compressed-object writeback and batch-in support;
- Crystal-specific sysfs, memcg, eventfd, and debugfs control surfaces;
- automatic and explicit swapout/swapin style operations;
- diagnostics for quota, pressure, writeback, batch-in, and recent operation snapshots.

Crystal Hybridswap keeps the standard zram ABI as the stable external contract. Crystal-specific behavior is exposed through additional nodes instead of changing the meaning of standard zram nodes.

---

## Why Crystal Hybridswap Exists

Crystal Hybridswap exists to preserve the practical user-space contract of Hybridswap while replacing a tightly coupled vendor-internal state machine with a simpler zram-slot writeback/readback design.

The old OPPO official Hybridswap implementation has real functional value. It provides a complete Hybridswap feature set, a mature user-space API, automatic policy, per-memcg control, and an integrated writeback/readback system. For devices that use the original vendor kernel, those properties are important and should not be treated as accidental.

The portability and maintenance problem is internal rather than functional. The old architecture combines extent allocation, reverse mapping, zram slot state, memcg ownership, backing storage, reclaim-in, batch-out, pre-out, and fault-out into one shared state machine. A page written back through that model is not only a zram slot with backing storage; it is also tied to extent lifetime, rmap entries, memcg accounting, LRU membership, and scene-specific recovery rules.

That design makes incremental patching expensive on generic kernels. A local change in readback, writeback, delete, failure recovery, or memcg accounting can affect multiple implicit invariants at once. Error localization is difficult because the observable failure may appear in a different scene from the state transition that caused it.

Crystal therefore keeps the user-visible parts that are useful for deployment and maintenance, but rewrites the internals. The guiding principles are:

- preserve the standard zram ABI and common Hybridswap-style control interfaces where practical;
- keep compatibility nodes as external contracts, not as commitments to old internal object identity;
- use zram-slot writeback/readback with ZMS packed compressed-object storage instead of migrating the extent/rmap/fault-out framework;
- validate page-level slot snapshots before committing batch-in data;
- split core worker, memcg, pressure, zram bridge, and statistics responsibilities into auditable layers;
- make policy outcomes visible through explicit counters, operation snapshots, and diagnostic reports.

This is not a denial of the value of OPPO official Hybridswap. It is a maintainability and auditability choice for kernel ports that need the Hybridswap user experience without inheriting the full vendor-internal extent/rmap state machine.

---

## 2. Architecture

### 2.1 Build and Configuration Model

The main configuration symbol is `CONFIG_CRYSTAL_HYBRIDSWAP`.

Typical configuration requirements are:

- `CONFIG_CRYSTAL_HYBRIDSWAP=y`;
- the normal upstream/vendor `ZRAM` implementation disabled, because Crystal provides its own zram-compatible device implementation;
- `MEMCG` and `CGROUPS` enabled;
- `ZSMALLOC` enabled or selected by the configuration;
- `EVENTFD` enabled or selected for pressure notification;
- optional `DEBUG_FS` for extended runtime diagnostics.

Relevant optional symbols include:

- `CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_EMPTY_APIS`: exposes compatibility placeholder interfaces for old user-space probes. It is disabled by default and does not restore the old hybridswap data path.
- `CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM`: exposes and enables the old Hybridswap `memory.swapd_memcgs_param` memcg swapd policy control logic. It is disabled by default. The legacy tuple contains compatibility/saved-only fields and fields that can still affect Crystal automatic memcg writeback when this option is enabled.
- `CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK`: enables the private zram writeback data path.
- `CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_MEMORY_TRACKING`: enables more detailed memory tracking when debugfs support is available.
- `CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_MULTI_COMP`: enables multi-stream or multi-compressor support where supported by the platform.
- `CONFIG_CRYSTAL_HYBRIDSWAP_SDDC`: enables the per-device similarity index, exact aliases, and resident delta representation (default y when Crystal Hybridswap is enabled; the runtime path remains opportunistic).
- `CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_LZ4KD`: builds the private 4 KiB LZ4KD ordinary/delta backend used by SDDC without registering a global Crypto API algorithm (default y on arm64 4 KiB-page builds).
- `CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_ZMS_NATIVE`: keeps validated SDDC DELTA and ALIAS wire objects intact in ZMS and pins their immutable resident references. It is enabled by default; unsupported objects retain the flatten fallback.
- `CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_KUNIT_TEST`: builds the private codec and SDDC state-machine KUnit coverage.
- `CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_DEF_COMP`: selects the default compression algorithm.

### 2.2 Component Layout

| Component | Responsibility |
|---|---|
| zram driver | Provides `/dev/zramX`, `/sys/block/zramX`, compression, slot state, read/write, writeback, batch-in, and standard zram ABI compatibility. |
| zram bridge | Provides Crystal-specific zram sysfs controls, runtime statistics, summaries, loop-backed device binding support, and operation snapshots. |
| core worker | Owns asynchronous work, automatic policy decisions, quota handling, force swapout, force swapin, batch-in dispatch, and multi-zram target selection. |
| memcg interface | Provides cgroup-level force operations, policy parameters, pressure nodes, and per-memcg status output. |
| pressure interface | Provides eventfd-based pressure registration and low/medium/critical pressure notification. |
| debugfs stats | Provides extended statistics and diagnostic reports for development and deep troubleshooting. |
| compression layer | Manages compression backends and streams used by the private zram data plane. |

The high-level data flow is:

```text
user space / cgroup / eventfd
        -> bridge interfaces
        -> Crystal core worker
        -> private zram data plane
        -> backing device
```

### 2.3 Private zram Data Plane

Crystal Hybridswap uses zram slot state rather than an extent-level object model. Written-back slots point to volatile ZMS handles, and ZMS packs compressed zram objects into PAGE_SIZE backing blocks. Important slot states include:

| State | Meaning |
|---|---|
| `ZRAM_WB` | The page has been written back to the backing device. |
| `ZRAM_UNDER_WB` | The page is under writeback or batch-in processing and must not be concurrently taken by another operation. |
| `ZRAM_IDLE` | The page is marked idle and may be selected by writeback policy. |
| `ZRAM_HUGE` | The page is treated as a huge or poorly-compressible candidate. |
| `ZRAM_INCOMPRESSIBLE` | The active compressor could not compress the page efficiently. |

All page state transitions are protected by slot-level locking. Writeback and batch-in use explicit ownership rules so that concurrent read, write, reclaim, reset, and device teardown paths do not corrupt slot state.

When SDDC is enabled with a delta-capable primary compressor, ordinary zram
objects are observed asynchronously. Exact streams can share one immutable
reference as handle-less aliases, while similar streams can be stored as an
LZ4KD delta against that reference. SDDC validates slot mutation and reference
generation at every commit, read, and writeback boundary. Before ZMS
writeback, aliases and deltas are flattened back to an ordinary zram stream,
so the resident reference graph is never persisted. See
[sddc/DESIGN.md](sddc/DESIGN.md) for the representation, wire format,
ownership, locking, accounting, and recovery invariants.

### 2.4 Control Plane

The control plane coordinates:

- automatic writeback policy;
- force swapout requests;
- force swapin and batch-in requests;
- daily writeback quota;
- device lifetime related controls;
- zram pressure based target selection;
- multi-zram target selection;
- pause/resume behavior during reconfiguration or teardown;
- last-operation snapshots and counters.

Automatic policy is best-effort. It evaluates memory pressure, swap availability, zram state, quota, backoff windows, and memcg candidates before dispatching writeback work. By default, the old Hybridswap memcg score policy from `memory.swapd_memcgs_param` is disabled, so automatic memcg writeback is not selected or weighted by that legacy parameter set unless `CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM` is enabled.

### 2.5 Key Operation Paths

#### Normal zram I/O

User-space reads and writes still go through the zram block device. Pages may stay compressed in memory or be served from the backing device if they were previously written back.

#### Writeback

Writeback scans zram slots according to the requested mode, such as idle pages, huge pages, huge idle pages, incompressible pages, or a selected page index. Eligible compressed objects are marked under writeback, packed by ZMS into backing-device blocks, and then have their slot state updated.

#### Batch-in

Batch-in reads pages back from the backing device and restores them into zram. It validates slot snapshots before committing data so that a page changed during the operation is not overwritten with stale contents.

#### Force swapout

Force swapout is a memcg or global best-effort operation that writes eligible pages to the backing device. It can partially succeed and can stop early because of no matching pages, quota exhaustion, pressure checks, target filtering, or writeback failures.

#### Force swapin

Force swapin is a best-effort batch-in operation. It selects target devices and pages, reads back written-out pages, and validates slot state before replacing in-memory zram entries.

---

## 3. Main Features

| Feature | Description |
|---|---|
| Standard zram ABI | Keeps `/dev/zramX`, `/sys/class/zram-control`, and standard `/sys/block/zramX` nodes such as `disksize`, `reset`, `compact`, `comp_algorithm`, `writeback`, `writeback_limit`, and `bd_stat`. |
| Backing device support | Supports a backing block device for page writeback. Crystal also provides a loop-device bridge for deployments that need late binding or clearer diagnostics. |
| Automatic writeback policy | Uses pressure, zram state, quota, device lifetime controls, and throttling windows to decide when to write back pages. |
| Force swapout | Allows explicit best-effort page writeback from memcg or global control paths. |
| Force swapin / batch-in | Allows written-back pages to be read back into zram with snapshot validation. |
| Force shrink | Provides memcg-level anonymous and file page shrink controls where available. |
| Per-memcg policy and statistics | Exposes cgroup-level pressure, parameters, operation results, and per-application summaries. |
| Multi-zram handling | Selects appropriate zram targets when multiple zram devices are present. |
| SDDC and LZ4KD delta | Optionally deduplicates exact resident streams and stores similar streams as validated deltas while preserving normal zram and ZMS behavior. |
| Pressure notification | Reports low, medium, and critical memory pressure through eventfd notification. |
| Diagnostics | Provides lightweight sysfs statistics, detailed Crystal statistics, operation snapshots, debugfs reports, and kernel logs. |

---

## 4. User Guide

### 4.1 Enabling the Module

A typical configuration enables:

```text
CONFIG_CRYSTAL_HYBRIDSWAP=y
CONFIG_MEMCG=y
CONFIG_CGROUPS=y
CONFIG_ZSMALLOC=y
CONFIG_EVENTFD=y
```

The normal `ZRAM` implementation should not be enabled at the same time. Enable `DEBUG_FS` if detailed debugfs diagnostics are required.

### 4.2 Recommended Initialization Sequence

The following sequence is generic. Replace `<zramX>`, `<backing_dev>`, `<loop_dev>`, and `<cg_path>` with platform-specific values.

```bash
# 1. Create a zram device. Some platforms do this from init scripts.
echo 1 > /sys/class/zram-control/hot_add

# 2. Select compression and size.
echo lz4 > /sys/block/<zramX>/comp_algorithm
echo 4G  > /sys/block/<zramX>/disksize

# 3. Bind a backing device.
echo /dev/<backing_dev> > /sys/block/<zramX>/backing_dev

# Optional: use Crystal loop-device bridge when the deployment requires it.
echo /dev/<loop_dev> > /sys/block/<zramX>/hybridswap_loop_device

# 4. Use the device as swap.
mkswap /dev/<zramX>
swapon /dev/<zramX>

# 5. Enable Crystal controls.
echo 1 > /sys/block/<zramX>/hybridswap_enable
echo 1 > /sys/block/<zramX>/hybridswap_core_enable
echo 0 > /sys/block/<zramX>/hybridswap_swapd_pause

# 6. Optional: tune automatic policy.
echo 1          > /sys/block/<zramX>/hybridswap_dev_life
echo 1000000000 > /sys/block/<zramX>/hybridswap_quota_day
echo 75         > /sys/block/<zramX>/hybridswap_zram_increase

# 7. Optional: configure legacy memcg swapd policy when
# CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM is enabled.
# Read the parameter nodes first and follow the format reported by the kernel.
echo '...' > /sys/fs/cgroup/memory/<cg_path>/memory.swapd_memcgs_param
echo '...' > /sys/fs/cgroup/memory/<cg_path>/memory.swapd_single_memcg_param
```

### 4.3 Common zram sysfs Nodes

| Node | Purpose |
|---|---|
| `disksize` | Sets zram device capacity. |
| `reset` | Resets the zram device. |
| `compact` | Requests zsmalloc compaction. |
| `comp_algorithm` | Selects the compression algorithm. |
| `backing_dev` | Binds a backing block device for writeback. |
| `writeback` | Triggers page writeback. |
| `writeback_limit` | Sets writeback limit. |
| `writeback_limit_enable` | Enables or disables writeback limit enforcement. |
| `sddc_stat` | Shows SDDC manager state, candidate activity, resident references/aliases/deltas, byte savings, and failure counters. It reports zero values when SDDC is unavailable. |
| `bd_stat` | Three-field data-path backing-device statistics using physical ZMS 4K-block units. |
| `zms_stat` | Shows Crystal ZMS backing-store state, packing, block allocation, dirty data, and read-merge diagnostics. |
| `writeback_cold_stat` | Shows age/cold-page selection counters used by automatic writeback when entry access-time tracking is enabled. |

### 4.4 Crystal zram Bridge Nodes

| Node | Purpose |
|---|---|
| `hybridswap_enable` | Main Crystal Hybridswap switch. |
| `hybridswap_core_enable` | Enables or disables core policy while preserving the main switch state. |
| `hybridswap_swapd_pause` | Pauses or resumes automatic swapd-style policy. |
| `hybridswap_loglevel` | Controls module log verbosity. |
| `hybridswap_vmstat` | Shows lightweight runtime state and recent operation information. |
| `hybridswap_crystal_stat` | Shows detailed zram I/O, quota, multi-zram, and pressure statistics. |
| `hybridswap_report` | Shows module capability and diagnostic summary. |
| `hybridswap_stat_snap` | Shows last swapout, swapin, writeback, and batch-in snapshots. |
| `hybridswap_meminfo` | Shows zram and memcg related memory summary. |
| `hybridswap_loop_device` | Records or binds a loop-backed device path where supported. |
| `hybridswap_dev_life` | Controls device-lifetime related policy behavior. |
| `hybridswap_quota_day` | Controls daily writeback quota. |
| `hybridswap_zram_increase` | Controls zram pressure or growth threshold. |

### 4.5 memcg Nodes

Common memcg nodes include:

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

`memory.force_shrink_anon_percent` accepts a percentage from 1 to 100
and requests enough anonymous reclaim to make zram/writeback pages reach
that percentage of the memcg's local anonymous working set. The existing
`memory.force_shrink_file` path keeps a global inactive-file reserve and
stops early when the reserve would fall below 768 MiB.

`memory.erm_avail_buffer_enable` controls whether OSvelte/ERM available
memory watermark overrides participate in the automatic policy. Its default
value comes from `CONFIG_CRYSTAL_HYBRIDSWAP_ERM_AVAIL_BUFFER_DEFAULT_ON`,
which defaults to enabled. `memory.erm_avail_buffer` accepts two MiB values:
`min_avail high_avail`, and requires `high_avail - min_avail >= 64`.
When enabled and valid, Crystal uses them only as the effective
`min_avail_buffers` and `high_avail_buffers` for automatic policy decisions.
The base `memory.avail_buffers` tuple, `free_swap_threshold`,
`zram_wm_ratio`, quota, dev_life, zram gate, backoff/window throttle, and
`force_*` interfaces are not overridden by ERM. `memory.swapd_policy_stat`
shows the base, ERM override, and effective condition source.

`memory.app_uid` is writable for compatibility, but Crystal ignores the
written value and derives the reported value from the tasks currently attached
to the memcg. `-1` means unknown or empty, `-2` means mixed UIDs, and
non-negative values are real Linux UIDs.

When `CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM` is enabled, Crystal also exposes the legacy `memory.swapd_memcgs_param` root cgroup node and `memory.swapd_single_memcg_param` per-memcg node. When that option is disabled, these nodes are hidden and automatic memcg writeback is not controlled by the old score/ratio policy.

When `CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_EMPTY_APIS` is enabled, additional placeholder nodes may appear for compatibility with old user-space probes. These placeholders do not restore old internal hybridswap behavior.

### 4.6 Diagnostics

Recommended diagnostic order:

```bash
cat /sys/block/<zramX>/hybridswap_vmstat
cat /sys/block/<zramX>/hybridswap_crystal_stat
cat /sys/block/<zramX>/hybridswap_stat_snap
cat /sys/block/<zramX>/sddc_stat
cat /sys/kernel/debug/crystal_hybridswap/stats
cat /sys/kernel/debug/crystal_hybridswap/report
dmesg | grep -i hybridswap
```

To confirm whether writeback or batch-in happened, check:

- `last_writeback_*` fields;
- `force_swapout_*` fields;
- `force_swapin_*` fields;
- `zram_bd_*` fields;
- `batchin_*` fields;
- pressure reason fields;
- debugfs counters;
- relevant kernel log messages.

---

## 5. API and ABI

### 5.1 Standard zram ABI

Crystal Hybridswap keeps the standard zram-facing ABI:

- `/dev/zramX` block devices;
- `/sys/class/zram-control` hot-add and management interface;
- standard `/sys/block/zramX` configuration nodes;
- standard writeback-facing nodes such as `backing_dev`, `writeback`, `writeback_limit`, `writeback_limit_enable`, and `bd_stat`.

`bd_stat` intentionally keeps the three-field output, but Crystal/ZMS reports
data-path physical 4K backing-block units there: current written-back blocks,
cumulative physical readback blocks, and cumulative physical writeback blocks.
ZMS internal maintenance I/O such as compaction is not included in `bd_stat`;
full ZMS physical I/O counters stay in `zms_stat`.

### 5.2 Crystal Extension Groups

Crystal-specific interfaces are grouped as:

1. zram bridge nodes: `hybridswap_vmstat`, `hybridswap_crystal_stat`, `hybridswap_report`, `hybridswap_stat_snap`, and related controls.
2. zram data-path diagnostics: the read-only `sddc_stat`, `zms_stat`, and `writeback_cold_stat` nodes.
3. memcg bridge nodes: `memory.force_swapout`, `memory.force_swapin`, `memory.force_shrink_*`, `memory.swapd_*`, and per-application summary nodes.
4. pressure eventfd interface: registration and notification for low, medium, and critical pressure levels.
5. debugfs interface: detailed counters, snapshots, and diagnostic reports.

### 5.3 Compatibility Placeholder APIs

`CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_EMPTY_APIS` is intended only for user-space compatibility when old scripts probe for legacy node names. The option is disabled by default. When enabled, those nodes are placeholders or saved-state views and do not imply old internal data-path compatibility.

### 5.4 Legacy memcg swapd Policy ABI

`CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM` controls the old Hybridswap `memory.swapd_memcgs_param` policy interface as a separate compatibility feature. It is disabled by default. When disabled, `memory.swapd_memcgs_param` and `memory.swapd_single_memcg_param` are not exposed, and Crystal automatic memcg writeback does not use the old score and ratio policy for candidate selection or weighting. `memory.app_score` remains available as a basic per-memcg value used by other compatibility and diagnostic paths; without the legacy policy option it does not by itself enable the old automatic memcg writeback policy.

When the option is enabled, `memory.swapd_memcgs_param` accepts the old level-based format: level count followed by per-level `min_score`, `max_score`, `ub_mem2zram_ratio`, `ub_zram2ufs_ratio`, and `refault_threshold` values. The fields that affect Crystal automatic memcg zram-to-UFS writeback are the score interval and `ub_zram2ufs_ratio`; the score interval selects matching memcgs and `ub_zram2ufs_ratio` contributes to candidate weighting and budget distribution. `ub_mem2zram_ratio` and `refault_threshold` are kept mainly as old ABI compatibility and display fields.

---

## 6. Differences from OPPO Official Hybridswap

This section explains design differences for Crystal Hybridswap users. The main distinction is scope: OPPO official Hybridswap is a complete vendor implementation with its own internal object model, while Crystal Hybridswap is a zram-compatible re-architecture that keeps useful external behavior and replaces the internal data path.

| Area | Crystal Hybridswap | OPPO official Hybridswap |
|---|---|---|
| Functional value | Keeps the practical Hybridswap user experience while simplifying internals for generic kernel maintenance. | Provides a complete and mature Hybridswap feature set with automatic policy, per-memcg controls, and integrated writeback/readback. |
| External contract | Keeps standard zram ABI and adds Crystal extension nodes. | Uses OPPO-specific Hybridswap behavior and private interfaces. |
| Internal state model | Uses zram slot state, ZMS compressed-object packing, and explicit writeback/batch-in ownership. | Uses an extent/rmap/object-style model with reclaim-in, batch-out, pre-out, and fault-out scenes. |
| Coupling model | Separates zram data path, core worker, memcg policy, pressure notification, and statistics. | Strongly couples zram slot state, backing storage, memcg mapping, rmap entries, and extent lifetime. |
| Writeback/readback model | Writes back selected compressed zram objects and restores pages through slot snapshot validation. | Uses the original vendor reclaim and recovery paths built around extent objects and scene-specific state transitions. |
| Policy engine | Uses Crystal core policy with pressure, quota, throttling, and multi-zram selection. | Uses the official vendor policy model coupled to its internal state. |
| Maintenance model | Favors explicit ownership, smaller layers, auditable counters, and localized failure reporting. | Requires many readback, writeback, recovery, and memcg changes to preserve shared implicit invariants. |
| Diagnostics | Separates standard zram ABI, Crystal sysfs stats, operation snapshots, debugfs reports, and kernel logs. | Uses the official vendor diagnostics model. |
| Compatibility stance | Preserves zram usability and selected user-space node compatibility without preserving old internal implementation identity. | Preserves the official OPPO implementation semantics. |

The practical result is that Crystal Hybridswap behaves like a zram-compatible system with an additional Crystal control plane. It is not an implementation of the OPPO internal extent/rmap/fault-out model. Developers should treat the two as externally related but internally different designs.

---

## 7. Security and Limitations

- The backing device must be selected carefully. Crystal Hybridswap does not make an unsuitable storage device safe for swap workloads.
- Force swapout and force swapin are best-effort operations. Partial success, no matching pages, quota exhaustion, pressure filtering, snapshot mismatch, and I/O failure are expected outcomes.
- Standard zram ABI compatibility takes priority for standard nodes. Crystal-specific information belongs in Crystal extension nodes or debugfs.
- debugfs is intended for development and deep diagnostics, not as a stable production ABI.
- Compatibility placeholder APIs are disabled by default and should only be enabled when required by user space.
- The legacy `memory.swapd_memcgs_param` policy ABI is disabled by default; enable it only when old user space needs that control surface and its score/`ub_zram2ufs_ratio` automatic memcg writeback behavior.
- SDDC is enabled by default when its Kconfig dependencies are met, requires 4 KiB pages and a delta-capable primary compressor, and keeps its delta wire format resident-only. `sddc_stat` reports whether a manager is active; allocation or runtime resource failures fall back to ordinary compression.
- The module does not provide the OPPO official internal extent/rmap/fault-out data path.
- Automatic policy decisions depend on runtime pressure, quota, memcg state, and backing-device availability; they should be treated as adaptive rather than deterministic.

---

## 8. Troubleshooting FAQ

### 8.1 Why does `backing_dev` show `none`?

Check the following:

1. The supplied path is a valid block device.
2. The zram device has been initialized and has a non-zero `disksize`.
3. The device is in a state that allows backing-device binding.
4. There are no stale written-back slots preventing late binding.
5. Kernel logs do not report backing-device validation or binding errors.

### 8.2 Why does `bd_stat` have only three fields?

Because `bd_stat` keeps the three-field ABI shape. Under Crystal/ZMS, those
fields use data-path physical 4K-block units and exclude internal ZMS
maintenance I/O. Use `zms_stat` for full ZMS physical read/write I/O, and
`hybridswap_crystal_stat`, `hybridswap_stat_snap`, and debugfs for other
Crystal-specific detailed counters.

### 8.3 Why are `hybridswap_vmstat` and `hybridswap_crystal_stat` separate?

`hybridswap_vmstat` is a lightweight view for current state and recent operations. `hybridswap_crystal_stat` is a broader diagnostic view covering zram I/O, quota, multi-zram behavior, and pressure state.

### 8.4 Why are force swapout and force swapin not exact megabyte operations?

Requests are converted to pages and then filtered by slot state, target cgroup, pressure conditions, writeback limits, snapshot state, and I/O results. Page-level best-effort execution is not an exact byte-for-byte transaction.

### 8.5 Why was automatic writeback skipped?

Common reasons include:

- `hybridswap_enable` is disabled;
- `hybridswap_core_enable` is disabled;
- `hybridswap_swapd_pause` is enabled;
- no backing device is available;
- zram pressure is below the configured threshold;
- the policy is in an empty-result backoff window;
- daily quota or window throttling blocks the operation;
- no memcg candidate is available;
- the selected device has no eligible pages.

### 8.6 How can I confirm that writeback or batch-in happened?

Use multiple evidence sources:

- `hybridswap_vmstat` for the latest operation summary;
- `hybridswap_crystal_stat` for accumulated counters;
- `hybridswap_stat_snap` for last-operation snapshots;
- debugfs stats for detailed counters;
- kernel logs for operation results and pressure messages.

---

## 9. Developer Maintenance Guide

### 9.1 Responsibility Map

| Area | Maintenance focus |
|---|---|
| Kconfig | Keep feature symbols, dependencies, defaults, and compatibility options explicit. |
| Makefile | Keep module object composition aligned with enabled features. |
| zram driver | Preserve standard zram ABI, slot-state correctness, compression behavior, writeback, batch-in, and device lifetime safety. |
| zram bridge | Keep Crystal sysfs controls, statistics, reports, and operation snapshots accurate and user-readable. |
| core worker | Maintain asynchronous work ordering, automatic policy, quota accounting, force operations, and multi-zram selection. |
| memcg interface | Maintain cgroup control semantics, parameter parsing, force operations, and per-memcg statistics. |
| pressure interface | Maintain eventfd registration, notification levels, and pressure reason reporting. |
| debugfs stats | Maintain detailed counters and diagnostic reports without treating debugfs as stable ABI. |
| compression layer | Maintain compressor selection, stream handling, and compatibility with zram data-plane expectations. |
| SDDC layer | Maintain immutable-reference ownership, slot and cookie ABA protection, resident decode/flatten behavior, and exact payload-byte accounting. |

### 9.2 Core Invariants

When changing the module, preserve these invariants:

1. zram device lifetime must remain safe across reset, removal, and asynchronous work.
2. Page-level slot state must be changed only while holding the proper slot protection.
3. `ZRAM_UNDER_WB` ownership must be explicit; the owner of writeback or batch-in work is responsible for clearing it.
4. Batch-in must validate slot snapshots before committing restored data.
5. Automatic policy must respect pause, disable, teardown, and reconfiguration states.
6. Pending asynchronous work should keep snapshot-style semantics unless the queueing model is intentionally redesigned.
7. Statistics should use clear units such as pages, bytes, nanoseconds, milliseconds, counts, or ratios.
8. Standard zram nodes must remain compatible; Crystal-specific data should stay in Crystal extension nodes or debugfs.
9. Compatibility placeholder APIs must not silently acquire real old-data-path semantics.
10. Legacy `memory.swapd_memcgs_param` policy behavior must remain behind `CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM`; when that option is disabled it must not influence automatic memcg writeback.
11. SDDC source/target work must validate the complete captured identity before commit, and writeback must validate both zram and SDDC snapshots.
12. SDDC references must be released through slot ownership; teardown must not force-drain a live reference.

### 9.3 Maintenance Recommendations

- Add new user-visible behavior through Crystal extension nodes when it is not part of the standard zram ABI.
- Keep `bd_stat` compatible with standard zram expectations.
- Keep debugfs output useful for diagnosis, but do not document it as a stable ABI.
- Keep automatic policy decisions observable through statistics and last-operation reasons.
- Keep memcg parameter parsing strict enough to reject malformed input but readable enough for platform scripts.
- Keep multi-zram selection deterministic for the same runtime state.
- Keep documentation focused on current module behavior, supported interfaces, and maintenance invariants.
