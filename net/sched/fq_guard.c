// SPDX-License-Identifier: GPL-2.0
/*
 * fq_guard.c - 守护数据接口的 root qdisc 恒为 fq(built-in)
 *
 * 背景:厂商 netd / 高通数据服务会在数据接口 up 后通过 tc 将 root qdisc
 * 覆盖为 Qualcomm 专有 QoS 栈(htb -> ppq -> {sfq, htb->tsd, htb->tsd}),
 * 实测导致 BBRv3 多流公平性极差(4 并发流带宽比 28.7:1)且队列深、
 * bufferbloat 明显。接口 down 时默认 qdisc 本就是 fq/mq+fq,问题只发生
 * 在 up 后的覆盖。
 *
 * 设计(built-in,不依赖任何模块导出):
 *  1) register_netdevice_notifier() 监听 NETDEV_UP / NETDEV_CHANGE /
 *     NETDEV_REGISTER(以及 NETDEV_UNREGISTER 用于释放资源);
 *  2) notifier 回调保持轻量:按接口名前缀白名单匹配后,分配 ctx 保存
 *     dev_net() 快照(get_net)与 strscpy 的 ifname,随后
 *     queue_delayed_work() 到专用单线程 workqueue("fq_guard")。
 *     禁止跨 work 持有裸 struct net_device 指针,不执行任何可睡眠操作;
 *  3) work 函数在 rtnl_lock() 内执行:dev_get_by_name() 凭快照重新解析
 *     设备(其已持引用,结束 dev_put,防删除竞态)。若 netif_running(dev)
 *     且当前 root qdisc 不是 fq(root 为 mq 时若所有子队列均为 fq 则视为
 *     已满足要求而跳过),则新建 fq qdisc 并按内核标准流程(与 sch_api.c
 *     qdisc_graft() 的 root 分支一致)替换;
 *  4) work 结束后若 enable && period_ms > 0 则自循环重排,周期守护以
 *     对抗 netd 的反复重配。
 *
 * 6.6 API 核对结论(全部对照本树源码):
 *  - qdisc_create(): net/sched/sch_api.c:1215 为 static。本补丁对
 *    sch_api.c 的唯一改动:去掉该定义处的 static 前缀(不改任何逻辑),
 *    built-in 下链接期解析,无需也不新增 EXPORT_SYMBOL。
 *    6.6 签名为 7 参数且无 ops 参数:
 *        qdisc_create(dev, dev_queue, parent, handle, tca, errp, extack)
 *    任务描述中的 5 参签名(带 &fq_qdisc_ops)为旧内核接口,不适用。
 *  - fq_qdisc_ops: sch_fq.c:1037 为 static,built-in 也无法 extern 引用,
 *    因此通过构造 TCA_KIND="fq" 的 nlattr 由 qdisc_create() 内部
 *    qdisc_lookup_ops() 查找;qdisc 类型判断一律用 q->ops->id 字符串
 *    比较("fq"/"mq"),同样避免引用 static 的 fq_qdisc_ops。
 *  - dev_graft_qdisc(): sch_generic.c:1124,6.6 签名为
 *    (struct netdev_queue *dev_queue, struct Qdisc *qdisc),非任务描述的
 *    (dev, q);已 EXPORT_SYMBOL,声明在 include/net/sch_generic.h:697。
 *  - qdisc_tree_flush(): 本 6.6 树中不存在(仅有 static inline
 *    qdisc_tree_flush_backlog)。旧 qdisc 的队列/统计清理由
 *    qdisc_put() -> __qdisc_destroy() 内的 qdisc_purge_queue() 完成,
 *    root 替换流程与 qdisc_graft() 的 root 分支保持一致。
 *  - qdisc_put(): sch_generic.c:1094,refcount_dec_and_test() 归零才
 *    __qdisc_destroy();TCQ_F_BUILTIN(noop_qdisc) 直接跳过。
 *  - dev_activate()/dev_deactivate(): sch_generic.c 已导出,
 *    声明在 include/net/sch_generic.h;须在 rtnl_lock() 内调用。
 *  - notifier 回调不得睡眠;work 在进程上下文。rtnl_lock 内禁止调用
 *    可能睡眠等待 rtnl 的接口;dev_deactivate() 在 qdisc_graft() 中
 *    即于 rtnl_lock 内调用,是内核既有合法用法。
 *
 * 参数(built-in 下):
 *  - 内核启动 cmdline:fq_guard.enable=0
 *  - 运行时:/sys/module/fq_guard/parameters/{enable,delay_ms,period_ms,blacklist}
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
#include <linux/netlink.h>
#include <linux/notifier.h>
#include <net/sch_generic.h>
#include <net/net_namespace.h>
#include <net/netlink.h>
#include <net/pkt_sched.h>

/* qdisc_create 在 sch_api.c 中为 static,本补丁唯一改动是去掉 static;
 * 此处显式 extern(built-in 链接期解析)。 */
