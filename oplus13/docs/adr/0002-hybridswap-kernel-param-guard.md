# hybridswap 调优参数采用内核内置守护

crystal_hybridswap 的调优参数（使能、avail_buffers、zram_wm_ratio、erm）默认值固化进内核，并由 fq_guard 式内核守护防止 ColorOS userspace 漂移改写——取代 WebUI 模块的 Go 常驻 daemon（定案于 2026-08-15，用户选定"激进档"）。

- **Status**: accepted
- **Considered Options**: (a) userspace daemon（模块现状）——零内核改动但依赖模块常驻、跨版本脆弱；(b) 保守档（仅固化默认值、无守护）——vendor 反写会使固化失效；(c) 采纳项——与树内 `net/sched/fq_guard.c` 先例一致，与"纯内核层"目标契合。
- **Consequences**: 参数被固定，vendor 按场景动态调节（appmng 场景触发）失效；守护为低频延迟工作 + 有限复查（fq_guard 模式），可经 Kconfig 整体回退；全部改动限于驱动内部 atomic/宏与新增文件，守 KMI 红线。
