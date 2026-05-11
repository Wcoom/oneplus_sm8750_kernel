// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "crystal_hybridswap: " fmt

#include <linux/err.h>
#include <linux/eventfd.h>
#include <linux/export.h>
#include <linux/file.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <linux/crystal_hybridswap.h>

#include "crystal_hybridswap_internal.h"

static DEFINE_MUTEX(pressure_lock);
static struct eventfd_ctx *pressure_events[CHS_PRESSURE_LEVELS];

static const char *pressure_level_name(unsigned int level)
{
	switch (level) {
	case CHS_PRESSURE_LOW:
		return "low";
	case CHS_PRESSURE_MEDIUM:
		return "medium";
	case CHS_PRESSURE_CRITICAL:
		return "critical";
	default:
		return "unknown";
	}
}

static void pressure_record(unsigned int level, const char *reason, int ret)
{
	mutex_lock(&chs.state_lock);
	chs.last_pressure_level = level;
	chs.last_pressure_ret = ret;
	strscpy(chs.last_pressure_reason,
		reason && reason[0] ? reason : "unknown",
		sizeof(chs.last_pressure_reason));
	mutex_unlock(&chs.state_lock);

	atomic64_set(&chs.stats.pressure_last_ret, ret);
}

ssize_t crystal_hybridswap_swapd_pressure_write(struct kernfs_open_file *of,
						char *buf, size_t nbytes,
						loff_t off)
{
	struct eventfd_ctx *ctx;
	struct eventfd_ctx *old;
	struct fd efile;
	unsigned int level;
	int efd;
	int ret = 0;

	buf = strim(buf);
	if (sscanf(buf, "%d %u", &efd, &level) != 2)
		return -EINVAL;
	if (level >= CHS_PRESSURE_LEVELS)
		return -EINVAL;
	if (efd < 0) {
		mutex_lock(&pressure_lock);
		old = pressure_events[level];
		pressure_events[level] = NULL;
		mutex_unlock(&pressure_lock);
		if (old) {
			eventfd_ctx_put(old);
			atomic64_inc(&chs.stats.pressure_released);
		}
		pressure_record(level, "unregister", 0);
		return nbytes;
	}

	efile = fdget(efd);
	if (!efile.file)
		return -EBADF;

	ctx = eventfd_ctx_fileget(efile.file);
	fdput(efile);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	mutex_lock(&pressure_lock);
	old = pressure_events[level];
	pressure_events[level] = ctx;
	mutex_unlock(&pressure_lock);

	if (old) {
		eventfd_ctx_put(old);
		atomic64_inc(&chs.stats.pressure_released);
	}
	atomic64_inc(&chs.stats.pressure_registered);

	return ret ? ret : nbytes;
}

