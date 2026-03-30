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
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/device.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/sort.h>
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
#include <linux/sysms_finder.h>
#include <linux/suspend.h>
#include <linux/spinlock.h>

#ifdef CONFIG_ZRAM_AUTO_SIZE
#include <linux/math64.h>
#include <linux/cpu.h>
#include <linux/minmax.h>
#include <linux/intfp.h>
#endif

#include "zram_drv.h"
#include "zram_wb.h"
#define CHECK_INTERVAL (150 * HZ) // 每150秒检查一次

/* 定义每次持有锁处理的页面数量，用于 mark_idle 分片式锁持有 */
#define MARK_IDLE_BATCH_SIZE 64

#define ZRAM_WB_MIN_OBJ_SIZE 256
#define ZRAM_BDEV_READ_BATCH_MAX 16
#define ZRAM_WB_READ_EWMA_WEIGHT 7
#define ZRAM_BDEV_READ_RUN_MAX 4

struct zram_wb_read_run {
	unsigned long start_handle;
	unsigned int page_start;
	unsigned int nr_pages;
};

static struct task_struct *monitor_thread;

static DEFINE_IDR(zram_index_idr);
/* idr index must be protected */
static DEFINE_MUTEX(zram_index_mutex);

static int zram_major;
static const char *default_compressor = CONFIG_ZRAM_DEF_COMP;

/* Module params (documentation at end) */
static unsigned int num_devices = 1;
#ifdef CONFIG_ZRAM_WRITEBACK
static unsigned int wb_read_policy = ZRAM_WB_READ_POLICY_STRICT;
static unsigned int wb_read_gap_pages = 1;
static unsigned int wb_frag_mode = ZRAM_WB_FRAG_MODE_ON;
#endif
/*
 * Pages that compress to sizes equals or greater than this are stored
 * uncompressed in memory.
 */
static size_t huge_class_size;

static const struct block_device_operations zram_devops;

static int zram_read_page(struct zram *zram, struct page *page, u32 index,
			  struct bio *parent);
enum zram_zspool_read_mode {
	ZRAM_ZS_READ_SAME,
	ZRAM_ZS_READ_HUGE,
	ZRAM_ZS_READ_COMPRESSED,
};

struct zram_zspool_read_ctx {
	enum zram_zspool_read_mode mode;
	unsigned long element;
	unsigned int size;
};

static int zram_prepare_read_from_zspool(struct zram *zram, u32 index,
					 struct page *tmp_page,
					 struct zram_zspool_read_ctx *ctx);
static int zram_finish_read_from_zspool(struct page *page,
					struct page *tmp_page,
					const struct zram_zspool_read_ctx *ctx,
					struct zcomp_strm *zstrm);
#ifdef CONFIG_ZRAM_WRITEBACK
static void zram_init_shrinker(struct zram *zram);
#endif

#ifdef CONFIG_ZRAM_WRITEBACK
unsigned int __read_mostly sysctl_zram_shrinker_active_window_ms = 5000;
#endif

static struct ctl_table zram_sysctl_table[] = {
#ifdef CONFIG_ZRAM_WRITEBACK
	{
		.procname	= "zram_shrinker_active_window_ms",
		.data		= &sysctl_zram_shrinker_active_window_ms,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= (void *)&(unsigned int){10000}, /* 最大 10 秒 */
	},
#endif
};
static struct ctl_table_header *zram_sysctl_table_header;

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

#ifdef CONFIG_ZRAM_WRITEBACK
static inline u32 zram_wb_ewma_update(u32 prev, u32 sample)
{
	return (prev * ZRAM_WB_READ_EWMA_WEIGHT + sample) /
		(ZRAM_WB_READ_EWMA_WEIGHT + 1);
}

static inline bool zram_is_quiescing(struct zram *zram)
{
	return atomic_read(&zram->quiescing);
}

static int zram_try_begin_pp(struct zram *zram)
{
	if (unlikely(zram_is_quiescing(zram)))
		return -EBUSY;

	if (atomic_cmpxchg(&zram->pp_in_progress, 0, 1))
		return -EAGAIN;

	smp_mb__after_atomic();
	if (unlikely(zram_is_quiescing(zram))) {
		atomic_set(&zram->pp_in_progress, 0);
		wake_up_all(&zram->pp_done_wait);
		return -EBUSY;
	}

	return 0;
}

static void zram_end_pp(struct zram *zram)
{
	if (atomic_xchg(&zram->pp_in_progress, 0))
		wake_up_all(&zram->pp_done_wait);
}

static void zram_wait_for_pp_idle(struct zram *zram)
{
	wait_event(zram->pp_done_wait, !atomic_read(&zram->pp_in_progress));
}

static void zram_begin_quiesce(struct zram *zram)
{
	atomic_set(&zram->quiescing, 1);
	smp_mb__after_atomic();
	wake_up_all(&zram->pp_done_wait);
}

static void zram_end_quiesce(struct zram *zram)
{
	atomic_set(&zram->quiescing, 0);
	wake_up_all(&zram->pp_done_wait);
}

static void zram_quiesce_device(struct zram *zram)
{
	zram_begin_quiesce(zram);
	blk_mq_freeze_queue(zram->disk->queue);
	blk_mq_quiesce_queue(zram->disk->queue);
	sync_blockdev(zram->disk->part0);
	zram_wait_for_pp_idle(zram);
	zram_wb_wait_for_idle(zram);
	invalidate_bdev(zram->disk->part0);
}

static void zram_unquiesce_device(struct zram *zram)
{
	zram_end_quiesce(zram);
	blk_mq_unquiesce_queue(zram->disk->queue);
	blk_mq_unfreeze_queue(zram->disk->queue);
}

static inline u8 zram_pick_wb_read_policy(struct zram *zram)
{
	if (zram->wb_read_policy != ZRAM_WB_READ_POLICY_ADAPTIVE)
		return zram->wb_read_policy;

	/*
	 * Adaptive policy:
	 * 1) fallback ratio high -> strict (avoid useless split bios)
	 * 2) fallback ratio low  -> relaxed (favor batching)
	 */
	if (READ_ONCE(zram->wb_read_fallback_ewma) >= 50)
		return ZRAM_WB_READ_POLICY_STRICT;

	return ZRAM_WB_READ_POLICY_RELAXED;
}
#else
static inline bool zram_is_quiescing(struct zram *zram)
{
	return false;
}

static inline int zram_try_begin_pp(struct zram *zram)
{
	return 0;
}

static inline void zram_end_pp(struct zram *zram) {}
static inline void zram_wait_for_pp_idle(struct zram *zram) {}
static inline void zram_begin_quiesce(struct zram *zram) {}
static inline void zram_end_quiesce(struct zram *zram) {}

static void zram_quiesce_device(struct zram *zram)
{
	blk_mq_freeze_queue(zram->disk->queue);
	blk_mq_quiesce_queue(zram->disk->queue);
	sync_blockdev(zram->disk->part0);
	invalidate_bdev(zram->disk->part0);
}

static void zram_unquiesce_device(struct zram *zram)
{
	blk_mq_unquiesce_queue(zram->disk->queue);
	blk_mq_unfreeze_queue(zram->disk->queue);
}
#endif

static inline bool init_done(struct zram *zram)
{
	return zram->disksize;
}

static inline struct zram *dev_to_zram(struct device *dev)
{
	return (struct zram *)dev_to_disk(dev)->private_data;
}

