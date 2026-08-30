// SPDX-License-Identifier: GPL-2.0
/*
 * fq_guard_ko.c - 守护数据接口的 root qdisc 恒为 fq(可加载模块版)
 *
 * 背景: 上游 whitewhale v3.5 prebuilt 内核(6.6.118-...-abogki20260808-4k)
 * 不含本地 fq_guard 定制。厂商 netd / 高通数据服务会在数据接口 up 后通过
 * tc 将 root qdisc 覆盖为 Qualcomm 专有 QoS 栈(htb -> ppq -> {sfq, htb->tsd,
 * htb->tsd}),实测导致 BBRv3 多流公平性极差(4 并发流带宽比 28.7:1)且队列深、
 * bufferbloat 明显。本模块把本地 built-in fq_guard 的逻辑移植为可加载模块,
 * 直接给上游 prebuilt 内核使用,无需重刷内核。
 *
 * 与本地 built-in 版的差异(上游内核 API 约束,其余逻辑逐字一致):
 *  - qdisc_create_by_kind() 是本地 C3 架构改造新增的导出,上游没有;
 *    本模块改用上游同样导出的 qdisc_create_dflt()(net/sched/sch_generic.c,
 *    EXPORT_SYMBOL)创建 fq qdisc,成功后手动 qdisc_hash_add() 入哈希表;
 *  - fq_qdisc_ops 在 sch_fq.c 是 static,上游与本地都无法 extern 引用;
 *    本地 built-in 靠 sch_api.c 内部 qdisc_lookup_ops("fq") 字符串查找,
 *    而上游 qdisc_lookup_ops 同样是 static。本模块在 init 时解析
 *    /proc/kallsyms 取 fq_qdisc_ops 地址(校验 ops->id == "fq")。
 *    前提 kernel.kptr_restrict=0:init 时自动尝试写 0;若被拒则解析失败
 *    并打印提示,可手动 `echo 0 > /proc/sys/kernel/kptr_restrict` 后重载。
 *  - 签名:目标内核 CONFIG_MODULE_SIG_PROTECT=y 时未签名模块可加载,
 *    但用本地密钥签名的模块会验签失败(fatal)。因此本模块构建时禁用
 *    CONFIG_MODULE_SIG_ALL,产出不带签名的 .ko。
 *
 * 设计(与本地版一致):
 *  1) register_netdevice_notifier() 监听 NETDEV_UP / NETDEV_CHANGE /
 *     NETDEV_REGISTER(以及 NETDEV_UNREGISTER 用于释放资源);
 *  2) notifier 回调保持轻量:按接口名前缀白名单匹配后,分配 ctx 保存
 *     dev_net() 快照(get_net)与 strscpy 的 ifname,随后
 *     queue_delayed_work() 到 system_power_efficient_wq。
 *     禁止跨 work 持有裸 struct net_device 指针,不执行任何可睡眠操作;
 *  3) work 函数在 rtnl_lock() 内执行:dev_get_by_name() 凭快照重新解析
 *     设备(其已持引用,结束 dev_put,防删除竞态)。若 netif_running(dev)
 *     且当前 root qdisc 不是 fq(root 为 mq 时若所有子队列均为 fq 则视为
 *     已满足要求而跳过),则新建 fq qdisc 并按内核标准流程(与 sch_api.c
 *     qdisc_graft() 的 root 分支一致)替换;
 *  4) 低功耗策略:默认不做无限周期轮询。仅在"检测到被覆盖并成功改回"
 *     后启动有限次数的复查(retry_burst),用于对抗 netd 在接口 up 后短时间
 *     内的反复重配;连续复查发现已是 fq 即停止,回到纯事件驱动的零开销
 *     状态。netd 之后若再次覆盖,会伴随 NETDEV_CHANGE 事件重新触发。
 *     兜底(event_recheck=true):work 快速路径跳过时(检查瞬间已是 fq)额外
 *     排一次复查——wifi 重连实测 netd 会在 fq_guard 检查之后才配置 htb,
 *     且 tc 配置不产生 NETDEV 事件,无兜底则迟到的覆盖永远不被发现。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/if.h>
#include <linux/notifier.h>
#include <linux/fs.h>
#include <net/sch_generic.h>
#include <net/pkt_sched.h>
#include <net/net_namespace.h>

/* ---- 模块参数 ---- */
static bool enable = true;
module_param(enable, bool, 0644);
MODULE_PARM_DESC(enable, "enable fq_guard (default true)");

