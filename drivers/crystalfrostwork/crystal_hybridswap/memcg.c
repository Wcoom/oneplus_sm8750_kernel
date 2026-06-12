// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "crystal_hybridswap: " fmt

#include <linux/cgroup.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/memcontrol.h>
#include <linux/mmzone.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/swap.h>
#include <linux/vmstat.h>
#include <linux/workqueue.h>
#include <trace/hooks/mm.h>
#include <trace/hooks/vmscan.h>

#include "crystal_hybridswap_internal.h"

/* Keep in sync with the private enum in mm/vmscan.c for the vendor hook. */
enum scan_balance {
	SCAN_EQUAL,
	SCAN_FRACT,
	SCAN_ANON,
	SCAN_FILE,
};

static DEFINE_MUTEX(memcg_lock);
static LIST_HEAD(memcg_list);
static bool memcg_cftypes_registered;
static bool memcg_css_offline_hook_registered;

#define CHS_FORCE_SHRINK_RECLAIM_INACTIVE	0UL
#define CHS_FORCE_SHRINK_RECLAIM_ALL		1UL
#define CHS_FORCE_SHRINK_DEFAULT_BATCH		(1UL << 10)
#define CHS_FORCE_SHRINK_MAX_BATCH_PAGES	(16UL * CHS_PAGES_PER_MB)
#define CHS_FORCE_SHRINK_MAX_QUEUED_PER_TYPE	4
#define CHS_FORCE_SHRINK_FILE_LOW_PAGES \
	(((unsigned long)SZ_512M + SZ_256M) >> PAGE_SHIFT)
#define CHS_PF_SHRINK_ANON			PF__HOLE__02000000

struct crystal_hybridswap_force_shrink_req {
	struct work_struct work;
	struct cgroup_subsys_state *css;
	bool file;
	unsigned long reclaim_flag;
	unsigned long target;
	unsigned long batch;
	u64 cgroup_id;
	char memcg_name[CHS_MEMCG_NAME_MAX];
};

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM
static struct crystal_hybridswap_swapd_param
	swapd_params[CHS_SWAPD_MAX_LEVEL_NUM];
static int swapd_param_levels;
static char swapd_memcgs_param_raw[CHS_POLICY_RAW_MAX];
#endif
static atomic_t swapd_avail_buffers;
static atomic_t swapd_min_avail_buffers;
static atomic_t swapd_high_avail_buffers;
static atomic64_t swapd_free_swap_threshold;
static atomic_t force_shrink_anon_queued;
static atomic_t force_shrink_file_queued;
static bool force_shrink_anon_hook_registered;

static struct crystal_hybridswap_memcg *
memcg_find_locked(struct cgroup_subsys_state *css)
{
	struct crystal_hybridswap_memcg *entry;

	list_for_each_entry(entry, &memcg_list, node) {
		if (entry->css == css)
			return entry;
	}

	return NULL;
}

static void memcg_free_entry_locked(struct crystal_hybridswap_memcg *entry)
{
	list_del(&entry->node);
	css_put(entry->css);
	kfree(entry);
	atomic64_dec_if_positive(&chs.stats.memcg_entries);
}

static void memcg_snapshot_copy(struct crystal_hybridswap_memcg *dst,
		const struct crystal_hybridswap_memcg *src)
{
	memset(dst, 0, sizeof(*dst));
	INIT_LIST_HEAD(&dst->node);
	dst->cgroup_id = src->cgroup_id;
	strscpy(dst->name, src->name, sizeof(dst->name));
	strscpy(dst->single_policy_raw, src->single_policy_raw,
		sizeof(dst->single_policy_raw));
	atomic64_set(&dst->app_score, atomic64_read(&src->app_score));
	atomic64_set(&dst->app_uid, atomic64_read(&src->app_uid));
	atomic64_set(&dst->ub_ufs2zram_ratio,
		atomic64_read(&src->ub_ufs2zram_ratio));
	atomic_set(&dst->ub_mem2zram_ratio,
		atomic_read(&src->ub_mem2zram_ratio));
	atomic_set(&dst->ub_zram2ufs_ratio,
		atomic_read(&src->ub_zram2ufs_ratio));
	atomic_set(&dst->refault_threshold,
		atomic_read(&src->refault_threshold));
	atomic_set(&dst->policy_level, atomic_read(&src->policy_level));
	atomic64_set(&dst->force_swapin,
		atomic64_read(&src->force_swapin));
	atomic64_set(&dst->force_swapin_pages,
		atomic64_read(&src->force_swapin_pages));
	atomic64_set(&dst->force_swapin_last_pages,
		atomic64_read(&src->force_swapin_last_pages));
	atomic64_set(&dst->force_swapin_last_effective_pages,
		atomic64_read(&src->force_swapin_last_effective_pages));
	atomic64_set(&dst->force_swapin_last_batchin_pages,
		atomic64_read(&src->force_swapin_last_batchin_pages));
	atomic64_set(&dst->force_swapin_last_ret,
		atomic64_read(&src->force_swapin_last_ret));
	atomic64_set(&dst->force_swapin_last_request,
		atomic64_read(&src->force_swapin_last_request));
	atomic64_set(&dst->force_swapin_last_read_errors,
		atomic64_read(&src->force_swapin_last_read_errors));
	atomic64_set(&dst->force_swapin_last_prepare_errors,
		atomic64_read(&src->force_swapin_last_prepare_errors));
	atomic64_set(&dst->force_swapout,
		atomic64_read(&src->force_swapout));
	atomic64_set(&dst->force_swapout_pages,
		atomic64_read(&src->force_swapout_pages));
	atomic64_set(&dst->force_swapout_effective_pages,
		atomic64_read(&src->force_swapout_effective_pages));
	atomic64_set(&dst->force_swapout_last_pages,
		atomic64_read(&src->force_swapout_last_pages));
	atomic64_set(&dst->force_swapout_last_effective_pages,
		atomic64_read(&src->force_swapout_last_effective_pages));
	atomic64_set(&dst->force_swapout_last_mb,
		atomic64_read(&src->force_swapout_last_mb));
	atomic64_set(&dst->force_swapout_last_compat_trigger_value,
		atomic64_read(&src->force_swapout_last_compat_trigger_value));
	atomic64_set(&dst->force_swapout_last_scanned_pages,
		atomic64_read(&src->force_swapout_last_scanned_pages));
	atomic64_set(&dst->force_swapout_last_eligible_pages,
		atomic64_read(&src->force_swapout_last_eligible_pages));
	atomic64_set(&dst->force_swapout_last_written_pages,
		atomic64_read(&src->force_swapout_last_written_pages));
	atomic64_set(&dst->force_swapout_last_unknown_or_filtered_pages,
		atomic64_read(&src->force_swapout_last_unknown_or_filtered_pages));
	atomic64_set(&dst->force_swapout_last_ret,
		atomic64_read(&src->force_swapout_last_ret));
	atomic64_set(&dst->force_shrink_anon,
		atomic64_read(&src->force_shrink_anon));
	atomic64_set(&dst->force_shrink_file,
		atomic64_read(&src->force_shrink_file));
	atomic64_set(&dst->force_shrink_anon_pages,
		atomic64_read(&src->force_shrink_anon_pages));
	atomic64_set(&dst->force_shrink_file_pages,
		atomic64_read(&src->force_shrink_file_pages));
	atomic64_set(&dst->force_shrink_anon_last_target,
		atomic64_read(&src->force_shrink_anon_last_target));
	atomic64_set(&dst->force_shrink_file_last_target,
		atomic64_read(&src->force_shrink_file_last_target));
	atomic64_set(&dst->force_shrink_anon_last_batch,
		atomic64_read(&src->force_shrink_anon_last_batch));
	atomic64_set(&dst->force_shrink_file_last_batch,
		atomic64_read(&src->force_shrink_file_last_batch));
	atomic64_set(&dst->force_shrink_anon_last_reclaimed,
		atomic64_read(&src->force_shrink_anon_last_reclaimed));
	atomic64_set(&dst->force_shrink_file_last_reclaimed,
		atomic64_read(&src->force_shrink_file_last_reclaimed));
	atomic64_set(&dst->force_shrink_anon_last_ret,
		atomic64_read(&src->force_shrink_anon_last_ret));
	atomic64_set(&dst->force_shrink_file_last_ret,
		atomic64_read(&src->force_shrink_file_last_ret));
	atomic64_set(&dst->force_shrink_anon_dropped,
		atomic64_read(&src->force_shrink_anon_dropped));
	atomic64_set(&dst->force_shrink_file_dropped,
		atomic64_read(&src->force_shrink_file_dropped));
	atomic64_set(&dst->force_shrink_anon_skipped,
		atomic64_read(&src->force_shrink_anon_skipped));
	atomic64_set(&dst->force_shrink_file_skipped,
		atomic64_read(&src->force_shrink_file_skipped));
}

static int memcg_collect_snapshots(struct crystal_hybridswap_memcg **out)
{
	struct crystal_hybridswap_memcg *entry;
	struct crystal_hybridswap_memcg *snapshots;
	int count = 0;
	int copied = 0;

	if (!out)
		return -EINVAL;
	*out = NULL;

	mutex_lock(&memcg_lock);
	list_for_each_entry(entry, &memcg_list, node)
		count++;
	mutex_unlock(&memcg_lock);

	if (!count)
		return 0;

	snapshots = kcalloc(count, sizeof(*snapshots), GFP_KERNEL);
	if (!snapshots)
		return -ENOMEM;

	mutex_lock(&memcg_lock);
	list_for_each_entry(entry, &memcg_list, node) {
		if (copied >= count)
			break;
		memcg_snapshot_copy(&snapshots[copied], entry);
		copied++;
	}
	mutex_unlock(&memcg_lock);

	*out = snapshots;
	return copied;
}

static int memcg_snapshot_at(struct crystal_hybridswap_memcg *snapshot,
		loff_t index)
{
	struct crystal_hybridswap_memcg *entry;
	loff_t pos = 0;
	int ret = -ENOENT;

	if (!snapshot || index < 0)
		return -EINVAL;

	mutex_lock(&memcg_lock);
	list_for_each_entry(entry, &memcg_list, node) {
		if (pos++ != index)
			continue;
		memcg_snapshot_copy(snapshot, entry);
		ret = 0;
		break;
	}
	mutex_unlock(&memcg_lock);

	return ret;
}

static void memcg_css_offline(void *data, struct cgroup_subsys_state *css,
		struct mem_cgroup *memcg)
{
	struct crystal_hybridswap_memcg *entry;

	if (!css)
		return;

	mutex_lock(&memcg_lock);
	entry = memcg_find_locked(css);
	if (entry) {
		chs_log_ratelimited(CHS_LOG_DEBUG,
			"memcg offline cleanup id=%llu name=%s\n",
			entry->cgroup_id, entry->name);
		memcg_free_entry_locked(entry);
	}
	mutex_unlock(&memcg_lock);
}

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM
static char *memcg_next_token(char **buf)
{
	char *token;

	while ((token = strsep(buf, " \t\n")) != NULL) {
		if (*token)
			return token;
	}

	return NULL;
}

static int memcg_parse_uint_token(const char *token, unsigned int *val)
{
	const char *value;

	if (!token || !val)
		return -EINVAL;

	value = strchr(token, '=');
	if (value)
		value++;
	else
		value = token;

	return kstrtouint(value, 0, val);
}
#endif

static s64 memcg_parse_compat_s64(char *buf)
{
	s64 val = 0;

	if (!buf)
		return 0;

	buf = strim(buf);
	if (!buf[0])
		return 0;
	if (kstrtos64(buf, 0, &val))
		return 0;

	return val;
}

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM
static unsigned int memcg_clamp_uint(unsigned int val, unsigned int max,
				     bool *clamped)
{
	if (val > max) {
		*clamped = true;
		return max;
	}

	return val;
}

static void memcg_copy_raw(char *dst, const char *src)
{
	char raw[CHS_POLICY_RAW_MAX];

	strscpy(raw, src ? src : "", sizeof(raw));
	strim(raw);
	strscpy(dst, raw, CHS_POLICY_RAW_MAX);
}
#endif

static s64 memcg_pages_to_mb(s64 pages)
{
	if (pages <= 0)
		return 0;

	return pages / (s64)CHS_PAGES_PER_MB;
}

static const char *memcg_force_shrink_type(bool file)
{
	return file ? "file" : "anon";
}

static atomic_t *memcg_force_shrink_queued_counter(bool file)
{
	return file ? &force_shrink_file_queued : &force_shrink_anon_queued;
}

