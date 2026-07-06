/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _CRYSTAL_HYBRIDSWAP_INTERNAL_H
#define _CRYSTAL_HYBRIDSWAP_INTERNAL_H

#include <linux/atomic.h>
#include <linux/cgroup.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/limits.h>
#include <linux/list.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/types.h>
#include <linux/time64.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#define CHS_NAME			"crystal_hybridswap"
#define CHS_MEMCG_NAME_MAX		32
#define CHS_LOOP_DEVICE_MAX		128
#define CHS_WB_MODE_MAX			32
#define CHS_INTERNAL_WB_MODE		"hybridswap_normal"
#define CHS_DEFAULT_WB_MODE		CHS_INTERNAL_WB_MODE
#define CHS_MAX_APP_SCORE		1000
#define CHS_MIN_RATIO			0
#define CHS_MAX_RATIO			100
#define CHS_DEFAULT_APP_SCORE		300
#define CHS_DEFAULT_RATIO		100
#define CHS_DEFAULT_ZRAM_WM_RATIO	75
#define CHS_DEFAULT_STORED_WM_RATIO	CHS_DEFAULT_ZRAM_WM_RATIO
#define CHS_DEFAULT_QUOTA_DAY		0x280000000ULL
#define CHS_SWAPD_MAX_LEVEL_NUM		10
#define CHS_POLICY_RAW_MAX		256
#define CHS_PRESSURE_REASON_MAX		32
#define CHS_POLICY_LEVEL_NONE		(-1)
#define CHS_POLICY_LEVEL_MANUAL		(-2)
#define CHS_PRESSURE_LOW		0
#define CHS_PRESSURE_MEDIUM		1
#define CHS_PRESSURE_CRITICAL		2
#define CHS_PRESSURE_LEVELS		3
#define CHS_PAGES_PER_MB		(SZ_1M >> PAGE_SHIFT)
#define CHS_POLICY_WAKE_MAX_WRITEBACK_MB	128
#define CHS_FORCE_GLOBAL_SCAN_PAGES	S64_MAX
#define CHS_AUTO_POLICY_NORMAL_INTERVAL_MS	5000
#define CHS_AUTO_POLICY_LOW_INTERVAL_MS		1000
#define CHS_AUTO_POLICY_MIN_WRITEBACK_INTERVAL_MS	5000
#define CHS_AUTO_POLICY_EMPTY_BACKOFF_BASE_MS	5000
#define CHS_AUTO_POLICY_EMPTY_BACKOFF_MAX_MS	60000
#define CHS_AUTO_POLICY_WINDOW_MS		60000
#define CHS_AUTO_POLICY_WINDOW_MAX_WRITEBACK_MB	256
#define CHS_AUTO_MEMCG_MAX_CANDIDATES		4
#define CHS_AUTO_MEMCG_MAX_WRITEBACK_MB		32
#define CHS_QUOTA_WINDOW_MS			(24UL * 60UL * 60UL * 1000UL)
#define CHS_DEV_LIFE_QUOTA_DIVISOR		10
#define CHS_DEV_LIFE_AUTO_BUDGET_PERCENT	25
#define CHS_ZRAM_INCREASE_PRESSURE_DIVISOR	2
#define CHS_ZRAM_INCREASE_BUDGET_DIVISOR	4
#define CHS_ZRAM_SLOW_IO_NS		(50ULL * NSEC_PER_MSEC)
#define CHS_ZRAM_SLOW_WORK_NS		(200ULL * NSEC_PER_MSEC)

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_EMPTY_APIS
#define CHS_LEGACY_EMPTY_APIS_ENABLED	1
#define CHS_LEGACY_EMPTY_APIS_STATE	"enabled"
#define CHS_LEGACY_EMPTY_APIS_VISIBILITY	"exposed"
#else
#define CHS_LEGACY_EMPTY_APIS_ENABLED	0
#define CHS_LEGACY_EMPTY_APIS_STATE	"disabled"
#define CHS_LEGACY_EMPTY_APIS_VISIBILITY	"hidden"
#endif
#define CHS_LEGACY_EMPTY_APIS_LIST \
	"memory.aging_anon,memory.swapd_pid,memory.psi," \
	"memory.ub_ufs2zram_ratio,zram.max_comp_streams"

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_SWAPD_MEMCGS_PARAM
#define CHS_LEGACY_SWAPD_MEMCGS_PARAM_ENABLED	1
#define CHS_LEGACY_SWAPD_MEMCGS_PARAM_STATE	"enabled"
#define CHS_LEGACY_SWAPD_MEMCGS_PARAM_VISIBILITY	"exposed"
#else
#define CHS_LEGACY_SWAPD_MEMCGS_PARAM_ENABLED	0
#define CHS_LEGACY_SWAPD_MEMCGS_PARAM_STATE	"disabled"
#define CHS_LEGACY_SWAPD_MEMCGS_PARAM_VISIBILITY	"hidden"
#endif
#define CHS_LEGACY_SWAPD_MEMCGS_PARAM_LIST \
	"memory.swapd_memcgs_param,memory.swapd_single_memcg_param"

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ERM_AVAIL_BUFFER_DEFAULT_ON
#define CHS_ERM_AVAIL_BUFFER_DEFAULT_ENABLE	1
#else
#define CHS_ERM_AVAIL_BUFFER_DEFAULT_ENABLE	0
#endif

