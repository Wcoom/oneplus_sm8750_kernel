// SPDX-License-Identifier: GPL-2.0
/*
 * ddl_guard.c - 容器作用域的 DDL 动态守护（OnePlus 13 / SM8750）
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
 *   全部锚点都是内核冷路径，零轮询、零常驻线程；容器不在时不产生任何写入。
 *   唯一的例外是启动期对厂商符号的有限次重试（见下），解析成功后彻底停止。
 *
 * 锚点（2026-09-29 真机 ftrace 实测确认，见 README「真机证据」）：
 *   1) kretprobe copy_pid_ns          flags & CLONE_NEWPID 时登记返回的新 pidns
 *   2) kprobe    zap_pid_ns_processes 容器 init 退出时的收尸路径，摘除登记的 ns
 *   3) kprobe    put_pid_ns           兜底：refcount 将归零（对象即将销毁）时摘除
 *   4) 加载时一次 for_each_process 对账 覆盖“本模块就位前容器已在运行”
 *
 * 两种形态共用本文件（唯一真源在内核树，.ko 目录是指向它的符号链接）：
 *   - 内置（CONFIG_DDL_GUARD=y，默认，MODULE 未定义）：
 *     vmlinux 的链接期**无法**引用厂商模块的符号，只能在运行时用
 *     kallsyms_lookup_name() 按名字解析（本内核 CONFIG_KALLSYMS_ALL=y，
 *     模块的数据符号也在 kallsyms 表里，故能找到 global_sched_ddl_enabled）。
 *     厂商模块由 vendor_boot ramdisk 经用户态 init 加载，晚于所有 initcall，
 *     所以启动期有一个有限次（60 次 × 2s）的重试，解析成功即彻底停止。
 *   - 可加载（CONFIG_DDL_GUARD=m 或 out-of-tree 构建，MODULE 已定义）：
 *     静态 extern 引用，insmod 时由内核按已加载模块的导出表解析，
 *     解析不到则 insmod 直接失败 —— 失败即安全。
 *
 * 失败即安全（红线，与本项目既有约定一致）：
 *   - 任一探针注册失败 → init 返回错误（.ko 形态 = insmod 失败）；
 *   - .ko 形态下 global_sched_ddl_enabled 解析失败 → 静态重定位失败、insmod 报错；
 *   - 内置形态下符号始终解析不到 → 重试耗尽即放弃，**从不写入**，DDL 保持当前值；
 *   - pidns 表满、或 kretprobe 出现漏事件（nmissed>0）→ 粘性按“有容器”处理，绝不开启；
 *   - 卸载不改动当前值（安全侧：DDL 保持卸载瞬间的值，必要时由用户或开机脚本重置）。
 *
 * 自我纠偏：每次重估都与开关的**实时值**比较。外部若改过开关（例如残留的旧保护
 * 脚本开机写了 0），下一次容器事件就会把它纠回本模块的判定结果，不会出现“模块
 * 自认为在管、实际已被覆盖”的状态。重估只在容器事件/参数写入/启动期有限重试时
 * 发生，依旧零轮询。
 *
 * 已知边界（务必知晓）：
 *   - 容器是崩溃链最可能的触发源但不是唯一：minidump 命中线程里有 kswapd0，
 *     说明内存回收路径也能走到 update_ddl_hit_history()。本模块只覆盖容器作用域，
 *     真在无容器时复现就把 policy 设 0 退回恒关。
 *   - “有容器”是全系统判定：任何进程 unshare(CLONE_NEWPID) 都会让 DDL 关闭
 *     （偏安全方向：DDL 关只是少一层调度优化，不会崩）。
 *   - 本模块只负责在容器期间关闭 DDL，**不做**任何越界检查或崩溃兜底。
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
#include <linux/kallsyms.h>
#include <asm/page.h>

/*
 * 内置时 moduleparam.h 把 MODULE_PARAM_PREFIX 定义为空串，参数会全部落进
 * /sys/module/kernel/parameters/（且 state/policy 这类通用名有重名冲突风险，
 * 冲突时 param_sysfs_builtin 里的 BUG_ON 会直接崩）。显式定义成 "ddl_guard."
 * 让两种形态的路径完全一致：
 *   /sys/module/ddl_guard/parameters/{state,policy,linger_ms,dry_run}
 * 这是内核里的标准做法，见 mm/kfence/core.c、kernel/rcu/tree.c。
 */
#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "ddl_guard."

/* ------------------------------------------------------------------ *
 * 厂商符号解析
 * ------------------------------------------------------------------ */

/*
 * 厂商模块 oplus_bsp_sched_assist 导出（sa_sysfs.c: EXPORT_SYMBOL(...)），
 * 即 /proc/oplus_scheduler/sched_assist/sched_ddl_enabled 背后的全局开关。
 */
#define DDLG_SYM	"global_sched_ddl_enabled"

static int *ddlg_flag;			/* 解析成功前为 NULL */
static int ddlg_applied = -1;		/* 最近一次写入值；-1 = 尚未接管 */
static int ddlg_tries;			/* 启动期解析重试计数 */

#ifdef MODULE
extern int global_sched_ddl_enabled;

static bool ddlg_resolve(void)
{
	ddlg_flag = &global_sched_ddl_enabled;
	return true;
}
#else
/*
 * 注意：不能用 __symbol_get()。本树里它只对 GPL_ONLY 符号放行，而该符号是普通
 * EXPORT_SYMBOL，必然失败；kallsyms_lookup_name() 才是可用路径（它会回落到
 * module_kallsyms_lookup_name，覆盖已加载模块的符号）。
 * 只有 CONFIG_KALLSYMS_ALL=y 时模块的数据符号才会留在 kallsyms 表里（见
 * kernel/module/kallsyms.c: is_core_symbol）——本内核已确认开启。
 */
