/*
 * Copyright (c) 2026 myflavor <admin@myflv.cn>. All rights reserved.
 * Based on Re-Kernel project by nep_timeline@outlook.com.
 * File: rkx_kprobe.c — Unified kprobe registration framework.
 *
 * 收敛 rkx_signal / rkx_binder / rkx_binder_kp 三处事件源重复的
 * "static struct kprobe + static bool 注册标志 + register/unregister 函数对"
 * 样板：事件源只声明 struct rkx_kprobe 表（符号名 + 回调），
 * 注册/注销、registered 状态与失败回滚统一由本文件处理。
 */

#include "rkx_log.h"
#include "rkx.h"
#include <linux/printk.h>
#include <linux/kprobes.h>

int rkx_register_kprobes(struct rkx_kprobe *probes, int n)
{
	int i;
	int rc;

	if (!probes || n <= 0)
		return -EINVAL;

	for (i = 0; i < n; i++) {
		if (probes[i].registered)
			continue;

		probes[i].kp.symbol_name = probes[i].symbol;
		probes[i].kp.pre_handler = probes[i].handler;

		rc = register_kprobe(&probes[i].kp);
		if (rc != LINE_SUCCESS) {
			rkx_log_err("register %s kprobe failed, rc=%d\n",
				    probes[i].symbol, rc);
			/* 回滚已注册项（内部会复位各 probe 的 registered 标志） */
			rkx_unregister_kprobes(probes, i);
			return rc;
		}
		probes[i].registered = true;
	}
	return LINE_SUCCESS;
}

void rkx_unregister_kprobes(struct rkx_kprobe *probes, int n)
{
	int i;

	if (!probes)
		return;

	for (i = 0; i < n; i++) {
		if (!probes[i].registered)
			continue;
		unregister_kprobe(&probes[i].kp);
		probes[i].registered = false;
	}
}