extern struct Qdisc *qdisc_create(struct net_device *dev,
				  struct netdev_queue *dev_queue,
				  u32 parent, u32 handle,
				  struct nlattr **tca, int *errp,
				  struct netlink_ext_ack *extack);

/* ---- 模块参数(built-in 即内核参数 + sysfs) ---- */
static bool enable = true;
module_param(enable, bool, 0644);
MODULE_PARM_DESC(enable, "enable fq_guard (default true)");

static int delay_ms = 3000;
module_param(delay_ms, int, 0644);
MODULE_PARM_DESC(delay_ms, "delay before forcing fq after iface event (ms, default 3000)");

static int period_ms = 15000;
module_param(period_ms, int, 0644);
MODULE_PARM_DESC(period_ms, "periodic guard interval (ms, default 15000, 0 to disable)");

static char *blacklist = "rmnet_ims,";
module_param(blacklist, charp, 0644);
MODULE_PARM_DESC(blacklist, "comma-separated blacklist prefixes (beats whitelist, default rmnet_ims)");

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
	struct delayed_work dwork;
};

static struct workqueue_struct *fqg_wq;
static struct fqg_ctx fqg_ctxs[FQG_MAX_CTX];
static struct notifier_block fqg_notifier;

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
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(whitelist); i++) {
		if (fqg_prefix_match(whitelist[i], name))
			return true;
	}
	return false;
}

/* 查找已存在的 ctx,否则分配一个(静态数组,无内存管理)。
 * notifier 与 work 均在 rtnl_lock() 内执行,天然互斥,无需额外锁。 */
static struct fqg_ctx *fqg_ctx_find_or_alloc(struct net_device *dev)
{
	struct fqg_ctx *ctx;
	unsigned int i;

	for (i = 0; i < FQG_MAX_CTX; i++) {
		ctx = &fqg_ctxs[i];
		if (ctx->ifname[0] && !strcmp(ctx->ifname, dev->name))
			return ctx;
	}

	for (i = 0; i < FQG_MAX_CTX; i++) {
		ctx = &fqg_ctxs[i];
		if (!ctx->ifname[0]) {
			memset(ctx, 0, sizeof(*ctx));
			INIT_DELAYED_WORK(&ctx->dwork, fqg_work);
			return ctx;
		}
	}
	return NULL;	/* 超过 FQG_MAX_CTX 上限 */
}

