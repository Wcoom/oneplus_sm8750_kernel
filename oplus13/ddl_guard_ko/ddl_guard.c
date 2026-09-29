// SPDX-License-Identifier: GPL-2.0
/*
 * ddl_guard.c - 容器作用域的 DDL 动态守护（OnePlus 13 / SM8750 可加载模块）
 *
 * 背景（完整证据见 oplus13/CLAUDE.md 第 22 项）：
 *   厂商永久模块 oplus_bsp_sched_assist 的 update_ddl_hit_history() 用失效 task 的
 *   pid 索引 ddl_sdata[PID_MAX_DEFAULT] 并写入，越界命中 UBSAN 陷阱（brk #0x5512），
 *   因 CONFIG_PANIC_ON_OOPS=y 直接重启（2026-09-11 一天 6 次，minidump 已定位）。
 *   该模块在 vendor_boot 内、缺完整 OEM kernel/oplus_cpu 源码无法安全重编，唯一可用
 *   的止血点就是它导出的 int global_sched_ddl_enabled：
 *     /proc/oplus_scheduler/sched_assist/sched_ddl_enabled 即它的用户态视图，
 *     置 0 后 oplus_ddl_check_preempt()/oplus_replace_next_task_ddl() 整条路径不进入，
 *     update_ddl_hit_history() 自然也不会被调用。
 *
 * 本模块 = 方案 A：把“恒关 DDL”升级为与容器生命周期绑定的被动开关
 *     容器启动 → DDL 关（立即）   容器停止 → linger_ms 后 DDL 开   无容器 → DDL 开
 *   全部锚点都是内核冷路径，零轮询、零常驻线程、零定时唤醒；容器不在时不产生任何写入。
 *
 * 锚点（2026-09-29 真机 ftrace 实测确认，见 README「真机证据」）：
 *   1) kretprobe copy_pid_ns          flags & CLONE_NEWPID 时登记返回的新 pidns
 *   2) kprobe    zap_pid_ns_processes 容器 init 退出时的收尸路径，摘除登记的 ns
 *   3) kprobe    put_pid_ns           兜底：refcount 将归零（对象即将销毁）时摘除
 *   4) 加载时一次 for_each_process 对账 覆盖“模块加载前容器已在运行”
 *
 * 失败即安全（红线，与本项目既有约定一致）：
 *   - 任一探针注册失败 → init 返回错误、模块不加载 → DDL 保持开机脚本写入的 0；
 *   - global_sched_ddl_enabled 解析失败 → 静态重定位失败、insmod 直接报错 → 同上；
 *   - pidns 表满、或 kretprobe 出现漏事件（nmissed>0）→ 粘性按“有容器”处理，绝不开启；
 *   - 卸载不改动当前值（安全侧：DDL 保持卸载瞬间的值，必要时由用户或开机脚本重置）。
 *
 * 已知边界（务必知晓）：
 *   - 容器是崩溃链最可能的触发源但不是唯一：minidump 命中线程里有 kswapd0，
 *     说明内存回收路径也能走到 update_ddl_hit_history()。本模块只覆盖容器作用域。
 *   - “有容器”是全系统判定：任何进程 unshare(CLONE_NEWPID) 都会让 DDL 关闭
 *     （偏安全方向：DDL 关只是少一层调度优化，不会崩）。
 *   - 本模块依赖运行内核的 vermagic/CRC，每次重刷内核都必须重新构建并推送 .ko。
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/pid_namespace.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/err.h>
#include <linux/string.h>
#include <asm/page.h>

/*
 * 厂商模块 oplus_bsp_sched_assist 导出（sa_sysfs.c: EXPORT_SYMBOL(...)），
 * 这里用静态重定位直接引用它：模块加载器在载入时按符号名解析（含已加载模块的
 * 导出表），解析不到则 insmod 当场失败 —— 这正是我们想要的失败模式：
 * DDL 保持开机脚本写入的 0，不会出现“模块以为自己在管、其实管不着”的状态。
 */
extern int global_sched_ddl_enabled;

/* ------------------------------------------------------------------ *
 * 可调参数
 * ------------------------------------------------------------------ */

static int dry_run;		/* 1 = 只记录不写开关（验证用） */
static int policy = 1;		/* 1 = 容器作用域（默认）；0 = 恒关（只关不开） */
static int linger_ms = 3000;	/* 容器停止后延迟多久恢复 DDL */

#define DDLG_LINGER_MAX_MS	600000

/* ------------------------------------------------------------------ *
 * pidns 登记表（全部访问都在 ddlg_lock 下）
 * ------------------------------------------------------------------ */

#define DDLG_MAX_NS	64

