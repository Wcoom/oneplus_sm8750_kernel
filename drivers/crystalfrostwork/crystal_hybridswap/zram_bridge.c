// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "crystal_hybridswap: " fmt

#include <linux/blkdev.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include "crystal_hybridswap_internal.h"

struct crystal_hybridswap_zram *
crystal_hybridswap_find_zram_locked(struct device *dev)
{
	struct crystal_hybridswap_zram *entry;

	list_for_each_entry(entry, &chs.zram_list, node) {
		if (entry->dev == dev)
			return entry;
	}

	return NULL;
}

static ssize_t hybridswap_enable_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE,
			 "hybridswap %s reclaim_in %s swapd %s\n",
			 crystal_hybridswap_enabled() ? "enable" : "disable",
			 crystal_hybridswap_core_enabled() ? "enable" : "disable",
			 crystal_hybridswap_swapd_paused() ? "pause" : "enable");
}

static ssize_t hybridswap_enable_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t len)
{
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;

	crystal_hybridswap_set_enabled(!!val);
	crystal_hybridswap_set_core_enabled(!!val);
	return len;
}
static DEVICE_ATTR_RW(hybridswap_enable);

static ssize_t hybridswap_core_enable_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "hybridswap %s reclaim_in %s\n",
			 crystal_hybridswap_core_enabled() ? "enable" : "disable",
			 crystal_hybridswap_core_enabled() ? "enable" : "disable");
}

static ssize_t hybridswap_core_enable_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t len)
{
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;

	crystal_hybridswap_set_core_enabled(!!val);
	return len;
}
static DEVICE_ATTR_RW(hybridswap_core_enable);

static ssize_t hybridswap_swapd_pause_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 crystal_hybridswap_swapd_paused());
}

static ssize_t hybridswap_swapd_pause_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t len)
{
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;

	crystal_hybridswap_set_swapd_pause(val);
	return len;
}
static DEVICE_ATTR_RW(hybridswap_swapd_pause);

static ssize_t hybridswap_loglevel_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "Hybridswap log level: %d\n",
			 crystal_hybridswap_loglevel());
}

static ssize_t hybridswap_loglevel_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t len)
{
	int level;
	int ret;

	ret = kstrtoint(buf, 0, &level);
	if (ret)
		return ret;
	if (level < 0 || level >= CHS_LOG_MAX) {
		chs_log(CHS_LOG_ERR, "val %d is not valid\n", level);
		return -EINVAL;
	}

	crystal_hybridswap_set_loglevel(level);
	return len;
}
static DEVICE_ATTR_RW(hybridswap_loglevel);

