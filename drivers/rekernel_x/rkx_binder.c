/*
 * Copyright (c) 2026 myflavor <admin@myflv.cn>. All rights reserved.
 * Based on Re-Kernel project by nep_timeline@outlook.com.
 * File: rkx_binder.c — Binder kprobe hooks & freeze detection.
 *
 * built-in 移植说明:目标内核(ReKernel 6.6.118-4k)未启用
 * CONFIG_ANDROID_VENDOR_HOOKS,原 android_vh_binder_* tracepoint 不存在,
 * 触发源替换为同语义 kprobe(见 register_binder / kp_* 注释),
 * 事件构造与判断逻辑与原 LKM 保持一致。
 */

#include "rkx_log.h"
#include "rkx.h"
#include <linux/printk.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/kprobes.h>
#include "../android/binder_internal.h"

/*
 * binder_alloc_copy_from_buffer() 由 drivers/android/binder_alloc.c
 * 导出(EXPORT_SYMBOL_GPL),built-in 同镜像内可直接链接。
 * 用于从内核 binder buffer 读取 transaction 数据(原 LKM 读用户空间
 * tr->data.ptr.buffer,内容一致;binder_proc_transaction 时刻数据已复制
 * 进内核 buffer,且目标进程的用户映射在当前上下文不可用,故读内核侧)。
 */
extern int binder_alloc_copy_from_buffer(struct binder_alloc *alloc, void *dest,
					 struct binder_buffer *buffer,
					 binder_size_t buffer_offset,
					 size_t bytes);

/*
 * line_binder_alloc_new_buf_locked — 原 android_vh_binder_alloc_new_buf_locked
 * 回调等价实现。alloc 直传(原实现从 free_async_space 经 container_of 取回),
 * size 语义与 vh 调用点一致(buffer 总大小)。
 */
static void line_binder_alloc_new_buf_locked(struct binder_alloc *alloc,
					     size_t size, int is_async)
{
	struct task_struct *p = NULL;

	if (is_async
		&& (alloc->free_async_space < 3 * (size + sizeof(struct binder_buffer))
		|| (alloc->free_async_space < WARN_AHEAD_SPACE))) {
		rcu_read_lock();
		p = find_task_by_vpid(alloc->pid);
		rcu_read_unlock();
		if (p != NULL && line_is_frozen(p)) {
			rkx_log_debug("Binder Free buffer full! from=%d | target=%d\n", task_uid(current).val, task_uid(p).val);
			struct rkx_event event = {
				.type = RKX_EVT_BINDER,
				.u.binder = {
					.binder_type = RKX_BINDER_FREE_BUFFER_FULL,
					.oneway = 1,
					.from_pid = task_tgid_nr(current),
					.from_uid = task_uid(current).val,
					.target_pid = task_tgid_nr(p),
					.target_uid = task_uid(p).val,
					.code = -1,
					.rpc_name = "FREE_BUFFER_FULL",
				},
			};
			rkx_send_event(&event);
		}
	}
}

/*
 * line_binder_reply — 原 android_vh_binder_reply 回调等价实现。
 * target_proc 由 binder_transaction_pre 无锁解析(见下)。
 */
static void line_binder_reply(struct binder_proc *target_proc, struct binder_proc *proc,
	struct binder_thread *thread, struct binder_transaction_data *tr)
{
	if (target_proc
		&& (NULL != target_proc->tsk)
		&& (NULL != proc->tsk)
		&& (task_uid(target_proc->tsk).val <= MAX_SYSTEM_UID)
		&& (proc->pid != target_proc->pid)
		&& line_is_frozen(target_proc->tsk)) {
		rkx_log_debug("Sync Binder Reply! from=%d | target=%d\n", task_uid(proc->tsk).val, task_uid(target_proc->tsk).val);
		struct rkx_event event = {
			.type = RKX_EVT_BINDER,
			.u.binder = {
				.binder_type = RKX_BINDER_REPLY,
				.from_pid = task_tgid_nr(proc->tsk),
				.from_uid = task_uid(proc->tsk).val,
				.target_pid = task_tgid_nr(target_proc->tsk),
				.target_uid = task_uid(target_proc->tsk).val,
				.code = -1,
				.rpc_name = "SYNC_BINDER_REPLY",
			},
		};
		rkx_send_event(&event);
	}
}

