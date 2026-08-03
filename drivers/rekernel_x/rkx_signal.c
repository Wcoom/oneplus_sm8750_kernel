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

static bool re_signal_hook;

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
		if (rkx_netlink_ready()) {
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
			sendMessage(&event);
		}
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

static struct kprobe kp_do_send_sig_info = {
	.symbol_name = "do_send_sig_info",
	.pre_handler = do_send_sig_info_pre,
};

int register_signal(void)
{
	int rc = LINE_SUCCESS;

	rc = register_kprobe(&kp_do_send_sig_info);
	if (rc != LINE_SUCCESS) {
		rkx_log_err("register do_send_sig_info kprobe failed, rc=%d\n", rc);
		return rc;
	}
	re_signal_hook = true;
	return LINE_SUCCESS;
}

void unregister_signal(void)
{
	if (re_signal_hook) {
		unregister_kprobe(&kp_do_send_sig_info);
		re_signal_hook = false;
	}
}