static ssize_t hybridswap_vmstat_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	char last_mode[CHS_WB_MODE_MAX];
	char last_in_memcg[CHS_MEMCG_NAME_MAX];
	char last_out_memcg[CHS_MEMCG_NAME_MAX];
	s64 last_in_score;
	s64 last_out_score;
	s64 last_in_request;
	s64 last_in_pages;
	s64 last_in_effective;
	s64 last_out_effective;
	u64 last_in_id;
	u64 last_out_id;
	s64 last_out_compat;
	s64 last_out_scanned;
	s64 last_out_eligible;
	s64 last_out_written;
	s64 last_out_unknown_or_filtered;
	s64 last_out_ret;
	ssize_t ret = 0;

	crystal_hybridswap_copy_last_writeback_mode(last_mode,
						    sizeof(last_mode));
	mutex_lock(&chs.state_lock);
	strscpy(last_in_memcg, chs.last_force_swapin_memcg,
		sizeof(last_in_memcg));
	strscpy(last_out_memcg, chs.last_force_swapout_memcg,
		sizeof(last_out_memcg));
	last_in_score = chs.last_force_swapin_app_score;
	last_out_score = chs.last_force_swapout_app_score;
	last_in_request = chs.last_force_swapin_request;
	last_in_pages = chs.last_force_swapin_pages;
	last_in_effective = chs.last_force_swapin_effective_pages;
	last_out_effective = chs.last_force_swapout_effective_pages;
	last_in_id = chs.last_force_swapin_cgroup_id;
	last_out_id = chs.last_force_swapout_cgroup_id;
	last_out_compat = chs.last_force_swapout_compat_trigger_value;
	last_out_scanned = chs.last_force_swapout_scanned_pages;
	last_out_eligible = chs.last_force_swapout_eligible_pages;
	last_out_written = chs.last_force_swapout_written_pages;
	last_out_unknown_or_filtered =
		chs.last_force_swapout_unknown_or_filtered_pages;
	last_out_ret = chs.last_force_swapout_ret;
	mutex_unlock(&chs.state_lock);

	ret += sysfs_emit_at(buf, ret, "hybridswap_enabled %d\n",
				 crystal_hybridswap_enabled());
	ret += sysfs_emit_at(buf, ret, "hybridswap_core_enabled %d\n",
				 crystal_hybridswap_core_enabled());
	ret += sysfs_emit_at(buf, ret, "swapd_pause %d\n",
				 crystal_hybridswap_swapd_paused());
	ret += sysfs_emit_at(buf, ret, "quota_day %llu\n",
				 crystal_hybridswap_quota_day());
	ret += sysfs_emit_at(buf, ret, "dev_life_level %u\n",
				 crystal_hybridswap_dev_life_level());
	ret += sysfs_emit_at(buf, ret, "zram_wm_ratio %lld\n",
				 crystal_hybridswap_zram_wm_ratio());
	ret += sysfs_emit_at(buf, ret, "pending_writeback_pages %lld\n",
				 atomic64_read(&chs.pending_writeback_pages));
	ret += sysfs_emit_at(buf, ret, "pending_force_swapout %lld\n",
				 atomic64_read(&chs.pending_force_swapout));
	ret += sysfs_emit_at(buf, ret, "pending_force_swapout_pages %lld\n",
				 atomic64_read(&chs.pending_force_swapout_pages));
	ret += sysfs_emit_at(buf, ret, "pending_force_swapin %lld\n",
				 atomic64_read(&chs.pending_force_swapin));
	ret += sysfs_emit_at(buf, ret, "pending_batchin_pages %lld\n",
				 atomic64_read(&chs.pending_batchin_pages));
	ret += sysfs_emit_at(buf, ret, "force_swapin %lld\n",
				 atomic64_read(&chs.stats.force_swapin));
	ret += sysfs_emit_at(buf, ret, "force_swapin_pages %lld\n",
				 atomic64_read(&chs.stats.force_swapin_pages));
	ret += sysfs_emit_at(buf, ret, "force_swapin_queued %lld\n",
				 atomic64_read(&chs.stats.force_swapin_queued));
	ret += sysfs_emit_at(buf, ret, "force_swapin_scheduled %lld\n",
				 atomic64_read(&chs.stats.force_swapin_scheduled));
	ret += sysfs_emit_at(buf, ret, "force_swapin_success %lld\n",
				 atomic64_read(&chs.stats.force_swapin_success));
	ret += sysfs_emit_at(buf, ret, "force_swapin_error %lld\n",
				 atomic64_read(&chs.stats.force_swapin_error));
	ret += sysfs_emit_at(buf, ret, "force_swapin_skipped %lld\n",
				 atomic64_read(&chs.stats.force_swapin_skipped));
	ret += sysfs_emit_at(buf, ret, "force_swapin_no_data %lld\n",
				 atomic64_read(&chs.stats.force_swapin_no_data));
	ret += sysfs_emit_at(buf, ret, "force_swapin_requested_pages %lld\n",
				 atomic64_read(&chs.stats.force_swapin_last_request_pages));
	ret += sysfs_emit_at(buf, ret, "force_swapin_effective_pages %lld\n",
				 atomic64_read(&chs.stats.force_swapin_last_effective_pages));
	ret += sysfs_emit_at(buf, ret, "force_swapin_moved_pages %lld\n",
				 atomic64_read(&chs.stats.force_swapin_last_batchin_pages));
	ret += sysfs_emit_at(buf, ret, "force_swapin_last_ret %lld\n",
				 atomic64_read(&chs.stats.force_swapin_last_ret));
	ret += sysfs_emit_at(buf, ret, "last_force_swapin_memcg %s\n",
				 last_in_memcg);
	ret += sysfs_emit_at(buf, ret, "last_force_swapin_cgroup_id %llu\n",
				 last_in_id);
	ret += sysfs_emit_at(buf, ret, "last_force_swapin_app_score %lld\n",
				 last_in_score);
	ret += sysfs_emit_at(buf, ret, "last_force_swapin_request %lld\n",
				 last_in_request);
	ret += sysfs_emit_at(buf, ret, "last_force_swapin_pages %lld\n",
				 last_in_pages);
	ret += sysfs_emit_at(buf, ret, "last_force_swapin_effective_pages %lld\n",
				 last_in_effective);
	ret += sysfs_emit_at(buf, ret, "force_swapout %lld\n",
				 atomic64_read(&chs.stats.force_swapout));
	ret += sysfs_emit_at(buf, ret, "force_swapout_pages %lld\n",
				 atomic64_read(&chs.stats.force_swapout_pages));
	ret += sysfs_emit_at(buf, ret, "force_swapout_effective_pages %lld\n",
				 atomic64_read(&chs.stats.force_swapout_effective_pages));
	ret += sysfs_emit_at(buf, ret, "force_swapout_queued %lld\n",
				 atomic64_read(&chs.stats.force_swapout_queued));
	ret += sysfs_emit_at(buf, ret, "force_swapout_scheduled %lld\n",
				 atomic64_read(&chs.stats.force_swapout_scheduled));
	ret += sysfs_emit_at(buf, ret, "force_swapout_success %lld\n",
				 atomic64_read(&chs.stats.force_swapout_success));
	ret += sysfs_emit_at(buf, ret, "force_swapout_error %lld\n",
				 atomic64_read(&chs.stats.force_swapout_error));
	ret += sysfs_emit_at(buf, ret, "force_swapout_skipped %lld\n",
				 atomic64_read(&chs.stats.force_swapout_skipped));
	ret += sysfs_emit_at(buf, ret, "force_swapout_no_data %lld\n",
				 atomic64_read(&chs.stats.force_swapout_no_data));
	ret += sysfs_emit_at(buf, ret, "force_swapout_last_scanned_pages %lld\n",
				 atomic64_read(&chs.stats.force_swapout_last_scanned_pages));
	ret += sysfs_emit_at(buf, ret, "force_swapout_last_eligible_pages %lld\n",
				 atomic64_read(&chs.stats.force_swapout_last_eligible_pages));
	ret += sysfs_emit_at(buf, ret, "force_swapout_last_written_pages %lld\n",
				 atomic64_read(&chs.stats.force_swapout_last_written_pages));
	ret += sysfs_emit_at(buf, ret,
				 "force_swapout_last_unknown_or_filtered_pages %lld\n",
				 atomic64_read(&chs.stats.force_swapout_last_unknown_or_filtered_pages));
	ret += sysfs_emit_at(buf, ret, "force_swapout_last_ret %lld\n",
				 atomic64_read(&chs.stats.force_swapout_last_ret));
	ret += sysfs_emit_at(buf, ret, "last_force_swapout_memcg %s\n",
				 last_out_memcg);
	ret += sysfs_emit_at(buf, ret, "last_force_swapout_target_cgroup_id %llu\n",
				 last_out_id);
	ret += sysfs_emit_at(buf, ret, "last_force_swapout_app_score %lld\n",
				 last_out_score);
	ret += sysfs_emit_at(buf, ret,
				 "last_force_swapout_compat_trigger_value %lld\n",
				 last_out_compat);
	ret += sysfs_emit_at(buf, ret, "last_force_swapout_scanned_pages %lld\n",
				 last_out_scanned);
	ret += sysfs_emit_at(buf, ret, "last_force_swapout_eligible_pages %lld\n",
				 last_out_eligible);
	ret += sysfs_emit_at(buf, ret, "last_force_swapout_written_pages %lld\n",
				 last_out_written);
	ret += sysfs_emit_at(buf, ret,
				 "last_force_swapout_unknown_or_filtered_pages %lld\n",
				 last_out_unknown_or_filtered);
	ret += sysfs_emit_at(buf, ret, "last_force_swapout_effective_pages %lld\n",
				 last_out_effective);
	ret += sysfs_emit_at(buf, ret, "last_force_swapout_ret %lld\n",
				 last_out_ret);
	ret += sysfs_emit_at(buf, ret, "force_shrink_anon %lld\n",
				 atomic64_read(&chs.stats.force_shrink_anon));
	ret += sysfs_emit_at(buf, ret, "force_shrink_anon_pages %lld\n",
				 atomic64_read(&chs.stats.force_shrink_anon_pages));
	ret += sysfs_emit_at(buf, ret, "force_shrink_anon_last_reclaimed_pages %lld\n",
				 atomic64_read(&chs.stats.force_shrink_anon_last_reclaimed));
	ret += sysfs_emit_at(buf, ret, "force_shrink_anon_last_ret %lld\n",
				 atomic64_read(&chs.stats.force_shrink_anon_last_ret));
	ret += sysfs_emit_at(buf, ret, "force_shrink_file %lld\n",
				 atomic64_read(&chs.stats.force_shrink_file));
	ret += sysfs_emit_at(buf, ret, "force_shrink_file_pages %lld\n",
				 atomic64_read(&chs.stats.force_shrink_file_pages));
	ret += sysfs_emit_at(buf, ret, "force_shrink_file_last_reclaimed_pages %lld\n",
				 atomic64_read(&chs.stats.force_shrink_file_last_reclaimed));
	ret += sysfs_emit_at(buf, ret, "force_shrink_file_last_ret %lld\n",
				 atomic64_read(&chs.stats.force_shrink_file_last_ret));
	ret += sysfs_emit_at(buf, ret, "force_shrink_queued %lld\n",
				 atomic64_read(&chs.stats.force_shrink_queued));
	ret += sysfs_emit_at(buf, ret, "force_shrink_worker_runs %lld\n",
				 atomic64_read(&chs.stats.force_shrink_worker_runs));
	ret += sysfs_emit_at(buf, ret, "writeback_queued %lld\n",
				 atomic64_read(&chs.stats.writeback_queued));
	ret += sysfs_emit_at(buf, ret, "writeback_scheduled %lld\n",
				 atomic64_read(&chs.stats.writeback_scheduled));
	ret += sysfs_emit_at(buf, ret, "writeback_success %lld\n",
				 atomic64_read(&chs.stats.writeback_success));
	ret += sysfs_emit_at(buf, ret, "writeback_error %lld\n",
				 atomic64_read(&chs.stats.writeback_error));
	ret += sysfs_emit_at(buf, ret, "writeback_skipped %lld\n",
				 atomic64_read(&chs.stats.writeback_skipped));
	ret += sysfs_emit_at(buf, ret, "writeback_no_data %lld\n",
				 atomic64_read(&chs.stats.writeback_no_data));
	ret += sysfs_emit_at(buf, ret, "last_writeback_ret %lld\n",
				 atomic64_read(&chs.stats.writeback_last_ret));
	ret += sysfs_emit_at(buf, ret, "last_writeback_pages %lld\n",
				 atomic64_read(&chs.stats.writeback_last_pages));
	ret += sysfs_emit_at(buf, ret, "last_writeback_written_pages %lld\n",
				 atomic64_read(&chs.stats.writeback_last_written_pages));
	ret += sysfs_emit_at(buf, ret, "last_writeback_mode %s\n",
				 last_mode);
	ret += sysfs_emit_at(buf, ret, "policy_wakeups %lld\n",
				 atomic64_read(&chs.stats.policy_wakeups));
	ret += sysfs_emit_at(buf, ret, "policy_worker_runs %lld\n",
				 atomic64_read(&chs.stats.policy_worker_runs));
	ret += sysfs_emit_at(buf, ret, "policy_writeback_queued %lld\n",
				 atomic64_read(&chs.stats.policy_writeback_queued));
	ret += sysfs_emit_at(buf, ret, "policy_writeback_skipped %lld\n",
				 atomic64_read(&chs.stats.policy_writeback_skipped));
	ret += sysfs_emit_at(buf, ret, "memcg_policy_parse_success %lld\n",
				 atomic64_read(&chs.stats.memcg_policy_parse_success));
	ret += sysfs_emit_at(buf, ret, "memcg_policy_parse_error %lld\n",
				 atomic64_read(&chs.stats.memcg_policy_parse_error));

	return ret;
}
static DEVICE_ATTR_RO(hybridswap_vmstat);

