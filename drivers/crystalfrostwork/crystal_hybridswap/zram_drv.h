/*
 * Compressed RAM block device
 *
 * Copyright (C) 2008, 2009, 2010  Nitin Gupta
 *               2012, 2013 Minchan Kim
 *
 * This code is released using a dual license strategy: BSD/GPL
 * You can choose the licence that better fits your requirements.
 *
 * Released under the terms of 3-clause BSD License
 * Released under the terms of GNU General Public License Version 2.0
 *
 */

#ifndef _ZRAM_DRV_H_
#define _ZRAM_DRV_H_

#include <linux/completion.h>
#include <linux/hashtable.h>
#include <linux/refcount.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/zsmalloc.h>
#include <linux/crypto.h>

#include "zcomp.h"
#include "zms.h"

/*
 * Private zram has its own Kconfig symbols.  Some lightweight M= builds
 * reuse an older prepared output tree where these symbols are not yet present in
 * include/generated/autoconf.h, so keep sane source-level defaults here.  The
 * real full-tree config can still override them.
 */
#ifndef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_DEF_COMP
#define CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_DEF_COMP "lzo"
#endif
#ifndef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
#define CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK 1
#endif
#ifndef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_TRACK_ENTRY_ACTIME
#define CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_TRACK_ENTRY_ACTIME 1
#endif

#define SECTORS_PER_PAGE_SHIFT	(PAGE_SHIFT - SECTOR_SHIFT)
#define SECTORS_PER_PAGE	(1 << SECTORS_PER_PAGE_SHIFT)
#define ZRAM_LOGICAL_BLOCK_SHIFT 12
#define ZRAM_LOGICAL_BLOCK_SIZE	(1 << ZRAM_LOGICAL_BLOCK_SHIFT)
#define ZRAM_SECTOR_PER_LOGICAL_BLOCK	\
	(1 << (ZRAM_LOGICAL_BLOCK_SHIFT - SECTOR_SHIFT))


/*
 * ZRAM is mainly used for memory efficiency so we want to keep memory
 * footprint small and thus squeeze size and zram pageflags into a flags
 * member. The lower ZRAM_FLAG_SHIFT bits is for object size (excluding
 * header), which cannot be larger than PAGE_SIZE (requiring PAGE_SHIFT
 * bits), the higher bits are for zram_pageflags.
 *
 * We use BUILD_BUG_ON() to make sure that zram pageflags don't overflow.
 */
#define ZRAM_FLAG_SHIFT (PAGE_SHIFT + 1)

/* Only 2 bits are allowed for comp priority index */
#define ZRAM_COMP_PRIORITY_MASK	0x3

/* Flags for zram pages (table[page_no].flags) */
enum zram_pageflags {
	/* zram slot is locked */
	ZRAM_LOCK = ZRAM_FLAG_SHIFT,
	ZRAM_SAME,	/* Page consists the same element */
	ZRAM_WB,	/* page is stored on backing_device */
	ZRAM_UNDER_WB,	/* page is under writeback */
	ZRAM_HUGE,	/* Incompressible page */
	ZRAM_IDLE,	/* not accessed page since last idle marking */
	ZRAM_INCOMPRESSIBLE, /* none of the algorithms could compress it */
	ZRAM_PREFETCHED, /* page was brought back by ZMS prefetch */

	ZRAM_COMP_PRIORITY_BIT1, /* First bit of comp priority index */
	ZRAM_COMP_PRIORITY_BIT2, /* Second bit of comp priority index */

	__NR_ZRAM_PAGEFLAGS,
};

/*-- Data structures */

/* Allocated for each disk page */
struct zram_table_entry {
	union {
		unsigned long handle;
		unsigned long element;
	};
	unsigned long flags;
	u64 memcg_id;
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_TRACK_ENTRY_ACTIME
	ktime_t ac_time;
#endif
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
	unsigned long prefetch_jiffies;
#endif
};