static inline struct zram_table_entry *zram_table_entry(struct zram *zram,
						 u32 index)
{
	return &zram->table[index];
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

static inline void zram_clear_flags(struct zram *zram, u32 index,
				    unsigned long mask)
{
	zram->table[index].flags &= ~mask;
}

void zram_set_flag(struct zram *zram, u32 index, enum zram_pageflags flag)
{
	zram->table[index].flags |= BIT(flag);
}

void zram_clear_flag(struct zram *zram, u32 index,
			enum zram_pageflags flag)
{
	zram->table[index].flags &= ~BIT(flag);
}

static inline bool zram_test_flag_atomic(struct zram *zram, u32 index,
					 enum zram_pageflags flag)
{
	return test_bit(flag, &zram->table[index].flags);
}

static inline void zram_set_flag_atomic(struct zram *zram, u32 index,
					enum zram_pageflags flag)
{
	set_bit(flag, &zram->table[index].flags);
}

static inline void zram_clear_flag_atomic(struct zram *zram, u32 index,
					  enum zram_pageflags flag)
{
	clear_bit(flag, &zram->table[index].flags);
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

static inline unsigned long zram_temp_to_flags(unsigned int temp)
{
	unsigned long bits = 0;

	if (temp & 1U)
		bits |= BIT(ZRAM_TEMP_0);
	if (temp & 2U)
		bits |= BIT(ZRAM_TEMP_1);
	if (temp & 4U)
		bits |= BIT(ZRAM_TEMP_2);

	return bits;
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
	flags |= zram_temp_to_flags(temp);
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

static void zram_set_active(struct zram *zram, u32 index);

#ifdef CONFIG_ZRAM_WRITEBACK
static int zram_wb_resources_init(struct zram *zram)
{
	zram->wb_page_pool = mempool_create_page_pool(1024, 0);
	if (!zram->wb_page_pool)
		goto err_pool;

	zram_init_shrinker(zram);
	if (!zram->zram_shrinker)
		goto err_shrinker;

	return 0;

err_shrinker:
	mempool_destroy(zram->wb_page_pool);
	zram->wb_page_pool = NULL;
err_pool:
	return -ENOMEM;
}

static void zram_wb_resources_free(struct zram *zram)
{
	if (zram->zram_shrinker) {
		unregister_shrinker(zram->zram_shrinker);
		shrinker_free(zram->zram_shrinker);
		zram->zram_shrinker = NULL;
	}
	if (zram->wb_page_pool) {
		mempool_destroy(zram->wb_page_pool);
		zram->wb_page_pool = NULL;
	}
}
#else
static inline int zram_wb_resources_init(struct zram *zram) { return 0; }
static inline void zram_wb_resources_free(struct zram *zram) { }
#endif

static void zram_commit_write(struct zram *zram, u32 index,
			      unsigned long handle, unsigned int size,
			      enum zram_pageflags new_flag,
			      unsigned long extra_flags)
{
	zram_slot_lock(zram, index);

	zram_free_page(zram, index);

	if (handle) {
		zram_set_handle(zram, index, handle);
		zram_set_obj_size(zram, index, size);
	}

	if (new_flag != 0)
		zram_set_flag(zram, index, new_flag);
	if (extra_flags)
		zram->table[index].flags |= extra_flags;

	zram_slot_unlock(zram, index);

	percpu_counter_inc(&zram->stats.pages_stored);
	if (size)
		percpu_counter_add(&zram->stats.compr_data_size, size);

	if (new_flag == ZRAM_HUGE) {
		percpu_counter_inc(&zram->stats.huge_pages);
		percpu_counter_inc(&zram->stats.huge_pages_since);
	} else if (new_flag == ZRAM_SAME) {
		percpu_counter_inc(&zram->stats.same_pages);
	}

#ifdef CONFIG_ZRAM_WRITEBACK
	zram_set_active(zram, index);
#endif
}

static unsigned long zram_page_flags(struct page *page)
{
	unsigned long flags = 0;

	if (PageAnon(page)) {
		flags |= BIT(ZRAM_PAGE_ANON);
	} else {
		flags |= BIT(ZRAM_PAGE_FILE);
		if (PageDirty(page) || PageWriteback(page))
			flags |= BIT(ZRAM_PAGE_DIRTY);
	}

	return flags;
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

#ifdef	CONFIG_ZRAM_WRITEBACK
static void zram_lru_add(struct zram *zram, struct zram_table_entry *entry)
{
	zram_set_flag_atomic(zram, entry - zram->table, ZRAM_REFERENCED);
	list_lru_add(&zram->zram_list_lru,
			     zram_wb_lru(zram, entry - zram->table));
}

static void zram_lru_del(struct zram *zram, struct zram_table_entry *entry)
{
	list_lru_del(&zram->zram_list_lru,
		     zram_wb_lru(zram, entry - zram->table));
}

static void zram_active_drain(struct zram *zram, struct zram_pagevec *pvec)
{
	int i;
	unsigned long flags;
	
	if (!pvec->nr)
		return;

	spin_lock_irqsave(&zram->active_list_lock, flags);
	for (i = 0; i < pvec->nr; i++) {
		unsigned long index = pvec->indices[i];
		struct list_head *lru = zram_wb_lru(zram, index);

		if (zram_test_flag_atomic(zram, index, ZRAM_ACTIVE)) {
			if (list_empty(lru)) {
				list_add_tail(lru, &zram->active_list);
				atomic_long_inc(&zram->active_pages);
			} else {
				list_move_tail(lru, &zram->active_list);
			}

			zram_clear_flag_atomic(zram, index, ZRAM_REFERENCED);
		}
	}
	spin_unlock_irqrestore(&zram->active_list_lock, flags);
	pvec->nr = 0;
}

static void zram_queue_active(struct zram *zram, u32 index)
{
	struct zram_pagevec *pvec;
	unsigned long flags;

	pvec = get_cpu_ptr(zram->active_pagevecs);
	spin_lock_irqsave(&pvec->lock, flags);

	if (pvec->nr < ZRAM_PAGEVEC_SIZE)
		pvec->indices[pvec->nr++] = index;

	if (pvec->nr >= ZRAM_PAGEVEC_SIZE)
		zram_active_drain(zram, pvec);

	spin_unlock_irqrestore(&pvec->lock, flags);
	put_cpu_ptr(zram->active_pagevecs);
}

static void zram_drain_all_pages(struct zram *zram)
{
	int cpu;
	
	for_each_possible_cpu(cpu) {
		struct zram_pagevec *pvec = per_cpu_ptr(zram->active_pagevecs, cpu);
		spin_lock_irq(&pvec->lock);
		zram_active_drain(zram, pvec);
		spin_unlock_irq(&pvec->lock);
	}
}

static void zram_set_active(struct zram *zram, u32 index)
{
	zram_slot_lock(zram, index);

	/* 3-bit saturating temperature (0..7). */
	zram_raise_temp_locked(zram, index, ZRAM_TEMP_INC_READ);
	
	/* 如果已经在活跃状态或 IDLE 状态，只设置 REFERENCED 标志 */
	if (zram_test_flag(zram, index, ZRAM_ACTIVE) ||
	    zram_test_flag(zram, index, ZRAM_IDLE)) {
		zram_set_flag(zram, index, ZRAM_REFERENCED);
		zram_slot_unlock(zram, index);
		return;
	}

	/* 标记为活跃，准备加入 active_list */
	zram_set_flag(zram, index, ZRAM_ACTIVE);
	zram_slot_unlock(zram, index);

	zram_queue_active(zram, index);
}

static void zram_promote_accessed(struct zram *zram, u32 index, bool from_wb)
{
	bool need_enqueue = false;
	bool count_readback = false;

	zram_slot_lock(zram, index);

	/* 普通访问 +2；从 backing device 读回直接回到最高温，重新开始降温流程。 */
	if (from_wb)
		zram_set_temp_locked(zram, index, ZRAM_TEMP_MAX);
	else
		zram_raise_temp_locked(zram, index, ZRAM_TEMP_INC_READ);

	if (from_wb && zram_test_flag(zram, index, ZRAM_WB_READ_ONCE)) {
		zram_clear_flag(zram, index, ZRAM_WB_READ_ONCE);
		count_readback = true;
	}

	if (zram_test_flag(zram, index, ZRAM_IDLE)) {
		zram_clear_flag(zram, index, ZRAM_IDLE);
		zram_lru_del(zram, &zram->table[index]);
	}

	if (zram_test_flag(zram, index, ZRAM_PP_SLOT))
		zram_clear_flag(zram, index, ZRAM_PP_SLOT);

	if (zram_test_flag(zram, index, ZRAM_WB_SECOND_CHANCE))
		zram_clear_flag(zram, index, ZRAM_WB_SECOND_CHANCE);

	zram_set_flag(zram, index, ZRAM_REFERENCED);

	if (!zram_test_flag(zram, index, ZRAM_ACTIVE)) {
		zram_set_flag(zram, index, ZRAM_ACTIVE);
		need_enqueue = true;
	}

	zram_slot_unlock(zram, index);

	if (count_readback)
		percpu_counter_inc(&zram->stats.bd_reads);

	if (need_enqueue)
		zram_queue_active(zram, index);
}
#endif

#if defined CONFIG_ZRAM_WRITEBACK
static struct zram_pp_ctl *init_pp_ctl_timeout(unsigned long timeout_ms, gfp_t gfp_mask)
{
	struct zram_pp_ctl *ctl;
	u32 idx;

	ctl = kmalloc(sizeof(*ctl), gfp_mask);
	if (!ctl)
		return NULL;

	init_completion(&ctl->all_done);
	atomic_set(&ctl->num_pp_slots, 0);
	for (idx = 0; idx < NUM_PP_BUCKETS; idx++)
		INIT_LIST_HEAD(&ctl->pp_buckets[idx]);
	
	if (timeout_ms > 0)
		ctl->deadline_jiffies = jiffies + msecs_to_jiffies(timeout_ms);
	else
		ctl->deadline_jiffies = 0;  /* 无时间限制 */
	
	return ctl;
}

static struct zram_pp_ctl *init_pp_ctl(void)
{
	return init_pp_ctl_timeout(0, GFP_KERNEL);
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
			  u32 index, gfp_t gfp_mask)
{
	struct zram_pp_slot *pps;
	u32 bid;

	pps = kmalloc(sizeof(*pps), gfp_mask);
	if (!pps)
		return false;

	INIT_LIST_HEAD(&pps->entry);
	pps->index = index;

	bid = zram_get_obj_size(zram, pps->index) / PP_BUCKET_SIZE_RANGE;
	list_add(&pps->entry, &ctl->pp_buckets[bid]);

	zram_set_flag(zram, pps->index, ZRAM_PP_SLOT);
	return true;
}

static int compare_pp_slot_index(const void *a, const void *b)
{
	const struct zram_pp_slot * const *pa = a;
	const struct zram_pp_slot * const *pb = b;

	if ((*pa)->index < (*pb)->index)
		return -1;
	if ((*pa)->index > (*pb)->index)
		return 1;
	return 0;
}

static int compare_pp_slot_frag_aware(const void *a, const void *b)
{
	const struct zram_pp_slot *sa = *(const struct zram_pp_slot * const *)a;
	const struct zram_pp_slot *sb = *(const struct zram_pp_slot * const *)b;
	unsigned long ia = sa->index;
	unsigned long ib = sb->index;

	/*
	 * Low bits encode a tiny pseudo-fragmentation score. Lower score first.
	 * This is intentionally lightweight and deterministic for reclaim path.
	 */
	unsigned int fa = (unsigned int)(ia & 0x3UL);
	unsigned int fb = (unsigned int)(ib & 0x3UL);

	if (fa < fb)
		return -1;
	if (fa > fb)
		return 1;

	if (ia < ib)
		return -1;
	if (ia > ib)
		return 1;
	return 0;
}

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

	/*
	 * 【优化】使用 clear_page 处理全 0 页面清零。
	 * 现代 CPU 架构（如 ARM64 的 dc zva, x86 的 rep stos）对页面级清零有专门优化，
	 * 比通用 memset 更能压榨内存带宽。全 0 页面在 ZRAM 中占比很高（通常 >60%）。
	 */
	if (likely(value == 0 && len == PAGE_SIZE)) {
		clear_page(ptr);
	} else if (value == 0) {
		memset(ptr, 0, len);
	} else {
		memset_l(ptr, value, len / sizeof(unsigned long));
	}
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
 * Uses atomic counter to track active list length and scan only once.
 * @max_scan: maximum number of pages to scan, 0 means unlimited
 */

static void mark_idle(struct zram *zram, ktime_t cutoff, int max_scan)
{
	unsigned long index_array[MARK_IDLE_BATCH_SIZE];
	unsigned long flags;
	int nr_indices = 0;
	int scanned = 0;
	int marked_idle = 0;
	unsigned long active_total;
	int target_scan;
	int batch_count = 0;
	int i;

	zram_drain_all_pages(zram);

	active_total = atomic_long_read(&zram->active_pages);
	
	target_scan = active_total;
	if (max_scan > 0 && target_scan > max_scan)
		target_scan = max_scan;
	
	/* 没有任何活跃页面，直接返回 */
	if (target_scan <= 0)
		return;

	spin_lock_irqsave(&zram->active_list_lock, flags);
	
	while (!list_empty(&zram->active_list) && scanned < target_scan) {
		unsigned long index;
		struct zram_wb_table_entry *wb_entry;
		
		wb_entry = list_first_entry(&zram->active_list,
					    struct zram_wb_table_entry, lru);
		index = wb_entry - zram->wb_table;
		
		scanned++;
		batch_count++;
		
		if (zram_test_flag_atomic(zram, index, ZRAM_REFERENCED)) {
			zram_clear_flag_atomic(zram, index, ZRAM_REFERENCED);
			list_move_tail(&wb_entry->lru, &zram->active_list);
			goto check_batch;
		}
		
#ifdef CONFIG_ZRAM_TRACK_ENTRY_ACTIME
		if (cutoff) {
			ktime_t ac_time = zram_read_ac_time(zram, index);
			if (ktime_after(ac_time, cutoff)) {
				list_move_tail(&wb_entry->lru, &zram->active_list);
				goto check_batch;
			}
		}
#endif

		if (!zram_slot_trylock(zram, index)) {
			list_move_tail(&wb_entry->lru, &zram->active_list);
			goto check_batch;
		}
		
		if (zram_test_flag(zram, index, ZRAM_WB) ||
		    !zram_allocated(zram, index)) {
			zram_slot_unlock(zram, index);
			list_del_init(&wb_entry->lru);
			zram_clear_flag_atomic(zram, index, ZRAM_ACTIVE);
			atomic_long_dec(&zram->active_pages);
			continue;
		}
		
		zram_clear_flag(zram, index, ZRAM_ACTIVE);
		zram_set_flag(zram, index, ZRAM_IDLE);
		atomic_long_dec(&zram->active_pages);
		marked_idle++;
		
		/* 保存 index 而不是移动到临时链表 */
		index_array[nr_indices++] = index;
		list_del_init(&wb_entry->lru);
		
		zram_slot_unlock(zram, index);
		
	check_batch:
		if (batch_count >= MARK_IDLE_BATCH_SIZE || need_resched()) {
			spin_unlock_irqrestore(&zram->active_list_lock, flags);
			
			/* 在不持锁期间处理已移出的页面 */
			for (i = 0; i < nr_indices; i++) {
				zram_lru_add(zram, zram_table_entry(zram, index_array[i]));
			}
			nr_indices = 0;
			
			cond_resched();
			batch_count = 0;
			spin_lock_irqsave(&zram->active_list_lock, flags);
		}
	}
	
	spin_unlock_irqrestore(&zram->active_list_lock, flags);
	
	/* 批量将冷页面移入 zram_list_lru */
	for (i = 0; i < nr_indices; i++) {
		zram_lru_add(zram, zram_table_entry(zram, index_array[i]));
	}
	pr_debug("zram: mark_idle scanned=%d, marked_idle=%d, active_total=%ld\n",
		 scanned, marked_idle, active_total);
}

static ssize_t idle_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	ktime_t cutoff_time = 0;
	ssize_t rv = -EINVAL;

	if (!sysfs_streq(buf, "all")) {
		u64 age_sec;

		if (IS_ENABLED(CONFIG_ZRAM_TRACK_ENTRY_ACTIME) && !kstrtoull(buf, 0, &age_sec))
			cutoff_time = ktime_sub(ktime_get_boottime(),
					ns_to_ktime(age_sec * NSEC_PER_SEC));
		else
			goto out;
	}
	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		up_read(&zram->init_lock);
		goto out;
	}
	mark_idle(zram, cutoff_time, 0);  /* 0 = unlimited for sysfs interface */
	up_read(&zram->init_lock);
	rv = len;

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
	sync_blockdev(bdev);
	invalidate_bdev(bdev);
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

static int read_from_bdev_batch(struct zram *zram, struct page **pages,
				unsigned int nr_pages, unsigned long entry,
				struct bio *parent);

static unsigned int zram_collect_bdev_read_runs(struct zram *zram,
					struct bio *bio,
					struct bvec_iter iter,
					unsigned long start_handle,
					struct page **pages,
					struct zram_wb_read_run *runs,
					unsigned int max_runs,
					unsigned int *run_count,
					unsigned int *bytes,
					u8 policy,
					u8 gap_pages)
{
	unsigned int nr_pages = 0;
	unsigned int nr_runs = 0;
	unsigned long next_handle = start_handle;
	struct zram_wb_read_run *cur_run = NULL;
	unsigned int page_cap = ZRAM_BDEV_READ_BATCH_MAX;

	if (!max_runs || !runs || !run_count || !bytes)
		goto out;

	while (iter.bi_size && nr_pages < page_cap &&
	       nr_runs < max_runs) {
		u32 index = iter.bi_sector >> SECTORS_PER_PAGE_SHIFT;
		u32 offset = (iter.bi_sector & (SECTORS_PER_PAGE - 1)) <<
				SECTOR_SHIFT;
		struct bio_vec bv = bio_iter_iovec(bio, iter);
		unsigned long flags;
		unsigned long handle;

		bv.bv_len = min_t(u32, bv.bv_len, PAGE_SIZE - offset);
		if (offset || is_partial_io(&bv))
			break;

		flags = READ_ONCE(zram->table[index].flags);
		if (!(flags & BIT(ZRAM_WB)))
			break;

		if (flags & (BIT(ZRAM_PP_SLOT) | BIT(ZRAM_LOCK)))
			break;

		handle = READ_ONCE(zram->table[index].handle);
		if (!handle)
			break;

		if (likely(handle == next_handle)) {
			if (!cur_run) {
				cur_run = &runs[nr_runs++];
				cur_run->start_handle = handle;
				cur_run->page_start = nr_pages;
				cur_run->nr_pages = 0;
			}
		} else {
			unsigned long gap;

			if (policy == ZRAM_WB_READ_POLICY_STRICT)
				break;

			if (handle < next_handle)
				break;

			gap = handle - next_handle;
			if (gap > gap_pages || nr_runs >= max_runs)
				break;

			cur_run = &runs[nr_runs++];
			cur_run->start_handle = handle;
			cur_run->page_start = nr_pages;
			cur_run->nr_pages = 0;
		}

		pages[nr_pages++] = bv.bv_page;
		cur_run->nr_pages++;
		next_handle++;
		bio_advance_iter(bio, &iter, bv.bv_len);
	}

out:
	*run_count = nr_runs;
	*bytes = nr_pages * PAGE_SIZE;
	return nr_pages;
}

static int read_from_bdev_runs(struct zram *zram,
			      struct page **pages,
			      struct zram_wb_read_run *runs,
			      unsigned int run_count,
			      struct bio *parent)
{
	unsigned int i;
	int ret;

	for (i = 0; i < run_count; i++) {
		ret = read_from_bdev_batch(zram,
					   &pages[runs[i].page_start],
					   runs[i].nr_pages,
					   runs[i].start_handle,
					   parent);
		if (ret)
			return ret;
	}

	return 0;
}

static int read_from_bdev_async(struct zram *zram, struct page **pages,
				unsigned int nr_pages, unsigned long entry,
				struct bio *parent)
{
	struct bio *bio;
	unsigned int i;

	bio = bio_alloc_bioset(zram->bdev, nr_pages, parent->bi_opf, GFP_NOIO,
				&zram->zram_bio_set);
	if (!bio)
		return -ENOMEM;

	bio->bi_iter.bi_sector = entry * (PAGE_SIZE >> 9);
	for (i = 0; i < nr_pages; i++) {
		flush_dcache_page(pages[i]);
		if (bio_add_page(bio, pages[i], PAGE_SIZE, 0) < PAGE_SIZE) {
			bio_put(bio);
			return -EIO;
		}
	}

	bio_chain(bio, parent);
	submit_bio(bio);

	return 0;
}

static int zram_writeback_slots(struct zram *zram, struct zram_pp_ctl *ctl, bool async)
{
	unsigned long batch_base_idx = 0;
	int batch_count = 0;
	int batch_cursor = 0;
	int sorted_nr = 0;
	int sorted_pos = 0;
	struct zram_wb_batch_request *active_req = NULL;
	struct zram_pp_slot *sorted_pps[ZRAM_WB_MAX_BATCH_SIZE];
	struct zram_pp_slot *pps;
	int ret = 0;
	gfp_t page_gfp_mask;
	u64 batch_credit_units = 0;
	const u64 wb_units_per_page = 1ULL << (PAGE_SHIFT - 12);

	if (async)
		page_gfp_mask = GFP_ATOMIC | __GFP_NOWARN | __GFP_MOVABLE;
	else
		page_gfp_mask = GFP_NOIO | __GFP_NOWARN | __GFP_MOVABLE;

	if (!async)
		atomic_set(&ctl->num_pp_slots, 1);

	while (1) {
		struct page *page;
		struct page *tmp_page;
		struct zcomp_strm *zstrm;
		struct zram_zspool_read_ctx read_ctx;
		unsigned long current_blk_idx;
		u32 index;

		tmp_page = NULL;
		zstrm = NULL;

		if (sorted_pos >= sorted_nr) {
			sorted_nr = 0;
			sorted_pos = 0;

			while (sorted_nr < ZRAM_WB_MAX_BATCH_SIZE &&
			       (pps = select_pp_slot(ctl))) {
				if (ctl->deadline_jiffies &&
				    time_after(jiffies, ctl->deadline_jiffies)) {
					release_pp_slot(zram, pps);
					break;
				}

				remove_pp_slot_from_ctl(pps);
				sorted_pps[sorted_nr++] = pps;
			}

			if (sorted_nr > 1) {
				int (*cmp)(const void *a, const void *b) =
					compare_pp_slot_index;

				if (zram->wb_frag_mode == ZRAM_WB_FRAG_MODE_ON)
					cmp = compare_pp_slot_frag_aware;

				sort(sorted_pps, sorted_nr, sizeof(sorted_pps[0]), cmp,
				     NULL);
			}

			if (sorted_nr > 1) {
				struct zram_pp_slot *run_priority[ZRAM_WB_MAX_BATCH_SIZE];
				int out = 0;
				int i = 0;

				/* Prefer longer contiguous index runs first. */
				while (i < sorted_nr) {
					int j = i + 1;

					while (j < sorted_nr &&
					       sorted_pps[j]->index == sorted_pps[j - 1]->index + 1)
						j++;

					if (j - i >= 4) {
						int k;

						for (k = i; k < j; k++)
							run_priority[out++] = sorted_pps[k];
					}

					i = j;
				}

				i = 0;
				while (i < sorted_nr) {
					int j = i + 1;

					while (j < sorted_nr &&
					       sorted_pps[j]->index == sorted_pps[j - 1]->index + 1)
						j++;

					if (j - i < 4) {
						int k;

						for (k = i; k < j; k++)
							run_priority[out++] = sorted_pps[k];
					}

					i = j;
				}

				memcpy(sorted_pps, run_priority,
				       out * sizeof(run_priority[0]));
			}

			if (!sorted_nr)
				break;
		}

		pps = sorted_pps[sorted_pos++];
		index = pps->index;

		if (unlikely(zram_is_quiescing(zram))) {
			release_pp_slot(zram, pps);
			ret = -EBUSY;
			break;
		}

		{
			int run_len = 1;
			bool run_start;

			run_start = (sorted_pos == 1) ||
				(sorted_pps[sorted_pos - 2]->index + 1 != index);

			while (sorted_pos - 1 + run_len < sorted_nr &&
			       sorted_pps[sorted_pos - 1 + run_len]->index ==
			       index + run_len)
				run_len++;

			/*
			 * Keep a contiguous run mapped into a single reserved extent
			 * whenever possible.
			 */
			if (run_start && run_len > 1 && batch_count > batch_cursor &&
			    (batch_count - batch_cursor) < run_len) {
				if (active_req) {
					zram_wb_submit_batch(active_req);
					active_req = NULL;
				}

				if (batch_credit_units) {
					spin_lock(&zram->wb_limit_lock);
					zram->bd_wb_limit += batch_credit_units;
					spin_unlock(&zram->wb_limit_lock);
					batch_credit_units = 0;
				}

				if (batch_base_idx && batch_count > batch_cursor) {
					free_block_bdev_range(zram,
							      batch_base_idx + batch_cursor,
							      batch_count - batch_cursor);
				}

				batch_cursor = batch_count;
			}
		}

		/* --- 1. 物理块管理（按 batch 预取配额，降低锁竞争） --- */
		if (batch_cursor >= batch_count) {
			bool wb_limit_enable;

			if (active_req) {
				zram_wb_submit_batch(active_req);
				active_req = NULL;
			}

			if (batch_credit_units) {
				spin_lock(&zram->wb_limit_lock);
				zram->bd_wb_limit += batch_credit_units;
				spin_unlock(&zram->wb_limit_lock);
				batch_credit_units = 0;
			}

			if (batch_base_idx && batch_count > batch_cursor) {
				free_block_bdev_range(zram,
						      batch_base_idx + batch_cursor,
						      batch_count - batch_cursor);
			}

			int want_count = ZRAM_WB_MAX_BATCH_SIZE;
			u64 max_wb_units = (u64)want_count * wb_units_per_page;

			batch_cursor = 0;
			batch_count = 0;
			wb_limit_enable = READ_ONCE(zram->wb_limit_enable);

			spin_lock(&zram->wb_limit_lock);
			if (wb_limit_enable) {
				u64 max_allowed_pages;

				if (!zram->bd_wb_limit) {
					spin_unlock(&zram->wb_limit_lock);
					release_pp_slot(zram, pps);
					ret = -EIO;
					break;
				}

				batch_credit_units = min(zram->bd_wb_limit, max_wb_units);
				zram->bd_wb_limit -= batch_credit_units;
				max_allowed_pages = div_u64(batch_credit_units, wb_units_per_page);
				want_count = min_t(int, want_count, (int)max_allowed_pages);
			}
			spin_unlock(&zram->wb_limit_lock);

			batch_base_idx = alloc_block_bdev_batch(zram, want_count, &batch_count);

			if (!batch_base_idx) {
				if (batch_credit_units) {
					spin_lock(&zram->wb_limit_lock);
					zram->bd_wb_limit += batch_credit_units;
					spin_unlock(&zram->wb_limit_lock);
					batch_credit_units = 0;
				}
				release_pp_slot(zram, pps);
				ret = -ENOSPC;
				break;
			}
		}

		current_blk_idx = batch_base_idx + batch_cursor;

		/* --- 2. BIO 请求管理 --- */
		if (!active_req) {
			active_req = alloc_wb_batch_request(zram, async ? NULL : ctl,
							    current_blk_idx, page_gfp_mask);
			if (!active_req) {
				release_pp_slot(zram, pps);
				if (async) {
					ret = 0;
					break;
				}
				ret = -ENOMEM;
				break;
			}
		}

		/* --- 3. 准备数据页 --- */
		page = mempool_alloc(zram->wb_page_pool, async ? GFP_NOWAIT : GFP_NOIO);
		if (!page) {
			if (active_req && active_req->count > 0) {
				zram_wb_submit_batch(active_req);
				active_req = NULL;
			} else if (active_req) {
				bio_put(active_req->bio);
				active_req = NULL;
			}
			release_pp_slot(zram, pps);
			ret = -ENOMEM;
			break;
		}
		/* --- 4. 读取 ZRAM 数据 --- */
	retry_read:
		zram_slot_lock(zram, index);
		if (!zram_test_flag(zram, index, ZRAM_PP_SLOT) ||
		    zram_test_flag(zram, index, ZRAM_WB)) {
			zram_slot_unlock(zram, index);
			mempool_free(page, zram->wb_page_pool);
			release_pp_slot(zram, pps);
			if (tmp_page)
				mempool_free(tmp_page, zram->io_page_pool);
			continue;
		}

		if (!tmp_page &&
		    !zram_test_flag(zram, index, ZRAM_SAME) &&
		    zram_get_handle(zram, index)) {
			zram_slot_unlock(zram, index);
			tmp_page = mempool_alloc(zram->io_page_pool,
						 async ? GFP_NOWAIT | __GFP_NOWARN :
						 GFP_NOIO | __GFP_NOWARN);
			if (!tmp_page) {
				mempool_free(page, zram->wb_page_pool);
				release_pp_slot(zram, pps);
				ret = -ENOMEM;
				break;
			}
			goto retry_read;
		}

		ret = zram_prepare_read_from_zspool(zram, index, tmp_page, &read_ctx);
		zram_slot_unlock(zram, index);

		if (!ret) {
			if (read_ctx.mode == ZRAM_ZS_READ_COMPRESSED)
				zstrm = zcomp_stream_get(zram->comp);

			ret = zram_finish_read_from_zspool(page, tmp_page, &read_ctx,
							   zstrm);

			if (zstrm)
				zcomp_stream_put(zram->comp);
		}

		if (tmp_page)
			mempool_free(tmp_page, zram->io_page_pool);

		if (ret) {
			mempool_free(page, zram->wb_page_pool);
			release_pp_slot(zram, pps);
			continue;
		}

#ifdef CONFIG_ZRAM_WRITEBACK
		zram_set_active(zram, index);
#endif

		/* --- 5. 将页面加入 BIO --- */
		if (bio_add_page(active_req->bio, page, PAGE_SIZE, 0) < PAGE_SIZE) {
			/* BIO 满了，提交旧的，开新的 */
			zram_wb_submit_batch(active_req);
			active_req = alloc_wb_batch_request(zram, async ? NULL : ctl,
						    current_blk_idx, page_gfp_mask);
			if (!active_req) {
				mempool_free(page, zram->wb_page_pool);
				release_pp_slot(zram, pps);
				if (async) {
					ret = 0;
					break;
				}
				ret = -ENOMEM;
				break;
			}
			if (bio_add_page(active_req->bio, page, PAGE_SIZE, 0) < PAGE_SIZE) {
				bio_put(active_req->bio);
				active_req = NULL;
				mempool_free(page, zram->wb_page_pool);
				release_pp_slot(zram, pps);
				ret = -EIO;
				break;
			}
		}

		/* --- 6. 成功添加 --- */
		if (!async)
			atomic_inc(&ctl->num_pp_slots);

		int idx = active_req->count++;
		active_req->sub_reqs[idx].pps = pps;
		active_req->sub_reqs[idx].blk_idx = current_blk_idx;
		active_req->sub_reqs[idx].index = index;
		batch_cursor++;
		if (batch_credit_units >= wb_units_per_page) {
			batch_credit_units -= wb_units_per_page;
			active_req->reserved_wb_units += wb_units_per_page;
		}

		if (active_req->count >= ZRAM_WB_MAX_BATCH_SIZE) {
			zram_wb_submit_batch(active_req);
			active_req = NULL;
		}
	}

	while (sorted_pos < sorted_nr)
		release_pp_slot(zram, sorted_pps[sorted_pos++]);

	/* 循环结束，提交残留的 BIO */
	if (active_req) {
		if (active_req->count > 0) {
			zram_wb_submit_batch(active_req);
		} else {
			bio_put(active_req->bio);
		}
		active_req = NULL;
	}

	/* --- 8. 清理 --- */
	if (batch_base_idx && batch_count > batch_cursor) {
		free_block_bdev_range(zram,
				      batch_base_idx + batch_cursor,
				      batch_count - batch_cursor);
	}
	if (batch_credit_units) {
		spin_lock(&zram->wb_limit_lock);
		zram->bd_wb_limit += batch_credit_units;
		spin_unlock(&zram->wb_limit_lock);
	}

	if (!async) {
		if (atomic_dec_and_test(&ctl->num_pp_slots))
			complete(&ctl->all_done);
		wait_for_completion(&ctl->all_done);
	}

	return ret;
}

#define PAGE_WRITEBACK			0
#define HUGE_WRITEBACK			(1 << 0)
#define IDLE_WRITEBACK			(1 << 1)
#define INCOMPRESSIBLE_WRITEBACK	(1 << 2)

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
				    struct zram_pp_ctl *ctl,
				    int nr_limit, gfp_t gfp_mask)
{
	u32 index = lo;
	int pages_found = 0;

	while (index < hi) {
		bool ok = true;

		if (nr_limit > 0 && pages_found >= nr_limit)
			break;

		/* 检查时间限制 */
		if (ctl->deadline_jiffies && time_after(jiffies, ctl->deadline_jiffies)) {
			pr_info("zram: writeback scan timeout after %d pages\n", pages_found);
			break;
		}

		/* 每 64MB (16384 页) 让出 CPU，防止长时间占用 */
		if ((index & 0x3FFF) == 0)
			cond_resched();

		zram_slot_lock(zram, index);
		if (!zram_allocated(zram, index))
			goto next;

		if (zram_test_flag(zram, index, ZRAM_WB) ||
		    zram_test_flag(zram, index, ZRAM_SAME))
			goto next;

		if (zram_test_flag(zram, index, ZRAM_PAGE_FILE)) {
			atomic64_inc(&zram->stats.wb_pages_skipped);
			if (zram_test_flag(zram, index, ZRAM_IDLE)) {
				zram_clear_flag(zram, index, ZRAM_IDLE);
				zram_lru_del(zram, &zram->table[index]);
			}
			goto next;
		}

		if (!zram_test_flag(zram, index, ZRAM_PAGE_ANON)) {
			atomic64_inc(&zram->stats.wb_pages_skipped);
			goto next;
		}

		if (mode & IDLE_WRITEBACK &&
		    !zram_test_flag(zram, index, ZRAM_IDLE))
			goto next;
		if (mode & HUGE_WRITEBACK &&
		    !zram_test_flag(zram, index, ZRAM_HUGE))
			goto next;
		if (mode & INCOMPRESSIBLE_WRITEBACK &&
		    !zram_test_flag(zram, index, ZRAM_INCOMPRESSIBLE))
			goto next;

		/* ROI filter for idle writeback: skip tiny compressed objects */
		if (mode == IDLE_WRITEBACK) {
			size_t obj_size = zram_get_obj_size(zram, index);
			unsigned int temp = zram_get_temp_locked(zram, index);

			if (temp > 0) {
				zram_set_temp_locked(zram, index, temp - 1);
				atomic64_inc(&zram->stats.wb_pages_skipped);
				goto next;
			}

			if (obj_size > 0 && obj_size < ZRAM_WB_MIN_OBJ_SIZE) {
				zram_clear_flag(zram, index, ZRAM_IDLE);
				zram_lru_del(zram, &zram->table[index]);
				goto next;
			}

			/* First hit gets a pass; write back only if it survives next cycle. */
			if (!zram_test_flag(zram, index, ZRAM_WB_SECOND_CHANCE)) {
				zram_set_flag(zram, index, ZRAM_WB_SECOND_CHANCE);
				atomic64_inc(&zram->stats.wb_pages_skipped);
				goto next;
			}
			zram_clear_flag(zram, index, ZRAM_WB_SECOND_CHANCE);
		}

		ok = place_pp_slot(zram, ctl, index, gfp_mask);
		if (ok)
			pages_found++;
next:
		zram_slot_unlock(zram, index);
		if (!ok)
			break;
		index++;
	}

	return pages_found;
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
	err = zram_try_begin_pp(zram);
	if (err) {
		up_read(&zram->init_lock);
		return err;
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

				scan_slots_for_writeback(zram, mode, lo, hi, ctl, 0,
						 GFP_KERNEL);
				break;
		}

		if (!strcmp(param, "type")) {
			err = parse_mode(val, &mode);
			if (err) {
				ret = err;
				goto release_init_lock;
			}

				scan_slots_for_writeback(zram, mode, lo, hi, ctl, 0,
						 GFP_KERNEL);
				break;
		}

		if (!strcmp(param, "page_index")) {
			err = parse_page_index(val, nr_pages, &lo, &hi);
			if (err) {
				ret = err;
				goto release_init_lock;
			}

				scan_slots_for_writeback(zram, mode, lo, hi, ctl, 0,
						 GFP_KERNEL);
				continue;
		}

		if (!strcmp(param, "page_indexes")) {
			err = parse_page_indexes(val, nr_pages, &lo, &hi);
			if (err) {
				ret = err;
				goto release_init_lock;
			}

				scan_slots_for_writeback(zram, mode, lo, hi, ctl, 0,
						 GFP_KERNEL);
				continue;
		}
	}

	/*
	 * 修改点：Sysfs 路径使用同步模式 (async = false)
	 * 这样用户态触发回写时可以等待完成，避免误导用户
	 */
	err = zram_writeback_slots(zram, ctl, false);
	if (err)
		ret = err;

release_init_lock:
	release_pp_ctl(zram, ctl);
	zram_end_pp(zram);
	up_read(&zram->init_lock);

	return ret;
}

static int read_from_bdev_sync(struct zram *zram, struct page **pages,
			       unsigned int nr_pages, unsigned long entry)
{
	struct bio *bio;
	unsigned int i;
	int err;

	bio = bio_alloc_bioset(zram->bdev, nr_pages, REQ_OP_READ, GFP_NOIO,
				&zram->zram_bio_set);
	if (!bio)
		return -ENOMEM;

	bio->bi_iter.bi_sector = entry * (PAGE_SIZE >> 9);
	for (i = 0; i < nr_pages; i++) {
		flush_dcache_page(pages[i]);
		if (bio_add_page(bio, pages[i], PAGE_SIZE, 0) < PAGE_SIZE) {
			bio_put(bio);
			return -EIO;
		}
	}

	err = submit_bio_wait(bio);
	bio_put(bio);

	return err;
}

static int read_from_bdev(struct zram *zram, struct page *page,
			unsigned long entry, struct bio *parent)
{
	struct page *pages[1] = { page };

	if (!parent) {
		if (WARN_ON_ONCE(!IS_ENABLED(ZRAM_PARTIAL_IO)))
			return -EIO;
		return read_from_bdev_sync(zram, pages, ARRAY_SIZE(pages), entry);
	}
	return read_from_bdev_async(zram, pages, ARRAY_SIZE(pages), entry,
				    parent);
	}

static int read_from_bdev_batch(struct zram *zram, struct page **pages,
				unsigned int nr_pages, unsigned long entry,
				struct bio *parent)
{
	if (!parent)
		return read_from_bdev_sync(zram, pages, nr_pages, entry);

	return read_from_bdev_async(zram, pages, nr_pages, entry, parent);
}
#else
static inline void reset_bdev(struct zram *zram) {};
static int read_from_bdev(struct zram *zram, struct page *page,
			unsigned long entry, struct bio *parent)
{
	return -EIO;
}
#endif

#ifdef CONFIG_ZRAM_MEMORY_TRACKING

static struct dentry *zram_debugfs_root;

static void zram_debugfs_create(void)
{
	zram_debugfs_root = debugfs_create_dir("zram", NULL);
}

static void zram_debugfs_destroy(void)
{
	debugfs_remove_recursive(zram_debugfs_root);
}

static ssize_t read_block_state(struct file *file, char __user *buf,
				size_t count, loff_t *ppos)
{
	char *kbuf;
	ssize_t index, written = 0;
	struct zram *zram = file->private_data;
	unsigned long nr_pages = zram->disksize >> PAGE_SHIFT;
	struct timespec64 ts;

	kbuf = kvmalloc(count, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		up_read(&zram->init_lock);
		kvfree(kbuf);
		return -EINVAL;
	}

	for (index = *ppos; index < nr_pages; index++) {
		int copied;

		zram_slot_lock(zram, index);
		if (!zram_allocated(zram, index))
			goto next;

		ts = ktime_to_timespec64(zram_read_ac_time(zram, index));
		copied = snprintf(kbuf + written, count,
			"%12zd %12lld.%06lu %c%c%c%c%c\n",
			index, (s64)ts.tv_sec,
			ts.tv_nsec / NSEC_PER_USEC,
			zram_test_flag(zram, index, ZRAM_SAME) ? 's' : '.',
			zram_test_flag(zram, index, ZRAM_WB) ? 'w' : '.',
			zram_test_flag(zram, index, ZRAM_HUGE) ? 'h' : '.',
			zram_test_flag(zram, index, ZRAM_IDLE) ? 'i' : '.',
			zram_test_flag(zram, index,
				       ZRAM_INCOMPRESSIBLE) ? 'n' : '.');

		if (count <= copied) {
			zram_slot_unlock(zram, index);
			break;
		}
		written += copied;
		count -= copied;
next:
		zram_slot_unlock(zram, index);
		*ppos += 1;
	}

	up_read(&zram->init_lock);
	if (copy_to_user(buf, kbuf, written))
		written = -EFAULT;
	kvfree(kbuf);

	return written;
}

static const struct file_operations proc_zram_block_state_op = {
	.open = simple_open,
	.read = read_block_state,
	.llseek = default_llseek,
};

static void zram_debugfs_register(struct zram *zram)
{
	if (!zram_debugfs_root)
		return;

	zram->debugfs_dir = debugfs_create_dir(zram->disk->disk_name,
						zram_debugfs_root);
	debugfs_create_file("block_state", 0400, zram->debugfs_dir,
				zram, &proc_zram_block_state_op);
}

static void zram_debugfs_unregister(struct zram *zram)
{
	debugfs_remove_recursive(zram->debugfs_dir);
}
#else
static void zram_debugfs_create(void) {};
static void zram_debugfs_destroy(void) {};
static void zram_debugfs_register(struct zram *zram) {};
static void zram_debugfs_unregister(struct zram *zram) {};
#endif

/*
 * We switched to per-cpu streams and this attr is not needed anymore.
 * However, we will keep it around for some time, because:
 * a) we may revert per-cpu streams in the future
 * b) it's visible to user space and we need to follow our 2 years
 *    retirement rule; but we already have a number of 'soon to be
 *    altered' attrs, so max_comp_streams need to wait for the next
 *    layoff cycle.
 */
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

static void comp_algorithm_set(struct zram *zram, const char *alg)
{
	if (zram->comp_alg && zram->comp_alg != default_compressor)
		kfree(zram->comp_alg);
	zram->comp_alg = alg;
}

static ssize_t comp_algorithm_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t sz;

	down_read(&zram->init_lock);
	sz = zcomp_available_show(zram->comp_alg, buf);
	up_read(&zram->init_lock);

	return sz;
}