static ssize_t hybridswap_crystal_stat_show(struct device *dev,
					    struct device_attribute *attr, char *buf)
{
	char auto_reason[CHS_PRESSURE_REASON_MAX];
	char pressure_reason[CHS_PRESSURE_REASON_MAX];
	struct crystal_hybridswap_zram_io_stats zram_io_stats;
	int zram_io_ret;
	ssize_t ret = 0;

	memset(&zram_io_stats, 0, sizeof(zram_io_stats));
	zram_io_ret = crystal_hybridswap_collect_zram_io_stats(&zram_io_stats);
	crystal_hybridswap_copy_last_auto_reason(auto_reason,
						 sizeof(auto_reason));
	mutex_lock(&chs.state_lock);
	strscpy(pressure_reason, chs.last_pressure_reason,
		sizeof(pressure_reason));
	mutex_unlock(&chs.state_lock);

	ret += sysfs_emit_at(buf, ret,
				 "debugfs_full_stats crystal_hybridswap/stats\n");
	ret += sysfs_emit_at(buf, ret, "writeback_quota_limit_bytes %lld\n",
				 atomic64_read(&chs.stats.writeback_quota_limit_bytes));
	ret += sysfs_emit_at(buf, ret,
				 "writeback_quota_effective_limit_bytes %lld\n",
				 atomic64_read(&chs.stats.writeback_quota_effective_limit_bytes));
	ret += sysfs_emit_at(buf, ret, "writeback_quota_used_pages %lld\n",
				 atomic64_read(&chs.stats.writeback_quota_used_pages));
	ret += sysfs_emit_at(buf, ret, "writeback_quota_remaining_pages %lld\n",
				 atomic64_read(&chs.stats.writeback_quota_remaining_pages));
	ret += sysfs_emit_at(buf, ret, "writeback_quota_skipped %lld\n",
				 atomic64_read(&chs.stats.writeback_quota_skipped));
	ret += sysfs_emit_at(buf, ret, "writeback_quota_capped %lld\n",
				 atomic64_read(&chs.stats.writeback_quota_capped));
	ret += sysfs_emit_at(buf, ret, "writeback_quota_resets %lld\n",
				 atomic64_read(&chs.stats.writeback_quota_resets));
	ret += sysfs_emit_at(buf, ret, "dev_life_auto_scaled %lld\n",
				 atomic64_read(&chs.stats.dev_life_auto_scaled));
	ret += sysfs_emit_at(buf, ret, "dev_life_quota_scaled %lld\n",
				 atomic64_read(&chs.stats.dev_life_quota_scaled));
	ret += sysfs_emit_at(buf, ret, "dev_life_force_soft_bypass %lld\n",
				 atomic64_read(&chs.stats.dev_life_force_soft_bypass));
	ret += sysfs_emit_at(buf, ret, "diagnostic_slow_io_ns %llu\n",
				 (unsigned long long)CHS_ZRAM_SLOW_IO_NS);
	ret += sysfs_emit_at(buf, ret, "diagnostic_slow_work_ns %llu\n",
				 (unsigned long long)CHS_ZRAM_SLOW_WORK_NS);
	ret += sysfs_emit_at(buf, ret, "zram_io_stats_ret %d\n", zram_io_ret);
	ret += sysfs_emit_at(buf, ret, "zram_io_devices_count %llu\n",
				 zram_io_stats.devices_count);
	ret += sysfs_emit_at(buf, ret, "zram_bd_pages %llu\n",
				 zram_io_stats.bd_pages);
	ret += sysfs_emit_at(buf, ret, "zram_bd_compressed_bytes %llu\n",
				 zram_io_stats.bd_compressed_bytes);
	ret += sysfs_emit_at(buf, ret, "zram_bd_read_pages %llu\n",
				 zram_io_stats.bd_read_pages);
	ret += sysfs_emit_at(buf, ret, "zram_bd_write_pages %llu\n",
				 zram_io_stats.bd_write_pages);
	ret += sysfs_emit_at(buf, ret, "zram_bd_read_sync_ios_count %llu\n",
				 zram_io_stats.bd_read_sync_ios_count);
	ret += sysfs_emit_at(buf, ret, "zram_bd_read_async_ios_count %llu\n",
				 zram_io_stats.bd_read_async_ios_count);
	ret += sysfs_emit_at(buf, ret, "zram_bd_read_failures_count %llu\n",
				 zram_io_stats.bd_read_failures_count);
	ret += sysfs_emit_at(buf, ret, "zram_bd_read_last_ret %lld\n",
				 zram_io_stats.bd_read_last_ret);
	ret += sysfs_emit_at(buf, ret, "zram_bd_read_max_ns %llu\n",
				 zram_io_stats.bd_read_max_ns);
	ret += sysfs_emit_at(buf, ret, "zram_bd_read_slow_ios_count %llu\n",
				 zram_io_stats.bd_read_slow_ios_count);
	ret += sysfs_emit_at(buf, ret, "zram_bd_write_ios_count %llu\n",
				 zram_io_stats.bd_write_ios_count);
	ret += sysfs_emit_at(buf, ret, "zram_bd_write_failures_count %llu\n",
				 zram_io_stats.bd_write_failures_count);
	ret += sysfs_emit_at(buf, ret, "zram_bd_write_last_ret %lld\n",
				 zram_io_stats.bd_write_last_ret);
	ret += sysfs_emit_at(buf, ret, "zram_bd_write_max_ns %llu\n",
				 zram_io_stats.bd_write_max_ns);
	ret += sysfs_emit_at(buf, ret, "zram_bd_write_slow_ios_count %llu\n",
				 zram_io_stats.bd_write_slow_ios_count);
	ret += sysfs_emit_at(buf, ret, "zram_batchin_runs_count %llu\n",
				 zram_io_stats.batchin_runs_count);
	ret += sysfs_emit_at(buf, ret, "zram_batchin_pages %llu\n",
				 zram_io_stats.batchin_pages);
	ret += sysfs_emit_at(buf, ret, "zram_batchin_failures_count %llu\n",
				 zram_io_stats.batchin_failures_count);
	ret += sysfs_emit_at(buf, ret, "zram_batchin_no_data_count %llu\n",
				 zram_io_stats.batchin_no_data_count);
	ret += sysfs_emit_at(buf, ret, "zram_batchin_filtered_pages %llu\n",
				 zram_io_stats.batchin_filtered_pages);
	ret += sysfs_emit_at(buf, ret,
				 "zram_batchin_snapshot_mismatch_pages %llu\n",
				 zram_io_stats.batchin_snapshot_mismatch_pages);
	ret += sysfs_emit_at(buf, ret, "zram_batchin_last_ret %lld\n",
				 zram_io_stats.batchin_last_ret);
	ret += sysfs_emit_at(buf, ret, "zram_batchin_zms_batches %llu\n",
				 zram_io_stats.batchin_zms_batches);
	ret += sysfs_emit_at(buf, ret, "zram_batchin_zms_items %llu\n",
				 zram_io_stats.batchin_zms_items);
	ret += sysfs_emit_at(buf, ret, "zram_batchin_zms_read_ios %llu\n",
				 zram_io_stats.batchin_zms_read_ios);
	ret += sysfs_emit_at(buf, ret, "zram_prefetch_runs %llu\n",
				 zram_io_stats.prefetch_runs);
	ret += sysfs_emit_at(buf, ret, "zram_prefetch_moved %llu\n",
				 zram_io_stats.prefetch_moved);
	ret += sysfs_emit_at(buf, ret, "zram_prefetch_hits %llu\n",
				 zram_io_stats.prefetch_hits);
	ret += sysfs_emit_at(buf, ret, "zram_prefetch_hit_pct %llu\n",
				 zram_io_stats.prefetch_hit_pct);
	ret += sysfs_emit_at(buf, ret, "zram_prefetch_invalidated %llu\n",
				 zram_io_stats.prefetch_invalidated);
	ret += sysfs_emit_at(buf, ret, "zram_prefetch_errors %llu\n",
				 zram_io_stats.prefetch_read_errors +
				 zram_io_stats.prefetch_prepare_errors +
				 zram_io_stats.prefetch_snapshot_mismatch +
				 zram_io_stats.prefetch_alloc_failures);
	ret += sysfs_emit_at(buf, ret, "zram_under_wb_waits %llu\n",
				 zram_io_stats.under_wb_waits);
	ret += sysfs_emit_at(buf, ret,
				 "zram_under_wb_wait_total_ns %llu\n",
				 zram_io_stats.under_wb_wait_total_ns);
	ret += sysfs_emit_at(buf, ret, "zram_under_wb_wait_max_ns %llu\n",
				 zram_io_stats.under_wb_wait_max_ns);
	ret += sysfs_emit_at(buf, ret, "zram_io_scan_errors_count %llu\n",
				 zram_io_stats.scan_errors_count);
	ret += sysfs_emit_at(buf, ret, "writeback_worker_runs %lld\n",
				 atomic64_read(&chs.stats.writeback_worker_runs));
	ret += sysfs_emit_at(buf, ret, "writeback_worker_max_ns %lld\n",
				 atomic64_read(&chs.stats.writeback_worker_max_ns));
	ret += sysfs_emit_at(buf, ret, "writeback_worker_slow_runs %lld\n",
				 atomic64_read(&chs.stats.writeback_worker_slow_runs));
	ret += sysfs_emit_at(buf, ret, "writeback_worker_last_ns %lld\n",
				 atomic64_read(&chs.stats.writeback_worker_last_ns));
	ret += sysfs_emit_at(buf, ret, "batchin_worker_runs %lld\n",
				 atomic64_read(&chs.stats.batchin_worker_runs));
	ret += sysfs_emit_at(buf, ret, "batchin_worker_max_ns %lld\n",
				 atomic64_read(&chs.stats.batchin_worker_max_ns));
	ret += sysfs_emit_at(buf, ret, "batchin_worker_slow_runs %lld\n",
				 atomic64_read(&chs.stats.batchin_worker_slow_runs));
	ret += sysfs_emit_at(buf, ret, "batchin_worker_last_ns %lld\n",
				 atomic64_read(&chs.stats.batchin_worker_last_ns));
	ret += sysfs_emit_at(buf, ret, "multi_zram_registered %lld\n",
				 atomic64_read(&chs.stats.multi_zram_registered));
	ret += sysfs_emit_at(buf, ret, "multi_zram_last_selected %lld\n",
				 atomic64_read(&chs.stats.multi_zram_last_selected));
	ret += sysfs_emit_at(buf, ret, "multi_zram_last_traversed %lld\n",
				 atomic64_read(&chs.stats.multi_zram_last_traversed));
	ret += sysfs_emit_at(buf, ret, "multi_zram_last_eligible %lld\n",
				 atomic64_read(&chs.stats.multi_zram_last_eligible));
	ret += sysfs_emit_at(buf, ret, "multi_zram_skip_no_backing %lld\n",
				 atomic64_read(&chs.stats.multi_zram_skip_no_backing));
	ret += sysfs_emit_at(buf, ret, "multi_zram_skip_limit %lld\n",
				 atomic64_read(&chs.stats.multi_zram_skip_limit));
	ret += sysfs_emit_at(buf, ret, "multi_zram_skip_no_resident %lld\n",
				 atomic64_read(&chs.stats.multi_zram_skip_no_resident));
	ret += sysfs_emit_at(buf, ret, "auto_policy_runs %lld\n",
				 atomic64_read(&chs.stats.auto_policy_runs));
	ret += sysfs_emit_at(buf, ret, "auto_writeback_queued %lld\n",
				 atomic64_read(&chs.stats.auto_writeback_queued));
	ret += sysfs_emit_at(buf, ret, "auto_writeback_skipped %lld\n",
				 atomic64_read(&chs.stats.auto_writeback_skipped));
	ret += sysfs_emit_at(buf, ret, "last_auto_writeback_ret %lld\n",
				 atomic64_read(&chs.stats.last_auto_writeback_ret));
	ret += sysfs_emit_at(buf, ret, "last_auto_writeback_written_pages %lld\n",
				 atomic64_read(&chs.stats.last_auto_writeback_written_pages));
	ret += sysfs_emit_at(buf, ret, "last_auto_reason %s\n", auto_reason);
	ret += sysfs_emit_at(buf, ret, "policy_zram_gate_result %lld\n",
				 atomic64_read(&chs.stats.policy_zram_gate_result));
	ret += sysfs_emit_at(buf, ret, "policy_window_throttled %lld\n",
				 atomic64_read(&chs.stats.policy_window_throttled));
	ret += sysfs_emit_at(buf, ret, "pressure_signaled %lld\n",
				 atomic64_read(&chs.stats.pressure_signaled));
	ret += sysfs_emit_at(buf, ret, "pressure_last_ret %lld\n",
				 atomic64_read(&chs.stats.pressure_last_ret));
	ret += sysfs_emit_at(buf, ret, "pressure_last_reason %s\n",
				 pressure_reason);

