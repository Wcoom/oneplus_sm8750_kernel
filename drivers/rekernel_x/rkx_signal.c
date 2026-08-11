/*
 * Copyright (c) 2026 myflavor <admin@myflv.cn>. All rights reserved.
 * Based on Re-Kernel project by nep_timeline@outlook.com.
 * File: rkx_signal.c — Signal kprobe hooks & frozen task mitigation.
 *
 * built-in 移植说明:目标内核(ReKernel 6.6.118-4k)未启用
 * CONFIG_ANDROID_VENDOR_HOOKS,原 android_vh_do_send_sig_info tracepoint
 * 不存在,触发源替换为 do_send_sig_info() kprobe(该函数非 static 且
 * EXPORT_SYMBOL_GPL,kallsyms 必然可见)。事件构造与判断逻辑不变。
 */

#include "rkx_log.h"
#include "rkx.h"
#include <linux/printk.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/kprobes.h>

static void line_signal(int sig, struct task_struct *killer, struct task_struct *dst)
{
	if (!dst || !killer)
		return;

	if (line_is_frozen(dst) &&
			(sig == SIGKILL
			|| sig == SIGTERM
			|| sig == SIGABRT
			|| sig == SIGQUIT)) {
		rkx_log_debug("Process Signal! signal=%d\n", sig);
		struct rkx_event event = {
			.type = RKX_EVT_SIGNAL,
			.u.signal = {
				.signal = sig,
				.killer_pid = task_tgid_nr(killer),
				.killer_uid = task_uid(killer).val,
				.dst_pid = task_tgid_nr(dst),
				.dst_uid = task_uid(dst).val,
			},
		};
		rkx_send_event(&event);
	}
}

/*
 * do_send_sig_info kprobe — 替代 android_vh_do_send_sig_info。
 * 6.6 签名:do_send_sig_info(sig, info, p, type),regs[0]=sig, regs[2]=p(dst)。
 * killer ≡ current,与目标内核 vh 调用点
 * trace_android_vh_do_send_sig_info(sig, current, p) 完全一致。
 */
static int __nocfi do_send_sig_info_pre(struct kprobe *p, struct pt_regs *regs)
{
	int sig = (int)regs->regs[0];
	struct task_struct *dst = (struct task_struct *)regs->regs[2];

	line_signal(sig, current, dst);
	return 0;
}

static struct rkx_kprobe kp_do_send_sig_info = {
	.symbol = "do_send_sig_info",
	.handler = do_send_sig_info_pre,
};

int register_signal(void)
{
	return rkx_register_kprobes(&kp_do_send_sig_info, 1);
}

void unregister_signal(void)
{
	rkx_unregister_kprobes(&kp_do_send_sig_info, 1);
}