static int delay_ms = 3000;
module_param(delay_ms, int, 0644);
MODULE_PARM_DESC(delay_ms, "delay before forcing fq after iface event (ms, default 3000)");

/* 复查间隔:仅在刚刚强制改回 fq 后使用,不是常开轮询。 */
static int recheck_ms = 5000;
module_param(recheck_ms, int, 0644);
MODULE_PARM_DESC(recheck_ms, "recheck interval after a forced change (ms, default 5000, 0 to disable)");

/* 复查次数上限:连续 retry_burst 次复查都发现已是 fq 就彻底停下,
 * 回到纯事件驱动。防止无限轮询带来的周期性唤醒与 CPU 占用。 */
static int retry_burst = 3;
module_param(retry_burst, int, 0644);
MODULE_PARM_DESC(retry_burst, "max rechecks after a forced change (default 3)");

static char *blacklist = "rmnet_ims,";
module_param(blacklist, charp, 0644);
MODULE_PARM_DESC(blacklist, "comma-separated blacklist prefixes (beats whitelist, default rmnet_ims)");

/* fq_qdisc_ops 地址(static,内核态无法 extern 引用)。
 * oplus 安全补丁禁止内核态读 /proc/kallsyms("kernel read not supported"),
 * 因此由用户态解析地址后经此参数传入(insmod 时指定);
 * 0 表示尝试内核态解析(大概率被拒,仅作 fallback)。 */
static unsigned long fq_ops_addr;
module_param(fq_ops_addr, ulong, 0644);
MODULE_PARM_DESC(fq_ops_addr, "address of fq_qdisc_ops from userspace kallsyms (0 = try in-kernel parse)");

/* 事件驱动兜底复查:netd 可能在 work 快速路径跳过(当时 root 已是 fq)
 * 之后才配置 htb,且 tc 配置不产生 NETDEV 事件(wifi 重连实测)。
 * 开启后每次 work 快速路径跳过都会多排一次复查,抓住迟到的覆盖。 */
static bool event_recheck = true;
module_param(event_recheck, bool, 0644);
MODULE_PARM_DESC(event_recheck, "schedule one fallback recheck when fast-path skipped (default true)");

/* ---- 白名单:仅守护以下前缀的接口 ---- */
static const char * const whitelist[] = {
	"rmnet_data", "r_rmnet_data", "wlan", "p2p", "wifi-aware",
	"vgate", "usb", "rndis", "eth", "bt-pan",
};

#define FQG_MAX_CTX	64

/* 每个受管接口一个上下文:notifier 中保存 dev_net 快照与 ifname,
 * work 中仅凭快照重新解析设备,不持有裸 dev 指针跨上下文。 */
struct fqg_ctx {
	struct net *net;		/* dev_net() 快照(已 get_net) */
	char ifname[IFNAMSIZ];
	int rechecks_left;		/* 剩余复查次数,0 表示不再自排 */
	struct delayed_work dwork;
};

static struct fqg_ctx fqg_ctxs[FQG_MAX_CTX];
static struct notifier_block fqg_notifier;

/* 从 /proc/kallsyms 解析出的 fq_qdisc_ops(static,无法 extern 引用)。
 * 由 fqg_init 一次性解析;解析失败则模块加载失败。 */
static const struct Qdisc_ops *fqg_fq_ops;

static void fqg_work(struct work_struct *work);

static bool fqg_prefix_match(const char *prefix, const char *name)
{
	return strncmp(prefix, name, strlen(prefix)) == 0;
}

/* 接口名是否匹配逗号分隔的前缀列表。
 * 在栈上拷贝(512B),不分配内存,可在 notifier 原子上下文调用。 */
static bool fqg_list_match(const char *list, const char *name)
{
	char buf[512];
	char *p, *tok;

	strscpy(buf, list, sizeof(buf));
	p = buf;
	while ((tok = strsep(&p, ",")) != NULL) {
		if (*tok && fqg_prefix_match(tok, name))
			return true;
	}
	return false;
}

static bool fqg_whitelist_match(const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(whitelist); i++) {
		if (fqg_prefix_match(whitelist[i], name))
			return true;
	}
	return false;
}

static struct fqg_ctx *fqg_ctx_find_or_alloc(const struct net_device *dev)
{
	struct fqg_ctx *free = NULL;
	int i;