	return ret;
}
static DEVICE_ATTR_RO(hybridswap_crystal_stat);

static ssize_t hybridswap_report_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE,
			 "Crystal Hybridswap stage-3\n"
			 "data_path=private_zram_writeback_and_batchin\n"
			 "legacy_empty_apis=%s\n"
			 "legacy_empty_apis_visibility=%s\n"
			 "legacy_empty_apis_list=%s\n"
			 "legacy_swapd_memcgs_param=%s\n"
			 "legacy_swapd_memcgs_param_visibility=%s\n"
			 "legacy_swapd_memcgs_param_list=%s\n"
			 "force_swapout=private_zram_per_memcg_best_effort_multi_zram\n"
			 "force_swapout_scope=zram_resident_pages_not_full_ram_anon_reclaim\n"
			 "force_swapout_unknown_identity=skip_and_count_unknown_or_filtered\n"
			 "force_swapin=private_zram_per_memcg_page_level_first; explicit_global_fallback_when_no_target\n"
			 "force_swapin_snapshot_validation=page_level_slot_state_verify_before_commit\n"
			 "force_shrink_file=async_bounded_memcg_reclaim_file_only_no_swap\n"
			 "force_shrink_anon=async_bounded_memcg_reclaim_anon_hook_or_best_effort\n"
			 "fault_out=disabled\n"
			 "writeback_modes=idle,huge,huge_idle,incompressible,page_index=N\n"
			 "internal_writeback_mode=%s\n"
			 "legacy_swapd_policy_formats=%s; swapd_memcgs_param levels+5-tuples; swapd_single_memcg_param 3-tuple\n"
			 "avail_buffers_policy=kernel_delayed_work_auto_MB_config_available_or_free_swap_pages_or_high_watermark_to_zram_writeback\n"
			 "zram_wm_ratio_api=memory.zram_wm_ratio_zram_watermark,old_compatible_range_0_100,no_runtime_min_clamp\n"
			 "auto_policy_controls=daily_quota,dev_life_budget_scale,zram_wm_ratio_gate,zram_increase_boost,empty_no_data_backoff,window_throttle,min_writeback_interval\n"
			 "auto_policy_limits=zram_wm_ratio:%lld window_mb:%u memcg_candidates:%u memcg_max_mb:%u quota_window_ms:%lu dev_life_auto_percent:%u\n"
			 "auto_memcg=legacy_swapd_memcgs_param_%s_policy_enabled_candidates_by_app_score_zram2ufs_ratio_recent_result_then_multi_zram_global_fallback\n"
			 "multi_zram_policy=auto_writeback_pressure_selected force_swapout_memcg_bounded_traverse force_swapin_memcg_writeback_first global_writeback_pages_fallback\n"
			 "diagnostics=backing_read_write_latency batchin_worker writeback_worker slow_logs_rate_limited\n"
			 "force_swapout_units=unknown_or_filtered fields with _pages suffix are page counts; no-suffix names are compatibility aliases\n"
			 "force_swapin_units=memory.force_swapin write value is a trigger/request value; use *_request and *_pages fields for units; total_info_per_app in_mb is compatibility only\n"
			 "force_shrink_units=*_last_target_pages/*_last_batch_pages/*_last_reclaimed_pages are page counts; old no-suffix names are compatibility aliases\n"
			 "zram_bd_stat=standard_3_position_fields_only; Crystal zram IO diagnostics are named zram_* fields in hybridswap_crystal_stat and debugfs stats\n"
			 "diagnostic_slow_io_ns=%llu\n"
			 "diagnostic_slow_work_ns=%llu\n"
			 "ub_ufs2zram_ratio_api=%s_per_memcg_page_level_batchin\n"
			 "not_migrated=legacy_OPPO_extent_rmap_faultout_readback_paths_or_standard_zram_ABI_changes\n"
			 "registered_zram=%lld\n"
			 "memcg_entries=%lld\n"
			 "policy_parse_success=%lld\n"
			 "policy_parse_error=%lld\n",
			 CHS_LEGACY_EMPTY_APIS_STATE,
			 CHS_LEGACY_EMPTY_APIS_VISIBILITY,
			 CHS_LEGACY_EMPTY_APIS_LIST,
			 CHS_LEGACY_SWAPD_MEMCGS_PARAM_STATE,
			 CHS_LEGACY_SWAPD_MEMCGS_PARAM_VISIBILITY,
			 CHS_LEGACY_SWAPD_MEMCGS_PARAM_LIST,
			 CHS_INTERNAL_WB_MODE,
			 CHS_LEGACY_SWAPD_MEMCGS_PARAM_VISIBILITY,
			 crystal_hybridswap_zram_wm_ratio(),
			 CHS_AUTO_POLICY_WINDOW_MAX_WRITEBACK_MB,
			 CHS_AUTO_MEMCG_MAX_CANDIDATES,
			 CHS_AUTO_MEMCG_MAX_WRITEBACK_MB,
			 CHS_QUOTA_WINDOW_MS,
			 CHS_DEV_LIFE_AUTO_BUDGET_PERCENT,
			 CHS_LEGACY_SWAPD_MEMCGS_PARAM_STATE,
			 (unsigned long long)CHS_ZRAM_SLOW_IO_NS,
			 (unsigned long long)CHS_ZRAM_SLOW_WORK_NS,
			 IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_EMPTY_APIS) ?
			 "compat_saved_only" : "hidden",
			 atomic64_read(&chs.stats.zram_register) -
			 atomic64_read(&chs.stats.zram_unregister),
			 atomic64_read(&chs.stats.memcg_entries),
			 atomic64_read(&chs.stats.memcg_policy_parse_success),
			 atomic64_read(&chs.stats.memcg_policy_parse_error));
}
static DEVICE_ATTR_RO(hybridswap_report);