static ssize_t comp_algorithm_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf,
				    size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	char *compressor;
	size_t sz;

	sz = strlen(buf);
	if (sz >= CRYPTO_MAX_ALG_NAME)
		return -E2BIG;

	compressor = kstrdup(buf, GFP_KERNEL);
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

	comp_algorithm_set(zram, compressor);
	up_write(&zram->init_lock);
	return len;
}

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
			/* 用户态读取使用 read 获取当前 CPU 值，避免遍历所有 CPU */
			(u64)percpu_counter_read(&zram->stats.notify_free));
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

	orig_size = percpu_counter_read(&zram->stats.pages_stored);
	max_used = atomic_long_read(&zram->stats.max_used_pages);

	ret = scnprintf(buf, PAGE_SIZE,
			"%8llu %8llu %8llu %8lu %8ld %8llu %8lu %8llu %8llu\n",
			orig_size << PAGE_SHIFT,
			(u64)percpu_counter_read(&zram->stats.compr_data_size),
			mem_used << PAGE_SHIFT,
			zram->limit_pages << PAGE_SHIFT,
			max_used << PAGE_SHIFT,
			(u64)percpu_counter_read(&zram->stats.same_pages),
			atomic_long_read(&pool_stats.pages_compacted),
			(u64)percpu_counter_read(&zram->stats.huge_pages),
			(u64)percpu_counter_read(&zram->stats.huge_pages_since));
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
			FOUR_K((u64)percpu_counter_read(&zram->stats.bd_count)),
			FOUR_K((u64)percpu_counter_read(&zram->stats.bd_reads)),
			FOUR_K((u64)percpu_counter_read(&zram->stats.bd_writes)));
	up_read(&zram->init_lock);

	return ret;
}