static unsigned long memcg_force_shrink_lru_pages(struct mem_cgroup *memcg,
							 bool file, bool active)
{
	if (file)
		return memcg_page_state(memcg,
			active ? NR_ACTIVE_FILE : NR_INACTIVE_FILE);

	return memcg_page_state(memcg,
		active ? NR_ACTIVE_ANON : NR_INACTIVE_ANON);
}

static bool memcg_force_shrink_file_low(void)
{
	return global_node_page_state(NR_INACTIVE_FILE) <
		CHS_FORCE_SHRINK_FILE_LOW_PAGES;
}

static int memcg_force_shrink_parse(struct mem_cgroup *memcg, bool file,
				    char *buf, unsigned long *reclaim_flag,
				    unsigned long *target, unsigned long *batch,
				    bool *clamped)
{
	unsigned long flag = 0;
	unsigned long user_batch = 0;
	unsigned long need_reclaim = 0;
	unsigned long req_batch = CHS_FORCE_SHRINK_DEFAULT_BATCH;
	int parsed;

	buf = strim(buf);
	parsed = sscanf(buf, "%lu %lu", &flag, &user_batch);
	if (parsed != 1 && parsed != 2)
		return -EINVAL;

	if (flag == CHS_FORCE_SHRINK_RECLAIM_INACTIVE) {
		need_reclaim = memcg_force_shrink_lru_pages(memcg, file, false);
	} else if (flag == CHS_FORCE_SHRINK_RECLAIM_ALL) {
		need_reclaim = memcg_force_shrink_lru_pages(memcg, file, false) +
			memcg_force_shrink_lru_pages(memcg, file, true);
	} else {
		need_reclaim = flag;
	}

	if (parsed == 2 && user_batch > 0)
		req_batch = user_batch;

	if (req_batch > CHS_FORCE_SHRINK_MAX_BATCH_PAGES) {
		req_batch = CHS_FORCE_SHRINK_MAX_BATCH_PAGES;
		*clamped = true;
	}

	if (need_reclaim > 0 && req_batch > need_reclaim)
		req_batch = need_reclaim;
	if (req_batch == 0)
		req_batch = 1;

	*reclaim_flag = flag;
	*target = need_reclaim;
	*batch = req_batch;
	return 0;
}

static void memcg_force_shrink_record_drop(
		struct crystal_hybridswap_memcg *entry, bool file)
{
	if (file) {
		atomic64_inc(&chs.stats.force_shrink_file_dropped);
		atomic64_set(&chs.stats.force_shrink_file_last_ret, -EBUSY);
		if (entry) {
			atomic64_inc(&entry->force_shrink_file_dropped);
			atomic64_set(&entry->force_shrink_file_last_ret, -EBUSY);
		}
	} else {
		atomic64_inc(&chs.stats.force_shrink_anon_dropped);
		atomic64_set(&chs.stats.force_shrink_anon_last_ret, -EBUSY);
		if (entry) {
			atomic64_inc(&entry->force_shrink_anon_dropped);
			atomic64_set(&entry->force_shrink_anon_last_ret, -EBUSY);
		}
	}
}

static void memcg_force_shrink_record_skip(
		struct crystal_hybridswap_memcg *entry, bool file, int ret)
{
	if (file) {
		atomic64_inc(&chs.stats.force_shrink_file_skipped);
		atomic64_set(&chs.stats.force_shrink_file_last_ret, ret);
		if (entry) {
			atomic64_inc(&entry->force_shrink_file_skipped);
			atomic64_set(&entry->force_shrink_file_last_ret, ret);
		}
	} else {
		atomic64_inc(&chs.stats.force_shrink_anon_skipped);
		atomic64_set(&chs.stats.force_shrink_anon_last_ret, ret);
		if (entry) {
			atomic64_inc(&entry->force_shrink_anon_skipped);
			atomic64_set(&entry->force_shrink_anon_last_ret, ret);
		}
	}
}

static void memcg_force_shrink_record_request(
		struct crystal_hybridswap_memcg *entry, bool file,
		unsigned long reclaim_flag, unsigned long target,
		unsigned long batch)
{
	if (file) {
		atomic64_inc(&entry->force_shrink_file);
		atomic64_set(&entry->force_shrink_file_last_param, reclaim_flag);
		atomic64_set(&entry->force_shrink_file_last_target, target);
		atomic64_set(&entry->force_shrink_file_last_batch, batch);
		atomic64_inc(&chs.stats.force_shrink_file);
		atomic64_set(&chs.stats.force_shrink_file_last_param,
			     reclaim_flag);
		atomic64_set(&chs.stats.force_shrink_file_last_target, target);
		atomic64_set(&chs.stats.force_shrink_file_last_batch, batch);
	} else {
		atomic64_inc(&entry->force_shrink_anon);
		atomic64_set(&entry->force_shrink_anon_last_param, reclaim_flag);
		atomic64_set(&entry->force_shrink_anon_last_target, target);
		atomic64_set(&entry->force_shrink_anon_last_batch, batch);
		atomic64_inc(&chs.stats.force_shrink_anon);
		atomic64_set(&chs.stats.force_shrink_anon_last_param,
			     reclaim_flag);
		atomic64_set(&chs.stats.force_shrink_anon_last_target, target);
		atomic64_set(&chs.stats.force_shrink_anon_last_batch, batch);
	}
}

static void memcg_force_shrink_record_result(
		struct cgroup_subsys_state *css, bool file,
		unsigned long target, unsigned long batch,
		unsigned long reclaimed, int ret)
{
	struct crystal_hybridswap_memcg *entry;

	if (file) {
		atomic64_add(reclaimed, &chs.stats.force_shrink_file_pages);
		atomic64_set(&chs.stats.force_shrink_file_last_target, target);
		atomic64_set(&chs.stats.force_shrink_file_last_batch, batch);
		atomic64_set(&chs.stats.force_shrink_file_last_reclaimed,
			     reclaimed);
		atomic64_set(&chs.stats.force_shrink_file_last_ret, ret);
		if (ret)
			atomic64_inc(&chs.stats.force_shrink_file_skipped);
	} else {
		atomic64_add(reclaimed, &chs.stats.force_shrink_anon_pages);
		atomic64_set(&chs.stats.force_shrink_anon_last_target, target);
		atomic64_set(&chs.stats.force_shrink_anon_last_batch, batch);
		atomic64_set(&chs.stats.force_shrink_anon_last_reclaimed,
			     reclaimed);
		atomic64_set(&chs.stats.force_shrink_anon_last_ret, ret);
		if (ret)
			atomic64_inc(&chs.stats.force_shrink_anon_skipped);
	}

	mutex_lock(&memcg_lock);
	entry = memcg_find_locked(css);
	if (entry) {
		if (file) {
			atomic64_add(reclaimed, &entry->force_shrink_file_pages);
			atomic64_set(&entry->force_shrink_file_last_target,
				     target);
			atomic64_set(&entry->force_shrink_file_last_batch,
				     batch);
			atomic64_set(&entry->force_shrink_file_last_reclaimed,
				     reclaimed);
			atomic64_set(&entry->force_shrink_file_last_ret, ret);
			if (ret)
				atomic64_inc(&entry->force_shrink_file_skipped);
		} else {
			atomic64_add(reclaimed, &entry->force_shrink_anon_pages);
			atomic64_set(&entry->force_shrink_anon_last_target,
				     target);
			atomic64_set(&entry->force_shrink_anon_last_batch,
				     batch);
			atomic64_set(&entry->force_shrink_anon_last_reclaimed,
				     reclaimed);
			atomic64_set(&entry->force_shrink_anon_last_ret, ret);
			if (ret)
				atomic64_inc(&entry->force_shrink_anon_skipped);
		}
	}
	mutex_unlock(&memcg_lock);
}

static void memcg_force_shrink_tune_scan_type(void *data,
					      enum scan_balance *scan_type)
{
	if (scan_type && (current->flags & CHS_PF_SHRINK_ANON))
		*scan_type = SCAN_ANON;
}

static void memcg_force_shrink_workfn(struct work_struct *work)
{
	struct crystal_hybridswap_force_shrink_req *req;
	struct mem_cgroup *memcg;
	unsigned long reclaimed_total = 0;
	unsigned int reclaim_options = MEMCG_RECLAIM_PROACTIVE;
	unsigned int old_flag = current->flags & CHS_PF_SHRINK_ANON;
	bool anon_flag_set = false;
	bool hook_registered = false;
	int ret = 0;

	req = container_of(work, struct crystal_hybridswap_force_shrink_req,
			   work);
	atomic64_inc(&chs.stats.force_shrink_worker_runs);

	memcg = mem_cgroup_from_css(req->css);
	if (!memcg || !mem_cgroup_online(memcg)) {
		ret = -ENODEV;
		chs_log(CHS_LOG_WARN,
			"force_shrink_%s skip offline memcg=%s id=%llu target=%lu batch=%lu\n",
			memcg_force_shrink_type(req->file), req->memcg_name,
			req->cgroup_id, req->target, req->batch);
		goto out;
	}

	if (!req->file) {
		hook_registered = READ_ONCE(force_shrink_anon_hook_registered);

		reclaim_options |= MEMCG_RECLAIM_MAY_SWAP;
		if (hook_registered) {
			current->flags |= CHS_PF_SHRINK_ANON;
			anon_flag_set = true;
		} else {
			atomic64_inc(&chs.stats.force_shrink_anon_best_effort);
		}
	}

	chs_log(CHS_LOG_INFO,
		"force_shrink_%s start memcg=%s id=%llu target=%lu batch=%lu mode=%s\n",
		memcg_force_shrink_type(req->file), req->memcg_name,
		req->cgroup_id, req->target, req->batch,
		req->file ? "file_only_no_swap" :
		(hook_registered ? "anon_hook" : "best_effort"));

	if (req->file && memcg_force_shrink_file_low()) {
		ret = -EBUSY;
		chs_log(CHS_LOG_INFO,
			"force_shrink_file skip low inactive_file memcg=%s id=%llu threshold_pages=%lu\n",
			req->memcg_name, req->cgroup_id,
			CHS_FORCE_SHRINK_FILE_LOW_PAGES);
		goto out;
	}

	while (reclaimed_total < req->target) {
		unsigned long todo = min(req->batch,
			req->target - reclaimed_total);
		unsigned long reclaimed;

		if (!todo)
			break;

		reclaimed = try_to_free_mem_cgroup_pages(memcg, todo, GFP_KERNEL,
						      reclaim_options);
		chs_log(CHS_LOG_INFO,
			"force_shrink_%s batch memcg=%s target=%lu batch=%lu reclaimed=%lu total=%lu\n",
			memcg_force_shrink_type(req->file), req->memcg_name,
			req->target, todo, reclaimed,
			reclaimed_total + reclaimed);

		if (!reclaimed)
			break;
		reclaimed_total += reclaimed;
		if (req->file && memcg_force_shrink_file_low()) {
			chs_log(CHS_LOG_INFO,
				"force_shrink_file stop low inactive_file memcg=%s id=%llu reclaimed=%lu threshold_pages=%lu\n",
				req->memcg_name, req->cgroup_id, reclaimed_total,
				CHS_FORCE_SHRINK_FILE_LOW_PAGES);
			break;
		}
		cond_resched();
	}

	if (!reclaimed_total && !ret)
		ret = -ENODATA;

out:
	if (anon_flag_set) {
		current->flags &= ~CHS_PF_SHRINK_ANON;
		current->flags |= old_flag;
	}

	memcg_force_shrink_record_result(req->css, req->file, req->target,
					       req->batch, reclaimed_total, ret);
	chs_log(CHS_LOG_INFO,
		"force_shrink_%s done memcg=%s id=%llu target=%lu batch=%lu reclaimed=%lu ret=%d queued=%d\n",
		memcg_force_shrink_type(req->file), req->memcg_name,
		req->cgroup_id, req->target, req->batch, reclaimed_total, ret,
		atomic_read(memcg_force_shrink_queued_counter(req->file)) - 1);

	atomic_dec(memcg_force_shrink_queued_counter(req->file));
	css_put(req->css);
	kfree(req);
}

