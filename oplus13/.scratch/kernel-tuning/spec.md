# Spec · 内核优化（泛化压榨）

> 来源：grill-with-docs 会话（2026-08），19 项决策全部与用户定案。
> 领域术语见 `../../CONTEXT.md`；源码层准入见 `../../docs/adr/0001-source-layer-entry-policy.md`。

## 决策摘要

- **目标**：泛化压榨（无具体痛点，全向性能）；体感锚点 = 微信冷启动、桌面/多任务滑动
- **手段**：配置层 → 源码层递进；源码层准入：配置层无收益即可原创（ADR-0001），守 KMI 红线
- **验证**：纯体感，三档判定（变好/变坏/无感），**无感即回退**
- **纪律**：一次一个变量；浸泡期 1 天；浸泡日志见 `浸泡日志.md`
- **落地**：浸泡期 KernelSU service.d 脚本快速 A/B（`99-sched-tune.sh`），定案值固化内核默认
- **顺序**：调度 → IO → 内存；网络轴排除

## 首攻项（2026-08-15 按真机实测重排）

- 旋钮：vendor **walt** governor 的 `zone_max_util_pct`（运行时 governor 为 walt，非 schedutil；schedutil 的 rate_limit_us 不适用且 walt 出厂限速已为 0）
- 第一档：80 → **90**（全部 zone、全部 policy 统一）；无感升档一次 →100；变坏或二次无感判死
- 持久化：service.d 脚本（`99-sched-tune.sh` v2）；**Kconfig 固化不适用**（walt 为 vendor 预编译模块，树内无源码）
- 已判死：schedutil rate_limit_us（walt 出厂即 0）、PELT multiplier（vendor 已设 4，向上无空间）
- **分层路线（用户已确认，2026-08-15）**：vendor 模块旋钮（walt 系列）的固化形态就是 service.d 脚本（树内无源码、Kconfig 无接线点）；树内旋钮（SCHED_FEAT/HZ/uclamp/IO/内存）定案后走真正的内核提交 + 重编译 + 刷机。浸泡期一律脚本 A/B，定案才烧源码。
- 待命候选：hispeed_load（90→70，写通路已验证）、rtg_boost_freq、SCHED_FEAT（debugfs 可挂载，NEXT_BUDDY/TTWU_QUEUE/HRTICK）、HZ 1000

## 候选队列（每项走同一循环：单变量 → 1 天浸泡 → 三档 → 无感回退）

1. 调度轴：`rate_limit_us` → `sched_pelt_multiplier`(2/4，写后回读防 vendor veto) → `HZ` 250→1000 → `SCHED_FEAT NEXT_BUDDY/TTWU_QUEUE` → uclamp
2. IO 轴（待调度轴定案后细化）
3. 内存轴（待 IO 轴后细化）