static ssize_t hybridswap_stat_snap_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	char last_mode[CHS_WB_MODE_MAX];
	char last_in_memcg[CHS_MEMCG_NAME_MAX];
	char last_out_memcg[CHS_MEMCG_NAME_MAX];
	s64 last_in_score;
	s64 last_out_score;
	s64 last_in_request;
	s64 last_in_pages;
	s64 last_in_effective;
	s64 last_out_effective;
	u64 last_in_id;
	u64 last_out_id;
	s64 last_out_compat;
	s64 last_out_scanned;
	s64 last_out_eligible;
	s64 last_out_written;
	s64 last_out_unknown_or_filtered;
	s64 last_out_ret;
	ssize_t ret = 0;

	crystal_hybridswap_copy_last_writeback_mode(last_mode,
						    sizeof(last_mode));
	mutex_lock(&chs.state_lock);
	strscpy(last_in_memcg, chs.last_force_swapin_memcg,
		sizeof(last_in_memcg));
	strscpy(last_out_memcg, chs.last_force_swapout_memcg,
		sizeof(last_out_memcg));
	last_in_score = chs.last_force_swapin_app_score;
	last_out_score = chs.last_force_swapout_app_score;
	last_in_request = chs.last_force_swapin_request;
	last_in_pages = chs.last_force_swapin_pages;
	last_in_effective = chs.last_force_swapin_effective_pages;
	last_out_effective = chs.last_force_swapout_effective_pages;
	last_in_id = chs.last_force_swapin_cgroup_id;
	last_out_id = chs.last_force_swapout_cgroup_id;
	last_out_compat = chs.last_force_swapout_compat_trigger_value;
	last_out_scanned = chs.last_force_swapout_scanned_pages;
	last_out_eligible = chs.last_force_swapout_eligible_pages;
	last_out_written = chs.last_force_swapout_written_pages;
	last_out_unknown_or_filtered =
		chs.last_force_swapout_unknown_or_filtered_pages;
	last_out_ret = chs.last_force_swapout_ret;
	mutex_unlock(&chs.state_lock);

	ret += sysfs_emit_at(buf, ret, "hybridswap_enabled %d\n",
				 crystal_hybridswap_enabled());
	ret += sysfs_emit_at(buf, ret, "hybridswap_core_enabled %d\n",
				 crystal_hybridswap_core_enabled());
	ret += sysfs_emit_at(buf, ret, "swapd_pause %d\n",
				 crystal_hybridswap_swapd_paused());
	ret += sysfs_emit_at(buf, ret, "quota_day %llu\n",
				 crystal_hybridswap_quota_day());
	ret += sysfs_emit_at(buf, ret, "dev_life_level %u\n",
				 crystal_hybridswap_dev_life_level());
	ret += sysfs_emit_at(buf, ret, "zram_wm_ratio %lld\n",
				 crystal_hybridswap_zram_wm_ratio());
	ret += sysfs_emit_at(buf, ret, "pending_writeback_pages %lld\n",
				 atomic64_read(&chs.pending_writeback_pages));
	ret += sysfs_emit_at(buf, ret, "pending_force_swapout_pages %lld\n",
				 atomic64_read(&chs.pending_force_swapout_pages));
	ret += sysfs_emit_at(buf, ret, "pending_batchin_pages %lld\n",
				 atomic64_read(&chs.pending_batchin_pages));
	ret += sysfs_emit_at(buf, ret, "force_swapin_pages %lld\n",
				 atomic64_read(&chs.stats.force_swapin_pages));
	ret += sysfs_emit_at(buf, ret, "force_swapin_last_request_pages %lld\n",
				 atomic64_read(&chs.stats.force_swapin_last_request_pages));
	ret += sysfs_emit_at(buf, ret, "force_swapin_last_effective_pages %lld\n",
				 atomic64_read(&chs.stats.force_swapin_last_effective_pages));
	ret += sysfs_emit_at(buf, ret, "force_swapin_last_batchin_pages %lld\n",
				 atomic64_read(&chs.stats.force_swapin_last_batchin_pages));
	ret += sysfs_emit_at(buf, ret, "force_swapin_last_ret %lld\n",
				 atomic64_read(&chs.stats.force_swapin_last_ret));
	ret += sysfs_emit_at(buf, ret, "last_force_swapin_memcg %s\n",
				 last_in_memcg);
	ret += sysfs_emit_at(buf, ret, "last_force_swapin_cgroup_id %llu\n",
				 last_in_id);
	ret += sysfs_emit_at(buf, ret, "last_force_swapin_app_score %lld\n",
				 last_in_score);
	ret += sysfs_emit_at(buf, ret, "last_force_swapin_request %lld\n",
				 last_in_request);
	ret += sysfs_emit_at(buf, ret, "last_force_swapin_pages %lld\n",
				 last_in_pages);
	ret += sysfs_emit_at(buf, ret, "last_force_swapin_effective_pages %lld\n",
				 last_in_effective);
	ret += sysfs_emit_at(buf, ret, "force_swapout_pages %lld\n",
				 atomic64_read(&chs.stats.force_swapout_pages));
	ret += sysfs_emit_at(buf, ret, "force_swapout_effective_pages %lld\n",
				 atomic64_read(&chs.stats.force_swapout_effective_pages));
	ret += sysfs_emit_at(buf, ret, "force_swapout_last_written_pages %lld\n",
				 atomic64_read(&chs.stats.force_swapout_last_written_pages));
	ret += sysfs_emit_at(buf, ret, "force_swapout_last_ret %lld\n",
				 atomic64_read(&chs.stats.force_swapout_last_ret));
	ret += sysfs_emit_at(buf, ret, "last_force_swapout_memcg %s\n",
				 last_out_memcg);
	ret += sysfs_emit_at(buf, ret, "last_force_swapout_target_cgroup_id %llu\n",
				 last_out_id);
	ret += sysfs_emit_at(buf, ret, "last_force_swapout_app_score %lld\n",
				 last_out_score);
	ret += sysfs_emit_at(buf, ret,
				 "last_force_swapout_compat_trigger_value %lld\n",
				 last_out_compat);
	ret += sysfs_emit_at(buf, ret, "last_force_swapout_scanned_pages %lld\n",
				 last_out_scanned);
	ret += sysfs_emit_at(buf, ret, "last_force_swapout_eligible_pages %lld\n",
				 last_out_eligible);
	ret += sysfs_emit_at(buf, ret, "last_force_swapout_written_pages %lld\n",
				 last_out_written);
	ret += sysfs_emit_at(buf, ret,
				 "last_force_swapout_unknown_or_filtered_pages %lld\n",
				 last_out_unknown_or_filtered);
	ret += sysfs_emit_at(buf, ret, "last_force_swapout_effective_pages %lld\n",
				 last_out_effective);
	ret += sysfs_emit_at(buf, ret, "last_force_swapout_ret %lld\n",
				 last_out_ret);
	ret += sysfs_emit_at(buf, ret, "last_writeback_ret %lld\n",
				 atomic64_read(&chs.stats.writeback_last_ret));
	ret += sysfs_emit_at(buf, ret, "last_writeback_pages %lld\n",
				 atomic64_read(&chs.stats.writeback_last_pages));
	ret += sysfs_emit_at(buf, ret, "last_writeback_written_pages %lld\n",
				 atomic64_read(&chs.stats.writeback_last_written_pages));
	ret += sysfs_emit_at(buf, ret, "last_writeback_mode %s\n",
				 last_mode);

	return ret;
}
static DEVICE_ATTR_RO(hybridswap_stat_snap);