static ssize_t memcg_force_shrink_write(struct kernfs_open_file *of, char *buf,
				       size_t nbytes, bool file)
{
	struct crystal_hybridswap_force_shrink_req *req;
	struct crystal_hybridswap_memcg *entry;
	struct cgroup_subsys_state *css = of_css(of);
	struct mem_cgroup *memcg;
	atomic_t *queued_counter;
	unsigned long reclaim_flag = 0;
	unsigned long target = 0;
	unsigned long batch = CHS_FORCE_SHRINK_DEFAULT_BATCH;
	bool clamped = false;
	int queued;
	int ret;

	entry = crystal_hybridswap_memcg_get(css, true);
	if (!entry)
		return -ENOMEM;

	memcg = mem_cgroup_from_css(css);
	if (!memcg) {
		memcg_force_shrink_record_skip(entry, file, -EINVAL);
		return nbytes;
	}

	ret = memcg_force_shrink_parse(memcg, file, buf, &reclaim_flag,
					     &target, &batch, &clamped);
	if (ret) {
		s64 param = memcg_parse_compat_s64(buf);

		memcg_force_shrink_record_request(entry, file, param, 0, batch);
		memcg_force_shrink_record_skip(entry, file, ret);
		chs_log(CHS_LOG_WARN,
			"force_shrink_%s parse error memcg=%s raw=%s ret=%d\n",
			memcg_force_shrink_type(file), entry->name, strim(buf), ret);
		return nbytes;
	}

	memcg_force_shrink_record_request(entry, file, reclaim_flag, target,
						 batch);

	chs_log(CHS_LOG_INFO,
		"normal page batch %lu, nr_need_reclaim %lu, file %d memcg=%s flag=%lu%s\n",
		batch, target, file, entry->name, reclaim_flag,
		clamped ? " clamped" : "");

	if (!target) {
		memcg_force_shrink_record_skip(entry, file, -ENODATA);
		chs_log(CHS_LOG_INFO,
			"force_shrink_%s skip empty memcg=%s flag=%lu batch=%lu\n",
			memcg_force_shrink_type(file), entry->name, reclaim_flag,
			batch);
		return nbytes;
	}

	if (!chs.wq) {
		memcg_force_shrink_record_skip(entry, file, -ENODEV);
		return nbytes;
	}

	queued_counter = memcg_force_shrink_queued_counter(file);
	queued = atomic_inc_return(queued_counter);
	if (queued > CHS_FORCE_SHRINK_MAX_QUEUED_PER_TYPE) {
		atomic_dec(queued_counter);
		memcg_force_shrink_record_drop(entry, file);
		chs_log(CHS_LOG_WARN,
			"force_shrink_%s drop queue_full memcg=%s target=%lu batch=%lu queued=%d max=%u\n",
			memcg_force_shrink_type(file), entry->name, target, batch,
			queued - 1, CHS_FORCE_SHRINK_MAX_QUEUED_PER_TYPE);
		return nbytes;
	}

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req) {
		atomic_dec(queued_counter);
		memcg_force_shrink_record_drop(entry, file);
		return -ENOMEM;
	}

	if (!css_tryget_online(css)) {
		atomic_dec(queued_counter);
		kfree(req);
		memcg_force_shrink_record_skip(entry, file, -ENODEV);
		return nbytes;
	}

	INIT_WORK(&req->work, memcg_force_shrink_workfn);
	req->css = css;
	req->file = file;
	req->reclaim_flag = reclaim_flag;
	req->target = target;
	req->batch = batch;
	req->cgroup_id = entry->cgroup_id;
	mutex_lock(&memcg_lock);
	strscpy(req->memcg_name, entry->name, sizeof(req->memcg_name));
	mutex_unlock(&memcg_lock);

	if (!queue_work(chs.wq, &req->work)) {
		css_put(css);
		atomic_dec(queued_counter);
		kfree(req);
		memcg_force_shrink_record_drop(entry, file);
		return nbytes;
	}

	atomic64_inc(&chs.stats.force_shrink_queued);
	chs_log(CHS_LOG_INFO,
		"force_shrink_%s queued memcg=%s id=%llu target=%lu batch=%lu queued=%d\n",
		memcg_force_shrink_type(file), req->memcg_name, req->cgroup_id,
		target, batch, queued);
	return nbytes;
}

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM
static bool memcg_apply_policy_locked(struct crystal_hybridswap_memcg *entry)
{
	s64 score;
	int i;

	if (!entry || swapd_param_levels <= 0) {
		if (entry)
			atomic_set(&entry->policy_level, CHS_POLICY_LEVEL_NONE);
		return false;
	}

	score = atomic64_read(&entry->app_score);
	for (i = 0; i < swapd_param_levels; i++) {
		if (score < swapd_params[i].min_score ||
		    score > swapd_params[i].max_score)
			continue;

		atomic_set(&entry->ub_mem2zram_ratio,
			   swapd_params[i].ub_mem2zram_ratio);
		atomic_set(&entry->ub_zram2ufs_ratio,
			   swapd_params[i].ub_zram2ufs_ratio);
		atomic_set(&entry->refault_threshold,
			   swapd_params[i].refault_threshold);
		atomic_set(&entry->policy_level, i);
		atomic64_inc(&chs.stats.memcg_policy_apply);
		return true;
	}

	atomic_set(&entry->policy_level, CHS_POLICY_LEVEL_NONE);
	return false;
}

static void memcg_apply_policy(struct crystal_hybridswap_memcg *entry)
{
	mutex_lock(&memcg_lock);
	memcg_apply_policy_locked(entry);
	mutex_unlock(&memcg_lock);
}

static void memcg_apply_policies_locked(void)
{
	struct crystal_hybridswap_memcg *entry;

	list_for_each_entry(entry, &memcg_list, node)
		memcg_apply_policy_locked(entry);
}
#else
static void memcg_apply_policy(struct crystal_hybridswap_memcg *entry)
{
	if (entry)
		atomic_set(&entry->policy_level, CHS_POLICY_LEVEL_NONE);
}
#endif

struct crystal_hybridswap_memcg *
crystal_hybridswap_memcg_get(struct cgroup_subsys_state *css, bool create)
{
	struct crystal_hybridswap_memcg *entry;
	char name[CHS_MEMCG_NAME_MAX] = "";

	if (!css)
		return NULL;

	mutex_lock(&memcg_lock);
	entry = memcg_find_locked(css);
	if (entry || !create)
		goto out;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		goto out;

	INIT_LIST_HEAD(&entry->node);
	entry->css = css;
	css_get(css);
	entry->cgroup_id = cgroup_id(css->cgroup);
	cgroup_name(css->cgroup, name, sizeof(name));
	strscpy(entry->name, name[0] ? name : "unknown", sizeof(entry->name));
	atomic64_set(&entry->app_score, CHS_DEFAULT_APP_SCORE);
	atomic64_set(&entry->app_uid, 0);
	atomic64_set(&entry->ub_ufs2zram_ratio, CHS_DEFAULT_RATIO);
	atomic_set(&entry->ub_mem2zram_ratio, CHS_DEFAULT_RATIO);
	atomic_set(&entry->ub_zram2ufs_ratio, CHS_DEFAULT_RATIO);
	atomic_set(&entry->refault_threshold, 0);
	atomic_set(&entry->policy_level, CHS_POLICY_LEVEL_NONE);
	atomic64_set(&entry->force_swapin, 0);
	atomic64_set(&entry->force_swapin_pages, 0);
	atomic64_set(&entry->force_swapin_last_pages, 0);
	atomic64_set(&entry->force_swapin_last_effective_pages, 0);
	atomic64_set(&entry->force_swapin_last_batchin_pages, 0);
	atomic64_set(&entry->force_swapin_last_ret, 0);
	atomic64_set(&entry->force_swapin_last_request, 0);
	atomic64_set(&entry->force_swapin_last_read_errors, 0);
	atomic64_set(&entry->force_swapin_last_prepare_errors, 0);
	atomic64_set(&entry->force_swapout, 0);
	atomic64_set(&entry->force_swapout_pages, 0);
	atomic64_set(&entry->force_swapout_effective_pages, 0);
	atomic64_set(&entry->force_swapout_last_pages, 0);
	atomic64_set(&entry->force_swapout_last_effective_pages, 0);
	atomic64_set(&entry->force_swapout_last_mb, 0);
	atomic64_set(&entry->force_swapout_last_compat_trigger_value, 0);
	atomic64_set(&entry->force_swapout_last_scanned_pages, 0);
	atomic64_set(&entry->force_swapout_last_eligible_pages, 0);
	atomic64_set(&entry->force_swapout_last_written_pages, 0);
	atomic64_set(&entry->force_swapout_last_unknown_or_filtered_pages, 0);
	atomic64_set(&entry->force_swapout_last_ret, 0);
	atomic64_set(&entry->force_shrink_anon, 0);
	atomic64_set(&entry->force_shrink_file, 0);
	atomic64_set(&entry->aging_anon, 0);
	atomic64_set(&entry->force_shrink_anon_last_param, 0);
	atomic64_set(&entry->force_shrink_file_last_param, 0);
	atomic64_set(&entry->aging_anon_last_param, 0);
	atomic64_set(&entry->force_shrink_anon_pages, 0);
	atomic64_set(&entry->force_shrink_file_pages, 0);
	atomic64_set(&entry->force_shrink_anon_last_target, 0);
	atomic64_set(&entry->force_shrink_file_last_target, 0);
	atomic64_set(&entry->force_shrink_anon_last_batch, 0);
	atomic64_set(&entry->force_shrink_file_last_batch, 0);
	atomic64_set(&entry->force_shrink_anon_last_reclaimed, 0);
	atomic64_set(&entry->force_shrink_file_last_reclaimed, 0);
	atomic64_set(&entry->force_shrink_anon_last_ret, 0);
	atomic64_set(&entry->force_shrink_file_last_ret, 0);
	atomic64_set(&entry->force_shrink_anon_dropped, 0);
	atomic64_set(&entry->force_shrink_file_dropped, 0);
	atomic64_set(&entry->force_shrink_anon_skipped, 0);
	atomic64_set(&entry->force_shrink_file_skipped, 0);
	atomic64_set(&entry->pending_writeback_pages, 0);
	entry->single_policy_raw[0] = '\0';
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM
	memcg_apply_policy_locked(entry);
#endif
	list_add_tail(&entry->node, &memcg_list);
	atomic64_inc(&chs.stats.memcg_entries);

out:
	mutex_unlock(&memcg_lock);
	return entry;
}

static int memcg_name_show(struct seq_file *m, void *v)
{
	struct crystal_hybridswap_memcg *entry;

	entry = crystal_hybridswap_memcg_get(seq_css(m), false);
	if (!entry)
		return -EPERM;

	seq_printf(m, "%s\n", entry->name);
	return 0;
}

static ssize_t memcg_name_write(struct kernfs_open_file *of, char *buf,
				size_t nbytes, loff_t off)
{
	struct crystal_hybridswap_memcg *entry;
	char *name;

	entry = crystal_hybridswap_memcg_get(of_css(of), true);
	if (!entry)
		return -ENOMEM;

	name = strim(buf);
	mutex_lock(&memcg_lock);
	strscpy(entry->name, name[0] ? name : "unknown", sizeof(entry->name));
	mutex_unlock(&memcg_lock);
	atomic64_inc(&chs.stats.memcg_param_updates);

	return nbytes;
}

static int memcg_app_score_write(struct cgroup_subsys_state *css,
				 struct cftype *cft, s64 val)
{
	struct crystal_hybridswap_memcg *entry;

	if (val < 0 || val > CHS_MAX_APP_SCORE)
		return -EINVAL;

	entry = crystal_hybridswap_memcg_get(css, true);
	if (!entry)
		return -ENOMEM;

	atomic64_set(&entry->app_score, val);
	memcg_apply_policy(entry);
	atomic64_inc(&chs.stats.memcg_param_updates);
	return 0;
}

static s64 memcg_app_score_read(struct cgroup_subsys_state *css,
				struct cftype *cft)
{
	struct crystal_hybridswap_memcg *entry;

	entry = crystal_hybridswap_memcg_get(css, false);
	if (!entry)
		return -EPERM;

	return atomic64_read(&entry->app_score);
}

static int memcg_app_uid_write(struct cgroup_subsys_state *css,
			       struct cftype *cft, s64 val)
{
	struct crystal_hybridswap_memcg *entry;

	if (val < 0)
		return -EINVAL;

	entry = crystal_hybridswap_memcg_get(css, true);
	if (!entry)
		return -ENOMEM;

	atomic64_set(&entry->app_uid, val);
	atomic64_inc(&chs.stats.memcg_param_updates);
	return 0;
}

static s64 memcg_app_uid_read(struct cgroup_subsys_state *css,
			      struct cftype *cft)
{
	struct crystal_hybridswap_memcg *entry;

	entry = crystal_hybridswap_memcg_get(css, false);
	if (!entry)
		return -EPERM;

