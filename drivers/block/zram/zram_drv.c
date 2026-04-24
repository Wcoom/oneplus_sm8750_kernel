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

#define KMSG_COMPONENT "zram"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/bio.h>
#include <linux/bitops.h>
#include <linux/cpu.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/device.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/backing-dev.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/sysfs.h>
#include <linux/debugfs.h>
#include <linux/cpuhotplug.h>
#include <linux/part_stat.h>
#include <linux/mm.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/sysms_finder.h>
#include <linux/suspend.h>

#ifdef CONFIG_ZRAM_AUTO_SIZE
#include <linux/math64.h>
	#include <linux/reciprocal_div.h>
	#include <linux/minmax.h>
	#include <linux/intfp.h>

	#ifndef FP_BITS
	#define FP_BITS 16
	#endif

#endif

#include "zram_drv.h"
#include "zram_wb.h"

#define CHECK_INTERVAL (180 * HZ) // 每180秒检查一次
#define ZRAM_AUTO_WRITEBACK_LIMIT_PAGES (64UL * 1024 * 1024 / PAGE_SIZE)

static struct task_struct *monitor_thread;
static struct task_struct *auto_writeback_thread;

static DEFINE_IDR(zram_index_idr);
/* idr index must be protected */
static DEFINE_MUTEX(zram_index_mutex);

static bool zram_get_single_device(struct zram **out_zram, struct device **out_dev)
{
	struct zram *zram;
	struct zram *single = NULL;
	struct device *dev = NULL;
	int id;

	mutex_lock(&zram_index_mutex);
	idr_for_each_entry(&zram_index_idr, zram, id) {
		if (single) {
			single = NULL;
			break;
		}
		single = zram;
	}

	if (single && single->disk) {
		dev = disk_to_dev(single->disk);
		if (!get_device(dev)) {
			single = NULL;
			dev = NULL;
		}
	} else {
		single = NULL;
	}
	mutex_unlock(&zram_index_mutex);

	if (!single)
		return false;

	*out_zram = single;
	*out_dev = dev;
	return true;
}

static int zram_major;
static const char *default_compressor = CONFIG_ZRAM_DEF_COMP;

/* Module params (documentation at end) */
static unsigned int num_devices = 1;
/*
 * Pages that compress to sizes equals or greater than this are stored
 * uncompressed in memory.
 */
static size_t huge_class_size;

static const struct block_device_operations zram_devops;

static int zram_read_page(struct zram *zram, struct page *page, u32 index,
			  struct bio *parent);
static unsigned long zram_auto_writeback(struct zram *zram,
					 unsigned long max_pages);
static int auto_writeback_func(void *data);
static int monitor_func(void *data);
static void zram_debugfs_create(void) {}
static void zram_debugfs_destroy(void) {}
static void zram_debugfs_register(struct zram *zram) {}
static void zram_debugfs_unregister(struct zram *zram) {}

#ifdef CONFIG_ZRAM_MULTI_COMP
u8 __read_mostly sysctl_zram_recomp_immediate = 1;

static struct ctl_table zram_sysctl_table[] = {
	{
		.procname	= "zram_recomp_immediate",
		.data		= &sysctl_zram_recomp_immediate,
		.maxlen		= sizeof(u8),
		.mode		= 0644,
		.proc_handler	= proc_dou8vec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_THREE,
	},
};
static struct ctl_table_header *zram_sysctl_table_header;
#endif //CONFIG_ZRAM_MULTI_COMP


static int zram_slot_trylock(struct zram *zram, u32 index)
{
	return bit_spin_trylock(ZRAM_LOCK, &zram->table[index].flags);
}

void zram_slot_lock(struct zram *zram, u32 index)
{
	bit_spin_lock(ZRAM_LOCK, &zram->table[index].flags);
}

void zram_slot_unlock(struct zram *zram, u32 index)
{
	bit_spin_unlock(ZRAM_LOCK, &zram->table[index].flags);
}

static inline bool init_done(struct zram *zram)
{
	return zram->disksize;
}

static inline struct zram *dev_to_zram(struct device *dev)
{
	return (struct zram *)dev_to_disk(dev)->private_data;
}

static unsigned long zram_get_handle(struct zram *zram, u32 index)
{
	return zram->table[index].handle;
}

void zram_set_handle(struct zram *zram, u32 index, unsigned long handle)
{
	zram->table[index].handle = handle;
}

/* flag operations require table entry bit_spin_lock() being held */
bool zram_test_flag(struct zram *zram, u32 index, enum zram_pageflags flag)
{
	return zram->table[index].flags & BIT(flag);
}

void zram_set_flag(struct zram *zram, u32 index, enum zram_pageflags flag)
{
	zram->table[index].flags |= BIT(flag);
}

static void zram_clear_flag(struct zram *zram, u32 index,
			enum zram_pageflags flag)
{
	zram->table[index].flags &= ~BIT(flag);
}

#define ZRAM_TEMP_BITS_MASK \
	(BIT(ZRAM_TEMP_0) | BIT(ZRAM_TEMP_1) | BIT(ZRAM_TEMP_2))
#define ZRAM_TEMP_MAX 7U
#define ZRAM_TEMP_INC_READ 2U

static inline unsigned int zram_get_temp_from_flags(unsigned long flags)
{
	return ((flags & BIT(ZRAM_TEMP_0)) ? 1U : 0U) |
	       ((flags & BIT(ZRAM_TEMP_1)) ? 2U : 0U) |
	       ((flags & BIT(ZRAM_TEMP_2)) ? 4U : 0U);
}

static inline unsigned int zram_get_temp_locked(struct zram *zram, u32 index)
{
	return zram_get_temp_from_flags(zram->table[index].flags);
}

static inline void zram_set_temp_locked(struct zram *zram, u32 index,
					unsigned int temp)
{
	unsigned long flags = zram->table[index].flags;

	if (temp > ZRAM_TEMP_MAX)
		temp = ZRAM_TEMP_MAX;

	flags &= ~ZRAM_TEMP_BITS_MASK;
	if (temp & 1U)
		flags |= BIT(ZRAM_TEMP_0);
	if (temp & 2U)
		flags |= BIT(ZRAM_TEMP_1);
	if (temp & 4U)
		flags |= BIT(ZRAM_TEMP_2);
	zram->table[index].flags = flags;
}

static inline void zram_raise_temp_locked(struct zram *zram, u32 index,
					unsigned int delta)
{
	unsigned int temp = zram_get_temp_locked(zram, index);

	if (delta >= ZRAM_TEMP_MAX - temp)
		temp = ZRAM_TEMP_MAX;
	else
		temp += delta;

	zram_set_temp_locked(zram, index, temp);
}

static inline void zram_promote_accessed(struct zram *zram, u32 index,
					 bool from_wb)
{
	unsigned int old_temp;
	unsigned int new_temp;
	const char *source = from_wb ? "wb" : "read";

	zram_slot_lock(zram, index);
	old_temp = zram_get_temp_locked(zram, index);
	if (from_wb)
		zram_set_temp_locked(zram, index, ZRAM_TEMP_MAX);
	else
		zram_raise_temp_locked(zram, index, ZRAM_TEMP_INC_READ);
	new_temp = zram_get_temp_locked(zram, index);
	zram_slot_unlock(zram, index);

	if (from_wb || old_temp == 0 || new_temp == ZRAM_TEMP_MAX)
		pr_info_ratelimited("zram %s promote index=%u source=%s temp=%u->%u\n",
				    zram->disk->disk_name, index, source,
				    old_temp, new_temp);
}

static size_t zram_get_obj_size(struct zram *zram, u32 index)
{
	return zram->table[index].flags & (BIT(ZRAM_FLAG_SHIFT) - 1);
}

static void zram_set_obj_size(struct zram *zram,
					u32 index, size_t size)
{
	unsigned long flags = zram->table[index].flags >> ZRAM_FLAG_SHIFT;

	zram->table[index].flags = (flags << ZRAM_FLAG_SHIFT) | size;
}

static inline bool zram_allocated(struct zram *zram, u32 index)
{
	return zram_get_obj_size(zram, index) ||
			zram_test_flag(zram, index, ZRAM_SAME) ||
			zram_test_flag(zram, index, ZRAM_WB);
}

static inline void update_used_max(struct zram *zram,
					const unsigned long pages)
{
	unsigned long cur_max = atomic_long_read(&zram->stats.max_used_pages);

	do {
		if (cur_max >= pages)
			return;
	} while (!atomic_long_try_cmpxchg(&zram->stats.max_used_pages,
					  &cur_max, pages));
}

static bool zram_can_store_page(struct zram *zram)
{
	unsigned long alloced_pages;

	alloced_pages = zs_get_total_pages(zram->mem_pool);
	update_used_max(zram, alloced_pages);

	return !zram->limit_pages || alloced_pages <= zram->limit_pages;
}


#if PAGE_SIZE != 4096
static inline bool is_partial_io(struct bio_vec *bvec)
{
	return bvec->bv_len != PAGE_SIZE;
}
#define ZRAM_PARTIAL_IO		1
#else
static inline bool is_partial_io(struct bio_vec *bvec)
{
	return false;
}
#endif

#define PAGE_WRITEBACK			0
#define HUGE_WRITEBACK			(1 << 0)
#define IDLE_WRITEBACK			(1 << 1)
#define INCOMPRESSIBLE_WRITEBACK	(1 << 2)

#ifdef	CONFIG_ZRAM_WRITEBACK
#define ZRAM_IDLE_WRITEBACK_BATCH	256

static void zram_lru_add(struct zram *zram, struct zram_table_entry *entry)
{
	entry->referenced = true;
	list_lru_add(&zram->zram_list_lru, &entry->lru);
}

static void zram_lru_del(struct zram *zram, struct zram_table_entry *entry)
{
	list_lru_del(&zram->zram_list_lru, &entry->lru);
}
#endif

static inline void zram_set_priority(struct zram *zram, u32 index, u32 prio)
{
	prio &= ZRAM_COMP_PRIORITY_MASK;
	/*
	 * Clear previous priority value first, in case if we recompress
	 * further an already recompressed page
	 */
	zram->table[index].flags &= ~(ZRAM_COMP_PRIORITY_MASK <<
				      ZRAM_COMP_PRIORITY_BIT1);
	zram->table[index].flags |= (prio << ZRAM_COMP_PRIORITY_BIT1);
}

static inline u32 zram_get_priority(struct zram *zram, u32 index)
{
	u32 prio = zram->table[index].flags >> ZRAM_COMP_PRIORITY_BIT1;

	return prio & ZRAM_COMP_PRIORITY_MASK;
}

static void zram_accessed(struct zram *zram, u32 index)
{
	/* Remove from LRU list if present */
	zram_lru_del(zram, &zram->table[index]);
	zram_clear_flag(zram, index, ZRAM_IDLE);
	zram_clear_flag(zram, index, ZRAM_PP_SLOT);
#ifdef CONFIG_ZRAM_TRACK_ENTRY_ACTIME
	zram->table[index].ac_time = ktime_get_boottime();
#endif
}

#if defined CONFIG_ZRAM_WRITEBACK || defined CONFIG_ZRAM_MULTI_COMP
static struct zram_pp_ctl *init_pp_ctl(void)
{
	struct zram_pp_ctl *ctl;
	u32 idx;

	ctl = kmalloc(sizeof(*ctl), GFP_KERNEL);
	if (!ctl)
		return NULL;

	init_completion(&ctl->all_done);
	atomic_set(&ctl->num_pp_slots, 0);
	for (idx = 0; idx < NUM_PP_BUCKETS; idx++)
		INIT_LIST_HEAD(&ctl->pp_buckets[idx]);
	return ctl;
}

static void remove_pp_slot_from_ctl(struct zram_pp_slot *pps)
{
	list_del_init(&pps->entry);
}

void free_pp_slot(struct zram *zram, struct zram_pp_slot *pps)
{
	zram_slot_lock(zram, pps->index);
	zram_clear_flag(zram, pps->index, ZRAM_PP_SLOT);
	zram_slot_unlock(zram, pps->index);

	kfree(pps);
}

static void release_pp_slot(struct zram *zram, struct zram_pp_slot *pps)
{
	remove_pp_slot_from_ctl(pps);
	free_pp_slot(zram, pps);
}

static void release_pp_ctl(struct zram *zram, struct zram_pp_ctl *ctl)
{
	u32 idx;

	if (!ctl)
		return;

	for (idx = 0; idx < NUM_PP_BUCKETS; idx++) {
		while (!list_empty(&ctl->pp_buckets[idx])) {
			struct zram_pp_slot *pps;

			pps = list_first_entry(&ctl->pp_buckets[idx],
					       struct zram_pp_slot,
					       entry);
			release_pp_slot(zram, pps);
		}
	}

	kfree(ctl);
}

static bool place_pp_slot(struct zram *zram, struct zram_pp_ctl *ctl,
			  u32 index)
{
	struct zram_pp_slot *pps;
	u32 bid;

	pps = kmalloc(sizeof(*pps), GFP_NOIO | __GFP_NOWARN);
	if (!pps)
		return false;

	INIT_LIST_HEAD(&pps->entry);
	pps->index = index;

	bid = zram_get_obj_size(zram, pps->index) / PP_BUCKET_SIZE_RANGE;
	list_add(&pps->entry, &ctl->pp_buckets[bid]);

	zram_set_flag(zram, pps->index, ZRAM_PP_SLOT);
	atomic_inc(&ctl->num_pp_slots);
	return true;
}

static bool zram_writeback_candidate_locked(struct zram *zram, u32 index,
					    u32 mode)
{
	unsigned int temp;

	if (!zram_allocated(zram, index))
		return false;

	if (zram_test_flag(zram, index, ZRAM_PP_SLOT) ||
	    zram_test_flag(zram, index, ZRAM_WB) ||
	    zram_test_flag(zram, index, ZRAM_SAME))
		return false;

	temp = zram_get_temp_locked(zram, index);
	if (temp > 0) {
		pr_info_ratelimited("zram %s writeback scan skip hot page index=%u temp=%u->%u\n",
				    zram->disk->disk_name, index, temp, temp - 1);
		zram_set_temp_locked(zram, index, temp - 1);
		return false;
	}

	if (mode & IDLE_WRITEBACK) {
		if (!zram_test_flag(zram, index, ZRAM_IDLE))
			return false;
		if (!zram_test_flag(zram, index, ZRAM_PAGE_ANON)) {
			pr_info_ratelimited("zram %s writeback scan skip non-anon page index=%u\n",
					    zram->disk->disk_name, index);
			return false;
		}
	}

	if ((mode & HUGE_WRITEBACK) &&
	    !zram_test_flag(zram, index, ZRAM_HUGE))
		return false;
	if ((mode & INCOMPRESSIBLE_WRITEBACK) &&
	    !zram_test_flag(zram, index, ZRAM_INCOMPRESSIBLE))
		return false;

	return true;
}