	for (i = 0; i < FQG_MAX_CTX; i++) {
		struct fqg_ctx *ctx = &fqg_ctxs[i];

		if (ctx->ifname[0] && strcmp(ctx->ifname, dev->name) == 0)
			return ctx;
		if (!free && !ctx->ifname[0])
			free = ctx;
	}
	return free;
}

/* notifier 回调(原子上下文):仅匹配 + 快照 + 排队,不做任何重活。 */
static int fqg_device_event(struct notifier_block *nb, unsigned long event,
			    void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct fqg_ctx *ctx;

	if (!enable)
		return NOTIFY_DONE;

	switch (event) {
	case NETDEV_UP:
	case NETDEV_CHANGE:
	case NETDEV_REGISTER:
		break;
	case NETDEV_UNREGISTER:
		/* 仅释放 ctx 的 net 引用,不触碰设备(设备正在注销) */
		for (ctx = fqg_ctxs; ctx < fqg_ctxs + FQG_MAX_CTX; ctx++) {
			if (ctx->ifname[0] &&
			    strcmp(ctx->ifname, dev->name) == 0) {
				ctx->ifname[0] = '\0';
				ctx->rechecks_left = 0;
				cancel_delayed_work_sync(&ctx->dwork);
				put_net(ctx->net);
				ctx->net = NULL;
			}
		}
		return NOTIFY_DONE;
	default:
		return NOTIFY_DONE;
	}

	/* 黑名单优先于白名单 */
	if (fqg_list_match(blacklist, dev->name))
		return NOTIFY_DONE;
	if (!fqg_whitelist_match(dev->name))
		return NOTIFY_DONE;

	ctx = fqg_ctx_find_or_alloc(dev);
	if (!ctx)
		return NOTIFY_DONE;

	/* net 引用只在首次分配时获取一次 */
	if (!ctx->ifname[0]) {
		ctx->net = get_net(dev_net(dev));
		strscpy(ctx->ifname, dev->name, IFNAMSIZ);
	}

	/* 事件去抖:已有 pending work 时不重排 */
	if (delayed_work_pending(&ctx->dwork))
		return NOTIFY_DONE;

	queue_delayed_work(system_power_efficient_wq, &ctx->dwork,
			   msecs_to_jiffies(delay_ms));
	return NOTIFY_DONE;
}