static ssize_t hybridswap_meminfo_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct sysinfo i;

	si_meminfo(&i);
	return scnprintf(buf, PAGE_SIZE,
			 "MemTotal: %lu kB\n"
			 "MemFree: %lu kB\n"
			 "MemAvailable: %lu kB\n"
			 "Buffers: %lu kB\n"
			 "HybridswapForceSwapoutPages: %lld\n"
			 "HybridswapForceSwapoutEffectivePages: %lld\n"
			 "HybridswapForceSwapoutLastWrittenPages: %lld\n"
			 "HybridswapForceSwapoutLastScannedPages: %lld\n"
			 "HybridswapForceSwapoutLastEligiblePages: %lld\n"
			 "HybridswapForceSwapoutLastUnknownOrFilteredPages: %lld\n"
			 "HybridswapForceSwapout: private_zram_per_memcg_best_effort\n"
			 "HybridswapForceSwapoutScope: zram_resident_pages_not_full_ram_anon_reclaim\n"
			 "HybridswapForceSwapinPages: %lld\n"
			 "HybridswapForceSwapinLastBatchinPages: %lld\n"
			 "HybridswapForceSwapinLastScannedPages: %lld\n"
			 "HybridswapForceSwapinLastMatchedPages: %lld\n"
			 "HybridswapForceSwapinLastFilteredPages: %lld\n"
			 "HybridswapForceSwapinLastSnapshotMismatch: %lld\n"
			 "HybridswapForceSwapinLastDevices: %lld\n"
			 "HybridswapPendingWritebackPages: %lld\n"
			 "HybridswapPendingBatchinPages: %lld\n"
			 "HybridswapBatchin: private_zram_per_memcg_page_level_first; explicit_global_fallback_when_no_target\n",
			 i.totalram << (PAGE_SHIFT - 10),
			 i.freeram << (PAGE_SHIFT - 10),
			 si_mem_available() << (PAGE_SHIFT - 10),
			 i.bufferram << (PAGE_SHIFT - 10),
			 atomic64_read(&chs.stats.force_swapout_pages),
			 atomic64_read(&chs.stats.force_swapout_effective_pages),
			 atomic64_read(&chs.stats.force_swapout_last_written_pages),
			 atomic64_read(&chs.stats.force_swapout_last_scanned_pages),
			 atomic64_read(&chs.stats.force_swapout_last_eligible_pages),
			 atomic64_read(&chs.stats.force_swapout_last_unknown_or_filtered_pages),
			 atomic64_read(&chs.stats.force_swapin_pages),
			 atomic64_read(&chs.stats.force_swapin_last_batchin_pages),
			 atomic64_read(&chs.stats.force_swapin_last_scanned_pages),
			 atomic64_read(&chs.stats.force_swapin_last_matched_pages),
			 atomic64_read(&chs.stats.force_swapin_last_filtered_pages),
			 atomic64_read(&chs.stats.force_swapin_last_snapshot_mismatch),
			 atomic64_read(&chs.stats.force_swapin_last_devices),
			 atomic64_read(&chs.pending_writeback_pages),
			 atomic64_read(&chs.pending_batchin_pages));
}
static DEVICE_ATTR_RO(hybridswap_meminfo);