static bool zram_writeback_mode_match_locked(struct zram *zram, u32 index,
					     u32 mode)
{

	if (mode & IDLE_WRITEBACK) {
		if (!zram_test_flag(zram, index, ZRAM_IDLE))
			return false;
		if (!zram_test_flag(zram, index, ZRAM_PAGE_ANON)) {
			pr_info_ratelimited("zram %s writeback scan skip non-anon page index=%u\n",
					    zram->disk->disk_name, index);
			return false;
		}
	}

	if ((mode & HUGE_WRITEBACK) &&
	    !zram_test_flag(zram, index, ZRAM_HUGE))
		return false;
	if ((mode & INCOMPRESSIBLE_WRITEBACK) &&
	    !zram_test_flag(zram, index, ZRAM_INCOMPRESSIBLE))
		return false;

	return true;
}

#ifdef CONFIG_ZRAM_WRITEBACK
struct zram_idle_wb_ctx {
	struct zram *zram;
	u32 mode;
	unsigned long lo;
	unsigned long hi;
	unsigned int nr_collected;
	unsigned long indexes[ZRAM_IDLE_WRITEBACK_BATCH];
	bool stopped;
};

static enum lru_status zram_idle_wb_lru_cb(struct list_head *item,
					  struct list_lru_one *lru,
					  spinlock_t *lock, void *cb_arg)
{
	struct zram_idle_wb_ctx *ctx = cb_arg;
	struct zram_table_entry *entry;
	unsigned long index;
	(void)lock;

	entry = container_of(item, struct zram_table_entry, lru);
	index = entry - ctx->zram->table;

	if (ctx->nr_collected >= ARRAY_SIZE(ctx->indexes)) {
		ctx->stopped = true;
		return LRU_STOP;
	}

	if (index < ctx->lo || index >= ctx->hi)
		return LRU_SKIP;

	if (!zram_slot_trylock(ctx->zram, index))
		return LRU_SKIP;

	if (!zram_writeback_candidate_locked(ctx->zram, index, ctx->mode)) {
		zram_slot_unlock(ctx->zram, index);
		return LRU_ROTATE;
	}

	zram_set_flag(ctx->zram, index, ZRAM_PP_SLOT);
	ctx->indexes[ctx->nr_collected++] = index;
	zram_slot_unlock(ctx->zram, index);

	return LRU_ROTATE;
}

static bool place_marked_pp_slot(struct zram *zram, struct zram_pp_ctl *ctl,
					 u32 index, u32 mode)
{
	struct zram_pp_slot *pps;
	u32 bid;

	pps = kmalloc(sizeof(*pps), GFP_NOIO | __GFP_NOWARN);
	if (!pps) {
		zram_slot_lock(zram, index);
		if (zram_test_flag(zram, index, ZRAM_PP_SLOT))
			zram_clear_flag(zram, index, ZRAM_PP_SLOT);
		zram_slot_unlock(zram, index);
		return false;
	}

	zram_slot_lock(zram, index);
	if (!zram_allocated(zram, index) ||
	    zram_test_flag(zram, index, ZRAM_WB) ||
	    zram_test_flag(zram, index, ZRAM_SAME) ||
	    !zram_writeback_mode_match_locked(zram, index, mode) ||
	    !zram_test_flag(zram, index, ZRAM_PP_SLOT)) {
		zram_clear_flag(zram, index, ZRAM_PP_SLOT);
		zram_slot_unlock(zram, index);
		kfree(pps);
		return true;
	}

	bid = zram_get_obj_size(zram, index) / PP_BUCKET_SIZE_RANGE;
	zram_slot_unlock(zram, index);

	INIT_LIST_HEAD(&pps->entry);
	pps->index = index;
	list_add(&pps->entry, &ctl->pp_buckets[bid]);
	atomic_inc(&ctl->num_pp_slots);
	return true;
}

static void clear_marked_pp_slot(struct zram *zram, u32 index)
{
	zram_slot_lock(zram, index);
	if (zram_test_flag(zram, index, ZRAM_PP_SLOT))
		zram_clear_flag(zram, index, ZRAM_PP_SLOT);
	zram_slot_unlock(zram, index);
}

static int scan_slots_for_writeback_lru(struct zram *zram, u32 mode,
					unsigned long lo, unsigned long hi,
					struct zram_pp_ctl *ctl)
{
	struct zram_idle_wb_ctx ctx = {
		.zram = zram,
		.mode = mode,
		.lo = lo,
		.hi = hi,
	};
	unsigned long nr_to_walk;
	unsigned int i;

	for (;;) {
		nr_to_walk = list_lru_count(&zram->zram_list_lru);
		if (!nr_to_walk)
			break;

		ctx.nr_collected = 0;
		ctx.stopped = false;
		list_lru_walk(&zram->zram_list_lru, zram_idle_wb_lru_cb,
			      &ctx, nr_to_walk);

		for (i = 0; i < ctx.nr_collected; i++) {
			if (!place_marked_pp_slot(zram, ctl, ctx.indexes[i], mode)) {
				while (++i < ctx.nr_collected)
					clear_marked_pp_slot(zram, ctx.indexes[i]);
				return 0;
			}
		}

		if (!ctx.stopped)
			break;

		cond_resched();
	}

	return 0;
}
#endif

static struct zram_pp_slot *select_pp_slot(struct zram_pp_ctl *ctl)
{
	struct zram_pp_slot *pps = NULL;
	s32 idx = NUM_PP_BUCKETS - 1;

	/* The higher the bucket id the more optimal slot post-processing is */
	while (idx >= 0) {
		pps = list_first_entry_or_null(&ctl->pp_buckets[idx],
					       struct zram_pp_slot,
					       entry);
		if (pps)
			break;

		idx--;
	}
	return pps;
}
#endif

static inline void zram_fill_page(void *ptr, unsigned long len,
					unsigned long value)
{
	WARN_ON_ONCE(!IS_ALIGNED(len, sizeof(unsigned long)));
	memset_l(ptr, value, len / sizeof(unsigned long));
}

static bool page_same_filled(void *ptr, unsigned long *element)
{
	unsigned long *page;
	unsigned long val;
	unsigned int pos, last_pos = PAGE_SIZE / sizeof(*page) - 1;

	page = (unsigned long *)ptr;
	val = page[0];

	if (val != page[last_pos])
		return false;

	for (pos = 1; pos < last_pos; pos++) {
		if (val != page[pos])
			return false;
	}

	*element = val;

	return true;
}

static ssize_t initstate_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	u32 val;
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	val = init_done(zram);
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%u\n", val);
}

static ssize_t disksize_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return scnprintf(buf, PAGE_SIZE, "%llu\n", zram->disksize);
}

static ssize_t mem_limit_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	u64 limit;
	char *tmp;
	struct zram *zram = dev_to_zram(dev);

	limit = memparse(buf, &tmp);
	if (buf == tmp) /* no chars parsed, invalid input */
		return -EINVAL;

	down_write(&zram->init_lock);
	zram->limit_pages = PAGE_ALIGN(limit) >> PAGE_SHIFT;
	up_write(&zram->init_lock);

	return len;
}

static ssize_t mem_used_max_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	int err;
	unsigned long val;
	struct zram *zram = dev_to_zram(dev);

	err = kstrtoul(buf, 10, &val);
	if (err || val != 0)
		return -EINVAL;

	down_read(&zram->init_lock);
	if (init_done(zram)) {
		atomic_long_set(&zram->stats.max_used_pages,
				zs_get_total_pages(zram->mem_pool));
	}
	up_read(&zram->init_lock);

	return len;
}

/*
 * Mark all pages which are older than or equal to cutoff as IDLE.
 * Callers should hold the zram init lock in read mode
 */
static void mark_idle(struct zram *zram, ktime_t cutoff)
{
	unsigned long nr_pages = zram->disksize >> PAGE_SHIFT;
	int index;

	for (index = 0; index < nr_pages; index++) {
		/*
		 * Do not mark ZRAM_SAME slots as ZRAM_IDLE, because no
		 * post-processing (recompress, writeback) happens to the
		 * ZRAM_SAME slot.
		 *
		 * And ZRAM_WB slots simply cannot be ZRAM_IDLE.
		 */
		zram_slot_lock(zram, index);
		if (!zram_allocated(zram, index) ||
		    zram_test_flag(zram, index, ZRAM_WB) ||
		    zram_test_flag(zram, index, ZRAM_SAME)) {
			zram_slot_unlock(zram, index);
			continue;
		}

#ifdef CONFIG_ZRAM_TRACK_ENTRY_ACTIME
		if (!cutoff || ktime_after(cutoff, zram->table[index].ac_time)) {
			if (!zram_test_flag(zram, index, ZRAM_IDLE)) {
				zram_set_flag(zram, index, ZRAM_IDLE);
				/* Add to LRU list for shrinker */
				zram_lru_add(zram, &zram->table[index]);
			}
		} else {
			/* Page was accessed recently, make sure it's not marked as IDLE */
			if (zram_test_flag(zram, index, ZRAM_IDLE)) {
				zram_clear_flag(zram, index, ZRAM_IDLE);
				zram_lru_del(zram, &zram->table[index]);
			}
		}
#else
		/* When access time tracking is disabled, we only mark pages as IDLE
		 * if they are not already marked. This prevents re-adding pages
		 * to the LRU list unnecessarily.
		 */
		if (!zram_test_flag(zram, index, ZRAM_IDLE)) {
			zram_set_flag(zram, index, ZRAM_IDLE);
			/* Add to LRU list for shrinker */
			zram_lru_add(zram, &zram->table[index]);
		}
#endif
		zram_slot_unlock(zram, index);
	}
}

static void mark_idle_range(struct zram *zram, ktime_t cutoff,
			    unsigned long start, unsigned long nr_scan)
{
	unsigned long nr_pages = zram->disksize >> PAGE_SHIFT;
	unsigned long i;
	unsigned long scanned = 0;
	unsigned long idle_set = 0;
	unsigned long idle_cleared = 0;
	unsigned long skipped_unallocated = 0;
	unsigned long skipped_wb = 0;
	unsigned long skipped_same = 0;

	if (!nr_pages || !nr_scan)
		return;

	for (i = 0; i < nr_scan; i++) {
		unsigned long index = (start + i) % nr_pages;

		/*
		 * Do not mark ZRAM_SAME slots as ZRAM_IDLE, because no
		 * post-processing (recompress, writeback) happens to the
		 * ZRAM_SAME slot.
		 *
		 * And ZRAM_WB slots simply cannot be ZRAM_IDLE.
		 */
		scanned++;
		zram_slot_lock(zram, index);
		if (!zram_allocated(zram, index) ||
		    zram_test_flag(zram, index, ZRAM_WB) ||
		    zram_test_flag(zram, index, ZRAM_SAME)) {
			if (!zram_allocated(zram, index))
				skipped_unallocated++;
			else if (zram_test_flag(zram, index, ZRAM_WB))
				skipped_wb++;
			else
				skipped_same++;
			zram_slot_unlock(zram, index);
			continue;
		}

#ifdef CONFIG_ZRAM_TRACK_ENTRY_ACTIME
		if (!cutoff || ktime_after(cutoff, zram->table[index].ac_time)) {
			if (!zram_test_flag(zram, index, ZRAM_IDLE)) {
				zram_set_flag(zram, index, ZRAM_IDLE);
				/* Add to LRU list for shrinker */
				zram_lru_add(zram, &zram->table[index]);
				idle_set++;
			}
		} else {
			/* Page was accessed recently, make sure it's not marked as IDLE */
			if (zram_test_flag(zram, index, ZRAM_IDLE)) {
				zram_clear_flag(zram, index, ZRAM_IDLE);
				zram_lru_del(zram, &zram->table[index]);
				idle_cleared++;
			}
		}
#else
		/* When access time tracking is disabled, we only mark pages as IDLE
		 * if they are not already marked. This prevents re-adding pages
		 * to the LRU list unnecessarily.
		 */
		if (!zram_test_flag(zram, index, ZRAM_IDLE)) {
			zram_set_flag(zram, index, ZRAM_IDLE);
			/* Add to LRU list for shrinker */
			zram_lru_add(zram, &zram->table[index]);
			idle_set++;
		}
#endif
		zram_slot_unlock(zram, index);
	}

	pr_info("zram %s idle-scan range=%lu/%lu start=%lu next=%lu cutoff=%s scanned=%lu idle_set=%lu idle_cleared=%lu skip_unalloc=%lu skip_wb=%lu skip_same=%lu\n",
		zram->disk->disk_name, nr_scan, nr_pages, start,
		(start + nr_scan) % nr_pages,
		cutoff ? "age-filtered" : "all",
		scanned, idle_set, idle_cleared,
		skipped_unallocated, skipped_wb, skipped_same);
}

static ssize_t idle_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	ktime_t cutoff_time = 0;
	ssize_t rv = -EINVAL;

	if (!sysfs_streq(buf, "all")) {
		/*
		 * If it did not parse as 'all' try to treat it as an integer
		 * when we have memory tracking enabled.
		 */
		u64 age_sec;

		if (IS_ENABLED(CONFIG_ZRAM_TRACK_ENTRY_ACTIME) && !kstrtoull(buf, 0, &age_sec))
			cutoff_time = ktime_sub(ktime_get_boottime(),
					ns_to_ktime(age_sec * NSEC_PER_SEC));
		else
			goto out;
	}

	down_read(&zram->init_lock);
	if (!init_done(zram))
		goto out_unlock;

	/*
	 * A cutoff_time of 0 marks everything as idle, this is the
	 * "all" behavior.
	 */
	mark_idle(zram, cutoff_time);
	rv = len;

out_unlock:
	up_read(&zram->init_lock);
out:
	return rv;
}

