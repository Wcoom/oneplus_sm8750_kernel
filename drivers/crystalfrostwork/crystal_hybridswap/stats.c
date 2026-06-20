// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "crystal_hybridswap: " fmt

#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/seq_file.h>

#include "crystal_hybridswap_internal.h"

void crystal_hybridswap_stats_show(struct seq_file *m)
{
	struct chs_avail_buffer_view avail_buffer_view;
	char last_mode[CHS_WB_MODE_MAX];
	char auto_reason[CHS_PRESSURE_REASON_MAX];
	char last_in_memcg[CHS_MEMCG_NAME_MAX];
	char last_out_memcg[CHS_MEMCG_NAME_MAX];
	struct crystal_hybridswap_zram_io_stats zram_io_stats;
	s64 last_in_score;
	s64 last_out_score;
	s64 last_in_request;
	s64 last_in_pages;
	s64 last_in_effective;
	s64 last_out_effective;
	s64 last_out_compat;
	s64 last_out_scanned;
	s64 last_out_eligible;
	s64 last_out_written;
	s64 last_out_unknown_or_filtered;
	s64 last_out_ret;
	u64 last_in_id;
	u64 last_out_id;
	int zram_io_ret;

	avail_buffer_view.base_min =
		atomic64_read(&chs.stats.avail_buffers_last_min);
	avail_buffer_view.base_high =
		atomic64_read(&chs.stats.avail_buffers_last_high);
	chs_update_avail_buffer_view(&avail_buffer_view);
	memset(&zram_io_stats, 0, sizeof(zram_io_stats));
	zram_io_ret = crystal_hybridswap_collect_zram_io_stats(&zram_io_stats);

	crystal_hybridswap_copy_last_writeback_mode(last_mode,
						    sizeof(last_mode));
	crystal_hybridswap_copy_last_auto_reason(auto_reason,
						 sizeof(auto_reason));

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
	last_out_compat = chs.last_force_swapout_compat_trigger_value;
	last_out_scanned = chs.last_force_swapout_scanned_pages;
	last_out_eligible = chs.last_force_swapout_eligible_pages;
	last_out_written = chs.last_force_swapout_written_pages;
	last_out_unknown_or_filtered =
		chs.last_force_swapout_unknown_or_filtered_pages;
	last_out_ret = chs.last_force_swapout_ret;
	last_in_id = chs.last_force_swapin_cgroup_id;
	last_out_id = chs.last_force_swapout_cgroup_id;
	mutex_unlock(&chs.state_lock);

	seq_printf(m, "enabled: %d\n", crystal_hybridswap_enabled());
	seq_printf(m, "core_enabled: %d\n", crystal_hybridswap_core_enabled());
	seq_printf(m, "legacy_empty_apis_enabled: %d\n",
		   CHS_LEGACY_EMPTY_APIS_ENABLED);
	seq_printf(m, "legacy_empty_apis_state: %s\n",
		   CHS_LEGACY_EMPTY_APIS_STATE);
	seq_printf(m, "legacy_empty_apis_visibility: %s\n",
		   CHS_LEGACY_EMPTY_APIS_VISIBILITY);
	seq_printf(m, "legacy_empty_apis_list: %s\n",
		   CHS_LEGACY_EMPTY_APIS_LIST);
	seq_printf(m, "legacy_swapd_memcgs_param_enabled: %d\n",
		   CHS_LEGACY_SWAPD_MEMCGS_PARAM_ENABLED);
	seq_printf(m, "legacy_swapd_memcgs_param_state: %s\n",
		   CHS_LEGACY_SWAPD_MEMCGS_PARAM_STATE);
	seq_printf(m, "legacy_swapd_memcgs_param_visibility: %s\n",
		   CHS_LEGACY_SWAPD_MEMCGS_PARAM_VISIBILITY);
	seq_printf(m, "legacy_swapd_memcgs_param_list: %s\n",
		   CHS_LEGACY_SWAPD_MEMCGS_PARAM_LIST);
	seq_printf(m, "swapd_pause: %d\n", crystal_hybridswap_swapd_paused());
	seq_printf(m, "loglevel: %d\n", crystal_hybridswap_loglevel());
	seq_printf(m, "quota_day: %llu\n", crystal_hybridswap_quota_day());
	seq_printf(m, "dev_life_level: %u\n",
		   crystal_hybridswap_dev_life_level());
	seq_puts(m, "zram_wm_ratio_api: memory.zram_wm_ratio controls zram watermark, range=0..100, no runtime min clamp\n");
	seq_printf(m, "zram_wm_ratio: %lld\n",
		   crystal_hybridswap_zram_wm_ratio());
	seq_printf(m, "pending_policy_wakeups: %lld\n",
		   atomic64_read(&chs.pending_policy_wakeups));
	seq_printf(m, "pending_writeback_pages: %lld\n",
		   atomic64_read(&chs.pending_writeback_pages));
	seq_printf(m, "pending_force_swapout: %lld\n",
		   atomic64_read(&chs.pending_force_swapout));
	seq_printf(m, "pending_force_swapout_pages: %lld\n",
		   atomic64_read(&chs.pending_force_swapout_pages));
	seq_printf(m, "pending_force_swapin: %lld\n",
		   atomic64_read(&chs.pending_force_swapin));
	seq_printf(m, "pending_batchin_pages: %lld\n",
		   atomic64_read(&chs.pending_batchin_pages));
	seq_printf(m, "zram_register: %lld\n",
		   atomic64_read(&chs.stats.zram_register));
	seq_printf(m, "zram_unregister: %lld\n",
		   atomic64_read(&chs.stats.zram_unregister));
	seq_printf(m, "zram_sysfs_errors: %lld\n",
		   atomic64_read(&chs.stats.zram_sysfs_errors));
	seq_printf(m, "zram_io_stats_ret: %d\n", zram_io_ret);
	seq_printf(m, "zram_io_devices_count: %llu\n",
		   zram_io_stats.devices_count);
	seq_printf(m, "zram_io_last_device_index: %lld\n",
		   zram_io_stats.last_device_index);
	seq_printf(m, "zram_bd_pages: %llu\n", zram_io_stats.bd_pages);
	seq_printf(m, "zram_bd_compressed_bytes: %llu\n",
		   zram_io_stats.bd_compressed_bytes);
	seq_printf(m, "zram_bd_read_pages: %llu\n",
		   zram_io_stats.bd_read_pages);
	seq_printf(m, "zram_bd_write_pages: %llu\n",
		   zram_io_stats.bd_write_pages);
	seq_printf(m, "zram_bd_read_sync_ios_count: %llu\n",
		   zram_io_stats.bd_read_sync_ios_count);
	seq_printf(m, "zram_bd_read_async_ios_count: %llu\n",
		   zram_io_stats.bd_read_async_ios_count);
	seq_printf(m, "zram_bd_read_failures_count: %llu\n",
		   zram_io_stats.bd_read_failures_count);
	seq_printf(m, "zram_bd_read_last_ret: %lld\n",
		   zram_io_stats.bd_read_last_ret);
	seq_printf(m, "zram_bd_read_total_ns: %llu\n",
		   zram_io_stats.bd_read_total_ns);
	seq_printf(m, "zram_bd_read_max_ns: %llu\n",
		   zram_io_stats.bd_read_max_ns);
	seq_printf(m, "zram_bd_read_slow_ios_count: %llu\n",
		   zram_io_stats.bd_read_slow_ios_count);
	seq_printf(m, "zram_bd_read_last_slow_device_index: %lld\n",
		   zram_io_stats.bd_read_last_slow_device_index);
	seq_printf(m, "zram_bd_read_last_slow_sector_index: %llu\n",
		   zram_io_stats.bd_read_last_slow_sector_index);
	seq_printf(m, "zram_bd_read_last_slow_slot_index: %llu\n",
		   zram_io_stats.bd_read_last_slow_slot_index);
	seq_printf(m, "zram_bd_read_last_slow_ret: %lld\n",
		   zram_io_stats.bd_read_last_slow_ret);
	seq_printf(m, "zram_bd_read_last_slow_ns: %llu\n",
		   zram_io_stats.bd_read_last_slow_ns);
	seq_printf(m, "zram_bd_write_ios_count: %llu\n",
		   zram_io_stats.bd_write_ios_count);
	seq_printf(m, "zram_bd_write_failures_count: %llu\n",
		   zram_io_stats.bd_write_failures_count);
	seq_printf(m, "zram_bd_write_last_ret: %lld\n",
		   zram_io_stats.bd_write_last_ret);
	seq_printf(m, "zram_bd_write_total_ns: %llu\n",
		   zram_io_stats.bd_write_total_ns);
	seq_printf(m, "zram_bd_write_max_ns: %llu\n",
		   zram_io_stats.bd_write_max_ns);
	seq_printf(m, "zram_bd_write_slow_ios_count: %llu\n",
		   zram_io_stats.bd_write_slow_ios_count);
	seq_printf(m, "zram_bd_write_last_slow_device_index: %lld\n",
		   zram_io_stats.bd_write_last_slow_device_index);
	seq_printf(m, "zram_bd_write_last_slow_sector_index: %llu\n",
		   zram_io_stats.bd_write_last_slow_sector_index);
	seq_printf(m, "zram_bd_write_last_slow_slot_index: %llu\n",
		   zram_io_stats.bd_write_last_slow_slot_index);
	seq_printf(m, "zram_bd_write_last_slow_ret: %lld\n",
		   zram_io_stats.bd_write_last_slow_ret);
	seq_printf(m, "zram_bd_write_last_slow_ns: %llu\n",
		   zram_io_stats.bd_write_last_slow_ns);
	seq_printf(m, "zram_batchin_runs_count: %llu\n",
		   zram_io_stats.batchin_runs_count);
	seq_printf(m, "zram_batchin_pages: %llu\n",
		   zram_io_stats.batchin_pages);
	seq_printf(m, "zram_batchin_failures_count: %llu\n",
		   zram_io_stats.batchin_failures_count);
	seq_printf(m, "zram_batchin_no_data_count: %llu\n",
		   zram_io_stats.batchin_no_data_count);
	seq_printf(m, "zram_batchin_filtered_pages: %llu\n",
		   zram_io_stats.batchin_filtered_pages);
	seq_printf(m, "zram_batchin_snapshot_mismatch_pages: %llu\n",
		   zram_io_stats.batchin_snapshot_mismatch_pages);
	seq_printf(m, "zram_batchin_total_ns: %llu\n",
		   zram_io_stats.batchin_total_ns);
	seq_printf(m, "zram_batchin_max_ns: %llu\n",
		   zram_io_stats.batchin_max_ns);
	seq_printf(m, "zram_batchin_slow_runs_count: %llu\n",
		   zram_io_stats.batchin_slow_runs_count);
	seq_printf(m, "zram_batchin_last_ret: %lld\n",
		   zram_io_stats.batchin_last_ret);
	seq_printf(m, "zram_batchin_zms_batches: %llu\n",
		   zram_io_stats.batchin_zms_batches);
	seq_printf(m, "zram_batchin_zms_items: %llu\n",
		   zram_io_stats.batchin_zms_items);
	seq_printf(m, "zram_batchin_zms_read_ios: %llu\n",
		   zram_io_stats.batchin_zms_read_ios);
	seq_printf(m, "zram_io_scan_errors_count: %llu\n",
		   zram_io_stats.scan_errors_count);
	seq_printf(m, "enable_store: %lld\n",
		   atomic64_read(&chs.stats.enable_store));
	seq_printf(m, "core_enable_store: %lld\n",
		   atomic64_read(&chs.stats.core_enable_store));
	seq_printf(m, "swapd_pause_store: %lld\n",
		   atomic64_read(&chs.stats.swapd_pause_store));
	seq_printf(m, "loglevel_store: %lld\n",
		   atomic64_read(&chs.stats.loglevel_store));
	seq_printf(m, "loop_device_store: %lld\n",
		   atomic64_read(&chs.stats.loop_device_store));
	seq_printf(m, "loop_device_bind_success: %lld\n",
		   atomic64_read(&chs.stats.loop_device_bind_success));
	seq_printf(m, "loop_device_bind_error: %lld\n",
		   atomic64_read(&chs.stats.loop_device_bind_error));
	seq_printf(m, "loop_device_last_ret: %lld\n",
		   atomic64_read(&chs.stats.loop_device_last_ret));
	seq_printf(m, "zram_increase_store: %lld\n",
		   atomic64_read(&chs.stats.zram_increase_store));
	seq_printf(m, "writeback_quota_limit_bytes: %lld\n",
		   atomic64_read(&chs.stats.writeback_quota_limit_bytes));
	seq_printf(m, "writeback_quota_effective_limit_bytes: %lld\n",
		   atomic64_read(&chs.stats.writeback_quota_effective_limit_bytes));
	seq_printf(m, "writeback_quota_used_pages: %lld\n",
		   atomic64_read(&chs.stats.writeback_quota_used_pages));
	seq_printf(m, "writeback_quota_used_bytes: %lld\n",
		   atomic64_read(&chs.stats.writeback_quota_used_bytes));
	seq_printf(m, "writeback_quota_remaining_pages: %lld\n",
		   atomic64_read(&chs.stats.writeback_quota_remaining_pages));
	seq_printf(m, "writeback_quota_skipped: %lld\n",
		   atomic64_read(&chs.stats.writeback_quota_skipped));
	seq_printf(m, "writeback_quota_capped: %lld\n",
		   atomic64_read(&chs.stats.writeback_quota_capped));
	seq_printf(m, "writeback_quota_resets: %lld\n",
		   atomic64_read(&chs.stats.writeback_quota_resets));
	seq_printf(m, "writeback_quota_window_start_jiffies: %lld\n",
		   atomic64_read(&chs.stats.writeback_quota_window_start_jiffies));
	seq_printf(m, "dev_life_auto_scaled: %lld\n",
		   atomic64_read(&chs.stats.dev_life_auto_scaled));
	seq_printf(m, "dev_life_quota_scaled: %lld\n",
		   atomic64_read(&chs.stats.dev_life_quota_scaled));
	seq_printf(m, "dev_life_force_soft_bypass: %lld\n",
		   atomic64_read(&chs.stats.dev_life_force_soft_bypass));
	seq_printf(m, "force_swapin: %lld\n",
		   atomic64_read(&chs.stats.force_swapin));
	seq_printf(m, "force_swapin_pages: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_pages));
	seq_printf(m, "force_swapin_queued: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_queued));
	seq_printf(m, "force_swapin_scheduled: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_scheduled));
	seq_printf(m, "force_swapin_success: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_success));
	seq_printf(m, "force_swapin_error: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_error));
	seq_printf(m, "force_swapin_skipped: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_skipped));
	seq_printf(m, "force_swapin_no_data: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_no_data));
	seq_printf(m, "force_swapin_last_request_pages: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_last_request_pages));
	seq_printf(m, "force_swapin_requested_pages: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_last_request_pages));
	seq_printf(m, "force_swapin_last_requested_pages: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_last_request_pages));
	seq_printf(m, "force_swapin_last_effective_pages: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_last_effective_pages));
	seq_printf(m, "force_swapin_effective_pages: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_last_effective_pages));
	seq_printf(m, "force_swapin_last_batchin_pages: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_last_batchin_pages));
	seq_printf(m, "force_swapin_moved_pages: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_last_batchin_pages));
	seq_printf(m, "force_swapin_last_moved_pages: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_last_batchin_pages));
	seq_printf(m, "force_swapin_last_ret: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_last_ret));
	seq_printf(m, "force_swapin_last_jiffies: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_last_jiffies));
	seq_printf(m, "force_swapin_per_memcg_success: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_per_memcg_success));
	seq_printf(m, "force_swapin_per_memcg_no_data: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_per_memcg_no_data));
	seq_printf(m, "force_swapin_per_memcg_error: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_per_memcg_error));
	seq_printf(m, "force_swapin_global_runs: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_global_runs));
	seq_printf(m, "force_swapin_last_scanned_pages: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_last_scanned_pages));
	seq_printf(m, "force_swapin_last_matched_pages: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_last_matched_pages));
	seq_printf(m, "force_swapin_last_skipped_pages: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_last_skipped_pages));
	seq_printf(m, "force_swapin_last_filtered_pages: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_last_filtered_pages));
	seq_printf(m, "force_swapin_last_snapshot_mismatch: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_last_snapshot_mismatch));
	seq_printf(m, "force_swapin_last_snapshot_mismatch_pages: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_last_snapshot_mismatch));
	seq_printf(m, "force_swapin_last_devices: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_last_devices));
	seq_printf(m, "force_swapout: %lld\n",
		   atomic64_read(&chs.stats.force_swapout));
	seq_printf(m, "force_swapout_pages: %lld\n",
		   atomic64_read(&chs.stats.force_swapout_pages));
	seq_printf(m, "force_swapout_effective_pages: %lld\n",
		   atomic64_read(&chs.stats.force_swapout_effective_pages));
	seq_printf(m, "force_swapout_queued: %lld\n",
		   atomic64_read(&chs.stats.force_swapout_queued));
	seq_printf(m, "force_swapout_scheduled: %lld\n",
		   atomic64_read(&chs.stats.force_swapout_scheduled));
	seq_printf(m, "force_swapout_success: %lld\n",
		   atomic64_read(&chs.stats.force_swapout_success));
	seq_printf(m, "force_swapout_error: %lld\n",
		   atomic64_read(&chs.stats.force_swapout_error));
	seq_printf(m, "force_swapout_skipped: %lld\n",
		   atomic64_read(&chs.stats.force_swapout_skipped));
	seq_printf(m, "force_swapout_no_data: %lld\n",
		   atomic64_read(&chs.stats.force_swapout_no_data));
	seq_printf(m, "force_swapout_last_compat_trigger_value: %lld\n",
		   atomic64_read(&chs.stats.force_swapout_last_compat_trigger_value));
	seq_printf(m, "force_swapout_last_scanned_pages: %lld\n",
		   atomic64_read(&chs.stats.force_swapout_last_scanned_pages));
	seq_printf(m, "force_swapout_last_eligible_pages: %lld\n",
		   atomic64_read(&chs.stats.force_swapout_last_eligible_pages));
	seq_printf(m, "force_swapout_last_written_pages: %lld\n",
		   atomic64_read(&chs.stats.force_swapout_last_written_pages));
	seq_printf(m, "force_swapout_last_unknown_or_filtered: %lld\n",
		   atomic64_read(&chs.stats.force_swapout_last_unknown_or_filtered_pages));
	seq_printf(m, "force_swapout_last_unknown_or_filtered_pages: %lld\n",
		   atomic64_read(&chs.stats.force_swapout_last_unknown_or_filtered_pages));
	seq_printf(m, "force_swapout_last_ret: %lld\n",
		   atomic64_read(&chs.stats.force_swapout_last_ret));
	seq_printf(m, "force_swapout_last_jiffies: %lld\n",
		   atomic64_read(&chs.stats.force_swapout_last_jiffies));
	seq_puts(m, "force_swapout_target_mode: private_zram_per_memcg_best_effort\n");
	seq_puts(m, "force_swapout_scan_scope: zram_resident_pages\n");
	seq_printf(m, "last_force_swapin_memcg: %s\n", last_in_memcg);
	seq_printf(m, "last_force_swapin_target_memcg_id: %llu\n", last_in_id);
	seq_printf(m, "last_force_swapin_cgroup_id: %llu\n", last_in_id);
	seq_printf(m, "force_swapin_target_memcg_id: %llu\n", last_in_id);
	seq_printf(m, "force_swapin_global_fallback: %d\n", !last_in_id);
	seq_printf(m, "last_force_swapin_app_score: %lld\n", last_in_score);
	seq_printf(m, "last_force_swapin_request: %lld\n", last_in_request);
	seq_printf(m, "last_force_swapin_requested_pages: %lld\n", last_in_pages);
	seq_printf(m, "last_force_swapin_pages: %lld\n", last_in_pages);
	seq_printf(m, "last_force_swapin_global_fallback: %d\n", !last_in_id);
	seq_printf(m, "last_force_swapin_effective_pages: %lld\n",
		   last_in_effective);
	seq_printf(m, "last_force_swapin_moved_pages: %lld\n",
		   atomic64_read(&chs.stats.force_swapin_last_batchin_pages));
	seq_printf(m, "last_force_swapout_memcg: %s\n", last_out_memcg);
	seq_printf(m, "last_force_swapout_cgroup_id: %llu\n", last_out_id);
	seq_printf(m, "last_force_swapout_app_score: %lld\n", last_out_score);
	seq_puts(m, "last_force_swapout_target_mode: private_zram_per_memcg_best_effort\n");
	seq_printf(m, "last_force_swapout_compat_trigger_value: %lld\n",
		   last_out_compat);
	seq_puts(m, "last_force_swapout_scan_scope: zram_resident_pages\n");
	seq_printf(m, "last_force_swapout_scanned_pages: %lld\n",
		   last_out_scanned);
	seq_printf(m, "last_force_swapout_eligible_pages: %lld\n",
		   last_out_eligible);
	seq_printf(m, "last_force_swapout_written_pages: %lld\n",
		   last_out_written);
	seq_printf(m, "last_force_swapout_effective_pages: %lld\n",
		   last_out_effective);
	seq_printf(m, "last_force_swapout_unknown_or_filtered: %lld\n",
		   last_out_unknown_or_filtered);
	seq_printf(m, "last_force_swapout_unknown_or_filtered_pages: %lld\n",
		   last_out_unknown_or_filtered);
	seq_printf(m, "last_force_swapout_ret: %lld\n", last_out_ret);
	seq_printf(m, "force_shrink_anon: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_anon));
	seq_printf(m, "force_shrink_anon_last_param: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_anon_last_param));
	seq_printf(m, "force_shrink_anon_pages: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_anon_pages));
	seq_printf(m, "force_shrink_anon_last_target: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_anon_last_target));
	seq_printf(m, "force_shrink_anon_last_target_pages: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_anon_last_target));
	seq_printf(m, "force_shrink_anon_last_batch: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_anon_last_batch));
	seq_printf(m, "force_shrink_anon_last_batch_pages: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_anon_last_batch));
	seq_printf(m, "force_shrink_anon_last_reclaimed: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_anon_last_reclaimed));
	seq_printf(m, "force_shrink_anon_last_reclaimed_pages: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_anon_last_reclaimed));
	seq_printf(m, "force_shrink_anon_last_ret: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_anon_last_ret));
	seq_printf(m, "force_shrink_anon_dropped: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_anon_dropped));
	seq_printf(m, "force_shrink_anon_skipped: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_anon_skipped));
	seq_printf(m, "force_shrink_anon_best_effort: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_anon_best_effort));
	seq_printf(m, "force_shrink_anon_hook: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_anon_hook));
	seq_printf(m, "force_shrink_file: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_file));
	seq_printf(m, "force_shrink_file_last_param: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_file_last_param));
	seq_printf(m, "force_shrink_file_pages: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_file_pages));
	seq_printf(m, "force_shrink_file_last_target: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_file_last_target));
	seq_printf(m, "force_shrink_file_last_target_pages: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_file_last_target));
	seq_printf(m, "force_shrink_file_last_batch: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_file_last_batch));
	seq_printf(m, "force_shrink_file_last_batch_pages: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_file_last_batch));
	seq_printf(m, "force_shrink_file_last_reclaimed: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_file_last_reclaimed));
	seq_printf(m, "force_shrink_file_last_reclaimed_pages: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_file_last_reclaimed));
	seq_printf(m, "force_shrink_file_last_ret: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_file_last_ret));
	seq_printf(m, "force_shrink_file_dropped: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_file_dropped));
	seq_printf(m, "force_shrink_file_skipped: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_file_skipped));
	seq_printf(m, "force_shrink_queued: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_queued));
	seq_printf(m, "force_shrink_worker_runs: %lld\n",
		   atomic64_read(&chs.stats.force_shrink_worker_runs));
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_EMPTY_APIS
	seq_printf(m, "aging_anon: %lld\n",
		   atomic64_read(&chs.stats.aging_anon));
	seq_printf(m, "aging_anon_last_param: %lld\n",
		   atomic64_read(&chs.stats.aging_anon_last_param));
#endif
	seq_printf(m, "writeback_queued: %lld\n",
		   atomic64_read(&chs.stats.writeback_queued));
	seq_printf(m, "writeback_scheduled: %lld\n",
		   atomic64_read(&chs.stats.writeback_scheduled));
	seq_printf(m, "writeback_success: %lld\n",
		   atomic64_read(&chs.stats.writeback_success));
	seq_printf(m, "writeback_error: %lld\n",
		   atomic64_read(&chs.stats.writeback_error));
	seq_printf(m, "writeback_skipped: %lld\n",
		   atomic64_read(&chs.stats.writeback_skipped));
	seq_printf(m, "writeback_no_data: %lld\n",
		   atomic64_read(&chs.stats.writeback_no_data));
	seq_printf(m, "writeback_noop: %lld\n",
		   atomic64_read(&chs.stats.writeback_noop));
	seq_printf(m, "writeback_noop_pages: %lld\n",
		   atomic64_read(&chs.stats.writeback_noop_pages));
	seq_printf(m, "writeback_last_ret: %lld\n",
		   atomic64_read(&chs.stats.writeback_last_ret));
	seq_printf(m, "writeback_last_pages: %lld\n",
		   atomic64_read(&chs.stats.writeback_last_pages));
	seq_printf(m, "writeback_last_written_pages: %lld\n",
		   atomic64_read(&chs.stats.writeback_last_written_pages));
	seq_printf(m, "writeback_last_mode: %s\n", last_mode);
	seq_printf(m, "writeback_last_jiffies: %lld\n",
		   atomic64_read(&chs.stats.writeback_last_jiffies));
	seq_printf(m, "writeback_worker_runs: %lld\n",
		   atomic64_read(&chs.stats.writeback_worker_runs));
	seq_printf(m, "writeback_worker_total_ns: %lld\n",
		   atomic64_read(&chs.stats.writeback_worker_total_ns));
	seq_printf(m, "writeback_worker_max_ns: %lld\n",
		   atomic64_read(&chs.stats.writeback_worker_max_ns));
	seq_printf(m, "writeback_worker_slow_runs: %lld\n",
		   atomic64_read(&chs.stats.writeback_worker_slow_runs));
	seq_printf(m, "writeback_worker_last_ns: %lld\n",
		   atomic64_read(&chs.stats.writeback_worker_last_ns));
	seq_printf(m, "batchin_worker_runs: %lld\n",
		   atomic64_read(&chs.stats.batchin_worker_runs));
	seq_printf(m, "batchin_worker_total_ns: %lld\n",
		   atomic64_read(&chs.stats.batchin_worker_total_ns));
	seq_printf(m, "batchin_worker_max_ns: %lld\n",
		   atomic64_read(&chs.stats.batchin_worker_max_ns));
	seq_printf(m, "batchin_worker_slow_runs: %lld\n",
		   atomic64_read(&chs.stats.batchin_worker_slow_runs));
	seq_printf(m, "batchin_worker_last_ns: %lld\n",
		   atomic64_read(&chs.stats.batchin_worker_last_ns));
	seq_printf(m, "multi_zram_registered: %lld\n",
		   atomic64_read(&chs.stats.multi_zram_registered));
	seq_printf(m, "multi_zram_last_selected: %lld\n",
		   atomic64_read(&chs.stats.multi_zram_last_selected));
	seq_printf(m, "multi_zram_last_traversed: %lld\n",
		   atomic64_read(&chs.stats.multi_zram_last_traversed));
	seq_printf(m, "multi_zram_last_eligible: %lld\n",
		   atomic64_read(&chs.stats.multi_zram_last_eligible));
	seq_printf(m, "multi_zram_skip_no_backing: %lld\n",
		   atomic64_read(&chs.stats.multi_zram_skip_no_backing));
	seq_printf(m, "multi_zram_skip_limit: %lld\n",
		   atomic64_read(&chs.stats.multi_zram_skip_limit));
	seq_printf(m, "multi_zram_skip_no_resident: %lld\n",
		   atomic64_read(&chs.stats.multi_zram_skip_no_resident));
	seq_printf(m, "multi_zram_force_swapout_devices: %lld\n",
		   atomic64_read(&chs.stats.multi_zram_force_swapout_devices));
	seq_printf(m, "multi_zram_force_swapin_devices: %lld\n",
		   atomic64_read(&chs.stats.multi_zram_force_swapin_devices));
	seq_printf(m, "multi_zram_global_batchin_devices: %lld\n",
		   atomic64_read(&chs.stats.multi_zram_global_batchin_devices));
	seq_printf(m, "diagnostic_slow_io_ns: %llu\n",
		   (unsigned long long)CHS_ZRAM_SLOW_IO_NS);
	seq_printf(m, "diagnostic_slow_work_ns: %llu\n",
		   (unsigned long long)CHS_ZRAM_SLOW_WORK_NS);
	seq_printf(m, "readback_snapshot_validation: page_level_enabled\n");
	seq_printf(m, "force_swapin_scope: private_zram_per_memcg_page_level_with_global_fallback\n");
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
	seq_printf(m, "last_auto_writeback_jiffies: %lld\n",
		   atomic64_read(&chs.stats.last_auto_writeback_jiffies));
	seq_printf(m, "last_auto_writeback_ret: %lld\n",
		   atomic64_read(&chs.stats.last_auto_writeback_ret));
	seq_printf(m, "last_auto_writeback_written_pages: %lld\n",
		   atomic64_read(&chs.stats.last_auto_writeback_written_pages));
	seq_printf(m, "last_auto_reason: %s\n", auto_reason);
	seq_printf(m, "policy_available_android_pages: %lld\n",
		   atomic64_read(&chs.stats.policy_available_android_pages));
	seq_printf(m, "policy_available_android_mb: %lld\n",
		   atomic64_read(&chs.stats.policy_available_android_mb));
	seq_printf(m, "policy_available_fallback: %lld\n",
		   atomic64_read(&chs.stats.policy_available_fallback));
	seq_printf(m, "policy_zram_pressure_ratio: %lld\n",
		   atomic64_read(&chs.stats.policy_zram_pressure_ratio));
	seq_printf(m, "policy_zram_effective_ratio: %lld\n",
		   atomic64_read(&chs.stats.policy_zram_effective_ratio));
	seq_printf(m, "policy_zram_wm_ratio: %lld\n",
		   atomic64_read(&chs.stats.policy_zram_wm_ratio));
	seq_printf(m, "policy_zram_gate_result: %lld\n",
		   atomic64_read(&chs.stats.policy_zram_gate_result));
	seq_printf(m, "policy_zram_resident_pages: %lld\n",
		   atomic64_read(&chs.stats.policy_zram_resident_pages));
	seq_printf(m, "policy_zram_total_pages: %lld\n",
		   atomic64_read(&chs.stats.policy_zram_total_pages));
	seq_printf(m, "policy_zram_increase_pages: %lld\n",
		   atomic64_read(&chs.stats.policy_zram_increase_pages));
	seq_printf(m, "policy_zram_increase_boost_pages: %lld\n",
		   atomic64_read(&chs.stats.policy_zram_increase_boost_pages));
	seq_printf(m, "policy_zram_increase_budget_pages: %lld\n",
		   atomic64_read(&chs.stats.policy_zram_increase_budget_pages));
	seq_printf(m, "policy_empty_rounds: %lld\n",
		   atomic64_read(&chs.stats.policy_empty_rounds));
	seq_printf(m, "policy_empty_backoff_interval_ms: %lld\n",
		   atomic64_read(&chs.stats.policy_empty_backoff_interval_ms));
	seq_printf(m, "policy_empty_backoff_skipped: %lld\n",
		   atomic64_read(&chs.stats.policy_empty_backoff_skipped));
	seq_printf(m, "policy_window_start_jiffies: %lld\n",
		   atomic64_read(&chs.stats.policy_window_start_jiffies));
	seq_printf(m, "policy_window_written_pages: %lld\n",
		   atomic64_read(&chs.stats.policy_window_written_pages));
	seq_printf(m, "policy_window_throttled: %lld\n",
		   atomic64_read(&chs.stats.policy_window_throttled));
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
	seq_printf(m, "auto_policy_normal_interval_ms: %u\n",
		   CHS_AUTO_POLICY_NORMAL_INTERVAL_MS);
	seq_printf(m, "auto_policy_low_interval_ms: %u\n",
		   CHS_AUTO_POLICY_LOW_INTERVAL_MS);
	seq_printf(m, "auto_policy_min_writeback_interval_ms: %u\n",
		   CHS_AUTO_POLICY_MIN_WRITEBACK_INTERVAL_MS);
	seq_printf(m, "avail_buffers_writes: %lld\n",
		   atomic64_read(&chs.stats.avail_buffers_writes));
	seq_printf(m, "avail_buffers_wakeups: %lld\n",
		   atomic64_read(&chs.stats.avail_buffers_wakeups));
	seq_printf(m, "avail_buffers_low_events: %lld\n",
		   atomic64_read(&chs.stats.avail_buffers_low_events));
	seq_printf(m, "avail_buffers_high_events: %lld\n",
		   atomic64_read(&chs.stats.avail_buffers_high_events));
	seq_printf(m, "avail_buffers_swap_low_events: %lld\n",
		   atomic64_read(&chs.stats.avail_buffers_swap_low_events));
	seq_printf(m, "avail_buffers_last_avail_mb: %lld\n",
		   atomic64_read(&chs.stats.avail_buffers_last_avail));
	seq_printf(m, "avail_buffers_last_min_mb: %lld\n",
		   atomic64_read(&chs.stats.avail_buffers_last_min));
	seq_printf(m, "avail_buffers_last_high_mb: %lld\n",
		   atomic64_read(&chs.stats.avail_buffers_last_high));
	seq_printf(m, "avail_buffers_last_free_swap_threshold_mb: %lld\n",
		   atomic64_read(&chs.stats.avail_buffers_last_free_swap_threshold));
	seq_printf(m, "erm_avail_buffer_default_enable: %d\n",
		   CHS_ERM_AVAIL_BUFFER_DEFAULT_ENABLE);
	seq_printf(m, "erm_avail_buffer_enable: %d\n",
		   atomic_read(&chs.erm_avail_buffer_enable));
	seq_printf(m, "erm_avail_buffer_valid: %d\n",
		   atomic_read(&chs.erm_avail_buffer_valid));
	seq_printf(m, "erm_min_avail_buffers_mb: %lld\n",
		   atomic64_read(&chs.erm_min_avail_buffer));
	seq_printf(m, "erm_high_avail_buffers_mb: %lld\n",
		   atomic64_read(&chs.erm_high_avail_buffer));
	seq_printf(m, "avail_buffers_effective_min_mb: %u\n",
		   avail_buffer_view.effective_min);
	seq_printf(m, "avail_buffers_effective_high_mb: %u\n",
		   avail_buffer_view.effective_high);
	seq_printf(m, "avail_buffers_effective_source: %s\n",
		   avail_buffer_view.override_active ? "erm" : "base");
	seq_printf(m, "erm_avail_buffer_enable_store: %lld\n",
		   atomic64_read(&chs.stats.erm_avail_buffer_enable_store));
	seq_printf(m, "erm_avail_buffer_writes: %lld\n",
		   atomic64_read(&chs.stats.erm_avail_buffer_writes));
	seq_printf(m, "erm_avail_buffer_last_ret: %lld\n",
		   atomic64_read(&chs.stats.erm_avail_buffer_last_ret));
	seq_printf(m, "avail_buffers_last_seen_avail_mb: %lld\n",
		   atomic64_read(&chs.stats.avail_buffers_last_seen_avail));
	seq_printf(m, "avail_buffers_last_free_swap_pages: %lld\n",
		   atomic64_read(&chs.stats.avail_buffers_last_free_swap_pages));
	seq_printf(m, "avail_buffers_last_free_swap_mb: %lld\n",
		   atomic64_read(&chs.stats.avail_buffers_last_free_swap_pages) /
		   (s64)CHS_PAGES_PER_MB);
	crystal_hybridswap_pressure_stats_show(m);
	seq_printf(m, "memcg_entries: %lld\n",
		   atomic64_read(&chs.stats.memcg_entries));
	seq_printf(m, "memcg_param_updates: %lld\n",
		   atomic64_read(&chs.stats.memcg_param_updates));
	seq_printf(m, "memcg_policy_parse_success: %lld\n",
		   atomic64_read(&chs.stats.memcg_policy_parse_success));
	seq_printf(m, "memcg_policy_parse_error: %lld\n",
		   atomic64_read(&chs.stats.memcg_policy_parse_error));
	seq_printf(m, "memcg_policy_apply: %lld\n",
		   atomic64_read(&chs.stats.memcg_policy_apply));
	seq_printf(m, "memcg_single_policy_parse_success: %lld\n",
		   atomic64_read(&chs.stats.memcg_single_policy_parse_success));
	seq_printf(m, "memcg_single_policy_parse_error: %lld\n",
		   atomic64_read(&chs.stats.memcg_single_policy_parse_error));
	crystal_hybridswap_memcg_stats_show(m);
}

static int stats_show(struct seq_file *m, void *v)
{
	crystal_hybridswap_stats_show(m);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(stats);

static int report_show(struct seq_file *m, void *v)
{
	seq_puts(m, "Crystal Hybridswap stage-3\n");
	seq_puts(m, "legacy_abi: zram sysfs + memcg cgroup + pressure eventfd\n");
	seq_puts(m, "data_path: private zram writeback + private zram batch-in\n");
	seq_printf(m, "legacy_empty_apis: %s (%s)\n",
		   CHS_LEGACY_EMPTY_APIS_STATE,
		   CHS_LEGACY_EMPTY_APIS_VISIBILITY);
	seq_printf(m, "legacy_empty_apis_list: %s\n",
		   CHS_LEGACY_EMPTY_APIS_LIST);
	seq_printf(m, "legacy_swapd_memcgs_param: %s (%s)\n",
		   CHS_LEGACY_SWAPD_MEMCGS_PARAM_STATE,
		   CHS_LEGACY_SWAPD_MEMCGS_PARAM_VISIBILITY);
	seq_printf(m, "legacy_swapd_memcgs_param_list: %s\n",
		   CHS_LEGACY_SWAPD_MEMCGS_PARAM_LIST);
	seq_puts(m, "force_swapout: private_zram_per_memcg_best_effort_multi_zram\n");
	seq_puts(m, "force_swapout_scope: zram_resident_pages_not_full_ram_anon_reclaim\n");
	seq_puts(m, "force_swapout_unknown_identity: skip_and_count_unknown_or_filtered\n");
	seq_puts(m, "force_swapout_units: unknown_or_filtered fields with _pages suffix are page counts; no-suffix names are compatibility aliases\n");
	seq_puts(m, "force_swapin: private_zram_per_memcg_page_level_first; explicit_global_fallback_when_no_target\n");
	seq_puts(m, "force_swapin_units: memory.force_swapin write value is a trigger/request value; use *_request and *_pages fields for units; total_info_per_app in_mb is compatibility only\n");
	seq_puts(m, "batchin_snapshot_validation: page_level_slot_state_verify_before_commit\n");
	seq_puts(m, "force_shrink_units: *_last_target_pages/*_last_batch_pages/*_last_reclaimed_pages are page counts; old no-suffix names are compatibility aliases\n");
	seq_puts(m, "diagnostics: backing_read_write_latency batchin_worker writeback_worker slow_logs_rate_limited\n");
	seq_puts(m, "zram_bd_stat: standard_3_position_fields_only; Crystal zram IO diagnostics are named zram_* fields in debugfs stats\n");
	seq_printf(m, "multi_zram: registered=%lld selected=%lld traversed=%lld eligible=%lld skip_no_backing=%lld skip_limit=%lld skip_no_resident=%lld\n",
		   atomic64_read(&chs.stats.multi_zram_registered),
		   atomic64_read(&chs.stats.multi_zram_last_selected),
		   atomic64_read(&chs.stats.multi_zram_last_traversed),
		   atomic64_read(&chs.stats.multi_zram_last_eligible),
		   atomic64_read(&chs.stats.multi_zram_skip_no_backing),
		   atomic64_read(&chs.stats.multi_zram_skip_limit),
		   atomic64_read(&chs.stats.multi_zram_skip_no_resident));
	seq_puts(m, "force_shrink_file: async bounded memcg direct reclaim, file-first/no-swap\n");
	seq_puts(m, "force_shrink_anon: async bounded memcg direct reclaim, anon hook when available otherwise best-effort\n");
	seq_puts(m, "fault_out: disabled; zram handles per-slot readback\n");
	seq_puts(m, "writeback_modes: idle,huge,huge_idle,incompressible,page_index=N\n");
	seq_printf(m, "internal_writeback_mode: %s\n", CHS_INTERNAL_WB_MODE);
	seq_printf(m, "legacy_swapd_policy_formats: %s; swapd_memcgs_param=<levels> <min> <max> <mem2zram> <zram2ufs> <refault>...; swapd_single_memcg_param=<mem2zram> <zram2ufs> <refault>\n",
		   CHS_LEGACY_SWAPD_MEMCGS_PARAM_VISIBILITY);
	seq_puts(m, "avail_buffers: stored ABI tuple configures kernel automatic policy; all four ABI values are MB; delayed work compares free_swap_threshold_mb after converting it to pages\n");
	seq_puts(m, "zram_wm_ratio_api: memory.zram_wm_ratio controls zram watermark, old-compatible range 0..100, no runtime min clamp\n");
	seq_puts(m, "auto_policy_controls: daily_quota + dev_life_budget_scale + zram_wm_ratio_gate + zram_increase_boost + empty_no_data_backoff + window_throttle + min_writeback_interval\n");
	seq_printf(m, "auto_policy_limits: zram_wm_ratio=%lld window_mb=%u memcg_candidates=%u memcg_max_mb=%u quota_window_ms=%lu dev_life_auto_percent=%u\n",
		   crystal_hybridswap_zram_wm_ratio(),
		   CHS_AUTO_POLICY_WINDOW_MAX_WRITEBACK_MB,
		   CHS_AUTO_MEMCG_MAX_CANDIDATES,
		   CHS_AUTO_MEMCG_MAX_WRITEBACK_MB,
		   CHS_QUOTA_WINDOW_MS,
		   CHS_DEV_LIFE_AUTO_BUDGET_PERCENT);
	seq_printf(m, "auto_memcg: legacy_swapd_memcgs_param=%s; when exposed, prefer policy-enabled memcg candidates by app_score/zram2ufs_ratio/recent_result; fallback to selected global zram writeback when no candidate queues\n",
		   CHS_LEGACY_SWAPD_MEMCGS_PARAM_STATE);
	seq_printf(m, "ub_ufs2zram_ratio_api: %s; per_memcg_batchin=page_level_slot_memcg_filter_no_legacy_extent\n",
		   IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_EMPTY_APIS) ?
		   "legacy_empty_compat_saved_only" : "hidden");
	seq_puts(m, "not_migrated: legacy OPPO extent/rmap/fault-out/readback paths and standard zram ABI changes\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(report);

void crystal_hybridswap_debugfs_init(void)
{
	chs.debugfs_root = debugfs_create_dir(CHS_NAME, NULL);
	if (IS_ERR_OR_NULL(chs.debugfs_root)) {
		chs.debugfs_root = NULL;
		return;
	}

	debugfs_create_file("stats", 0444, chs.debugfs_root, NULL, &stats_fops);
	debugfs_create_file("report", 0444, chs.debugfs_root, NULL, &report_fops);
}

void crystal_hybridswap_debugfs_exit(void)
{
	debugfs_remove_recursive(chs.debugfs_root);
	chs.debugfs_root = NULL;
}