enum chs_log_level {
	CHS_LOG_ERR = 0,
	CHS_LOG_WARN,
	CHS_LOG_INFO,
	CHS_LOG_DEBUG,
	CHS_LOG_MAX,
};

struct chs_avail_buffer_view {
	unsigned int base_min;
	unsigned int base_high;
	unsigned int effective_min;
	unsigned int effective_high;
	bool override_active;
};

#define CHS_LOG_PREFIX		"[HYB_ZRAM]"

static inline char chs_log_level_char(int level)
{
	switch (level) {
	case CHS_LOG_ERR:
		return 'E';
	case CHS_LOG_WARN:
		return 'W';
	case CHS_LOG_INFO:
		return 'I';
	case CHS_LOG_DEBUG:
		return 'D';
	default:
		return '?';
	}
}

#define __chs_log_emit(klevel, level, fmt, ...) \
	printk(klevel CHS_LOG_PREFIX \
	       " tgid=%d pid=%d level=%c comm=%s func=%s line=%d " fmt, \
	       current->tgid, current->pid, chs_log_level_char(level), \
	       current->comm, __func__, __LINE__, ##__VA_ARGS__)

#define __chs_log_emit_ratelimited(klevel, level, fmt, ...) \
	printk_ratelimited(klevel CHS_LOG_PREFIX \
			  " tgid=%d pid=%d level=%c comm=%s func=%s line=%d " fmt, \
			  current->tgid, current->pid, chs_log_level_char(level), \
			  current->comm, __func__, __LINE__, ##__VA_ARGS__)

#define chs_log(level, fmt, ...) do { \
	int __level = (level); \
	if (__level <= crystal_hybridswap_loglevel()) { \
		switch (__level) { \
		case CHS_LOG_ERR: \
			__chs_log_emit(KERN_ERR, __level, fmt, ##__VA_ARGS__); \
			break; \
		case CHS_LOG_WARN: \
			__chs_log_emit(KERN_WARNING, __level, fmt, ##__VA_ARGS__); \
			break; \
		case CHS_LOG_DEBUG: \
			__chs_log_emit(KERN_DEBUG, __level, fmt, ##__VA_ARGS__); \
			break; \
		default: \
			__chs_log_emit(KERN_INFO, __level, fmt, ##__VA_ARGS__); \
			break; \
		} \
	} \
} while (0)

#define chs_log_ratelimited(level, fmt, ...) do { \
	int __level = (level); \
	if (__level <= crystal_hybridswap_loglevel()) { \
		switch (__level) { \
		case CHS_LOG_ERR: \
			__chs_log_emit_ratelimited(KERN_ERR, __level, fmt, ##__VA_ARGS__); \
			break; \
		case CHS_LOG_WARN: \
			__chs_log_emit_ratelimited(KERN_WARNING, __level, fmt, ##__VA_ARGS__); \
			break; \
		case CHS_LOG_DEBUG: \
			__chs_log_emit_ratelimited(KERN_DEBUG, __level, fmt, ##__VA_ARGS__); \
			break; \
		default: \
			__chs_log_emit_ratelimited(KERN_INFO, __level, fmt, ##__VA_ARGS__); \
			break; \
		} \
	} \
} while (0)

static inline unsigned int crystal_hybridswap_u64_percent(u64 val, u64 total)
{
	u64 percent;

	if (!total)
		return 0;

	percent = mul_u64_u64_div_u64(val, 100, total);
	return percent > UINT_MAX ? UINT_MAX : (unsigned int)percent;
}

/*
 * Private zram writeback/batch-in return a positive page count on success,
 * -ENODATA for a successful scan with no matching page, or a negative errno.
 */
struct crystal_hybridswap_writeback_stats {
	unsigned long scanned_pages;
	unsigned long eligible_pages;
	unsigned long written_pages;
	unsigned long unknown_or_filtered_pages;
};

struct crystal_hybridswap_batchin_stats {
	unsigned long scanned_pages;
	unsigned long matched_pages;
	unsigned long moved_pages;
	unsigned long skipped_pages;
	unsigned long filtered_pages;
	unsigned long read_errors;
	unsigned long prepare_errors;
	unsigned long snapshot_mismatch;
	int last_error;
};

struct crystal_hybridswap_memcg_zram_stats {
	u64 resident_pages;
	u64 writeback_pages;
	u64 same_pages;
	u64 huge_pages;
	u64 zram_compressed_size;
	u64 zram_original_size;
	u64 writeback_size;
	u64 writeback_original_size;
	u64 scanned_pages;
	u64 matched_pages;
	u64 devices_scanned;
	u64 devices_with_data;
	u64 scan_errors;
};

struct crystal_hybridswap_zram_io_stats {
	u64 devices_count;
	s64 last_device_index;
	u64 bd_pages;
	u64 bd_compressed_bytes;
	u64 bd_read_pages;
	u64 bd_write_pages;
	u64 bd_physical_read_pages;
	u64 bd_physical_read_ios_count;
	u64 bd_physical_read_failed_pages;
	u64 bd_physical_write_pages;
	u64 bd_physical_write_bytes;
	u64 bd_physical_write_ios_count;
	u64 bd_physical_write_failed_pages;
	u64 bd_read_sync_ios_count;
	u64 bd_read_async_ios_count;
	u64 bd_read_failures_count;
	s64 bd_read_last_ret;
	u64 bd_read_total_ns;
	u64 bd_read_max_ns;
	u64 bd_read_slow_ios_count;
	s64 bd_read_last_slow_device_index;
	u64 bd_read_last_slow_sector_index;
	u64 bd_read_last_slow_slot_index;
	s64 bd_read_last_slow_ret;
	u64 bd_read_last_slow_ns;
	u64 bd_write_ios_count;
	u64 bd_write_failures_count;
	s64 bd_write_last_ret;
	u64 bd_write_total_ns;
	u64 bd_write_max_ns;
	u64 bd_write_slow_ios_count;
	s64 bd_write_last_slow_device_index;
	u64 bd_write_last_slow_sector_index;
	u64 bd_write_last_slow_slot_index;
	s64 bd_write_last_slow_ret;
	u64 bd_write_last_slow_ns;
	u64 batchin_runs_count;
	u64 batchin_pages;
	u64 batchin_failures_count;
	u64 batchin_no_data_count;
	u64 batchin_filtered_pages;
	u64 batchin_read_errors_count;
	u64 batchin_prepare_errors_count;
	u64 batchin_snapshot_mismatch_pages;
	u64 batchin_total_ns;
	u64 batchin_max_ns;
	u64 batchin_slow_runs_count;
	s64 batchin_last_ret;
	u64 batchin_zms_batches;
	u64 batchin_zms_items;
	u64 batchin_zms_read_ios;
	u64 prefetch_runs;
	u64 prefetch_candidates;
	u64 prefetch_submitted;
	u64 prefetch_moved;
	u64 prefetch_skipped;
	u64 prefetch_read_errors;
	u64 prefetch_prepare_errors;
	u64 prefetch_snapshot_mismatch;
	u64 prefetch_no_data;
	u64 prefetch_alloc_failures;
	u64 prefetch_hits;
	u64 prefetch_stale_hits;
	u64 prefetch_expired;
	u64 prefetch_reclaimed;
	u64 prefetch_invalidated;
	u64 prefetch_hit_pct;
	u64 scan_errors_count;
};

struct crystal_hybridswap_zram_pressure {
	u64 stored_pages;
	u64 same_pages;
	u64 writeback_pages;
	u64 resident_pages;
	u64 total_pages;
	u64 increase_pages;
	u64 wb_limit_pages;
	u64 device_id;
	unsigned int resident_ratio;
	bool valid;
	bool backing_dev;
	bool wb_limit_enabled;
	bool wb_limit_exhausted;
};

struct crystal_hybridswap_auto_memcg_candidate {
	u64 cgroup_id;
	s64 app_score;
	s64 budget_pages;
	unsigned int zram2ufs_ratio;
	int policy_level;
	char name[CHS_MEMCG_NAME_MAX];
};

typedef int (*crystal_hybridswap_zram_writeback_t)(struct device *dev,
		const char *mode, unsigned long nr_pages,
		struct crystal_hybridswap_writeback_stats *stats);
typedef int (*crystal_hybridswap_zram_writeback_ext_t)(struct device *dev,
		const char *mode, unsigned long nr_pages, bool auto_req,
		struct crystal_hybridswap_writeback_stats *stats);
typedef int (*crystal_hybridswap_zram_force_writeback_t)(struct device *dev,
		const char *mode, unsigned long nr_pages, u64 target_cgroup_id,
		struct crystal_hybridswap_writeback_stats *stats);
typedef int (*crystal_hybridswap_zram_force_writeback_ext_t)(struct device *dev,
		const char *mode, unsigned long nr_pages, u64 target_cgroup_id,
		bool auto_req, struct crystal_hybridswap_writeback_stats *stats);
typedef int (*crystal_hybridswap_zram_batchin_t)(struct device *dev,
		unsigned long nr_pages, u64 target_cgroup_id,
		struct crystal_hybridswap_batchin_stats *stats);

struct crystal_hybridswap_stats {
	atomic64_t zram_register;
	atomic64_t zram_unregister;
	atomic64_t zram_sysfs_errors;
	atomic64_t enable_store;
	atomic64_t core_enable_store;
	atomic64_t swapd_pause_store;
	atomic64_t loglevel_store;
	atomic64_t loop_device_store;
	atomic64_t loop_device_bind_success;
	atomic64_t loop_device_bind_error;
	atomic64_t loop_device_last_ret;
	atomic64_t zram_increase_store;
	atomic64_t writeback_quota_limit_bytes;
	atomic64_t writeback_quota_effective_limit_bytes;
	atomic64_t writeback_quota_used_pages;
	atomic64_t writeback_quota_used_bytes;
	atomic64_t writeback_quota_remaining_pages;
	atomic64_t writeback_quota_remaining_bytes;
	atomic64_t writeback_quota_skipped;
	atomic64_t writeback_quota_capped;
	atomic64_t writeback_quota_resets;
	atomic64_t writeback_quota_window_start_jiffies;
	atomic64_t dev_life_level;
	atomic64_t dev_life_auto_scaled;
	atomic64_t dev_life_quota_scaled;
	atomic64_t dev_life_force_soft_bypass;
	atomic64_t force_swapin;
	atomic64_t force_swapin_pages;
	atomic64_t force_swapin_queued;
	atomic64_t force_swapin_scheduled;
	atomic64_t force_swapin_success;
	atomic64_t force_swapin_error;
	atomic64_t force_swapin_skipped;
	atomic64_t force_swapin_no_data;
	atomic64_t force_swapin_last_request_pages;
	atomic64_t force_swapin_last_effective_pages;
	atomic64_t force_swapin_last_batchin_pages;
	atomic64_t force_swapin_last_ret;
	atomic64_t force_swapin_last_jiffies;
	atomic64_t force_swapout;
	atomic64_t force_swapout_pages;
	atomic64_t force_swapout_effective_pages;
	atomic64_t force_swapout_queued;
	atomic64_t force_swapout_scheduled;
	atomic64_t force_swapout_success;
	atomic64_t force_swapout_error;
	atomic64_t force_swapout_skipped;
	atomic64_t force_swapout_no_data;
	atomic64_t force_swapout_last_compat_trigger_value;
	atomic64_t force_swapout_last_scanned_pages;
	atomic64_t force_swapout_last_eligible_pages;
	atomic64_t force_swapout_last_written_pages;
	atomic64_t force_swapout_last_unknown_or_filtered_pages;
	atomic64_t force_swapout_last_ret;
	atomic64_t force_swapout_last_jiffies;
	atomic64_t force_swapin_per_memcg_success;
	atomic64_t force_swapin_per_memcg_no_data;
	atomic64_t force_swapin_per_memcg_error;
	atomic64_t force_swapin_global_runs;
	atomic64_t force_swapin_last_scanned_pages;
	atomic64_t force_swapin_last_matched_pages;
	atomic64_t force_swapin_last_skipped_pages;
	atomic64_t force_swapin_last_filtered_pages;
	atomic64_t force_swapin_last_read_errors;
	atomic64_t force_swapin_last_prepare_errors;
	atomic64_t force_swapin_last_snapshot_mismatch;
	atomic64_t force_swapin_last_devices;
	atomic64_t force_shrink_anon;
	atomic64_t force_shrink_file;
	atomic64_t aging_anon;
	atomic64_t force_shrink_anon_last_param;
	atomic64_t force_shrink_file_last_param;
	atomic64_t aging_anon_last_param;
	atomic64_t force_shrink_queued;
	atomic64_t force_shrink_worker_runs;
	atomic64_t force_shrink_anon_pages;
	atomic64_t force_shrink_file_pages;
	atomic64_t force_shrink_anon_last_target;
	atomic64_t force_shrink_file_last_target;
	atomic64_t force_shrink_anon_last_batch;
	atomic64_t force_shrink_file_last_batch;
	atomic64_t force_shrink_anon_last_reclaimed;
	atomic64_t force_shrink_file_last_reclaimed;
	atomic64_t force_shrink_anon_last_ret;
	atomic64_t force_shrink_file_last_ret;
	atomic64_t force_shrink_anon_dropped;
	atomic64_t force_shrink_file_dropped;
	atomic64_t force_shrink_anon_skipped;
	atomic64_t force_shrink_file_skipped;
	atomic64_t force_shrink_anon_best_effort;
	atomic64_t force_shrink_anon_hook;
	atomic64_t writeback_queued;
	atomic64_t writeback_scheduled;
	atomic64_t writeback_success;
	atomic64_t writeback_error;
	atomic64_t writeback_skipped;
	atomic64_t writeback_no_data;
	atomic64_t writeback_noop;
	atomic64_t writeback_noop_pages;
	atomic64_t writeback_last_ret;
	atomic64_t writeback_last_pages;
	atomic64_t writeback_last_written_pages;
	atomic64_t writeback_last_jiffies;
	atomic64_t writeback_worker_runs;
	atomic64_t writeback_worker_total_ns;
	atomic64_t writeback_worker_max_ns;
	atomic64_t writeback_worker_slow_runs;
	atomic64_t writeback_worker_last_ns;
	atomic64_t batchin_worker_runs;
	atomic64_t batchin_worker_total_ns;
	atomic64_t batchin_worker_max_ns;
	atomic64_t batchin_worker_slow_runs;
	atomic64_t batchin_worker_last_ns;
	atomic64_t multi_zram_registered;
	atomic64_t multi_zram_last_selected;
	atomic64_t multi_zram_last_traversed;
	atomic64_t multi_zram_last_eligible;
	atomic64_t multi_zram_skip_no_backing;
	atomic64_t multi_zram_skip_limit;
	atomic64_t multi_zram_skip_no_resident;
	atomic64_t multi_zram_force_swapout_devices;
	atomic64_t multi_zram_force_swapin_devices;
	atomic64_t multi_zram_global_batchin_devices;
	atomic64_t policy_wakeups;
	atomic64_t policy_worker_runs;
	atomic64_t policy_writeback_queued;
	atomic64_t policy_writeback_skipped;
	atomic64_t auto_policy_runs;
	atomic64_t auto_writeback_queued;
	atomic64_t auto_writeback_skipped;
	atomic64_t last_auto_jiffies;
	atomic64_t last_auto_writeback_jiffies;
	atomic64_t last_auto_writeback_ret;
	atomic64_t last_auto_writeback_written_pages;
	atomic64_t policy_available_android_pages;
	atomic64_t policy_available_android_mb;
	atomic64_t policy_available_fallback;
	atomic64_t policy_zram_pressure_ratio;
	atomic64_t policy_zram_effective_ratio;
	atomic64_t policy_zram_wm_ratio;
	atomic64_t policy_zram_gate_result;
	atomic64_t policy_zram_resident_pages;
	atomic64_t policy_zram_total_pages;
	atomic64_t policy_zram_increase_pages;
	atomic64_t policy_zram_increase_boost_pages;
	atomic64_t policy_zram_increase_budget_pages;
	atomic64_t policy_empty_rounds;
	atomic64_t policy_empty_backoff_interval_ms;
	atomic64_t policy_empty_backoff_skipped;
	atomic64_t policy_window_start_jiffies;
	atomic64_t policy_window_written_pages;
	atomic64_t policy_window_throttled;
	atomic64_t auto_memcg_candidate_count;
	atomic64_t auto_per_memcg_queued;
	atomic64_t auto_per_memcg_success;
	atomic64_t auto_per_memcg_no_data;
	atomic64_t auto_per_memcg_error;
	atomic64_t auto_global_fallback;
	atomic64_t avail_buffers_writes;
	atomic64_t avail_buffers_wakeups;
	atomic64_t avail_buffers_low_events;
	atomic64_t avail_buffers_high_events;
	atomic64_t avail_buffers_swap_low_events;
	atomic64_t avail_buffers_last_avail;
	atomic64_t avail_buffers_last_min;
	atomic64_t avail_buffers_last_high;
	atomic64_t avail_buffers_last_free_swap_threshold;
	atomic64_t avail_buffers_effective_min;
	atomic64_t avail_buffers_effective_high;
	atomic64_t erm_avail_buffer_enable_store;
	atomic64_t erm_avail_buffer_writes;
	atomic64_t erm_avail_buffer_last_ret;
	atomic64_t avail_buffers_last_seen_avail;
	atomic64_t avail_buffers_last_free_swap_pages;
	atomic64_t pressure_registered;
	atomic64_t pressure_released;
	atomic64_t pressure_signaled;
	atomic64_t pressure_no_listener;
	atomic64_t pressure_signal_errors;
	atomic64_t pressure_low_signaled;
	atomic64_t pressure_medium_signaled;
	atomic64_t pressure_critical_signaled;
	atomic64_t pressure_last_ret;
	atomic64_t memcg_entries;
	atomic64_t memcg_param_updates;
	atomic64_t memcg_policy_parse_success;
	atomic64_t memcg_policy_parse_error;
	atomic64_t memcg_policy_apply;
	atomic64_t memcg_single_policy_parse_success;
	atomic64_t memcg_single_policy_parse_error;
};

struct zram;

struct crystal_hybridswap_zram {
	struct list_head node;
	struct device *dev;
	struct zram *zram;
	crystal_hybridswap_zram_writeback_t writeback;
	crystal_hybridswap_zram_writeback_ext_t writeback_ext;
	crystal_hybridswap_zram_force_writeback_t force_writeback;
	crystal_hybridswap_zram_force_writeback_ext_t force_writeback_ext;
	crystal_hybridswap_zram_batchin_t batchin;
	unsigned long zram_increase_pages;
	unsigned long registered_jiffies;
};

struct crystal_hybridswap_swapd_param {
	unsigned int min_score;
	unsigned int max_score;
	unsigned int ub_mem2zram_ratio;
	unsigned int ub_zram2ufs_ratio;
	unsigned int refault_threshold;
};

struct crystal_hybridswap_memcg {
	struct list_head node;
	struct cgroup_subsys_state *css;
	u64 cgroup_id;
	char name[CHS_MEMCG_NAME_MAX];
	atomic64_t app_score;
	atomic64_t app_uid;
	atomic64_t ub_ufs2zram_ratio;
	atomic_t ub_mem2zram_ratio;
	atomic_t ub_zram2ufs_ratio;
	atomic_t refault_threshold;
	atomic_t policy_level;
	atomic64_t force_swapin;
	atomic64_t force_swapin_pages;
	atomic64_t force_swapin_last_pages;
	atomic64_t force_swapin_last_effective_pages;
	atomic64_t force_swapin_last_batchin_pages;
	atomic64_t force_swapin_last_ret;
	atomic64_t force_swapin_last_request;
	atomic64_t force_swapin_last_read_errors;
	atomic64_t force_swapin_last_prepare_errors;
	atomic64_t force_swapout;
	atomic64_t force_swapout_pages;
	atomic64_t force_swapout_effective_pages;
	atomic64_t force_swapout_last_pages;
	atomic64_t force_swapout_last_effective_pages;
	atomic64_t force_swapout_last_mb;
	atomic64_t force_swapout_last_compat_trigger_value;
	atomic64_t force_swapout_last_scanned_pages;
	atomic64_t force_swapout_last_eligible_pages;
	atomic64_t force_swapout_last_written_pages;
	atomic64_t force_swapout_last_unknown_or_filtered_pages;
	atomic64_t force_swapout_last_ret;
	atomic64_t force_shrink_anon;
	atomic64_t force_shrink_file;
	atomic64_t aging_anon;
	atomic64_t force_shrink_anon_last_param;
	atomic64_t force_shrink_file_last_param;
	atomic64_t aging_anon_last_param;
	atomic64_t force_shrink_anon_pages;
	atomic64_t force_shrink_file_pages;
	atomic64_t force_shrink_anon_last_target;
	atomic64_t force_shrink_file_last_target;
	atomic64_t force_shrink_anon_last_batch;
	atomic64_t force_shrink_file_last_batch;
	atomic64_t force_shrink_anon_last_reclaimed;
	atomic64_t force_shrink_file_last_reclaimed;
	atomic64_t force_shrink_anon_last_ret;
	atomic64_t force_shrink_file_last_ret;
	atomic64_t force_shrink_anon_dropped;
	atomic64_t force_shrink_file_dropped;
	atomic64_t force_shrink_anon_skipped;
	atomic64_t force_shrink_file_skipped;
	atomic64_t pending_writeback_pages;
	char single_policy_raw[CHS_POLICY_RAW_MAX];
};

/*
 * Global state remains centralized to keep legacy stats/debug outputs stable.
 * Lock domains:
 * - state_lock protects last-operation snapshots and string fields.
 * - zram_lock protects zram registration plus pending writeback/batch-in work.
 * - atomic counters are intentionally sampled locklessly by stats paths.
 */
struct crystal_hybridswap_state {
	struct mutex state_lock; /* protects global configuration and snapshots */
	struct mutex zram_lock; /* protects zram registration and pending work */
	struct list_head zram_list;
	struct workqueue_struct *wq;
	struct work_struct writeback_work;
	struct work_struct batchin_work;
	struct delayed_work policy_work;
	atomic_t enabled;
	atomic_t core_enabled;
	atomic_t swapd_pause;
	atomic_t dev_life;
	atomic_t loglevel;
	atomic_t erm_avail_buffer_enable;
	atomic64_t quota_day;
	atomic64_t stored_wm_ratio;
	atomic64_t erm_min_avail_buffer;
	atomic64_t erm_high_avail_buffer;
	atomic_t erm_avail_buffer_valid;
	atomic64_t pending_policy_wakeups;
	atomic64_t pending_writeback_pages;
	atomic64_t pending_force_swapout;
	atomic64_t pending_force_swapout_pages;
	atomic64_t force_swapout_inflight;
	wait_queue_head_t force_swapout_wait;
	atomic64_t pending_batchin_pages;
	atomic64_t pending_force_swapin;
	struct device *pending_writeback_dev;
	struct zram *pending_writeback_zram;
	crystal_hybridswap_zram_writeback_t pending_writeback_fn;
	crystal_hybridswap_zram_writeback_ext_t pending_writeback_ext_fn;
	u64 pending_batchin_target_cgroup_id;
	s64 pending_batchin_app_score;
	char pending_batchin_memcg[CHS_MEMCG_NAME_MAX];
	char pending_writeback_mode[CHS_WB_MODE_MAX];
	bool pending_writeback_auto;
	atomic_t policy_suspended;
	atomic_t system_sleeping;
	char last_writeback_mode[CHS_WB_MODE_MAX];
	char loop_device[CHS_LOOP_DEVICE_MAX];
	char last_force_swapin_memcg[CHS_MEMCG_NAME_MAX];
	char last_force_swapout_memcg[CHS_MEMCG_NAME_MAX];
	u64 last_force_swapin_cgroup_id;
	u64 last_force_swapout_cgroup_id;
	s64 last_force_swapin_app_score;
	s64 last_force_swapout_app_score;
	s64 last_force_swapin_request;
	s64 last_force_swapout_mb;
	s64 last_force_swapin_pages;
	s64 last_force_swapout_pages;
	s64 last_force_swapin_effective_pages;
	s64 last_force_swapout_effective_pages;
	s64 last_force_swapout_compat_trigger_value;
	s64 last_force_swapout_scanned_pages;
	s64 last_force_swapout_eligible_pages;
	s64 last_force_swapout_written_pages;
	s64 last_force_swapout_unknown_or_filtered_pages;
	s64 last_force_swapout_ret;
	int last_pressure_level;
	int last_pressure_ret;
	char last_pressure_reason[CHS_PRESSURE_REASON_MAX];
	char last_auto_reason[CHS_PRESSURE_REASON_MAX];
	char auto_last_failure_reason[CHS_PRESSURE_REASON_MAX];
	unsigned long auto_last_empty_jiffies;
	unsigned long auto_empty_skip_jiffies;
	unsigned long auto_policy_window_start;
	unsigned long auto_last_failure_log_jiffies;
	u64 auto_policy_window_written_pages;
	unsigned long quota_window_start;
	u32 auto_last_failure_repeats;
	u64 quota_used_pages;
	s64 auto_last_writeback_result;
	s64 auto_last_failure_ret;
	struct crystal_hybridswap_zram_pressure last_zram_pressure;
	struct dentry *debugfs_root;
	struct crystal_hybridswap_stats stats;
};

extern struct crystal_hybridswap_state chs;

void crystal_hybridswap_stats_init(struct crystal_hybridswap_stats *stats);
void crystal_hybridswap_stats_show(struct seq_file *m);
void crystal_hybridswap_debugfs_init(void);
void crystal_hybridswap_debugfs_exit(void);

struct crystal_hybridswap_zram *
crystal_hybridswap_find_zram_locked(struct device *dev);

void crystal_hybridswap_set_enabled(bool enabled);
bool crystal_hybridswap_enabled(void);
void crystal_hybridswap_set_core_enabled(bool enabled);
bool crystal_hybridswap_core_enabled(void);
void crystal_hybridswap_set_swapd_pause(bool pause);
bool crystal_hybridswap_swapd_paused(void);
void crystal_hybridswap_set_dev_life(unsigned int level);
bool crystal_hybridswap_dev_life(void);
unsigned int crystal_hybridswap_dev_life_level(void);
void crystal_hybridswap_set_loglevel(int level);
int crystal_hybridswap_loglevel(void);
void crystal_hybridswap_set_quota_day(u64 quota);
u64 crystal_hybridswap_quota_day(void);
int crystal_hybridswap_set_loop_device(const char *buf, size_t len);
ssize_t crystal_hybridswap_get_loop_device(char *buf);
void crystal_hybridswap_record_loop_device_bind(int ret);
int crystal_hybridswap_set_zram_wm_ratio(s64 val);
s64 crystal_hybridswap_zram_wm_ratio(void);
int crystal_hybridswap_set_stored_wm_ratio(s64 val);
s64 crystal_hybridswap_stored_wm_ratio(void);
s64 crystal_hybridswap_mb_to_pages(s64 mb);
void crystal_hybridswap_account_physical_write_pages(unsigned long pages);
int crystal_hybridswap_queue_writeback(struct device *dev, const char *mode,
				       s64 pages);
int crystal_hybridswap_queue_force_swapout(u64 target_cgroup_id,
					   const char *memcg_name, s64 app_score,
					   s64 compat_trigger_value);
int crystal_hybridswap_queue_force_swapin(u64 target_cgroup_id,
		const char *memcg_name, s64 app_score, s64 request,
		s64 request_pages, s64 pages);
void crystal_hybridswap_record_force_swapin(const char *memcg_name,
					    u64 cgroup_id, s64 app_score,
					    s64 request, s64 request_pages,
					    s64 effective_pages);
void crystal_hybridswap_record_force_swapout(const char *memcg_name,
					     u64 cgroup_id, s64 app_score,
					     s64 compat_trigger_value);
void crystal_hybridswap_copy_last_writeback_mode(char *buf, size_t len);
void crystal_hybridswap_copy_last_auto_reason(char *buf, size_t len);
void chs_update_avail_buffer_view(struct chs_avail_buffer_view *view);
void crystal_hybridswap_queue_policy_wakeup(unsigned int avail,
					   unsigned int min_avail,
					   unsigned int high_avail,
					   u64 free_swap_threshold);
void crystal_hybridswap_update_auto_policy(void);
void crystal_hybridswap_suspend_auto_policy_sync(void);
void crystal_hybridswap_resume_auto_policy(void);
bool crystal_hybridswap_system_sleeping(void);
void crystal_hybridswap_drain_force_swapout(void);
void crystal_hybridswap_clear_pending_writeback_locked(void);

bool zram_try_get(struct zram *zram);
void zram_put(struct zram *zram);
int crystal_hybridswap_private_zram_register(struct device *dev,
		struct zram *zram);
void crystal_hybridswap_private_zram_unregister(struct device *dev);
int zram_driver_init(void);
void zram_driver_exit(void);
int zram_bind_backing_dev(struct device *dev, const char *buf, size_t len);
int zram_writeback_device(struct device *dev, const char *mode,
					       unsigned long nr_pages,
		struct crystal_hybridswap_writeback_stats *stats);
int zram_writeback_device_ext(struct device *dev, const char *mode,
					       unsigned long nr_pages,
					       bool auto_req,
		struct crystal_hybridswap_writeback_stats *stats);
int zram_force_writeback_device(struct device *dev, const char *mode,
					       unsigned long nr_pages, u64 target_cgroup_id,
					       struct crystal_hybridswap_writeback_stats *stats);
int zram_force_writeback_device_ext(struct device *dev, const char *mode,
					       unsigned long nr_pages, u64 target_cgroup_id,
					       bool auto_req,
					       struct crystal_hybridswap_writeback_stats *stats);
int zram_batchin_device(struct device *dev, unsigned long nr_pages,
		u64 target_cgroup_id,
		struct crystal_hybridswap_batchin_stats *stats);
int crystal_hybridswap_zram_pressure_snapshot(struct device *dev,
		struct crystal_hybridswap_zram_pressure *snapshot);
int crystal_hybridswap_zram_io_stats(struct device *dev,
		struct crystal_hybridswap_zram_io_stats *stats);
int crystal_hybridswap_collect_zram_io_stats(
		struct crystal_hybridswap_zram_io_stats *stats);
int crystal_hybridswap_zram_memcg_stats(struct device *dev, u64 cgroup_id,
		struct crystal_hybridswap_memcg_zram_stats *stats);
int crystal_hybridswap_collect_memcg_zram_stats(u64 cgroup_id,
		struct crystal_hybridswap_memcg_zram_stats *stats);

int crystal_hybridswap_memcg_init(void);
void crystal_hybridswap_memcg_exit(void);
struct crystal_hybridswap_memcg *
crystal_hybridswap_memcg_get(struct cgroup_subsys_state *css, bool create);
void crystal_hybridswap_memcg_record_force_swapout_result(u64 cgroup_id,
		unsigned long scanned_pages, unsigned long eligible_pages,
		unsigned long written_pages,
		unsigned long unknown_or_filtered_pages, int ret);
void crystal_hybridswap_memcg_record_force_swapin_result(u64 cgroup_id,
		unsigned long scanned_pages, unsigned long matched_pages,
		unsigned long moved_pages, unsigned long skipped_pages,
		unsigned long filtered_pages, unsigned long read_errors,
		unsigned long prepare_errors, unsigned long snapshot_mismatch,
		int ret);
int crystal_hybridswap_memcg_collect_auto_candidates(
		struct crystal_hybridswap_auto_memcg_candidate *candidates,
		int max_candidates, s64 total_budget_pages);
void crystal_hybridswap_memcg_stats_show(struct seq_file *m);

ssize_t crystal_hybridswap_swapd_pressure_write(struct kernfs_open_file *of,
						char *buf, size_t nbytes,
						loff_t off);
int crystal_hybridswap_report_pressure(unsigned int level, const char *reason);
void crystal_hybridswap_pressure_stats_show(struct seq_file *m);
void crystal_hybridswap_pressure_exit(void);

#endif /* _CRYSTAL_HYBRIDSWAP_INTERNAL_H */
