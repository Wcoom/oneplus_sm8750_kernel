// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "crystal_hybridswap: " fmt

#include <linux/cgroup.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/suspend.h>
#include <linux/swap.h>
#include <linux/vmstat.h>
#include <linux/workqueue.h>

#include "crystal_hybridswap_internal.h"

struct crystal_hybridswap_state chs;

struct crystal_hybridswap_force_swapout_req {
	struct work_struct work;
	struct device *dev;
	crystal_hybridswap_zram_force_writeback_t force_writeback;
	u64 target_cgroup_id;
	s64 app_score;
	s64 compat_trigger_value;
	s64 target_pages;
	bool auto_req;
	char memcg_name[CHS_MEMCG_NAME_MAX];
};

#define CHS_MAX_ZRAM_TARGETS	16

struct crystal_hybridswap_zram_target {
	struct device *dev;
	struct zram *zram;
	crystal_hybridswap_zram_writeback_t writeback;
	crystal_hybridswap_zram_force_writeback_t force_writeback;
	crystal_hybridswap_zram_force_writeback_ext_t force_writeback_ext;
	crystal_hybridswap_zram_batchin_t batchin;
	unsigned long zram_increase_pages;
	struct crystal_hybridswap_zram_pressure pressure;
};

struct crystal_hybridswap_writeback_target_selection {
	int best;
	int ret;
	int eligible;
	int no_writeback;
	int no_backing;
	int limit_exhausted;
	int no_resident;
};

static int crystal_hybridswap_queue_writeback_internal(struct device *dev,
		const char *mode, s64 pages, bool auto_req);
static int crystal_hybridswap_queue_force_swapout_internal(
		u64 target_cgroup_id, const char *memcg_name, s64 app_score,
		s64 compat_trigger_value, s64 target_pages, bool auto_req);
static void crystal_hybridswap_auto_record_result(int ret, s64 written_pages,
		bool per_memcg);

static void crystal_hybridswap_add_pending_pages_locked(atomic64_t *counter,
							 s64 pages)
{
	s64 pending;

	if (pages >= CHS_FORCE_GLOBAL_SCAN_PAGES) {
		atomic64_set(counter, CHS_FORCE_GLOBAL_SCAN_PAGES);
		return;
	}
	if (pages <= 0)
		return;

	pending = atomic64_read(counter);
	if (pending >= CHS_FORCE_GLOBAL_SCAN_PAGES)
		return;
	if (pending > CHS_FORCE_GLOBAL_SCAN_PAGES - pages)
		pending = CHS_FORCE_GLOBAL_SCAN_PAGES;
	else
		pending += pages;
	atomic64_set(counter, pending);
}

static int crystal_hybridswap_take_pending_writeback(struct device **dev,
		struct zram **zram,
		crystal_hybridswap_zram_writeback_t *writeback,
		crystal_hybridswap_zram_writeback_ext_t *writeback_ext,
		char *mode, size_t mode_size, s64 *pages, bool *force,
		bool *auto_req)
{
	*dev = NULL;
	*zram = NULL;
	*writeback = NULL;
	*writeback_ext = NULL;

	*force = false;
	*auto_req = false;

	mutex_lock(&chs.zram_lock);
	*pages = atomic64_xchg(&chs.pending_writeback_pages, 0);
	if (*pages <= 0) {
		mutex_unlock(&chs.zram_lock);
		return -ENOENT;
	}

	*dev = chs.pending_writeback_dev;
	*zram = chs.pending_writeback_zram;
	*writeback = chs.pending_writeback_fn;
	*writeback_ext = chs.pending_writeback_ext_fn;
	if (*dev)
		get_device(*dev);
	strscpy(mode, chs.pending_writeback_mode[0] ?
		chs.pending_writeback_mode : CHS_DEFAULT_WB_MODE, mode_size);
	*auto_req = chs.pending_writeback_auto;
	chs.pending_writeback_dev = NULL;
	chs.pending_writeback_zram = NULL;
	chs.pending_writeback_fn = NULL;
	chs.pending_writeback_ext_fn = NULL;
	chs.pending_writeback_mode[0] = '\0';
	chs.pending_writeback_auto = false;
	mutex_unlock(&chs.zram_lock);

	return 0;
}

static int crystal_hybridswap_take_pending_batchin(s64 *pages,
		u64 *target_cgroup_id, char *memcg_name, size_t memcg_name_size,
		s64 *app_score)
{
	mutex_lock(&chs.zram_lock);
	*pages = atomic64_xchg(&chs.pending_batchin_pages, 0);
	atomic64_xchg(&chs.pending_force_swapin, 0);
	if (*pages <= 0) {
		mutex_unlock(&chs.zram_lock);
		return -ENOENT;
	}

	*target_cgroup_id = chs.pending_batchin_target_cgroup_id;
	if (memcg_name && memcg_name_size)
		strscpy(memcg_name, chs.pending_batchin_memcg[0] ?
			chs.pending_batchin_memcg : "unknown", memcg_name_size);
	if (app_score)
		*app_score = chs.pending_batchin_app_score;
	chs.pending_batchin_target_cgroup_id = 0;
	chs.pending_batchin_memcg[0] = '\0';
	chs.pending_batchin_app_score = 0;
	mutex_unlock(&chs.zram_lock);

	return 0;
}

void crystal_hybridswap_clear_pending_writeback_locked(void)
{
	if (chs.pending_writeback_zram)
		zram_put(chs.pending_writeback_zram);
	chs.pending_writeback_dev = NULL;
	chs.pending_writeback_zram = NULL;
	chs.pending_writeback_fn = NULL;
	chs.pending_writeback_ext_fn = NULL;
	chs.pending_writeback_mode[0] = '\0';
	chs.pending_writeback_auto = false;
	atomic64_set(&chs.pending_writeback_pages, 0);
}

static void crystal_hybridswap_force_swapout_drain_done(void)
{
	if (atomic64_dec_and_test(&chs.force_swapout_inflight))
		wake_up_all(&chs.force_swapout_wait);
}

void crystal_hybridswap_drain_force_swapout(void)
{
	wait_event(chs.force_swapout_wait,
		atomic64_read(&chs.force_swapout_inflight) == 0);
}

void crystal_hybridswap_suspend_auto_policy_sync(void)
{
	atomic_inc(&chs.policy_suspended);
	atomic64_set(&chs.pending_policy_wakeups, 0);
	if (chs.wq)
		cancel_delayed_work_sync(&chs.policy_work);
}

void crystal_hybridswap_resume_auto_policy(void)
{
	if (atomic_read(&chs.policy_suspended) > 0)
		atomic_dec(&chs.policy_suspended);
	crystal_hybridswap_update_auto_policy();
}

bool crystal_hybridswap_system_sleeping(void)
{
	return atomic_read(&chs.system_sleeping) > 0;
}