static bool ddlg_resolve(void)
{
	unsigned long addr;

	if (ddlg_flag)
		return true;

	addr = kallsyms_lookup_name(DDLG_SYM);
	if (!addr)
		return false;

	ddlg_flag = (int *)addr;
	pr_info("已解析 " DDLG_SYM " @ %px（当前值 %d）\n",
		ddlg_flag, READ_ONCE(*ddlg_flag));
	return true;
}
#endif

/* ------------------------------------------------------------------ *
 * 可调参数
 * ------------------------------------------------------------------ */

static int dry_run;		/* 1 = 只记录不写开关（验证用） */
static int policy = 1;		/* 1 = 容器作用域（默认）；0 = 恒关（只关不开） */
static int linger_ms = 3000;	/* 容器停止后延迟多久恢复 DDL */

#define DDLG_LINGER_MAX_MS	600000
#define DDLG_RESOLVE_TRIES	60	/* 启动期解析重试次数（× 2s = 120s） */
#define DDLG_RESOLVE_IVL_MS	2000

/* ------------------------------------------------------------------ *
 * pidns 登记表（全部访问都在 ddlg_lock 下）
 * ------------------------------------------------------------------ */

#define DDLG_MAX_NS	64

static struct pid_namespace *ddlg_ns[DDLG_MAX_NS];
static int ddlg_n;
static bool ddlg_overflow;	/* 表满：粘性按“有容器”处理 */
static bool ddlg_degraded;	/* 锚点不可信（如漏事件）：粘性恒关 */
static DEFINE_SPINLOCK(ddlg_lock);

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
	int want, actual;

	/*
	 * 内置形态在这里做启动期的有限次重试：厂商模块由用户态 init 从
	 * vendor_boot ramdisk 加载，晚于所有 initcall。解析成功后再也不会重排，
	 * 不影响“零轮询”的稳态语义。
	 */
	if (!ddlg_resolve()) {
		if (ddlg_tries < DDLG_RESOLVE_TRIES) {
			ddlg_tries++;
			mod_delayed_work(system_power_efficient_wq, &ddlg_work,
					 msecs_to_jiffies(DDLG_RESOLVE_IVL_MS));
		} else if (ddlg_tries++ == DDLG_RESOLVE_TRIES) {
			pr_err("解析 " DDLG_SYM " 失败（重试 %d 次后放弃）："
			       "本模块不再写入，DDL 保持当前值\n",
			       DDLG_RESOLVE_TRIES);
		}
		return;
	}

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

	actual = READ_ONCE(*ddlg_flag);

	if (dry_run) {
		pr_info("[dry-run] 容器=%d 期望 DDL=%d（未写入，当前=%d）\n",
			busy, want, actual);
		return;
	}

	/*
	 * 关键：与**实时值**比较，而不是与上次写入值比较。这样即使开关被外部改过
	 * （残留的旧保护脚本开机写 0、用户手动 echo），下一次重估也会把它纠回本模块
	 * 的判定结果，而不会永久停在“自认为在管、实际已被覆盖”的状态。
	 * 重估只发生在容器事件 / 参数写入 / 启动期有限次重试，不引入任何轮询。
	 */
	if (want == actual)
		return;

	WRITE_ONCE(*ddlg_flag, want);
	ddlg_applied = want;	/* 仅用于 state 诊断：最近一次写入值 */
	pr_info("DDL %s（容器=%d，原值 %d）\n", want ? "开启" : "关闭", busy,
		actual);
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
 * 对账：覆盖“本模块就位前容器已在运行”
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
		pr_warn("启动时已有 %d 个容器 pidns 在用，按“容器运行中”处理\n",
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
			 "resolved=%d ns=%d overflow=%d degraded=%d policy=%d dry_run=%d linger_ms=%d applied=%d ddl=%d nmissed=%d tries=%d\n",
			 ddlg_flag ? 1 : 0,
			 n, overflow, ddlg_degraded, policy, dry_run, linger_ms,
			 ddlg_applied,
			 ddlg_flag ? READ_ONCE(*ddlg_flag) : -1,
			 ddlg_kr_copy_pid_ns.nmissed, ddlg_tries);
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

	/*
	 * 任何一步失败都让 init 失败（.ko 形态 = insmod 失败）：模块不加载 =
	 * DDL 保持开机脚本写下的值，这是本项目约定的安全缺省，
	 * 绝不出现“半管不管”的中间态。
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

	/* .ko 形态在这里就解析好了；内置形态要等厂商模块加载，由 work 重试 */
	ddlg_resolve();

	ddlg_scan_existing();
	ddlg_kick(true);

	pr_info("已加载：锚点 copy_pid_ns/zap_pid_ns_processes/put_pid_ns 就绪，"
		"符号%s，当前 DDL=%d，linger_ms=%d，dry_run=%d，policy=%d\n",
		ddlg_flag ? "已解析" : "待解析",
		ddlg_flag ? READ_ONCE(*ddlg_flag) : -1,
		linger_ms, dry_run, policy);
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
	pr_info("已卸载：DDL 保持当前值 %d 不变\n",
		ddlg_flag ? READ_ONCE(*ddlg_flag) : -1);
}

module_init(ddlg_init);
module_exit(ddlg_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("容器作用域的 DDL 动态守护（Droidspaces 容器运行时关闭厂商 DDL）");
MODULE_AUTHOR("Wcoom");
MODULE_VERSION("1.1.0");