static ssize_t writeback_skip_pages_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t ret;

	down_read(&zram->init_lock);
	ret = scnprintf(buf, PAGE_SIZE, "%llu\n",
			(u64)atomic64_read(&zram->stats.wb_pages_skipped));
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
static DEVICE_ATTR_RO(writeback_skip_pages);
#endif
static DEVICE_ATTR_RO(debug_stat);

static void zram_meta_free(struct zram *zram, u64 disksize)
{
	size_t num_pages = disksize >> PAGE_SHIFT;
	size_t index;

	if (!zram->table)
		return;

	/* Free all pages that are still in this zram device */
	for (index = 0; index < num_pages; index++)
		zram_free_page(zram, index);

	zs_destroy_pool(zram->mem_pool);

	if (zram->io_page_pool) {
		mempool_destroy(zram->io_page_pool);
		zram->io_page_pool = NULL;
	}

#ifdef CONFIG_ZRAM_WRITEBACK
	/* Destroy the per-device idle LRU and cold writeback metadata. */
	list_lru_destroy(&zram->zram_list_lru);

	if (zram->active_pagevecs) {
		zram_drain_all_pages(zram);
		free_percpu(zram->active_pagevecs);
		zram->active_pagevecs = NULL;
	}
	vfree(zram->wb_table);
	zram->wb_table = NULL;
#endif

#ifdef CONFIG_ZRAM_TRACK_ENTRY_ACTIME
	vfree(zram->ac_time_table);
	zram->ac_time_table = NULL;
#endif
	
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

#ifdef CONFIG_ZRAM_TRACK_ENTRY_ACTIME
	zram->ac_time_table = vzalloc(array_size(num_pages,
						 sizeof(*zram->ac_time_table)));
	if (!zram->ac_time_table)
		goto err_table;
#endif

#ifdef CONFIG_ZRAM_WRITEBACK
	zram->wb_table = vzalloc(array_size(num_pages, sizeof(*zram->wb_table)));
	if (!zram->wb_table)
		goto err_actime_table;

	/* Initialize the LRU list heads for each cold writeback slot. */
	for (index = 0; index < num_pages; index++)
		INIT_LIST_HEAD(zram_wb_lru(zram, index));
#endif

	zram->mem_pool = zs_create_pool(zram->disk->disk_name);
	if (!zram->mem_pool)
		goto err_wb_table;

	if (!huge_class_size)
		huge_class_size = zs_huge_class_size(zram->mem_pool);

	zram->io_page_pool = mempool_create_page_pool(64, 0);
	if (!zram->io_page_pool)
		goto err_pool;

	#ifdef CONFIG_ZRAM_WRITEBACK
	/* Initialize the per-device idle LRU */
	if (list_lru_init(&zram->zram_list_lru))
		goto err_io_pool;
	#endif

#ifdef CONFIG_ZRAM_WRITEBACK
	zram->active_pagevecs = alloc_percpu(struct zram_pagevec);
	if (!zram->active_pagevecs)
		goto err_lru;
	
	{
		int cpu;
		for_each_possible_cpu(cpu) {
			struct zram_pagevec *pvec = per_cpu_ptr(zram->active_pagevecs, cpu);
			pvec->nr = 0;
			spin_lock_init(&pvec->lock);
		}
	}
	
	INIT_LIST_HEAD(&zram->active_list);
	spin_lock_init(&zram->active_list_lock);
	atomic_long_set(&zram->active_pages, 0);
#endif
	
	return true;

#ifdef CONFIG_ZRAM_WRITEBACK
err_lru:
	list_lru_destroy(&zram->zram_list_lru);
#endif
err_io_pool:
	mempool_destroy(zram->io_page_pool);
	zram->io_page_pool = NULL;
err_pool:
	zs_destroy_pool(zram->mem_pool);
	zram->mem_pool = NULL;
err_wb_table:

#ifdef CONFIG_ZRAM_WRITEBACK
	vfree(zram->wb_table);
	zram->wb_table = NULL;
err_actime_table:
#endif
#ifdef CONFIG_ZRAM_TRACK_ENTRY_ACTIME
	vfree(zram->ac_time_table);
	zram->ac_time_table = NULL;
#endif
err_table:
	vfree(zram->table);
	zram->table = NULL;
	return false;
}

/*
 * To protect concurrent access to the same index entry,
 * caller should hold this table index entry's bit_spinlock to
 * indicate this index entry is accessing.
 */