/* work 函数(进程上下文,rtnl_lock 内):按快照重解析设备并强制 fq。 */
static void fqg_work(struct work_struct *work)
{
	struct fqg_ctx *ctx = container_of(work, struct fqg_ctx, dwork.work);
	struct net_device *dev;
	struct Qdisc *root, *old;
	unsigned int i;
	bool changed = false;
	bool dev_up = false;

	rtnl_lock();

	/* 接口可能已被删除:凭快照重新解析,不使用裸指针 */
	dev = dev_get_by_name(ctx->net, ctx->ifname);
	if (!dev)
		goto out_unlock;

	/* 仅在接口 up 时守护 */
	dev_up = netif_running(dev);
	if (!dev_up)
		goto out_put;

	/* 1) root qdisc 已是 fq:跳过(快速路径,只做字符串比较) */
	root = rtnl_dereference(dev->qdisc);
	if (root && !strcmp(root->ops->id, "fq"))
		goto out_put;

	/* 2) root 为 mq 时:子队列全为 fq 即视为已满足(保留 mq 多队列) */
	if (root && !strcmp(root->ops->id, "mq")) {
		bool all_fq = true;

		for (i = 0; i < dev->num_tx_queues; i++) {
			struct Qdisc *cq;

			cq = rtnl_dereference(
				netdev_get_tx_queue(dev, i)->qdisc_sleeping);
			if (!cq || strcmp(cq->ops->id, "fq")) {
				all_fq = false;
				break;
			}
		}
		if (all_fq)
			goto out_put;
	}

	/* 3) 新建 fq qdisc:上游内核用 qdisc_create_dflt()(sch_generic.c,
	 *    已 EXPORT_SYMBOL)创建,ops 为 kallsyms 解析出的 fq_qdisc_ops。
	 *    该函数失败返回 NULL(非 ERR_PTR);成功后手动入哈希表(与
	 *    qdisc_create() 的语义对齐,便于 tc 工具查询)。 */
	{
		struct Qdisc *new;

		new = qdisc_create_dflt(netdev_get_tx_queue(dev, 0),
					fqg_fq_ops, TC_H_ROOT, NULL);
		if (!new)
			goto out_put;	/* 失败:保留现状,下个周期重试 */

		qdisc_hash_add(new, false);

		/* 4) 标准 root 替换流程(与 sch_api.c qdisc_graft() 的
		 *    root 分支一致):
		 *    - dev_deactivate():停 tx,各 tx queue 的旧 qdisc
		 *      qdisc_sleeping 换为 noop 并释放引用;
		 *    - 逐 tx queue dev_graft_qdisc() 挂上 new;多个 queue
		 *      共享 new 时 i>0 递增引用;
		 *    - dev->qdisc 槽位独立递增引用后指向 new;
		 *    - 释放旧 root(引用归零后 __qdisc_destroy 清理队列);
		 *    - dev_activate():恢复 tx 并激活 new。 */
		dev_deactivate(dev);

		for (i = 0; i < dev->num_tx_queues; i++) {
			struct netdev_queue *dev_queue =
				netdev_get_tx_queue(dev, i);

			old = dev_graft_qdisc(dev_queue, new);
			if (i > 0)
				qdisc_refcount_inc(new);
			qdisc_put(old);
		}

		old = rtnl_dereference(dev->qdisc);
		qdisc_refcount_inc(new);
		rcu_assign_pointer(dev->qdisc, new);
		qdisc_put(old);

		dev_activate(dev);

		changed = true;
		netdev_info(dev, "fq_guard: root qdisc forced to fq (%u tx queues)\n",
			    dev->num_tx_queues);
	}

out_put:
	dev_put(dev);
out_unlock:
	rtnl_unlock();

	/* 5) 有限复查(低功耗):只有刚刚强制改回 fq 才值得复查——说明 netd
	 *    正在争抢,短期内可能再次覆盖。若本次发现已是 fq(changed==false)
	 *    则递减预算,连续几次都没被改动就彻底停下,不再排任何 work。
	 *    此后回到纯事件驱动:netd 若再覆盖,会伴随 NETDEV_CHANGE 重新触发。
	 *
	 *    兜底(event_recheck):work 快速路径跳过时(检查瞬间 root 已是 fq,
	 *    rechecks_left==0)排一次复查。实测 wifi 重连时 netd 在 fq_guard
	 *    的 work 执行之后才配置 htb,且 tc 配置不产生 NETDEV 事件——
	 *    若无兜底,迟到的覆盖将永远不被发现。 */
	if (!enable || recheck_ms <= 0)
		return;

	if (changed)
		ctx->rechecks_left = retry_burst;	/* 被覆盖过,重置预算 */
	else if (ctx->rechecks_left > 0)
		ctx->rechecks_left--;
	else if (dev_up && event_recheck)
		ctx->rechecks_left = 1;			/* 兜底:多排一次复查 */

	if (ctx->rechecks_left > 0 && ctx->ifname[0])
		queue_delayed_work(system_power_efficient_wq, &ctx->dwork,
				   msecs_to_jiffies(recheck_ms));
}

/* 解析 fq_qdisc_ops 地址:
 * 1) 优先用用户态经 fq_ops_addr 参数传入的地址(校验 ops->id == "fq");
 * 2) fallback:内核态读 /proc/kallsyms(oplus 安全补丁会拒绝,
 *    即 dmesg 的 "kernel read not supported for file /kallsyms")。 */
static const struct Qdisc_ops *fqg_resolve_fq_ops(void)
{
	struct file *f;
	char buf[512];
	ssize_t n;
	loff_t pos;
	const struct Qdisc_ops *ops;

	if (fq_ops_addr) {
		ops = (const struct Qdisc_ops *)fq_ops_addr;
		/* id 是定长数组(非指针),恒非空 */
		if (strcmp(ops->id, "fq") == 0)
			return ops;
		pr_err("fq_guard_ko: fq_ops_addr=0x%lx id mismatch (%s)\n",
		       fq_ops_addr, ops->id);
		return NULL;
	}

	/* kptr_restrict=2 时 root 也读不到地址,先尝试放开 */
	f = filp_open("/proc/sys/kernel/kptr_restrict", O_WRONLY, 0);
	if (!IS_ERR(f)) {
		pos = 0;
		kernel_write(f, "0", 1, &pos);
		filp_close(f, NULL);
	}

	f = filp_open("/proc/kallsyms", O_RDONLY, 0);
	if (IS_ERR(f))
		return NULL;

	pos = 0;
	while ((n = kernel_read(f, buf, sizeof(buf) - 1, &pos)) > 0) {
		char *p = buf, *nl;

		buf[n] = '\0';
		while ((nl = strchr(p, '\n')) != NULL) {
			unsigned long addr;
			char type;
			char name[128];

			*nl = '\0';
			if (sscanf(p, "%lx %c %127s", &addr, &type, name) == 3 &&
			    strcmp(name, "fq_qdisc_ops") == 0) {
				filp_close(f, NULL);
				if (!addr) {
					pr_err("fq_guard_ko: kallsyms addr hidden "
					       "(kptr_restrict), run: "
					       "echo 0 > /proc/sys/kernel/kptr_restrict\n");
					return NULL;
				}
				ops = (const struct Qdisc_ops *)addr;
				/* id 是定长数组(非指针),恒非空 */
				if (strcmp(ops->id, "fq") == 0)
					return ops;
				pr_err("fq_guard_ko: kallsyms fq_qdisc_ops id "
				       "mismatch (%s)\n", ops->id);
				return NULL;
			}
			p = nl + 1;
		}
	}
	filp_close(f, NULL);
	return NULL;
}