#ifdef CONFIG_ZRAM_WRITEBACK
static ssize_t writeback_limit_enable_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	u64 val;
	ssize_t ret = -EINVAL;

	if (kstrtoull(buf, 10, &val))
		return ret;

	down_read(&zram->init_lock);
	spin_lock(&zram->wb_limit_lock);
	zram->wb_limit_enable = val;
	spin_unlock(&zram->wb_limit_lock);
	up_read(&zram->init_lock);
	ret = len;

	return ret;
}

static ssize_t writeback_limit_enable_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	bool val;
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	spin_lock(&zram->wb_limit_lock);
	val = zram->wb_limit_enable;
	spin_unlock(&zram->wb_limit_lock);
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t writeback_limit_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	u64 val;
	ssize_t ret = -EINVAL;

	if (kstrtoull(buf, 10, &val))
		return ret;

	down_read(&zram->init_lock);
	spin_lock(&zram->wb_limit_lock);
	zram->bd_wb_limit = val;
	spin_unlock(&zram->wb_limit_lock);
	up_read(&zram->init_lock);
	ret = len;

	return ret;
}

static ssize_t writeback_limit_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	u64 val;
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	spin_lock(&zram->wb_limit_lock);
	val = zram->bd_wb_limit;
	spin_unlock(&zram->wb_limit_lock);
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%llu\n", val);
}

static void reset_bdev(struct zram *zram)
{
	struct block_device *bdev;

	if (!zram->backing_dev)
		return;

	bdev = zram->bdev;
	blkdev_put(bdev, zram);
	/* hope filp_close flush all of IO */
	filp_close(zram->backing_dev, NULL);
	zram->backing_dev = NULL;
	zram->bdev = NULL;
	zram->disk->fops = &zram_devops;
	kvfree(zram->bitmap);
	zram->bitmap = NULL;
}

static ssize_t backing_dev_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct file *file;
	struct zram *zram = dev_to_zram(dev);
	char *p;
	ssize_t ret;

	down_read(&zram->init_lock);
	file = zram->backing_dev;
	if (!file) {
		memcpy(buf, "none\n", 5);
		up_read(&zram->init_lock);
		return 5;
	}

	p = file_path(file, buf, PAGE_SIZE - 1);
	if (IS_ERR(p)) {
		ret = PTR_ERR(p);
		goto out;
	}

	ret = strlen(p);
	memmove(buf, p, ret);
	buf[ret++] = '\n';
out:
	up_read(&zram->init_lock);
	return ret;
}

static ssize_t backing_dev_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	char *file_name;
	size_t sz;
	struct file *backing_dev = NULL;
	struct inode *inode;
	struct address_space *mapping;
	unsigned int bitmap_sz;
	unsigned long nr_pages, *bitmap = NULL;
	struct block_device *bdev = NULL;
	int err;
	struct zram *zram = dev_to_zram(dev);

	file_name = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!file_name)
		return -ENOMEM;

	down_write(&zram->init_lock);
	if (init_done(zram)) {
		pr_info("Can't setup backing device for initialized device\n");
		err = -EBUSY;
		goto out;
	}

	strscpy(file_name, buf, PATH_MAX);
	/* ignore trailing newline */
	sz = strlen(file_name);
	if (sz > 0 && file_name[sz - 1] == '\n')
		file_name[sz - 1] = 0x00;

	backing_dev = filp_open_block(file_name, O_RDWR|O_LARGEFILE, 0);
	if (IS_ERR(backing_dev)) {
		err = PTR_ERR(backing_dev);
		backing_dev = NULL;
		goto out;
	}

	mapping = backing_dev->f_mapping;
	inode = mapping->host;

	/* Support only block device in this moment */
	if (!S_ISBLK(inode->i_mode)) {
		err = -ENOTBLK;
		goto out;
	}

	bdev = blkdev_get_by_dev(inode->i_rdev, BLK_OPEN_READ | BLK_OPEN_WRITE,
				 zram, NULL);
	if (IS_ERR(bdev)) {
		err = PTR_ERR(bdev);
		bdev = NULL;
		goto out;
	}

	nr_pages = i_size_read(inode) >> PAGE_SHIFT;
	/* Refuse to use zero sized device (also prevents self reference) */
	if (!nr_pages) {
		err = -EINVAL;
		goto out;
	}

	bitmap_sz = BITS_TO_LONGS(nr_pages) * sizeof(long);
	bitmap = kvzalloc(bitmap_sz, GFP_KERNEL);
	if (!bitmap) {
		err = -ENOMEM;
		goto out;
	}

	reset_bdev(zram);

	zram->bdev = bdev;
	zram->backing_dev = backing_dev;
	zram->bitmap = bitmap;
	zram->nr_pages = nr_pages;
	up_write(&zram->init_lock);

	pr_info("setup backing device %s\n", file_name);
	kfree(file_name);

	return len;
out:
	kvfree(bitmap);

	if (bdev)
		blkdev_put(bdev, zram);

	if (backing_dev)
		filp_close(backing_dev, NULL);

	up_write(&zram->init_lock);

	kfree(file_name);

	return err;
}

static void read_from_bdev_async(struct zram *zram, struct page *page,
			unsigned long entry, struct bio *parent)
{
	struct bio *bio;

	bio = bio_alloc(zram->bdev, 1, parent->bi_opf, GFP_NOIO);
	bio->bi_iter.bi_sector = entry * (PAGE_SIZE >> 9);
	__bio_add_page(bio, page, PAGE_SIZE, 0);
	bio_chain(bio, parent);
	submit_bio(bio);
}

static int zram_writeback_slots(struct zram *zram, struct zram_pp_ctl *ctl,
				 unsigned long max_pages,
				 unsigned long *pages_written)
{
	unsigned long blk_idx = 0;
	struct zram_pp_slot *pps;
	int ret = 0;
	unsigned long written = 0;
	u32 index;
	int nr_pps = atomic_read(&ctl->num_pp_slots);

	if (!nr_pps)
		return 0;

	if (pages_written)
		*pages_written = 0;

	while ((pps = select_pp_slot(ctl))) {
		struct zram_wb_request *req;
		struct page *page;

		if (max_pages && written >= max_pages)
			break;

		spin_lock(&zram->wb_limit_lock);
		if (zram->wb_limit_enable && !zram->bd_wb_limit) {
			spin_unlock(&zram->wb_limit_lock);
			ret = -EIO;
			break;
		}
		spin_unlock(&zram->wb_limit_lock);

		if (!blk_idx) {
			blk_idx = alloc_block_bdev(zram);
			if (!blk_idx) {
				ret = -ENOSPC;
				break;
			}
		}

		req = alloc_wb_request(zram, pps, ctl, blk_idx);
		if (IS_ERR(req)) {
			ret = PTR_ERR(req);
			break;
		}
		page = bio_first_page_all(req->bio);

		index = pps->index;
		zram_slot_lock(zram, index);
		/*
		 * scan_slots() sets ZRAM_PP_SLOT and relases slot lock, so
		 * slots can change in the meantime. If slots are accessed or
		 * freed they lose ZRAM_PP_SLOT flag and hence we don't
		 * post-process them.
		 */
		if (!zram_test_flag(zram, index, ZRAM_PP_SLOT))
			goto next;
		zram_slot_unlock(zram, index);

		if (zram_read_page(zram, page, index, NULL)) {
			release_pp_slot(zram, pps);
			continue;
		}

		nr_pps--;
		written++;
		if (pages_written)
			(*pages_written)++;
		remove_pp_slot_from_ctl(pps);
		blk_idx = 0;
		submit_bio(req->bio);
		continue;

next:
		zram_slot_unlock(zram, index);
		release_pp_slot(zram, pps);
		free_wb_request(req);

		cond_resched();
	}

	if (blk_idx)
		free_block_bdev(zram, blk_idx);

	if (nr_pps && atomic_sub_and_test(nr_pps, &ctl->num_pp_slots))
		complete(&ctl->all_done);

	/* wait until all async bios completed */
	wait_for_completion(&ctl->all_done);

	return ret;
}
static int parse_page_index(char *val, unsigned long nr_pages,
			    unsigned long *lo, unsigned long *hi)
{
	int ret;

	ret = kstrtoul(val, 10, lo);
	if (ret)
		return ret;
	if (*lo >= nr_pages)
		return -ERANGE;
	*hi = *lo + 1;
	return 0;
}

static int parse_page_indexes(char *val, unsigned long nr_pages,
			      unsigned long *lo, unsigned long *hi)
{
	char *delim;
	int ret;

	delim = strchr(val, '-');
	if (!delim)
		return -EINVAL;

	*delim = 0x00;
	ret = kstrtoul(val, 10, lo);
	if (ret)
		return ret;
	if (*lo >= nr_pages)
		return -ERANGE;

	ret = kstrtoul(delim + 1, 10, hi);
	if (ret)
		return ret;
	if (*hi >= nr_pages || *lo > *hi)
		return -ERANGE;
	*hi += 1;
	return 0;
}

static int parse_mode(char *val, u32 *mode)
{
	*mode = 0;

	if (!strcmp(val, "idle"))
		*mode = IDLE_WRITEBACK;
	if (!strcmp(val, "huge"))
		*mode = HUGE_WRITEBACK;
	if (!strcmp(val, "huge_idle"))
		*mode = IDLE_WRITEBACK | HUGE_WRITEBACK;
	if (!strcmp(val, "incompressible"))
		*mode = INCOMPRESSIBLE_WRITEBACK;

	if (*mode == 0)
		return -EINVAL;
	return 0;
}

static int scan_slots_for_writeback(struct zram *zram, u32 mode,
				    unsigned long lo, unsigned long hi,
				    struct zram_pp_ctl *ctl)
{
	#ifdef CONFIG_ZRAM_WRITEBACK
	if (mode & IDLE_WRITEBACK)
		return scan_slots_for_writeback_lru(zram, mode, lo, hi, ctl);
	#endif

	u32 index = lo;

	while (index < hi) {
		bool ok = true;

		zram_slot_lock(zram, index);
		if (!zram_writeback_candidate_locked(zram, index, mode))
			goto next;

		ok = place_pp_slot(zram, ctl, index);
next:
		zram_slot_unlock(zram, index);
		if (!ok)
			break;
		index++;
	}

	return 0;
}

static ssize_t writeback_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	u64 nr_pages = zram->disksize >> PAGE_SHIFT;
	unsigned long lo = 0, hi = nr_pages;
	struct zram_pp_ctl *ctl = NULL;
	char *args, *param, *val;
	ssize_t ret = len;
	int err, mode = 0;

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		up_read(&zram->init_lock);
		return -EINVAL;
	}

	/* Do not permit concurrent post-processing actions. */
	if (atomic_xchg(&zram->pp_in_progress, 1)) {
		up_read(&zram->init_lock);
		return -EAGAIN;
	}

	if (!zram->backing_dev) {
		ret = -ENODEV;
		goto release_init_lock;
	}

	ctl = init_pp_ctl();
	if (!ctl) {
		ret = -ENOMEM;
		goto release_init_lock;
	}

	args = skip_spaces(buf);
	while (*args) {
		args = next_arg(args, &param, &val);

		/*
		 * Workaround to support the old writeback interface.
		 *
		 * The old writeback interface has a minor inconsistency and
		 * requires key=value only for page_index parameter, while the
		 * writeback mode is a valueless parameter.
		 *
		 * This is not the case anymore and now all parameters are
		 * required to have values, however, we need to support the
		 * legacy writeback interface format so we check if we can
		 * recognize a valueless parameter as the (legacy) writeback
		 * mode.
		 */
		if (!val || !*val) {
			err = parse_mode(param, &mode);
			if (err) {
				ret = err;
				goto release_init_lock;
			}

			scan_slots_for_writeback(zram, mode, lo, hi, ctl);
			break;
		}

		if (!strcmp(param, "type")) {
			err = parse_mode(val, &mode);
			if (err) {
				ret = err;
				goto release_init_lock;
			}

			scan_slots_for_writeback(zram, mode, lo, hi, ctl);
			break;
		}

		if (!strcmp(param, "page_index")) {
			err = parse_page_index(val, nr_pages, &lo, &hi);
			if (err) {
				ret = err;
				goto release_init_lock;
			}

			scan_slots_for_writeback(zram, mode, lo, hi, ctl);
			continue;
		}

		if (!strcmp(param, "page_indexes")) {
			err = parse_page_indexes(val, nr_pages, &lo, &hi);
			if (err) {
				ret = err;
				goto release_init_lock;
			}

			scan_slots_for_writeback(zram, mode, lo, hi, ctl);
			continue;
		}
	}

	err = zram_writeback_slots(zram, ctl, 0, NULL);
	if (err)
		ret = err;

release_init_lock:
	release_pp_ctl(zram, ctl);
	atomic_set(&zram->pp_in_progress, 0);
	up_read(&zram->init_lock);

	return ret;
}

struct zram_work {
	struct work_struct work;
	struct zram *zram;
	unsigned long entry;
	struct page *page;
	int error;
};

static void zram_sync_read(struct work_struct *work)
{
	struct zram_work *zw = container_of(work, struct zram_work, work);
	struct bio_vec bv;
	struct bio bio;

	bio_init(&bio, zw->zram->bdev, &bv, 1, REQ_OP_READ);
	bio.bi_iter.bi_sector = zw->entry * (PAGE_SIZE >> 9);
	__bio_add_page(&bio, zw->page, PAGE_SIZE, 0);
	zw->error = submit_bio_wait(&bio);
}

/*
 * Block layer want one ->submit_bio to be active at a time, so if we use
 * chained IO with parent IO in same context, it's a deadlock. To avoid that,
 * use a worker thread context.
 */
static int read_from_bdev_sync(struct zram *zram, struct page *page,
				unsigned long entry)
{
	struct zram_work work;

	work.page = page;
	work.zram = zram;
	work.entry = entry;

	INIT_WORK_ONSTACK(&work.work, zram_sync_read);
	queue_work(system_unbound_wq, &work.work);
	flush_work(&work.work);
	destroy_work_on_stack(&work.work);

	return work.error;
}