void zram_free_page(struct zram *zram, size_t index)
{
	unsigned long handle;
	unsigned long flags;
	unsigned long clear_mask;

#ifdef CONFIG_ZRAM_TRACK_ENTRY_ACTIME
	zram_clear_ac_time(zram, index);
#endif
	/* Remove from LRU list if present */
	if (zram_test_flag(zram, index, ZRAM_IDLE))
		zram_lru_del(zram, &zram->table[index]);

	clear_mask = BIT(ZRAM_IDLE) | BIT(ZRAM_INCOMPRESSIBLE) |
		     BIT(ZRAM_PAGE_ANON) | BIT(ZRAM_PAGE_FILE) |
		     BIT(ZRAM_PAGE_DIRTY) | BIT(ZRAM_WB_SECOND_CHANCE) |
		     BIT(ZRAM_WB_READ_ONCE) |
		     BIT(ZRAM_REFERENCED) | BIT(ZRAM_TEMP_0) |
		     BIT(ZRAM_TEMP_1) | BIT(ZRAM_TEMP_2) |
		     BIT(ZRAM_PP_SLOT);
	zram_clear_flags(zram, index, clear_mask);
	
#ifdef CONFIG_ZRAM_WRITEBACK
	if (zram_test_flag(zram, index, ZRAM_ACTIVE)) {
		spin_lock_irqsave(&zram->active_list_lock, flags);
		if (!list_empty(zram_wb_lru(zram, index))) {
			list_del_init(zram_wb_lru(zram, index));
			atomic_long_dec(&zram->active_pages);
		}
		zram_clear_flag(zram, index, ZRAM_ACTIVE);
		spin_unlock_irqrestore(&zram->active_list_lock, flags);
	}
#endif

	if (zram_test_flag(zram, index, ZRAM_HUGE)) {
		zram_clear_flag(zram, index, ZRAM_HUGE);
		percpu_counter_dec(&zram->stats.huge_pages);
	}

	if (zram_test_flag(zram, index, ZRAM_WB)) {
		handle = zram_get_handle(zram, index);

		zram_clear_flag(zram, index, ZRAM_WB);
		free_block_bdev(zram, handle);
		goto out;
	}

	/*
	 * No memory is allocated for same element filled pages.
	 * Simply clear same page flag.
	 */
	if (zram_test_flag(zram, index, ZRAM_SAME)) {
		zram_clear_flag(zram, index, ZRAM_SAME);
		percpu_counter_dec(&zram->stats.same_pages);
		goto out;
	}

	handle = zram_get_handle(zram, index);
	if (!handle)
		return;

	zs_free(zram->mem_pool, handle);

	percpu_counter_sub(&zram->stats.compr_data_size,
			 zram_get_obj_size(zram, index));
out:
	percpu_counter_dec(&zram->stats.pages_stored);
	zram_set_handle(zram, index, 0);
	zram_set_obj_size(zram, index, 0);
	WARN_ON_ONCE(zram->table[index].flags & ~(1UL << ZRAM_LOCK));
}

static int read_same_filled_page(struct zram *zram, struct page *page,
				 u32 index)
{
	unsigned long element = zram_get_handle(zram, index);
	void *mem = kmap_local_page(page);

	zram_fill_page(mem, PAGE_SIZE, element);
	kunmap_local(mem);
	return 0;
}

static int zram_copy_obj_to_page(struct zram *zram, unsigned long handle,
				 unsigned int size, struct page *page)
{
	void *mem = kmap_local_page(page);
	void *src;

	src = zs_obj_read_begin(zram->mem_pool, handle, mem);
	if (src != mem)
		memcpy(mem, src, size);
	zs_obj_read_end(zram->mem_pool, handle, src);
	kunmap_local(mem);

	return 0;
}

static int zram_prepare_read_from_zspool(struct zram *zram, u32 index,
					 struct page *tmp_page,
					 struct zram_zspool_read_ctx *ctx)
{
	unsigned long handle = zram_get_handle(zram, index);

	if (zram_test_flag(zram, index, ZRAM_SAME) || !handle) {
		ctx->mode = ZRAM_ZS_READ_SAME;
		ctx->element = handle;
		ctx->size = 0;
		return 0;
	}

	if (zram_test_flag(zram, index, ZRAM_HUGE)) {
		ctx->mode = ZRAM_ZS_READ_HUGE;
		ctx->size = PAGE_SIZE;
	} else {
		ctx->mode = ZRAM_ZS_READ_COMPRESSED;
		ctx->size = zram_get_obj_size(zram, index);
	}

	return zram_copy_obj_to_page(zram, handle, ctx->size, tmp_page);
}

static void zram_copy_page_from_page(struct page *dst_page, struct page *src_page)
{
	void *src = kmap_local_page(src_page);
	void *dst = kmap_local_page(dst_page);

	copy_page(dst, src);
	kunmap_local(dst);
	kunmap_local(src);
}

static int zram_decompress_from_page(struct page *dst_page, struct page *src_page,
				     unsigned int size,
				     struct zcomp_strm *zstrm)
{
	void *src = kmap_local_page(src_page);
	void *dst = kmap_local_page(dst_page);
	int ret;

	prefetchw(dst);
	ret = zcomp_decompress(zstrm, src, size, dst);
	kunmap_local(dst);
	kunmap_local(src);

	return ret;
}

static int zram_finish_read_from_zspool(struct page *page, struct page *tmp_page,
					const struct zram_zspool_read_ctx *ctx,
					struct zcomp_strm *zstrm)
{
	switch (ctx->mode) {
	case ZRAM_ZS_READ_SAME:
		{
			void *mem = kmap_local_page(page);

			zram_fill_page(mem, PAGE_SIZE, ctx->element);
			kunmap_local(mem);
		}
		return 0;
	case ZRAM_ZS_READ_HUGE:
		zram_copy_page_from_page(page, tmp_page);
		return 0;
	case ZRAM_ZS_READ_COMPRESSED:
		return zram_decompress_from_page(page, tmp_page, ctx->size, zstrm);
	}

	return -EINVAL;
}

#ifdef CONFIG_ZRAM_WRITEBACK
static void zram_wb_update_batch_stats(struct zram *zram,
				       unsigned int nr_pages,
				       unsigned int run_count)
{
	u32 prev_batch;
	u32 prev_fallback;

	atomic64_add(nr_pages, &zram->stats.wb_read_batch_pages);
	atomic64_add(run_count, &zram->stats.wb_read_batch_bios);
	if (nr_pages <= 1)
		atomic64_inc(&zram->stats.wb_read_batch_fallbacks);

	prev_batch = READ_ONCE(zram->wb_read_batch_ewma);
	WRITE_ONCE(zram->wb_read_batch_ewma,
		   zram_wb_ewma_update(prev_batch, nr_pages));

	prev_fallback = READ_ONCE(zram->wb_read_fallback_ewma);
	WRITE_ONCE(zram->wb_read_fallback_ewma,
		   zram_wb_ewma_update(prev_fallback,
				      (nr_pages <= 1) ? 100 : 0));
}

static int zram_read_from_wb(struct zram *zram, struct bio *bio,
			     struct bvec_iter iter, u32 index,
			     unsigned long handle,
			     struct page **bdev_pages,
			     struct zram_wb_read_run *bdev_runs,
			     unsigned int *processed,
			     unsigned int *bdev_nr_pages)
{
	unsigned int run_count = 0;
	unsigned int nr_pages;
	u8 policy = zram_pick_wb_read_policy(zram);
	int ret;

	nr_pages = zram_collect_bdev_read_runs(zram, bio, iter, handle,
					      bdev_pages, bdev_runs,
					      ZRAM_BDEV_READ_RUN_MAX,
					      &run_count, processed,
					      policy,
					      zram->wb_read_gap_pages);
	if (!nr_pages) {
		struct bio_vec bv = bio_iter_iovec(bio, iter);

		bdev_pages[0] = bv.bv_page;
		bdev_runs[0].start_handle = handle;
		bdev_runs[0].page_start = 0;
		bdev_runs[0].nr_pages = 1;
		run_count = 1;
		nr_pages = 1;
		*processed = PAGE_SIZE;
	}

	ret = read_from_bdev_runs(zram, bdev_pages, bdev_runs, run_count, bio);
	if (!ret) {
		*bdev_nr_pages = nr_pages;
		zram_wb_update_batch_stats(zram, nr_pages, run_count);
	}

	return ret;
}
#endif

static int zram_read_page(struct zram *zram, struct page *page, u32 index,
			  struct bio *parent)
{
	struct page *tmp_page = NULL;
	struct zram_zspool_read_ctx read_ctx;
	struct zcomp_strm *zstrm = NULL;
	bool from_wb = false;
	int ret;

retry:

	zram_slot_lock(zram, index);
	if (!zram_test_flag(zram, index, ZRAM_WB)) {
		if (!tmp_page &&
		    !zram_test_flag(zram, index, ZRAM_SAME) &&
		    zram_get_handle(zram, index)) {
			zram_slot_unlock(zram, index);
			tmp_page = mempool_alloc(zram->io_page_pool,
						 GFP_NOIO | __GFP_NOWARN);
			if (!tmp_page)
				return -ENOMEM;
			goto retry;
		}

		ret = zram_prepare_read_from_zspool(zram, index, tmp_page, &read_ctx);
		zram_slot_unlock(zram, index);

		if (!ret) {
			if (read_ctx.mode == ZRAM_ZS_READ_COMPRESSED)
				zstrm = zcomp_stream_get(zram->comp);

			ret = zram_finish_read_from_zspool(page, tmp_page,
							   &read_ctx, zstrm);

			if (zstrm)
				zcomp_stream_put(zram->comp);
		}
	} else {
		unsigned long handle = zram_get_handle(zram, index);

		zram_slot_unlock(zram, index);
		from_wb = true;
		ret = read_from_bdev(zram, page, handle, parent);
	}

	if (tmp_page)
		mempool_free(tmp_page, zram->io_page_pool);

	/* Should NEVER happen. Return bio error if it does. */
	if (WARN_ON(ret < 0))
		pr_err("Decompression failed! err=%d, page=%u\n", ret, index);

#ifdef CONFIG_ZRAM_WRITEBACK
	if (likely(!ret))
		zram_promote_accessed(zram, index, from_wb);
#endif

	return ret;
}

/*
 * Use a temporary buffer to decompress the page, as the decompressor
 * always expects a full page for the output.
 */
static int zram_bvec_read_partial(struct zram *zram, struct bio_vec *bvec,
				  u32 index, int offset)
{
	struct page *page;
	int ret;

	page = mempool_alloc(zram->io_page_pool, GFP_NOIO | __GFP_NOWARN);
	if (!page)
		return -ENOMEM;
	ret = zram_read_page(zram, page, index, NULL);
	if (likely(!ret))
		memcpy_to_bvec(bvec, page_address(page) + offset);
	mempool_free(page, zram->io_page_pool);
	return ret;
}


static int write_same_filled_page(struct zram *zram, unsigned long fill,
				  u32 index, unsigned long page_flags)
{
	zram_commit_write(zram, index, fill, 0, ZRAM_SAME, page_flags);

	return 0;
}

static int write_incompressible_page(struct zram *zram, const void *src,
				     u32 index, unsigned long page_flags)
{
	unsigned long handle;
	void *dst;

	handle = zs_malloc(zram->mem_pool, PAGE_SIZE,
			   GFP_NOWAIT | __GFP_HIGHMEM | __GFP_MOVABLE);
	if (IS_ERR_VALUE(handle))
		return PTR_ERR((void *)handle);

	if (!zram_can_store_page(zram)) {
		zs_free(zram->mem_pool, handle);
		return -ENOMEM;
	}

	dst = zs_map_object(zram->mem_pool, handle, ZS_MM_WO);
	copy_page(dst, src);
	zs_unmap_object(zram->mem_pool, handle);

	zram_commit_write(zram, index, handle, PAGE_SIZE, ZRAM_HUGE,
			 page_flags | BIT(ZRAM_INCOMPRESSIBLE));

	return 0;
}

static int zram_write_page(struct zram *zram, struct page *page, u32 index)
{
	int ret = 0;
	unsigned long handle;
	unsigned int comp_len;
	void *dst, *mem;
	struct zcomp_strm *zstrm;
	unsigned long element;
	unsigned long page_flags;
	bool same_filled;

	page_flags = zram_page_flags(page);
	mem = kmap_local_page(page);
	same_filled = page_same_filled(mem, &element);
	if (same_filled)
		goto write_same;

	zstrm = zcomp_stream_get(zram->comp);
	ret = zcomp_compress(zstrm, mem, &comp_len);

	if (unlikely(ret)) {
		kunmap_local(mem);
		pr_err("Compression failed! err=%d\n", ret);
		zcomp_stream_put(zram->comp);
		return ret;
	}

	if (comp_len >= huge_class_size) {
		zcomp_stream_put(zram->comp);
		ret = write_incompressible_page(zram, mem, index, page_flags);
		kunmap_local(mem);
		return ret;
	}
	kunmap_local(mem);

	handle = zs_malloc(zram->mem_pool, comp_len,
			__GFP_KSWAPD_RECLAIM |
			__GFP_NOWARN |
			__GFP_HIGHMEM |
			__GFP_MOVABLE |
			__GFP_CMA);
	if (IS_ERR_VALUE(handle)) {
		zcomp_stream_put(zram->comp);
		return PTR_ERR((void *)handle);
	}

	if (!zram_can_store_page(zram)) {
		zs_free(zram->mem_pool, handle);
		zcomp_stream_put(zram->comp);
		return -ENOMEM;
	}

	dst = zs_map_object(zram->mem_pool, handle, ZS_MM_WO);
	memcpy(dst, zstrm->buffer, comp_len);
	zs_unmap_object(zram->mem_pool, handle);
	zcomp_stream_put(zram->comp);

	zram_commit_write(zram, index, handle, comp_len, 0, page_flags);

	return 0;

write_same:
	kunmap_local(mem);
	return write_same_filled_page(zram, element, index, page_flags);
}

/*
 * This is a partial IO. Read the full page before writing the changes.
 */
static int zram_bvec_write_partial(struct zram *zram, struct bio_vec *bvec,
				   u32 index, int offset, struct bio *bio)
{
	struct page *page;
	int ret;

	page = mempool_alloc(zram->io_page_pool, GFP_NOIO | __GFP_NOWARN);
	if (!page)
		return -ENOMEM;

	ret = zram_read_page(zram, page, index, bio);
	if (!ret) {
		memcpy_from_bvec(page_address(page) + offset, bvec);
		ret = zram_write_page(zram, page, index);
	}
	mempool_free(page, zram->io_page_pool);
	return ret;
}