static ssize_t hybridswap_loop_device_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	return crystal_hybridswap_get_loop_device(buf);
}

static ssize_t hybridswap_loop_device_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t len)
{
	int ret;

	ret = zram_bind_backing_dev(dev, buf, len);
	crystal_hybridswap_record_loop_device_bind(ret);
	if (ret) {
		chs_log(CHS_LOG_ERR,
			"hybridswap_loop_device backing_dev bind failed ret=%d\n",
			ret);
		return ret;
	}

	ret = crystal_hybridswap_set_loop_device(buf, len);
	if (ret) {
		chs_log(CHS_LOG_ERR,
			"hybridswap_loop_device state update failed ret=%d\n", ret);
		return ret;
	}

	chs_log(CHS_LOG_INFO, "hybridswap_loop_device backing_dev bind success\n");
	return len;
}
static DEVICE_ATTR_RW(hybridswap_loop_device);

static ssize_t hybridswap_dev_life_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 crystal_hybridswap_dev_life() ? "enable" : "disable");
}

static ssize_t hybridswap_dev_life_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t len)
{
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;

	crystal_hybridswap_set_dev_life(val);
	return len;
}
static DEVICE_ATTR_RW(hybridswap_dev_life);

static ssize_t hybridswap_quota_day_show(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%llu\n",
			 crystal_hybridswap_quota_day());
}

