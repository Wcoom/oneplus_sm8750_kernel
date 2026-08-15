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

## 首攻项

- 旋钮：schedutil `rate_limit_us`（单旋钮，双向；sysfs：`/sys/devices/system/cpu/cpufreq/policy*/schedutil/rate_limit_us`）
- 第一档数值：0（取消限速）；变坏则降档（半值）
- 范围：全部 cpufreq policy 统一
- 固化：定案后新增 Kconfig 选项（接线点 `kernel/sched/cpufreq_schedutil.c:761`）

## 候选队列（每项走同一循环：单变量 → 1 天浸泡 → 三档 → 无感回退）

1. 调度轴：`rate_limit_us` → `sched_pelt_multiplier`(2/4，写后回读防 vendor veto) → `HZ` 250→1000 → `SCHED_FEAT NEXT_BUDDY/TTWU_QUEUE` → uclamp
2. IO 轴（待调度轴定案后细化）
3. 内存轴（待 IO 轴后细化）