static int zram_bvec_write(struct zram *zram, struct bio_vec *bvec,
			   u32 index, int offset, struct bio *bio)
{
	if (is_partial_io(bvec))
		return zram_bvec_write_partial(zram, bvec, index, offset, bio);
	return zram_write_page(zram, bvec->bv_page, index);
}

static void zram_bio_discard(struct zram *zram, struct bio *bio)
{
	size_t n = bio->bi_iter.bi_size;
	u32 index = bio->bi_iter.bi_sector >> SECTORS_PER_PAGE_SHIFT;
	u32 offset = (bio->bi_iter.bi_sector & (SECTORS_PER_PAGE - 1)) <<
			SECTOR_SHIFT;
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
		percpu_counter_inc(&zram->stats.notify_free);
		index++;
		n -= PAGE_SIZE;
	}

	bio_endio(bio);
}

static void zram_bio_read(struct zram *zram, struct bio *bio)
{
	unsigned long start_time = bio_start_io_acct(bio);
	struct bvec_iter iter = bio->bi_iter;
	struct page *tmp_page = NULL;
	struct zcomp_strm *zstrm = NULL;
	struct page *bdev_pages[ZRAM_BDEV_READ_BATCH_MAX];
	struct zram_wb_read_run bdev_runs[ZRAM_BDEV_READ_RUN_MAX];
	int batch_count = 0;
	bool from_bdev;

	do {
		u32 index = iter.bi_sector >> SECTORS_PER_PAGE_SHIFT;
		u32 offset = (iter.bi_sector & (SECTORS_PER_PAGE - 1)) <<
				SECTOR_SHIFT;
		struct bio_vec bv = bio_iter_iovec(bio, iter);
		unsigned int bdev_nr_pages = 0;
		unsigned int processed = 0;
		int ret;

		bv.bv_len = min_t(u32, bv.bv_len, PAGE_SIZE - offset);
		processed = bv.bv_len;
		from_bdev = false;

		if (is_partial_io(&bv)) {
			if (zstrm) {
				zcomp_stream_put(zram->comp);
				zstrm = NULL;
			}
			ret = zram_bvec_read_partial(zram, &bv, index, offset);
			goto check_err;
		}

		/* Prefetch next table entry for sequential reads */
		if (iter.bi_size > bv.bv_len) {
			u32 next_idx = (iter.bi_sector +
				(bv.bv_len >> SECTOR_SHIFT)) >>
				SECTORS_PER_PAGE_SHIFT;
			prefetch(&zram->table[next_idx]);
		}

		{
			unsigned long flags = READ_ONCE(zram->table[index].flags);

			if ((flags & BIT(ZRAM_SAME)) &&
			    !(flags & (BIT(ZRAM_IDLE) | BIT(ZRAM_PP_SLOT)))) {
				ret = read_same_filled_page(zram, bv.bv_page, index);
				goto check_err;
			}

		#ifdef CONFIG_ZRAM_WRITEBACK
			if ((flags & BIT(ZRAM_WB)) &&
			    !(flags & (BIT(ZRAM_PP_SLOT) | BIT(ZRAM_LOCK)))) {
				unsigned long handle = READ_ONCE(zram->table[index].handle);

				if (zstrm) {
					zcomp_stream_put(zram->comp);
					zstrm = NULL;
					batch_count = 0;
				}
				from_bdev = true;
				ret = zram_read_from_wb(zram, bio, iter, index, handle,
						bdev_pages, bdev_runs,
						&processed, &bdev_nr_pages);
				goto check_err;
			}
		#endif
		}

		/* Prefetch output page for decompression write */
		prefetchw(page_address(bv.bv_page));

	retry:
		zram_slot_lock(zram, index);

		if (zram_test_flag(zram, index, ZRAM_WB)) {
		#ifdef CONFIG_ZRAM_WRITEBACK
			unsigned long handle = zram_get_handle(zram, index);

			zram_slot_unlock(zram, index);
			if (zstrm) {
				zcomp_stream_put(zram->comp);
				zstrm = NULL;
				batch_count = 0;
			}
			from_bdev = true;
			ret = zram_read_from_wb(zram, bio, iter, index, handle,
						bdev_pages, bdev_runs,
						&processed, &bdev_nr_pages);
			goto check_err;
		#else
			zram_slot_unlock(zram, index);
			ret = -EIO;
			goto check_err;
		#endif
		} else {
			struct zram_zspool_read_ctx read_ctx;

			if (!tmp_page &&
			    !zram_test_flag(zram, index, ZRAM_SAME) &&
			    zram_get_handle(zram, index)) {
				zram_slot_unlock(zram, index);
				tmp_page = mempool_alloc(zram->io_page_pool,
							 GFP_NOIO | __GFP_NOWARN);
				if (!tmp_page) {
					ret = -ENOMEM;
					goto check_err;
				}
				goto retry;
			}

			ret = zram_prepare_read_from_zspool(zram, index, tmp_page,
							   &read_ctx);
			zram_slot_unlock(zram, index);

			if (unlikely(ret))
				goto check_err;

			if (read_ctx.mode == ZRAM_ZS_READ_COMPRESSED) {
				if (!zstrm) {
					zstrm = zcomp_stream_get(zram->comp);
					batch_count = 0;
				}
				ret = zram_finish_read_from_zspool(bv.bv_page, tmp_page,
								   &read_ctx, zstrm);

				if (++batch_count >= ZRAM_READ_BATCH_MAX) {
					zcomp_stream_put(zram->comp);
					zstrm = NULL;
				}
			} else {
				ret = zram_finish_read_from_zspool(bv.bv_page, tmp_page,
								   &read_ctx, NULL);
			}
		}

		if (WARN_ON(ret < 0))
			pr_err("Decompression failed! err=%d, page=%u\n",
			       ret, index);

#ifdef CONFIG_ZRAM_WRITEBACK
		if (likely(!ret)) {
			unsigned int active_pages = bdev_nr_pages ?: 1;
			unsigned int active_idx;

			for (active_idx = 0; active_idx < active_pages; active_idx++)
				zram_promote_accessed(zram, index + active_idx,
						      from_bdev);
		}
#endif

check_err:
		if (ret < 0) {
			atomic64_inc(&zram->stats.failed_reads);
			bio->bi_status = BLK_STS_IOERR;
			break;
		}
		if (!from_bdev)
			flush_dcache_page(bv.bv_page);

		if (!from_bdev) {
#ifdef CONFIG_ZRAM_TRACK_ENTRY_ACTIME
			zram_write_ac_time(zram, index, ktime_get_boottime());
#endif
		}

		bio_advance_iter(bio, &iter, processed);
	} while (iter.bi_size);

	if (zstrm)
		zcomp_stream_put(zram->comp);
	if (tmp_page)
		mempool_free(tmp_page, zram->io_page_pool);

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

#ifdef CONFIG_ZRAM_TRACK_ENTRY_ACTIME
		/* 无锁更新时间，原子性由 CPU 保证（或可接受微小误差） */
		zram_write_ac_time(zram, index, ktime_get_boottime());
#endif

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
	struct blk_plug plug;

	blk_start_plug(&plug);

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

	blk_finish_plug(&plug);
}

static void zram_slot_free_notify(struct block_device *bdev,
				unsigned long index)
{
	struct zram *zram;

	zram = bdev->bd_disk->private_data;

	percpu_counter_inc(&zram->stats.notify_free);
	if (!zram_slot_trylock(zram, index)) {
		atomic64_inc(&zram->stats.miss_free);
		return;
	}

	zram_free_page(zram, index);
	zram_slot_unlock(zram, index);
}

static void zram_destroy_comp(struct zram *zram)
{
	if (zram->comp) {
		zcomp_destroy(zram->comp);
		zram->comp = NULL;
	}
	if (zram->comp_alg && zram->comp_alg != default_compressor)
		kfree(zram->comp_alg);
	zram->comp_alg = NULL;
}

/* 前向声明：percpu 计数器管理函数 */
static int zram_stats_init(struct zram *zram);
static void zram_stats_destroy(struct zram *zram);