static struct pid_namespace *ddlg_ns[DDLG_MAX_NS];
static int ddlg_n;
static bool ddlg_overflow;	/* 表满：粘性按“有容器”处理 */
static bool ddlg_degraded;	/* 锚点不可信（如漏事件）：粘性恒关 */
static DEFINE_SPINLOCK(ddlg_lock);

static int *ddlg_flag;		/* &global_sched_ddl_enabled */
static int ddlg_applied;	/* 最近一次写入值 */

static int ddlg_state_get(char *buf, const struct kernel_param *kp);

/* 返回被摘除元素的下标，未找到返回 -1；需持锁 */
static int __ddlg_find(struct pid_namespace *ns)
{
	int i;

	for (i = 0; i < ddlg_n; i++)
		if (ddlg_ns[i] == ns)
			return i;
	return -1;
}

/* 返回 1 表示“表由空变非空”；需持锁 */
static int __ddlg_track(struct pid_namespace *ns)
{
	if (!ns || ns == &init_pid_ns)
		return 0;

	if (__ddlg_find(ns) >= 0)
		return 0;

	if (ddlg_n >= DDLG_MAX_NS) {
		if (!ddlg_overflow) {
			ddlg_overflow = true;
			pr_warn("pidns 表已满(%d)，转为粘性按“有容器”处理\n",
				DDLG_MAX_NS);
			return 1;
		}
		return 0;
	}

	ddlg_ns[ddlg_n++] = ns;
	pr_info("登记容器 pidns %px（表内 %d 个）\n", ns, ddlg_n);

	return ddlg_n == 1;
}

/* 返回 1 表示“表由非空变空且无不确定因素”；需持锁 */
static int __ddlg_remove_at(int idx, const char *why)
{
	struct pid_namespace *ns = ddlg_ns[idx];

	ddlg_ns[idx] = ddlg_ns[--ddlg_n];
	pr_info("摘除容器 pidns %px（%s，表内剩 %d 个）\n", ns, why, ddlg_n);

	return ddlg_n == 0 && !ddlg_overflow;
}

/* 当前是否应按“有容器”处理；需持锁 */
static bool __ddlg_busy(void)
{
	return ddlg_n > 0 || ddlg_overflow;
}

/* ------------------------------------------------------------------ *
 * 唯一的状态出口：延迟 work
 * ------------------------------------------------------------------ */

static struct kretprobe ddlg_kr_copy_pid_ns;	/* 定义见「锚点 1」 */

static void ddlg_apply(struct work_struct *work);
static DECLARE_DELAYED_WORK(ddlg_work, ddlg_apply);

/*
 * 由探针回调（原子上下文）与参数写入（进程上下文）调用。
 * mod_delayed_work() 文档明确“safe to call from any context including IRQ handler”，
 * 且 delay=0 时“guaranteed to be scheduled immediately regardless of its current state”。
 */
static void ddlg_kick(bool urgent)
{
	struct workqueue_struct *wq = system_power_efficient_wq;

	if (urgent)
		mod_delayed_work(wq, &ddlg_work, 0);
	else
		mod_delayed_work(wq, &ddlg_work,
				 msecs_to_jiffies(linger_ms));
}

static void ddlg_apply(struct work_struct *work)
{
	unsigned long flags;
	bool busy;
	int want;

	if (unlikely(!ddlg_flag))
		return;

	/* 漏事件说明“无容器”这个判断不可靠 → 粘性恒关 */
	if (ddlg_kr_copy_pid_ns.nmissed && !ddlg_degraded) {
		ddlg_degraded = true;
		pr_err("kretprobe 漏事件 %d 次，判定不可信，粘性恒关 DDL\n",
		       ddlg_kr_copy_pid_ns.nmissed);
	}

	spin_lock_irqsave(&ddlg_lock, flags);
	busy = __ddlg_busy();
	spin_unlock_irqrestore(&ddlg_lock, flags);

	if (ddlg_degraded || !policy)
		want = 0;
	else
		want = busy ? 0 : 1;

	if (dry_run) {
		pr_info("[dry-run] 容器=%d 期望 DDL=%d（未写入，当前=%d）\n",
			busy, want, READ_ONCE(*ddlg_flag));
		return;
	}

	if (want == ddlg_applied)
		return;

	WRITE_ONCE(*ddlg_flag, want);
	ddlg_applied = want;
	pr_info("DDL %s（容器=%d）\n", want ? "开启" : "关闭", busy);
}

/* ------------------------------------------------------------------ *
 * 锚点 1：copy_pid_ns —— 容器 PID 命名空间的创建
 * ------------------------------------------------------------------ */

static int ddlg_copy_pid_ns_entry(struct kretprobe_instance *ri,
				  struct pt_regs *regs)
{
	unsigned long nsflags = regs_get_kernel_argument(regs, 0);