static int crystal_hybridswap_pm_notifier(struct notifier_block *nb,
					  unsigned long action, void *data)
{
	switch (action) {
	case PM_HIBERNATION_PREPARE:
	case PM_SUSPEND_PREPARE:
	case PM_RESTORE_PREPARE:
		atomic_set(&chs.system_sleeping, 1);
		crystal_hybridswap_suspend_auto_policy_sync();
		break;
	case PM_POST_HIBERNATION:
	case PM_POST_SUSPEND:
	case PM_POST_RESTORE:
		atomic_set(&chs.system_sleeping, 0);
		crystal_hybridswap_resume_auto_policy();
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block crystal_hybridswap_pm_nb = {
	.notifier_call = crystal_hybridswap_pm_notifier,
};

static void crystal_hybridswap_record_writeback(const char *mode, s64 pages,
						int ret, s64 written_pages)
{
	mutex_lock(&chs.state_lock);
	strscpy(chs.last_writeback_mode, mode, sizeof(chs.last_writeback_mode));
	mutex_unlock(&chs.state_lock);

	atomic64_set(&chs.stats.writeback_last_pages, pages);
	atomic64_set(&chs.stats.writeback_last_ret, ret);
	atomic64_set(&chs.stats.writeback_last_written_pages, written_pages);
	atomic64_set(&chs.stats.writeback_last_jiffies, jiffies);
}

static void crystal_hybridswap_record_batchin(s64 pages, int ret,
					      s64 batchin_pages)
{
	atomic64_set(&chs.stats.force_swapin_last_effective_pages, pages);
	atomic64_set(&chs.stats.force_swapin_last_ret, ret);
	atomic64_set(&chs.stats.force_swapin_last_batchin_pages, batchin_pages);
	atomic64_set(&chs.stats.force_swapin_last_jiffies, jiffies);
}

s64 crystal_hybridswap_mb_to_pages(s64 mb)
{
	if (mb <= 0)
		return CHS_PAGES_PER_MB;
	if (mb > S64_MAX / CHS_PAGES_PER_MB)
		return S64_MAX;

	return mb * CHS_PAGES_PER_MB;
}

void crystal_hybridswap_record_force_swapin(const char *memcg_name,
					    u64 cgroup_id, s64 app_score,
					    s64 request, s64 request_pages,
					    s64 effective_pages)
{
	atomic64_inc(&chs.stats.force_swapin);
	atomic64_add(request_pages, &chs.stats.force_swapin_pages);
	atomic64_set(&chs.stats.force_swapin_last_request_pages, request_pages);
	atomic64_set(&chs.stats.force_swapin_last_effective_pages,
		     effective_pages);
	chs_log(CHS_LOG_INFO,
		"force_swapin request memcg=%s id=%llu score=%lld request=%lld request_pages=%lld effective_pages=%lld scope=%s path=private_zram_batchin\n",
		memcg_name && memcg_name[0] ? memcg_name : "unknown",
		cgroup_id, app_score, request, request_pages, effective_pages,
		cgroup_id ? "per_memcg_page_level" : "global_best_effort");

	mutex_lock(&chs.state_lock);
	strscpy(chs.last_force_swapin_memcg,
		memcg_name && memcg_name[0] ? memcg_name : "unknown",
		sizeof(chs.last_force_swapin_memcg));
	chs.last_force_swapin_cgroup_id = cgroup_id;
	chs.last_force_swapin_app_score = app_score;
	chs.last_force_swapin_request = request;
	chs.last_force_swapin_pages = request_pages;
	chs.last_force_swapin_effective_pages = effective_pages;
	mutex_unlock(&chs.state_lock);
}

void crystal_hybridswap_record_force_swapout(const char *memcg_name,
					     u64 cgroup_id, s64 app_score,
					     s64 compat_trigger_value)
{
	atomic64_inc(&chs.stats.force_swapout);
	atomic64_set(&chs.stats.force_swapout_last_compat_trigger_value,
		     compat_trigger_value);
	chs_log(CHS_LOG_INFO,
		"force_swapout request target_memcg=%s target_cgroup_id=%llu score=%lld target_mode=per_memcg_best_effort compat_trigger_value=%lld scan_scope=zram_resident_pages\n",
		memcg_name && memcg_name[0] ? memcg_name : "unknown",
		cgroup_id, app_score, compat_trigger_value);

	mutex_lock(&chs.state_lock);
	strscpy(chs.last_force_swapout_memcg,
		memcg_name && memcg_name[0] ? memcg_name : "unknown",
		sizeof(chs.last_force_swapout_memcg));
	chs.last_force_swapout_cgroup_id = cgroup_id;
	chs.last_force_swapout_app_score = app_score;
	chs.last_force_swapout_mb = compat_trigger_value;
	chs.last_force_swapout_pages = 0;
	chs.last_force_swapout_effective_pages = 0;
	chs.last_force_swapout_compat_trigger_value = compat_trigger_value;
	chs.last_force_swapout_scanned_pages = 0;
	chs.last_force_swapout_eligible_pages = 0;
	chs.last_force_swapout_written_pages = 0;
	chs.last_force_swapout_unknown_or_filtered_pages = 0;
	chs.last_force_swapout_ret = 0;
	mutex_unlock(&chs.state_lock);
}

static void crystal_hybridswap_record_force_swapout_result(
		struct crystal_hybridswap_force_swapout_req *req,
		struct crystal_hybridswap_writeback_stats *wb_stats, int ret)
{
	s64 written = wb_stats ? wb_stats->written_pages : 0;
	s64 scanned = wb_stats ? wb_stats->scanned_pages : 0;
	s64 eligible = wb_stats ? wb_stats->eligible_pages : 0;
	s64 unknown_or_filtered = wb_stats ?
		wb_stats->unknown_or_filtered_pages : 0;

	atomic64_add(written, &chs.stats.force_swapout_pages);
	atomic64_set(&chs.stats.force_swapout_effective_pages, written);
	atomic64_set(&chs.stats.force_swapout_last_compat_trigger_value,
		     req->compat_trigger_value);
	atomic64_set(&chs.stats.force_swapout_last_scanned_pages, scanned);
	atomic64_set(&chs.stats.force_swapout_last_eligible_pages, eligible);
	atomic64_set(&chs.stats.force_swapout_last_written_pages, written);
	atomic64_set(&chs.stats.force_swapout_last_unknown_or_filtered_pages,
		     unknown_or_filtered);
	atomic64_set(&chs.stats.force_swapout_last_ret, ret);
	atomic64_set(&chs.stats.force_swapout_last_jiffies, jiffies);

	mutex_lock(&chs.state_lock);
	strscpy(chs.last_force_swapout_memcg,
		req->memcg_name[0] ? req->memcg_name : "unknown",
		sizeof(chs.last_force_swapout_memcg));
	chs.last_force_swapout_cgroup_id = req->target_cgroup_id;
	chs.last_force_swapout_app_score = req->app_score;
	chs.last_force_swapout_mb = req->compat_trigger_value;
	chs.last_force_swapout_pages = written;
	chs.last_force_swapout_effective_pages = written;
	chs.last_force_swapout_compat_trigger_value =
		req->compat_trigger_value;
	chs.last_force_swapout_scanned_pages = scanned;
	chs.last_force_swapout_eligible_pages = eligible;
	chs.last_force_swapout_written_pages = written;
	chs.last_force_swapout_unknown_or_filtered_pages = unknown_or_filtered;
	chs.last_force_swapout_ret = ret;
	mutex_unlock(&chs.state_lock);

	crystal_hybridswap_memcg_record_force_swapout_result(
		req->target_cgroup_id, scanned, eligible, written,
		unknown_or_filtered, ret);
}

static void crystal_hybridswap_record_force_swapout_queue_failure(
		u64 target_cgroup_id, const char *memcg_name, s64 app_score,
		s64 compat_trigger_value, s64 target_pages, bool auto_req, int ret)
{
	struct crystal_hybridswap_force_swapout_req req = { };
	struct crystal_hybridswap_writeback_stats wb_stats = { };

	req.target_cgroup_id = target_cgroup_id;
	req.app_score = app_score;
	req.compat_trigger_value = compat_trigger_value;
	req.target_pages = target_pages;
	req.auto_req = auto_req;
	strscpy(req.memcg_name,
		memcg_name && memcg_name[0] ? memcg_name : "unknown",
		sizeof(req.memcg_name));

	atomic64_inc(&chs.stats.force_swapout_error);
	crystal_hybridswap_record_force_swapout_result(&req, &wb_stats, ret);
}

void crystal_hybridswap_copy_last_writeback_mode(char *buf, size_t len)
{
	if (!buf || !len)
		return;

	mutex_lock(&chs.state_lock);
	strscpy(buf, chs.last_writeback_mode[0] ?
		chs.last_writeback_mode : "none", len);
	mutex_unlock(&chs.state_lock);
}

void crystal_hybridswap_copy_last_auto_reason(char *buf, size_t len)
{
	if (!buf || !len)
		return;

	mutex_lock(&chs.state_lock);
	strscpy(buf, chs.last_auto_reason[0] ?
		chs.last_auto_reason : "none", len);
	mutex_unlock(&chs.state_lock);
}

static void crystal_hybridswap_record_auto_reason(const char *reason)
{
	mutex_lock(&chs.state_lock);
	strscpy(chs.last_auto_reason, reason && reason[0] ? reason : "none",
		sizeof(chs.last_auto_reason));
	mutex_unlock(&chs.state_lock);
}

static bool crystal_hybridswap_auto_result_needs_backoff(int ret,
		s64 written_pages)
{
	return ret == -ENODATA || ret == -ENODEV || ret == -ENXIO ||
		ret == -EDQUOT || ret == -ENOSPC || ret == -EOPNOTSUPP ||
		(ret >= 0 && written_pages <= 0);
}

static const char *crystal_hybridswap_auto_failure_reason(int ret)
{
	switch (ret) {
	case -ENODATA:
		return "auto_writeback_no_data";
	case -ENODEV:
		return "auto_no_target";
	case -ENXIO:
		return "auto_no_backing_dev";
	case -EDQUOT:
		return "auto_writeback_limit";
	case -ENOSPC:
		return "auto_writeback_nospace";
	case -EOPNOTSUPP:
		return "auto_writeback_unsupported";
	default:
		return "auto_writeback_error";
	}
}

static void crystal_hybridswap_auto_reset_failure_state(void)
{
	mutex_lock(&chs.state_lock);
	chs.auto_last_failure_reason[0] = '\0';
	chs.auto_last_failure_ret = 0;
	chs.auto_last_failure_repeats = 0;
	chs.auto_last_failure_log_jiffies = 0;
	mutex_unlock(&chs.state_lock);
}

static bool crystal_hybridswap_auto_should_log_failure(const char *reason,
		int ret, unsigned long now, u32 *repeats, bool *changed)
{
	bool do_log = false;
	unsigned long interval;
	bool state_changed;

	mutex_lock(&chs.state_lock);
	state_changed = chs.auto_last_failure_ret != ret ||
		strncmp(chs.auto_last_failure_reason, reason,
			sizeof(chs.auto_last_failure_reason));
	if (state_changed) {
		strscpy(chs.auto_last_failure_reason, reason,
			sizeof(chs.auto_last_failure_reason));
		chs.auto_last_failure_ret = ret;
		chs.auto_last_failure_repeats = 0;
		chs.auto_last_failure_log_jiffies = 0;
	} else {
		chs.auto_last_failure_repeats++;
	}
	interval = chs.auto_empty_skip_jiffies;
	if (!interval)
		interval = msecs_to_jiffies(
			CHS_AUTO_POLICY_EMPTY_BACKOFF_BASE_MS);
	if (state_changed || !chs.auto_last_failure_log_jiffies ||
	    time_after_eq(now, chs.auto_last_failure_log_jiffies + interval)) {
		chs.auto_last_failure_log_jiffies = now;
		do_log = true;
	}
	if (repeats)
		*repeats = chs.auto_last_failure_repeats;
	if (changed)
		*changed = state_changed;
	mutex_unlock(&chs.state_lock);

	return do_log;
}

static void crystal_hybridswap_auto_log_failure(int level, const char *reason,
		int ret, const char *detail)
{
	u32 repeats = 0;
	bool changed = false;

	if (!crystal_hybridswap_auto_should_log_failure(reason, ret, jiffies,
			&repeats, &changed))
		return;

	chs_log(changed ? level : CHS_LOG_DEBUG,
		"auto_writeback stable_failure reason=%s ret=%d repeats=%u backoff_ms=%lld detail=%s\n",
		reason, ret, repeats,
		atomic64_read(&chs.stats.policy_empty_backoff_interval_ms),
		detail && detail[0] ? detail : "none");
}

static s64 crystal_hybridswap_clamp_u64_to_s64(u64 val)
{
	return val > S64_MAX ? S64_MAX : (s64)val;
}

static void crystal_hybridswap_atomic64_update_max(atomic64_t *max, s64 val)
{
	s64 old;

	if (val <= 0)
		return;

	old = atomic64_read(max);
	while (old < val) {
		s64 prev = atomic64_cmpxchg(max, old, val);

		if (prev == old)
			break;
		old = prev;
	}
}

static void crystal_hybridswap_record_worker_latency(atomic64_t *total,
		atomic64_t *max, atomic64_t *slow, atomic64_t *last, u64 ns,
		const char *worker, int ret)
{
	s64 ns_s64 = crystal_hybridswap_clamp_u64_to_s64(ns);

	atomic64_add(ns_s64, total);
	atomic64_set(last, ns_s64);
	crystal_hybridswap_atomic64_update_max(max, ns_s64);
	if (ns >= CHS_ZRAM_SLOW_WORK_NS) {
		atomic64_inc(slow);
		chs_log_ratelimited(CHS_LOG_WARN,
			"slow worker name=%s latency_ns=%llu ret=%d\n",
			worker ? worker : "unknown", (unsigned long long)ns,
			ret);
	}
}

static void crystal_hybridswap_add_writeback_stats(
		struct crystal_hybridswap_writeback_stats *dst,
		const struct crystal_hybridswap_writeback_stats *src)
{
	if (!dst || !src)
		return;
	dst->scanned_pages += src->scanned_pages;
	dst->eligible_pages += src->eligible_pages;
	dst->written_pages += src->written_pages;
	dst->unknown_or_filtered_pages += src->unknown_or_filtered_pages;
}

static void crystal_hybridswap_add_batchin_stats(
		struct crystal_hybridswap_batchin_stats *dst,
		const struct crystal_hybridswap_batchin_stats *src)
{
	if (!dst || !src)
		return;
	dst->scanned_pages += src->scanned_pages;
	dst->matched_pages += src->matched_pages;
	dst->moved_pages += src->moved_pages;
	dst->skipped_pages += src->skipped_pages;
	dst->filtered_pages += src->filtered_pages;
	dst->read_errors += src->read_errors;
	dst->prepare_errors += src->prepare_errors;
	dst->snapshot_mismatch += src->snapshot_mismatch;
	if (src->last_error)
		dst->last_error = src->last_error;
}

static u64 crystal_hybridswap_pages_to_bytes_u64(u64 pages)
{
	if (pages > (U64_MAX >> PAGE_SHIFT))
		return U64_MAX;
	return pages << PAGE_SHIFT;
}

static u64 crystal_hybridswap_scale_u64_percent(u64 val, unsigned int percent)
{
	u64 whole;
	u64 rem;

	if (!val || !percent)
		return 0;
	if (percent >= 100)
		return val;

	whole = div64_u64(val, 100);
	rem = val - whole * 100;
	return whole * percent + div64_u64(rem * percent, 100);
}

static unsigned int crystal_hybridswap_dev_life_quota_percent(unsigned int level)
{
	if (!level)
		return 100;
	if (level == 1)
		return 75;
	if (level == 2)
		return 50;
	return 100 / CHS_DEV_LIFE_QUOTA_DIVISOR;
}

static unsigned int crystal_hybridswap_dev_life_auto_percent(unsigned int level)
{
	if (!level)
		return 100;
	if (level == 1)
		return 75;
	if (level == 2)
		return 50;
	return CHS_DEV_LIFE_AUTO_BUDGET_PERCENT;
}

static u64 crystal_hybridswap_effective_quota_limit(u64 quota,
		unsigned int life_level, bool use_dev_life, bool account_scale)
{
	unsigned int percent;
	u64 effective;

	if (!quota)
		return 0;
	if (!use_dev_life || !life_level)
		return quota;

	percent = crystal_hybridswap_dev_life_quota_percent(life_level);
	effective = crystal_hybridswap_scale_u64_percent(quota, percent);
	if (effective < quota && account_scale)
		atomic64_inc(&chs.stats.dev_life_quota_scaled);

	return effective;
}

static void crystal_hybridswap_update_quota_stats_locked(u64 quota,
		u64 effective_quota)
{
	u64 used_bytes = crystal_hybridswap_pages_to_bytes_u64(
		chs.quota_used_pages);
	u64 remaining_bytes;
	u64 remaining_pages;

	if (!effective_quota) {
		remaining_bytes = U64_MAX;
		remaining_pages = S64_MAX;
	} else if (effective_quota > used_bytes) {
		remaining_bytes = effective_quota - used_bytes;
		remaining_pages = remaining_bytes >> PAGE_SHIFT;
	} else {
		remaining_bytes = 0;
		remaining_pages = 0;
	}

	atomic64_set(&chs.stats.writeback_quota_limit_bytes,
		     crystal_hybridswap_clamp_u64_to_s64(quota));
	atomic64_set(&chs.stats.writeback_quota_effective_limit_bytes,
		     crystal_hybridswap_clamp_u64_to_s64(effective_quota));
	atomic64_set(&chs.stats.writeback_quota_used_pages,
		     crystal_hybridswap_clamp_u64_to_s64(chs.quota_used_pages));
	atomic64_set(&chs.stats.writeback_quota_used_bytes,
		     crystal_hybridswap_clamp_u64_to_s64(used_bytes));
	atomic64_set(&chs.stats.writeback_quota_remaining_pages,
		     crystal_hybridswap_clamp_u64_to_s64(remaining_pages));
	atomic64_set(&chs.stats.writeback_quota_remaining_bytes,
		     crystal_hybridswap_clamp_u64_to_s64(remaining_bytes));
	atomic64_set(&chs.stats.writeback_quota_window_start_jiffies,
		     chs.quota_window_start);
}

static void crystal_hybridswap_refresh_quota_window_locked(unsigned long now)
{
	unsigned long window = max_t(unsigned long, 1,
		msecs_to_jiffies(CHS_QUOTA_WINDOW_MS));
	u64 quota = atomic64_read(&chs.quota_day);
	u64 effective_quota;

	if (!chs.quota_window_start) {
		chs.quota_window_start = now;
	} else if (time_after_eq(now, chs.quota_window_start + window)) {
		chs.quota_window_start = now;
		chs.quota_used_pages = 0;
		atomic64_inc(&chs.stats.writeback_quota_resets);
	}

	effective_quota = crystal_hybridswap_effective_quota_limit(quota,
		crystal_hybridswap_dev_life_level(), true, false);
	crystal_hybridswap_update_quota_stats_locked(quota, effective_quota);
}

static s64 crystal_hybridswap_quota_remaining_pages(bool use_dev_life,
		bool account_scale)
{
	unsigned long now = jiffies;
	u64 quota;
	u64 effective_quota;
	u64 used_bytes;
	s64 remaining_pages;

	mutex_lock(&chs.state_lock);
	crystal_hybridswap_refresh_quota_window_locked(now);
	quota = atomic64_read(&chs.quota_day);
	effective_quota = crystal_hybridswap_effective_quota_limit(quota,
		crystal_hybridswap_dev_life_level(), use_dev_life, account_scale);
	used_bytes = crystal_hybridswap_pages_to_bytes_u64(chs.quota_used_pages);
	if (!effective_quota)
		remaining_pages = CHS_FORCE_GLOBAL_SCAN_PAGES;
	else if (effective_quota > used_bytes)
		remaining_pages = crystal_hybridswap_clamp_u64_to_s64(
			(effective_quota - used_bytes) >> PAGE_SHIFT);
	else
		remaining_pages = 0;
	crystal_hybridswap_update_quota_stats_locked(quota, effective_quota);
	mutex_unlock(&chs.state_lock);

	return remaining_pages;
}

static s64 crystal_hybridswap_limit_writeback_pages(s64 pages, bool auto_req,
		bool force, const char *source)
{
	s64 requested = pages;
	s64 remaining;

	if (requested <= 0)
		requested = 1;

	if (force && crystal_hybridswap_dev_life())
		atomic64_inc(&chs.stats.dev_life_force_soft_bypass);

	remaining = crystal_hybridswap_quota_remaining_pages(!force, true);
	if (remaining >= CHS_FORCE_GLOBAL_SCAN_PAGES)
		return requested;

	if (remaining <= 0) {
		atomic64_inc(&chs.stats.writeback_quota_skipped);
		chs_log_ratelimited(CHS_LOG_INFO,
			"%s writeback blocked by daily quota requested=%lld auto=%d force=%d quota=%lld used_pages=%lld\n",
			source ? source : "unknown", requested, auto_req, force,
			atomic64_read(&chs.stats.writeback_quota_effective_limit_bytes),
			atomic64_read(&chs.stats.writeback_quota_used_pages));
		return 0;
	}

	if (requested >= CHS_FORCE_GLOBAL_SCAN_PAGES || requested > remaining) {
		atomic64_inc(&chs.stats.writeback_quota_capped);
		chs_log_ratelimited(CHS_LOG_INFO,
			"%s writeback capped by daily quota requested=%lld effective=%lld auto=%d force=%d\n",
			source ? source : "unknown", requested, remaining,
			auto_req, force);
		return remaining;
	}

	return requested;
}

static u64 crystal_hybridswap_apply_dev_life_auto_budget(u64 pages)
{
	unsigned int level = crystal_hybridswap_dev_life_level();
	unsigned int percent;
	u64 scaled;

	if (!level || !pages)
		return pages;

	percent = crystal_hybridswap_dev_life_auto_percent(level);
	scaled = crystal_hybridswap_scale_u64_percent(pages, percent);
	if (!scaled)
		scaled = 1;
	if (scaled < pages)
		atomic64_inc(&chs.stats.dev_life_auto_scaled);
	return scaled;
}

void crystal_hybridswap_account_physical_write_pages(unsigned long pages)
{
	if (!pages)
		return;

	mutex_lock(&chs.state_lock);
	crystal_hybridswap_refresh_quota_window_locked(jiffies);
	if (chs.quota_used_pages > U64_MAX - pages)
		chs.quota_used_pages = U64_MAX;
	else
		chs.quota_used_pages += pages;
	crystal_hybridswap_update_quota_stats_locked(
		atomic64_read(&chs.quota_day),
		crystal_hybridswap_effective_quota_limit(
			atomic64_read(&chs.quota_day),
			crystal_hybridswap_dev_life_level(), true, false));
	mutex_unlock(&chs.state_lock);
}

static u64 crystal_hybridswap_mb_to_pages_u64(u64 mb)
{
	if (!mb)
		return 0;
	if (mb > (u64)S64_MAX / CHS_PAGES_PER_MB)
		return S64_MAX;

	return mb * CHS_PAGES_PER_MB;
}

static u64 crystal_hybridswap_pages_to_mb_u64(u64 pages)
{
	if (!pages)
		return 0;

	return div64_u64(pages, CHS_PAGES_PER_MB);
}

static long crystal_hybridswap_android_avail_pages(bool *fallback)
{
	long available;
	unsigned long pagecache;
	unsigned long wmark_low = 0;
	unsigned long reclaimable;
	struct zone *zone;

	if (fallback)
		*fallback = false;

	if (!CHS_PAGES_PER_MB || !totalreserve_pages) {
		if (fallback)
			*fallback = true;
		return si_mem_available();
	}

	available = global_zone_page_state(NR_FREE_PAGES) - totalreserve_pages;
	for_each_zone(zone)
		wmark_low += low_wmark_pages(zone);

	pagecache = global_node_page_state(NR_ACTIVE_FILE) +
		global_node_page_state(NR_INACTIVE_FILE);
	pagecache -= min(pagecache / 2, wmark_low);
	available += pagecache;

	reclaimable = global_node_page_state_pages(NR_SLAB_RECLAIMABLE_B) +
		global_node_page_state(NR_KERNEL_MISC_RECLAIMABLE);
	available += reclaimable - min(reclaimable / 2, wmark_low);

	if (available < 0)
		available = 0;
	return available;
}

static unsigned int crystal_hybridswap_current_avail_mb(void)
{
	bool fallback = false;
	long pages = crystal_hybridswap_android_avail_pages(&fallback);
	s64 mb;

	if (pages <= 0)
		pages = 0;
	mb = pages / CHS_PAGES_PER_MB;
	if (mb > UINT_MAX)
		mb = UINT_MAX;

	atomic64_set(&chs.stats.policy_available_android_pages, pages);
	atomic64_set(&chs.stats.policy_available_android_mb, mb);
	if (fallback)
		atomic64_inc(&chs.stats.policy_available_fallback);

	return mb;
}

static int crystal_hybridswap_collect_zram_targets(
		struct crystal_hybridswap_zram_target *targets, int max_targets)
{
	struct crystal_hybridswap_zram *entry;
	int count = 0;
	int registered = 0;
	int i;

	if (!targets || max_targets <= 0)
		return 0;

	memset(targets, 0, sizeof(*targets) * max_targets);
	mutex_lock(&chs.zram_lock);
	list_for_each_entry(entry, &chs.zram_list, node) {
		registered++;
		if (count >= max_targets || !entry->dev || !entry->zram)
			continue;
		if (!zram_try_get(entry->zram))
			continue;
		targets[count].dev = entry->dev;
		targets[count].zram = entry->zram;
		targets[count].writeback = entry->writeback;
		targets[count].force_writeback = entry->force_writeback;
		targets[count].force_writeback_ext = entry->force_writeback_ext;
		targets[count].batchin = entry->batchin;
		targets[count].zram_increase_pages = entry->zram_increase_pages;
		get_device(targets[count].dev);
		count++;
	}
	mutex_unlock(&chs.zram_lock);

	atomic64_set(&chs.stats.multi_zram_registered, registered);
	atomic64_set(&chs.stats.multi_zram_last_traversed, registered);
	for (i = 0; i < count; i++) {
		int ret = crystal_hybridswap_zram_pressure_snapshot(
			targets[i].dev, &targets[i].pressure);

		if (!ret)
			targets[i].pressure.increase_pages =
				targets[i].zram_increase_pages;
	}

	return count;
}

static void crystal_hybridswap_put_zram_targets(
		struct crystal_hybridswap_zram_target *targets, int count)
{
	int i;

	for (i = 0; i < count; i++) {
		if (targets[i].zram)
			zram_put(targets[i].zram);
		if (targets[i].dev)
			put_device(targets[i].dev);
	}
}

static void crystal_hybridswap_add_zram_io_stats(
		struct crystal_hybridswap_zram_io_stats *dst,
		const struct crystal_hybridswap_zram_io_stats *src)
{
	if (!dst || !src)
		return;

	dst->devices_count += src->devices_count;
	dst->last_device_index = src->last_device_index;
	dst->bd_pages += src->bd_pages;
	dst->bd_compressed_bytes += src->bd_compressed_bytes;
	dst->bd_read_pages += src->bd_read_pages;
	dst->bd_write_pages += src->bd_write_pages;
	dst->bd_physical_read_pages += src->bd_physical_read_pages;
	dst->bd_physical_read_ios_count += src->bd_physical_read_ios_count;
	dst->bd_physical_read_failed_pages +=
		src->bd_physical_read_failed_pages;
	dst->bd_physical_write_pages += src->bd_physical_write_pages;
	dst->bd_physical_write_bytes += src->bd_physical_write_bytes;
	dst->bd_physical_write_ios_count += src->bd_physical_write_ios_count;
	dst->bd_physical_write_failed_pages +=
		src->bd_physical_write_failed_pages;
	dst->bd_read_sync_ios_count += src->bd_read_sync_ios_count;
	dst->bd_read_async_ios_count += src->bd_read_async_ios_count;
	dst->bd_read_failures_count += src->bd_read_failures_count;
	dst->bd_read_last_ret = src->bd_read_last_ret;
	dst->bd_read_total_ns += src->bd_read_total_ns;
	dst->bd_read_max_ns = max(dst->bd_read_max_ns, src->bd_read_max_ns);
	dst->bd_read_slow_ios_count += src->bd_read_slow_ios_count;
	if (src->bd_read_slow_ios_count) {
		dst->bd_read_last_slow_device_index =
			src->bd_read_last_slow_device_index;
		dst->bd_read_last_slow_sector_index =
			src->bd_read_last_slow_sector_index;
		dst->bd_read_last_slow_slot_index =
			src->bd_read_last_slow_slot_index;
		dst->bd_read_last_slow_ret = src->bd_read_last_slow_ret;
		dst->bd_read_last_slow_ns = src->bd_read_last_slow_ns;
	}
	dst->bd_write_ios_count += src->bd_write_ios_count;
	dst->bd_write_failures_count += src->bd_write_failures_count;
	dst->bd_write_last_ret = src->bd_write_last_ret;
	dst->bd_write_total_ns += src->bd_write_total_ns;
	dst->bd_write_max_ns = max(dst->bd_write_max_ns, src->bd_write_max_ns);
	dst->bd_write_slow_ios_count += src->bd_write_slow_ios_count;
	if (src->bd_write_slow_ios_count) {
		dst->bd_write_last_slow_device_index =
			src->bd_write_last_slow_device_index;
		dst->bd_write_last_slow_sector_index =
			src->bd_write_last_slow_sector_index;
		dst->bd_write_last_slow_slot_index =
			src->bd_write_last_slow_slot_index;
		dst->bd_write_last_slow_ret = src->bd_write_last_slow_ret;
		dst->bd_write_last_slow_ns = src->bd_write_last_slow_ns;
	}
	dst->batchin_runs_count += src->batchin_runs_count;
	dst->batchin_pages += src->batchin_pages;
	dst->batchin_failures_count += src->batchin_failures_count;
	dst->batchin_no_data_count += src->batchin_no_data_count;
	dst->batchin_filtered_pages += src->batchin_filtered_pages;
	dst->batchin_read_errors_count += src->batchin_read_errors_count;
	dst->batchin_prepare_errors_count += src->batchin_prepare_errors_count;
	dst->batchin_snapshot_mismatch_pages +=
		src->batchin_snapshot_mismatch_pages;
	dst->batchin_total_ns += src->batchin_total_ns;
	dst->batchin_max_ns = max(dst->batchin_max_ns, src->batchin_max_ns);
	dst->batchin_slow_runs_count += src->batchin_slow_runs_count;
	if (src->batchin_runs_count)
		dst->batchin_last_ret = src->batchin_last_ret;
	dst->batchin_zms_batches += src->batchin_zms_batches;
	dst->batchin_zms_items += src->batchin_zms_items;
	dst->batchin_zms_read_ios += src->batchin_zms_read_ios;
	dst->prefetch_runs += src->prefetch_runs;
	dst->prefetch_candidates += src->prefetch_candidates;
	dst->prefetch_submitted += src->prefetch_submitted;
	dst->prefetch_moved += src->prefetch_moved;
	dst->prefetch_skipped += src->prefetch_skipped;
	dst->prefetch_read_errors += src->prefetch_read_errors;
	dst->prefetch_prepare_errors += src->prefetch_prepare_errors;
	dst->prefetch_snapshot_mismatch += src->prefetch_snapshot_mismatch;
	dst->prefetch_no_data += src->prefetch_no_data;
	dst->prefetch_alloc_failures += src->prefetch_alloc_failures;
	dst->prefetch_hits += src->prefetch_hits;
	dst->prefetch_stale_hits += src->prefetch_stale_hits;
	dst->prefetch_expired += src->prefetch_expired;
	dst->prefetch_reclaimed += src->prefetch_reclaimed;
	dst->prefetch_invalidated += src->prefetch_invalidated;
	dst->prefetch_hit_pct = dst->prefetch_moved ?
		mul_u64_u64_div_u64(dst->prefetch_hits, 100,
				    dst->prefetch_moved) : 0;
	dst->scan_errors_count += src->scan_errors_count;
}

int crystal_hybridswap_collect_zram_io_stats(
		struct crystal_hybridswap_zram_io_stats *stats)
{
	struct crystal_hybridswap_zram_target *targets;
	struct crystal_hybridswap_zram_io_stats *cur;
	int count;
	int ret;
	int i;

	if (!stats)
		return -EINVAL;

	memset(stats, 0, sizeof(*stats));
	stats->last_device_index = -1;
	stats->bd_read_last_slow_device_index = -1;
	stats->bd_write_last_slow_device_index = -1;

	targets = kcalloc(CHS_MAX_ZRAM_TARGETS, sizeof(*targets), GFP_KERNEL);
	if (!targets)
		return -ENOMEM;

	cur = kzalloc(sizeof(*cur), GFP_KERNEL);
	if (!cur) {
		kfree(targets);
		return -ENOMEM;
	}

	count = crystal_hybridswap_collect_zram_targets(targets,
		CHS_MAX_ZRAM_TARGETS);
	if (!count) {
		ret = -ENODEV;
		goto out_free;
	}

	for (i = 0; i < count; i++) {
		ret = crystal_hybridswap_zram_io_stats(targets[i].dev, cur);
		if (ret) {
			stats->scan_errors_count++;
			continue;
		}
		crystal_hybridswap_add_zram_io_stats(stats, cur);
	}

	ret = stats->devices_count ? 0 : -ENODATA;
	crystal_hybridswap_put_zram_targets(targets, count);
out_free:
	kfree(cur);
	kfree(targets);
	return ret;
}

static s64 crystal_hybridswap_zram_target_id(
		const struct crystal_hybridswap_zram_target *target)
{
	if (!target || !target->pressure.valid)
		return -1;
	return crystal_hybridswap_clamp_u64_to_s64(target->pressure.device_id);
}

static int crystal_hybridswap_writeback_selection_ret(
		const struct crystal_hybridswap_writeback_target_selection *selection,
		int count)
{
	if (!count)
		return -ENODEV;
	if (!selection)
		return -ENODEV;
	if (selection->eligible > 0)
		return -EAGAIN;
	if (selection->limit_exhausted && !selection->no_backing &&
	    !selection->no_resident)
		return -EDQUOT;
	if (selection->no_backing && !selection->limit_exhausted &&
	    !selection->no_resident)
		return -ENXIO;
	if (selection->no_resident && !selection->limit_exhausted &&
	    !selection->no_backing)
		return -ENODATA;
	if (selection->no_writeback == count)
		return -EOPNOTSUPP;
	if (selection->limit_exhausted)
		return -EDQUOT;
	if (selection->no_backing)
		return -ENXIO;
	if (selection->no_resident)
		return -ENODATA;
	return -ENODEV;
}

static int crystal_hybridswap_select_writeback_target(
		struct crystal_hybridswap_zram_target *targets, int count,
		struct crystal_hybridswap_writeback_target_selection *selection)
{
	struct crystal_hybridswap_writeback_target_selection result = {
		.best = -1,
		.ret = -ENODEV,
	};
	u64 best_score = 0;
	int i;

	atomic64_set(&chs.stats.multi_zram_last_selected, -1);
	atomic64_set(&chs.stats.multi_zram_last_eligible, 0);

	for (i = 0; i < count; i++) {
		u64 score;

		if (!targets[i].writeback) {
			result.no_writeback++;
			continue;
		}
		if (!targets[i].pressure.valid || !targets[i].pressure.backing_dev) {
			result.no_backing++;
			atomic64_inc(&chs.stats.multi_zram_skip_no_backing);
			continue;
		}
		if (targets[i].pressure.wb_limit_exhausted) {
			result.limit_exhausted++;
			atomic64_inc(&chs.stats.multi_zram_skip_limit);
			continue;
		}
		score = targets[i].pressure.resident_pages +
			targets[i].pressure.increase_pages;
		if (!score) {
			result.no_resident++;
			atomic64_inc(&chs.stats.multi_zram_skip_no_resident);
			continue;
		}
		result.eligible++;
		if (result.best < 0 || score > best_score) {
			result.best = i;
			best_score = score;
		}
	}

	atomic64_set(&chs.stats.multi_zram_last_eligible, result.eligible);
	if (result.best >= 0) {
		atomic64_set(&chs.stats.multi_zram_last_selected,
			crystal_hybridswap_zram_target_id(&targets[result.best]));
		result.ret = 0;
	} else {
		result.ret = crystal_hybridswap_writeback_selection_ret(&result,
			count);
	}
	if (selection)
		*selection = result;
	return result.best;
}

static noinline_for_stack int crystal_hybridswap_get_zram_pressure(
		struct crystal_hybridswap_zram_pressure *pressure)
{
	struct crystal_hybridswap_zram_target *targets;
	int count;
	int ret = -ENODEV;
	int i;

	if (!pressure)
		return -EINVAL;

	memset(pressure, 0, sizeof(*pressure));
	targets = kcalloc(CHS_MAX_ZRAM_TARGETS, sizeof(*targets), GFP_KERNEL);
	if (!targets)
		return -ENOMEM;
	count = crystal_hybridswap_collect_zram_targets(targets,
		CHS_MAX_ZRAM_TARGETS);
	for (i = 0; i < count; i++) {
		struct crystal_hybridswap_zram_pressure *cur = &targets[i].pressure;

		if (!cur->valid)
			continue;
		pressure->stored_pages += cur->stored_pages;
		pressure->same_pages += cur->same_pages;
		pressure->writeback_pages += cur->writeback_pages;
		pressure->resident_pages += cur->resident_pages;
		pressure->total_pages += cur->total_pages;
		pressure->increase_pages += cur->increase_pages;
		pressure->valid = true;
		ret = 0;
	}
	crystal_hybridswap_put_zram_targets(targets, count);

	if (!ret) {
		pressure->resident_ratio = crystal_hybridswap_u64_percent(
			pressure->resident_pages, pressure->total_pages);
		atomic64_set(&chs.stats.policy_zram_pressure_ratio,
			     pressure->resident_ratio);
		atomic64_set(&chs.stats.policy_zram_resident_pages,
			     crystal_hybridswap_clamp_u64_to_s64(
			     pressure->resident_pages));
		atomic64_set(&chs.stats.policy_zram_total_pages,
			     crystal_hybridswap_clamp_u64_to_s64(
			     pressure->total_pages));
		atomic64_set(&chs.stats.policy_zram_increase_pages,
			     crystal_hybridswap_clamp_u64_to_s64(
			     pressure->increase_pages));
		mutex_lock(&chs.state_lock);
		chs.last_zram_pressure = *pressure;
		mutex_unlock(&chs.state_lock);
	}

	kfree(targets);
	return ret;
}

int crystal_hybridswap_collect_memcg_zram_stats(u64 cgroup_id,
		struct crystal_hybridswap_memcg_zram_stats *stats)
{
	struct crystal_hybridswap_zram_target *targets;
	struct crystal_hybridswap_memcg_zram_stats *cur;
	int count;
	int ret = -ENODATA;
	int i;

	if (!stats || !cgroup_id)
		return -EINVAL;

	targets = kcalloc(CHS_MAX_ZRAM_TARGETS, sizeof(*targets), GFP_KERNEL);
	if (!targets)
		return -ENOMEM;
	cur = kzalloc(sizeof(*cur), GFP_KERNEL);
	if (!cur) {
		kfree(targets);
		return -ENOMEM;
	}

	memset(stats, 0, sizeof(*stats));
	count = crystal_hybridswap_collect_zram_targets(targets,
		CHS_MAX_ZRAM_TARGETS);
	for (i = 0; i < count; i++) {
		int err;

		memset(cur, 0, sizeof(*cur));
		stats->devices_scanned++;
		err = crystal_hybridswap_zram_memcg_stats(targets[i].dev,
			cgroup_id, cur);
		if (err) {
			if (err != -ENODATA)
				stats->scan_errors++;
			continue;
		}
		stats->devices_with_data++;
		stats->resident_pages += cur->resident_pages;
		stats->writeback_pages += cur->writeback_pages;
		stats->same_pages += cur->same_pages;
		stats->huge_pages += cur->huge_pages;
		stats->zram_compressed_size += cur->zram_compressed_size;
		stats->zram_original_size += cur->zram_original_size;
		stats->writeback_size += cur->writeback_size;
		stats->writeback_original_size += cur->writeback_original_size;
		stats->scanned_pages += cur->scanned_pages;
		stats->matched_pages += cur->matched_pages;
		ret = 0;
	}
	crystal_hybridswap_put_zram_targets(targets, count);
	kfree(cur);
	kfree(targets);
	if (!count)
		return -ENODEV;
	return ret;
}

static unsigned int crystal_hybridswap_zram_effective_ratio(
		const struct crystal_hybridswap_zram_pressure *pressure)
{
	u64 boosted_pages;
	u64 effective_pages;

	if (!pressure || !pressure->valid || !pressure->total_pages)
		return 0;

	boosted_pages = div64_u64(pressure->increase_pages,
		CHS_ZRAM_INCREASE_PRESSURE_DIVISOR);
	atomic64_set(&chs.stats.policy_zram_increase_boost_pages,
		     crystal_hybridswap_clamp_u64_to_s64(boosted_pages));
	effective_pages = pressure->resident_pages;
	if (effective_pages > U64_MAX - boosted_pages)
		effective_pages = U64_MAX;
	else
		effective_pages += boosted_pages;

	if (effective_pages >= pressure->total_pages)
		return 100;
	return crystal_hybridswap_u64_percent(effective_pages,
		pressure->total_pages);
}

static bool crystal_hybridswap_zram_gate(
		const struct crystal_hybridswap_zram_pressure *pressure,
		bool free_swap_low)
{
	unsigned int wm_ratio = crystal_hybridswap_zram_wm_ratio();
	unsigned int effective_ratio;
	bool gate = free_swap_low;

	effective_ratio = crystal_hybridswap_zram_effective_ratio(pressure);
	if (pressure && pressure->valid && effective_ratio >= wm_ratio)
		gate = true;

	atomic64_set(&chs.stats.policy_zram_wm_ratio, wm_ratio);
	atomic64_set(&chs.stats.policy_zram_effective_ratio, effective_ratio);
	atomic64_set(&chs.stats.policy_zram_gate_result, gate);
	return gate;
}

static u64 crystal_hybridswap_auto_target_pages(unsigned int avail,
		unsigned int min_avail, unsigned int high_avail,
		unsigned int cur_avail, bool free_swap_low,
		const struct crystal_hybridswap_zram_pressure *zram_pressure)
{
	u64 target_avail;
	u64 pages_to_write;

	target_avail = high_avail ? high_avail : avail;
	if (!target_avail)
		target_avail = min_avail;
	if (target_avail > cur_avail)
		pages_to_write = (target_avail - cur_avail) * CHS_PAGES_PER_MB;
	else if (free_swap_low)
		pages_to_write = CHS_PAGES_PER_MB;
	else
		pages_to_write = 1;

	if (zram_pressure && zram_pressure->increase_pages) {
		u64 increase_budget = div64_u64(zram_pressure->increase_pages,
			CHS_ZRAM_INCREASE_BUDGET_DIVISOR);

		atomic64_set(&chs.stats.policy_zram_increase_budget_pages,
			crystal_hybridswap_clamp_u64_to_s64(increase_budget));
		if (pages_to_write > U64_MAX - increase_budget)
			pages_to_write = U64_MAX;
		else
			pages_to_write += increase_budget;
	}

	return pages_to_write;
}

static void crystal_hybridswap_auto_record_result(int ret, s64 written_pages,
		bool per_memcg)
{
	bool backoff = crystal_hybridswap_auto_result_needs_backoff(ret,
		written_pages);

	atomic64_set(&chs.stats.last_auto_writeback_ret, ret);
	atomic64_set(&chs.stats.last_auto_writeback_written_pages, written_pages);

	mutex_lock(&chs.state_lock);
	chs.auto_last_writeback_result = ret;
	if (backoff) {
		chs.auto_last_empty_jiffies = jiffies;
		if (chs.auto_empty_skip_jiffies)
			chs.auto_empty_skip_jiffies = min_t(unsigned long,
				chs.auto_empty_skip_jiffies << 1,
				msecs_to_jiffies(
				CHS_AUTO_POLICY_EMPTY_BACKOFF_MAX_MS));
		else
			chs.auto_empty_skip_jiffies = msecs_to_jiffies(
				CHS_AUTO_POLICY_EMPTY_BACKOFF_BASE_MS);
		atomic64_inc(&chs.stats.policy_empty_rounds);
		atomic64_set(&chs.stats.policy_empty_backoff_interval_ms,
			     jiffies_to_msecs(chs.auto_empty_skip_jiffies));
	} else {
		chs.auto_empty_skip_jiffies = 0;
		atomic64_set(&chs.stats.policy_empty_backoff_interval_ms, 0);
		if (ret >= 0) {
			chs.auto_policy_window_written_pages += written_pages;
			atomic64_set(&chs.stats.policy_window_written_pages,
				     crystal_hybridswap_clamp_u64_to_s64(
				     chs.auto_policy_window_written_pages));
		}
	}
	mutex_unlock(&chs.state_lock);

	if (!backoff)
		crystal_hybridswap_auto_reset_failure_state();

	if (!per_memcg)
		return;
	if (ret == -ENODATA || ret == -ENODEV || ret == -ENXIO ||
	    (ret >= 0 && written_pages <= 0))
		atomic64_inc(&chs.stats.auto_per_memcg_no_data);
	else if (ret < 0)
		atomic64_inc(&chs.stats.auto_per_memcg_error);
	else
		atomic64_inc(&chs.stats.auto_per_memcg_success);
}

static unsigned long crystal_hybridswap_empty_backoff_left(unsigned long now)
{
	unsigned long last_empty;
	unsigned long skip;
	unsigned long left = 0;

	mutex_lock(&chs.state_lock);
	last_empty = chs.auto_last_empty_jiffies;
	skip = chs.auto_empty_skip_jiffies;
	if (last_empty && skip && time_before(now, last_empty + skip))
		left = last_empty + skip - now;
	mutex_unlock(&chs.state_lock);

	return left;
}

static bool crystal_hybridswap_window_throttled(unsigned long now,
		u64 pages_to_write, unsigned long *left)
{
	u64 max_pages = CHS_AUTO_POLICY_WINDOW_MAX_WRITEBACK_MB *
		CHS_PAGES_PER_MB;
	bool throttled = false;

	mutex_lock(&chs.state_lock);
	if (!chs.auto_policy_window_start ||
	    time_after_eq(now, chs.auto_policy_window_start +
			  msecs_to_jiffies(CHS_AUTO_POLICY_WINDOW_MS))) {
		chs.auto_policy_window_start = now;
		chs.auto_policy_window_written_pages = 0;
	}
	if (chs.auto_policy_window_written_pages + pages_to_write > max_pages) {
		throttled = true;
		if (left)
			*left = chs.auto_policy_window_start +
				msecs_to_jiffies(CHS_AUTO_POLICY_WINDOW_MS) - now;
	} else if (left) {
		*left = 0;
	}
	atomic64_set(&chs.stats.policy_window_start_jiffies,
		     chs.auto_policy_window_start);
	atomic64_set(&chs.stats.policy_window_written_pages,
		     crystal_hybridswap_clamp_u64_to_s64(
		     chs.auto_policy_window_written_pages));
	mutex_unlock(&chs.state_lock);

	if (throttled)
		atomic64_inc(&chs.stats.policy_window_throttled);
	return throttled;
}

static noinline_for_stack int
crystal_hybridswap_queue_auto_memcg_writeback(u64 pages_to_write)
{
	struct crystal_hybridswap_auto_memcg_candidate candidates[
		CHS_AUTO_MEMCG_MAX_CANDIDATES];
	int count;
	int queued = 0;
	int first_ret = 0;
	int i;

	count = crystal_hybridswap_memcg_collect_auto_candidates(candidates,
		CHS_AUTO_MEMCG_MAX_CANDIDATES,
		min_t(u64, pages_to_write, S64_MAX));
	atomic64_set(&chs.stats.auto_memcg_candidate_count, count);
	if (count <= 0)
		return 0;

	for (i = 0; i < count; i++) {
		int ret;

		if (candidates[i].budget_pages <= 0)
			continue;
		ret = crystal_hybridswap_queue_force_swapout_internal(
			candidates[i].cgroup_id, candidates[i].name,
			candidates[i].app_score, candidates[i].budget_pages,
			candidates[i].budget_pages, true);
		if (ret) {
			if (!first_ret)
				first_ret = ret;
			atomic64_inc(&chs.stats.auto_per_memcg_error);
			chs_log_ratelimited(CHS_LOG_WARN,
				"auto_policy per_memcg_auto queue failed memcg=%s id=%llu budget_pages=%lld ratio=%u policy=%d ret=%d\n",
				candidates[i].name, candidates[i].cgroup_id,
				candidates[i].budget_pages,
				candidates[i].zram2ufs_ratio,
				candidates[i].policy_level, ret);
			continue;
		}
		queued++;
		atomic64_inc(&chs.stats.auto_per_memcg_queued);
		chs_log(CHS_LOG_INFO,
			"auto_policy per_memcg_auto queued memcg=%s id=%llu score=%lld budget_pages=%lld ratio=%u policy=%d\n",
			candidates[i].name, candidates[i].cgroup_id,
			candidates[i].app_score, candidates[i].budget_pages,
			candidates[i].zram2ufs_ratio, candidates[i].policy_level);
	}

	return queued ? queued : first_ret;
}

static unsigned long crystal_hybridswap_auto_policy_delay(bool low_memory)
{
	unsigned int msecs = low_memory ? CHS_AUTO_POLICY_LOW_INTERVAL_MS :
		CHS_AUTO_POLICY_NORMAL_INTERVAL_MS;

	return max_t(unsigned long, 1, msecs_to_jiffies(msecs));
}

static bool crystal_hybridswap_auto_policy_configured(void)
{
	return atomic64_read(&chs.stats.avail_buffers_last_avail) > 0 ||
		atomic64_read(&chs.stats.avail_buffers_last_min) > 0 ||
		atomic64_read(&chs.stats.avail_buffers_last_high) > 0 ||
		atomic64_read(&chs.stats.avail_buffers_last_free_swap_threshold) > 0 ||
		(atomic_read(&chs.erm_avail_buffer_enable) &&
		 atomic_read(&chs.erm_avail_buffer_valid));
}

void chs_update_avail_buffer_view(struct chs_avail_buffer_view *view)
{
	bool active = atomic_read(&chs.erm_avail_buffer_enable) &&
		atomic_read(&chs.erm_avail_buffer_valid);
	u64 erm_min;
	u64 erm_high;

	view->effective_min = view->base_min;
	view->effective_high = view->base_high;

	if (active) {
		erm_min = atomic64_read(&chs.erm_min_avail_buffer);
		erm_high = atomic64_read(&chs.erm_high_avail_buffer);
		if (erm_min <= UINT_MAX && erm_high <= UINT_MAX) {
			view->effective_min = erm_min;
			view->effective_high = erm_high;
		} else {
			active = false;
		}
	}

	atomic64_set(&chs.stats.avail_buffers_effective_min,
		     view->effective_min);
	atomic64_set(&chs.stats.avail_buffers_effective_high,
		     view->effective_high);
	view->override_active = active;
}

static bool crystal_hybridswap_policy_suspended(void)
{
	return atomic_read(&chs.policy_suspended) > 0;
}

static bool crystal_hybridswap_has_zram_writeback(void)
{
	struct crystal_hybridswap_zram *entry;
	bool has_zram = false;

	mutex_lock(&chs.zram_lock);
	list_for_each_entry(entry, &chs.zram_list, node) {
		if (entry->dev && entry->zram && entry->writeback) {
			has_zram = true;
			break;
		}
	}
	mutex_unlock(&chs.zram_lock);

	return has_zram;
}

static bool crystal_hybridswap_auto_policy_active(void)
{
	return chs.wq && !crystal_hybridswap_policy_suspended() &&
		crystal_hybridswap_enabled() &&
		crystal_hybridswap_core_enabled() &&
		!crystal_hybridswap_swapd_paused() &&
		crystal_hybridswap_auto_policy_configured() &&
		crystal_hybridswap_has_zram_writeback();
}

void crystal_hybridswap_update_auto_policy(void)
{
	if (!chs.wq)
		return;

	if (!crystal_hybridswap_auto_policy_active()) {
		atomic64_set(&chs.pending_policy_wakeups, 0);
		cancel_delayed_work(&chs.policy_work);
		chs_log_ratelimited(CHS_LOG_DEBUG,
				    "auto_policy inactive enabled=%d core=%d pause=%d configured=%d has_zram=%d suspended=%d\n",
				    crystal_hybridswap_enabled(),
				    crystal_hybridswap_core_enabled(),
				    crystal_hybridswap_swapd_paused(),
				    crystal_hybridswap_auto_policy_configured(),
				    crystal_hybridswap_has_zram_writeback(),
				    crystal_hybridswap_policy_suspended());
		return;
	}

	chs_log_ratelimited(CHS_LOG_DEBUG, "auto_policy queued\n");
	mod_delayed_work(chs.wq, &chs.policy_work, 0);
}

void crystal_hybridswap_queue_policy_wakeup(unsigned int avail,
					   unsigned int min_avail,
					   unsigned int high_avail,
					   u64 free_swap_threshold)
{
	atomic64_set(&chs.stats.avail_buffers_last_avail, avail);
	atomic64_set(&chs.stats.avail_buffers_last_min, min_avail);
	atomic64_set(&chs.stats.avail_buffers_last_high, high_avail);
	atomic64_set(&chs.stats.avail_buffers_last_free_swap_threshold,
		     crystal_hybridswap_clamp_u64_to_s64(free_swap_threshold));
	atomic64_inc(&chs.stats.policy_wakeups);
	atomic64_inc(&chs.pending_policy_wakeups);

	if (crystal_hybridswap_auto_policy_active()) {
		mod_delayed_work(chs.wq, &chs.policy_work, 0);
		atomic64_inc(&chs.stats.avail_buffers_wakeups);
		chs_log_ratelimited(CHS_LOG_DEBUG,
				    "auto_policy wakeup avail_mb=%u min_mb=%u high_mb=%u free_swap_threshold_mb=%llu\n",
				    avail, min_avail, high_avail,
				    free_swap_threshold);
	} else {
		chs_log_ratelimited(CHS_LOG_DEBUG,
				    "auto_policy wakeup skipped inactive avail_mb=%u min_mb=%u high_mb=%u free_swap_threshold_mb=%llu\n",
				    avail, min_avail, high_avail,
				    free_swap_threshold);
		crystal_hybridswap_update_auto_policy();
	}
}

static void crystal_hybridswap_policy_workfn(struct work_struct *work)
{
	struct crystal_hybridswap_zram_pressure *zram_pressure;
	unsigned int avail;
	unsigned int min_avail;
	unsigned int high_avail;
	unsigned int base_min_avail;
	unsigned int base_high_avail;
	struct chs_avail_buffer_view avail_buffer_view;
	unsigned int cur_avail;
	s64 free_swap_threshold;
	u64 free_swap_pages;
	u64 free_swap_mb;
	u64 pages_to_write = 0;
	long swap_pages;
	unsigned long now = jiffies;
	unsigned long next_delay;
	unsigned long min_interval;
	unsigned long last_writeback;
	unsigned long backoff_left;
	unsigned long window_left = 0;
	bool low_buffer;
	bool below_high;
	bool free_swap_low;
	bool zram_gate;
	bool erm_override;
	const char *reason = "avail_buffers_ok";
	int ret;

	atomic64_xchg(&chs.pending_policy_wakeups, 0);
	atomic64_inc(&chs.stats.policy_worker_runs);
	atomic64_inc(&chs.stats.auto_policy_runs);
	atomic64_set(&chs.stats.last_auto_jiffies, now);

	zram_pressure = kzalloc(sizeof(*zram_pressure), GFP_KERNEL);
	if (!zram_pressure) {
		atomic64_inc(&chs.stats.policy_writeback_skipped);
		atomic64_inc(&chs.stats.auto_writeback_skipped);
		reason = "auto_nomem";
		goto out_stop;
	}

	if (!crystal_hybridswap_auto_policy_configured()) {
		atomic64_inc(&chs.stats.policy_writeback_skipped);
		atomic64_inc(&chs.stats.auto_writeback_skipped);
		reason = "auto_unconfigured";
		goto out_stop;
	}

	avail = atomic64_read(&chs.stats.avail_buffers_last_avail);
	base_min_avail = atomic64_read(&chs.stats.avail_buffers_last_min);
	base_high_avail = atomic64_read(&chs.stats.avail_buffers_last_high);
	avail_buffer_view.base_min = base_min_avail;
	avail_buffer_view.base_high = base_high_avail;
	chs_update_avail_buffer_view(&avail_buffer_view);
	min_avail = avail_buffer_view.effective_min;
	high_avail = avail_buffer_view.effective_high;
	erm_override = avail_buffer_view.override_active;
	free_swap_threshold = atomic64_read(
		&chs.stats.avail_buffers_last_free_swap_threshold);
	cur_avail = crystal_hybridswap_current_avail_mb();
	swap_pages = get_nr_swap_pages();
	free_swap_pages = swap_pages > 0 ? swap_pages : 0;
	free_swap_mb = crystal_hybridswap_pages_to_mb_u64(free_swap_pages);

	atomic64_set(&chs.stats.avail_buffers_last_seen_avail, cur_avail);
	atomic64_set(&chs.stats.avail_buffers_last_free_swap_pages,
		     crystal_hybridswap_clamp_u64_to_s64(free_swap_pages));

	low_buffer = (min_avail && cur_avail < min_avail) ||
		     (!min_avail && avail && cur_avail < avail);
	below_high = high_avail && cur_avail < high_avail;
	free_swap_low = free_swap_threshold > 0 &&
			free_swap_pages <
			crystal_hybridswap_mb_to_pages_u64(free_swap_threshold);

	if (low_buffer)
		atomic64_inc(&chs.stats.avail_buffers_low_events);
	if (below_high)
		atomic64_inc(&chs.stats.avail_buffers_high_events);
	if (free_swap_low)
		atomic64_inc(&chs.stats.avail_buffers_swap_low_events);

	if (!crystal_hybridswap_enabled() ||
	    !crystal_hybridswap_core_enabled() ||
	    crystal_hybridswap_swapd_paused()) {
		atomic64_inc(&chs.stats.policy_writeback_skipped);
		atomic64_inc(&chs.stats.auto_writeback_skipped);
		reason = "policy_paused";
		crystal_hybridswap_report_pressure(CHS_PRESSURE_MEDIUM, reason);
		goto out_stop;
	}

	if (!crystal_hybridswap_has_zram_writeback()) {
		atomic64_inc(&chs.stats.policy_writeback_skipped);
		atomic64_inc(&chs.stats.auto_writeback_skipped);
		reason = "writeback_nodev";
		crystal_hybridswap_report_pressure(CHS_PRESSURE_CRITICAL,
						   reason);
		goto out_stop;
	}

	ret = crystal_hybridswap_get_zram_pressure(zram_pressure);
	if (ret) {
		memset(zram_pressure, 0, sizeof(*zram_pressure));
		atomic64_set(&chs.stats.policy_zram_gate_result, 1);
		zram_gate = true;
	} else {
		zram_gate = crystal_hybridswap_zram_gate(zram_pressure,
						       free_swap_low);
	}

	chs_log_ratelimited(CHS_LOG_DEBUG,
		"auto_policy run avail_cfg_mb=%u base_min_mb=%u base_high_mb=%u min_mb=%u high_mb=%u erm_override=%d current_mb=%u free_swap_pages=%llu free_swap_mb=%llu free_swap_threshold_mb=%lld low=%d below_high=%d free_swap_low=%d zram_valid=%d zram_ratio=%u zram_effective=%lld zram_wm=%lld resident=%llu total=%llu increase=%llu gate=%d\n",
		avail, base_min_avail, base_high_avail, min_avail, high_avail,
		erm_override, cur_avail, free_swap_pages, free_swap_mb,
		free_swap_threshold, low_buffer, below_high, free_swap_low,
		zram_pressure->valid, zram_pressure->resident_ratio,
		atomic64_read(&chs.stats.policy_zram_effective_ratio),
		atomic64_read(&chs.stats.policy_zram_wm_ratio),
		zram_pressure->resident_pages, zram_pressure->total_pages,
		zram_pressure->increase_pages, zram_gate);

	if (!low_buffer && !below_high && !free_swap_low) {
		atomic64_inc(&chs.stats.policy_writeback_skipped);
		atomic64_inc(&chs.stats.auto_writeback_skipped);
		crystal_hybridswap_report_pressure(CHS_PRESSURE_LOW, reason);
		next_delay = crystal_hybridswap_auto_policy_delay(false);
		goto out_reschedule;
	}

	if (!zram_gate) {
		atomic64_inc(&chs.stats.policy_writeback_skipped);
		atomic64_inc(&chs.stats.auto_writeback_skipped);
		reason = "zram_pressure_low";
		crystal_hybridswap_report_pressure(CHS_PRESSURE_LOW, reason);
		next_delay = crystal_hybridswap_auto_policy_delay(false);
		chs_log_ratelimited(CHS_LOG_INFO,
			"auto_policy skip reason=%s current_mb=%u low=%d below_high=%d free_swap_low=%d free_swap_pages=%llu free_swap_mb=%llu zram_ratio=%u resident=%llu total=%llu\n",
			reason, cur_avail, low_buffer, below_high, free_swap_low,
			free_swap_pages, free_swap_mb, zram_pressure->resident_ratio,
			zram_pressure->resident_pages, zram_pressure->total_pages);
		goto out_reschedule;
	}

	backoff_left = crystal_hybridswap_empty_backoff_left(now);
	if (backoff_left) {
		atomic64_inc(&chs.stats.policy_writeback_skipped);
		atomic64_inc(&chs.stats.auto_writeback_skipped);
		atomic64_inc(&chs.stats.policy_empty_backoff_skipped);
		reason = "empty_backoff";
		crystal_hybridswap_report_pressure(CHS_PRESSURE_LOW, reason);
		next_delay = min(backoff_left,
				 crystal_hybridswap_auto_policy_delay(true));
		if (!next_delay)
			next_delay = 1;
		chs_log_ratelimited(CHS_LOG_INFO,
			"auto_policy backoff reason=%s left=%u ms empty_rounds=%lld\n",
			reason, jiffies_to_msecs(backoff_left),
			atomic64_read(&chs.stats.policy_empty_rounds));
		goto out_reschedule;
	}

	min_interval = max_t(unsigned long, 1,
		msecs_to_jiffies(CHS_AUTO_POLICY_MIN_WRITEBACK_INTERVAL_MS));
	last_writeback = atomic64_read(&chs.stats.last_auto_writeback_jiffies);
	if (last_writeback && time_before(now, last_writeback + min_interval)) {
		atomic64_inc(&chs.stats.policy_writeback_skipped);
		atomic64_inc(&chs.stats.auto_writeback_skipped);
		reason = "auto_throttled";
		crystal_hybridswap_report_pressure(CHS_PRESSURE_MEDIUM, reason);
		next_delay = last_writeback + min_interval - now;
		next_delay = min(next_delay,
				 crystal_hybridswap_auto_policy_delay(true));
		if (!next_delay)
			next_delay = 1;
		goto out_reschedule;
	}

	pages_to_write = crystal_hybridswap_auto_target_pages(avail,
		min_avail, high_avail, cur_avail, free_swap_low, zram_pressure);
	pages_to_write = crystal_hybridswap_apply_dev_life_auto_budget(
		pages_to_write);
	pages_to_write = min_t(u64, pages_to_write,
				       CHS_POLICY_WAKE_MAX_WRITEBACK_MB *
				       CHS_PAGES_PER_MB);
	pages_to_write = min_t(u64, pages_to_write, S64_MAX);
	if (!pages_to_write)
		pages_to_write = 1;

	pages_to_write = crystal_hybridswap_limit_writeback_pages(
		pages_to_write, true, false, "auto_policy");
	if (!pages_to_write) {
		atomic64_inc(&chs.stats.policy_writeback_skipped);
		atomic64_inc(&chs.stats.auto_writeback_skipped);
		reason = "daily_quota_exhausted";
		crystal_hybridswap_report_pressure(CHS_PRESSURE_MEDIUM, reason);
		next_delay = crystal_hybridswap_auto_policy_delay(true);
		goto out_reschedule;
	}

	if (crystal_hybridswap_window_throttled(now, pages_to_write,
						 &window_left)) {
		atomic64_inc(&chs.stats.policy_writeback_skipped);
		atomic64_inc(&chs.stats.auto_writeback_skipped);
		reason = "window_throttled";
		crystal_hybridswap_report_pressure(CHS_PRESSURE_MEDIUM, reason);
		next_delay = window_left ? window_left :
			crystal_hybridswap_auto_policy_delay(true);
		chs_log_ratelimited(CHS_LOG_INFO,
			"auto_policy throttle reason=%s pages=%llu window_written=%lld window_left_ms=%u\n",
			reason, pages_to_write,
			atomic64_read(&chs.stats.policy_window_written_pages),
			jiffies_to_msecs(next_delay));
		goto out_reschedule;
	}

	ret = crystal_hybridswap_queue_auto_memcg_writeback(pages_to_write);
	if (ret > 0) {
		atomic64_add(ret, &chs.stats.policy_writeback_queued);
		atomic64_add(ret, &chs.stats.auto_writeback_queued);
		atomic64_set(&chs.stats.last_auto_writeback_jiffies, now);
		reason = free_swap_low ? "per_memcg_auto_swap_low" :
			(low_buffer ? "per_memcg_auto_low" :
			 "per_memcg_auto_high");
		crystal_hybridswap_report_pressure(free_swap_low ?
			CHS_PRESSURE_CRITICAL : CHS_PRESSURE_MEDIUM, reason);
		atomic64_inc(&chs.stats.avail_buffers_wakeups);
		chs_log(CHS_LOG_INFO,
			"auto_policy queued per_memcg_auto count=%d pages=%llu reason=%s low=%d below_high=%d free_swap_low=%d free_swap_pages=%llu free_swap_mb=%llu zram_ratio=%u effective_ratio=%lld wm_ratio=%lld increase=%llu dev_life=%u\n",
			ret, pages_to_write, reason, low_buffer, below_high,
			free_swap_low, free_swap_pages, free_swap_mb,
			zram_pressure->resident_ratio,
			atomic64_read(&chs.stats.policy_zram_effective_ratio),
			atomic64_read(&chs.stats.policy_zram_wm_ratio),
			zram_pressure->increase_pages,
			crystal_hybridswap_dev_life_level());
		next_delay = crystal_hybridswap_auto_policy_delay(true);
		goto out_reschedule;
	}
	if (ret < 0) {
		chs_log_ratelimited(CHS_LOG_WARN,
			"auto_policy per_memcg_auto all queues failed pages=%llu ret=%d fallback=global\n",
			pages_to_write, ret);
	}

	atomic64_inc(&chs.stats.auto_global_fallback);
	ret = crystal_hybridswap_queue_writeback_internal(NULL,
		CHS_INTERNAL_WB_MODE, (s64)pages_to_write, true);
	if (ret) {
		atomic64_inc(&chs.stats.policy_writeback_skipped);
		atomic64_inc(&chs.stats.auto_writeback_skipped);
		reason = crystal_hybridswap_auto_failure_reason(ret);
		crystal_hybridswap_report_pressure(
			ret == -ENOSPC ? CHS_PRESSURE_CRITICAL : CHS_PRESSURE_MEDIUM,
			reason);
		backoff_left = crystal_hybridswap_auto_result_needs_backoff(ret, 0) ?
			crystal_hybridswap_empty_backoff_left(jiffies) : 0;
		next_delay = backoff_left ? backoff_left :
			crystal_hybridswap_auto_policy_delay(true);
		goto out_reschedule;
	}

	atomic64_inc(&chs.stats.policy_writeback_queued);
	atomic64_inc(&chs.stats.auto_writeback_queued);
	atomic64_set(&chs.stats.last_auto_writeback_jiffies, now);
	if (free_swap_low) {
		reason = "global_fallback_swap_low";
		crystal_hybridswap_report_pressure(CHS_PRESSURE_CRITICAL,
						   reason);
	} else if (low_buffer) {
		reason = "global_fallback_low";
		crystal_hybridswap_report_pressure(CHS_PRESSURE_MEDIUM, reason);
	} else {
		reason = "global_fallback_high";
		crystal_hybridswap_report_pressure(CHS_PRESSURE_MEDIUM, reason);
	}
	atomic64_inc(&chs.stats.avail_buffers_wakeups);
	chs_log(CHS_LOG_INFO,
		"auto_policy queued global_fallback pages=%llu reason=%s low=%d below_high=%d free_swap_low=%d free_swap_pages=%llu free_swap_mb=%llu zram_ratio=%u effective_ratio=%lld wm_ratio=%lld increase=%llu dev_life=%u\n",
		pages_to_write, reason, low_buffer, below_high, free_swap_low,
		free_swap_pages, free_swap_mb, zram_pressure->resident_ratio,
		atomic64_read(&chs.stats.policy_zram_effective_ratio),
		atomic64_read(&chs.stats.policy_zram_wm_ratio),
		zram_pressure->increase_pages,
		crystal_hybridswap_dev_life_level());
	next_delay = crystal_hybridswap_auto_policy_delay(true);

out_reschedule:
	crystal_hybridswap_record_auto_reason(reason);
	chs_log_ratelimited(CHS_LOG_DEBUG,
			    "auto_policy reschedule reason=%s next_delay=%lu\n",
			    reason, next_delay);
	if (crystal_hybridswap_auto_policy_active())
		mod_delayed_work(chs.wq, &chs.policy_work, next_delay);
	kfree(zram_pressure);
	return;

out_stop:
	crystal_hybridswap_record_auto_reason(reason);
	chs_log_ratelimited(CHS_LOG_DEBUG,
			    "auto_policy stopped reason=%s\n", reason);
	kfree(zram_pressure);
}

static void crystal_hybridswap_force_swapout_workfn(struct work_struct *work)
{
	struct crystal_hybridswap_force_swapout_req *req;
	struct crystal_hybridswap_zram_target *targets = NULL;
	struct crystal_hybridswap_writeback_stats wb_stats;
	s64 remaining;
	int count;
	int first_ret = 0;
	int ret = -ENODEV;
	int traversed = 0;
	int eligible_devices = 0;
	s64 selected = -1;
	int i;

	req = container_of(work, struct crystal_hybridswap_force_swapout_req,
			   work);
	memset(&wb_stats, 0, sizeof(wb_stats));
	atomic64_inc(&chs.stats.force_swapout_scheduled);

	targets = kcalloc(CHS_MAX_ZRAM_TARGETS, sizeof(*targets), GFP_KERNEL);
	if (!targets) {
		ret = -ENOMEM;
		atomic64_inc(&chs.stats.force_swapout_skipped);
		crystal_hybridswap_record_force_swapout_result(req, &wb_stats,
			ret);
		chs_log_ratelimited(CHS_LOG_ERR,
			"force_swapout skipped no memory target_memcg=%s target_cgroup_id=%llu compat_trigger_value=%lld ret=%d\n",
			req->memcg_name, req->target_cgroup_id,
			req->compat_trigger_value, ret);
		crystal_hybridswap_report_pressure(CHS_PRESSURE_CRITICAL,
			"force_swapout_nomem");
		goto out;
	}

	count = crystal_hybridswap_collect_zram_targets(targets,
		CHS_MAX_ZRAM_TARGETS);
	if (!count) {
		atomic64_inc(&chs.stats.force_swapout_skipped);
		crystal_hybridswap_record_force_swapout_result(req, &wb_stats,
			-ENODEV);
		chs_log(CHS_LOG_ERR,
			"force_swapout skipped no device target_memcg=%s target_cgroup_id=%llu compat_trigger_value=%lld scan_scope=zram_resident_pages ret=%d\n",
			req->memcg_name, req->target_cgroup_id,
			req->compat_trigger_value, -ENODEV);
		crystal_hybridswap_report_pressure(CHS_PRESSURE_CRITICAL,
			"force_swapout_nodev");
		goto out_targets;
	}

	req->target_pages = crystal_hybridswap_limit_writeback_pages(
		req->target_pages > 0 ? req->target_pages :
		CHS_FORCE_GLOBAL_SCAN_PAGES, req->auto_req, !req->auto_req,
		req->auto_req ? "per_memcg_auto" : "force_swapout");
	if (req->target_pages <= 0) {
		ret = -EDQUOT;
		atomic64_inc(&chs.stats.force_swapout_skipped);
		crystal_hybridswap_record_force_swapout_result(req, &wb_stats, ret);
		chs_log_ratelimited(CHS_LOG_INFO,
			"force_swapout skipped daily quota target_memcg=%s target_cgroup_id=%llu target_mode=%s compat_trigger_value=%lld\n",
			req->memcg_name, req->target_cgroup_id,
			req->auto_req ? "per_memcg_auto" : "per_memcg_best_effort",
			req->compat_trigger_value);
		crystal_hybridswap_report_pressure(CHS_PRESSURE_MEDIUM,
			req->auto_req ? "per_memcg_auto_quota" :
			"force_swapout_quota");
		if (req->auto_req)
			crystal_hybridswap_auto_record_result(ret, 0, true);
		goto out_targets;
	}

	remaining = req->target_pages;
	chs_log(CHS_LOG_INFO,
		"force_swapout start target_memcg=%s target_cgroup_id=%llu target_mode=%s compat_trigger_value=%lld target_pages=%lld scan_scope=zram_resident_pages devices=%d quota_remaining_pages=%lld dev_life=%u\n",
		req->memcg_name, req->target_cgroup_id,
		req->auto_req ? "per_memcg_auto" : "per_memcg_best_effort",
		req->compat_trigger_value, req->target_pages, count,
		atomic64_read(&chs.stats.writeback_quota_remaining_pages),
		crystal_hybridswap_dev_life_level());

	for (i = 0; i < count && remaining > 0; i++) {
		struct crystal_hybridswap_memcg_zram_stats memcg_stats;
		struct crystal_hybridswap_writeback_stats cur_stats;
		u64 budget;
		unsigned long dev_pages;
		int err;

		traversed++;
		if (!targets[i].force_writeback && !targets[i].force_writeback_ext)
			continue;
		if (!targets[i].pressure.valid || !targets[i].pressure.backing_dev) {
			atomic64_inc(&chs.stats.multi_zram_skip_no_backing);
			continue;
		}
		if (targets[i].pressure.wb_limit_exhausted) {
			atomic64_inc(&chs.stats.multi_zram_skip_limit);
			continue;
		}

		budget = remaining;
		if (req->target_cgroup_id) {
			err = crystal_hybridswap_zram_memcg_stats(targets[i].dev,
				req->target_cgroup_id, &memcg_stats);
			if (err || !memcg_stats.resident_pages) {
				atomic64_inc(&chs.stats.multi_zram_skip_no_resident);
				continue;
			}
			budget = min_t(u64, budget, memcg_stats.resident_pages);
		} else {
			budget = min_t(u64, budget,
				targets[i].pressure.resident_pages);
		}
		if (!budget) {
			atomic64_inc(&chs.stats.multi_zram_skip_no_resident);
			continue;
		}
		dev_pages = budget > ULONG_MAX ? ULONG_MAX : (unsigned long)budget;
		eligible_devices++;
		if (selected < 0)
			selected = crystal_hybridswap_zram_target_id(&targets[i]);

		memset(&cur_stats, 0, sizeof(cur_stats));
		if (targets[i].force_writeback_ext) {
			err = targets[i].force_writeback_ext(targets[i].dev,
				CHS_INTERNAL_WB_MODE, dev_pages,
				req->target_cgroup_id, req->auto_req,
				&cur_stats);
		} else {
			err = targets[i].force_writeback(targets[i].dev,
				CHS_INTERNAL_WB_MODE, dev_pages,
				req->target_cgroup_id, &cur_stats);
		}
		crystal_hybridswap_add_writeback_stats(&wb_stats, &cur_stats);
		if (err < 0 && err != -ENODATA && !first_ret)
			first_ret = err;
		if (cur_stats.written_pages) {
			if (remaining > cur_stats.written_pages)
				remaining -= cur_stats.written_pages;
			else
				remaining = 0;
		}
	}

	atomic64_set(&chs.stats.multi_zram_last_traversed, traversed);
	atomic64_set(&chs.stats.multi_zram_last_eligible, eligible_devices);
	atomic64_set(&chs.stats.multi_zram_last_selected, selected);
	atomic64_set(&chs.stats.multi_zram_force_swapout_devices,
		eligible_devices);

	ret = wb_stats.written_pages ?
		(wb_stats.written_pages > INT_MAX ? INT_MAX :
		 (int)wb_stats.written_pages) :
		(first_ret ? first_ret : -ENODATA);
	crystal_hybridswap_record_force_swapout_result(req, &wb_stats, ret);
	if (req->auto_req)
		crystal_hybridswap_auto_record_result(ret,
			wb_stats.written_pages, true);

	if (ret == -ENODATA) {
		atomic64_inc(&chs.stats.force_swapout_skipped);
		atomic64_inc(&chs.stats.force_swapout_no_data);
		crystal_hybridswap_report_pressure(CHS_PRESSURE_LOW,
			req->auto_req ? "per_memcg_auto_no_data" :
			"force_swapout_no_data");
	} else if (ret < 0) {
		atomic64_inc(&chs.stats.force_swapout_error);
		crystal_hybridswap_report_pressure(CHS_PRESSURE_CRITICAL,
			req->auto_req ? "per_memcg_auto_error" :
			"force_swapout_error");
	} else {
		atomic64_inc(&chs.stats.force_swapout_success);
		crystal_hybridswap_report_pressure(CHS_PRESSURE_LOW,
			req->auto_req ? "per_memcg_auto_success" :
			"force_swapout_success");
	}

	chs_log(CHS_LOG_INFO,
		"force_swapout done target_memcg=%s target_cgroup_id=%llu target_mode=%s compat_trigger_value=%lld target_pages=%lld scan_scope=zram_resident_pages devices=%d eligible_devices=%d selected=%lld scanned_pages=%lu eligible_pages=%lu written_pages=%lu effective_pages=%lu unknown_or_filtered=%lu ret=%d quota_used_pages=%lld quota_remaining_pages=%lld\n",
		req->memcg_name, req->target_cgroup_id,
		req->auto_req ? "per_memcg_auto" : "per_memcg_best_effort",
		req->compat_trigger_value, req->target_pages, traversed,
		eligible_devices, selected, wb_stats.scanned_pages,
		wb_stats.eligible_pages, wb_stats.written_pages,
		wb_stats.written_pages, wb_stats.unknown_or_filtered_pages, ret,
		atomic64_read(&chs.stats.writeback_quota_used_pages),
		atomic64_read(&chs.stats.writeback_quota_remaining_pages));

out_targets:
	if (targets)
		crystal_hybridswap_put_zram_targets(targets, count);
	kfree(targets);
out:
	atomic64_dec(&chs.pending_force_swapout);
	crystal_hybridswap_force_swapout_drain_done();
	kfree(req);
}

static void crystal_hybridswap_writeback_workfn(struct work_struct *work)
{
	struct device *dev;
	struct zram *zram;
	crystal_hybridswap_zram_writeback_t writeback;
	crystal_hybridswap_zram_writeback_ext_t writeback_ext;
	char mode[CHS_WB_MODE_MAX];
	char detail[160];
	s64 pages;
	bool force;
	bool auto_req;
	u64 start_ns;
	struct crystal_hybridswap_writeback_stats wb_stats;
	s64 written_pages = 0;
	int ret = 0;

	if (crystal_hybridswap_take_pending_writeback(&dev, &zram, &writeback,
						      &writeback_ext, mode,
						      sizeof(mode), &pages, &force,
						      &auto_req))
		return;

	start_ns = ktime_get_ns();
	atomic64_inc(&chs.stats.writeback_worker_runs);

	if (!force && (!crystal_hybridswap_enabled() ||
		       !crystal_hybridswap_core_enabled() ||
	    crystal_hybridswap_swapd_paused())) {
		ret = -EBUSY;
		atomic64_inc(&chs.stats.writeback_skipped);
		if (auto_req)
			crystal_hybridswap_auto_record_result(ret, 0, false);
		crystal_hybridswap_record_writeback(mode, pages, ret, 0);
		chs_log_ratelimited(CHS_LOG_INFO,
				    "writeback skipped mode=%s pages=%lld force=%d enabled=%d core=%d pause=%d\n",
				    mode, pages, force, crystal_hybridswap_enabled(),
				    crystal_hybridswap_core_enabled(),
				    crystal_hybridswap_swapd_paused());
		crystal_hybridswap_report_pressure(CHS_PRESSURE_MEDIUM,
						   "writeback_skip");
		goto out_put;
	}

	if (!dev || (!writeback && !writeback_ext)) {
		ret = -ENODEV;
		atomic64_inc(&chs.stats.writeback_skipped);
		if (auto_req)
			crystal_hybridswap_auto_record_result(ret, 0, false);
		crystal_hybridswap_record_writeback(mode, pages, ret, 0);
		if (auto_req) {
			scnprintf(detail, sizeof(detail),
				"mode=%s pages=%lld force=%d", mode, pages, force);
			crystal_hybridswap_auto_log_failure(CHS_LOG_INFO,
				crystal_hybridswap_auto_failure_reason(ret), ret,
				detail);
		} else {
			chs_log(CHS_LOG_ERR,
				"writeback skipped no device mode=%s pages=%lld force=%d\n",
				mode, pages, force);
		}
		crystal_hybridswap_report_pressure(CHS_PRESSURE_CRITICAL,
					   "writeback_nodev");
		goto out_put;
	}

	pages = crystal_hybridswap_limit_writeback_pages(pages, auto_req, force,
		"writeback");
	if (pages <= 0) {
		ret = -EDQUOT;
		atomic64_inc(&chs.stats.writeback_skipped);
		if (auto_req)
			crystal_hybridswap_auto_record_result(ret, 0, false);
		crystal_hybridswap_record_writeback(mode, 0, ret, 0);
		if (auto_req) {
			scnprintf(detail, sizeof(detail),
				"dev=%s mode=%s force=%d quota_remaining_pages=%lld",
				dev_name(dev), mode, force,
				atomic64_read(&chs.stats.writeback_quota_remaining_pages));
			crystal_hybridswap_auto_log_failure(CHS_LOG_INFO,
				crystal_hybridswap_auto_failure_reason(ret), ret,
				detail);
		} else {
			chs_log_ratelimited(CHS_LOG_INFO,
				"writeback skipped daily quota dev=%s mode=%s force=%d auto=%d\n",
				dev_name(dev), mode, force, auto_req);
		}
		crystal_hybridswap_report_pressure(CHS_PRESSURE_MEDIUM,
					   "writeback_quota");
		goto out_put;
	}

	atomic64_inc(&chs.stats.writeback_scheduled);
	chs_log_ratelimited(CHS_LOG_DEBUG,
			    "writeback start dev=%s mode=%s pages=%lld force=%d quota_remaining_pages=%lld\n",
			    dev_name(dev), mode, pages, force,
			    atomic64_read(&chs.stats.writeback_quota_remaining_pages));
	memset(&wb_stats, 0, sizeof(wb_stats));
	if (writeback_ext)
		ret = writeback_ext(dev, mode, pages > 0 ? pages : 1,
				    auto_req, &wb_stats);
	else
		ret = writeback(dev, mode, pages > 0 ? pages : 1, &wb_stats);
	written_pages = wb_stats.written_pages;
	if (!written_pages && ret > 0)
		written_pages = ret;
	crystal_hybridswap_record_writeback(mode, pages, ret, written_pages);
	if (auto_req && ret)
		crystal_hybridswap_auto_record_result(ret, written_pages, false);
	if (ret == -ENODATA) {
		atomic64_inc(&chs.stats.writeback_skipped);
		atomic64_inc(&chs.stats.writeback_no_data);
		if (auto_req) {
			scnprintf(detail, sizeof(detail),
				"dev=%s mode=%s requested=%lld force=%d scanned=%lu eligible=%lu written=%lld filtered=%lu reason=no_matching_pages",
				dev_name(dev), mode, pages, force,
				wb_stats.scanned_pages, wb_stats.eligible_pages,
				written_pages, wb_stats.unknown_or_filtered_pages);
			crystal_hybridswap_auto_log_failure(CHS_LOG_INFO,
				crystal_hybridswap_auto_failure_reason(ret), ret,
				detail);
		} else {
			chs_log_ratelimited(CHS_LOG_INFO,
				    "writeback no data dev=%s mode=%s requested=%lld force=%d reason=no_matching_pages\n",
				    dev_name(dev), mode, pages, force);
		}
		crystal_hybridswap_report_pressure(CHS_PRESSURE_LOW,
					   "writeback_no_data");
	} else if (ret < 0) {
		atomic64_inc(&chs.stats.writeback_error);
		if (auto_req && crystal_hybridswap_auto_result_needs_backoff(ret,
				written_pages)) {
			scnprintf(detail, sizeof(detail),
				"dev=%s mode=%s pages=%lld written=%lld force=%d scanned=%lu eligible=%lu filtered=%lu",
				dev_name(dev), mode, pages, written_pages, force,
				wb_stats.scanned_pages, wb_stats.eligible_pages,
				wb_stats.unknown_or_filtered_pages);
			crystal_hybridswap_auto_log_failure(CHS_LOG_WARN,
				crystal_hybridswap_auto_failure_reason(ret), ret,
				detail);
		} else {
			chs_log(CHS_LOG_ERR,
				"writeback error dev=%s mode=%s pages=%lld written=%lld ret=%d force=%d\n",
				dev_name(dev), mode, pages, written_pages, ret, force);
		}
		crystal_hybridswap_report_pressure(CHS_PRESSURE_CRITICAL,
					   "writeback_error");
	} else if (ret > 0) {
		atomic64_inc(&chs.stats.writeback_success);
		if (auto_req)
			crystal_hybridswap_auto_record_result(ret, written_pages, false);
		chs_log_ratelimited(CHS_LOG_INFO,
				    "writeback success dev=%s mode=%s requested=%lld written=%d force=%d\n",
				    dev_name(dev), mode, pages, ret, force);
		crystal_hybridswap_report_pressure(CHS_PRESSURE_LOW,
					   "writeback_success");
	} else {
		ret = -ENODATA;
		atomic64_inc(&chs.stats.writeback_skipped);
		atomic64_inc(&chs.stats.writeback_no_data);
		if (auto_req)
			crystal_hybridswap_auto_record_result(ret, 0, false);
		crystal_hybridswap_record_writeback(mode, pages, ret, 0);
		if (auto_req) {
			scnprintf(detail, sizeof(detail),
				"dev=%s mode=%s requested=%lld force=%d scanned=%lu eligible=%lu filtered=%lu reason=zero_written",
				dev_name(dev), mode, pages, force,
				wb_stats.scanned_pages, wb_stats.eligible_pages,
				wb_stats.unknown_or_filtered_pages);
			crystal_hybridswap_auto_log_failure(CHS_LOG_INFO,
				crystal_hybridswap_auto_failure_reason(ret), ret,
				detail);
		} else {
			chs_log_ratelimited(CHS_LOG_INFO,
				    "writeback no data dev=%s mode=%s requested=%lld force=%d reason=zero_written\n",
				    dev_name(dev), mode, pages, force);
		}
		crystal_hybridswap_report_pressure(CHS_PRESSURE_LOW,
					   "writeback_no_data");
	}

out_put:
	crystal_hybridswap_record_worker_latency(
		&chs.stats.writeback_worker_total_ns,
		&chs.stats.writeback_worker_max_ns,
		&chs.stats.writeback_worker_slow_runs,
		&chs.stats.writeback_worker_last_ns,
		ktime_get_ns() - start_ns, "writeback", ret);
	if (zram)
		zram_put(zram);
	if (dev)
		put_device(dev);
}

static void crystal_hybridswap_batchin_workfn(struct work_struct *work)
{
	struct crystal_hybridswap_zram_target *targets = NULL;
	struct crystal_hybridswap_batchin_stats batchin_stats;
	char memcg_name[CHS_MEMCG_NAME_MAX];
	u64 target_cgroup_id = 0;
	s64 app_score = 0;
	s64 pages;
	s64 remaining;
	u64 start_ns;
	bool full_scan;
	int count = 0;
	int first_ret = 0;
	int ret = -ENODEV;
	int traversed = 0;
	int eligible_devices = 0;
	s64 selected = -1;
	int i;

	if (crystal_hybridswap_take_pending_batchin(&pages,
			&target_cgroup_id, memcg_name, sizeof(memcg_name),
			&app_score))
		return;

	start_ns = ktime_get_ns();
	atomic64_inc(&chs.stats.batchin_worker_runs);
	memset(&batchin_stats, 0, sizeof(batchin_stats));
	if (pages <= 0)
		pages = CHS_FORCE_GLOBAL_SCAN_PAGES;
	full_scan = pages >= CHS_FORCE_GLOBAL_SCAN_PAGES;
	remaining = pages;

	if (!target_cgroup_id)
		atomic64_inc(&chs.stats.force_swapin_global_runs);

	targets = kcalloc(CHS_MAX_ZRAM_TARGETS, sizeof(*targets), GFP_KERNEL);
	if (!targets) {
		ret = -ENOMEM;
		atomic64_inc(&chs.stats.force_swapin_skipped);
		chs_log_ratelimited(CHS_LOG_ERR,
			"force_swapin skipped no memory target_memcg=%s target_cgroup_id=%llu effective_pages=%lld scope=%s ret=%d\n",
			memcg_name, target_cgroup_id, pages,
			target_cgroup_id ? "per_memcg_page_level" :
			"global_best_effort", ret);
		crystal_hybridswap_report_pressure(CHS_PRESSURE_CRITICAL,
			"force_swapin_nomem");
		goto out_record;
	}

	count = crystal_hybridswap_collect_zram_targets(targets,
		CHS_MAX_ZRAM_TARGETS);
	if (!count) {
		atomic64_inc(&chs.stats.force_swapin_skipped);
		chs_log_ratelimited(CHS_LOG_ERR,
			"force_swapin skipped no device target_memcg=%s target_cgroup_id=%llu effective_pages=%lld scope=%s ret=%d\n",
			memcg_name, target_cgroup_id, pages,
			target_cgroup_id ? "per_memcg_page_level" :
			"global_best_effort", ret);
		crystal_hybridswap_report_pressure(CHS_PRESSURE_CRITICAL,
			"force_swapin_nodev");
		goto out_targets;
	}

	atomic64_inc(&chs.stats.force_swapin_scheduled);
	chs_log_ratelimited(CHS_LOG_DEBUG,
		"force_swapin start target_memcg=%s target_cgroup_id=%llu app_score=%lld effective_pages=%lld scope=%s devices=%d\n",
		memcg_name, target_cgroup_id, app_score, pages,
		target_cgroup_id ? "per_memcg_page_level" :
		"global_best_effort", count);

	for (i = 0; i < count && (full_scan || remaining > 0); i++) {
		struct crystal_hybridswap_memcg_zram_stats memcg_stats;
		struct crystal_hybridswap_batchin_stats cur_stats;
		u64 limit = full_scan ? U64_MAX : (u64)remaining;
		u64 budget;
		unsigned long dev_pages;
		int err;

		traversed++;
		if (!targets[i].batchin)
			continue;
		if (!targets[i].pressure.valid || !targets[i].pressure.backing_dev) {
			atomic64_inc(&chs.stats.multi_zram_skip_no_backing);
			continue;
		}

		if (target_cgroup_id) {
			err = crystal_hybridswap_zram_memcg_stats(targets[i].dev,
				target_cgroup_id, &memcg_stats);
			if (err || !memcg_stats.writeback_pages) {
				atomic64_inc(&chs.stats.multi_zram_skip_no_resident);
				continue;
			}
			budget = min_t(u64, limit, memcg_stats.writeback_pages);
		} else {
			if (!targets[i].pressure.writeback_pages) {
				atomic64_inc(&chs.stats.multi_zram_skip_no_resident);
				continue;
			}
			budget = min_t(u64, limit, targets[i].pressure.writeback_pages);
		}
		if (!budget) {
			atomic64_inc(&chs.stats.multi_zram_skip_no_resident);
			continue;
		}

		dev_pages = budget > ULONG_MAX ? ULONG_MAX : (unsigned long)budget;
		eligible_devices++;
		if (selected < 0)
			selected = crystal_hybridswap_zram_target_id(&targets[i]);

		memset(&cur_stats, 0, sizeof(cur_stats));
		err = targets[i].batchin(targets[i].dev, dev_pages,
			target_cgroup_id, &cur_stats);
		crystal_hybridswap_add_batchin_stats(&batchin_stats, &cur_stats);
		if (cur_stats.last_error && !first_ret)
			first_ret = cur_stats.last_error;
		if (err < 0 && err != -ENODATA && !first_ret)
			first_ret = err;
		if (!full_scan && cur_stats.moved_pages) {
			if (remaining > cur_stats.moved_pages)
				remaining -= cur_stats.moved_pages;
			else
				remaining = 0;
		}
	}

	ret = batchin_stats.moved_pages ?
		(batchin_stats.moved_pages > INT_MAX ? INT_MAX :
		 (int)batchin_stats.moved_pages) :
		(first_ret ? first_ret : -ENODATA);

out_targets:
	if (targets)
		crystal_hybridswap_put_zram_targets(targets, count);
	kfree(targets);
out_record:
	atomic64_set(&chs.stats.multi_zram_last_traversed, traversed);
	atomic64_set(&chs.stats.multi_zram_last_eligible, eligible_devices);
	atomic64_set(&chs.stats.multi_zram_last_selected, selected);
	if (target_cgroup_id)
		atomic64_set(&chs.stats.multi_zram_force_swapin_devices,
			eligible_devices);
	else
		atomic64_set(&chs.stats.multi_zram_global_batchin_devices,
			eligible_devices);
	atomic64_set(&chs.stats.force_swapin_last_scanned_pages,
		batchin_stats.scanned_pages);
	atomic64_set(&chs.stats.force_swapin_last_matched_pages,
		batchin_stats.matched_pages);
	atomic64_set(&chs.stats.force_swapin_last_skipped_pages,
		batchin_stats.skipped_pages);
	atomic64_set(&chs.stats.force_swapin_last_filtered_pages,
		batchin_stats.filtered_pages);
	atomic64_set(&chs.stats.force_swapin_last_read_errors,
		batchin_stats.read_errors);
	atomic64_set(&chs.stats.force_swapin_last_prepare_errors,
		batchin_stats.prepare_errors);
	atomic64_set(&chs.stats.force_swapin_last_snapshot_mismatch,
		batchin_stats.snapshot_mismatch);
	atomic64_set(&chs.stats.force_swapin_last_devices, eligible_devices);
	crystal_hybridswap_record_batchin(pages, ret,
		batchin_stats.moved_pages);
	if (target_cgroup_id)
		crystal_hybridswap_memcg_record_force_swapin_result(
			target_cgroup_id, batchin_stats.scanned_pages,
			batchin_stats.matched_pages, batchin_stats.moved_pages,
			batchin_stats.skipped_pages, batchin_stats.filtered_pages,
			batchin_stats.read_errors, batchin_stats.prepare_errors,
			batchin_stats.snapshot_mismatch, ret);

	if (ret == -ENODATA) {
		atomic64_inc(&chs.stats.force_swapin_skipped);
		atomic64_inc(&chs.stats.force_swapin_no_data);
		if (target_cgroup_id)
			atomic64_inc(&chs.stats.force_swapin_per_memcg_no_data);
		chs_log_ratelimited(CHS_LOG_INFO,
			"force_swapin no data target_memcg=%s target_cgroup_id=%llu effective_pages=%lld scope=%s devices=%d eligible_devices=%d scanned=%lu matched=%lu filtered=%lu mismatch=%lu reason=no_writeback_pages\n",
			memcg_name, target_cgroup_id, pages,
			target_cgroup_id ? "per_memcg_page_level" :
			"global_best_effort", traversed, eligible_devices,
			batchin_stats.scanned_pages, batchin_stats.matched_pages,
			batchin_stats.filtered_pages,
			batchin_stats.snapshot_mismatch);
		crystal_hybridswap_report_pressure(CHS_PRESSURE_LOW,
			"force_swapin_no_data");
	} else if (ret < 0) {
		atomic64_inc(&chs.stats.force_swapin_error);
		if (target_cgroup_id)
			atomic64_inc(&chs.stats.force_swapin_per_memcg_error);
		chs_log_ratelimited(CHS_LOG_ERR,
			"force_swapin error target_memcg=%s target_cgroup_id=%llu effective_pages=%lld scope=%s devices=%d eligible_devices=%d scanned_pages=%lu matched_pages=%lu moved_pages=%lu filtered_pages=%lu read_errors=%lu prepare_errors=%lu snapshot_mismatch_pages=%lu ret=%d\n",
			memcg_name, target_cgroup_id, pages,
			target_cgroup_id ? "per_memcg_page_level" :
			"global_best_effort", traversed, eligible_devices,
			batchin_stats.scanned_pages, batchin_stats.matched_pages,
			batchin_stats.moved_pages, batchin_stats.filtered_pages,
			batchin_stats.read_errors, batchin_stats.prepare_errors,
			batchin_stats.snapshot_mismatch, ret);
		crystal_hybridswap_report_pressure(CHS_PRESSURE_CRITICAL,
			"force_swapin_error");
	} else {
		atomic64_inc(&chs.stats.force_swapin_success);
		if (batchin_stats.last_error) {
			atomic64_inc(&chs.stats.force_swapin_error);
			if (target_cgroup_id)
				atomic64_inc(&chs.stats.force_swapin_per_memcg_error);
		}
		if (target_cgroup_id)
			atomic64_inc(&chs.stats.force_swapin_per_memcg_success);
		chs_log_ratelimited(batchin_stats.last_error ? CHS_LOG_WARN :
			CHS_LOG_INFO,
			"force_swapin success target_memcg=%s target_cgroup_id=%llu effective_pages=%lld scope=%s devices=%d eligible_devices=%d selected=%lld scanned_pages=%lu matched_pages=%lu moved_pages=%lu skipped_pages=%lu filtered_pages=%lu read_errors=%lu prepare_errors=%lu snapshot_mismatch_pages=%lu ret=%d last_error=%d\n",
			memcg_name, target_cgroup_id, pages,
			target_cgroup_id ? "per_memcg_page_level" :
			"global_best_effort", traversed, eligible_devices,
			selected, batchin_stats.scanned_pages,
			batchin_stats.matched_pages, batchin_stats.moved_pages,
			batchin_stats.skipped_pages, batchin_stats.filtered_pages,
			batchin_stats.read_errors, batchin_stats.prepare_errors,
			batchin_stats.snapshot_mismatch, ret, batchin_stats.last_error);
		crystal_hybridswap_report_pressure(CHS_PRESSURE_LOW,
			"force_swapin_success");
	}

	crystal_hybridswap_record_worker_latency(
		&chs.stats.batchin_worker_total_ns,
		&chs.stats.batchin_worker_max_ns,
		&chs.stats.batchin_worker_slow_runs,
		&chs.stats.batchin_worker_last_ns,
		ktime_get_ns() - start_ns, "batchin", ret);
}

void crystal_hybridswap_stats_init(struct crystal_hybridswap_stats *stats)
{
	atomic64_set(&stats->zram_register, 0);
	atomic64_set(&stats->zram_unregister, 0);
	atomic64_set(&stats->zram_sysfs_errors, 0);
	atomic64_set(&stats->enable_store, 0);
	atomic64_set(&stats->core_enable_store, 0);
	atomic64_set(&stats->swapd_pause_store, 0);
	atomic64_set(&stats->loglevel_store, 0);
	atomic64_set(&stats->loop_device_store, 0);
	atomic64_set(&stats->loop_device_bind_success, 0);
	atomic64_set(&stats->loop_device_bind_error, 0);
	atomic64_set(&stats->loop_device_last_ret, 0);
	atomic64_set(&stats->zram_increase_store, 0);
	atomic64_set(&stats->writeback_quota_limit_bytes, 0);
	atomic64_set(&stats->writeback_quota_effective_limit_bytes, 0);
	atomic64_set(&stats->writeback_quota_used_pages, 0);
	atomic64_set(&stats->writeback_quota_used_bytes, 0);
	atomic64_set(&stats->writeback_quota_remaining_pages, 0);
	atomic64_set(&stats->writeback_quota_remaining_bytes, 0);
	atomic64_set(&stats->writeback_quota_skipped, 0);
	atomic64_set(&stats->writeback_quota_capped, 0);
	atomic64_set(&stats->writeback_quota_resets, 0);
	atomic64_set(&stats->writeback_quota_window_start_jiffies, 0);
	atomic64_set(&stats->dev_life_level, 0);
	atomic64_set(&stats->dev_life_auto_scaled, 0);
	atomic64_set(&stats->dev_life_quota_scaled, 0);
	atomic64_set(&stats->dev_life_force_soft_bypass, 0);
	atomic64_set(&stats->force_swapin, 0);
	atomic64_set(&stats->force_swapin_pages, 0);
	atomic64_set(&stats->force_swapin_queued, 0);
	atomic64_set(&stats->force_swapin_scheduled, 0);
	atomic64_set(&stats->force_swapin_success, 0);
	atomic64_set(&stats->force_swapin_error, 0);
	atomic64_set(&stats->force_swapin_skipped, 0);
	atomic64_set(&stats->force_swapin_no_data, 0);
	atomic64_set(&stats->force_swapin_last_request_pages, 0);
	atomic64_set(&stats->force_swapin_last_effective_pages, 0);
	atomic64_set(&stats->force_swapin_last_batchin_pages, 0);
	atomic64_set(&stats->force_swapin_last_ret, 0);
	atomic64_set(&stats->force_swapin_last_jiffies, 0);
	atomic64_set(&stats->force_swapin_per_memcg_success, 0);
	atomic64_set(&stats->force_swapin_per_memcg_no_data, 0);
	atomic64_set(&stats->force_swapin_per_memcg_error, 0);
	atomic64_set(&stats->force_swapin_global_runs, 0);
	atomic64_set(&stats->force_swapin_last_scanned_pages, 0);
	atomic64_set(&stats->force_swapin_last_matched_pages, 0);
	atomic64_set(&stats->force_swapin_last_skipped_pages, 0);
	atomic64_set(&stats->force_swapin_last_filtered_pages, 0);
	atomic64_set(&stats->force_swapin_last_read_errors, 0);
	atomic64_set(&stats->force_swapin_last_prepare_errors, 0);
	atomic64_set(&stats->force_swapin_last_snapshot_mismatch, 0);
	atomic64_set(&stats->force_swapin_last_devices, 0);
	atomic64_set(&stats->force_swapout, 0);
	atomic64_set(&stats->force_swapout_pages, 0);
	atomic64_set(&stats->force_swapout_effective_pages, 0);
	atomic64_set(&stats->force_swapout_queued, 0);
	atomic64_set(&stats->force_swapout_scheduled, 0);
	atomic64_set(&stats->force_swapout_success, 0);
	atomic64_set(&stats->force_swapout_error, 0);
	atomic64_set(&stats->force_swapout_skipped, 0);
	atomic64_set(&stats->force_swapout_no_data, 0);
	atomic64_set(&stats->force_swapout_last_compat_trigger_value, 0);
	atomic64_set(&stats->force_swapout_last_scanned_pages, 0);
	atomic64_set(&stats->force_swapout_last_eligible_pages, 0);
	atomic64_set(&stats->force_swapout_last_written_pages, 0);
	atomic64_set(&stats->force_swapout_last_unknown_or_filtered_pages, 0);
	atomic64_set(&stats->force_swapout_last_ret, 0);
	atomic64_set(&stats->force_swapout_last_jiffies, 0);
	atomic64_set(&stats->force_shrink_anon, 0);
	atomic64_set(&stats->force_shrink_file, 0);
	atomic64_set(&stats->aging_anon, 0);
	atomic64_set(&stats->force_shrink_anon_last_param, 0);
	atomic64_set(&stats->force_shrink_file_last_param, 0);
	atomic64_set(&stats->aging_anon_last_param, 0);
	atomic64_set(&stats->force_shrink_queued, 0);
	atomic64_set(&stats->force_shrink_worker_runs, 0);
	atomic64_set(&stats->force_shrink_anon_pages, 0);
	atomic64_set(&stats->force_shrink_file_pages, 0);
	atomic64_set(&stats->force_shrink_anon_last_target, 0);
	atomic64_set(&stats->force_shrink_file_last_target, 0);
	atomic64_set(&stats->force_shrink_anon_last_batch, 0);
	atomic64_set(&stats->force_shrink_file_last_batch, 0);
	atomic64_set(&stats->force_shrink_anon_last_reclaimed, 0);
	atomic64_set(&stats->force_shrink_file_last_reclaimed, 0);
	atomic64_set(&stats->force_shrink_anon_last_ret, 0);
	atomic64_set(&stats->force_shrink_file_last_ret, 0);
	atomic64_set(&stats->force_shrink_anon_dropped, 0);
	atomic64_set(&stats->force_shrink_file_dropped, 0);
	atomic64_set(&stats->force_shrink_anon_skipped, 0);
	atomic64_set(&stats->force_shrink_file_skipped, 0);
	atomic64_set(&stats->force_shrink_anon_best_effort, 0);
	atomic64_set(&stats->force_shrink_anon_hook, 0);
	atomic64_set(&stats->writeback_queued, 0);
	atomic64_set(&stats->writeback_scheduled, 0);
	atomic64_set(&stats->writeback_success, 0);
	atomic64_set(&stats->writeback_error, 0);
	atomic64_set(&stats->writeback_skipped, 0);
	atomic64_set(&stats->writeback_no_data, 0);
	atomic64_set(&stats->writeback_noop, 0);
	atomic64_set(&stats->writeback_noop_pages, 0);
	atomic64_set(&stats->writeback_last_ret, 0);
	atomic64_set(&stats->writeback_last_pages, 0);
	atomic64_set(&stats->writeback_last_written_pages, 0);
	atomic64_set(&stats->writeback_last_jiffies, 0);
	atomic64_set(&stats->writeback_worker_runs, 0);
	atomic64_set(&stats->writeback_worker_total_ns, 0);
	atomic64_set(&stats->writeback_worker_max_ns, 0);
	atomic64_set(&stats->writeback_worker_slow_runs, 0);
	atomic64_set(&stats->writeback_worker_last_ns, 0);
	atomic64_set(&stats->batchin_worker_runs, 0);
	atomic64_set(&stats->batchin_worker_total_ns, 0);
	atomic64_set(&stats->batchin_worker_max_ns, 0);
	atomic64_set(&stats->batchin_worker_slow_runs, 0);
	atomic64_set(&stats->batchin_worker_last_ns, 0);
	atomic64_set(&stats->multi_zram_registered, 0);
	atomic64_set(&stats->multi_zram_last_selected, -1);
	atomic64_set(&stats->multi_zram_last_traversed, 0);
	atomic64_set(&stats->multi_zram_last_eligible, 0);
	atomic64_set(&stats->multi_zram_skip_no_backing, 0);
	atomic64_set(&stats->multi_zram_skip_limit, 0);
	atomic64_set(&stats->multi_zram_skip_no_resident, 0);
	atomic64_set(&stats->multi_zram_force_swapout_devices, 0);
	atomic64_set(&stats->multi_zram_force_swapin_devices, 0);
	atomic64_set(&stats->multi_zram_global_batchin_devices, 0);
	atomic64_set(&stats->policy_wakeups, 0);
	atomic64_set(&stats->policy_worker_runs, 0);
	atomic64_set(&stats->policy_writeback_queued, 0);
	atomic64_set(&stats->policy_writeback_skipped, 0);
	atomic64_set(&stats->auto_policy_runs, 0);
	atomic64_set(&stats->auto_writeback_queued, 0);
	atomic64_set(&stats->auto_writeback_skipped, 0);
	atomic64_set(&stats->last_auto_jiffies, 0);
	atomic64_set(&stats->last_auto_writeback_jiffies, 0);
	atomic64_set(&stats->last_auto_writeback_ret, 0);
	atomic64_set(&stats->last_auto_writeback_written_pages, 0);
	atomic64_set(&stats->policy_available_android_pages, 0);
	atomic64_set(&stats->policy_available_android_mb, 0);
	atomic64_set(&stats->policy_available_fallback, 0);
	atomic64_set(&stats->policy_zram_pressure_ratio, 0);
	atomic64_set(&stats->policy_zram_effective_ratio, 0);
	atomic64_set(&stats->policy_zram_wm_ratio, CHS_DEFAULT_ZRAM_WM_RATIO);
	atomic64_set(&stats->policy_zram_gate_result, 0);
	atomic64_set(&stats->policy_zram_resident_pages, 0);
	atomic64_set(&stats->policy_zram_total_pages, 0);
	atomic64_set(&stats->policy_zram_increase_pages, 0);
	atomic64_set(&stats->policy_zram_increase_boost_pages, 0);
	atomic64_set(&stats->policy_zram_increase_budget_pages, 0);
	atomic64_set(&stats->policy_empty_rounds, 0);
	atomic64_set(&stats->policy_empty_backoff_interval_ms, 0);
	atomic64_set(&stats->policy_empty_backoff_skipped, 0);
	atomic64_set(&stats->policy_window_start_jiffies, 0);
	atomic64_set(&stats->policy_window_written_pages, 0);
	atomic64_set(&stats->policy_window_throttled, 0);
	atomic64_set(&stats->auto_memcg_candidate_count, 0);
	atomic64_set(&stats->auto_per_memcg_queued, 0);
	atomic64_set(&stats->auto_per_memcg_success, 0);
	atomic64_set(&stats->auto_per_memcg_no_data, 0);
	atomic64_set(&stats->auto_per_memcg_error, 0);
	atomic64_set(&stats->auto_global_fallback, 0);
	atomic64_set(&stats->avail_buffers_writes, 0);
	atomic64_set(&stats->avail_buffers_wakeups, 0);
	atomic64_set(&stats->avail_buffers_low_events, 0);
	atomic64_set(&stats->avail_buffers_high_events, 0);
	atomic64_set(&stats->avail_buffers_swap_low_events, 0);
	atomic64_set(&stats->avail_buffers_last_avail, 0);
	atomic64_set(&stats->avail_buffers_last_min, 0);
	atomic64_set(&stats->avail_buffers_last_high, 0);
	atomic64_set(&stats->avail_buffers_last_free_swap_threshold, 0);
	atomic64_set(&stats->avail_buffers_effective_min, 0);
	atomic64_set(&stats->avail_buffers_effective_high, 0);
	atomic64_set(&stats->erm_avail_buffer_enable_store, 0);
	atomic64_set(&stats->erm_avail_buffer_writes, 0);
	atomic64_set(&stats->erm_avail_buffer_last_ret, 0);
	atomic64_set(&stats->avail_buffers_last_seen_avail, 0);
	atomic64_set(&stats->avail_buffers_last_free_swap_pages, 0);
	atomic64_set(&stats->pressure_registered, 0);
	atomic64_set(&stats->pressure_released, 0);
	atomic64_set(&stats->pressure_signaled, 0);
	atomic64_set(&stats->pressure_no_listener, 0);
	atomic64_set(&stats->pressure_signal_errors, 0);
	atomic64_set(&stats->pressure_low_signaled, 0);
	atomic64_set(&stats->pressure_medium_signaled, 0);
	atomic64_set(&stats->pressure_critical_signaled, 0);
	atomic64_set(&stats->pressure_last_ret, 0);
	atomic64_set(&stats->memcg_entries, 0);
	atomic64_set(&stats->memcg_param_updates, 0);
	atomic64_set(&stats->memcg_policy_parse_success, 0);
	atomic64_set(&stats->memcg_policy_parse_error, 0);
	atomic64_set(&stats->memcg_policy_apply, 0);
	atomic64_set(&stats->memcg_single_policy_parse_success, 0);
	atomic64_set(&stats->memcg_single_policy_parse_error, 0);
}

void crystal_hybridswap_set_enabled(bool enabled)
{
	int old = atomic_xchg(&chs.enabled, enabled);

	atomic64_inc(&chs.stats.enable_store);
	if (!!old != enabled)
		chs_log(CHS_LOG_INFO, "hybridswap_enable changed to %d\n",
			enabled);
	else
		chs_log(CHS_LOG_DEBUG, "hybridswap_enable unchanged %d\n",
			enabled);
	crystal_hybridswap_update_auto_policy();
}

bool crystal_hybridswap_enabled(void)
{
	return atomic_read(&chs.enabled);
}

void crystal_hybridswap_set_core_enabled(bool enabled)
{
	int old = atomic_xchg(&chs.core_enabled, enabled);

	atomic64_inc(&chs.stats.core_enable_store);
	if (!!old != enabled)
		chs_log(CHS_LOG_INFO, "hybridswap_core_enable changed to %d\n",
			enabled);
	else
		chs_log(CHS_LOG_DEBUG, "hybridswap_core_enable unchanged %d\n",
			enabled);
	crystal_hybridswap_update_auto_policy();
}

bool crystal_hybridswap_core_enabled(void)
{
	return atomic_read(&chs.core_enabled);
}

void crystal_hybridswap_set_swapd_pause(bool pause)
{
	int old = atomic_xchg(&chs.swapd_pause, pause);

	atomic64_inc(&chs.stats.swapd_pause_store);
	if (!!old != pause)
		chs_log(CHS_LOG_INFO, "hybridswap_swapd_pause changed to %d\n",
			pause);
	else
		chs_log(CHS_LOG_DEBUG, "hybridswap_swapd_pause unchanged %d\n",
			pause);
	crystal_hybridswap_update_auto_policy();
}

bool crystal_hybridswap_swapd_paused(void)
{
	return atomic_read(&chs.swapd_pause);
}

void crystal_hybridswap_set_dev_life(unsigned int level)
{
	atomic_set(&chs.dev_life, level);
	atomic64_set(&chs.stats.dev_life_level, level);
	mutex_lock(&chs.state_lock);
	crystal_hybridswap_refresh_quota_window_locked(jiffies);
	crystal_hybridswap_update_quota_stats_locked(
		atomic64_read(&chs.quota_day),
		crystal_hybridswap_effective_quota_limit(
			atomic64_read(&chs.quota_day), level, true, false));
	mutex_unlock(&chs.state_lock);
	chs_log(CHS_LOG_INFO, "hybridswap_dev_life level set to %u\n", level);
	crystal_hybridswap_update_auto_policy();
}

bool crystal_hybridswap_dev_life(void)
{
	return atomic_read(&chs.dev_life) != 0;
}

unsigned int crystal_hybridswap_dev_life_level(void)
{
	int level = atomic_read(&chs.dev_life);

	return level < 0 ? 0 : level;
}

void crystal_hybridswap_set_loglevel(int level)
{
	atomic_set(&chs.loglevel, level);
	atomic64_inc(&chs.stats.loglevel_store);
	chs_log(CHS_LOG_ERR, "hybridswap loglevel set to %d\n", level);
}

int crystal_hybridswap_loglevel(void)
{
	return atomic_read(&chs.loglevel);
}

void crystal_hybridswap_set_quota_day(u64 quota)
{
	atomic64_set(&chs.quota_day, quota);
	mutex_lock(&chs.state_lock);
	crystal_hybridswap_refresh_quota_window_locked(jiffies);
	crystal_hybridswap_update_quota_stats_locked(quota,
		crystal_hybridswap_effective_quota_limit(quota,
			crystal_hybridswap_dev_life_level(), true, false));
	mutex_unlock(&chs.state_lock);
}

u64 crystal_hybridswap_quota_day(void)
{
	return atomic64_read(&chs.quota_day);
}

int crystal_hybridswap_set_loop_device(const char *buf, size_t len)
{
	char tmp[CHS_LOOP_DEVICE_MAX];
	size_t copy;

	if (!buf)
		return -EINVAL;

	copy = min(len, sizeof(tmp) - 1);
	memcpy(tmp, buf, copy);
	tmp[copy] = '\0';
	strim(tmp);

	mutex_lock(&chs.state_lock);
	strscpy(chs.loop_device, tmp, sizeof(chs.loop_device));
	mutex_unlock(&chs.state_lock);
	atomic64_inc(&chs.stats.loop_device_store);
	crystal_hybridswap_update_auto_policy();

	return 0;
}

void crystal_hybridswap_record_loop_device_bind(int ret)
{
	atomic64_set(&chs.stats.loop_device_last_ret, ret);
	if (ret) {
		atomic64_inc(&chs.stats.loop_device_bind_error);
		chs_log(CHS_LOG_ERR, "hybridswap_loop_device bind failed ret=%d\n",
			ret);
	} else {
		atomic64_inc(&chs.stats.loop_device_bind_success);
		chs_log(CHS_LOG_INFO, "hybridswap_loop_device bind success\n");
	}
}

ssize_t crystal_hybridswap_get_loop_device(char *buf)
{
	ssize_t ret;

	mutex_lock(&chs.state_lock);
	ret = scnprintf(buf, PAGE_SIZE, "%s\n", chs.loop_device);
	mutex_unlock(&chs.state_lock);

	return ret;
}

int crystal_hybridswap_set_zram_wm_ratio(s64 val)
{
	if (val < CHS_MIN_RATIO || val > CHS_MAX_RATIO)
		return -EINVAL;

	atomic64_set(&chs.stored_wm_ratio, val);
	atomic64_set(&chs.stats.policy_zram_wm_ratio, val);
	atomic64_inc(&chs.stats.memcg_param_updates);
	crystal_hybridswap_update_auto_policy();
	return 0;
}

s64 crystal_hybridswap_zram_wm_ratio(void)
{
	return atomic64_read(&chs.stored_wm_ratio);
}

int crystal_hybridswap_set_stored_wm_ratio(s64 val)
{
	return crystal_hybridswap_set_zram_wm_ratio(val);
}

s64 crystal_hybridswap_stored_wm_ratio(void)
{
	return crystal_hybridswap_zram_wm_ratio();
}

static int crystal_hybridswap_queue_writeback_internal(struct device *dev,
		const char *mode, s64 pages, bool auto_req)
{
	struct crystal_hybridswap_zram_target *targets = NULL;
	struct crystal_hybridswap_zram *entry = NULL;
	struct crystal_hybridswap_writeback_target_selection selection = {
		.best = -1,
		.ret = -ENODEV,
	};
	struct zram *zram = NULL;
	crystal_hybridswap_zram_writeback_t writeback = NULL;
	crystal_hybridswap_zram_writeback_ext_t writeback_ext = NULL;
	char wb_mode[CHS_WB_MODE_MAX];
	char detail[160];
	s64 pending_pages;
	int count = 0;
	int selected = -1;
	int ret = 0;

	if (pages <= 0)
		pages = 1;

	strscpy(wb_mode, mode && mode[0] ? mode : CHS_DEFAULT_WB_MODE,
		sizeof(wb_mode));

	if (!chs.wq) {
		atomic64_inc(&chs.stats.writeback_queued);
		atomic64_inc(&chs.stats.writeback_skipped);
		crystal_hybridswap_record_writeback(wb_mode, pages, -ENODEV, 0);
		if (auto_req) {
			scnprintf(detail, sizeof(detail),
				"mode=%s pages=%lld reason=no_workqueue", wb_mode,
				pages);
			crystal_hybridswap_auto_record_result(-ENODEV, 0, false);
			crystal_hybridswap_auto_log_failure(CHS_LOG_INFO,
				crystal_hybridswap_auto_failure_reason(-ENODEV),
				-ENODEV, detail);
		} else {
			chs_log_ratelimited(CHS_LOG_ERR,
				"queue writeback skipped no workqueue mode=%s pages=%lld\n",
				wb_mode, pages);
		}
		return -ENODEV;
	}

	if (!dev) {
		targets = kcalloc(CHS_MAX_ZRAM_TARGETS, sizeof(*targets), GFP_KERNEL);
		if (!targets) {
			atomic64_inc(&chs.stats.writeback_queued);
			atomic64_inc(&chs.stats.writeback_skipped);
			crystal_hybridswap_record_writeback(wb_mode, pages, -ENOMEM, 0);
			return -ENOMEM;
		}
		count = crystal_hybridswap_collect_zram_targets(targets,
			CHS_MAX_ZRAM_TARGETS);
		selected = crystal_hybridswap_select_writeback_target(targets,
			count, &selection);
		if (selected >= 0) {
			dev = targets[selected].dev;
			writeback = targets[selected].writeback;
		}
	}

	mutex_lock(&chs.zram_lock);
	if (dev) {
		entry = crystal_hybridswap_find_zram_locked(dev);
		if (entry && entry->writeback && entry->zram &&
		    zram_try_get(entry->zram)) {
			dev = entry->dev;
			zram = entry->zram;
			writeback = entry->writeback;
			writeback_ext = entry->writeback_ext;
			pending_pages = atomic64_read(&chs.pending_writeback_pages);
			crystal_hybridswap_clear_pending_writeback_locked();
			atomic64_set(&chs.pending_writeback_pages, pending_pages);
			chs.pending_writeback_dev = dev;
			chs.pending_writeback_zram = zram;
			chs.pending_writeback_fn = writeback;
			chs.pending_writeback_ext_fn = writeback_ext;
			strscpy(chs.pending_writeback_mode, wb_mode,
				sizeof(chs.pending_writeback_mode));
			chs.pending_writeback_auto = auto_req;
			crystal_hybridswap_add_pending_pages_locked(
				&chs.pending_writeback_pages, pages);
		} else {
			dev = NULL;
			writeback = NULL;
			writeback_ext = NULL;
		}
	}
	mutex_unlock(&chs.zram_lock);

	if (count)
		crystal_hybridswap_put_zram_targets(targets, count);

	atomic64_inc(&chs.stats.writeback_queued);

	if (!dev || (!writeback && !writeback_ext)) {
		if (!ret)
			ret = selected >= 0 ? -EAGAIN : selection.ret;
		atomic64_inc(&chs.stats.writeback_skipped);
		crystal_hybridswap_record_writeback(wb_mode, pages, ret, 0);
		scnprintf(detail, sizeof(detail),
			"mode=%s pages=%lld registered=%d selected=%d eligible=%d no_writeback=%d no_backing=%d limit=%d no_resident=%d",
			wb_mode, pages, count, selected, selection.eligible,
			selection.no_writeback, selection.no_backing,
			selection.limit_exhausted, selection.no_resident);
		if (auto_req) {
			crystal_hybridswap_auto_record_result(ret, 0, false);
			if (crystal_hybridswap_auto_result_needs_backoff(ret, 0))
				crystal_hybridswap_auto_log_failure(CHS_LOG_INFO,
					crystal_hybridswap_auto_failure_reason(ret), ret,
					detail);
			else
				chs_log_ratelimited(CHS_LOG_WARN,
					"queue writeback skipped %s auto=%d ret=%d\n",
					detail, auto_req, ret);
		} else {
			chs_log_ratelimited(CHS_LOG_ERR,
				"queue writeback skipped %s auto=%d ret=%d\n",
				detail, auto_req, ret);
		}
		goto out_free_targets;
	}

	chs_log_ratelimited(CHS_LOG_DEBUG,
		"queue writeback dev=%s mode=%s pages=%lld auto=%d selected=%lld traversed=%lld eligible=%lld\n",
		dev_name(dev), wb_mode, pages, auto_req,
		atomic64_read(&chs.stats.multi_zram_last_selected),
		atomic64_read(&chs.stats.multi_zram_last_traversed),
		atomic64_read(&chs.stats.multi_zram_last_eligible));
	queue_work(chs.wq, &chs.writeback_work);

out_free_targets:
	kfree(targets);
	return ret;
}

int crystal_hybridswap_queue_writeback(struct device *dev, const char *mode,
				       s64 pages)
{
	return crystal_hybridswap_queue_writeback_internal(dev, mode, pages,
							 false);
}

static int crystal_hybridswap_queue_force_swapout_internal(
		u64 target_cgroup_id, const char *memcg_name, s64 app_score,
		s64 compat_trigger_value, s64 target_pages, bool auto_req)
{
	struct crystal_hybridswap_force_swapout_req *req;
	struct crystal_hybridswap_zram *entry;
	bool has_force_writeback = false;
	int registered = 0;
	int ret = 0;

	if (!chs.wq) {
		atomic64_inc(&chs.stats.force_swapout_queued);
		atomic64_inc(&chs.stats.force_swapout_skipped);
		crystal_hybridswap_record_force_swapout_queue_failure(
			target_cgroup_id, memcg_name, app_score,
			compat_trigger_value, target_pages, auto_req, -ENODEV);
		chs_log_ratelimited(CHS_LOG_ERR,
			"force_swapout queue skipped no workqueue target_memcg=%s target_cgroup_id=%llu compat_trigger_value=%lld\n",
			memcg_name && memcg_name[0] ? memcg_name : "unknown",
			target_cgroup_id, compat_trigger_value);
		return -ENODEV;
	}

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req) {
		atomic64_inc(&chs.stats.force_swapout_queued);
		atomic64_inc(&chs.stats.force_swapout_skipped);
		crystal_hybridswap_record_force_swapout_queue_failure(
			target_cgroup_id, memcg_name, app_score,
			compat_trigger_value, target_pages, auto_req, -ENOMEM);
		return -ENOMEM;
	}

	INIT_WORK(&req->work, crystal_hybridswap_force_swapout_workfn);
	req->target_cgroup_id = target_cgroup_id;
	req->app_score = app_score;
	req->compat_trigger_value = compat_trigger_value;
	req->target_pages = target_pages;
	req->auto_req = auto_req;
	strscpy(req->memcg_name,
		memcg_name && memcg_name[0] ? memcg_name : "unknown",
		sizeof(req->memcg_name));

	mutex_lock(&chs.zram_lock);
	list_for_each_entry(entry, &chs.zram_list, node) {
		registered++;
		if (entry->dev &&
		    (entry->force_writeback || entry->force_writeback_ext))
			has_force_writeback = true;
	}
	if (has_force_writeback)
		atomic64_inc(&chs.force_swapout_inflight);
	mutex_unlock(&chs.zram_lock);
	atomic64_set(&chs.stats.multi_zram_registered, registered);

	atomic64_inc(&chs.stats.force_swapout_queued);
	if (!has_force_writeback) {
		atomic64_inc(&chs.stats.force_swapout_skipped);
		crystal_hybridswap_record_force_swapout_queue_failure(
			target_cgroup_id, req->memcg_name, app_score,
			compat_trigger_value, target_pages, auto_req, -ENODEV);
		chs_log_ratelimited(CHS_LOG_ERR,
			"force_swapout queue skipped no zram target_memcg=%s target_cgroup_id=%llu compat_trigger_value=%lld registered=%d\n",
			req->memcg_name, target_cgroup_id, compat_trigger_value,
			registered);
		kfree(req);
		return -ENODEV;
	}

	atomic64_inc(&chs.pending_force_swapout);
	chs_log_ratelimited(CHS_LOG_INFO,
		"force_swapout queued target_memcg=%s target_cgroup_id=%llu target_mode=%s compat_trigger_value=%lld target_pages=%lld scan_scope=zram_resident_pages registered=%d traversal=multi_zram\n",
		req->memcg_name, target_cgroup_id,
		auto_req ? "per_memcg_auto" : "per_memcg_best_effort",
		compat_trigger_value, target_pages, registered);
	if (!queue_work(chs.wq, &req->work)) {
		atomic64_dec(&chs.pending_force_swapout);
		crystal_hybridswap_force_swapout_drain_done();
		atomic64_inc(&chs.stats.force_swapout_skipped);
		ret = -EBUSY;
		crystal_hybridswap_record_force_swapout_queue_failure(
			target_cgroup_id, req->memcg_name, app_score,
			compat_trigger_value, target_pages, auto_req, ret);
		kfree(req);
	}

	return ret;
}

int crystal_hybridswap_queue_force_swapout(u64 target_cgroup_id,
					   const char *memcg_name, s64 app_score,
					   s64 compat_trigger_value)
{
	return crystal_hybridswap_queue_force_swapout_internal(target_cgroup_id,
		memcg_name, app_score, compat_trigger_value, 0, false);
}

int crystal_hybridswap_queue_force_swapin(u64 target_cgroup_id,
		const char *memcg_name, s64 app_score, s64 request,
		s64 request_pages, s64 pages)
{
	struct crystal_hybridswap_zram *entry;
	bool has_batchin = false;
	int registered = 0;
	int ret = 0;

	if (pages <= 0)
		pages = CHS_FORCE_GLOBAL_SCAN_PAGES;

	atomic64_inc(&chs.stats.force_swapin_queued);
	if (!chs.wq) {
		atomic64_inc(&chs.stats.force_swapin_skipped);
		atomic64_inc(&chs.stats.force_swapin_error);
		if (target_cgroup_id) {
			atomic64_inc(&chs.stats.force_swapin_per_memcg_error);
			crystal_hybridswap_memcg_record_force_swapin_result(
				target_cgroup_id, 0, 0, 0, 0, 0, 0, 0, 0,
				-ENODEV);
		}
		crystal_hybridswap_record_batchin(pages, -ENODEV, 0);
		chs_log_ratelimited(CHS_LOG_ERR,
			"force_swapin queue skipped no workqueue target_memcg=%s target_cgroup_id=%llu request=%lld request_pages=%lld effective_pages=%lld\n",
			memcg_name && memcg_name[0] ? memcg_name : "unknown",
			target_cgroup_id, request, request_pages, pages);
		return -ENODEV;
	}

	mutex_lock(&chs.zram_lock);
	list_for_each_entry(entry, &chs.zram_list, node) {
		registered++;
		if (entry->dev && entry->batchin)
			has_batchin = true;
	}
	if (!has_batchin) {
		ret = -ENODEV;
	} else if (atomic64_read(&chs.pending_force_swapin) > 0 &&
		   chs.pending_batchin_target_cgroup_id != target_cgroup_id) {
		ret = -EBUSY;
	} else {
		chs.pending_batchin_target_cgroup_id = target_cgroup_id;
		chs.pending_batchin_app_score = app_score;
		strscpy(chs.pending_batchin_memcg,
			memcg_name && memcg_name[0] ? memcg_name : "unknown",
			sizeof(chs.pending_batchin_memcg));
		crystal_hybridswap_add_pending_pages_locked(
			&chs.pending_batchin_pages, pages);
		atomic64_inc(&chs.pending_force_swapin);
	}
	mutex_unlock(&chs.zram_lock);
	atomic64_set(&chs.stats.multi_zram_registered, registered);

	if (ret) {
		atomic64_inc(&chs.stats.force_swapin_skipped);
		atomic64_inc(&chs.stats.force_swapin_error);
		if (target_cgroup_id) {
			atomic64_inc(&chs.stats.force_swapin_per_memcg_error);
			crystal_hybridswap_memcg_record_force_swapin_result(
				target_cgroup_id, 0, 0, 0, 0, 0, 0, 0, 0,
				ret);
		}
		crystal_hybridswap_record_batchin(pages, ret, 0);
		chs_log_ratelimited(CHS_LOG_ERR,
			"force_swapin queue skipped target_memcg=%s target_cgroup_id=%llu request=%lld request_pages=%lld effective_pages=%lld registered=%d ret=%d\n",
			memcg_name && memcg_name[0] ? memcg_name : "unknown",
			target_cgroup_id, request, request_pages, pages, registered,
			ret);
		return ret;
	}

	chs_log_ratelimited(CHS_LOG_DEBUG,
		"force_swapin queued target_memcg=%s target_cgroup_id=%llu app_score=%lld request=%lld request_pages=%lld effective_pages=%lld scope=%s registered=%d traversal=multi_zram\n",
		memcg_name && memcg_name[0] ? memcg_name : "unknown",
		target_cgroup_id, app_score, request, request_pages, pages,
		target_cgroup_id ? "per_memcg_page_level" :
		"global_best_effort", registered);
	queue_work(chs.wq, &chs.batchin_work);

	return 0;
}

static int __init crystal_hybridswap_init(void)
{
	int ret;

	mutex_init(&chs.state_lock);
	mutex_init(&chs.zram_lock);
	INIT_LIST_HEAD(&chs.zram_list);
	init_waitqueue_head(&chs.force_swapout_wait);
	INIT_WORK(&chs.writeback_work, crystal_hybridswap_writeback_workfn);
	INIT_WORK(&chs.batchin_work, crystal_hybridswap_batchin_workfn);
	INIT_DELAYED_WORK(&chs.policy_work, crystal_hybridswap_policy_workfn);
	crystal_hybridswap_stats_init(&chs.stats);

	atomic_set(&chs.enabled, 0);
	atomic_set(&chs.core_enabled, 0);
	atomic_set(&chs.swapd_pause, 0);
	atomic_set(&chs.dev_life, 0);
	atomic_set(&chs.loglevel, CHS_LOG_MAX);
	atomic_set(&chs.erm_avail_buffer_enable,
		   CHS_ERM_AVAIL_BUFFER_DEFAULT_ENABLE);
	atomic64_set(&chs.quota_day, CHS_DEFAULT_QUOTA_DAY);
	atomic64_set(&chs.stored_wm_ratio, CHS_DEFAULT_ZRAM_WM_RATIO);
	atomic64_set(&chs.erm_min_avail_buffer, 0);
	atomic64_set(&chs.erm_high_avail_buffer, 0);
	atomic_set(&chs.erm_avail_buffer_valid, 0);
	atomic_set(&chs.policy_suspended, 0);
	atomic_set(&chs.system_sleeping, 0);
	atomic64_set(&chs.pending_policy_wakeups, 0);
	atomic64_set(&chs.pending_writeback_pages, 0);
	atomic64_set(&chs.pending_force_swapout, 0);
	atomic64_set(&chs.pending_force_swapout_pages, 0);
	atomic64_set(&chs.force_swapout_inflight, 0);
	atomic64_set(&chs.pending_batchin_pages, 0);
	atomic64_set(&chs.pending_force_swapin, 0);
	chs.pending_writeback_dev = NULL;
	chs.pending_writeback_zram = NULL;
	chs.pending_writeback_fn = NULL;
	chs.pending_writeback_ext_fn = NULL;
	chs.pending_batchin_target_cgroup_id = 0;
	chs.pending_batchin_app_score = 0;
	chs.pending_batchin_memcg[0] = '\0';
	strscpy(chs.pending_writeback_mode, CHS_DEFAULT_WB_MODE,
		sizeof(chs.pending_writeback_mode));
	chs.pending_writeback_auto = false;
	chs.last_writeback_mode[0] = '\0';
	chs.auto_last_failure_reason[0] = '\0';
	chs.auto_last_empty_jiffies = 0;
	chs.auto_empty_skip_jiffies = 0;
	chs.auto_policy_window_start = 0;
	chs.auto_last_failure_log_jiffies = 0;
	chs.auto_policy_window_written_pages = 0;
	chs.quota_window_start = jiffies;
	chs.auto_last_failure_repeats = 0;
	chs.quota_used_pages = 0;
	crystal_hybridswap_update_quota_stats_locked(CHS_DEFAULT_QUOTA_DAY,
		CHS_DEFAULT_QUOTA_DAY);
	chs.auto_last_writeback_result = 0;
	chs.auto_last_failure_ret = 0;
	memset(&chs.last_zram_pressure, 0, sizeof(chs.last_zram_pressure));
	chs.loop_device[0] = '\0';
	strscpy(chs.last_force_swapin_memcg, "none",
		sizeof(chs.last_force_swapin_memcg));
	strscpy(chs.last_force_swapout_memcg, "none",
		sizeof(chs.last_force_swapout_memcg));
	chs.last_force_swapin_cgroup_id = 0;
	chs.last_force_swapout_cgroup_id = 0;
	chs.last_force_swapin_app_score = 0;
	chs.last_force_swapout_app_score = 0;
	chs.last_force_swapin_request = 0;
	chs.last_force_swapout_mb = 0;
	chs.last_force_swapin_pages = 0;
	chs.last_force_swapout_pages = 0;
	chs.last_force_swapin_effective_pages = 0;
	chs.last_force_swapout_effective_pages = 0;
	chs.last_force_swapout_compat_trigger_value = 0;
	chs.last_force_swapout_scanned_pages = 0;
	chs.last_force_swapout_eligible_pages = 0;
	chs.last_force_swapout_written_pages = 0;
	chs.last_force_swapout_unknown_or_filtered_pages = 0;
	chs.last_force_swapout_ret = 0;
	chs.last_pressure_level = -1;
	chs.last_pressure_ret = 0;
	strscpy(chs.last_pressure_reason, "none",
		sizeof(chs.last_pressure_reason));
	strscpy(chs.last_auto_reason, "none",
		sizeof(chs.last_auto_reason));

	chs.wq = alloc_workqueue(CHS_NAME, WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!chs.wq)
		return -ENOMEM;

	ret = register_pm_notifier(&crystal_hybridswap_pm_nb);
	if (ret) {
		destroy_workqueue(chs.wq);
		chs.wq = NULL;
		return ret;
	}

	crystal_hybridswap_debugfs_init();

	ret = crystal_hybridswap_memcg_init();
	if (ret) {
		unregister_pm_notifier(&crystal_hybridswap_pm_nb);
		crystal_hybridswap_debugfs_exit();
		destroy_workqueue(chs.wq);
		chs.wq = NULL;
		return ret;
	}

	ret = zram_driver_init();
	if (ret) {
		unregister_pm_notifier(&crystal_hybridswap_pm_nb);
		crystal_hybridswap_memcg_exit();
		crystal_hybridswap_debugfs_exit();
		destroy_workqueue(chs.wq);
		chs.wq = NULL;
		return ret;
	}

	pr_info("zram data plane compatibility framework initialized\n");
	return 0;
}

static void __exit crystal_hybridswap_exit(void)
{
	unregister_pm_notifier(&crystal_hybridswap_pm_nb);
	crystal_hybridswap_suspend_auto_policy_sync();
	zram_driver_exit();
	crystal_hybridswap_memcg_exit();
	crystal_hybridswap_pressure_exit();

	if (chs.wq) {
		cancel_delayed_work_sync(&chs.policy_work);
		flush_workqueue(chs.wq);
		destroy_workqueue(chs.wq);
		chs.wq = NULL;
	}

	crystal_hybridswap_debugfs_exit();
	pr_info("zram data plane compatibility framework exited\n");
}

module_init(crystal_hybridswap_init);
module_exit(crystal_hybridswap_exit);

MODULE_AUTHOR("阿菌•未霜");
MODULE_AUTHOR("whitewhale");
MODULE_DESCRIPTION("Compressed RAM Block Device with Crystal Hybridswap support");
MODULE_LICENSE("GPL");