/* 加载时扫描现有接口:与 notifier 事件路径走同一套 ctx + work 机制,
 * 只是触发源从"接口事件"换成"遍历 init_net 设备链表"。
 * 在 rtnl_lock 内遍历(for_each_netdev 要求),只排队不干活。 */
static void fqg_scan_existing(void)
{
	struct net_device *dev;

	rtnl_lock();
	for_each_netdev(&init_net, dev) {
		struct fqg_ctx *ctx;

		if (!enable)
			break;
		if (fqg_list_match(blacklist, dev->name))
			continue;
		if (!fqg_whitelist_match(dev->name))
			continue;

		ctx = fqg_ctx_find_or_alloc(dev);
		if (!ctx)
			continue;
		if (!ctx->ifname[0]) {
			ctx->net = get_net(dev_net(dev));
			strscpy(ctx->ifname, dev->name, IFNAMSIZ);
		}
		if (!delayed_work_pending(&ctx->dwork))
			queue_delayed_work(system_power_efficient_wq,
					   &ctx->dwork,
					   msecs_to_jiffies(delay_ms));
	}
	rtnl_unlock();
}

static int __init fqg_init(void)
{
	unsigned int i;

	/* delayed_work 一次性初始化:槽位在 UNREGISTER 后会被复用,
	 * 若每次复用都重新 INIT 会破坏 timer/work 的内部状态。 */
	for (i = 0; i < FQG_MAX_CTX; i++)
		INIT_DELAYED_WORK(&fqg_ctxs[i].dwork, fqg_work);

	fqg_fq_ops = fqg_resolve_fq_ops();
	if (!fqg_fq_ops) {
		pr_err("fq_guard_ko: cannot resolve fq_qdisc_ops, module abort\n");
		return -EINVAL;
	}

	/* 不自建 workqueue:复用 system_power_efficient_wq。 */
	fqg_notifier.notifier_call = fqg_device_event;
	register_netdevice_notifier(&fqg_notifier);

	/* 模块是事后加载,接口 up 事件早已过去:主动扫描一遍现有接口,
	 * 对白名单接口各排一轮 work(去抖后 delay_ms 内执行),
	 * 使"加载即生效",不依赖下一次 NETDEV_UP/CHANGE。 */
	fqg_scan_existing();

	return 0;
}

static void __exit fqg_exit(void)
{
	unsigned int i;

	unregister_netdevice_notifier(&fqg_notifier);

	/* 等待所有 work 结束并释放 ctx 资源 */
	for (i = 0; i < FQG_MAX_CTX; i++) {
		struct fqg_ctx *ctx = &fqg_ctxs[i];

		if (!ctx->ifname[0])
			continue;
		ctx->ifname[0] = '\0';
		ctx->rechecks_left = 0;
		cancel_delayed_work_sync(&ctx->dwork);
		put_net(ctx->net);
		ctx->net = NULL;
	}
}

module_init(fqg_init);
module_exit(fqg_exit);

/* kallsyms 解析需要 VFS 文件操作(kernel_read/write/filp_open),
 * 该 namespace 是内核刻意加的"非驱动勿用"警告,此处确需使用 */
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("fq_guard");
MODULE_DESCRIPTION("Keep root qdisc of data interfaces on fq for BBRv3 (upstream-prebuilt ko)");
MODULE_VERSION("1.1");