	return atomic64_read(&entry->app_uid);
}

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_EMPTY_APIS
static int memcg_ub_ufs2zram_ratio_write(struct cgroup_subsys_state *css,
					 struct cftype *cft, s64 val)
{
	struct crystal_hybridswap_memcg *entry;

	if (val < CHS_MIN_RATIO || val > CHS_MAX_RATIO)
		return -EINVAL;

	entry = crystal_hybridswap_memcg_get(css, true);
	if (!entry)
		return -ENOMEM;

	atomic64_set(&entry->ub_ufs2zram_ratio, val);
	atomic64_inc(&chs.stats.memcg_param_updates);
	return 0;
}

static s64 memcg_ub_ufs2zram_ratio_read(struct cgroup_subsys_state *css,
					struct cftype *cft)
{
	struct crystal_hybridswap_memcg *entry;

	entry = crystal_hybridswap_memcg_get(css, false);
	if (!entry)
		return -EPERM;

	return atomic64_read(&entry->ub_ufs2zram_ratio);
}
#endif

static int memcg_force_swapin_write(struct cgroup_subsys_state *css,
				    struct cftype *cft, s64 val)
{
	struct crystal_hybridswap_memcg *entry;
	s64 request = val;
	s64 request_pages = val > 0 ? val : 1;
	s64 effective_pages = CHS_FORCE_GLOBAL_SCAN_PAGES;
	s64 app_score;
	int ret;

	entry = crystal_hybridswap_memcg_get(css, true);
	if (!entry)
		return -ENOMEM;

	app_score = atomic64_read(&entry->app_score);
	atomic64_inc(&entry->force_swapin);
	atomic64_add(request_pages, &entry->force_swapin_pages);
	atomic64_set(&entry->force_swapin_last_request, request);
	atomic64_set(&entry->force_swapin_last_pages, request_pages);
	atomic64_set(&entry->force_swapin_last_effective_pages,
		       effective_pages);
	atomic64_set(&entry->force_swapin_last_batchin_pages, 0);
	atomic64_set(&entry->force_swapin_last_ret, 0);
	atomic64_set(&entry->force_swapin_last_read_errors, 0);
	atomic64_set(&entry->force_swapin_last_prepare_errors, 0);
	crystal_hybridswap_record_force_swapin(entry->name, entry->cgroup_id,
					       app_score, request, request_pages,
					       effective_pages);

	ret = crystal_hybridswap_queue_force_swapin(entry->cgroup_id,
		entry->name, app_score, request, request_pages, effective_pages);
	if (ret) {
		atomic64_set(&entry->force_swapin_last_ret, ret);
		crystal_hybridswap_report_pressure(CHS_PRESSURE_CRITICAL,
						   "force_swapin_error");
	}

	return 0;
}

static int memcg_force_swapout_write(struct cgroup_subsys_state *css,
				     struct cftype *cft, s64 val)
{
	struct crystal_hybridswap_memcg *entry;
	s64 compat_trigger_value = val;
	s64 app_score;
	int ret;

	entry = crystal_hybridswap_memcg_get(css, true);
	if (!entry)
		return -ENOMEM;

	app_score = atomic64_read(&entry->app_score);
	atomic64_inc(&entry->force_swapout);
	atomic64_set(&entry->force_swapout_last_mb, compat_trigger_value);
	atomic64_set(&entry->force_swapout_last_pages, 0);
	atomic64_set(&entry->force_swapout_last_effective_pages, 0);
	atomic64_set(&entry->force_swapout_last_compat_trigger_value,
		     compat_trigger_value);
	atomic64_set(&entry->force_swapout_last_scanned_pages, 0);
	atomic64_set(&entry->force_swapout_last_eligible_pages, 0);
	atomic64_set(&entry->force_swapout_last_written_pages, 0);
	atomic64_set(&entry->force_swapout_last_unknown_or_filtered_pages, 0);
	atomic64_set(&entry->force_swapout_last_ret, 0);
	crystal_hybridswap_record_force_swapout(entry->name, entry->cgroup_id,
						app_score, compat_trigger_value);

	ret = crystal_hybridswap_queue_force_swapout(entry->cgroup_id,
						   entry->name, app_score,
						   compat_trigger_value);
	if (ret)
		crystal_hybridswap_report_pressure(CHS_PRESSURE_CRITICAL,
						   "force_swapout_error");

	return 0;
}

void crystal_hybridswap_memcg_record_force_swapout_result(u64 cgroup_id,
		unsigned long scanned_pages, unsigned long eligible_pages,
		unsigned long written_pages,
		unsigned long unknown_or_filtered_pages, int ret)
{
	struct crystal_hybridswap_memcg *entry;

	mutex_lock(&memcg_lock);
	list_for_each_entry(entry, &memcg_list, node) {
		if (entry->cgroup_id != cgroup_id)
			continue;

		atomic64_add(written_pages, &entry->force_swapout_pages);
		atomic64_set(&entry->force_swapout_effective_pages, written_pages);
		atomic64_set(&entry->force_swapout_last_pages, written_pages);
		atomic64_set(&entry->force_swapout_last_effective_pages,
			     written_pages);
		atomic64_set(&entry->force_swapout_last_scanned_pages,
			     scanned_pages);
		atomic64_set(&entry->force_swapout_last_eligible_pages,
			     eligible_pages);
		atomic64_set(&entry->force_swapout_last_written_pages,
			     written_pages);
		atomic64_set(&entry->force_swapout_last_unknown_or_filtered_pages,
			     unknown_or_filtered_pages);
		atomic64_set(&entry->force_swapout_last_ret, ret);
		break;
	}
	mutex_unlock(&memcg_lock);
}

void crystal_hybridswap_memcg_record_force_swapin_result(u64 cgroup_id,
		unsigned long scanned_pages, unsigned long matched_pages,
		unsigned long moved_pages, unsigned long skipped_pages,
		unsigned long filtered_pages, unsigned long read_errors,
		unsigned long prepare_errors, unsigned long snapshot_mismatch,
		int ret)
{
	struct crystal_hybridswap_memcg *entry;

	mutex_lock(&memcg_lock);
	list_for_each_entry(entry, &memcg_list, node) {
		if (entry->cgroup_id != cgroup_id)
			continue;

		atomic64_set(&entry->force_swapin_last_batchin_pages, moved_pages);
		atomic64_set(&entry->force_swapin_last_effective_pages, moved_pages);
		atomic64_set(&entry->force_swapin_last_ret, ret);
		atomic64_set(&entry->force_swapin_last_read_errors, read_errors);
		atomic64_set(&entry->force_swapin_last_prepare_errors, prepare_errors);
		chs_log_ratelimited(CHS_LOG_DEBUG,
			"force_swapin memcg result id=%llu name=%s scanned_pages=%lu matched_pages=%lu moved_pages=%lu skipped_pages=%lu filtered_pages=%lu read_errors=%lu prepare_errors=%lu snapshot_mismatch_pages=%lu ret=%d\n",
			cgroup_id, entry->name, scanned_pages, matched_pages,
			moved_pages, skipped_pages, filtered_pages, read_errors,
			prepare_errors, snapshot_mismatch, ret);
		break;
	}
	mutex_unlock(&memcg_lock);
}

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM
static u64 memcg_auto_candidate_weight(struct crystal_hybridswap_memcg *entry,
					      unsigned int ratio)
{
	s64 score = atomic64_read(&entry->app_score);
	s64 last_ret = atomic64_read(&entry->force_swapout_last_ret);
	s64 last_eligible = atomic64_read(&entry->force_swapout_last_eligible_pages);
	u64 weight;

	if (score < 0)
		score = 0;
	if (score > CHS_MAX_APP_SCORE)
		score = CHS_MAX_APP_SCORE;

	/* Higher app score usually means colder/background work. */
	weight = (u64)ratio * (u64)(score + 1);
	if (last_ret == -ENODATA && !last_eligible)
		weight = max_t(u64, 1, weight / 4);
	else if (last_eligible > 0)
		weight += ratio;

	return weight;
}
#endif

int crystal_hybridswap_memcg_collect_auto_candidates(
		struct crystal_hybridswap_auto_memcg_candidate *candidates,
		int max_candidates, s64 total_budget_pages)
{
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM
	struct crystal_hybridswap_memcg *entry;
	u64 weights[CHS_AUTO_MEMCG_MAX_CANDIDATES] = { 0 };
	u64 total_weight = 0;
	int count = 0;
	int i;
#endif

	if (!candidates || max_candidates <= 0 || total_budget_pages <= 0)
		return 0;

	max_candidates = min_t(int, max_candidates, CHS_AUTO_MEMCG_MAX_CANDIDATES);
	memset(candidates, 0, sizeof(*candidates) * max_candidates);

#ifndef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM
	return 0;
#else
	mutex_lock(&memcg_lock);
	list_for_each_entry(entry, &memcg_list, node) {
		struct crystal_hybridswap_auto_memcg_candidate cand;
		unsigned int ratio = atomic_read(&entry->ub_zram2ufs_ratio);
		int policy_level = atomic_read(&entry->policy_level);
		u64 weight;
		int pos;

		if (policy_level == CHS_POLICY_LEVEL_NONE || !ratio)
			continue;

		weight = memcg_auto_candidate_weight(entry, ratio);
		if (!weight)
			continue;

		memset(&cand, 0, sizeof(cand));
		cand.cgroup_id = entry->cgroup_id;
		cand.app_score = atomic64_read(&entry->app_score);
		cand.zram2ufs_ratio = ratio;
		cand.policy_level = policy_level;
		strscpy(cand.name, entry->name, sizeof(cand.name));

		for (pos = 0; pos < count; pos++) {
			if (weight > weights[pos])
				break;
		}
		if (pos >= max_candidates)
			continue;
		if (count < max_candidates)
			count++;
		for (i = count - 1; i > pos; i--) {
			candidates[i] = candidates[i - 1];
			weights[i] = weights[i - 1];
		}
		candidates[pos] = cand;
		weights[pos] = weight;
	}

	for (i = 0; i < count; i++)
		total_weight += weights[i];
	if (total_weight) {
		u64 remaining = total_budget_pages;
		u64 remaining_weight = total_weight;
		u64 max_budget = CHS_AUTO_MEMCG_MAX_WRITEBACK_MB *
			CHS_PAGES_PER_MB;

		for (i = 0; i < count; i++) {
			u64 budget;

			if (!remaining || !remaining_weight) {
				candidates[i].budget_pages = 0;
				continue;
			}

			budget = mul_u64_u64_div_u64(remaining, weights[i],
				remaining_weight);
			if (!budget)
				budget = 1;
			budget = min_t(u64, budget, max_budget);
			budget = min_t(u64, budget, remaining);
			candidates[i].budget_pages = budget;
			remaining -= budget;
			remaining_weight -= weights[i];
		}
	}
	mutex_unlock(&memcg_lock);

	return count;
#endif
}

static ssize_t memcg_force_shrink_anon(struct kernfs_open_file *of, char *buf,
				       size_t nbytes, loff_t off)
{
	return memcg_force_shrink_write(of, buf, nbytes, false);
}

static ssize_t memcg_force_shrink_file(struct kernfs_open_file *of, char *buf,
				       size_t nbytes, loff_t off)
{
	return memcg_force_shrink_write(of, buf, nbytes, true);
}

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_EMPTY_APIS
static ssize_t memcg_aging_anon(struct kernfs_open_file *of, char *buf,
				size_t nbytes, loff_t off)
{
	struct crystal_hybridswap_memcg *entry;
	s64 param = memcg_parse_compat_s64(buf);

	entry = crystal_hybridswap_memcg_get(of_css(of), true);
	if (!entry)
		return -ENOMEM;

	atomic64_inc(&entry->aging_anon);
	atomic64_set(&entry->aging_anon_last_param, param);
	atomic64_inc(&chs.stats.aging_anon);
	atomic64_set(&chs.stats.aging_anon_last_param, param);
	return nbytes;
}
#endif