static int read_from_bdev(struct zram *zram, struct page *page,
			unsigned long entry, struct bio *parent)
{
	atomic64_inc(&zram->stats.bd_reads);
	if (!parent) {
		if (WARN_ON_ONCE(!IS_ENABLED(ZRAM_PARTIAL_IO)))
			return -EIO;
		return read_from_bdev_sync(zram, page, entry);
	}
	read_from_bdev_async(zram, page, entry, parent);
	return 0;
}
#else
static inline void reset_bdev(struct zram *zram) {};
static int read_from_bdev(struct zram *zram, struct page *page,
			unsigned long entry, struct bio *parent)
{
	return -EIO;
}
#endif

static ssize_t max_comp_streams_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", num_online_cpus());
}

static ssize_t max_comp_streams_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	return len;
}

static void comp_algorithm_set(struct zram *zram, u32 prio, const char *alg)
{
	if (zram->comp_algs[prio] && zram->comp_algs[prio] != default_compressor)
		kfree(zram->comp_algs[prio]);
	zram->comp_algs[prio] = alg;
}

static int __comp_algorithm_store(struct zram *zram, u32 prio, const char *alg)
{
	char *compressor;
	size_t sz;

	if (!alg)
		return -EINVAL;

	sz = strlen(alg);
	if (sz >= CRYPTO_MAX_ALG_NAME)
		return -E2BIG;

	compressor = kstrdup(alg, GFP_KERNEL);
	if (!compressor)
		return -ENOMEM;

	if (sz > 0 && compressor[sz - 1] == '\n')
		compressor[sz - 1] = 0x00;

	if (!zcomp_available_algorithm(compressor)) {
		kfree(compressor);
		return -EINVAL;
	}

	down_write(&zram->init_lock);
	if (init_done(zram)) {
		up_write(&zram->init_lock);
		kfree(compressor);
		pr_info("Can't change algorithm for initialized device\n");
		return -EBUSY;
	}

	comp_algorithm_set(zram, prio, compressor);
	up_write(&zram->init_lock);
	return 0;
}

static ssize_t __comp_algorithm_show(struct zram *zram, u32 prio, char *buf)
{
	ssize_t sz;
	const char *alg;

	down_read(&zram->init_lock);
	alg = zram->comp_algs[prio] ? zram->comp_algs[prio] : default_compressor;
	sz = zcomp_available_show(alg, buf);
	up_read(&zram->init_lock);

	return sz;
}

static ssize_t comp_algorithm_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return __comp_algorithm_show(zram, ZRAM_PRIMARY_COMP, buf);
}

static ssize_t comp_algorithm_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	int ret;

	ret = __comp_algorithm_store(zram, ZRAM_PRIMARY_COMP, buf);
	return ret ? ret : len;
}

#ifdef CONFIG_ZRAM_MULTI_COMP
static ssize_t recomp_algorithm_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t sz = 0;
	u32 prio;

	for (prio = ZRAM_SECONDARY_COMP; prio < ZRAM_MAX_COMPS; prio++) {
		if (!zram->comp_algs[prio])
			continue;

		sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2, "#%d: ", prio);
		sz += __comp_algorithm_show(zram, prio, buf + sz);
	}

	return sz;
}

static ssize_t recomp_algorithm_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	int prio = ZRAM_SECONDARY_COMP;
	char *args, *param, *val;
	char *alg = NULL;
	int ret;

	args = skip_spaces(buf);
	while (*args) {
		args = next_arg(args, &param, &val);

		if (!val || !*val)
			return -EINVAL;

		if (!strcmp(param, "algo")) {
			alg = val;
			continue;
		}

		if (!strcmp(param, "priority")) {
			ret = kstrtoint(val, 10, &prio);
			if (ret)
				return ret;
			continue;
		}
	}

	if (!alg)
		return -EINVAL;

	if (prio < ZRAM_SECONDARY_COMP || prio >= ZRAM_MAX_COMPS)
		return -EINVAL;

	ret = __comp_algorithm_store(zram, prio, alg);
	return ret ? ret : len;
}
#endif

static ssize_t compact_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		up_read(&zram->init_lock);
		return -EINVAL;
	}

	zs_compact(zram->mem_pool);
	up_read(&zram->init_lock);

	return len;
}

static ssize_t io_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t ret;

	down_read(&zram->init_lock);
	ret = scnprintf(buf, PAGE_SIZE,
			"%8llu %8llu 0 %8llu\n",
			(u64)atomic64_read(&zram->stats.failed_reads),
			(u64)atomic64_read(&zram->stats.failed_writes),
			(u64)atomic64_read(&zram->stats.notify_free));
	up_read(&zram->init_lock);

	return ret;
}

static ssize_t mm_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	struct zs_pool_stats pool_stats;
	u64 orig_size, mem_used = 0;
	long max_used;
	ssize_t ret;

	memset(&pool_stats, 0x00, sizeof(struct zs_pool_stats));

	down_read(&zram->init_lock);
	if (init_done(zram)) {
		mem_used = zs_get_total_pages(zram->mem_pool);
		zs_pool_stats(zram->mem_pool, &pool_stats);
	}

	orig_size = atomic64_read(&zram->stats.pages_stored);
	max_used = atomic_long_read(&zram->stats.max_used_pages);

	ret = scnprintf(buf, PAGE_SIZE,
			"%8llu %8llu %8llu %8lu %8ld %8llu %8lu %8llu %8llu\n",
			orig_size << PAGE_SHIFT,
			(u64)atomic64_read(&zram->stats.compr_data_size),
			mem_used << PAGE_SHIFT,
			zram->limit_pages << PAGE_SHIFT,
			max_used << PAGE_SHIFT,
			(u64)atomic64_read(&zram->stats.same_pages),
			atomic_long_read(&pool_stats.pages_compacted),
			(u64)atomic64_read(&zram->stats.huge_pages),
			(u64)atomic64_read(&zram->stats.huge_pages_since));
	up_read(&zram->init_lock);

	return ret;
}

#ifdef CONFIG_ZRAM_WRITEBACK
#define FOUR_K(x) ((x) * (1 << (PAGE_SHIFT - 12)))
static ssize_t bd_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t ret;

	down_read(&zram->init_lock);
	ret = scnprintf(buf, PAGE_SIZE,
		"%8llu %8llu %8llu\n",
			FOUR_K((u64)atomic64_read(&zram->stats.bd_count)),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_reads)),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_writes)));
	up_read(&zram->init_lock);

	return ret;
}
#endif

static ssize_t debug_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int version = 1;
	struct zram *zram = dev_to_zram(dev);
	ssize_t ret;

	down_read(&zram->init_lock);
	ret = scnprintf(buf, PAGE_SIZE,
			"version: %d\n%8llu %8llu\n",
			version,
			(u64)atomic64_read(&zram->stats.writestall),
			(u64)atomic64_read(&zram->stats.miss_free));
	up_read(&zram->init_lock);

	return ret;
}

static DEVICE_ATTR_RO(io_stat);
static DEVICE_ATTR_RO(mm_stat);
#ifdef CONFIG_ZRAM_WRITEBACK
static DEVICE_ATTR_RO(bd_stat);
#endif
static DEVICE_ATTR_RO(debug_stat);

static void zram_meta_free(struct zram *zram, u64 disksize)
{
	size_t num_pages = disksize >> PAGE_SHIFT;
	size_t index;

	if (!zram->table)
		return;

	destroy_device_zram_writeback(zram);

	/* Free all pages that are still in this zram device */
	for (index = 0; index < num_pages; index++)
		zram_free_page(zram, index);

	zs_destroy_pool(zram->mem_pool);
	zram->mem_pool = NULL;
	
	/* Destroy the per-device idle LRU */
	list_lru_destroy(&zram->zram_list_lru);
	
	vfree(zram->table);
	zram->table = NULL;
}

static bool zram_meta_alloc(struct zram *zram, u64 disksize)
{
	size_t num_pages;
	u32 index;

	num_pages = disksize >> PAGE_SHIFT;
	zram->table = vzalloc(array_size(num_pages, sizeof(*zram->table)));
	if (!zram->table)
		return false;

	/* Initialize the LRU list heads for each table entry */
	for (index = 0; index < num_pages; index++)
		INIT_LIST_HEAD(&zram->table[index].lru);

	zram->mem_pool = zs_create_pool(zram->disk->disk_name);
	if (!zram->mem_pool) {
		vfree(zram->table);
		zram->table = NULL;
		return false;
	}

	if (!huge_class_size)
		huge_class_size = zs_huge_class_size(zram->mem_pool);
	
	/* Initialize the per-device idle LRU */
	if (list_lru_init(&zram->zram_list_lru)) {
		zs_destroy_pool(zram->mem_pool);
		zram->mem_pool = NULL;
		vfree(zram->table);
		zram->table = NULL;
		return false;
	}

	if (init_device_zram_writeback(zram)) {
		list_lru_destroy(&zram->zram_list_lru);
		zs_destroy_pool(zram->mem_pool);
		zram->mem_pool = NULL;
		vfree(zram->table);
		zram->table = NULL;
		return false;
	}
	
	return true;
}

/*
 * To protect concurrent access to the same index entry,
 * caller should hold this table index entry's bit_spinlock to
 * indicate this index entry is accessing.
 */
void zram_free_page(struct zram *zram, size_t index)
{
	unsigned long handle;

#ifdef CONFIG_ZRAM_TRACK_ENTRY_ACTIME
	zram->table[index].ac_time = 0;
#endif
	/* Remove from LRU list if present */
	if (zram_test_flag(zram, index, ZRAM_IDLE))
		zram_lru_del(zram, &zram->table[index]);

	zram_clear_flag(zram, index, ZRAM_IDLE);
	zram_clear_flag(zram, index, ZRAM_INCOMPRESSIBLE);
	zram_clear_flag(zram, index, ZRAM_TEMP_0);
	zram_clear_flag(zram, index, ZRAM_TEMP_1);
	zram_clear_flag(zram, index, ZRAM_TEMP_2);
	zram_clear_flag(zram, index, ZRAM_PP_SLOT);
	zram_clear_flag(zram, index, ZRAM_PAGE_ANON);
	zram_clear_flag(zram, index, ZRAM_PAGE_FILE);
	zram_clear_flag(zram, index, ZRAM_PAGE_DIRTY);
	zram_set_priority(zram, index, 0);

	if (zram_test_flag(zram, index, ZRAM_HUGE)) {
		zram_clear_flag(zram, index, ZRAM_HUGE);
		atomic64_dec(&zram->stats.huge_pages);
	}

	if (zram_test_flag(zram, index, ZRAM_WB)) {
		zram_clear_flag(zram, index, ZRAM_WB);
		free_block_bdev(zram, zram_get_handle(zram, index));
		goto out;
	}

	/*
	 * No memory is allocated for same element filled pages.
	 * Simply clear same page flag.
	 */
	if (zram_test_flag(zram, index, ZRAM_SAME)) {
		zram_clear_flag(zram, index, ZRAM_SAME);
		atomic64_dec(&zram->stats.same_pages);
		goto out;
	}

	handle = zram_get_handle(zram, index);
	if (!handle)
		return;

	zs_free(zram->mem_pool, handle);

	atomic64_sub(zram_get_obj_size(zram, index),
			 &zram->stats.compr_data_size);
out:
	atomic64_dec(&zram->stats.pages_stored);
	zram_set_handle(zram, index, 0);
	zram_set_obj_size(zram, index, 0);
	WARN_ON_ONCE(zram->table[index].flags & ~(1UL << ZRAM_LOCK));
}

static int read_same_filled_page(struct zram *zram, struct page *page,
				 u32 index)
{
	void *mem;

	mem = kmap_local_page(page);
	zram_fill_page(mem, PAGE_SIZE, zram_get_handle(zram, index));
	kunmap_local(mem);
	return 0;
}

static int read_incompressible_page(struct zram *zram, struct page *page,
				    u32 index)
{
	unsigned long handle;
	void *src, *dst;

	handle = zram_get_handle(zram, index);
	src = zs_map_object(zram->mem_pool, handle, ZS_MM_RO);
	dst = kmap_local_page(page);
	copy_page(dst, src);
	kunmap_local(dst);
	zs_unmap_object(zram->mem_pool, handle);

	return 0;
}

static int read_compressed_page(struct zram *zram, struct page *page, u32 index)
{
	struct zcomp_strm *zstrm;
	unsigned long handle;
	unsigned int size;
	void *src, *dst;
	int ret, prio;

	handle = zram_get_handle(zram, index);
	size = zram_get_obj_size(zram, index);
	prio = zram_get_priority(zram, index);

	zstrm = zcomp_stream_get(zram->comps[prio]);
	src = zs_map_object(zram->mem_pool, handle, ZS_MM_RO);
	dst = kmap_local_page(page);
	ret = zcomp_decompress(zstrm, src, size, dst);
	kunmap_local(dst);
	zs_unmap_object(zram->mem_pool, handle);
	zcomp_stream_put(zram->comps[prio]);

	return ret;
}

/*
 * Reads (decompresses if needed) a page from zspool (zsmalloc).
 * Corresponding ZRAM slot should be locked.
 */
static int zram_read_from_zspool(struct zram *zram, struct page *page,
				 u32 index)
{
	if (zram_test_flag(zram, index, ZRAM_SAME) ||
	    !zram_get_handle(zram, index))
		return read_same_filled_page(zram, page, index);

	if (!zram_test_flag(zram, index, ZRAM_HUGE))
		return read_compressed_page(zram, page, index);
	else
		return read_incompressible_page(zram, page, index);
}

static int zram_read_page(struct zram *zram, struct page *page, u32 index,
			  struct bio *parent)
{
	int ret;
	bool from_wb;

	zram_slot_lock(zram, index);
	from_wb = zram_test_flag(zram, index, ZRAM_WB);
	if (!from_wb) {
		/* Slot should be locked through out the function call */
		ret = zram_read_from_zspool(zram, page, index);
		zram_slot_unlock(zram, index);
	} else {
		/*
		 * The slot should be unlocked before reading from the backing
		 * device.
		 */
		zram_slot_unlock(zram, index);

		ret = read_from_bdev(zram, page, zram_get_handle(zram, index),
				     parent);
	}

	if (!ret)
		zram_promote_accessed(zram, index, from_wb);

	/* Should NEVER happen. Return bio error if it does. */
	if (WARN_ON(ret < 0))
		pr_err("Decompression failed! err=%d, page=%u\n", ret, index);

	return ret;
}

/*
 * Use a temporary buffer to decompress the page, as the decompressor
 * always expects a full page for the output.
 */