/* netdevice notifier:只做轻量工作(匹配 -> 快照 -> 排队),不睡眠。 */
static int fqg_device_event(struct notifier_block *nb, unsigned long event,
			    void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct fqg_ctx *ctx;
	unsigned int i;

	if (!enable)
		return NOTIFY_DONE;

	switch (event) {
	case NETDEV_UP:
	case NETDEV_CHANGE:
	case NETDEV_REGISTER:
		break;
	case NETDEV_UNREGISTER:
		/* 接口销毁:清空 ifname 停止自循环,归还 net 引用。
		 * notifier 持 rtnl_lock,与 work 互斥,cancel 安全。 */
		for (i = 0; i < FQG_MAX_CTX; i++) {
			ctx = &fqg_ctxs[i];
			if (ctx->ifname[0] && !strcmp(ctx->ifname, dev->name)) {
				ctx->ifname[0] = '\0';
				cancel_delayed_work(&ctx->dwork);
				put_net(ctx->net);
				break;
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

	/* 轻量:快照 dev_net + ifname,延迟排队 */
	ctx = fqg_ctx_find_or_alloc(dev);
	if (!ctx)
		return NOTIFY_DONE;

	ctx->net = get_net(dev_net(dev));
	strscpy(ctx->ifname, dev->name, IFNAMSIZ);

	queue_delayed_work(fqg_wq, &ctx->dwork,
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
	int err;

	rtnl_lock();

	/* 接口可能已被删除:凭快照重新解析,不使用裸指针 */
	dev = dev_get_by_name(ctx->net, ctx->ifname);
	if (!dev)
		goto out_unlock;

	/* 仅在接口 up 时守护(down 时默认 qdisc 本就是 fq) */
	if (!netif_running(dev))
		goto out_put;

	/* 1) root qdisc 已是 fq:跳过 */
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

	/* 3) 新建 fq qdisc:构造 TCA_KIND="fq",由 qdisc_create() 内部
	 *    qdisc_lookup_ops() 查找已注册的 fq_qdisc_ops(避免引用
	 *    sch_fq.c 中的 static 符号)。handle=0 自动分配。 */
	{
		struct nlattr *tca[TCA_MAX + 1] = {};
		struct netlink_ext_ack extack = {};
		struct {
			struct nlattr nla;
			char kind[IFNAMSIZ];
		} kind = {
			.nla = {
				.nla_len = NLA_HDRLEN + sizeof("fq"),
				.nla_type = TCA_KIND,
			},
			.kind = "fq",
		};
		struct Qdisc *new;

		tca[TCA_KIND] = &kind.nla;
		new = qdisc_create(dev, netdev_get_tx_queue(dev, 0),
				   TC_H_ROOT, 0, tca, &err, &extack);
		if (!new)
			goto out_put;	/* 失败:保留现状,下个周期重试 */

		/* 4) 标准 root 替换流程(与 sch_api.c qdisc_graft() 的
		 *    root 分支一致):
		 *    - dev_deactivate():停 tx,各 tx queue 的旧 qdisc
		 *      qdisc_sleeping 换为 noop 并释放引用;
		 *    - 逐 tx queue dev_graft_qdisc() 挂上 new;多个 queue
		 *      共享 new 时 i>0 递增引用;
		 *    - dev->qdisc 槽位独立递增引用后指向 new;
		 *    - 释放旧 root(引用归零后 __qdisc_destroy 清理队列);
		 *    - dev_activate():恢复 tx 并激活 new。
		 *    qdisc_offload_graft_root() 仅影响 offload 设备,
		 *    手机网卡无 qdisc offload,按主线行为跳过。 */
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

		netdev_info(dev, "fq_guard: root qdisc forced to fq (%u tx queues)\n",
			    dev->num_tx_queues);
	}

out_put:
	dev_put(dev);
out_unlock:
	rtnl_unlock();

	/* 5) 周期自循环守护(防 netd 反复重配) */
	if (enable && period_ms > 0)
		queue_delayed_work(fqg_wq, &ctx->dwork,
				   msecs_to_jiffies(period_ms));
}

static int __init fqg_init(void)
{
	fqg_wq = create_singlethread_workqueue("fq_guard");
	if (!fqg_wq)
		return -ENOMEM;

	fqg_notifier.notifier_call = fqg_device_event;
	register_netdevice_notifier(&fqg_notifier);

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
		cancel_delayed_work_sync(&ctx->dwork);
		put_net(ctx->net);
		ctx->ifname[0] = '\0';
	}

	flush_workqueue(fqg_wq);
	destroy_workqueue(fqg_wq);
}

module_init(fqg_init);
module_exit(fqg_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("fq_guard");
MODULE_DESCRIPTION("Keep root qdisc of data interfaces on fq for BBRv3 fairness");
MODULE_VERSION("1.0");