static int memcg_swap_stat_show(struct seq_file *m, void *v)
{
	struct crystal_hybridswap_memcg *entry;
	struct crystal_hybridswap_memcg_zram_stats zram_stats;
	s64 out_pages;
	s64 in_pages;
	s64 zram_compressed_kb = 0;
	s64 zram_original_kb = 0;
	s64 eswap_compressed_kb = 0;
	s64 eswap_original_kb = 0;
	s64 eswap_cur_kb = 0;
	int zram_ret;

	entry = crystal_hybridswap_memcg_get(seq_css(m), false);
	if (!entry)
		return -EPERM;

	out_pages = atomic64_read(&entry->force_swapout_pages);
	in_pages = atomic64_read(&entry->force_swapin_pages);
	zram_ret = crystal_hybridswap_collect_memcg_zram_stats(entry->cgroup_id,
		&zram_stats);
	if (!zram_ret) {
		zram_compressed_kb = zram_stats.zram_compressed_size >> 10;
		zram_original_kb = zram_stats.zram_original_size >> 10;
		eswap_compressed_kb = zram_stats.writeback_size >> 10;
		eswap_original_kb = zram_stats.writeback_original_size >> 10;
		eswap_cur_kb = zram_stats.writeback_pages << (PAGE_SHIFT - 10);
	}

	seq_printf(m, "%-32s %12lld KB\n", "zramCompressedSize:",
		   zram_compressed_kb);
	seq_printf(m, "%-32s %12lld KB\n", "zramOrignalSize:",
		   zram_original_kb);
	seq_printf(m, "%-32s %12lld KB\n", "eswapCompressedSize:",
		   eswap_compressed_kb);
	seq_printf(m, "%-32s %12lld KB\n", "eswapOrignalSize:",
		   eswap_original_kb);
	seq_printf(m, "%-32s %12lld\n", "eswapOutTotal:",
		   atomic64_read(&entry->force_swapout));
	seq_printf(m, "%-32s %12lld KB\n", "eswapOutSize:",
		   out_pages << (PAGE_SHIFT - 10));
	seq_printf(m, "%-32s %12lld\n", "eswapInTotal:",
		   atomic64_read(&entry->force_swapin));
	seq_printf(m, "%-32s %12lld KB\n", "eswapInSize:",
		   in_pages << (PAGE_SHIFT - 10));
	seq_printf(m, "%-32s %12lld\n", "pageInTotal:",
		   atomic64_read(&entry->force_swapin_last_batchin_pages));
	seq_printf(m, "%-32s %12lld KB\n", "eswapSizeCur:", eswap_cur_kb);
	seq_printf(m, "%-32s %12lld KB\n", "eswapSizeMax:",
		   eswap_cur_kb);
	seq_printf(m, "%-32s %12lld\n", "appScore:",
		   atomic64_read(&entry->app_score));
	seq_printf(m, "%-32s %12lld\n", "appUid:",
		   atomic64_read(&entry->app_uid));
	seq_printf(m, "%-32s %12d\n", "policyLevel:",
		   atomic_read(&entry->policy_level));
	seq_printf(m, "%-32s %12u\n", "ubMem2zramRatio:",
		   atomic_read(&entry->ub_mem2zram_ratio));
	seq_printf(m, "%-32s %12u\n", "ubZram2ufsRatio:",
		   atomic_read(&entry->ub_zram2ufs_ratio));
	seq_printf(m, "%-32s %12lld\n", "ubUfs2zramRatio:",
		   atomic64_read(&entry->ub_ufs2zram_ratio));
	seq_printf(m, "%-32s %12s\n", "ubUfs2zramRatioApi:",
		   CHS_LEGACY_EMPTY_APIS_VISIBILITY);
	seq_printf(m, "%-32s %12u\n", "refaultThreshold:",
		   atomic_read(&entry->refault_threshold));
	seq_printf(m, "%-32s %12s\n", "batchoutStatus:",
		   "private_zram_per_memcg_best_effort");
	seq_printf(m, "%-32s %12lld\n", "forceSwapoutCompatTriggerValue:",
		   atomic64_read(&entry->force_swapout_last_compat_trigger_value));
	seq_printf(m, "%-32s %12lld\n", "forceSwapoutLastPages:",
		   atomic64_read(&entry->force_swapout_last_pages));
	seq_printf(m, "%-32s %12lld\n", "forceSwapoutEffectivePages:",
		   atomic64_read(&entry->force_swapout_last_effective_pages));
	seq_printf(m, "%-32s %12lld\n", "forceSwapoutScannedPages:",
		   atomic64_read(&entry->force_swapout_last_scanned_pages));
	seq_printf(m, "%-32s %12lld\n", "forceSwapoutEligiblePages:",
		   atomic64_read(&entry->force_swapout_last_eligible_pages));
	seq_printf(m, "%-32s %12lld\n", "forceSwapoutWrittenPages:",
		   atomic64_read(&entry->force_swapout_last_written_pages));
	seq_printf(m, "%-32s %12lld\n", "forceSwapoutUnknownOrFiltered:",
		   atomic64_read(&entry->force_swapout_last_unknown_or_filtered_pages));
	seq_printf(m, "%-32s %12lld\n", "forceSwapoutUnknownOrFilteredPages:",
		   atomic64_read(&entry->force_swapout_last_unknown_or_filtered_pages));
	seq_printf(m, "%-32s %12lld\n", "forceSwapoutLastRet:",
		   atomic64_read(&entry->force_swapout_last_ret));
	seq_printf(m, "%-32s %12lld\n", "forceSwapinLastRequest:",
		   atomic64_read(&entry->force_swapin_last_request));
	seq_printf(m, "%-32s %12lld\n", "forceSwapinRequestedPages:",
		   atomic64_read(&entry->force_swapin_last_pages));
	seq_printf(m, "%-32s %12lld\n", "forceSwapinLastPages:",
		   atomic64_read(&entry->force_swapin_last_pages));
	seq_printf(m, "%-32s %12lld\n", "forceSwapinEffectivePages:",
		   atomic64_read(&entry->force_swapin_last_effective_pages));
	seq_printf(m, "%-32s %12lld\n", "forceSwapinMovedPages:",
		   atomic64_read(&entry->force_swapin_last_batchin_pages));
	seq_printf(m, "%-32s %12lld\n", "forceSwapinBatchinPages:",
		   atomic64_read(&entry->force_swapin_last_batchin_pages));
	seq_printf(m, "%-32s %12lld\n", "forceSwapinLastRet:",
		   atomic64_read(&entry->force_swapin_last_ret));
	seq_printf(m, "%-32s %12lld\n", "zramResidentPages:",
		   zram_ret ? 0 : zram_stats.resident_pages);
	seq_printf(m, "%-32s %12lld\n", "zramWritebackPages:",
		   zram_ret ? 0 : zram_stats.writeback_pages);
	seq_printf(m, "%-32s %12lld\n", "zramStatsDevicesScanned:",
		   zram_ret ? 0 : zram_stats.devices_scanned);
	seq_printf(m, "%-32s %12lld\n", "zramStatsDevicesWithData:",
		   zram_ret ? 0 : zram_stats.devices_with_data);
	seq_printf(m, "%-32s %12d\n", "zramStatsRet:", zram_ret);
	return 0;
}

static void *memcg_total_info_per_app_start(struct seq_file *m, loff_t *pos)
{
	struct crystal_hybridswap_memcg *snapshot;
	int ret;

	if (*pos == 0)
		return SEQ_START_TOKEN;

	snapshot = kmalloc(sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot)
		return ERR_PTR(-ENOMEM);

	ret = memcg_snapshot_at(snapshot, *pos - 1);
	if (ret) {
		kfree(snapshot);
		return ret == -ENOENT ? NULL : ERR_PTR(ret);
	}

	return snapshot;
}

static void *memcg_total_info_per_app_next(struct seq_file *m, void *v,
		loff_t *pos)
{
	struct crystal_hybridswap_memcg *snapshot = v;
	int ret;

	++*pos;
	if (v == SEQ_START_TOKEN)
		return memcg_total_info_per_app_start(m, pos);

	ret = memcg_snapshot_at(snapshot, *pos - 1);
	if (ret) {
		kfree(snapshot);
		return ret == -ENOENT ? NULL : ERR_PTR(ret);
	}

	return snapshot;
}

static void memcg_total_info_per_app_stop(struct seq_file *m, void *v)
{
	if (!v || v == SEQ_START_TOKEN || IS_ERR(v))
		return;

	kfree(v);
}