static int zram_bvec_read_partial(struct zram *zram, struct bio_vec *bvec,
				  u32 index, int offset)
{
	struct page *page = alloc_page(GFP_NOIO);
	int ret;

	if (!page)
		return -ENOMEM;
	ret = zram_read_page(zram, page, index, NULL);
	if (likely(!ret))
		memcpy_to_bvec(bvec, page_address(page) + offset);
	__free_page(page);
	return ret;
}

static int zram_bvec_read(struct zram *zram, struct bio_vec *bvec,
			  u32 index, int offset, struct bio *bio)
{
	if (is_partial_io(bvec))
		return zram_bvec_read_partial(zram, bvec, index, offset);
	return zram_read_page(zram, bvec->bv_page, index, bio);
}

static int write_same_filled_page(struct zram *zram, unsigned long fill,
				  u32 index, struct page *page)
{
	zram_slot_lock(zram, index);
	zram_set_flag(zram, index, ZRAM_SAME);
	zram_set_handle(zram, index, fill);

	if (PageAnon(page)) {
		zram_set_flag(zram, index, ZRAM_PAGE_ANON);
	} else {
		zram_set_flag(zram, index, ZRAM_PAGE_FILE);
		/* 块层写入时常设 Writeback，因此两者需一并判断 */
		if (PageDirty(page) || PageWriteback(page))
			zram_set_flag(zram, index, ZRAM_PAGE_DIRTY);
	}

	zram_slot_unlock(zram, index);

	atomic64_inc(&zram->stats.same_pages);
	atomic64_inc(&zram->stats.pages_stored);

	return 0;
}

static int write_incompressible_page(struct zram *zram, struct page *page,
				     u32 index, u8 prio)
{
	unsigned long handle;
	void *src, *dst;

	/*
	 * This function is called from preemptible context so we don't need
	 * to do optimistic and fallback to pessimistic handle allocation,
	 * like we do for compressible pages.
	 */
	handle = zs_malloc(zram->mem_pool, PAGE_SIZE,
			   GFP_NOIO | __GFP_HIGHMEM | __GFP_MOVABLE);
	if (IS_ERR_VALUE(handle))
		return PTR_ERR((void *)handle);

	if (!zram_can_store_page(zram)) {
		zcomp_stream_put(zram->comps[ZRAM_PRIMARY_COMP]);
		zs_free(zram->mem_pool, handle);
		return -ENOMEM;
	}

	dst = zs_map_object(zram->mem_pool, handle, ZS_MM_WO);
	src = kmap_local_page(page);
	memcpy(dst, src, PAGE_SIZE);
	kunmap_local(src);
	zs_unmap_object(zram->mem_pool, handle);

	zram_slot_lock(zram, index);
	zram_set_flag(zram, index, ZRAM_HUGE);
	zram_set_handle(zram, index, handle);
	zram_set_obj_size(zram, index, PAGE_SIZE);
	zram_set_priority(zram, index, prio);

	if (PageAnon(page)) {
		zram_set_flag(zram, index, ZRAM_PAGE_ANON);
	} else {
		zram_set_flag(zram, index, ZRAM_PAGE_FILE);
		if (PageDirty(page) || PageWriteback(page))
			zram_set_flag(zram, index, ZRAM_PAGE_DIRTY);
	}

	zram_slot_unlock(zram, index);

	atomic64_add(PAGE_SIZE, &zram->stats.compr_data_size);
	atomic64_inc(&zram->stats.huge_pages);
	atomic64_inc(&zram->stats.huge_pages_since);
	atomic64_inc(&zram->stats.pages_stored);

	return 0;
}

static int zram_write_page(struct zram *zram, struct page *page, u32 index)
{
	int ret = 0;
	unsigned long handle;
	unsigned int comp_len;
	void *dst, *mem;
	struct zcomp_strm *zstrm = NULL;
	unsigned long element;
	bool same_filled;
	u8 prio, prio_max = zram->num_active_comps;
#ifdef CONFIG_ZRAM_MULTI_COMP
	prio_max = min(prio_max, sysctl_zram_recomp_immediate + 1);
#endif //CONFIG_ZRAM_MULTI_COMP

	/* First, free memory allocated to this slot (if any) */
	zram_slot_lock(zram, index);
	zram_free_page(zram, index);
	zram_slot_unlock(zram, index);


	mem = kmap_atomic(page);
	same_filled = page_same_filled(mem, &element);
	kunmap_atomic(mem);
	if (same_filled)
		return write_same_filled_page(zram, element, index, page);

	for (prio = ZRAM_PRIMARY_COMP; prio < prio_max; prio++) {
		if (!zram->comps[prio])
			continue;

		zstrm = zcomp_stream_get(zram->comps[prio]);
		mem = kmap_local_page(page);
		ret = zcomp_compress(zstrm, mem, &comp_len);
		kunmap_local(mem);

		if (unlikely(ret)) {
			pr_err("Compression failed! err=%d\n", ret);
			goto out;
		}

		if (comp_len < huge_class_size)
			break;

		zcomp_stream_put(zram->comps[prio]);
		zstrm = NULL;
	}

	if (!zstrm) {
		if (prio >= zram->num_active_comps) {
			zram_slot_lock(zram, index);
			zram_set_flag(zram, index, ZRAM_INCOMPRESSIBLE);
			zram_slot_unlock(zram, index);
		}

		ret = write_incompressible_page(zram, page, index, prio - 1);
		goto out;
	}

	handle = zs_malloc(zram->mem_pool, comp_len,
			__GFP_KSWAPD_RECLAIM |
			__GFP_NOWARN |
			__GFP_HIGHMEM |
			__GFP_MOVABLE |
			__GFP_CMA);
	if (IS_ERR_VALUE(handle)) {
		ret = PTR_ERR((void *)handle);
		goto out;
	}

	if (!zram_can_store_page(zram)) {
		zs_free(zram->mem_pool, handle);
		ret = -ENOMEM;
		goto out;
	}

	dst = zs_map_object(zram->mem_pool, handle, ZS_MM_WO);

	memcpy(dst, zstrm->buffer, comp_len);
	zs_unmap_object(zram->mem_pool, handle);

	zram_slot_lock(zram, index);
	zram_set_handle(zram, index, handle);
	zram_set_obj_size(zram, index, comp_len);
	zram_set_priority(zram, index, prio);

	if (PageAnon(page)) {
		zram_set_flag(zram, index, ZRAM_PAGE_ANON);
	} else {
		zram_set_flag(zram, index, ZRAM_PAGE_FILE);
		if (PageDirty(page) || PageWriteback(page))
			zram_set_flag(zram, index, ZRAM_PAGE_DIRTY);
	}

	zram_slot_unlock(zram, index);

	/* Update stats */
	atomic64_inc(&zram->stats.pages_stored);
	atomic64_add(comp_len, &zram->stats.compr_data_size);

out:
	if (zstrm)
		zcomp_stream_put(zram->comps[prio]);
	return ret;
}

/*
 * This is a partial IO. Read the full page before writing the changes.
 */
static int zram_bvec_write_partial(struct zram *zram, struct bio_vec *bvec,
				   u32 index, int offset, struct bio *bio)
{
	struct page *page = alloc_page(GFP_NOIO);
	int ret;

	if (!page)
		return -ENOMEM;

	ret = zram_read_page(zram, page, index, bio);
	if (!ret) {
		memcpy_from_bvec(page_address(page) + offset, bvec);
		ret = zram_write_page(zram, page, index);
	}
	__free_page(page);
	return ret;
}

static int zram_bvec_write(struct zram *zram, struct bio_vec *bvec,
			   u32 index, int offset, struct bio *bio)
{
	if (is_partial_io(bvec))
		return zram_bvec_write_partial(zram, bvec, index, offset, bio);
	return zram_write_page(zram, bvec->bv_page, index);
}

#ifdef CONFIG_ZRAM_MULTI_COMP
#define RECOMPRESS_IDLE		(1 << 0)
#define RECOMPRESS_HUGE		(1 << 1)

static int scan_slots_for_recompress(struct zram *zram, u32 mode, u32 prio_max,
				     struct zram_pp_ctl *ctl)
{
	unsigned long nr_pages = zram->disksize >> PAGE_SHIFT;
	unsigned long index;

	for (index = 0; index < nr_pages; index++) {
		bool ok = true;

		zram_slot_lock(zram, index);
		if (!zram_allocated(zram, index))
			goto next;

		{
			unsigned int temp = zram_get_temp_locked(zram, index);

			if (temp > 0) {
				pr_info_ratelimited("zram %s recompress scan skip hot page index=%lu temp=%u->%u\n",
						    zram->disk->disk_name, index, temp, temp - 1);
				zram_set_temp_locked(zram, index, temp - 1);
				goto next;
			}
		}

		if (mode & RECOMPRESS_IDLE &&
		    !zram_test_flag(zram, index, ZRAM_IDLE))
			goto next;

		if (mode & RECOMPRESS_HUGE &&
		    !zram_test_flag(zram, index, ZRAM_HUGE))
			goto next;

		if (zram_test_flag(zram, index, ZRAM_WB) ||
		    zram_test_flag(zram, index, ZRAM_SAME) ||
		    zram_test_flag(zram, index, ZRAM_INCOMPRESSIBLE))
			goto next;

		/* Already compressed with same of higher priority */
		if (zram_get_priority(zram, index) + 1 >= prio_max)
			goto next;

		ok = place_pp_slot(zram, ctl, index);
next:
		zram_slot_unlock(zram, index);
		if (!ok)
			break;
	}

	return 0;
}

/*
 * This function will decompress (unless it's ZRAM_HUGE) the page and then
 * attempt to compress it using provided compression algorithm priority
 * (which is potentially more effective).
 *
 * Corresponding ZRAM slot should be locked.
 */
static int recompress_slot(struct zram *zram, u32 index, struct page *page,
			   u64 *num_recomp_pages, u32 threshold, u32 prio,
			   u32 prio_max)
{
	struct zcomp_strm *zstrm = NULL;
	unsigned long handle_old;
	unsigned long handle_new;
	unsigned int comp_len_old;
	unsigned int comp_len_new;
	unsigned int class_index_old;
	unsigned int class_index_new;
	u32 num_recomps = 0;
	void *src, *dst;
	int ret;

	handle_old = zram_get_handle(zram, index);
	if (!handle_old)
		return -EINVAL;

	comp_len_old = zram_get_obj_size(zram, index);
	/*
	 * Do not recompress objects that are already "small enough".
	 */
	if (comp_len_old < threshold)
		return 0;

	ret = zram_read_from_zspool(zram, page, index);
	if (ret)
		return ret;

	/*
	 * We touched this entry so mark it as non-IDLE. This makes sure that
	 * we don't preserve IDLE flag and don't incorrectly pick this entry
	 * for different post-processing type (e.g. writeback).
	 */
	zram_clear_flag(zram, index, ZRAM_IDLE);

	class_index_old = zs_lookup_class_index(zram->mem_pool, comp_len_old);

	prio = max(prio, zram_get_priority(zram, index) + 1);
	/*
	 * Recompression slots scan should not select slots that are
	 * already compressed with a higher priority algorithm, but
	 * just in case
	 */
	if (prio >= prio_max)
		return 0;

	/*
	 * Iterate the secondary comp algorithms list (in order of priority)
	 * and try to recompress the page.
	 */
	for (; prio < prio_max; prio++) {
		if (!zram->comps[prio])
			continue;

		num_recomps++;
		zstrm = zcomp_stream_get(zram->comps[prio]);
		src = kmap_atomic(page);
		ret = zcomp_compress(zstrm, src, &comp_len_new);
		kunmap_atomic(src);

		if (ret) {
			zcomp_stream_put(zram->comps[prio]);
			return ret;
		}

		class_index_new = zs_lookup_class_index(zram->mem_pool,
							comp_len_new);

		/* Continue until we make progress */
		if (class_index_new >= class_index_old ||
		    (threshold && comp_len_new >= threshold)) {
			zcomp_stream_put(zram->comps[prio]);
			continue;
		}

		/* Recompression was successful so break out */
		break;
	}

	/*
	 * We did not try to recompress, e.g. when we have only one
	 * secondary algorithm and the page is already recompressed
	 * using that algorithm
	 */
	if (!zstrm)
		return 0;

	/*
	 * Decrement the limit (if set) on pages we can recompress, even
	 * when current recompression was unsuccessful or did not compress
	 * the page below the threshold, because we still spent resources
	 * on it.
	 */
	if (*num_recomp_pages)
		*num_recomp_pages -= 1;

	if (class_index_new >= class_index_old) {
		/*
		 * Secondary algorithms failed to re-compress the page
		 * in a way that would save memory, mark the object as
		 * incompressible so that we will not try to compress
		 * it again.
		 *
		 * We need to make sure that all secondary algorithms have
		 * failed, so we test if the number of recompressions matches
		 * the number of active secondary algorithms.
		 */
		if (num_recomps == zram->num_active_comps - 1)
			zram_set_flag(zram, index, ZRAM_INCOMPRESSIBLE);
		return 0;
	}

	/* Successful recompression but above threshold */
	if (threshold && comp_len_new >= threshold)
		return 0;

	/*
	 * No direct reclaim (slow path) for handle allocation and no
	 * re-compression attempt (unlike in zram_write_bvec()) since
	 * we already have stored that object in zsmalloc. If we cannot
	 * alloc memory for recompressed object then we bail out and
	 * simply keep the old (existing) object in zsmalloc.
	 */
	handle_new = zs_malloc(zram->mem_pool, comp_len_new,
			       __GFP_KSWAPD_RECLAIM |
			       __GFP_NOWARN |
			       __GFP_HIGHMEM |
			       __GFP_MOVABLE);
	if (IS_ERR_VALUE(handle_new)) {
		zcomp_stream_put(zram->comps[prio]);
		return PTR_ERR((void *)handle_new);
	}

	dst = zs_map_object(zram->mem_pool, handle_new, ZS_MM_WO);
	memcpy(dst, zstrm->buffer, comp_len_new);
	zcomp_stream_put(zram->comps[prio]);

	zs_unmap_object(zram->mem_pool, handle_new);

	zram_free_page(zram, index);
	zram_set_handle(zram, index, handle_new);
	zram_set_obj_size(zram, index, comp_len_new);
	zram_set_priority(zram, index, prio);

	atomic64_add(comp_len_new, &zram->stats.compr_data_size);
	atomic64_inc(&zram->stats.pages_stored);

	return 0;
}