	/* 只关心 CLONE_NEWPID；mount/uts/ipc/net 命名空间的高频噪声直接跳过 */
	if (!(nsflags & CLONE_NEWPID))
		return 1;	/* 非 0 = 不调用对应的返回处理 */

	memcpy(ri->data, &nsflags, sizeof(nsflags));
	return 0;
}

static int ddlg_copy_pid_ns_ret(struct kretprobe_instance *ri,
				struct pt_regs *regs)
{
	struct pid_namespace *ns =
		(struct pid_namespace *)regs_return_value(regs);
	unsigned long flags;
	int urgent = 0;

	if (IS_ERR_OR_NULL(ns))
		return 0;

	spin_lock_irqsave(&ddlg_lock, flags);
	urgent = __ddlg_track(ns);
	spin_unlock_irqrestore(&ddlg_lock, flags);

	if (urgent)
		ddlg_kick(true);	/* 容器出现 → 立即关 */

	return 0;
}

static struct kretprobe ddlg_kr_copy_pid_ns = {
	.kp.symbol_name	= "copy_pid_ns",
	.entry_handler	= ddlg_copy_pid_ns_entry,
	.handler	= ddlg_copy_pid_ns_ret,
	.data_size	= sizeof(unsigned long),
	.maxactive	= 64,
};

/* ------------------------------------------------------------------ *
 * 锚点 2：zap_pid_ns_processes —— 容器 init 退出后的收尸
 * ------------------------------------------------------------------ */

static int ddlg_zap_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct pid_namespace *ns =
		(struct pid_namespace *)regs_get_kernel_argument(regs, 0);
	unsigned long flags;
	int idx, empty = 0;

	if (READ_ONCE(ddlg_n) == 0)
		return 0;	/* 无容器时的快速路径：一次原子读，不取锁 */

	if (!ns)
		return 0;

	spin_lock_irqsave(&ddlg_lock, flags);
	idx = __ddlg_find(ns);
	if (idx >= 0)
		empty = __ddlg_remove_at(idx, "zap");
	spin_unlock_irqrestore(&ddlg_lock, flags);

	if (empty)
		ddlg_kick(false);	/* 容器收尸 → linger_ms 后再开 */

	return 0;
}

static struct kprobe ddlg_kp_zap = {
	.symbol_name	= "zap_pid_ns_processes",
	.pre_handler	= ddlg_zap_pre,
};

/* ------------------------------------------------------------------ *
 * 锚点 3：put_pid_ns —— 兜底，防表泄漏（对象即将销毁时摘除）
 * ------------------------------------------------------------------ */

static int ddlg_put_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct pid_namespace *ns =
		(struct pid_namespace *)regs_get_kernel_argument(regs, 0);
	unsigned long flags;
	int idx, empty = 0;

	/*
	 * 稳态（无容器）下这是每个进程退出都会走的热路径，先做无锁快路径。
	 * 只有表非空（= 有容器在跑）时才需要看这个锚点。
	 */
	if (READ_ONCE(ddlg_n) == 0)
		return 0;

	if (!ns || ns == &init_pid_ns)
		return 0;

	spin_lock_irqsave(&ddlg_lock, flags);
	idx = __ddlg_find(ns);
	/*
	 * put_pid_ns() 内部是 refcount_dec_and_test()：此处 pre_handler 读到的
	 * count 还没减。count == 1 意味着本次调用后对象即被销毁，必须摘除；
	 * count > 1 说明对象仍存活（可能容器还在跑），保持登记。
	 */
	if (idx >= 0 && refcount_read(&ns->ns.count) == 1)
		empty = __ddlg_remove_at(idx, "put");
	spin_unlock_irqrestore(&ddlg_lock, flags);

	if (empty)
		ddlg_kick(false);

	return 0;
}

static struct kprobe ddlg_kp_put = {
	.symbol_name	= "put_pid_ns",
	.pre_handler	= ddlg_put_pre,
};

/* ------------------------------------------------------------------ *
 * 对账：覆盖“模块加载前容器已在运行”
 * ------------------------------------------------------------------ */

static void ddlg_scan_existing(void)
{
	struct task_struct *p;
	unsigned long flags;

	rcu_read_lock();
	spin_lock_irqsave(&ddlg_lock, flags);
	for_each_process(p) {
		struct pid_namespace *ns = task_active_pid_ns(p);

		if (!ns || ns == &init_pid_ns)
			continue;
		__ddlg_track(ns);
	}
	spin_unlock_irqrestore(&ddlg_lock, flags);
	rcu_read_unlock();

	if (ddlg_n)
		pr_warn("加载时已有 %d 个容器 pidns 在用，按“容器运行中”处理\n",
			ddlg_n);
}

/* ------------------------------------------------------------------ *
 * 参数
 * ------------------------------------------------------------------ */