static void zram_reset_device(struct zram *zram)
{
	down_write(&zram->init_lock);

	zram->limit_pages = 0;

	set_capacity_and_notify(zram->disk, 0);
	part_stat_set_all(zram->disk->part0, 0);

	/* I/O operation under all of CPU are done so let's free */
	zram_meta_free(zram, zram->disksize);
	zram->disksize = 0;
	zram_destroy_comp(zram);

	/* Reset percpu counters - destroy and re-init */
	zram_stats_destroy(zram);
	zram_stats_init(zram);

	/* Reset remaining atomic stats */
	atomic64_set(&zram->stats.failed_reads, 0);
	atomic64_set(&zram->stats.failed_writes, 0);
	atomic_long_set(&zram->stats.max_used_pages, 0);
	atomic64_set(&zram->stats.writestall, 0);
	atomic64_set(&zram->stats.miss_free, 0);
#ifdef CONFIG_ZRAM_WRITEBACK
	atomic64_set(&zram->stats.wb_pages_skipped, 0);
	atomic64_set(&zram->stats.wb_read_batch_pages, 0);
	atomic64_set(&zram->stats.wb_read_batch_bios, 0);
	atomic64_set(&zram->stats.wb_read_batch_fallbacks, 0);
	WRITE_ONCE(zram->wb_read_batch_ewma, 0);
	WRITE_ONCE(zram->wb_read_fallback_ewma, 0);
	WRITE_ONCE(zram->current_shrinker_window_ms,
		   READ_ONCE(sysctl_zram_shrinker_active_window_ms));
	WARN_ON_ONCE(atomic_read(&zram->pp_in_progress));
	WARN_ON_ONCE(atomic_read(&zram->wb_inflight));
#endif

	reset_bdev(zram);

	comp_algorithm_set(zram, default_compressor);
	up_write(&zram->init_lock);
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

#ifdef CONFIG_ZRAM_AUTO_SIZE
	if (sysfs_streq(buf, "auto")) {
        u64 total_mem = (u64)totalram_pages() << PAGE_SHIFT; // 总物理内存
        unsigned int num_cores = num_online_cpus(); // 在线 CPU 核心数
        u64 base_ratio;
		u64 target_size;
        u64 combined_pressure_factor_percent;

        // 1. 优化 base_ratio 的计算：确保 8 核时达到 100%
        // 每个核心贡献 13%，上限 100%
        base_ratio = min_t(u64, num_cores * 13ULL, 100ULL); 
        
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
	        target_size = roundup(target_size, 1ULL << 30);

        // 4. 调整 clamp 范围：允许 Zram 大小超过物理内存
        // 最小 Zram 1GB，或总内存的 1/8 （取两者最大值）
        u64 min_allowed_size = max_t(u64, 1ULL * 1024 * 1024 * 1024ULL, div64_ul(total_mem, 8ULL)); 
        // 最大 Zram 可以是 64GB，或总内存的 2 倍（取两者最小值）
        u64 max_allowed_size = min_t(u64, 64ULL * 1024 * 1024 * 1024ULL, total_mem * 2ULL); 
        
        // 确保最小不会超过最大
        if (min_allowed_size > max_allowed_size) {
            min_allowed_size = max_allowed_size; 
        }

        target_size = clamp(target_size, min_allowed_size, max_allowed_size);
        


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

	if (zram->comp_alg) {
		comp = zcomp_create(zram->comp_alg);
		if (IS_ERR(comp)) {
			pr_err("Cannot initialise %s compressing backend\n",
			       zram->comp_alg);
			err = PTR_ERR(comp);
			goto out_free_comp;
		}
		zram->comp = comp;
	}
	zram->disksize = disksize;
	set_capacity_and_notify(zram->disk, zram->disksize >> SECTOR_SHIFT);
	up_write(&zram->init_lock);

	return len;

out_free_comp:
	zram_destroy_comp(zram);
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

	/* Freeze front-end I/O and drain async writeback before reset. */
	zram_quiesce_device(zram);
	zram_reset_device(zram);
	zram_unquiesce_device(zram);

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
static void zram_free_disk(struct gendisk *disk)
{
	struct zram *zram = disk->private_data;
	bioset_exit(&zram->zram_bio_set);
	zram_stats_destroy(zram);
	kfree(zram);
}

static const struct block_device_operations zram_devops = {
	.open = zram_open,
	.submit_bio = zram_submit_bio,
	.swap_slot_free_notify = zram_slot_free_notify,
	.free_disk = zram_free_disk,
	.owner = THIS_MODULE
};

#ifdef CONFIG_ZRAM_AUTO_SIZE
static ssize_t pressure_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
    unsigned int mem_pressure, zram_pressure;
    
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

    // 参数合法性检查
    if (!buf || len == 0) {
        pr_err("zram: invalid buffer parameters\n");
        return -EINVAL;
    }
    
    if (!zram) {
        pr_err("zram: zram device data is NULL\n");
        return -ENODEV;
    }

	/* 分配本地缓冲区，避免修改原始buf */
	local_buf = kmemdup_nul(buf, len, GFP_KERNEL);
    if (!local_buf) {
        pr_err("zram: memory allocation failed\n");
        return -ENOMEM;
    }

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
	&dev_attr_writeback_skip_pages.attr,
#endif
	&dev_attr_io_stat.attr,
	&dev_attr_mm_stat.attr,
#ifdef CONFIG_ZRAM_WRITEBACK
	&dev_attr_bd_stat.attr,
#endif
	&dev_attr_debug_stat.attr,
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
static int zram_stats_init(struct zram *zram)
{
	int ret;

	ret = percpu_counter_init(&zram->stats.compr_data_size, 0, GFP_KERNEL);
	if (ret)
		return ret;
	ret = percpu_counter_init(&zram->stats.notify_free, 0, GFP_KERNEL);
	if (ret)
		goto err_notify_free;
	ret = percpu_counter_init(&zram->stats.same_pages, 0, GFP_KERNEL);
	if (ret)
		goto err_same_pages;
	ret = percpu_counter_init(&zram->stats.huge_pages, 0, GFP_KERNEL);
	if (ret)
		goto err_huge_pages;
	ret = percpu_counter_init(&zram->stats.huge_pages_since, 0, GFP_KERNEL);
	if (ret)
		goto err_huge_pages_since;
	ret = percpu_counter_init(&zram->stats.pages_stored, 0, GFP_KERNEL);
	if (ret)
		goto err_pages_stored;
#ifdef CONFIG_ZRAM_WRITEBACK
	ret = percpu_counter_init(&zram->stats.bd_count, 0, GFP_KERNEL);
	if (ret)
		goto err_bd_count;
	ret = percpu_counter_init(&zram->stats.bd_reads, 0, GFP_KERNEL);
	if (ret)
		goto err_bd_reads;
	ret = percpu_counter_init(&zram->stats.bd_writes, 0, GFP_KERNEL);
	if (ret)
		goto err_bd_writes;
#endif
	return 0;

#ifdef CONFIG_ZRAM_WRITEBACK
err_bd_writes:
	percpu_counter_destroy(&zram->stats.bd_reads);
err_bd_reads:
	percpu_counter_destroy(&zram->stats.bd_count);
err_bd_count:
	percpu_counter_destroy(&zram->stats.pages_stored);
#endif
err_pages_stored:
	percpu_counter_destroy(&zram->stats.huge_pages_since);
err_huge_pages_since:
	percpu_counter_destroy(&zram->stats.huge_pages);
err_huge_pages:
	percpu_counter_destroy(&zram->stats.same_pages);
err_same_pages:
	percpu_counter_destroy(&zram->stats.notify_free);
err_notify_free:
	percpu_counter_destroy(&zram->stats.compr_data_size);
	return ret;
}

static void zram_stats_destroy(struct zram *zram)
{
	percpu_counter_destroy(&zram->stats.compr_data_size);
	percpu_counter_destroy(&zram->stats.notify_free);
	percpu_counter_destroy(&zram->stats.same_pages);
	percpu_counter_destroy(&zram->stats.huge_pages);
	percpu_counter_destroy(&zram->stats.huge_pages_since);
	percpu_counter_destroy(&zram->stats.pages_stored);
#ifdef CONFIG_ZRAM_WRITEBACK
	percpu_counter_destroy(&zram->stats.bd_count);
	percpu_counter_destroy(&zram->stats.bd_reads);
	percpu_counter_destroy(&zram->stats.bd_writes);
#endif
}

static int zram_add(void)
{
	struct zram *zram;
	int ret, device_id;
	unsigned long total_mem = (u64)totalram_pages() << PAGE_SHIFT; // 总物理内存
	u64 default_disksize = roundup((u64)total_mem, 1ULL << 30);

	zram = kzalloc(sizeof(struct zram), GFP_KERNEL);
	if (!zram)
		return -ENOMEM;

	ret = zram_stats_init(zram);
	if (ret)
		goto out_free_dev;

	if (bioset_init(&zram->zram_bio_set, 32, 0, BIOSET_NEED_BVECS)) {
		ret = -ENOMEM;
		goto out_free_stats;
	}

	ret = idr_alloc(&zram_index_idr, zram, 0, 0, GFP_KERNEL);
	if (ret < 0)
		goto out_free_bioset;
	device_id = ret;

	init_rwsem(&zram->init_lock);
#ifdef CONFIG_ZRAM_WRITEBACK
	spin_lock_init(&zram->wb_limit_lock);
	spin_lock_init(&zram->bitmap_lock);
	atomic_set(&zram->wb_inflight, 0);
	atomic_set(&zram->quiescing, 0);
	init_waitqueue_head(&zram->wb_done_wait);
	init_waitqueue_head(&zram->pp_done_wait);
	zram->wb_read_policy = clamp_t(u8, wb_read_policy,
				      ZRAM_WB_READ_POLICY_STRICT,
				      ZRAM_WB_READ_POLICY_ADAPTIVE);
	zram->wb_read_gap_pages = clamp_t(u8, wb_read_gap_pages, 0, 16);
	zram->wb_frag_mode = clamp_t(u8, wb_frag_mode,
				     ZRAM_WB_FRAG_MODE_OFF,
				     ZRAM_WB_FRAG_MODE_ON);
	zram->wb_read_batch_ewma = 0;
	zram->wb_read_fallback_ewma = 0;
	zram->current_shrinker_window_ms =
		READ_ONCE(sysctl_zram_shrinker_active_window_ms);
	atomic64_set(&zram->stats.wb_read_batch_pages, 0);
	atomic64_set(&zram->stats.wb_read_batch_bios, 0);
	atomic64_set(&zram->stats.wb_read_batch_fallbacks, 0);
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

	comp_algorithm_set(zram, default_compressor);

	set_capacity(zram->disk, 0);

	down_write(&zram->init_lock);
	if (!zram_meta_alloc(zram, default_disksize)) {
		up_write(&zram->init_lock);
		ret = -ENOMEM;
		goto out_cleanup_disk;
	}

	if (zram->comp_alg) {
		struct zcomp *comp = zcomp_create(zram->comp_alg);
		if (IS_ERR(comp)) {
			pr_err("Cannot initialise %s compressing backend\n",
				zram->comp_alg);
			zram_destroy_comp(zram);
			zram_meta_free(zram, default_disksize);
			up_write(&zram->init_lock);
			ret = PTR_ERR(comp);
			goto out_cleanup_disk;
		}
		zram->comp = comp;
	}

	zram->disksize = default_disksize;
	set_capacity_and_notify(zram->disk, zram->disksize >> SECTOR_SHIFT);
#ifdef CONFIG_ZRAM_WRITEBACK
	if (zram_wb_resources_init(zram)) {
		zram_destroy_comp(zram);
		zram_meta_free(zram, default_disksize);
		up_write(&zram->init_lock);
		goto out_cleanup_disk;
	}
#endif
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
#ifdef CONFIG_ZRAM_WRITEBACK
	zram_wb_resources_free(zram);
#endif
	if (zram->disk) {
		put_disk(zram->disk);

	}
	idr_remove(&zram_index_idr, device_id);
	return ret;

out_free_idr:
	idr_remove(&zram_index_idr, device_id);
out_free_bioset:
	bioset_exit(&zram->zram_bio_set);
out_free_stats:
	zram_stats_destroy(zram);
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
		/* Drain front-end I/O and async writeback before teardown. */
		zram_quiesce_device(zram);
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
	zram_begin_quiesce(zram);
	zram_wait_for_pp_idle(zram);
	zram_wb_wait_for_idle(zram);
	zram_reset_device(zram);

	// 释放zram_shrinker和相关资源
#ifdef CONFIG_ZRAM_WRITEBACK
	zram_wb_resources_free(zram);
#endif

	put_disk(zram->disk);
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

#ifdef CONFIG_ZRAM_WRITEBACK
/* 主动回写（带超时限制 + 流量限制），返回写回的页面数 */
static int zram_proactive_writeback(struct zram *zram, unsigned long timeout_ms,
				    int max_pages)
{
	u64 nr_pages = zram->disksize >> PAGE_SHIFT;
	struct zram_pp_ctl *ctl;
	int err;
	int pages_to_write = 0;
	int pages_written = 0;

	down_read(&zram->init_lock);
	if (!init_done(zram) || !zram->backing_dev) {
		up_read(&zram->init_lock);
		return 0;
	}

	err = zram_try_begin_pp(zram);
	if (err) {
		up_read(&zram->init_lock);
		return err;
	}

	/* 使用带超时的初始化 */
	ctl = init_pp_ctl_timeout(timeout_ms, GFP_KERNEL);
	if (!ctl) {
		zram_end_pp(zram);
		up_read(&zram->init_lock);
		return -ENOMEM;
	}
	/* 扫描 IDLE 页面，限制最大数量 */
	pages_to_write = scan_slots_for_writeback(zram, IDLE_WRITEBACK, 0,
			nr_pages, ctl, max_pages, GFP_KERNEL);
	
	if (pages_to_write > 0) {
		err = zram_writeback_slots(zram, ctl, false);
		if (err < 0)
			pages_written = err;
		else
			pages_written = pages_to_write;
	}

	pr_info_ratelimited("zram: proactive_wb timeout_ms=%lu max_pages=%d scanned=%d written=%d\n",
			    timeout_ms, max_pages, pages_to_write, pages_written);

	release_pp_ctl(zram, ctl);
	zram_end_pp(zram);
	up_read(&zram->init_lock);

	return pages_written;
}

// 计算zram使用率
static unsigned long __get_zram_usage(struct zram *zram)
{
	u64 pages_stored, total_pages, bd_count;

	if (!init_done(zram))
		return 0;

	pages_stored = percpu_counter_read(&zram->stats.pages_stored);
	bd_count = percpu_counter_read(&zram->stats.bd_count);
	total_pages = zram->disksize >> PAGE_SHIFT;

	if (total_pages == 0)
		return 0;

	if (pages_stored <= bd_count)
		return 0;

	return ((pages_stored - bd_count) * 100) / total_pages;
}

// 计算内存占用率
static int get_memory_usage(void)
{
    struct sysinfo si;
    si_meminfo(&si);

    unsigned long total = si.totalram;
    unsigned long free = si_mem_available();

    if (total == 0)
        return 0;

    return ((total - free) * 100) / total;
}

// 监控线程函数
static int monitor_func(void *data)
{
	struct zram *zram;
	int id;
	ktime_t cutoff_time = 0;
	/* 临时数组用于快照 ID，避免长时间持有全局锁 */
	int zram_ids[32];
	int num_devs;
	int i;

	/* 压力自适应状态追踪 */
	int high_pressure_count = 0;
	int medium_pressure_count = 0;
	bool proactive_writeback_pending = false;
	int pending_writeback_max_pages = 0;
	int proactive_writeback_max_pages = 0;

	while (!kthread_should_stop()) {
		int mem_usage = get_memory_usage();
		unsigned long current_check_interval = CHECK_INTERVAL;
		unsigned int current_idle_threshold_sec = 600;
		unsigned int current_shrinker_window_ms =
			READ_ONCE(sysctl_zram_shrinker_active_window_ms);
		int current_max_scan = 0;
		bool dynamic_adjustment_active = false;
		bool do_proactive_writeback = false;

		/* 检查是否需要执行主动回写 */
		if (proactive_writeback_pending) {
			do_proactive_writeback = true;
			proactive_writeback_pending = false;
			proactive_writeback_max_pages = pending_writeback_max_pages;
			pending_writeback_max_pages = 0;
		}

		/* 压力自适应逻辑 */
		if (mem_usage > 80) {
			current_check_interval = 90 * HZ;
			current_idle_threshold_sec = 180;
			current_max_scan = 600000;
			current_shrinker_window_ms = 10000;
			dynamic_adjustment_active = true;
			medium_pressure_count = 0;

			if (!do_proactive_writeback) {
				high_pressure_count++;
				if (high_pressure_count >= 3) {
					proactive_writeback_pending = true;
					pending_writeback_max_pages = 98304; /* 384MB / 4KB */
				}
			}

		} else if (mem_usage > 73) {
			current_check_interval = 120 * HZ;
			current_idle_threshold_sec = 360;
			current_max_scan = 400000;
			dynamic_adjustment_active = true;

			high_pressure_count = 0;
			if (!do_proactive_writeback) {
				medium_pressure_count++;
				if (medium_pressure_count >= 4) {
					proactive_writeback_pending = true;
					pending_writeback_max_pages = 16384; /* 64MB / 4KB */
				}
			}

		} else {
			high_pressure_count = 0;
			medium_pressure_count = 0;
		}

		if (IS_ENABLED(CONFIG_ZRAM_TRACK_ENTRY_ACTIME))
			cutoff_time = ktime_sub(ktime_get_boottime(), ns_to_ktime(current_idle_threshold_sec * NSEC_PER_SEC));
		else
			cutoff_time = 0;

		/* 息屏/休眠时不执行 mark_idle 与主动回写，与 shrinker 一致，减少低功耗状态下的 I/O */
		if (system_entering_hibernation()) {
			schedule_timeout_interruptible(current_check_interval);
			continue;
		}

		num_devs = 0;
		mutex_lock(&zram_index_mutex);
		idr_for_each_entry(&zram_index_idr, zram, id) {
			if (num_devs < ARRAY_SIZE(zram_ids))
				zram_ids[num_devs++] = id;
		}
		mutex_unlock(&zram_index_mutex);

		for (i = 0; i < num_devs; i++) {
			rcu_read_lock();
			zram = idr_find(&zram_index_idr, zram_ids[i]);
			if (!zram || !zram->disk || !get_device(disk_to_dev(zram->disk))) {
				rcu_read_unlock();
				continue;
			}
			rcu_read_unlock();

				down_read(&zram->init_lock);
				if (init_done(zram) && dynamic_adjustment_active) {
					mark_idle(zram, cutoff_time, current_max_scan);
					WRITE_ONCE(zram->current_shrinker_window_ms,
						   current_shrinker_window_ms);

					atomic_set(&zram->shrinker_in_active_period, 1);
					zram->shrinker_active_start = 0;
				} else if (init_done(zram)) {
					WRITE_ONCE(zram->current_shrinker_window_ms,
						   READ_ONCE(sysctl_zram_shrinker_active_window_ms));
				}
				up_read(&zram->init_lock);
			
			/* 释放磁盘引用 */
			put_device(disk_to_dev(zram->disk));

			/* 处理完一个设备后让出 CPU，防止连续处理占用太多时间 */
			cond_resched();
		}
		
		/* 连续高压力后主动触发写回，不依赖 shrinker 被动调用 */
		if (do_proactive_writeback && num_devs > 0) {
			/* 游戏避让：检查是否在前台游戏运行 */
			if (!check_game_pid()) {
				proactive_writeback_pending = false;
				pending_writeback_max_pages = 0;
				proactive_writeback_max_pages = 0;
				high_pressure_count = 0;
				medium_pressure_count = 0;
			} else {
				for (i = 0; i < num_devs; i++) {
					rcu_read_lock();
					zram = idr_find(&zram_index_idr, zram_ids[i]);
					if (!zram || !zram->disk || !get_device(disk_to_dev(zram->disk))) {
						rcu_read_unlock();
						continue;
					}
					rcu_read_unlock();

					if (zram->backing_dev)
						zram_proactive_writeback(zram, 5000,
									 proactive_writeback_max_pages);

					put_device(disk_to_dev(zram->disk));
					cond_resched();
				}
				high_pressure_count = 0;
				medium_pressure_count = 0;
				proactive_writeback_max_pages = 0;
			}
		}

		schedule_timeout_interruptible(current_check_interval);
	}

	return 0;
}

/* 比较函数：用于排序 */
static int compare_ulong(const void *a, const void *b)
{
    unsigned long ua = *(const unsigned long *)a;
    unsigned long ub = *(const unsigned long *)b;
    if (ua < ub) return -1;
    if (ua > ub) return 1;
    return 0;
}

/*
 * LRU 遍历回调：收集种子
 * 注意：此函数在持有 LRU 锁的情况下运行，必须快速返回
 */
static enum lru_status zram_seed_collect_cb(struct list_head *item, struct list_lru_one *l,
                                            spinlock_t *lock, void *arg)
{
	struct zram_wb_table_entry *entry = container_of(item,
						 struct zram_wb_table_entry, lru);
	struct zram_shrink_work *work = arg;
	struct zram *zram = work->zram;
	unsigned long index = entry - zram->wb_table;
	unsigned long flags = READ_ONCE(zram_table_entry(zram, index)->flags);

	if (work->nr_candidates >= BATCH_SIZE)
        return LRU_STOP;

    if (!zram_allocated(zram, index) || (flags & BIT(ZRAM_WB))) {
        zram_clear_flag_atomic(zram, index, ZRAM_IDLE);
        return LRU_REMOVED;
    }

    if (zram_test_flag_atomic(zram, index, ZRAM_REFERENCED)) {
        zram_clear_flag_atomic(zram, index, ZRAM_REFERENCED);
        return LRU_ROTATE;
    }

	if ((flags & BIT(ZRAM_PP_SLOT)) || (flags & BIT(ZRAM_LOCK))) {
		return LRU_ROTATE;
	}

	/* File-backed pages are filtered from writeback path with separate policy. */
	if (flags & BIT(ZRAM_PAGE_FILE)) {
		atomic64_inc(&zram->stats.wb_pages_skipped);
		zram_clear_flag_atomic(zram, index, ZRAM_IDLE);
		return LRU_REMOVED;
	}

	/* Only anonymous pages are eligible for writeback in shrinker path. */
	if (!(flags & BIT(ZRAM_PAGE_ANON))) {
		atomic64_inc(&zram->stats.wb_pages_skipped);
		return LRU_ROTATE;
	}

    /* ROI filter: skip pages that cost more IO than memory they free */
    if (flags & BIT(ZRAM_SAME)) {
        zram_clear_flag_atomic(zram, index, ZRAM_IDLE);
        return LRU_REMOVED;
    }

    {
        size_t obj_size = zram_get_obj_size(zram, index);

        if (obj_size > 0 && obj_size < ZRAM_WB_MIN_OBJ_SIZE) {
            zram_clear_flag_atomic(zram, index, ZRAM_IDLE);
            return LRU_REMOVED;
        }
    }

    work->candidates[work->nr_candidates++] = index;

    return LRU_ROTATE;
}

/* 
 * 尝试锁定并声明 Slot 所有权 
 * 增加了对 handle 的检查，防止空指针解引用
 */
static bool try_claim_slot(struct zram *zram, unsigned long index)
{
    /* 使用 trylock 防止死锁 */
    if (!zram_slot_trylock(zram, index))
        return false;

    /* 严格检查：已分配且有有效句柄 */
    if (!zram_allocated(zram, index) || !zram_get_handle(zram, index)) {
        zram_slot_unlock(zram, index);
        return false;
    }

    /* 检查是否正在回写或已被其他进程锁定 */
    if (zram_test_flag(zram, index, ZRAM_WB) ||
        zram_test_flag(zram, index, ZRAM_PP_SLOT) ||
        zram_test_flag(zram, index, ZRAM_REFERENCED) ||
        zram_test_flag(zram, index, ZRAM_SAME)) {
        zram_slot_unlock(zram, index);
        return false;
    }

	if (!zram_test_flag(zram, index, ZRAM_PAGE_ANON)) {
		atomic64_inc(&zram->stats.wb_pages_skipped);
		zram_slot_unlock(zram, index);
		return false;
	}

	{
		unsigned int temp = zram_get_temp_locked(zram, index);

		if (temp > 0) {
			zram_set_temp_locked(zram, index, temp - 1);
			atomic64_inc(&zram->stats.wb_pages_skipped);
			zram_slot_unlock(zram, index);
			return false;
		}
	}

	if (!zram_test_flag(zram, index, ZRAM_WB_SECOND_CHANCE)) {
		zram_set_flag(zram, index, ZRAM_WB_SECOND_CHANCE);
		atomic64_inc(&zram->stats.wb_pages_skipped);
		zram_slot_unlock(zram, index);
		return false;
	}
	zram_clear_flag(zram, index, ZRAM_WB_SECOND_CHANCE);

    /* ROI filter: skip tiny objects where IO cost dwarfs memory savings */
    {
        size_t obj_size = zram_get_obj_size(zram, index);

        if (obj_size > 0 && obj_size < ZRAM_WB_MIN_OBJ_SIZE) {
            if (zram_test_flag(zram, index, ZRAM_IDLE)) {
                zram_clear_flag(zram, index, ZRAM_IDLE);
                zram_lru_del(zram, &zram->table[index]);
            }
            zram_slot_unlock(zram, index);
            return false;
        }
    }

    /* 标记为预处理槽位 */
    zram_set_flag(zram, index, ZRAM_PP_SLOT);
    zram_slot_unlock(zram, index);
    return true;
}

/* 回滚操作：清除标记 */
static void rollback_slot(struct zram *zram, unsigned long index)
{
    zram_slot_lock(zram, index);
    zram_clear_flag(zram, index, ZRAM_PP_SLOT);
    zram_slot_unlock(zram, index);
}

/* 
 * 扫描窗口：以 seed_idx 为中心
 * 修正了循环边界溢出问题
 */
static int scan_window(struct zram *zram, unsigned long seed_idx,
		       unsigned long *claimed_list, int max_size)
{
	unsigned long start, end, i;
	unsigned long max_pages = zram->disksize >> PAGE_SHIFT;
	int count = 0;

	if (max_size <= 0 || !max_pages || seed_idx >= max_pages)
		return 0;

	/* 计算边界，注意无符号数下溢 */
	start = (seed_idx > WINDOW_RADIUS) ? seed_idx - WINDOW_RADIUS : 0;
	end = min(seed_idx + WINDOW_RADIUS, max_pages - 1);

	for (i = start; i <= end && count < max_size; i++) {
		unsigned long flags = READ_ONCE(zram->table[i].flags);

		/* 无锁快筛：明显不可能成功的槽位直接跳过，减少 trylock 争用 */
		if (flags & (BIT(ZRAM_WB) | BIT(ZRAM_PP_SLOT) |
					 BIT(ZRAM_REFERENCED) | BIT(ZRAM_SAME)))
			continue;

		if (try_claim_slot(zram, i))
			claimed_list[count++] = i;
	}

	return count;
}

/*
 * 核心扫描逻辑
 */
static unsigned long zram_shrinker_scan(struct shrinker *shrinker, struct shrink_control *sc)
{
    struct zram *zram = shrinker->private_data;
    struct zram_shrink_work *work = NULL;
    int i, k, nr_claimed, err;
    unsigned long pages_scheduled = 0;
    unsigned long last_window_end = 0;

    /* Skip shrinker during hibernation/suspend to reduce I/O in low-power state */
    if (system_entering_hibernation()) {
        sc->nr_scanned = 0;
        return SHRINK_STOP;
    }

	if (!down_read_trylock(&zram->init_lock)) {
		return SHRINK_STOP;
	}

	err = zram_try_begin_pp(zram);
	if (err)
		goto out_stop;

    if (!init_done(zram))
		goto out_end_pp;

    if (__get_zram_usage(zram) > 75)
		goto out_end_pp;

    if (!zram->backing_dev || !gfp_has_io_fs(sc->gfp_mask))
		goto out_end_pp;

    if (atomic_read(&zram->shrinker_in_active_period)) {
        if (zram->shrinker_active_start == 0) {
            zram->shrinker_active_start = jiffies;
        } else {
            unsigned long elapsed = jiffies - zram->shrinker_active_start;
			unsigned long window_jiffies =
				msecs_to_jiffies(READ_ONCE(zram->current_shrinker_window_ms));

            if (elapsed > window_jiffies) {
                atomic_set(&zram->shrinker_in_active_period, 0);
				goto out_end_pp;
            }
        }
    } else {
		goto out_end_pp;
    }

    /* Single allocation to keep reclaim-path stack shallow and avoid extra alloc/free churn. */
    work = kzalloc(sizeof(*work), GFP_NOWAIT | __GFP_NOWARN);
    if (!work)
		goto out_end_pp;

    work->zram = zram;
	work->ctl = init_pp_ctl_timeout(0, GFP_NOWAIT | __GFP_NOWARN);
	if (!work->ctl) {
		goto out_free_work_end_pp;
	}

    list_lru_shrink_walk(&zram->zram_list_lru, sc, zram_seed_collect_cb, work);

    if (work->nr_candidates == 0)
        goto out;

    sort(work->candidates, work->nr_candidates, sizeof(unsigned long), compare_ulong, NULL);

    for (i = 0; i < work->nr_candidates; i++) {
        unsigned long seed = work->candidates[i];
		unsigned long max_pages = zram->disksize >> PAGE_SHIFT;
		unsigned long window_end = min(seed + WINDOW_RADIUS, max_pages - 1);
        
        if (seed <= last_window_end && last_window_end != 0)
            continue;

		nr_claimed = scan_window(zram, seed, work->window_claimed,
					ZRAM_WINDOW_CLAIMED_SIZE);
        
        if (nr_claimed == 0) continue;

		if (nr_claimed >= MIN_AGGREGATE) {
			last_window_end = window_end;

            for (k = 0; k < nr_claimed; k++) {
				if (place_pp_slot(zram, work->ctl,
						  work->window_claimed[k],
						  GFP_NOWAIT | __GFP_NOWARN)) {
                    pages_scheduled++;
                } else {
                    rollback_slot(zram, work->window_claimed[k]);
                }
            }
        } else {
            for (k = 0; k < nr_claimed; k++) {
                unsigned long idx = work->window_claimed[k];
                
                if (idx == seed) {
					if (place_pp_slot(zram, work->ctl, idx,
						  GFP_NOWAIT | __GFP_NOWARN)) {
                        pages_scheduled++;
                    } else {
                        rollback_slot(zram, idx);
                    }
                } else {
                    rollback_slot(zram, idx);
                }
            }
        }
    }

    if (pages_scheduled > 0) {
		zram_writeback_slots(zram, work->ctl, true);
	}

out:
    release_pp_ctl(zram, work->ctl);
    kfree(work);
    zram_end_pp(zram);
    up_read(&zram->init_lock);
    return pages_scheduled;

out_free_work_end_pp:
    kfree(work);
out_end_pp:
    zram_end_pp(zram);
out_stop:
    up_read(&zram->init_lock);
    return SHRINK_STOP;
}

static unsigned long zram_shrinker_count(struct shrinker *shrinker, struct shrink_control *sc)
{
    struct zram *zram = shrinker->private_data;
    unsigned long count = 0;

	if (!down_read_trylock(&zram->init_lock)) {
		return 0;
	}

	if (!init_done(zram)) {
		goto out;
	}

	if (zram_is_quiescing(zram)) {
		goto out;
	}

    if (__get_zram_usage(zram) > 75)
        goto out;

    if (!zram->backing_dev || !gfp_has_io_fs(sc->gfp_mask))
        goto out;

    if (!percpu_counter_sum(&zram->stats.pages_stored))
        goto out;

    if (!zram->zram_list_lru.node)
        goto out;

    if (atomic_read(&zram->shrinker_in_active_period)) {
        if (zram->shrinker_active_start == 0) {
            zram->shrinker_active_start = jiffies;
        } else {
            unsigned long elapsed = jiffies - zram->shrinker_active_start;
			unsigned long window_jiffies =
				msecs_to_jiffies(READ_ONCE(zram->current_shrinker_window_ms));
            if (elapsed > window_jiffies) {
                atomic_set(&zram->shrinker_in_active_period, 0);
            }
        }
    } else {
        goto out;
    }

    count = list_lru_shrink_count(&zram->zram_list_lru, sc);

    if (count > 4096UL) {
        count = 4096UL;
    }

out:
    up_read(&zram->init_lock);
    return count;
}

static void zram_init_shrinker(struct zram *zram)
{
	struct shrinker *shrinker;

	shrinker = shrinker_alloc(SHRINKER_NUMA_AWARE, "mm-zram");
	if (!shrinker)
		return;

	shrinker->count_objects = zram_shrinker_count;
	shrinker->scan_objects = zram_shrinker_scan;
	shrinker->batch = 512;
	shrinker->seeks = 4;
	shrinker->private_data = zram;
	zram->zram_shrinker = shrinker;
	atomic_set(&zram->shrinker_in_active_period, 0);
	zram->shrinker_active_start = 0;

	shrinker_register(zram->zram_shrinker);
}
#endif

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
	monitor_thread = kthread_run(monitor_func, NULL, "zram_monitor");
	zram_sysctl_table_header = register_sysctl("vm", zram_sysctl_table);
#endif

	return 0;

out_error:
	destroy_devices();
	return ret;
}

static void __exit zram_exit(void)
{
#ifdef CONFIG_ZRAM_WRITEBACK
	if (monitor_thread) {
		kthread_stop(monitor_thread);
		monitor_thread = NULL;
	}
	unregister_sysctl_table(zram_sysctl_table_header);
#endif

	destroy_devices();
	destroy_zram_writeback();
}

module_init(zram_init);
module_exit(zram_exit);

module_param(num_devices, uint, 0);
MODULE_PARM_DESC(num_devices, "Number of pre-created zram devices");

#ifdef CONFIG_ZRAM_WRITEBACK
module_param(wb_read_policy, uint, 0644);
MODULE_PARM_DESC(wb_read_policy,
	"writeback read policy: 0=strict, 1=relaxed, 2=adaptive");
module_param(wb_read_gap_pages, uint, 0644);
MODULE_PARM_DESC(wb_read_gap_pages,
	"max tolerated page gaps for relaxed/adaptive wb read batching");
module_param(wb_frag_mode, uint, 0644);
MODULE_PARM_DESC(wb_frag_mode,
	"writeback fragmentation mode (default: on): 0=off, 1=auto, 2=on");
#endif

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Nitin Gupta <ngupta@vflare.org>");
MODULE_AUTHOR("Coolapk@BrokeStar&Github@whitewhale0612");
MODULE_DESCRIPTION("Compressed RAM Block Device");