/*
 * rkx_hook_binder_transaction — 原 android_vh_binder_trans 回调等价实现,
 * 由 rkx_binder_kp.c 的 binder_proc_transaction kprobe pre_handler 调用。
 *
 * 语义对齐说明(相对原 vh 版本):
 *  - vh 触发于 binder_transaction() 内,此时 target_proc 需加锁解析,
 *    kprobe pre_handler 复刻会死锁/悬垂,故改挂 binder_proc_transaction()
 *    (其参数即 target_proc,t 生命期由调用者 tmpref 保证);
 *  - 发送者:原用 binder_transaction 的 proc 参数(发送进程),
 *    binder_proc_transaction 调用链与发送线程同栈,current 即发送者,
 *    task_tgid_nr/task_uid(current) 与原 proc->tsk 取值完全一致;
 *  - tr->flags == t->flags、tr->code == t->code(buffer 分配前已复制);
 *  - rpc_name:原读用户空间 tr->data.ptr.buffer,现读内核 t->buffer
 *    (同一 parcel 内容,无用户空间竞态)。
 */
void rkx_hook_binder_transaction(struct binder_transaction *t,
				 struct binder_proc *target_proc)
{
	if (!(t->flags & TF_ONE_WAY) /* sync binder */
		&& target_proc
		&& (NULL != target_proc->tsk)
		&& (task_uid(current).val > MIN_USERAPP_UID)
		&& (task_tgid_nr(current) != target_proc->pid)
		&& line_is_frozen(target_proc->tsk)) {
		rkx_log_debug("Sync Binder Transaction! from=%d | target=%d\n", task_uid(current).val, task_uid(target_proc->tsk).val);
		struct rkx_event event = {
			.type = RKX_EVT_BINDER,
			.u.binder = {
				.binder_type = RKX_BINDER_TRANSACTION,
				.from_pid = task_tgid_nr(current),
				.from_uid = task_uid(current).val,
				.target_pid = task_tgid_nr(target_proc->tsk),
				.target_uid = task_uid(target_proc->tsk).val,
				.code = -1,
				.rpc_name = "SYNC_BINDER",
			},
		};
		rkx_send_event(&event);
	}

	if ((t->flags & TF_ONE_WAY) /* async binder */
		&& target_proc
		&& (NULL != target_proc->tsk)
		&& (task_uid(current).val > MIN_USERAPP_UID)
		&& (task_tgid_nr(current) != target_proc->pid)
		&& line_is_frozen(target_proc->tsk)) {
		char buf_data[INTERFACETOKEN_BUFF_SIZE];
		char rpc_name[INTERFACETOKEN_BUFF_SIZE] = {0};
		size_t buf_data_size;
		int i = 0, j = 0;

		buf_data_size = t->buffer->data_size > INTERFACETOKEN_BUFF_SIZE ? INTERFACETOKEN_BUFF_SIZE : t->buffer->data_size;
		if (!binder_alloc_copy_from_buffer(&target_proc->alloc, buf_data, t->buffer, 0, buf_data_size)) {
			if (buf_data_size > PARCEL_OFFSET) {
				char *p = (char *)(buf_data) + PARCEL_OFFSET;
				j = PARCEL_OFFSET + 1;
				while (i < INTERFACETOKEN_BUFF_SIZE && j < buf_data_size && *p != '\0') {
					rpc_name[i++] = *p;
					j += 2;
					p += 2;
				}
				if (i == INTERFACETOKEN_BUFF_SIZE) rpc_name[i-1] = '\0';
			}
			rkx_log_debug("ASync Binder Transaction! from=%d | target=%d\n", task_uid(current).val, task_uid(target_proc->tsk).val);
			struct rkx_event event = {
				.type = RKX_EVT_BINDER,
				.u.binder = {
					.binder_type = RKX_BINDER_TRANSACTION,
					.oneway = 1,
					.from_pid = task_tgid_nr(current),
					.from_uid = task_uid(current).val,
					.target_pid = task_tgid_nr(target_proc->tsk),
					.target_uid = task_uid(target_proc->tsk).val,
					.code = t->code,
				},
			};
			strscpy(event.u.binder.rpc_name, rpc_name, sizeof(event.u.binder.rpc_name));
			rkx_send_event(&event);
		}
	}
}