static int ddlg_param_set(const char *val, const struct kernel_param *kp)
{
	int ret;

	ret = param_set_int(val, kp);
	if (ret)
		return ret;

	if (linger_ms < 0)
		linger_ms = 0;
	if (linger_ms > DDLG_LINGER_MAX_MS)
		linger_ms = DDLG_LINGER_MAX_MS;
	dry_run = !!dry_run;
	policy = !!policy;

	ddlg_kick(true);	/* 参数变了立刻重估一次 */
	return 0;
}

static const struct kernel_param_ops ddlg_int_ops = {
	.set	= ddlg_param_set,
	.get	= param_get_int,
};

module_param_cb(dry_run, &ddlg_int_ops, &dry_run, 0644);
MODULE_PARM_DESC(dry_run, "1=只记录不写开关（验证用），默认 0");

module_param_cb(policy, &ddlg_int_ops, &policy, 0644);
MODULE_PARM_DESC(policy, "1=容器作用域（默认），0=恒关（只关不开）");

module_param_cb(linger_ms, &ddlg_int_ops, &linger_ms, 0644);
MODULE_PARM_DESC(linger_ms, "容器停止后延迟多少毫秒恢复 DDL，默认 3000");

static int ddlg_state_get(char *buf, const struct kernel_param *kp)
{
	unsigned long flags;
	int n, overflow;

	spin_lock_irqsave(&ddlg_lock, flags);
	n = ddlg_n;
	overflow = ddlg_overflow;
	spin_unlock_irqrestore(&ddlg_lock, flags);

	return scnprintf(buf, PAGE_SIZE,
			 "ns=%d overflow=%d degraded=%d policy=%d dry_run=%d linger_ms=%d applied=%d ddl=%d nmissed=%d\n",
			 n, overflow, ddlg_degraded, policy, dry_run, linger_ms,
			 ddlg_applied,
			 ddlg_flag ? READ_ONCE(*ddlg_flag) : -1,
			 ddlg_kr_copy_pid_ns.nmissed);
}

static const struct kernel_param_ops ddlg_state_ops = {
	.get	= ddlg_state_get,
};

module_param_cb(state, &ddlg_state_ops, NULL, 0444);
MODULE_PARM_DESC(state, "只读：当前判定状态");

/* ------------------------------------------------------------------ *
 * init / exit
 * ------------------------------------------------------------------ */

static int __init ddlg_init(void)
{
	int ret;

	ddlg_flag = &global_sched_ddl_enabled;
	ddlg_applied = READ_ONCE(*ddlg_flag);

	/*
	 * 任何一步失败都直接让 insmod 失败：模块不加载 = DDL 保持开机脚本写下的 0，
	 * 这是本项目约定的安全缺省，绝不出现“半管不管”的中间态。
	 */
	ret = register_kretprobe(&ddlg_kr_copy_pid_ns);
	if (ret < 0) {
		pr_err("copy_pid_ns kretprobe 注册失败: %d\n", ret);
		return ret;
	}

	ret = register_kprobe(&ddlg_kp_zap);
	if (ret < 0) {
		pr_err("zap_pid_ns_processes kprobe 注册失败: %d\n", ret);
		goto err_kr;
	}

	ret = register_kprobe(&ddlg_kp_put);
	if (ret < 0) {
		pr_err("put_pid_ns kprobe 注册失败: %d\n", ret);
		goto err_zap;
	}

	ddlg_scan_existing();
	ddlg_kick(true);

	pr_info("已加载：锚点 copy_pid_ns/zap_pid_ns_processes/put_pid_ns 就绪，"
		"当前 DDL=%d，linger_ms=%d，dry_run=%d，policy=%d\n",
		READ_ONCE(*ddlg_flag), linger_ms, dry_run, policy);
	return 0;

err_zap:
	unregister_kprobe(&ddlg_kp_zap);
err_kr:
	unregister_kretprobe(&ddlg_kr_copy_pid_ns);
	return ret;
}

static void __exit ddlg_exit(void)
{
	cancel_delayed_work_sync(&ddlg_work);

	unregister_kprobe(&ddlg_kp_put);
	unregister_kprobe(&ddlg_kp_zap);
	unregister_kretprobe(&ddlg_kr_copy_pid_ns);

	/*
	 * 卸载是个“放弃管辖”的动作：不动开关，保持卸载瞬间的值（安全侧）。
	 * 若此时正因容器而关闭，DDL 将一直保持关闭，直到有人重置或重启。
	 */
	pr_info("已卸载：DDL 保持当前值 %d 不变\n", READ_ONCE(*ddlg_flag));
}

module_init(ddlg_init);
module_exit(ddlg_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("容器作用域的 DDL 动态守护（Droidspaces 容器运行时关闭厂商 DDL）");
MODULE_AUTHOR("Wcoom");
MODULE_VERSION("1.0.0");