static ssize_t recompress_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t len)
{
	u32 prio = ZRAM_SECONDARY_COMP, prio_max = ZRAM_MAX_COMPS;
	struct zram *zram = dev_to_zram(dev);
	char *args, *param, *val, *algo = NULL;
	u64 num_recomp_pages = ULLONG_MAX;
	struct zram_pp_ctl *ctl = NULL;
	struct zram_pp_slot *pps;
	u32 mode = 0, threshold = 0;
	struct page *page = NULL;
	ssize_t ret;

	args = skip_spaces(buf);
	while (*args) {
		args = next_arg(args, &param, &val);

		if (!val || !*val)
			return -EINVAL;

		if (!strcmp(param, "type")) {
			if (!strcmp(val, "idle"))
				mode = RECOMPRESS_IDLE;
			if (!strcmp(val, "huge"))
				mode = RECOMPRESS_HUGE;
			if (!strcmp(val, "huge_idle"))
				mode = RECOMPRESS_IDLE | RECOMPRESS_HUGE;
			continue;
		}

		if (!strcmp(param, "max_pages")) {
			/*
			 * Limit the number of entries (pages) we attempt to
			 * recompress.
			 */
			ret = kstrtoull(val, 10, &num_recomp_pages);
			if (ret)
				return ret;
			continue;
		}

		if (!strcmp(param, "threshold")) {
			/*
			 * We will re-compress only idle objects equal or
			 * greater in size than watermark.
			 */
			ret = kstrtouint(val, 10, &threshold);
			if (ret)
				return ret;
			continue;
		}

		if (!strcmp(param, "algo")) {
			algo = val;
			continue;
		}
	}

	if (threshold >= huge_class_size)
		return -EINVAL;

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		ret = -EINVAL;
		goto release_init_lock;
	}

	/* Do not permit concurrent post-processing actions. */
	if (atomic_xchg(&zram->pp_in_progress, 1)) {
		up_read(&zram->init_lock);
		return -EAGAIN;
	}

	if (algo) {
		bool found = false;

		for (; prio < ZRAM_MAX_COMPS; prio++) {
			if (!zram->comp_algs[prio])
				continue;

			if (!strcmp(zram->comp_algs[prio], algo)) {
				prio_max = min(prio + 1, ZRAM_MAX_COMPS);
				found = true;
				break;
			}
		}

		if (!found) {
			ret = -EINVAL;
			goto release_init_lock;
		}
	}

	page = alloc_page(GFP_KERNEL);
	if (!page) {
		ret = -ENOMEM;
		goto release_init_lock;
	}

	ctl = init_pp_ctl();
	if (!ctl) {
		ret = -ENOMEM;
		goto release_init_lock;
	}

	scan_slots_for_recompress(zram, mode, prio_max, ctl);

	ret = len;
	while ((pps = select_pp_slot(ctl))) {
		int err = 0;

		if (!num_recomp_pages)
			break;

		zram_slot_lock(zram, pps->index);
		if (!zram_test_flag(zram, pps->index, ZRAM_PP_SLOT))
			goto next;

		err = recompress_slot(zram, pps->index, page,
				      &num_recomp_pages, threshold,
				      prio, prio_max);
next:
		zram_slot_unlock(zram, pps->index);
		release_pp_slot(zram, pps);

		if (err) {
			ret = err;
			break;
		}

		cond_resched();
	}

release_init_lock:
	if (page)
		__free_page(page);
	release_pp_ctl(zram, ctl);
	atomic_set(&zram->pp_in_progress, 0);
	up_read(&zram->init_lock);
	return ret;
}
#endif

static void zram_bio_discard(struct zram *zram, struct bio *bio)
{
	size_t n = bio->bi_iter.bi_size;
	u32 index = bio->bi_iter.bi_sector >> SECTORS_PER_PAGE_SHIFT;
	u32 offset = (bio->bi_iter.bi_sector & (SECTORS_PER_PAGE - 1)) <<
			SECTOR_SHIFT;

	/*
	 * zram manages data in physical block size units. Because logical block
	 * size isn't identical with physical block size on some arch, we
	 * could get a discard request pointing to a specific offset within a
	 * certain physical block.  Although we can handle this request by
	 * reading that physiclal block and decompressing and partially zeroing
	 * and re-compressing and then re-storing it, this isn't reasonable
	 * because our intent with a discard request is to save memory.  So
	 * skipping this logical block is appropriate here.
	 */
	if (offset) {
		if (n <= (PAGE_SIZE - offset))
			return;

		n -= (PAGE_SIZE - offset);
		index++;
	}

	while (n >= PAGE_SIZE) {
		zram_slot_lock(zram, index);
		zram_free_page(zram, index);
		zram_slot_unlock(zram, index);
		atomic64_inc(&zram->stats.notify_free);
		index++;
		n -= PAGE_SIZE;
	}

	bio_endio(bio);
}

static void zram_bio_read(struct zram *zram, struct bio *bio)
{
	unsigned long start_time = bio_start_io_acct(bio);
	struct bvec_iter iter = bio->bi_iter;

	do {
		u32 index = iter.bi_sector >> SECTORS_PER_PAGE_SHIFT;
		u32 offset = (iter.bi_sector & (SECTORS_PER_PAGE - 1)) <<
				SECTOR_SHIFT;
		struct bio_vec bv = bio_iter_iovec(bio, iter);

		bv.bv_len = min_t(u32, bv.bv_len, PAGE_SIZE - offset);

		if (zram_bvec_read(zram, &bv, index, offset, bio) < 0) {
			atomic64_inc(&zram->stats.failed_reads);
			bio->bi_status = BLK_STS_IOERR;
			break;
		}
		flush_dcache_page(bv.bv_page);

		zram_slot_lock(zram, index);
		zram_accessed(zram, index);
		zram_slot_unlock(zram, index);

		bio_advance_iter_single(bio, &iter, bv.bv_len);
	} while (iter.bi_size);

	bio_end_io_acct(bio, start_time);
	bio_endio(bio);
}

static void zram_bio_write(struct zram *zram, struct bio *bio)
{
	unsigned long start_time = bio_start_io_acct(bio);
	struct bvec_iter iter = bio->bi_iter;

	do {
		u32 index = iter.bi_sector >> SECTORS_PER_PAGE_SHIFT;
		u32 offset = (iter.bi_sector & (SECTORS_PER_PAGE - 1)) <<
				SECTOR_SHIFT;
		struct bio_vec bv = bio_iter_iovec(bio, iter);

		bv.bv_len = min_t(u32, bv.bv_len, PAGE_SIZE - offset);

		if (zram_bvec_write(zram, &bv, index, offset, bio) < 0) {
			atomic64_inc(&zram->stats.failed_writes);
			bio->bi_status = BLK_STS_IOERR;
			break;
		}

		zram_slot_lock(zram, index);
		zram_accessed(zram, index);
		zram_slot_unlock(zram, index);

		bio_advance_iter_single(bio, &iter, bv.bv_len);
	} while (iter.bi_size);

	bio_end_io_acct(bio, start_time);
	bio_endio(bio);
}

/*
 * Handler function for all zram I/O requests.
 */
static void zram_submit_bio(struct bio *bio)
{
	struct zram *zram = bio->bi_bdev->bd_disk->private_data;

	switch (bio_op(bio)) {
	case REQ_OP_READ:
		zram_bio_read(zram, bio);
		break;
	case REQ_OP_WRITE:
		zram_bio_write(zram, bio);
		break;
	case REQ_OP_DISCARD:
	case REQ_OP_WRITE_ZEROES:
		zram_bio_discard(zram, bio);
		break;
	default:
		WARN_ON_ONCE(1);
		bio_endio(bio);
	}
}

#ifdef CONFIG_ZRAM_WRITEBACK
static DECLARE_WAIT_QUEUE_HEAD(auto_writeback_wq);
static atomic_t auto_writeback_pending = ATOMIC_INIT(0);

static unsigned long zram_auto_writeback(struct zram *zram,
					 unsigned long max_pages)
{
	u64 nr_pages = zram->disksize >> PAGE_SHIFT;
	struct zram_pp_ctl *ctl;
	unsigned long pages_written = 0;
	int err;

	if (!zram || !zram->disk)
		return 0;

	down_read(&zram->init_lock);
	if (!init_done(zram) || !zram->backing_dev) {
		up_read(&zram->init_lock);
		return 0;
	}
	up_read(&zram->init_lock);

	down_read(&zram->init_lock);
	if (atomic_xchg(&zram->pp_in_progress, 1)) {
		up_read(&zram->init_lock);
		return 0;
	}
	ctl = init_pp_ctl();
	if (!ctl) {
		atomic_set(&zram->pp_in_progress, 0);
		up_read(&zram->init_lock);
		return 0;
	}

	scan_slots_for_writeback(zram, IDLE_WRITEBACK, 0, nr_pages, ctl);
	err = zram_writeback_slots(zram, ctl, max_pages, &pages_written);
	if (err)
		pr_info_ratelimited("zram %s auto writeback failed ret=%d pages=%lu limit_pages=%lu\n",
				    zram->disk->disk_name, err, pages_written, max_pages);

	release_pp_ctl(zram, ctl);
	atomic_set(&zram->pp_in_progress, 0);
	up_read(&zram->init_lock);

	if (pages_written)
		pr_info_ratelimited("zram %s auto writeback triggered mode=idle pages=%lu limit_pages=%lu\n",
				    zram->disk->disk_name, pages_written, max_pages);

	return pages_written;
}

static int auto_writeback_func(void *data)
{
	(void)data;

	while (!kthread_should_stop()) {
		struct device *single_dev = NULL;
		struct zram *single_zram = NULL;
		int zram_ids[32];
		int num_devs = 0;

		wait_event_freezable(auto_writeback_wq,
				     kthread_should_stop() ||
				     atomic_read(&auto_writeback_pending));
		if (kthread_should_stop())
			break;

		while (atomic_xchg(&auto_writeback_pending, 0) &&
		       !kthread_should_stop()) {
			if (zram_get_single_device(&single_zram, &single_dev)) {
				pr_info_ratelimited("zram auto writeback worker triggered by monitor (single-device fast path)\n");
				zram_auto_writeback(single_zram,
						    ZRAM_AUTO_WRITEBACK_LIMIT_PAGES);
				put_device(single_dev);
				cond_resched();
				continue;
			}

			unsigned long remaining_pages = ZRAM_AUTO_WRITEBACK_LIMIT_PAGES;
			int id;
			int i;
			struct zram *zram;

			num_devs = 0;

			mutex_lock(&zram_index_mutex);
			idr_for_each_entry(&zram_index_idr, zram, id) {
				if (num_devs < ARRAY_SIZE(zram_ids))
					zram_ids[num_devs++] = id;
			}
			mutex_unlock(&zram_index_mutex);

			pr_info_ratelimited("zram auto writeback worker triggered by monitor\n");

			for (i = 0; i < num_devs && !kthread_should_stop(); i++) {
				struct device *dev;

				rcu_read_lock();
				zram = idr_find(&zram_index_idr, zram_ids[i]);
				if (!zram || !zram->disk || !get_device(disk_to_dev(zram->disk))) {
					rcu_read_unlock();
					continue;
				}
				dev = disk_to_dev(zram->disk);
				rcu_read_unlock();

				if (remaining_pages == 0) {
					put_device(dev);
					break;
				}

				remaining_pages -= min(remaining_pages,
						       zram_auto_writeback(zram, remaining_pages));
				put_device(dev);
				cond_resched();
			}
		}
	}

	return 0;
}

static int monitor_func(void *data)
{
	(void)data;
	unsigned int scan_rounds = 0;

	while (!kthread_should_stop()) {
		ktime_t cutoff_time;
		struct device *single_dev = NULL;
		struct zram *single_zram = NULL;
		int zram_ids[32];
		int num_devs = 0;
		int id;
		int i;
		struct zram *zram;

#ifdef CONFIG_ZRAM_TRACK_ENTRY_ACTIME
		cutoff_time = ktime_sub(ktime_get_boottime(),
					ns_to_ktime(600ULL * NSEC_PER_SEC));
#else
		cutoff_time = 0;
#endif

		if (zram_get_single_device(&single_zram, &single_dev)) {
			unsigned long nr_pages;
			unsigned long scan_pages;
			unsigned long start;

			down_read(&single_zram->init_lock);
			if (init_done(single_zram)) {
				nr_pages = single_zram->disksize >> PAGE_SHIFT;
				if (nr_pages) {
					scan_pages = nr_pages / 8;
					if (!scan_pages)
						scan_pages = nr_pages;
					start = READ_ONCE(single_zram->idle_scan_cursor) % nr_pages;
					mark_idle_range(single_zram, cutoff_time, start, scan_pages);
					WRITE_ONCE(single_zram->idle_scan_cursor,
						   (start + scan_pages) % nr_pages);
					if (++scan_rounds >= 4) {
						scan_rounds = 0;
						pr_info("zram monitor trigger writeback after 4 scans (single-device fast path)\n");
						atomic_set(&auto_writeback_pending, 1);
						wake_up(&auto_writeback_wq);
					}
				}
			}
			up_read(&single_zram->init_lock);
			put_device(single_dev);
			schedule_timeout_interruptible(CHECK_INTERVAL);
			continue;
		}

		mutex_lock(&zram_index_mutex);
		idr_for_each_entry(&zram_index_idr, zram, id) {
			if (num_devs < ARRAY_SIZE(zram_ids))
				zram_ids[num_devs++] = id;
		}
		mutex_unlock(&zram_index_mutex);

		for (i = 0; i < num_devs && !kthread_should_stop(); i++) {
			unsigned long nr_pages;
			unsigned long scan_pages;
			unsigned long start;
			struct device *dev;

			rcu_read_lock();
			zram = idr_find(&zram_index_idr, zram_ids[i]);
			if (!zram || !zram->disk || !get_device(disk_to_dev(zram->disk))) {
				rcu_read_unlock();
				continue;
			}
			dev = disk_to_dev(zram->disk);
			rcu_read_unlock();

			down_read(&zram->init_lock);
			if (init_done(zram)) {
				nr_pages = zram->disksize >> PAGE_SHIFT;
				if (nr_pages) {
					scan_pages = nr_pages / 8;
					if (!scan_pages)
						scan_pages = nr_pages;
					start = READ_ONCE(zram->idle_scan_cursor) % nr_pages;
					mark_idle_range(zram, cutoff_time, start, scan_pages);
					WRITE_ONCE(zram->idle_scan_cursor,
						   (start + scan_pages) % nr_pages);
				}
			}
			up_read(&zram->init_lock);

			put_device(dev);
			cond_resched();
		}

		if (num_devs > 0) {
			if (++scan_rounds >= 4) {
				scan_rounds = 0;
				pr_info("zram monitor trigger writeback after 4 scans\n");
				atomic_set(&auto_writeback_pending, 1);
				wake_up(&auto_writeback_wq);
			}
		} else {
			scan_rounds = 0;
		}

		schedule_timeout_interruptible(CHECK_INTERVAL);
	}

	return 0;
}
#endif