struct zram_stats {
	atomic64_t compr_data_size;	/* compressed size of pages stored */
	atomic64_t failed_reads;	/* can happen when memory is too low */
	atomic64_t failed_writes;	/* can happen when memory is too low */
	atomic64_t notify_free;	/* no. of swap slot free notifications */
	atomic64_t same_pages;		/* no. of same element filled pages */
	atomic64_t huge_pages;		/* no. of huge pages */
	atomic64_t huge_pages_since;	/* no. of huge pages since zram set up */
	atomic64_t pages_stored;	/* no. of pages currently stored */
	atomic_long_t max_used_pages;	/* no. of maximum pages stored */
	atomic64_t writestall;		/* no. of write slow paths */
	atomic64_t miss_free;		/* no. of missed free */
#ifdef	CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
	atomic64_t bd_count;		/* no. of pages in backing device */
	atomic64_t bd_compr_data_size;	/* compressed bytes in backing device */
	atomic64_t bd_reads;		/* no. of logical pages read from backing device */
	atomic64_t bd_writes;		/* no. of logical pages written to backing device */
	atomic64_t bd_read_sync_ios;	/* synchronous backing reads */
	atomic64_t bd_read_async_ios;	/* asynchronous backing read submits */
	atomic64_t bd_read_failures;	/* synchronous backing read failures */
	atomic64_t bd_read_last_ret;	/* most recent backing read errno */
	atomic64_t bd_read_total_ns;	/* cumulative sync read latency */
	atomic64_t bd_read_max_ns;	/* max sync read latency */
	atomic64_t bd_read_slow_ios;	/* slow sync read count */
	atomic64_t bd_read_last_slow_dev;	/* zram disk minor */
	atomic64_t bd_read_last_slow_sector;
	atomic64_t bd_read_last_slow_index;
	atomic64_t bd_read_last_slow_ret;
	atomic64_t bd_read_last_slow_ns;
	atomic64_t bd_write_ios;	/* backing write submits */
	atomic64_t bd_write_failures;	/* backing write failures */
	atomic64_t bd_write_last_ret;	/* most recent backing write errno */
	atomic64_t bd_write_total_ns;	/* cumulative write latency */
	atomic64_t bd_write_max_ns;	/* max write latency */
	atomic64_t bd_write_slow_ios;	/* slow write count */
	atomic64_t bd_write_last_slow_dev;
	atomic64_t bd_write_last_slow_sector;
	atomic64_t bd_write_last_slow_index;
	atomic64_t bd_write_last_slow_ret;
	atomic64_t bd_write_last_slow_ns;
	atomic64_t batchin_runs;
	atomic64_t batchin_pages;
	atomic64_t batchin_failures;
	atomic64_t batchin_no_data;
	atomic64_t batchin_filtered_pages;
	atomic64_t batchin_read_errors;
	atomic64_t batchin_prepare_errors;
	atomic64_t batchin_snapshot_mismatch;
		atomic64_t batchin_total_ns;
		atomic64_t batchin_max_ns;
		atomic64_t batchin_slow_runs;
		atomic64_t batchin_last_ret;
		atomic64_t batchin_zms_batches;
		atomic64_t batchin_zms_items;
		atomic64_t batchin_zms_read_ios;
		atomic64_t prefetch_runs;
		atomic64_t prefetch_candidates;
		atomic64_t prefetch_submitted;
		atomic64_t prefetch_moved;
		atomic64_t prefetch_skipped;
		atomic64_t prefetch_read_errors;
		atomic64_t prefetch_prepare_errors;
		atomic64_t prefetch_snapshot_mismatch;
		atomic64_t prefetch_no_data;
		atomic64_t prefetch_alloc_failures;
		atomic64_t prefetch_hits;
		atomic64_t prefetch_stale_hits;
		atomic64_t prefetch_expired;
		atomic64_t prefetch_reclaimed;
		atomic64_t prefetch_invalidated;
		atomic64_t auto_wb_cold_age_ms;
	atomic64_t auto_wb_scan_pages;
	atomic64_t auto_wb_cold_selected;
	atomic64_t auto_wb_hot_skipped;
	atomic64_t auto_wb_kind_skipped;
	atomic64_t auto_wb_idle_selected;
	atomic64_t auto_wb_age_selected;
	atomic64_t auto_wb_unknown_age_skipped;
	atomic64_t auto_wb_last_age_ms;
	atomic64_t auto_wb_max_age_ms;
#endif
};

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_MULTI_COMP
#define ZRAM_PRIMARY_COMP	0U
#define ZRAM_SECONDARY_COMP	1U
#define ZRAM_MAX_COMPS	4U
#else
#define ZRAM_PRIMARY_COMP	0U
#define ZRAM_SECONDARY_COMP	0U
#define ZRAM_MAX_COMPS	1U
#endif

#define ZRAM_MEMCG_STATS_HASH_BITS	8

struct zram_memcg_stats_entry {
	struct hlist_node node;
	u64 cgroup_id;
	atomic64_t resident_pages;
	atomic64_t writeback_pages;
	atomic64_t same_pages;
	atomic64_t huge_pages;
	atomic64_t zram_compressed_size;
	atomic64_t zram_original_size;
	atomic64_t writeback_size;
	atomic64_t writeback_original_size;
};

struct zram {
	struct zram_table_entry *table;
	struct zs_pool *mem_pool;
	struct zcomp *comps[ZRAM_MAX_COMPS];
	struct gendisk *disk;
	/* Prevent concurrent execution of device init */
	struct rw_semaphore init_lock;
	/* Protect private zram lifetime independently from struct device refs. */
	spinlock_t ref_lock;
	refcount_t refcount;
	struct completion ref_completion;
	bool removing;
	/*
	 * the number of pages zram can consume for storing compressed data
	 */
	unsigned long limit_pages;

	struct zram_stats stats;
	/* Protects memcg_stats_table and per-memcg cached counters. */
	spinlock_t memcg_stats_lock;
	DECLARE_HASHTABLE(memcg_stats_table, ZRAM_MEMCG_STATS_HASH_BITS);
	/*
	 * This is the limit on amount of *uncompressed* worth of data
	 * we can store in a disk.
	 */
	u64 disksize;	/* bytes */
	const char *comp_algs[ZRAM_MAX_COMPS];
	s8 num_active_comps;
	/*
	 * zram is claimed so open request will be failed
	 */
	bool claim; /* Protected by disk->open_mutex */
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
	struct file *backing_dev;
	spinlock_t wb_limit_lock;
	bool wb_limit_enable;
	u64 bd_wb_limit;
	u32 prefetch_last_fault_index;
	u32 prefetch_prev_fault_index;
	u64 prefetch_last_fault_memcg_id;
	bool prefetch_fault_valid;
	struct block_device *bdev;
	struct zms *zms;
	struct work_struct zms_gc_work;
	struct delayed_work zms_gc_periodic_work;
	atomic_t zms_gc_pending;
	bool zms_gc_stopping;
#endif
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_MEMORY_TRACKING
	struct dentry *debugfs_dir;
#endif
};
#endif