/*
 * binder_alloc_new_buf_locked kprobe — 替代 android_vh_binder_alloc_new_buf_locked。
 * 6.6 签名:binder_alloc_new_buf_locked(alloc, new_buffer, size, is_async),
 * regs[0]=alloc, regs[2]=size, regs[3]=is_async。pre_handler 在函数入口,
 * alloc->free_async_space 尚未被函数修改,与原 vh 调用点(函数体开头)一致。
 */
static int __nocfi binder_alloc_new_buf_locked_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct binder_alloc *alloc = (struct binder_alloc *)regs->regs[0];
	size_t size = (size_t)regs->regs[2];
	int is_async = (int)regs->regs[3];

	if (unlikely(!alloc))
		return 0;

	line_binder_alloc_new_buf_locked(alloc, size, is_async);
	return 0;
}

/*
 * binder_transaction kprobe — 仅处理 reply 分支,替代 android_vh_binder_reply。
 * 6.6 签名:binder_transaction(proc, thread, tr, reply, extra_buffers_size),
 * regs[0]=proc, regs[1]=thread, regs[2]=tr, regs[3]=reply。
 *
 * target_proc 解析(无锁):reply 目标事务在 thread->transaction_stack 栈顶
 * (in_reply_to),其 to_thread->proc 即接收 reply 的进程。该栈仅当前线程
 * 自 binder_transaction/binder_thread_read 修改,pre_handler 运行于函数
 * 入口、事务尚未 pop,故无锁读取安全(不复刻 binder_get_txn_from_and_acq_inner
 * 的加锁路径,避免 kprobe 内重入锁导致死锁)。
 * trans 分支不经由此处处理(binder_proc_transaction 不处理 reply)。
 */
static int __nocfi binder_transaction_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct binder_proc *proc = (struct binder_proc *)regs->regs[0];
	struct binder_thread *thread = (struct binder_thread *)regs->regs[1];
	struct binder_transaction_data *tr = (struct binder_transaction_data *)regs->regs[2];
	int reply = (int)regs->regs[3];
	struct binder_transaction *in_reply_to;
	struct binder_proc *target_proc;

	if (unlikely(!reply || !proc || !thread || !tr))
		return 0;

	in_reply_to = READ_ONCE(thread->transaction_stack);
	if (unlikely(!in_reply_to || !in_reply_to->to_thread || !in_reply_to->to_thread->proc))
		return 0;

	target_proc = in_reply_to->to_thread->proc;
	line_binder_reply(target_proc, proc, thread, tr);
	return 0;
}

static struct rkx_kprobe kp_binder_alloc_new_buf_locked = {
	.symbol = "binder_alloc_new_buf_locked",
	.handler = binder_alloc_new_buf_locked_pre,
};

static struct rkx_kprobe kp_binder_transaction = {
	.symbol = "binder_transaction",
	.handler = binder_transaction_pre,
};

/*
 * 原语义:两个 kprobe 独立注册,任一失败只记日志、不阻塞另一个,
 * 注册函数始终返回成功。故不能合并为一次表注册(合并会在失败时
 * 回滚全部并中断),这里逐个调用统一接口(单元素)保持原行为。
 */
int register_binder(void)
{
	rkx_register_kprobes(&kp_binder_alloc_new_buf_locked, 1);
	rkx_register_kprobes(&kp_binder_transaction, 1);

	return LINE_SUCCESS;
}

void unregister_binder(void)
{
	rkx_unregister_kprobes(&kp_binder_alloc_new_buf_locked, 1);
	rkx_unregister_kprobes(&kp_binder_transaction, 1);
}