static ssize_t hybridswap_quota_day_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t len)
{
	unsigned long long val;
	int ret;

	ret = kstrtoull(buf, 0, &val);
	if (ret)
		return ret;

	crystal_hybridswap_set_quota_day(val);
	return len;
}
static DEVICE_ATTR_RW(hybridswap_quota_day);

static ssize_t hybridswap_zram_increase_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	struct crystal_hybridswap_zram *entry;
	unsigned long value = 0;

	mutex_lock(&chs.zram_lock);
	entry = crystal_hybridswap_find_zram_locked(dev);
	if (entry)
		value = entry->zram_increase_pages >> 8;
	mutex_unlock(&chs.zram_lock);

	return scnprintf(buf, PAGE_SIZE, "%lu\n", value);
}

static ssize_t hybridswap_zram_increase_store(struct device *dev,
					      struct device_attribute *attr,
					      const char *buf, size_t len)
{
	struct crystal_hybridswap_zram *entry;
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;

	mutex_lock(&chs.zram_lock);
	entry = crystal_hybridswap_find_zram_locked(dev);
	if (entry)
		entry->zram_increase_pages = val << 8;
	mutex_unlock(&chs.zram_lock);

	if (!entry)
		return -ENODEV;

	atomic64_inc(&chs.stats.zram_increase_store);
	crystal_hybridswap_update_auto_policy();
	return len;
}
static DEVICE_ATTR_RW(hybridswap_zram_increase);

static struct attribute *crystal_hybridswap_zram_attrs[] = {
	&dev_attr_hybridswap_enable.attr,
	&dev_attr_hybridswap_core_enable.attr,
	&dev_attr_hybridswap_swapd_pause.attr,
	&dev_attr_hybridswap_loglevel.attr,
	&dev_attr_hybridswap_vmstat.attr,
	&dev_attr_hybridswap_crystal_stat.attr,
	&dev_attr_hybridswap_report.attr,
	&dev_attr_hybridswap_stat_snap.attr,
	&dev_attr_hybridswap_meminfo.attr,
	&dev_attr_hybridswap_loop_device.attr,
	&dev_attr_hybridswap_dev_life.attr,
	&dev_attr_hybridswap_quota_day.attr,
	&dev_attr_hybridswap_zram_increase.attr,
	NULL,
};

static const struct attribute_group crystal_hybridswap_zram_group = {
	.attrs = crystal_hybridswap_zram_attrs,
};

int crystal_hybridswap_private_zram_register(struct device *dev,
		struct zram *zram)
{
	struct crystal_hybridswap_zram *entry;
	int ret;

	if (!dev || !zram)
		return -EINVAL;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->dev = dev;
	entry->zram = zram;
	entry->writeback = zram_writeback_device;
	entry->writeback_ext = zram_writeback_device_ext;
	entry->force_writeback = zram_force_writeback_device;
	entry->force_writeback_ext = zram_force_writeback_device_ext;
	entry->batchin = zram_batchin_device;
	entry->registered_jiffies = jiffies;
	INIT_LIST_HEAD(&entry->node);

	mutex_lock(&chs.zram_lock);
	if (crystal_hybridswap_find_zram_locked(dev)) {
		mutex_unlock(&chs.zram_lock);
		kfree(entry);
		return -EEXIST;
	}
	list_add_tail(&entry->node, &chs.zram_list);
	mutex_unlock(&chs.zram_lock);

	ret = sysfs_create_group(&dev->kobj, &crystal_hybridswap_zram_group);
	if (ret) {
		mutex_lock(&chs.zram_lock);
		list_del(&entry->node);
		mutex_unlock(&chs.zram_lock);
		kfree(entry);
		atomic64_inc(&chs.stats.zram_sysfs_errors);
		return ret;
	}

	atomic64_inc(&chs.stats.zram_register);
	crystal_hybridswap_update_auto_policy();
	return 0;
}

void crystal_hybridswap_private_zram_unregister(struct device *dev)
{
	struct crystal_hybridswap_zram *entry;

	if (!dev)
		return;

	crystal_hybridswap_suspend_auto_policy_sync();

	mutex_lock(&chs.zram_lock);
	entry = crystal_hybridswap_find_zram_locked(dev);
	if (entry)
		list_del(&entry->node);
	if (chs.pending_writeback_dev == dev)
		crystal_hybridswap_clear_pending_writeback_locked();
	mutex_unlock(&chs.zram_lock);

	if (chs.wq) {
		flush_work(&chs.writeback_work);
		flush_work(&chs.batchin_work);
	}
	crystal_hybridswap_drain_force_swapout();

	if (!entry) {
		crystal_hybridswap_resume_auto_policy();
		return;
	}

	sysfs_remove_group(&dev->kobj, &crystal_hybridswap_zram_group);
	kfree(entry);
	atomic64_inc(&chs.stats.zram_unregister);
	crystal_hybridswap_resume_auto_policy();
}