static void zram_slot_free_notify(struct block_device *bdev,
				unsigned long index)
{
	struct zram *zram;

	zram = bdev->bd_disk->private_data;

	atomic64_inc(&zram->stats.notify_free);
	if (!zram_slot_trylock(zram, index)) {
		atomic64_inc(&zram->stats.miss_free);
		return;
	}

	zram_free_page(zram, index);
	zram_slot_unlock(zram, index);
}

static void zram_destroy_comps(struct zram *zram)
{
	u32 prio;

	for (prio = 0; prio < ZRAM_MAX_COMPS; prio++) {
		struct zcomp *comp = zram->comps[prio];

		zram->comps[prio] = NULL;
		if (!comp)
			continue;
		zcomp_destroy(comp);
		zram->num_active_comps--;
	}

	for (prio = ZRAM_PRIMARY_COMP; prio < ZRAM_MAX_COMPS; prio++) {
		/* Do not free statically defined compression algorithms */
		if (zram->comp_algs[prio] != default_compressor)
			kfree(zram->comp_algs[prio]);
		zram->comp_algs[prio] = NULL;
	}
}

static void zram_reset_device(struct zram *zram)
{
	down_write(&zram->init_lock);

	zram->limit_pages = 0;

	set_capacity_and_notify(zram->disk, 0);
	part_stat_set_all(zram->disk->part0, 0);

	/* I/O operation under all of CPU are done so let's free */
	zram_meta_free(zram, zram->disksize);
	zram->disksize = 0;
	zram_destroy_comps(zram);
	memset(&zram->stats, 0, sizeof(zram->stats));
	atomic_set(&zram->pp_in_progress, 0);
	reset_bdev(zram);

	comp_algorithm_set(zram, ZRAM_PRIMARY_COMP, default_compressor);
	up_write(&zram->init_lock);
}

static inline u64 _round_up(u64 num, u64 multiple) {
    if (multiple == 0) return num;
    u64 remainder = num % multiple;
    if (remainder == 0) return num;
    return num + multiple - remainder;
}

#ifdef CONFIG_ZRAM_AUTO_SIZE
u64 calculate_pressure_factor_log_slow_to_fast_kernel(u64 mem_pressure, u64 zram_pressure, u64 min_num, u64 max_num) {
    s32 pressure_diff = 0;
    s32 pressure_increase_log;
    s32 base_factor_log;
    s32 combined_factor_log;
    s32 scaling_factor_log;
    u64 combined_pressure_factor_percent;

    if (mem_pressure > 60) {
        pressure_diff += (s32)(mem_pressure - 60);
    }
    if (zram_pressure > 50) {
        pressure_diff += (s32)(zram_pressure - 50);
    }

    if (pressure_diff <= 0) {
        return min_num;
    }

    // 1. 将压力差转换为 log 格式
    pressure_increase_log = u64_to_log32fpmax((u64)pressure_diff);

    // 2.添加一个缩放因子来减缓增长
    scaling_factor_log = u64_to_log32fpmax(15ULL);  // log(15)
    pressure_increase_log = pressure_increase_log - scaling_factor_log;  // log(x) - log(15) = log(x/15)

    // 3. 将基础因子（100）转换为 log 格式
    base_factor_log = u64_to_log32fpmax(100ULL);

    // 4. 在 log 空间中相加，模拟线性空间的乘法
    combined_factor_log = base_factor_log + pressure_increase_log;

    // 5. 将结果从 log 格式转换回线性整数
    combined_pressure_factor_percent = log32fpmax_to_u64(combined_factor_log);

    // 6. 限制结果的范围
    combined_pressure_factor_percent = min(combined_pressure_factor_percent, max_num);
    if (combined_pressure_factor_percent < min_num) {
        combined_pressure_factor_percent = min_num;
    }

    return combined_pressure_factor_percent;
}
#endif

static ssize_t disksize_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	u64 disksize;
	struct zcomp *comp;
	struct zram *zram = dev_to_zram(dev);
	int err;
	u32 prio;

#ifdef CONFIG_ZRAM_AUTO_SIZE
	if (sysfs_streq(buf, "auto")) {
        unsigned long total_mem = (u64)totalram_pages() << PAGE_SHIFT; // 总物理内存
        unsigned int num_cores = num_online_cpus(); // 在线 CPU 核心数
        u64 base_ratio;
		u64 target_size;
        u64 combined_pressure_factor_percent;

        // 1. 优化 base_ratio 的计算：确保 8 核时达到 100%
        // 每个核心贡献 13%，上限 100%
        base_ratio = min(num_cores * 13ULL, 100ULL); 
        
        // 计算基于内存和核心数的初始目标大小
        target_size = div64_ul(total_mem * base_ratio, 100ULL);

        // 2. 引入历史内存和 Zram 压力因子
        spin_lock(&zram->pressure_lock);
        unsigned int mem_pressure = zram->historical_mem_pressure;
        unsigned int zram_pressure = zram->historical_zram_pressure;
        spin_unlock(&zram->pressure_lock);

        combined_pressure_factor_percent = 100ULL; // 默认压力因子为 100% (不增加也不减少)

        // 根据压力数据调整因子
    	combined_pressure_factor_percent = calculate_pressure_factor_log_slow_to_fast_kernel(mem_pressure, zram_pressure, 100ULL, 200ULL);

        // 应用压力因子
        target_size = div64_ul(target_size * combined_pressure_factor_percent, 100ULL);

        // 3. 向上取整到最近的 GB
        target_size = _round_up(target_size, 1ULL << 30);

        // 4. 调整 clamp 范围：允许 Zram 大小超过物理内存
        // 最小 Zram 1GB，或总内存的 1/8 （取两者最大值）
        u64 min_allowed_size = max(1ULL * 1024 * 1024 * 1024ULL, div64_ul(total_mem, 8ULL)); 
        // 最大 Zram 可以是 64GB，或总内存的 2 倍（取两者最小值）
        u64 max_allowed_size = min(64ULL * 1024 * 1024 * 1024ULL, total_mem * 2ULL); 
        
        // 确保最小不会超过最大
        if (min_allowed_size > max_allowed_size) {
            min_allowed_size = max_allowed_size; 
        }

        target_size = clamp(target_size, min_allowed_size, max_allowed_size);
        
        pr_info("zram: auto-calculated disksize: %llu (mem: %luGB, cores: %u, pressure: %u:%u, factor: %llu%%)\n",
                target_size, total_mem >> 30, num_cores, mem_pressure, zram_pressure, combined_pressure_factor_percent);

		disksize = target_size;
    } else {
        // 用户手动设置 Zram 大小
        disksize  = memparse(buf, NULL);
	}
#else
	disksize = memparse(buf, NULL);
#endif

	if (!disksize)
		return -EINVAL;

	down_write(&zram->init_lock);
	if (init_done(zram)) {
		pr_info("Cannot change disksize for initialized device\n");
		err = -EBUSY;
		goto out_unlock;
	}

	disksize = PAGE_ALIGN(disksize);
	if (!zram_meta_alloc(zram, disksize)) {
		err = -ENOMEM;
		goto out_unlock;
	}

	for (prio = 0; prio < ZRAM_MAX_COMPS; prio++) {
		if (!zram->comp_algs[prio])
			continue;

		comp = zcomp_create(zram->comp_algs[prio]);
		if (IS_ERR(comp)) {
			pr_err("Cannot initialise %s compressing backend\n",
			       zram->comp_algs[prio]);
			err = PTR_ERR(comp);
			goto out_free_comps;
		}

		zram->comps[prio] = comp;
		zram->num_active_comps++;
	}
	zram->disksize = disksize;
	set_capacity_and_notify(zram->disk, zram->disksize >> SECTOR_SHIFT);
	up_write(&zram->init_lock);

	return len;

out_free_comps:
	zram_destroy_comps(zram);
	zram_meta_free(zram, disksize);
out_unlock:
	up_write(&zram->init_lock);
	return err;
}

static ssize_t reset_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	int ret;
	unsigned short do_reset;
	struct zram *zram;
	struct gendisk *disk;

	ret = kstrtou16(buf, 10, &do_reset);
	if (ret)
		return ret;

	if (!do_reset)
		return -EINVAL;

	zram = dev_to_zram(dev);
	disk = zram->disk;

	mutex_lock(&disk->open_mutex);
	/* Do not reset an active device or claimed device */
	if (disk_openers(disk) || zram->claim) {
		mutex_unlock(&disk->open_mutex);
		return -EBUSY;
	}

	/* From now on, anyone can't open /dev/zram[0-9] */
	zram->claim = true;
	mutex_unlock(&disk->open_mutex);

	/* Make sure all the pending I/O are finished */
	sync_blockdev(disk->part0);
	zram_reset_device(zram);

	mutex_lock(&disk->open_mutex);
	zram->claim = false;
	mutex_unlock(&disk->open_mutex);

	return len;
}

static int zram_open(struct gendisk *disk, blk_mode_t mode)
{
	struct zram *zram = disk->private_data;

	WARN_ON(!mutex_is_locked(&disk->open_mutex));

	/* zram was claimed to reset so open request fails */
	if (zram->claim)
		return -EBUSY;
	return 0;
}

static const struct block_device_operations zram_devops = {
	.open = zram_open,
	.submit_bio = zram_submit_bio,
	.swap_slot_free_notify = zram_slot_free_notify,
	.owner = THIS_MODULE
};

#ifdef CONFIG_ZRAM_AUTO_SIZE
static ssize_t pressure_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct zram *zram;
    unsigned int mem_pressure, zram_pressure;
    
    // 参数合法性检查
    if (!dev || !buf) {
        pr_err("zram: invalid parameters in pressure_show\n");
        return -EINVAL;
    }
    
    zram = dev_to_zram(dev);
    if (!zram) {
        pr_warn("zram: zram device data is NULL\n");
        return scnprintf(buf, PAGE_SIZE, "NULL:NULL\n");
    }
    
    // 在读取时也需要加锁，确保数据一致性
    spin_lock(&zram->pressure_lock);
    mem_pressure = zram->historical_mem_pressure;
    zram_pressure = zram->historical_zram_pressure;
    spin_unlock(&zram->pressure_lock);
    
    // 使用 scnprintf 而不是 sprintf，更安全
    return scnprintf(buf, PAGE_SIZE, "%u:%u\n", mem_pressure, zram_pressure);
}

static ssize_t pressure_store(struct device *dev, struct device_attribute *attr,
                               const char *buf, size_t len)
{
    struct zram *zram = dev_to_zram(dev);
    unsigned int mem_pressure, zram_pressure;
    char *colon_pos;
    char *local_buf;
    int ret = -EINVAL;

    pr_debug("zram: pressure_store called with len=%zu, buf='%.*s'\n", 
             len, (int)len, buf);

    // 参数合法性检查
    if (!buf || len == 0) {
        pr_err("zram: invalid buffer parameters\n");
        return -EINVAL;
    }
    
    if (!zram) {
        pr_err("zram: zram device data is NULL\n");
        return -ENODEV;
    }

    // 分配本地缓冲区，避免修改原始buf
    local_buf = kmalloc(len + 1, GFP_KERNEL);
    if (!local_buf) {
        pr_err("zram: memory allocation failed\n");
        return -ENOMEM;
    }

    // 复制数据并确保字符串终止
    memcpy(local_buf, buf, len);
    local_buf[len] = '\0';

    // 移除可能的换行符
    if (len > 0 && local_buf[len - 1] == '\n') {
        local_buf[len - 1] = '\0';
    }

    // 查找冒号分隔符
    colon_pos = strchr(local_buf, ':');
    if (!colon_pos) {
        pr_err("zram: invalid pressure format, expected 'mem_pressure:zram_pressure'\n");
        ret = -EINVAL;
        goto out;
    }

    // 分割字符串
    *colon_pos = '\0';

    // 解析内存压力
    ret = kstrtouint(local_buf, 10, &mem_pressure);
    if (ret) {
        pr_err("zram: invalid memory pressure value\n");
        goto out;
    }

    // 解析 Zram 压力
    ret = kstrtouint(colon_pos + 1, 10, &zram_pressure);
    if (ret) {
        pr_err("zram: invalid zram pressure value\n");
        goto out;
    }

    // 限制压力值在 0-100 之间
    mem_pressure = clamp(mem_pressure, 0U, 100U);
    zram_pressure = clamp(zram_pressure, 0U, 100U);

    spin_lock(&zram->pressure_lock);
    zram->historical_mem_pressure = mem_pressure;
    zram->historical_zram_pressure = zram_pressure;
    spin_unlock(&zram->pressure_lock);

    pr_debug("zram: updated historical pressure to %u:%u\n", mem_pressure, zram_pressure);
    
    ret = len;

out:
    kfree(local_buf);
    return ret;
}
#endif

static DEVICE_ATTR_WO(compact);
static DEVICE_ATTR_RW(disksize);
static DEVICE_ATTR_RO(initstate);
static DEVICE_ATTR_WO(reset);
static DEVICE_ATTR_WO(mem_limit);
static DEVICE_ATTR_WO(mem_used_max);
static DEVICE_ATTR_WO(idle);
static DEVICE_ATTR_RW(max_comp_streams);
static DEVICE_ATTR_RW(comp_algorithm);
#ifdef CONFIG_ZRAM_WRITEBACK
static DEVICE_ATTR_RW(backing_dev);
static DEVICE_ATTR_WO(writeback);
static DEVICE_ATTR_RW(writeback_limit);
static DEVICE_ATTR_RW(writeback_limit_enable);
#endif
#ifdef CONFIG_ZRAM_MULTI_COMP
static DEVICE_ATTR_RW(recomp_algorithm);
static DEVICE_ATTR_WO(recompress);
#endif
#ifdef CONFIG_ZRAM_AUTO_SIZE
static DEVICE_ATTR_RW(pressure);
#endif

