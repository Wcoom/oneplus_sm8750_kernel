/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scan_balance：get_scan_count() 对 anon/file 的扫描配额策略枚举。
 *
 * 原为 mm/vmscan.c 私有定义；vendor hook android_vh_tune_scan_type
 * （include/trace/hooks/vmscan.h）将其作为参数类型暴露给 handler，
 * 迫使 handler（crystal_hybridswap memcg.c 等）复制同构枚举副本并靠
 * 注释保持同步。上移至此共享头，消除副本漂移风险。
 *
 * 值序即语义（SCAN_EQUAL=0 起），不得改动，以免破坏 hook 契约。
 */
#ifndef _LINUX_VMSCAN_BALANCE_H
#define _LINUX_VMSCAN_BALANCE_H

enum scan_balance {
	SCAN_EQUAL,
	SCAN_FRACT,
	SCAN_ANON,
	SCAN_FILE,
};

#endif /* _LINUX_VMSCAN_BALANCE_H */