static int memcg_total_info_per_app_show(struct seq_file *m, void *v)
{
	struct crystal_hybridswap_memcg *entry = v;
	struct crystal_hybridswap_memcg_zram_stats zram_stats;
	s64 zram_compressed_kb = 0;
	s64 zram_original_kb = 0;
	s64 eswap_compressed_kb = 0;
	s64 eswap_original_kb = 0;
	int zram_ret;

	if (v == SEQ_START_TOKEN) {
		seq_printf(m, "%-8s %-8s %-8s %-8s %-8s %-24s %-8s %-8s %-8s %-8s %-8s %-10s %-12s %-12s %-12s %-8s\n",
			   "anon", "zram_c", "zram_p", "eswap_c", "eswap_p",
			   "memcg_n", "score", "uid", "policy", "out_trig",
			   "in_mb", "in_request", "in_req_pages",
			   "in_eff_pages", "in_moved_pages", "in_ret");
		return 0;
	}

	zram_ret = crystal_hybridswap_collect_memcg_zram_stats(entry->cgroup_id,
		&zram_stats);
	if (!zram_ret) {
		zram_compressed_kb = zram_stats.zram_compressed_size >> 10;
		zram_original_kb = zram_stats.zram_original_size >> 10;
		eswap_compressed_kb = zram_stats.writeback_size >> 10;
		eswap_original_kb = zram_stats.writeback_original_size >> 10;
	}
	seq_printf(m, "%-8d %-8lld %-8lld %-8lld %-8lld %-24s %-8lld %-8lld %-8d %-8lld %-8lld %-10lld %-12lld %-12lld %-12lld %-8lld\n",
		   0, zram_compressed_kb, zram_original_kb,
		   eswap_compressed_kb, eswap_original_kb,
		   entry->name,
		   atomic64_read(&entry->app_score),
		   atomic64_read(&entry->app_uid),
		   atomic_read(&entry->policy_level),
		   atomic64_read(&entry->force_swapout_last_mb),
		   memcg_pages_to_mb(atomic64_read(&entry->force_swapin_last_pages)),
		   atomic64_read(&entry->force_swapin_last_request),
		   atomic64_read(&entry->force_swapin_last_pages),
		   atomic64_read(&entry->force_swapin_last_effective_pages),
		   atomic64_read(&entry->force_swapin_last_batchin_pages),
		   atomic64_read(&entry->force_swapin_last_ret));

	return 0;
}

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_EMPTY_APIS
static int memcg_psi_show(struct seq_file *m, void *v)
{
	seq_puts(m, "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
	seq_puts(m, "full avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
	seq_printf(m, "policy_parse_success=%lld policy_parse_error=%lld\n",
		   atomic64_read(&chs.stats.memcg_policy_parse_success),
		   atomic64_read(&chs.stats.memcg_policy_parse_error));
	seq_printf(m, "single_policy_parse_success=%lld single_policy_parse_error=%lld\n",
		   atomic64_read(&chs.stats.memcg_single_policy_parse_success),
		   atomic64_read(&chs.stats.memcg_single_policy_parse_error));
	return 0;
}
#endif

static int memcg_zram_wm_ratio_write(struct cgroup_subsys_state *css,
				     struct cftype *cft, s64 val)
{
	return crystal_hybridswap_set_zram_wm_ratio(val);
}

static s64 memcg_zram_wm_ratio_read(struct cgroup_subsys_state *css,
				    struct cftype *cft)
{
	return crystal_hybridswap_zram_wm_ratio();
}

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_EMPTY_APIS
static s64 swapd_pid_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	return -1;
}
#endif

static ssize_t avail_buffers_write(struct kernfs_open_file *of, char *buf,
				   size_t nbytes, loff_t off)
{
	unsigned int avail, min_avail, high_avail;
	unsigned long long free_swap_threshold;

	buf = strim(buf);
	if (sscanf(buf, "%u %u %u %llu", &avail, &min_avail, &high_avail,
		   &free_swap_threshold) != 4)
		return -EINVAL;

	atomic_set(&swapd_avail_buffers, avail);
	atomic_set(&swapd_min_avail_buffers, min_avail);
	atomic_set(&swapd_high_avail_buffers, high_avail);
	atomic64_set(&swapd_free_swap_threshold, free_swap_threshold);
	atomic64_inc(&chs.stats.memcg_param_updates);
	atomic64_inc(&chs.stats.avail_buffers_writes);
	crystal_hybridswap_queue_policy_wakeup(avail, min_avail, high_avail,
						 free_swap_threshold);
	return nbytes;
}

static int avail_buffers_show(struct seq_file *m, void *v)
{
	seq_printf(m, "avail_buffers: %u\n",
		   atomic_read(&swapd_avail_buffers));
	seq_printf(m, "min_avail_buffers: %u\n",
		   atomic_read(&swapd_min_avail_buffers));
	seq_printf(m, "high_avail_buffers: %u\n",
		   atomic_read(&swapd_high_avail_buffers));
	seq_printf(m, "free_swap_threshold: %llu\n",
		   (unsigned long long)atomic64_read(&swapd_free_swap_threshold));
	return 0;
}

static int swapd_policy_stat_show(struct seq_file *m, void *v)
{
	char auto_reason[CHS_PRESSURE_REASON_MAX];
	s64 last_free_swap_pages =
		atomic64_read(&chs.stats.avail_buffers_last_free_swap_pages);

	crystal_hybridswap_copy_last_auto_reason(auto_reason,
						    sizeof(auto_reason));

	seq_printf(m, "writes: %lld\n",
		   atomic64_read(&chs.stats.avail_buffers_writes));
	seq_printf(m, "wakeups: %lld\n",
		   atomic64_read(&chs.stats.avail_buffers_wakeups));
	seq_printf(m, "pending_policy_wakeups: %lld\n",
		   atomic64_read(&chs.pending_policy_wakeups));
	seq_printf(m, "policy_wakeups: %lld\n",
		   atomic64_read(&chs.stats.policy_wakeups));
	seq_printf(m, "policy_worker_runs: %lld\n",
		   atomic64_read(&chs.stats.policy_worker_runs));
	seq_printf(m, "policy_writeback_queued: %lld\n",
		   atomic64_read(&chs.stats.policy_writeback_queued));
	seq_printf(m, "policy_writeback_skipped: %lld\n",
		   atomic64_read(&chs.stats.policy_writeback_skipped));
	seq_printf(m, "auto_policy_runs: %lld\n",
		   atomic64_read(&chs.stats.auto_policy_runs));
	seq_printf(m, "auto_writeback_queued: %lld\n",
		   atomic64_read(&chs.stats.auto_writeback_queued));
	seq_printf(m, "auto_writeback_skipped: %lld\n",
		   atomic64_read(&chs.stats.auto_writeback_skipped));
	seq_printf(m, "last_auto_jiffies: %lld\n",
		   atomic64_read(&chs.stats.last_auto_jiffies));
	seq_printf(m, "last_auto_reason: %s\n", auto_reason);
	seq_printf(m, "last_auto_writeback_jiffies: %lld\n",
		   atomic64_read(&chs.stats.last_auto_writeback_jiffies));
	seq_printf(m, "last_auto_writeback_ret: %lld\n",
		   atomic64_read(&chs.stats.last_auto_writeback_ret));
	seq_printf(m, "last_auto_writeback_written_pages: %lld\n",
		   atomic64_read(&chs.stats.last_auto_writeback_written_pages));
	seq_printf(m, "policy_available_android_pages: %lld\n",
		   atomic64_read(&chs.stats.policy_available_android_pages));
	seq_printf(m, "policy_available_android_mb: %lld\n",
		   atomic64_read(&chs.stats.policy_available_android_mb));
	seq_printf(m, "policy_available_fallback: %lld\n",
		   atomic64_read(&chs.stats.policy_available_fallback));
	seq_printf(m, "zram_pressure_ratio: %lld\n",
		   atomic64_read(&chs.stats.policy_zram_pressure_ratio));
	seq_printf(m, "zram_effective_ratio: %lld\n",
		   atomic64_read(&chs.stats.policy_zram_effective_ratio));
	seq_printf(m, "zram_wm_ratio: %lld\n",
		   atomic64_read(&chs.stats.policy_zram_wm_ratio));
	seq_printf(m, "zram_gate_result: %lld\n",
		   atomic64_read(&chs.stats.policy_zram_gate_result));
	seq_printf(m, "zram_resident_pages: %lld\n",
		   atomic64_read(&chs.stats.policy_zram_resident_pages));
	seq_printf(m, "zram_total_pages: %lld\n",
		   atomic64_read(&chs.stats.policy_zram_total_pages));
	seq_printf(m, "zram_increase_pages: %lld\n",
		   atomic64_read(&chs.stats.policy_zram_increase_pages));
	seq_printf(m, "zram_increase_boost_pages: %lld\n",
		   atomic64_read(&chs.stats.policy_zram_increase_boost_pages));
	seq_printf(m, "zram_increase_budget_pages: %lld\n",
		   atomic64_read(&chs.stats.policy_zram_increase_budget_pages));
	seq_printf(m, "empty_backoff_interval_ms: %lld\n",
		   atomic64_read(&chs.stats.policy_empty_backoff_interval_ms));
	seq_printf(m, "empty_backoff_skipped: %lld\n",
		   atomic64_read(&chs.stats.policy_empty_backoff_skipped));
	seq_printf(m, "empty_rounds: %lld\n",
		   atomic64_read(&chs.stats.policy_empty_rounds));
	seq_printf(m, "window_start_jiffies: %lld\n",
		   atomic64_read(&chs.stats.policy_window_start_jiffies));
	seq_printf(m, "window_written_pages: %lld\n",
		   atomic64_read(&chs.stats.policy_window_written_pages));
	seq_printf(m, "window_throttled: %lld\n",
		   atomic64_read(&chs.stats.policy_window_throttled));
	seq_printf(m, "writeback_quota_used_pages: %lld\n",
		   atomic64_read(&chs.stats.writeback_quota_used_pages));
	seq_printf(m, "writeback_quota_remaining_pages: %lld\n",
		   atomic64_read(&chs.stats.writeback_quota_remaining_pages));
	seq_printf(m, "writeback_quota_skipped: %lld\n",
		   atomic64_read(&chs.stats.writeback_quota_skipped));
	seq_printf(m, "writeback_quota_capped: %lld\n",
		   atomic64_read(&chs.stats.writeback_quota_capped));
	seq_printf(m, "writeback_quota_resets: %lld\n",
		   atomic64_read(&chs.stats.writeback_quota_resets));
	seq_printf(m, "dev_life_level: %lld\n",
		   atomic64_read(&chs.stats.dev_life_level));
	seq_printf(m, "dev_life_auto_scaled: %lld\n",
		   atomic64_read(&chs.stats.dev_life_auto_scaled));
	seq_printf(m, "dev_life_quota_scaled: %lld\n",
		   atomic64_read(&chs.stats.dev_life_quota_scaled));
	seq_printf(m, "dev_life_force_soft_bypass: %lld\n",
		   atomic64_read(&chs.stats.dev_life_force_soft_bypass));
	seq_printf(m, "auto_memcg_candidate_count: %lld\n",
		   atomic64_read(&chs.stats.auto_memcg_candidate_count));
	seq_printf(m, "auto_per_memcg_queued: %lld\n",
		   atomic64_read(&chs.stats.auto_per_memcg_queued));
	seq_printf(m, "auto_per_memcg_success: %lld\n",
		   atomic64_read(&chs.stats.auto_per_memcg_success));
	seq_printf(m, "auto_per_memcg_no_data: %lld\n",
		   atomic64_read(&chs.stats.auto_per_memcg_no_data));
	seq_printf(m, "auto_per_memcg_error: %lld\n",
		   atomic64_read(&chs.stats.auto_per_memcg_error));
	seq_printf(m, "auto_global_fallback: %lld\n",
		   atomic64_read(&chs.stats.auto_global_fallback));
	seq_printf(m, "normal_interval_ms: %u\n",
		   CHS_AUTO_POLICY_NORMAL_INTERVAL_MS);
	seq_printf(m, "low_interval_ms: %u\n",
		   CHS_AUTO_POLICY_LOW_INTERVAL_MS);
	seq_printf(m, "min_writeback_interval_ms: %u\n",
		   CHS_AUTO_POLICY_MIN_WRITEBACK_INTERVAL_MS);
	seq_printf(m, "window_ms: %u\n", CHS_AUTO_POLICY_WINDOW_MS);
	seq_printf(m, "window_max_mb: %u\n",
		   CHS_AUTO_POLICY_WINDOW_MAX_WRITEBACK_MB);
	seq_printf(m, "low_events: %lld\n",
		   atomic64_read(&chs.stats.avail_buffers_low_events));
	seq_printf(m, "high_events: %lld\n",
		   atomic64_read(&chs.stats.avail_buffers_high_events));
	seq_printf(m, "swap_low_events: %lld\n",
		   atomic64_read(&chs.stats.avail_buffers_swap_low_events));
	seq_printf(m, "last_avail_buffers_mb: %lld\n",
		   atomic64_read(&chs.stats.avail_buffers_last_avail));
	seq_printf(m, "last_min_avail_buffers_mb: %lld\n",
		   atomic64_read(&chs.stats.avail_buffers_last_min));
	seq_printf(m, "last_high_avail_buffers_mb: %lld\n",
		   atomic64_read(&chs.stats.avail_buffers_last_high));
	seq_printf(m, "last_free_swap_threshold_mb: %lld\n",
		   atomic64_read(&chs.stats.avail_buffers_last_free_swap_threshold));
	seq_printf(m, "last_seen_avail_mb: %lld\n",
		   atomic64_read(&chs.stats.avail_buffers_last_seen_avail));
	seq_printf(m, "last_free_swap_pages: %lld\n", last_free_swap_pages);
	seq_printf(m, "last_free_swap_mb: %lld\n",
		   memcg_pages_to_mb(last_free_swap_pages));
	return 0;
}

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM
static int parse_swapd_memcgs_param(char *buf)
{
	struct crystal_hybridswap_swapd_param parsed[CHS_SWAPD_MAX_LEVEL_NUM];
	char raw[CHS_POLICY_RAW_MAX];
	unsigned int level_num;
	char *token;
	int levels = 0;
	int i;
	bool parse_error = false;

	memcg_copy_raw(raw, buf);
	memset(parsed, 0, sizeof(parsed));
	buf = strim(buf);
	token = memcg_next_token(&buf);
	if (!token || memcg_parse_uint_token(token, &level_num)) {
		parse_error = true;
		level_num = 0;
	} else if (level_num > CHS_SWAPD_MAX_LEVEL_NUM) {
		parse_error = true;
		level_num = CHS_SWAPD_MAX_LEVEL_NUM;
	}

	for (i = 0; i < level_num; i++) {
		unsigned int min_score, max_score;
		unsigned int mem2zram, zram2ufs, refault;

		token = memcg_next_token(&buf);
		if (!token || memcg_parse_uint_token(token, &min_score)) {
			parse_error = true;
			break;
		}
		token = memcg_next_token(&buf);
		if (!token || memcg_parse_uint_token(token, &max_score)) {
			parse_error = true;
			break;
		}
		token = memcg_next_token(&buf);
		if (!token || memcg_parse_uint_token(token, &mem2zram)) {
			parse_error = true;
			break;
		}
		token = memcg_next_token(&buf);
		if (!token || memcg_parse_uint_token(token, &zram2ufs)) {
			parse_error = true;
			break;
		}
		token = memcg_next_token(&buf);
		if (!token || memcg_parse_uint_token(token, &refault)) {
			parse_error = true;
			break;
		}

		min_score = memcg_clamp_uint(min_score, CHS_MAX_APP_SCORE,
					     &parse_error);
		max_score = memcg_clamp_uint(max_score, CHS_MAX_APP_SCORE,
					     &parse_error);
		mem2zram = memcg_clamp_uint(mem2zram, CHS_MAX_RATIO,
					    &parse_error);
		zram2ufs = memcg_clamp_uint(zram2ufs, CHS_MAX_RATIO,
					    &parse_error);
		if (min_score > max_score) {
			swap(min_score, max_score);
			parse_error = true;
		}

		parsed[i].min_score = min_score;
		parsed[i].max_score = max_score;
		parsed[i].ub_mem2zram_ratio = mem2zram;
		parsed[i].ub_zram2ufs_ratio = zram2ufs;
		parsed[i].refault_threshold = refault;
		levels++;
	}

	if (level_num != levels || memcg_next_token(&buf))
		parse_error = true;

	if (parse_error) {
		atomic64_inc(&chs.stats.memcg_policy_parse_error);
		return -EINVAL;
	}

	mutex_lock(&memcg_lock);
	strscpy(swapd_memcgs_param_raw, raw, sizeof(swapd_memcgs_param_raw));
	memset(swapd_params, 0, sizeof(swapd_params));
	memcpy(swapd_params, parsed, sizeof(parsed));
	swapd_param_levels = levels;
	memcg_apply_policies_locked();
	mutex_unlock(&memcg_lock);

	atomic64_inc(&chs.stats.memcg_param_updates);
	atomic64_inc(&chs.stats.memcg_policy_parse_success);

	return 0;
}

static ssize_t swapd_memcgs_param_write(struct kernfs_open_file *of, char *buf,
					size_t nbytes, loff_t off)
{
	int ret = parse_swapd_memcgs_param(buf);

	return ret ? ret : nbytes;
}

static int swapd_memcgs_param_show(struct seq_file *m, void *v)
{
	int i;

	mutex_lock(&memcg_lock);
	for (i = 0; i < CHS_SWAPD_MAX_LEVEL_NUM; i++) {
		seq_printf(m, "level %d min score: %u\n", i,
			   swapd_params[i].min_score);
		seq_printf(m, "level %d max score: %u\n", i,
			   swapd_params[i].max_score);
		seq_printf(m, "level %d ub_mem2zram_ratio: %u\n", i,
			   swapd_params[i].ub_mem2zram_ratio);
		seq_printf(m, "level %d ub_zram2ufs_ratio: %u\n", i,
			   swapd_params[i].ub_zram2ufs_ratio);
		seq_printf(m, "memcg %d refault_threshold: %u\n", i,
			   swapd_params[i].refault_threshold);
	}
	seq_printf(m, "levels: %d\n", swapd_param_levels);
	seq_printf(m, "raw: %s\n", swapd_memcgs_param_raw);
	seq_printf(m, "parse_success: %lld\n",
		   atomic64_read(&chs.stats.memcg_policy_parse_success));
	seq_printf(m, "parse_error: %lld\n",
		   atomic64_read(&chs.stats.memcg_policy_parse_error));
	mutex_unlock(&memcg_lock);
	return 0;
}

static ssize_t swapd_single_memcg_param_write(struct kernfs_open_file *of,
					      char *buf, size_t nbytes,
					       loff_t off)
{
	struct crystal_hybridswap_memcg *entry;
	unsigned int vals[3] = { 0, 0, 0 };
	char raw[CHS_POLICY_RAW_MAX];
	char *token;
	int parsed = 0;
	bool parse_error = false;

	entry = crystal_hybridswap_memcg_get(of_css(of), true);
	if (!entry)
		return -ENOMEM;

	memcg_copy_raw(raw, buf);
	buf = strim(buf);
	while ((token = memcg_next_token(&buf)) != NULL) {
		if (parsed >= ARRAY_SIZE(vals) ||
		    memcg_parse_uint_token(token, &vals[parsed])) {
			parse_error = true;
			break;
		}
		parsed++;
	}

	if (parsed != ARRAY_SIZE(vals))
		parse_error = true;

	if (!parse_error) {
		vals[0] = memcg_clamp_uint(vals[0], CHS_MAX_RATIO,
					       &parse_error);
		vals[1] = memcg_clamp_uint(vals[1], CHS_MAX_RATIO,
					       &parse_error);
	}

	if (parse_error) {
		atomic64_inc(&chs.stats.memcg_single_policy_parse_error);
		return -EINVAL;
	}

	mutex_lock(&memcg_lock);
	atomic_set(&entry->ub_mem2zram_ratio, vals[0]);
	atomic_set(&entry->ub_zram2ufs_ratio, vals[1]);
	atomic_set(&entry->refault_threshold, vals[2]);
	atomic_set(&entry->policy_level, CHS_POLICY_LEVEL_MANUAL);
	strscpy(entry->single_policy_raw, raw, sizeof(entry->single_policy_raw));
	mutex_unlock(&memcg_lock);

	atomic64_inc(&chs.stats.memcg_param_updates);
	atomic64_inc(&chs.stats.memcg_single_policy_parse_success);
	return nbytes;
}

static int swapd_single_memcg_param_show(struct seq_file *m, void *v)
{
	struct crystal_hybridswap_memcg *entry;

	entry = crystal_hybridswap_memcg_get(seq_css(m), false);
	if (!entry)
		return -EPERM;

	seq_printf(m, "memcg name: %s\n", entry->name);
	seq_printf(m, "memcg score: %lld\n",
		   atomic64_read(&entry->app_score));
	seq_printf(m, "memcg uid: %lld\n",
		   atomic64_read(&entry->app_uid));
	seq_printf(m, "memcg policy_level: %d\n",
		   atomic_read(&entry->policy_level));
	seq_printf(m, "memcg ub_mem2zram_ratio: %u\n",
		   atomic_read(&entry->ub_mem2zram_ratio));
	seq_printf(m, "memcg ub_zram2ufs_ratio: %u\n",
		   atomic_read(&entry->ub_zram2ufs_ratio));
	seq_printf(m, "memcg ub_ufs2zram_ratio: %lld\n",
		   atomic64_read(&entry->ub_ufs2zram_ratio));
	seq_printf(m, "memcg ub_ufs2zram_ratio_api: %s\n",
		   CHS_LEGACY_EMPTY_APIS_VISIBILITY);
	seq_printf(m, "memcg refault_threshold: %u\n",
		   atomic_read(&entry->refault_threshold));
	seq_printf(m, "raw: %s\n", entry->single_policy_raw);
	return 0;
}
#endif

void crystal_hybridswap_memcg_stats_show(struct seq_file *m)
{
	struct crystal_hybridswap_memcg *snapshots;
	int count;
	int i;

	mutex_lock(&memcg_lock);
	seq_printf(m, "memcg_legacy_empty_apis: %s (%s)\n",
		   CHS_LEGACY_EMPTY_APIS_STATE,
		   CHS_LEGACY_EMPTY_APIS_VISIBILITY);
	seq_printf(m, "memcg_legacy_empty_apis_list: %s\n",
		   CHS_LEGACY_EMPTY_APIS_LIST);
	seq_printf(m, "memcg_legacy_swapd_memcgs_param: %s (%s)\n",
		   CHS_LEGACY_SWAPD_MEMCGS_PARAM_STATE,
		   CHS_LEGACY_SWAPD_MEMCGS_PARAM_VISIBILITY);
	seq_printf(m, "memcg_legacy_swapd_memcgs_param_list: %s\n",
		   CHS_LEGACY_SWAPD_MEMCGS_PARAM_LIST);
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM
	seq_printf(m, "memcg_policy_levels: %d\n", swapd_param_levels);
	seq_printf(m, "memcg_policy_raw: %s\n", swapd_memcgs_param_raw);
#else
	seq_puts(m, "memcg_policy_levels: 0\n");
	seq_puts(m, "memcg_policy_raw:\n");
#endif
	seq_puts(m, "zram_wm_ratio_api: memory.zram_wm_ratio controls zram watermark, range=0..100, no runtime min clamp\n");
	seq_printf(m, "zram_wm_ratio: %lld\n",
		   crystal_hybridswap_zram_wm_ratio());
	seq_printf(m,
		   "avail_buffers: avail_mb=%u min_mb=%u high_mb=%u free_swap_threshold_mb=%llu writes=%lld wakeups=%lld policy_wakeups=%lld worker_runs=%lld low=%lld high=%lld swap_low=%lld last_seen_avail_mb=%lld last_free_swap_pages=%lld last_free_swap_mb=%lld\n",
		   atomic_read(&swapd_avail_buffers),
		   atomic_read(&swapd_min_avail_buffers),
		   atomic_read(&swapd_high_avail_buffers),
		   (unsigned long long)atomic64_read(&swapd_free_swap_threshold),
		   atomic64_read(&chs.stats.avail_buffers_writes),
		   atomic64_read(&chs.stats.avail_buffers_wakeups),
		   atomic64_read(&chs.stats.policy_wakeups),
		   atomic64_read(&chs.stats.policy_worker_runs),
		   atomic64_read(&chs.stats.avail_buffers_low_events),
		   atomic64_read(&chs.stats.avail_buffers_high_events),
		   atomic64_read(&chs.stats.avail_buffers_swap_low_events),
		   atomic64_read(&chs.stats.avail_buffers_last_seen_avail),
		   atomic64_read(&chs.stats.avail_buffers_last_free_swap_pages),
		   memcg_pages_to_mb(atomic64_read(
		   &chs.stats.avail_buffers_last_free_swap_pages)));
	seq_printf(m,
		   "auto_policy: runs=%lld queued=%lld skipped=%lld last_jiffies=%lld last_writeback_jiffies=%lld last_ret=%lld last_written=%lld android_avail_pages=%lld android_avail_mb=%lld fallback=%lld zram_ratio=%lld zram_effective_ratio=%lld zram_wm_ratio=%lld zram_gate=%lld zram_resident=%lld zram_total=%lld zram_increase=%lld zram_increase_boost=%lld zram_increase_budget=%lld quota_used_pages=%lld quota_remaining_pages=%lld quota_skipped=%lld quota_capped=%lld quota_resets=%lld dev_life_level=%lld dev_life_auto_scaled=%lld dev_life_quota_scaled=%lld dev_life_force_soft_bypass=%lld empty_rounds=%lld empty_backoff_ms=%lld window_start=%lld window_written=%lld window_throttled=%lld candidates=%lld per_memcg_queued=%lld per_memcg_success=%lld per_memcg_no_data=%lld per_memcg_error=%lld global_fallback=%lld normal_ms=%u low_ms=%u min_writeback_ms=%u window_ms=%u window_max_mb=%u\n",
		   atomic64_read(&chs.stats.auto_policy_runs),
		   atomic64_read(&chs.stats.auto_writeback_queued),
		   atomic64_read(&chs.stats.auto_writeback_skipped),
		   atomic64_read(&chs.stats.last_auto_jiffies),
		   atomic64_read(&chs.stats.last_auto_writeback_jiffies),
		   atomic64_read(&chs.stats.last_auto_writeback_ret),
		   atomic64_read(&chs.stats.last_auto_writeback_written_pages),
		   atomic64_read(&chs.stats.policy_available_android_pages),
		   atomic64_read(&chs.stats.policy_available_android_mb),
		   atomic64_read(&chs.stats.policy_available_fallback),
		   atomic64_read(&chs.stats.policy_zram_pressure_ratio),
		   atomic64_read(&chs.stats.policy_zram_effective_ratio),
		   atomic64_read(&chs.stats.policy_zram_wm_ratio),
		   atomic64_read(&chs.stats.policy_zram_gate_result),
		   atomic64_read(&chs.stats.policy_zram_resident_pages),
		   atomic64_read(&chs.stats.policy_zram_total_pages),
		   atomic64_read(&chs.stats.policy_zram_increase_pages),
		   atomic64_read(&chs.stats.policy_zram_increase_boost_pages),
		   atomic64_read(&chs.stats.policy_zram_increase_budget_pages),
		   atomic64_read(&chs.stats.writeback_quota_used_pages),
		   atomic64_read(&chs.stats.writeback_quota_remaining_pages),
		   atomic64_read(&chs.stats.writeback_quota_skipped),
		   atomic64_read(&chs.stats.writeback_quota_capped),
		   atomic64_read(&chs.stats.writeback_quota_resets),
		   atomic64_read(&chs.stats.dev_life_level),
		   atomic64_read(&chs.stats.dev_life_auto_scaled),
		   atomic64_read(&chs.stats.dev_life_quota_scaled),
		   atomic64_read(&chs.stats.dev_life_force_soft_bypass),
		   atomic64_read(&chs.stats.policy_empty_rounds),
		   atomic64_read(&chs.stats.policy_empty_backoff_interval_ms),
		   atomic64_read(&chs.stats.policy_window_start_jiffies),
		   atomic64_read(&chs.stats.policy_window_written_pages),
		   atomic64_read(&chs.stats.policy_window_throttled),
		   atomic64_read(&chs.stats.auto_memcg_candidate_count),
		   atomic64_read(&chs.stats.auto_per_memcg_queued),
		   atomic64_read(&chs.stats.auto_per_memcg_success),
		   atomic64_read(&chs.stats.auto_per_memcg_no_data),
		   atomic64_read(&chs.stats.auto_per_memcg_error),
		   atomic64_read(&chs.stats.auto_global_fallback),
		   CHS_AUTO_POLICY_NORMAL_INTERVAL_MS,
		   CHS_AUTO_POLICY_LOW_INTERVAL_MS,
		   CHS_AUTO_POLICY_MIN_WRITEBACK_INTERVAL_MS,
		   CHS_AUTO_POLICY_WINDOW_MS,
		   CHS_AUTO_POLICY_WINDOW_MAX_WRITEBACK_MB);
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM
	for (i = 0; i < swapd_param_levels; i++) {
		seq_printf(m,
			   "memcg_policy[%d]: min=%u max=%u mem2zram=%u zram2ufs=%u refault=%u\n",
			   i, swapd_params[i].min_score,
			   swapd_params[i].max_score,
			   swapd_params[i].ub_mem2zram_ratio,
			   swapd_params[i].ub_zram2ufs_ratio,
			   swapd_params[i].refault_threshold);
	}
#endif
	mutex_unlock(&memcg_lock);

	count = memcg_collect_snapshots(&snapshots);
	if (count < 0) {
		chs_log_ratelimited(CHS_LOG_WARN,
			"memcg stats snapshot failed ret=%d\n", count);
		return;
	}

	for (i = 0; i < count; i++) {
		struct crystal_hybridswap_memcg *entry = &snapshots[i];
		struct crystal_hybridswap_memcg_zram_stats zram_stats;
		int zram_ret;

		zram_ret = crystal_hybridswap_collect_memcg_zram_stats(
			entry->cgroup_id, &zram_stats);
		seq_printf(m,
			   "memcg: id=%llu name=%s score=%lld uid=%lld policy=%d mem2zram=%u zram2ufs=%u ufs2zram=%lld refault=%u zram_resident=%llu zram_writeback=%llu zram_compressed=%llu zram_original=%llu eswap_compressed=%llu eswap_original=%llu zram_devices_scanned=%llu zram_devices_with_data=%llu zram_scan_ret=%d force_out=%lld force_out_pages=%lld force_out_effective_pages=%lld force_out_compat_trigger=%lld force_out_scanned=%lld force_out_eligible=%lld force_out_written=%lld force_out_unknown_or_filtered=%lld force_out_unknown_or_filtered_pages=%lld force_out_last_ret=%lld force_in=%lld force_in_pages=%lld force_in_last_request=%lld force_in_requested_pages=%lld force_in_effective_pages=%lld force_in_moved_pages=%lld force_in_last_effective_pages=%lld force_in_last_batchin=%lld force_in_last_ret=%lld shrink_anon_req=%lld shrink_anon_pages=%lld shrink_anon_last_target=%lld shrink_anon_last_target_pages=%lld shrink_anon_last_batch=%lld shrink_anon_last_batch_pages=%lld shrink_anon_last_reclaimed=%lld shrink_anon_last_reclaimed_pages=%lld shrink_anon_last_ret=%lld shrink_anon_drop=%lld shrink_anon_skip=%lld shrink_file_req=%lld shrink_file_pages=%lld shrink_file_last_target=%lld shrink_file_last_target_pages=%lld shrink_file_last_batch=%lld shrink_file_last_batch_pages=%lld shrink_file_last_reclaimed=%lld shrink_file_last_reclaimed_pages=%lld shrink_file_last_ret=%lld shrink_file_drop=%lld shrink_file_skip=%lld force_swapout=private_zram_per_memcg_best_effort scope=zram_resident_pages batchin=private_zram_per_memcg_page_level raw=%s\n",
			   entry->cgroup_id, entry->name,
			   atomic64_read(&entry->app_score),
			   atomic64_read(&entry->app_uid),
			   atomic_read(&entry->policy_level),
			   atomic_read(&entry->ub_mem2zram_ratio),
			   atomic_read(&entry->ub_zram2ufs_ratio),
			   atomic64_read(&entry->ub_ufs2zram_ratio),
			   atomic_read(&entry->refault_threshold),
			   zram_ret ? 0 : zram_stats.resident_pages,
			   zram_ret ? 0 : zram_stats.writeback_pages,
			   zram_ret ? 0 : zram_stats.zram_compressed_size,
			   zram_ret ? 0 : zram_stats.zram_original_size,
			   zram_ret ? 0 : zram_stats.writeback_size,
			   zram_ret ? 0 : zram_stats.writeback_original_size,
			   zram_ret ? 0 : zram_stats.devices_scanned,
			   zram_ret ? 0 : zram_stats.devices_with_data,
			   zram_ret,
			   atomic64_read(&entry->force_swapout),
			   atomic64_read(&entry->force_swapout_pages),
			   atomic64_read(&entry->force_swapout_effective_pages),
			   atomic64_read(&entry->force_swapout_last_compat_trigger_value),
			   atomic64_read(&entry->force_swapout_last_scanned_pages),
			   atomic64_read(&entry->force_swapout_last_eligible_pages),
			   atomic64_read(&entry->force_swapout_last_written_pages),
			   atomic64_read(&entry->force_swapout_last_unknown_or_filtered_pages),
			   atomic64_read(&entry->force_swapout_last_unknown_or_filtered_pages),
			   atomic64_read(&entry->force_swapout_last_ret),
			   atomic64_read(&entry->force_swapin),
			   atomic64_read(&entry->force_swapin_pages),
			   atomic64_read(&entry->force_swapin_last_request),
			   atomic64_read(&entry->force_swapin_last_pages),
			   atomic64_read(&entry->force_swapin_last_effective_pages),
			   atomic64_read(&entry->force_swapin_last_batchin_pages),
			   atomic64_read(&entry->force_swapin_last_effective_pages),
			   atomic64_read(&entry->force_swapin_last_batchin_pages),
			   atomic64_read(&entry->force_swapin_last_ret),
			   atomic64_read(&entry->force_shrink_anon),
			   atomic64_read(&entry->force_shrink_anon_pages),
			   atomic64_read(&entry->force_shrink_anon_last_target),
			   atomic64_read(&entry->force_shrink_anon_last_target),
			   atomic64_read(&entry->force_shrink_anon_last_batch),
			   atomic64_read(&entry->force_shrink_anon_last_batch),
			   atomic64_read(&entry->force_shrink_anon_last_reclaimed),
			   atomic64_read(&entry->force_shrink_anon_last_reclaimed),
			   atomic64_read(&entry->force_shrink_anon_last_ret),
			   atomic64_read(&entry->force_shrink_anon_dropped),
			   atomic64_read(&entry->force_shrink_anon_skipped),
			   atomic64_read(&entry->force_shrink_file),
			   atomic64_read(&entry->force_shrink_file_pages),
			   atomic64_read(&entry->force_shrink_file_last_target),
			   atomic64_read(&entry->force_shrink_file_last_target),
			   atomic64_read(&entry->force_shrink_file_last_batch),
			   atomic64_read(&entry->force_shrink_file_last_batch),
			   atomic64_read(&entry->force_shrink_file_last_reclaimed),
			   atomic64_read(&entry->force_shrink_file_last_reclaimed),
			   atomic64_read(&entry->force_shrink_file_last_ret),
			   atomic64_read(&entry->force_shrink_file_dropped),
			   atomic64_read(&entry->force_shrink_file_skipped),
			   entry->single_policy_raw);
	}
	kfree(snapshots);
}

static struct cftype crystal_hybridswap_memcg_files[] = {
	{ .name = "force_shrink_anon", .write = memcg_force_shrink_anon },
	{ .name = "force_shrink_file", .write = memcg_force_shrink_file },
	{
		.name = "total_info_per_app",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.seq_start = memcg_total_info_per_app_start,
		.seq_next = memcg_total_info_per_app_next,
		.seq_stop = memcg_total_info_per_app_stop,
		.seq_show = memcg_total_info_per_app_show,
	},
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_EMPTY_APIS
	{ .name = "aging_anon", .write = memcg_aging_anon },
#endif
	{ .name = "swap_stat", .seq_show = memcg_swap_stat_show },
	{
		.name = "name",
		.write = memcg_name_write,
		.seq_show = memcg_name_show,
	},
	{
		.name = "app_score",
		.write_s64 = memcg_app_score_write,
		.read_s64 = memcg_app_score_read,
	},
	{
		.name = "app_uid",
		.write_s64 = memcg_app_uid_write,
		.read_s64 = memcg_app_uid_read,
	},
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_EMPTY_APIS
	{
		.name = "ub_ufs2zram_ratio",
		.write_s64 = memcg_ub_ufs2zram_ratio_write,
		.read_s64 = memcg_ub_ufs2zram_ratio_read,
	},
#endif
	{ .name = "force_swapin", .write_s64 = memcg_force_swapin_write },
	{ .name = "force_swapout", .write_s64 = memcg_force_swapout_write },
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_EMPTY_APIS
	{
		.name = "psi",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.seq_show = memcg_psi_show,
	},
#endif
	{
		.name = "zram_wm_ratio",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = memcg_zram_wm_ratio_write,
		.read_s64 = memcg_zram_wm_ratio_read,
	},
	{
		.name = "swapd_pressure",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write = crystal_hybridswap_swapd_pressure_write,
	},
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_EMPTY_APIS
	{
		.name = "swapd_pid",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.read_s64 = swapd_pid_read,
	},
#endif
	{
		.name = "avail_buffers",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write = avail_buffers_write,
		.seq_show = avail_buffers_show,
	},
	{
		.name = "swapd_policy_stat",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.seq_show = swapd_policy_stat_show,
	},
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM
	{
		.name = "swapd_memcgs_param",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write = swapd_memcgs_param_write,
		.seq_show = swapd_memcgs_param_show,
	},
	{
		.name = "swapd_single_memcg_param",
		.write = swapd_single_memcg_param_write,
		.seq_show = swapd_single_memcg_param_show,
	},
#endif
	{ }
};

int crystal_hybridswap_memcg_init(void)
{
	int ret;

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM
	swapd_memcgs_param_raw[0] = '\0';
	swapd_param_levels = 0;
#endif
	atomic_set(&swapd_avail_buffers, 0);
	atomic_set(&swapd_min_avail_buffers, 0);
	atomic_set(&swapd_high_avail_buffers, 0);
	atomic64_set(&swapd_free_swap_threshold, 0);
	atomic_set(&force_shrink_anon_queued, 0);
	atomic_set(&force_shrink_file_queued, 0);
	WRITE_ONCE(force_shrink_anon_hook_registered, false);
	memcg_css_offline_hook_registered = false;

	ret = register_trace_android_vh_mem_cgroup_css_offline(
			memcg_css_offline, NULL);
	if (ret) {
		chs_log(CHS_LOG_WARN,
			"memcg css_offline hook unavailable ret=%d, offline entries will be released at module exit\n",
			ret);
	} else {
		memcg_css_offline_hook_registered = true;
		chs_log(CHS_LOG_INFO, "memcg css_offline hook registered\n");
	}

	ret = register_trace_android_vh_tune_scan_type(
			memcg_force_shrink_tune_scan_type, NULL);
	if (ret) {
		chs_log(CHS_LOG_WARN,
			"force_shrink_anon tune_scan_type hook unavailable ret=%d, use best-effort anon reclaim\n",
			ret);
	} else {
		WRITE_ONCE(force_shrink_anon_hook_registered, true);
		atomic64_inc(&chs.stats.force_shrink_anon_hook);
		chs_log(CHS_LOG_INFO,
			"force_shrink_anon tune_scan_type hook registered\n");
	}

	ret = cgroup_add_legacy_cftypes(&memory_cgrp_subsys,
					crystal_hybridswap_memcg_files);
	if (ret) {
		if (READ_ONCE(force_shrink_anon_hook_registered)) {
			unregister_trace_android_vh_tune_scan_type(
					memcg_force_shrink_tune_scan_type, NULL);
			WRITE_ONCE(force_shrink_anon_hook_registered, false);
		}
		if (memcg_css_offline_hook_registered) {
			unregister_trace_android_vh_mem_cgroup_css_offline(
					memcg_css_offline, NULL);
			memcg_css_offline_hook_registered = false;
		}
		return ret;
	}

	memcg_cftypes_registered = true;
	return 0;
}

void crystal_hybridswap_memcg_exit(void)
{
	struct crystal_hybridswap_memcg *entry;
	struct crystal_hybridswap_memcg *tmp;

	if (memcg_cftypes_registered) {
		cgroup_rm_cftypes(crystal_hybridswap_memcg_files);
		memcg_cftypes_registered = false;
	}

	if (READ_ONCE(force_shrink_anon_hook_registered)) {
		unregister_trace_android_vh_tune_scan_type(
				memcg_force_shrink_tune_scan_type, NULL);
		WRITE_ONCE(force_shrink_anon_hook_registered, false);
	}

	if (memcg_css_offline_hook_registered) {
		unregister_trace_android_vh_mem_cgroup_css_offline(
				memcg_css_offline, NULL);
		memcg_css_offline_hook_registered = false;
	}

	if (chs.wq)
		flush_workqueue(chs.wq);

	mutex_lock(&memcg_lock);
	list_for_each_entry_safe(entry, tmp, &memcg_list, node)
		memcg_free_entry_locked(entry);
	mutex_unlock(&memcg_lock);
}