static struct attribute *zram_disk_attrs[] = {
	&dev_attr_disksize.attr,
	&dev_attr_initstate.attr,
	&dev_attr_reset.attr,
	&dev_attr_compact.attr,
	&dev_attr_mem_limit.attr,
	&dev_attr_mem_used_max.attr,
	&dev_attr_idle.attr,
	&dev_attr_max_comp_streams.attr,
	&dev_attr_comp_algorithm.attr,
#ifdef CONFIG_ZRAM_WRITEBACK
	&dev_attr_backing_dev.attr,
	&dev_attr_writeback.attr,
	&dev_attr_writeback_limit.attr,
	&dev_attr_writeback_limit_enable.attr,
#endif
	&dev_attr_io_stat.attr,
	&dev_attr_mm_stat.attr,
#ifdef CONFIG_ZRAM_WRITEBACK
	&dev_attr_bd_stat.attr,
#endif
	&dev_attr_debug_stat.attr,
#ifdef CONFIG_ZRAM_MULTI_COMP
	&dev_attr_recomp_algorithm.attr,
	&dev_attr_recompress.attr,
#endif
#ifdef CONFIG_ZRAM_AUTO_SIZE
	&dev_attr_pressure.attr,
#endif
	NULL,
};

ATTRIBUTE_GROUPS(zram_disk);

/*
 * Allocate and initialize new zram device. the function returns
 * '>= 0' device_id upon success, and negative value otherwise.
 */
static int zram_add(void)
{
	struct zram *zram;
	int ret, device_id;
	unsigned long total_mem = (u64)totalram_pages() << PAGE_SHIFT; // 总物理内存
	u64 default_disksize = _round_up(total_mem, 1ULL << 30);

	zram = kzalloc(sizeof(struct zram), GFP_KERNEL);
	if (!zram)
		return -ENOMEM;

	ret = idr_alloc(&zram_index_idr, zram, 0, 0, GFP_KERNEL);
	if (ret < 0)
		goto out_free_dev;
	device_id = ret;

	init_rwsem(&zram->init_lock);
#ifdef CONFIG_ZRAM_WRITEBACK
	spin_lock_init(&zram->wb_limit_lock);
#endif

	/* gendisk structure */
	zram->disk = blk_alloc_disk(NUMA_NO_NODE);
	if (!zram->disk) {
		pr_err("Error allocating disk structure for device %d\n",
			device_id);
		ret = -ENOMEM;
		goto out_free_idr;
	}

	zram->disk->major = zram_major;
	zram->disk->first_minor = device_id;
	zram->disk->minors = 1;
	zram->disk->flags |= GENHD_FL_NO_PART;
	zram->disk->fops = &zram_devops;
	zram->disk->private_data = zram;

#ifdef CONFIG_ZRAM_AUTO_SIZE
	spin_lock_init(&zram->pressure_lock);
	zram->historical_mem_pressure = 0; 
	zram->historical_zram_pressure = 0;
#endif

	snprintf(zram->disk->disk_name, 16, "zram%d", device_id);
	atomic_set(&zram->pp_in_progress, 0);

	comp_algorithm_set(zram, ZRAM_PRIMARY_COMP, default_compressor);

	/* Actual capacity set using sysfs (/sys/block/zram<id>/disksize */
	set_capacity(zram->disk, 0);

	down_write(&zram->init_lock);
	if (!zram_meta_alloc(zram, default_disksize)) {
		up_write(&zram->init_lock);
		ret = -ENOMEM;
		goto out_cleanup_disk;
	}

	for (u32 prio = 0; prio < ZRAM_MAX_COMPS; prio++) {
		if (!zram->comp_algs[prio])
			continue;

		struct zcomp *comp = zcomp_create(zram->comp_algs[prio]);
		if (IS_ERR(comp)) {
			pr_err("Cannot initialise %s compressing backend\n",
				zram->comp_algs[prio]);
			zram_destroy_comps(zram); // 清理已创建的压缩器
			zram_meta_free(zram, default_disksize); // 释放元数据
			up_write(&zram->init_lock);
			ret = PTR_ERR(comp);
			goto out_cleanup_disk;
		}
		zram->comps[prio] = comp;
		zram->num_active_comps++;
	}

	zram->disksize = default_disksize;
	set_capacity_and_notify(zram->disk, zram->disksize >> SECTOR_SHIFT);
	up_write(&zram->init_lock);

	/* zram devices sort of resembles non-rotational disks */
	blk_queue_flag_set(QUEUE_FLAG_NONROT, zram->disk->queue);
	blk_queue_flag_set(QUEUE_FLAG_SYNCHRONOUS, zram->disk->queue);

	/*
	 * To ensure that we always get PAGE_SIZE aligned
	 * and n*PAGE_SIZED sized I/O requests.
	 */
	blk_queue_physical_block_size(zram->disk->queue, PAGE_SIZE);
	blk_queue_logical_block_size(zram->disk->queue,
					ZRAM_LOGICAL_BLOCK_SIZE);
	blk_queue_io_min(zram->disk->queue, PAGE_SIZE);
	blk_queue_io_opt(zram->disk->queue, PAGE_SIZE);
	zram->disk->queue->limits.discard_granularity = PAGE_SIZE;
	blk_queue_max_discard_sectors(zram->disk->queue, UINT_MAX);

	/*
	 * zram_bio_discard() will clear all logical blocks if logical block
	 * size is identical with physical block size(PAGE_SIZE). But if it is
	 * different, we will skip discarding some parts of logical blocks in
	 * the part of the request range which isn't aligned to physical block
	 * size.  So we can't ensure that all discarded logical blocks are
	 * zeroed.
	 */
	if (ZRAM_LOGICAL_BLOCK_SIZE == PAGE_SIZE)
		blk_queue_max_write_zeroes_sectors(zram->disk->queue, UINT_MAX);

	blk_queue_flag_set(QUEUE_FLAG_STABLE_WRITES, zram->disk->queue);
	ret = device_add_disk(NULL, zram->disk, zram_disk_groups);
	if (ret)
		goto out_cleanup_disk;

	zram_debugfs_register(zram);
	pr_info("Added device: %s with default size %llu bytes\n", zram->disk->disk_name, default_disksize);
	return device_id;
out_cleanup_disk:
	put_disk(zram->disk);
out_free_idr:
	idr_remove(&zram_index_idr, device_id);
out_free_dev:
	kfree(zram);
	return ret;
}

static int zram_remove(struct zram *zram)
{
	bool claimed;

	mutex_lock(&zram->disk->open_mutex);
	if (disk_openers(zram->disk)) {
		mutex_unlock(&zram->disk->open_mutex);
		return -EBUSY;
	}

	claimed = zram->claim;
	if (!claimed)
		zram->claim = true;
	mutex_unlock(&zram->disk->open_mutex);

	zram_debugfs_unregister(zram);

	if (claimed) {
		/*
		 * If we were claimed by reset_store(), del_gendisk() will
		 * wait until reset_store() is done, so nothing need to do.
		 */
		;
	} else {
		/* Make sure all the pending I/O are finished */
		sync_blockdev(zram->disk->part0);
		zram_reset_device(zram);
	}

	pr_info("Removed device: %s\n", zram->disk->disk_name);

	del_gendisk(zram->disk);

	/* del_gendisk drains pending reset_store */
	WARN_ON_ONCE(claimed && zram->claim);

	/*
	 * disksize_store() may be called in between zram_reset_device()
	 * and del_gendisk(), so run the last reset to avoid leaking
	 * anything allocated with disksize_store()
	 */
	zram_reset_device(zram);

	#ifdef CONFIG_ZRAM_WRITEBACK
	list_lru_destroy(&zram->zram_list_lru);
	#endif

	put_disk(zram->disk);
	kfree(zram);
	return 0;
}

/* zram-control sysfs attributes */

/*
 * NOTE: hot_add attribute is not the usual read-only sysfs attribute. In a
 * sense that reading from this file does alter the state of your system -- it
 * creates a new un-initialized zram device and returns back this device's
 * device_id (or an error code if it fails to create a new device).
 */
static ssize_t hot_add_show(const struct class *class,
			const struct class_attribute *attr,
			char *buf)
{
	int ret;

	mutex_lock(&zram_index_mutex);
	ret = zram_add();
	mutex_unlock(&zram_index_mutex);

	if (ret < 0)
		return ret;
	return scnprintf(buf, PAGE_SIZE, "%d\n", ret);
}
/* This attribute must be set to 0400, so CLASS_ATTR_RO() can not be used */
static struct class_attribute class_attr_hot_add =
	__ATTR(hot_add, 0400, hot_add_show, NULL);

static ssize_t hot_remove_store(const struct class *class,
			const struct class_attribute *attr,
			const char *buf,
			size_t count)
{
	struct zram *zram;
	int ret, dev_id;

	/* dev_id is gendisk->first_minor, which is `int' */
	ret = kstrtoint(buf, 10, &dev_id);
	if (ret)
		return ret;
	if (dev_id < 0)
		return -EINVAL;

	mutex_lock(&zram_index_mutex);

	zram = idr_find(&zram_index_idr, dev_id);
	if (zram) {
		ret = zram_remove(zram);
		if (!ret)
			idr_remove(&zram_index_idr, dev_id);
	} else {
		ret = -ENODEV;
	}

	mutex_unlock(&zram_index_mutex);
	return ret ? ret : count;
}
static CLASS_ATTR_WO(hot_remove);

static struct attribute *zram_control_class_attrs[] = {
	&class_attr_hot_add.attr,
	&class_attr_hot_remove.attr,
	NULL,
};
ATTRIBUTE_GROUPS(zram_control_class);

static struct class zram_control_class = {
	.name		= "zram-control",
	.class_groups	= zram_control_class_groups,
};

static int zram_remove_cb(int id, void *ptr, void *data)
{
	WARN_ON_ONCE(zram_remove(ptr));
	return 0;
}

static void destroy_devices(void)
{
	class_unregister(&zram_control_class);
	idr_for_each(&zram_index_idr, &zram_remove_cb, NULL);
	zram_debugfs_destroy();
	idr_destroy(&zram_index_idr);
	unregister_blkdev(zram_major, "zram");
	cpuhp_remove_multi_state(CPUHP_ZCOMP_PREPARE);
}


static int __init zram_init(void)
{
	int ret;

	BUILD_BUG_ON(__NR_ZRAM_PAGEFLAGS > BITS_PER_LONG);

	ret = cpuhp_setup_state_multi(CPUHP_ZCOMP_PREPARE, "block/zram:prepare",
				      zcomp_cpu_up_prepare, zcomp_cpu_dead);
	if (ret < 0)
		return ret;

	ret = class_register(&zram_control_class);
	if (ret) {
		pr_err("Unable to register zram-control class\n");
		cpuhp_remove_multi_state(CPUHP_ZCOMP_PREPARE);
		return ret;
	}

	zram_debugfs_create();
	zram_major = register_blkdev(0, "zram");
	if (zram_major <= 0) {
		pr_err("Unable to get major number\n");
		class_unregister(&zram_control_class);
		cpuhp_remove_multi_state(CPUHP_ZCOMP_PREPARE);
		return -EBUSY;
	}

	while (num_devices != 0) {
		mutex_lock(&zram_index_mutex);
		ret = zram_add();
		mutex_unlock(&zram_index_mutex);
		if (ret < 0)
			goto out_error;
		num_devices--;
	}

	if (setup_zram_writeback())
		goto out_error;

#ifdef CONFIG_ZRAM_WRITEBACK
	auto_writeback_thread = kthread_run(auto_writeback_func, NULL, "zram_auto_wb");
	if (IS_ERR(auto_writeback_thread)) {
		ret = PTR_ERR(auto_writeback_thread);
		auto_writeback_thread = NULL;
		goto out_error_writeback;
	}

	monitor_thread = kthread_run(monitor_func, NULL, "zram_monitor");
	if (IS_ERR(monitor_thread)) {
		ret = PTR_ERR(monitor_thread);
		monitor_thread = NULL;
		goto out_stop_auto_writeback;
	}
#endif

#ifdef CONFIG_ZRAM_MULTI_COMP
#define ZRAM_IR_VERSION "1.2"
#define ZRAM_IR_PROGNAME "ZRAM Immediate Recompression (ZRAM-IR)"
#define ZRAM_IR_AUTHOR   "Masahito Suzuki"
	printk(KERN_INFO "%s %s by %s\n",
		ZRAM_IR_PROGNAME, ZRAM_IR_VERSION, ZRAM_IR_AUTHOR);
	zram_sysctl_table_header = register_sysctl("vm", zram_sysctl_table);
#endif //CONFIG_ZRAM_MULTI_COMP

	return 0;

#ifdef CONFIG_ZRAM_WRITEBACK
out_stop_auto_writeback:
	if (auto_writeback_thread) {
		kthread_stop(auto_writeback_thread);
		auto_writeback_thread = NULL;
	}
out_error_writeback:
	destroy_zram_writeback();
	return ret;
#endif

out_error:
	destroy_devices();
	return ret;
}

static void __exit zram_exit(void)
{
#ifdef CONFIG_ZRAM_WRITEBACK
	if (auto_writeback_thread) {
		kthread_stop(auto_writeback_thread);
		auto_writeback_thread = NULL;
	}
	if (monitor_thread) {
		kthread_stop(monitor_thread);
		monitor_thread = NULL;
	}
#endif

#ifdef CONFIG_ZRAM_MULTI_COMP
	unregister_sysctl_table(zram_sysctl_table_header);
#endif //CONFIG_ZRAM_MULTI_COMP

	destroy_zram_writeback();
	destroy_devices();
}

module_init(zram_init);
module_exit(zram_exit);

module_param(num_devices, uint, 0);
MODULE_PARM_DESC(num_devices, "Number of pre-created zram devices");

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Nitin Gupta <ngupta@vflare.org>");
MODULE_AUTHOR("Coolapk@BrokeStar");
MODULE_DESCRIPTION("Compressed RAM Block Device");