int crystal_hybridswap_report_pressure(unsigned int level, const char *reason)
{
	struct eventfd_ctx *ctx;
	int ret = 0;

	if (level >= CHS_PRESSURE_LEVELS) {
		pressure_record(level, reason, -EINVAL);
		atomic64_inc(&chs.stats.pressure_signal_errors);
		chs_log_ratelimited(CHS_LOG_WARN,
				    "pressure signal invalid level=%u reason=%s ret=%d\n",
				    level,
				    reason && reason[0] ? reason : "unknown",
				    -EINVAL);
		return -EINVAL;
	}

	/*
	 * pressure_events[] owns the eventfd_ctx reference acquired during
	 * registration. eventfd_ctx has no exported ref-get helper, so hold
	 * pressure_lock while signaling to pin ctx against unregister/replace.
	 * eventfd_signal() is non-sleeping and only takes the eventfd waitqueue
	 * spinlock, keeping this internal protection narrow and ABI-neutral.
	 */
	mutex_lock(&pressure_lock);
	ctx = pressure_events[level];
	if (ctx)
		ret = eventfd_signal(ctx, 1);
	else
		ret = -ENOENT;
	mutex_unlock(&pressure_lock);

	pressure_record(level, reason, ret);
	if (ret >= 0) {
		chs_log_ratelimited(CHS_LOG_INFO,
				    "pressure signal level=%u name=%s reason=%s ret=%d\n",
				    level, pressure_level_name(level),
				    reason && reason[0] ? reason : "unknown", ret);
		atomic64_inc(&chs.stats.pressure_signaled);
		switch (level) {
		case CHS_PRESSURE_LOW:
			atomic64_inc(&chs.stats.pressure_low_signaled);
			break;
		case CHS_PRESSURE_MEDIUM:
			atomic64_inc(&chs.stats.pressure_medium_signaled);
			break;
		case CHS_PRESSURE_CRITICAL:
			atomic64_inc(&chs.stats.pressure_critical_signaled);
			break;
		}
	} else if (ret == -ENOENT) {
		chs_log_ratelimited(CHS_LOG_DEBUG,
				    "pressure signal skipped no_listener level=%u name=%s reason=%s ret=%d\n",
				    level, pressure_level_name(level),
				    reason && reason[0] ? reason : "unknown", ret);
		atomic64_inc(&chs.stats.pressure_no_listener);
	} else {
		chs_log_ratelimited(CHS_LOG_WARN,
				    "pressure signal error level=%u name=%s reason=%s ret=%d\n",
				    level, pressure_level_name(level),
				    reason && reason[0] ? reason : "unknown", ret);
		atomic64_inc(&chs.stats.pressure_signal_errors);
	}

	return ret;
}

int crystal_hybridswap_signal_pressure(unsigned int level)
{
	return crystal_hybridswap_report_pressure(level, "external");
}
EXPORT_SYMBOL_GPL(crystal_hybridswap_signal_pressure);

void crystal_hybridswap_pressure_stats_show(struct seq_file *m)
{
	char reason[CHS_PRESSURE_REASON_MAX];
	int level;
	int ret;

	mutex_lock(&chs.state_lock);
	level = chs.last_pressure_level;
	ret = chs.last_pressure_ret;
	strscpy(reason, chs.last_pressure_reason, sizeof(reason));
	mutex_unlock(&chs.state_lock);

	seq_printf(m, "pressure_registered: %lld\n",
		   atomic64_read(&chs.stats.pressure_registered));
	seq_printf(m, "pressure_released: %lld\n",
		   atomic64_read(&chs.stats.pressure_released));
	seq_printf(m, "pressure_signaled: %lld\n",
		   atomic64_read(&chs.stats.pressure_signaled));
	seq_printf(m, "pressure_no_listener: %lld\n",
		   atomic64_read(&chs.stats.pressure_no_listener));
	seq_printf(m, "pressure_signal_errors: %lld\n",
		   atomic64_read(&chs.stats.pressure_signal_errors));
	seq_printf(m, "pressure_low_signaled: %lld\n",
		   atomic64_read(&chs.stats.pressure_low_signaled));
	seq_printf(m, "pressure_medium_signaled: %lld\n",
		   atomic64_read(&chs.stats.pressure_medium_signaled));
	seq_printf(m, "pressure_critical_signaled: %lld\n",
		   atomic64_read(&chs.stats.pressure_critical_signaled));
	seq_printf(m, "pressure_last_level: %d\n", level);
	seq_printf(m, "pressure_last_level_name: %s\n",
		   pressure_level_name(level));
	seq_printf(m, "pressure_last_reason: %s\n", reason);
	seq_printf(m, "pressure_last_ret: %d\n", ret);
}

void crystal_hybridswap_pressure_exit(void)
{
	struct eventfd_ctx *ctx[CHS_PRESSURE_LEVELS];
	int i;

	mutex_lock(&pressure_lock);
	for (i = 0; i < CHS_PRESSURE_LEVELS; i++) {
		ctx[i] = pressure_events[i];
		pressure_events[i] = NULL;
	}
	mutex_unlock(&pressure_lock);

	for (i = 0; i < CHS_PRESSURE_LEVELS; i++) {
		if (ctx[i]) {
			eventfd_ctx_put(ctx[i]);
			atomic64_inc(&chs.stats.pressure_released);
		}
	}
}
