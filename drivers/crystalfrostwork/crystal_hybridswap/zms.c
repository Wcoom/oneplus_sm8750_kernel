// SPDX-License-Identifier: GPL-2.0
/*
 * Zsmalloc-inspired volatile backing storage for Crystal zram writeback.
 *
 * ZMS keeps all metadata in RAM and packs compressed zram objects into
 * PAGE_SIZE backing blocks.  The backing device contains only object bytes;
 * losing the in-memory metadata is fine because swap is rebuilt on boot.
 */
#define pr_fmt(fmt) "zms: " fmt

#include <linux/bio.h>
#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/blkdev.h>
#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/percpu_counter.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/vmalloc.h>

#include "zms.h"

#define ZMS_MAX_CLASSES		(PAGE_SIZE / ZMS_CLASS_SIZE)
#define ZMS_BLOCK_RESERVED	1UL
#define ZMS_RECLAIM_BEFORE_ALLOC_MAX	64U
#define ZMS_FLUSH_PINNED_RETRY_MAX	64U
#define ZMS_STAT_COUNTER_BATCH		32
#define ZMS_DIRTY_HARD_MAX_PAGES	(SZ_32M >> PAGE_SHIFT)
#define ZMS_DIRTY_FLUSH_DELAY_MS	20U
#define ZMS_CLEAN_CACHE_MAX_PAGES	(SZ_64M >> PAGE_SHIFT)
#define ZMS_HANDLE_LOCK_BITS		8
#define ZMS_HANDLE_LOCKS		(1U << ZMS_HANDLE_LOCK_BITS)
#define ZMS_PIN_DROP_DATA		(1 << 26)
#define ZMS_PIN_DEFER_FREE		(1 << 27)
#define ZMS_PIN_RELEASING		(1 << 28)
#define ZMS_PIN_FROZEN			(1 << 29)
#define ZMS_PIN_COUNT_MASK		(ZMS_PIN_DROP_DATA - 1)
#define ZMS_PIN_CLEANUP_MASK		(ZMS_PIN_DROP_DATA | ZMS_PIN_DEFER_FREE)
#define ZMS_PIN_BLOCKED_MASK		(ZMS_PIN_CLEANUP_MASK | ZMS_PIN_RELEASING | \
					 ZMS_PIN_FROZEN)

enum zms_handle_flags {
	ZMS_HANDLE_PENDING_FREE = BIT(0),
};

enum zms_fullness {
	ZMS_FG_LOW,
	ZMS_FG_MID,
	ZMS_FG_ALMOST_FULL,
	ZMS_FG_FULL,
	ZMS_NR_FULLNESS,
};

enum zms_stat_counter_id {
	ZMS_STAT_USED_BLOCKS,
	ZMS_STAT_PENDING_FREE,
	ZMS_STAT_OBJECTS,
	ZMS_STAT_STORED_BYTES,
	ZMS_STAT_PACKED_BYTES,
	ZMS_STAT_PARTIAL_BLOCKS,
	ZMS_STAT_LOW_BLOCKS,
	ZMS_STAT_MID_BLOCKS,
	ZMS_STAT_ALMOST_FULL_BLOCKS,
	ZMS_STAT_FULL_BLOCKS,
	ZMS_STAT_DIRTY_BLOCKS,
	ZMS_STAT_DIRTY_PAGES,
	ZMS_NR_STAT_COUNTERS,
};

struct zms_block {
	unsigned int class_size;
	unsigned int pages;
	unsigned int slots;
	unsigned int used;
	enum zms_fullness fullness;
	unsigned long *bitmap;
	unsigned long *slot_handles;
	void *data;
	bool dirty;
	bool bd_stat_dirty;
	bool listed;
	bool reading;
	bool clean_cached;
	bool clean_referenced;
	int read_ret;
	atomic_t pin_state;
	struct list_head list;
	struct list_head clean_node;
	wait_queue_head_t read_wait;
	unsigned long blocks[];
};

struct zms_class {
	unsigned int size;
	unsigned int pages_per_zspage;
	unsigned int slots_per_zspage;
	unsigned int listed_blocks;
	struct list_head fullness[ZMS_NR_FULLNESS];
	struct mutex lock;
};

struct zms_handle_entry {
	struct zms_block *block;
	unsigned int offset;
	unsigned int size;
	unsigned int slot;
	u32 generation;
	u8 flags;
};

struct zms_load_snapshot {
	struct zms_block *block;
	unsigned int offset;
	unsigned int size;
	unsigned int slot;
	u32 generation;
	bool valid;
};

struct zms {
	struct block_device *bdev;
	unsigned long nr_blocks;
	unsigned long nr_handles;
	unsigned long next_block;
	unsigned long *block_bitmap;
	unsigned long *pending_free;
	struct zms_handle_entry *handles;
	struct zms_class classes[ZMS_MAX_CLASSES];
	struct percpu_counter statcounters[ZMS_NR_STAT_COUNTERS];
	atomic_long_t alloc_blocks;
	atomic_long_t alloc_run_successes;
	atomic_long_t alloc_run_failures;
	atomic_long_t reclaim_before_alloc_calls;
	atomic_long_t reclaim_before_alloc_handles;
	atomic_long_t read_merge_waits;
	atomic_long_t read_merge_wakeups;
	atomic_long_t read_merge_hits;
	atomic_long_t read_merge_mismatch;
	atomic_long_t read_merge_failures;
	atomic_long_t load_resident_hits;
	atomic_long_t load_disk_misses;
	atomic_long_t clean_cache_evictions;
	atomic_long_t clean_cache_fullness_drops;
	atomic_long_t clean_cache_demand_hits;
	atomic_long_t store_batch_calls;
	atomic_long_t store_batch_items;
	atomic_long_t store_batch_resident_reuses;
	atomic_long_t store_batch_new_blocks;
	atomic_long_t alloc_run_success_pages;
	atomic_long_t alloc_run_partial_pages;
	atomic_long_t alloc_run_fallback_pages;
	atomic_long_t alloc_run_segments;
	atomic_long_t valid_classes;
	spinlock_t pending_lock;
	spinlock_t alloc_lock;
	spinlock_t clean_lock;
	struct list_head clean_lru;
	unsigned long clean_cache_pages;
	unsigned long clean_cache_max_pages;
	spinlock_t handle_locks[ZMS_HANDLE_LOCKS];
	struct work_struct free_work;
	struct delayed_work flush_work;
	atomic64_t physical_read_pages;
	atomic64_t physical_read_ios;
	atomic64_t physical_read_failed_pages;
	atomic64_t physical_write_pages;
	atomic64_t physical_write_ios;
	atomic64_t physical_write_failed_pages;
	atomic64_t bd_stat_read_pages;
	atomic64_t bd_stat_write_pages;
	zms_account_write_pages_t account_write_pages;
	unsigned long dirty_low_pages;
	unsigned long dirty_high_pages;
	unsigned long dirty_hard_pages;
	bool destroying;
	bool ready;
	bool statcounters_ready;
};

static struct zms_class *zms_class_for_size(struct zms *zms, size_t size)
{
	unsigned int idx;

	if (!size || size > PAGE_SIZE)
		return NULL;

	idx = zms_size_to_class(size);
	if (idx >= ZMS_MAX_CLASSES)
		return NULL;

	return &zms->classes[idx];
}

static sector_t zms_block_sector(unsigned long block)
{
	return block * (PAGE_SIZE >> SECTOR_SHIFT);
}

static unsigned int zms_calculate_zspage_pages(unsigned int class_size)
{
	unsigned int i;
	unsigned int best = 1;
	unsigned int min_waste = UINT_MAX;

	if (is_power_of_2(class_size))
		return 1;

	for (i = 1; i <= ZMS_MAX_PAGES_PER_ZSPAGE; i++) {
		unsigned int waste = (i * PAGE_SIZE) % class_size;

		if (waste < min_waste) {
			min_waste = waste;
			best = i;
		}
	}

	return best;
}

static void zms_init_dirty_watermarks(struct zms *zms)
{
	unsigned long usable = zms->nr_blocks - ZMS_BLOCK_RESERVED;
	unsigned long hard;

	hard = max_t(unsigned long, usable / 16, 1024UL);
	hard = min_t(unsigned long, hard, ZMS_DIRTY_HARD_MAX_PAGES);
	hard = min_t(unsigned long, hard, usable);
	if (!hard)
		hard = 1;

	zms->dirty_hard_pages = hard;
	zms->dirty_high_pages = max_t(unsigned long, hard / 2, 1UL);
	zms->dirty_low_pages = zms->dirty_high_pages / 2;
}

static unsigned int zms_block_bytes(const struct zms_block *block)
{
	return block->pages << PAGE_SHIFT;
}

static spinlock_t *zms_handle_lock(struct zms *zms, unsigned long handle)
{
	return &zms->handle_locks[handle & (ZMS_HANDLE_LOCKS - 1)];
}

static bool zms_handle_active(const struct zms_handle_entry *entry)
{
	return entry->block && !(entry->flags & ZMS_HANDLE_PENDING_FREE);
}

static void *zms_block_data_load(const struct zms_block *block);
static void zms_block_data_publish(struct zms_block *block, void *data);
static void zms_block_data_clear(struct zms *zms, struct zms_block *block);
static void zms_clean_cache_del(struct zms *zms, struct zms_block *block);

static bool zms_block_ref_tryget(struct zms_block *block)
{
	int old;

	old = atomic_read(&block->pin_state);
	for (;;) {
		if (old & ZMS_PIN_BLOCKED_MASK)
			return false;
		if ((old & ZMS_PIN_COUNT_MASK) == ZMS_PIN_COUNT_MASK)
			return false;
		if (atomic_try_cmpxchg(&block->pin_state, &old, old + 1))
			return true;
	}
}

static void zms_block_finish_releasing(struct zms *zms, struct zms_block *block)
{
	int old;

	old = atomic_read(&block->pin_state);
	for (;;) {
		int cleanup = old & ZMS_PIN_CLEANUP_MASK;
		bool schedule_free = cleanup & ZMS_PIN_DEFER_FREE;

		if (WARN_ON_ONCE(!(old & ZMS_PIN_RELEASING)))
			return;
		if (WARN_ON_ONCE(old & (ZMS_PIN_FROZEN | ZMS_PIN_COUNT_MASK)))
			return;

		if (cleanup & ZMS_PIN_DROP_DATA)
			zms_block_data_clear(zms, block);

		if (atomic_try_cmpxchg(&block->pin_state, &old, 0)) {
			wake_up_all(&block->read_wait);
			if (schedule_free)
				schedule_work(&zms->free_work);
			return;
		}
	}
}

static void zms_block_ref_put(struct zms *zms, struct zms_block *block)
{
	int old;

	old = atomic_read(&block->pin_state);
	for (;;) {
		int cleanup = old & ZMS_PIN_CLEANUP_MASK;
		int count = old & ZMS_PIN_COUNT_MASK;
		int new;

		if (WARN_ON_ONCE(!(old & ZMS_PIN_COUNT_MASK)))
			return;
		if (WARN_ON_ONCE(old & (ZMS_PIN_FROZEN | ZMS_PIN_RELEASING)))
			return;

		if (count > 1)
			new = old - 1;
		else if (cleanup)
			new = ZMS_PIN_RELEASING | cleanup;
		else
			new = 0;

		if (atomic_try_cmpxchg(&block->pin_state, &old, new)) {
			if (new & ZMS_PIN_RELEASING)
				zms_block_finish_releasing(zms, block);
			return;
		}
	}
}

static bool zms_block_freeze(struct zms_block *block)
{
	int old = 0;

	return atomic_try_cmpxchg(&block->pin_state, &old, ZMS_PIN_FROZEN);
}

static void zms_block_unfreeze(struct zms *zms, struct zms_block *block)
{
	int old;
	int new;

	old = atomic_read(&block->pin_state);
	for (;;) {
		int cleanup = old & ZMS_PIN_CLEANUP_MASK;

		if (WARN_ON_ONCE(!(old & ZMS_PIN_FROZEN)))
			return;
		if (WARN_ON_ONCE(old & (ZMS_PIN_RELEASING | ZMS_PIN_COUNT_MASK)))
			return;

		new = cleanup ? ZMS_PIN_RELEASING | cleanup : 0;
		if (atomic_try_cmpxchg(&block->pin_state, &old, new))
			break;
	}

	if (new & ZMS_PIN_RELEASING)
		zms_block_finish_releasing(zms, block);
	else
		wake_up_all(&block->read_wait);
}

static bool zms_block_frozen(const struct zms_block *block)
{
	return atomic_read(&block->pin_state) & ZMS_PIN_FROZEN;
}

static bool zms_block_pinned(const struct zms_block *block)
{
	return atomic_read(&block->pin_state) != 0;
}

static void zms_block_request_cleanup(struct zms *zms, struct zms_block *block,
				      int flags)
{
	int old;
	int new;

	if (WARN_ON_ONCE(flags & ~ZMS_PIN_CLEANUP_MASK))
		flags &= ZMS_PIN_CLEANUP_MASK;
	if (WARN_ON_ONCE(!flags))
		return;

	old = atomic_read(&block->pin_state);
	for (;;) {
		if (old & ZMS_PIN_RELEASING) {
			new = old | flags;
		} else if (old & ZMS_PIN_FROZEN) {
			new = old | flags;
		} else if (old & ZMS_PIN_COUNT_MASK) {
			new = old | flags;
		} else {
			new = ZMS_PIN_RELEASING | flags;
		}

		if (atomic_try_cmpxchg(&block->pin_state, &old, new))
			break;
	}

	if (new & ZMS_PIN_RELEASING && !(old & ZMS_PIN_RELEASING))
		zms_block_finish_releasing(zms, block);
}

static void *zms_block_data_load(const struct zms_block *block)
{
	return smp_load_acquire(&block->data);
}

static void zms_block_data_publish(struct zms_block *block, void *data)
{
	smp_store_release(&block->data, data);
}

static void zms_block_data_clear(struct zms *zms, struct zms_block *block)
{
	void *data = zms_block_data_load(block);

	if (!data)
		return;
	zms_clean_cache_del(zms, block);
	zms_block_data_publish(block, NULL);
	kvfree(data);
}

static struct page *zms_data_page(void *addr)
{
	if (is_vmalloc_addr(addr))
		return vmalloc_to_page(addr);

	return virt_to_page(addr);
}

static void zms_io_clear(struct zms_io *io)
{
	if (io)
		memset(io, 0, sizeof(*io));
}

static void zms_stat_add(struct zms *zms, enum zms_stat_counter_id id, s64 delta)
{
	if (!zms->statcounters_ready)
		return;

	percpu_counter_add_batch(&zms->statcounters[id], delta,
				 ZMS_STAT_COUNTER_BATCH);
}

static unsigned long zms_stat_read_positive(struct zms *zms,
					    enum zms_stat_counter_id id)
{
	return (unsigned long)percpu_counter_read_positive(&zms->statcounters[id]);
}

static unsigned long zms_dirty_pages(struct zms *zms)
{
	return zms_stat_read_positive(zms, ZMS_STAT_DIRTY_PAGES);
}

static void zms_fullness_stats_add(struct zms *zms, enum zms_fullness fullness,
				   s64 pages);
static void zms_mark_dirty_locked(struct zms *zms, struct zms_block *block,
				  bool account_bd_stat);
static void zms_clear_dirty_locked(struct zms *zms, struct zms_block *block);
static int zms_flush_to(struct zms *zms, unsigned long target_pages,
			gfp_t gfp, struct zms_io *last_io);
static int zms_read_block(struct zms *zms, struct zms_block *block,
			  gfp_t gfp, struct zms_io *io,
			  bool account_bd_stat);

static bool zms_clean_cache_eligible(const struct zms_block *block)
{
	if (!block || block->dirty || !block->used || !zms_block_data_load(block))
		return false;

	return block->fullness >= ZMS_FG_MID;
}

static void zms_clean_cache_del(struct zms *zms, struct zms_block *block)
{
	unsigned long flags;

	if (!block || !READ_ONCE(block->clean_cached))
		return;

	spin_lock_irqsave(&zms->clean_lock, flags);
	if (READ_ONCE(block->clean_cached)) {
		list_del_init(&block->clean_node);
		WRITE_ONCE(block->clean_cached, false);
		WRITE_ONCE(block->clean_referenced, false);
		if (zms->clean_cache_pages >= block->pages)
			WRITE_ONCE(zms->clean_cache_pages,
				   zms->clean_cache_pages - block->pages);
		else
			WRITE_ONCE(zms->clean_cache_pages, 0);
	}
	spin_unlock_irqrestore(&zms->clean_lock, flags);
}

static bool zms_clean_cache_add_mode(struct zms *zms,
				     struct zms_block *block,
				     bool touch_existing)
{
	unsigned long flags;

	if (!zms_clean_cache_eligible(block))
		return false;

	spin_lock_irqsave(&zms->clean_lock, flags);
	if (!zms_clean_cache_eligible(block)) {
		spin_unlock_irqrestore(&zms->clean_lock, flags);
		return false;
	}
	if (READ_ONCE(block->clean_cached)) {
		if (touch_existing)
			WRITE_ONCE(block->clean_referenced, true);
		spin_unlock_irqrestore(&zms->clean_lock, flags);
		return false;
	}

	WRITE_ONCE(block->clean_cached, true);
	WRITE_ONCE(block->clean_referenced, false);
	list_add(&block->clean_node, &zms->clean_lru);
	WRITE_ONCE(zms->clean_cache_pages,
		   zms->clean_cache_pages + block->pages);
	spin_unlock_irqrestore(&zms->clean_lock, flags);
	return true;
}

static bool zms_clean_cache_add(struct zms *zms, struct zms_block *block)
{
	return zms_clean_cache_add_mode(zms, block, true);
}

static bool zms_clean_cache_add_untouched(struct zms *zms,
					  struct zms_block *block)
{
	return zms_clean_cache_add_mode(zms, block, false);
}

static void zms_clean_cache_touch(struct zms_block *block)
{
	if (!block || !READ_ONCE(block->clean_cached))
		return;

	WRITE_ONCE(block->clean_referenced, true);
}

static void zms_clean_cache_shrink(struct zms *zms, unsigned long max_pages)
{
	unsigned int scanned = 0;

	while (READ_ONCE(zms->clean_cache_pages) > max_pages &&
	       scanned++ < ZMS_FLUSH_PINNED_RETRY_MAX) {
		struct zms_block *block;
		unsigned long flags;

		spin_lock_irqsave(&zms->clean_lock, flags);
		if (list_empty(&zms->clean_lru)) {
			spin_unlock_irqrestore(&zms->clean_lock, flags);
			return;
		}
		block = list_last_entry(&zms->clean_lru, struct zms_block,
					clean_node);
		if (READ_ONCE(block->clean_referenced)) {
			WRITE_ONCE(block->clean_referenced, false);
			list_move(&block->clean_node, &zms->clean_lru);
			spin_unlock_irqrestore(&zms->clean_lock, flags);
			continue;
		}
		if (!zms_block_freeze(block)) {
			list_move(&block->clean_node, &zms->clean_lru);
			spin_unlock_irqrestore(&zms->clean_lock, flags);
			continue;
		}

		list_del_init(&block->clean_node);
		WRITE_ONCE(block->clean_cached, false);
		WRITE_ONCE(block->clean_referenced, false);
		atomic_long_inc(&zms->clean_cache_evictions);
		if (zms->clean_cache_pages >= block->pages)
			WRITE_ONCE(zms->clean_cache_pages,
				   zms->clean_cache_pages - block->pages);
		else
			WRITE_ONCE(zms->clean_cache_pages, 0);
		spin_unlock_irqrestore(&zms->clean_lock, flags);

		if (!block->dirty)
			zms_block_data_clear(zms, block);
		zms_block_unfreeze(zms, block);
	}
}

static void zms_drop_block_data_locked(struct zms *zms, struct zms_block *block)
{
	if (!block || !zms_block_data_load(block))
		return;

	zms_clean_cache_del(zms, block);
	if (zms_block_frozen(block)) {
		zms_block_data_clear(zms, block);
		return;
	}

	zms_block_request_cleanup(zms, block, ZMS_PIN_DROP_DATA);
}

static void zms_release_block_data_locked(struct zms *zms,
					  struct zms_block *block)
{
	if (WARN_ON_ONCE(block->dirty))
		return;

	if (!zms_block_data_load(block))
		return;

	if (zms_clean_cache_eligible(block)) {
		if (zms_clean_cache_add(zms, block))
			zms_clean_cache_shrink(zms, zms->clean_cache_max_pages);
		return;
	}

	zms_drop_block_data_locked(zms, block);
}

static void zms_reconcile_clean_cache_locked(struct zms *zms,
					      struct zms_block *block)
{
	if (WARN_ON_ONCE(block->dirty))
		return;
	if (!zms_block_data_load(block))
		return;

	if (zms_clean_cache_eligible(block)) {
		if (zms_clean_cache_add_untouched(zms, block))
			zms_clean_cache_shrink(zms, zms->clean_cache_max_pages);
		return;
	}

	zms_drop_block_data_locked(zms, block);
}

static void zms_wait_read_done(struct zms_class *class, struct zms_block *block)
{
	DEFINE_WAIT(wait);

	for (;;) {
		prepare_to_wait(&block->read_wait, &wait, TASK_UNINTERRUPTIBLE);
		if (!block->reading)
			break;
		mutex_unlock(&class->lock);
		schedule();
		mutex_lock(&class->lock);
	}
	finish_wait(&block->read_wait, &wait);
}

static void zms_io_accumulate(struct zms_io *dst, const struct zms_io *src)
{
	if (!dst || !src || !src->submitted)
		return;

	dst->submitted = true;
	dst->write = src->write;
	dst->block_index = src->block_index;
	dst->latency_ns = src->latency_ns;
	dst->ret = src->ret;
	dst->read_submitted += src->read_submitted;
	dst->read_ios += src->read_ios;
	dst->read_failed += src->read_failed;
	dst->read_total_latency_ns += src->read_total_latency_ns;
	if (src->read_max_latency_ns > dst->read_max_latency_ns)
		dst->read_max_latency_ns = src->read_max_latency_ns;
	dst->read_last_block = src->read_last_block;
	dst->read_last_latency_ns = src->read_last_latency_ns;
	dst->read_last_ret = src->read_last_ret;
	dst->write_submitted += src->write_submitted;
	dst->write_ios += src->write_ios;
	dst->write_failed += src->write_failed;
	dst->write_total_latency_ns += src->write_total_latency_ns;
	if (src->write_max_latency_ns > dst->write_max_latency_ns)
		dst->write_max_latency_ns = src->write_max_latency_ns;
	dst->write_last_block = src->write_last_block;
	dst->write_last_latency_ns = src->write_last_latency_ns;
	dst->write_last_ret = src->write_last_ret;
}

static void zms_pending_stat_dec_if_set(struct zms *zms, unsigned long handle)
{
	unsigned long flags;

	spin_lock_irqsave(&zms->pending_lock, flags);
	if (handle <= zms->nr_handles &&
	    test_and_clear_bit(handle, zms->pending_free))
		zms_stat_add(zms, ZMS_STAT_PENDING_FREE, -1);
	spin_unlock_irqrestore(&zms->pending_lock, flags);
}

static void zms_handle_clear_pending_flag(struct zms *zms, unsigned long handle)
{
	unsigned long flags;

	if (!handle || handle > zms->nr_handles)
		return;

	spin_lock_irqsave(zms_handle_lock(zms, handle), flags);
	zms->handles[handle].flags &= ~ZMS_HANDLE_PENDING_FREE;
	spin_unlock_irqrestore(zms_handle_lock(zms, handle), flags);
}

static int zms_get_handle_ref(struct zms *zms, unsigned long handle,
			      struct zms_load_snapshot *snapshot)
{
	struct zms_handle_entry *entry;
	unsigned long flags;
	int ret = 0;

	memset(snapshot, 0, sizeof(*snapshot));
	if (!handle || handle > zms->nr_handles)
		return -EINVAL;

	spin_lock_irqsave(zms_handle_lock(zms, handle), flags);
	entry = &zms->handles[handle];
	if (!zms_handle_active(entry)) {
		ret = -ENOENT;
		goto out;
	}
	if (!zms_block_ref_tryget(entry->block)) {
		ret = -EAGAIN;
		goto out;
	}
	snapshot->block = entry->block;
	snapshot->offset = entry->offset;
	snapshot->size = entry->size;
	snapshot->slot = entry->slot;
	snapshot->generation = entry->generation;
	snapshot->valid = true;
out:
	spin_unlock_irqrestore(zms_handle_lock(zms, handle), flags);
	return ret;
}

static bool zms_handle_matches_snapshot(struct zms *zms, unsigned long handle,
					const struct zms_load_snapshot *snapshot)
{
	struct zms_handle_entry *entry;
	unsigned long flags;
	bool match;

	if (!snapshot->valid || !handle || handle > zms->nr_handles)
		return false;

	spin_lock_irqsave(zms_handle_lock(zms, handle), flags);
	entry = &zms->handles[handle];
	match = zms_handle_active(entry) &&
		entry->block == snapshot->block &&
		entry->slot == snapshot->slot &&
		entry->offset == snapshot->offset &&
		entry->size == snapshot->size &&
		entry->generation == snapshot->generation;
	spin_unlock_irqrestore(zms_handle_lock(zms, handle), flags);
	return match;
}

static bool zms_get_slot_cookie(struct zms *zms, unsigned long handle,
				struct zms_block *block, unsigned int slot,
				struct zms_object_cookie *cookie)
{
	struct zms_handle_entry *entry;
	unsigned long flags;
	bool match;

	if (!handle || handle > zms->nr_handles)
		return false;

	spin_lock_irqsave(zms_handle_lock(zms, handle), flags);
	entry = &zms->handles[handle];
	match = zms_handle_active(entry) && entry->block == block &&
		entry->slot == slot;
	if (match) {
		cookie->handle = handle;
		cookie->generation = entry->generation;
	}
	spin_unlock_irqrestore(zms_handle_lock(zms, handle), flags);
	return match;
}

static bool zms_handle_publish_store(struct zms *zms, unsigned long handle,
				     struct zms_block *block,
				     unsigned int offset, unsigned int size,
				     unsigned int slot)
{
	struct zms_handle_entry *entry;
	unsigned long flags;
	bool published = false;

	spin_lock_irqsave(zms_handle_lock(zms, handle), flags);
	entry = &zms->handles[handle];
	if (!entry->block && !(entry->flags & ZMS_HANDLE_PENDING_FREE)) {
		entry->generation++;
		entry->block = block;
		entry->offset = offset;
		entry->size = size;
		entry->slot = slot;
		entry->flags = 0;
		published = true;
	}
	spin_unlock_irqrestore(zms_handle_lock(zms, handle), flags);
	return published;
}

static void zms_account_physical_io(struct zms *zms, unsigned int op,
				    unsigned int pages, bool account_bd_stat,
				    int ret)
{
	if (!zms || !pages)
		return;

	if (op == REQ_OP_WRITE) {
		atomic64_inc(&zms->physical_write_ios);
		if (ret) {
			atomic64_add(pages, &zms->physical_write_failed_pages);
			return;
		}

		atomic64_add(pages, &zms->physical_write_pages);
		if (account_bd_stat)
			atomic64_add(pages, &zms->bd_stat_write_pages);
		if (zms->account_write_pages)
			zms->account_write_pages(pages);
		return;
	}

	atomic64_inc(&zms->physical_read_ios);
	if (ret) {
		atomic64_add(pages, &zms->physical_read_failed_pages);
	} else {
		atomic64_add(pages, &zms->physical_read_pages);
		if (account_bd_stat)
			atomic64_add(pages, &zms->bd_stat_read_pages);
	}
}

static void zms_io_record(struct zms *zms, struct zms_io *io, unsigned int op,
			  unsigned long block,
			  unsigned int pages, bool account_bd_stat,
			  u64 latency_ns, int ret)
{
	zms_account_physical_io(zms, op, pages, account_bd_stat, ret);

	if (!io)
		return;

	io->submitted = true;
	io->write = op == REQ_OP_WRITE;
	io->block_index = block;
	io->latency_ns = latency_ns;
	io->ret = ret;
	if (op == REQ_OP_WRITE) {
		io->write_submitted += pages;
		io->write_ios++;
		if (ret)
			io->write_failed += pages;
		if (U64_MAX - io->write_total_latency_ns < latency_ns)
			io->write_total_latency_ns = U64_MAX;
		else
			io->write_total_latency_ns += latency_ns;
		if (latency_ns > io->write_max_latency_ns)
			io->write_max_latency_ns = latency_ns;
		io->write_last_block = block;
		io->write_last_latency_ns = latency_ns;
		io->write_last_ret = ret;
	} else {
		io->read_submitted += pages;
		io->read_ios++;
		if (ret)
			io->read_failed += pages;
		if (U64_MAX - io->read_total_latency_ns < latency_ns)
			io->read_total_latency_ns = U64_MAX;
		else
			io->read_total_latency_ns += latency_ns;
		if (latency_ns > io->read_max_latency_ns)
			io->read_max_latency_ns = latency_ns;
		io->read_last_block = block;
		io->read_last_latency_ns = latency_ns;
		io->read_last_ret = ret;
	}
}

static bool zms_block_needs_readback(const struct zms_block *block)
{
	return !zms_block_data_load(block);
}

static int zms_read_frozen_block_unlocked(struct zms *zms,
					  struct zms_class *class,
					  struct zms_block *block,
					  gfp_t gfp, struct zms_io *io)
{
	int ret;

	if (!zms_block_needs_readback(block))
		return 0;
	if (block->reading)
		return -EBUSY;

	block->reading = true;
	block->read_ret = 0;
	mutex_unlock(&class->lock);

	ret = zms_read_block(zms, block, gfp, io, false);

	mutex_lock(&class->lock);
	block->read_ret = ret;
	block->reading = false;
	wake_up_all(&block->read_wait);
	return ret;
}

static unsigned int zms_contiguous_run(const struct zms_block *block,
				       unsigned int start)
{
	unsigned int run = 1;

	while (start + run < block->pages &&
	       block->blocks[start + run] == block->blocks[start] + run)
		run++;

	return run;
}

static int zms_submit_run_data(struct zms *zms, struct zms_block *block,
			       void *base, unsigned int page_idx,
			       unsigned int nr_pages, unsigned int op,
			       gfp_t gfp, struct zms_io *io,
			       bool account_bd_stat)
{
	unsigned int done = 0;

	(void)gfp;
	while (done < nr_pages) {
		struct bio_vec bvecs[ZMS_MAX_PAGES_PER_ZSPAGE];
		unsigned int submitted = 0;
		struct bio bio;
		u64 start;
		int ret;

		bio_init(&bio, zms->bdev, bvecs, ARRAY_SIZE(bvecs),
			 op | REQ_SYNC);
		bio.bi_iter.bi_sector =
			zms_block_sector(block->blocks[page_idx + done]);

		while (done + submitted < nr_pages) {
			unsigned int cur = page_idx + done + submitted;
			void *data = (char *)base + (cur << PAGE_SHIFT);
			struct page *page = zms_data_page(data);
			int added;

			added = bio_add_page(&bio, page, PAGE_SIZE,
					     offset_in_page(data));
			if (added != PAGE_SIZE)
				break;
			submitted++;
		}

		if (WARN_ON_ONCE(!submitted)) {
			bio_uninit(&bio);
			return -EIO;
		}

		start = ktime_get_ns();
		ret = submit_bio_wait(&bio);
		zms_io_record(zms, io, op, block->blocks[page_idx + done],
			      submitted, account_bd_stat,
			      ktime_get_ns() - start, ret);
		bio_uninit(&bio);
		if (ret)
			return ret;
		done += submitted;
	}

	return 0;
}

static int zms_submit_block_data(struct zms *zms, struct zms_block *block,
				 void *data, unsigned int op, gfp_t gfp,
				 struct zms_io *io, bool account_bd_stat)
{
	unsigned int i;
	int ret;

	for (i = 0; i < block->pages; ) {
		unsigned int run = zms_contiguous_run(block, i);

		ret = zms_submit_run_data(zms, block, data, i, run, op, gfp,
					  io, account_bd_stat);
		if (ret)
			return ret;
		i += run;
	}

	return 0;
}

static int zms_write_block(struct zms *zms, struct zms_block *block,
			   gfp_t gfp, struct zms_io *io, bool account_bd_stat)
{
	void *data = zms_block_data_load(block);

	if (WARN_ON_ONCE(!data))
		return -EIO;

	return zms_submit_block_data(zms, block, data, REQ_OP_WRITE, gfp, io,
				     account_bd_stat);
}

static int zms_flush_class_to_locked(struct zms *zms, struct zms_class *class,
				     unsigned long target_pages, gfp_t gfp,
				     struct zms_io *io)
{
	bool saw_pinned;
	enum zms_fullness fullness;

restart:
	saw_pinned = false;
	for (fullness = ZMS_FG_LOW; fullness < ZMS_NR_FULLNESS; fullness++) {
		struct zms_block *block;

		list_for_each_entry(block, &class->fullness[fullness], list) {
			bool account_bd_stat;
			int ret;

			if (target_pages && zms_dirty_pages(zms) <= target_pages)
				return 0;
			if (!block->dirty)
				continue;
			if (!zms_block_freeze(block)) {
				saw_pinned = true;
				continue;
			}
			account_bd_stat = block->bd_stat_dirty;
			mutex_unlock(&class->lock);
			ret = zms_write_block(zms, block, gfp, io,
					      account_bd_stat);
			mutex_lock(&class->lock);
			if (!ret) {
				zms_clear_dirty_locked(zms, block);
				zms_release_block_data_locked(zms, block);
			}
			zms_block_unfreeze(zms, block);
			if (ret)
				return ret;
			goto restart;
		}
	}

	if (target_pages && zms_dirty_pages(zms) <= target_pages)
		return 0;
	if (saw_pinned)
		return -EAGAIN;
	return 0;
}

static int zms_flush_class_locked(struct zms *zms, struct zms_class *class,
				  gfp_t gfp, struct zms_io *io)
{
	return zms_flush_class_to_locked(zms, class, 0, gfp, io);
}

static int zms_read_block(struct zms *zms, struct zms_block *block,
			  gfp_t gfp, struct zms_io *io, bool account_bd_stat)
{
	void *data;
	int ret;

	if (zms_block_data_load(block))
		return 0;

	data = kvzalloc(zms_block_bytes(block), gfp);
	if (!data)
		return -ENOMEM;

	ret = zms_submit_block_data(zms, block, data, REQ_OP_READ, gfp, io,
				    account_bd_stat);
	if (ret) {
		kvfree(data);
		return ret;
	}
	zms_block_data_publish(block, data);
	return ret;
}

static unsigned long zms_alloc_disk_block_locked(struct zms *zms)
{
	unsigned long scan;
	unsigned int pass;

	if (!zms->block_bitmap || zms->nr_blocks <= ZMS_BLOCK_RESERVED)
		return 0;

	scan = zms->next_block;
	if (scan < ZMS_BLOCK_RESERVED || scan >= zms->nr_blocks)
		scan = ZMS_BLOCK_RESERVED;

	for (pass = 0; pass < 2; pass++) {
		unsigned long end = pass ? zms->next_block : zms->nr_blocks;

		if (end <= ZMS_BLOCK_RESERVED)
			end = zms->nr_blocks;
		while (scan < end) {
			scan = find_next_zero_bit(zms->block_bitmap, end, scan);
			if (scan >= end)
				break;
			__set_bit(scan, zms->block_bitmap);
			zms_stat_add(zms, ZMS_STAT_USED_BLOCKS, 1);
			zms->next_block = scan + 1;
			if (zms->next_block >= zms->nr_blocks)
				zms->next_block = ZMS_BLOCK_RESERVED;
			return scan;
		}
		scan = ZMS_BLOCK_RESERVED;
	}

	return 0;
}

static bool zms_alloc_disk_run_locked(struct zms *zms, unsigned long *blocks,
				      unsigned int nr_blocks)
{
	unsigned long scan;
	unsigned int pass;

	if (!nr_blocks)
		return false;
	if (nr_blocks == 1) {
		blocks[0] = zms_alloc_disk_block_locked(zms);
		return blocks[0] != 0;
	}
	if (!zms->block_bitmap || zms->nr_blocks <= ZMS_BLOCK_RESERVED ||
	    nr_blocks > zms->nr_blocks - ZMS_BLOCK_RESERVED)
		return false;

	scan = zms->next_block;
	if (scan < ZMS_BLOCK_RESERVED || scan >= zms->nr_blocks)
		scan = ZMS_BLOCK_RESERVED;

	for (pass = 0; pass < 2; pass++) {
		unsigned long end = pass ? zms->next_block : zms->nr_blocks;

		if (end <= ZMS_BLOCK_RESERVED)
			end = zms->nr_blocks;
		while (scan + nr_blocks <= end) {
			unsigned long next_set;

			scan = find_next_zero_bit(zms->block_bitmap, end, scan);
			if (scan + nr_blocks > end)
				break;

			next_set = find_next_bit(zms->block_bitmap,
						 scan + nr_blocks, scan);
			if (next_set >= scan + nr_blocks) {
				unsigned int i;

				for (i = 0; i < nr_blocks; i++) {
					__set_bit(scan + i, zms->block_bitmap);
					blocks[i] = scan + i;
				}
				zms_stat_add(zms, ZMS_STAT_USED_BLOCKS, nr_blocks);
				zms->next_block = scan + nr_blocks;
				if (zms->next_block >= zms->nr_blocks)
					zms->next_block = ZMS_BLOCK_RESERVED;
				return true;
			}

			scan = next_set + 1;
		}
		scan = ZMS_BLOCK_RESERVED;
	}

	return false;
}

static unsigned int zms_alloc_disk_partial_run_locked(struct zms *zms,
						      unsigned long *blocks,
						      unsigned int nr_blocks,
						      unsigned int *segments)
{
	unsigned int allocated = 0;

	while (allocated < nr_blocks) {
		unsigned long best_start = 0;
		unsigned int best_len = 0;
		unsigned long scan;
		unsigned int pass;
		unsigned int remaining = nr_blocks - allocated;

		if (!zms->block_bitmap || zms->nr_blocks <= ZMS_BLOCK_RESERVED)
			break;

		scan = zms->next_block;
		if (scan < ZMS_BLOCK_RESERVED || scan >= zms->nr_blocks)
			scan = ZMS_BLOCK_RESERVED;

		for (pass = 0; pass < 2; pass++) {
			unsigned long end = pass ? zms->next_block : zms->nr_blocks;

			if (end <= ZMS_BLOCK_RESERVED)
				end = zms->nr_blocks;

			while (scan < end) {
				unsigned long next_set;
				unsigned int len;

				scan = find_next_zero_bit(zms->block_bitmap, end, scan);
				if (scan >= end)
					break;

				next_set = find_next_bit(zms->block_bitmap,
							 min_t(unsigned long,
							       end, scan + remaining),
							 scan);
				len = next_set - scan;
				if (len > best_len) {
					best_start = scan;
					best_len = len;
					if (best_len == remaining)
						break;
				}

				scan = next_set + 1;
			}

			if (best_len == remaining)
				break;
			scan = ZMS_BLOCK_RESERVED;
		}

		if (!best_len)
			break;

		zms_stat_add(zms, ZMS_STAT_USED_BLOCKS, best_len);
		while (best_len && allocated < nr_blocks) {
			__set_bit(best_start, zms->block_bitmap);
			blocks[allocated++] = best_start++;
			best_len--;
		}
		if (segments)
			(*segments)++;
		zms->next_block = best_start;
		if (zms->next_block >= zms->nr_blocks)
			zms->next_block = ZMS_BLOCK_RESERVED;
	}

	return allocated;
}

static void zms_free_disk_block_locked(struct zms *zms, unsigned long block)
{
	if (WARN_ON_ONCE(block < ZMS_BLOCK_RESERVED || block >= zms->nr_blocks))
		return;

	WARN_ON_ONCE(!test_and_clear_bit(block, zms->block_bitmap));
	zms_stat_add(zms, ZMS_STAT_USED_BLOCKS, -1);
}

static unsigned long zms_alloc_disk_block(struct zms *zms)
{
	unsigned long block;
	unsigned long flags;

	spin_lock_irqsave(&zms->alloc_lock, flags);
	block = zms_alloc_disk_block_locked(zms);
	spin_unlock_irqrestore(&zms->alloc_lock, flags);
	return block;
}

static bool zms_alloc_disk_run(struct zms *zms, unsigned long *blocks,
			       unsigned int nr_blocks)
{
	bool ret;
	unsigned long flags;

	spin_lock_irqsave(&zms->alloc_lock, flags);
	ret = zms_alloc_disk_run_locked(zms, blocks, nr_blocks);
	spin_unlock_irqrestore(&zms->alloc_lock, flags);
	return ret;
}

static unsigned int zms_alloc_disk_partial_run(struct zms *zms,
					       unsigned long *blocks,
					       unsigned int nr_blocks,
					       unsigned int *segments)
{
	unsigned int ret;
	unsigned long flags;

	spin_lock_irqsave(&zms->alloc_lock, flags);
	ret = zms_alloc_disk_partial_run_locked(zms, blocks, nr_blocks, segments);
	spin_unlock_irqrestore(&zms->alloc_lock, flags);
	return ret;
}

static void zms_free_disk_block(struct zms *zms, unsigned long block)
{
	unsigned long flags;

	spin_lock_irqsave(&zms->alloc_lock, flags);
	zms_free_disk_block_locked(zms, block);
	spin_unlock_irqrestore(&zms->alloc_lock, flags);
}

static void zms_mark_dirty_locked(struct zms *zms, struct zms_block *block,
				  bool account_bd_stat)
{
	if (!block)
		return;

	zms_clean_cache_del(zms, block);
	if (account_bd_stat)
		block->bd_stat_dirty = true;
	if (block->dirty)
		return;

	zms_stat_add(zms, ZMS_STAT_DIRTY_BLOCKS, 1);
	zms_stat_add(zms, ZMS_STAT_DIRTY_PAGES, block->pages);
	block->dirty = true;
}

static void zms_clear_dirty_locked(struct zms *zms, struct zms_block *block)
{
	if (!block || !block->dirty)
		return;

	zms_stat_add(zms, ZMS_STAT_DIRTY_BLOCKS, -1);
	zms_stat_add(zms, ZMS_STAT_DIRTY_PAGES, -(s64)block->pages);
	block->dirty = false;
	block->bd_stat_dirty = false;
}

static void zms_free_block(struct zms *zms, struct zms_block *block)
{
	void *data;

	if (!block)
		return;

	zms_clean_cache_del(zms, block);
	zms_clear_dirty_locked(zms, block);
	kvfree(block->bitmap);
	kvfree(block->slot_handles);
	data = zms_block_data_load(block);
	if (data)
		kvfree(data);
	kfree(block);
}

static enum zms_fullness zms_block_fullness(const struct zms_block *block)
{
	unsigned int pct;

	if (WARN_ON_ONCE(!block->used || block->used > block->slots))
		return ZMS_FG_LOW;
	if (block->used == block->slots)
		return ZMS_FG_FULL;

	pct = block->used * 100U / block->slots;
	if (pct <= 33U)
		return ZMS_FG_LOW;
	if (pct <= 66U)
		return ZMS_FG_MID;

	return ZMS_FG_ALMOST_FULL;
}

static void zms_insert_block_locked(struct zms *zms, struct zms_class *class,
				    struct zms_block *block)
{
	if (WARN_ON_ONCE(block->listed))
		return;

	block->fullness = zms_block_fullness(block);
	list_add_tail(&block->list, &class->fullness[block->fullness]);
	block->listed = true;
	class->listed_blocks++;
	if (class->listed_blocks == 1)
		atomic_long_inc(&zms->valid_classes);
	zms_fullness_stats_add(zms, block->fullness, block->pages);
}

static void zms_remove_block_locked(struct zms *zms, struct zms_class *class,
				    struct zms_block *block)
{
	if (WARN_ON_ONCE(!block->listed))
		return;

	zms_fullness_stats_add(zms, block->fullness, -(s64)block->pages);
	list_del_init(&block->list);
	block->listed = false;
	if (WARN_ON_ONCE(!class->listed_blocks))
		return;
	class->listed_blocks--;
	if (!class->listed_blocks) {
		WARN_ON_ONCE(!atomic_long_read(&zms->valid_classes));
		if (atomic_long_read(&zms->valid_classes))
			atomic_long_dec(&zms->valid_classes);
	}
}

static void zms_fix_fullness_locked(struct zms *zms, struct zms_class *class,
				    struct zms_block *block)
{
	enum zms_fullness old_fullness;
	enum zms_fullness fullness;

	if (!block->used)
		return;
	if (WARN_ON_ONCE(!block->listed))
		return;

	old_fullness = block->fullness;
	fullness = zms_block_fullness(block);
	if (fullness == old_fullness)
		return;

	zms_fullness_stats_add(zms, old_fullness, -(s64)block->pages);
	list_move_tail(&block->list, &class->fullness[fullness]);
	block->fullness = fullness;
	zms_fullness_stats_add(zms, block->fullness, block->pages);
	if (fullness <= ZMS_FG_LOW) {
		if (READ_ONCE(block->clean_cached))
			atomic_long_inc(&zms->clean_cache_fullness_drops);
		zms_clean_cache_del(zms, block);
	}
}

static struct zms_block *zms_find_available_block_locked(struct zms_class *class)
{
	int fullness;

	for (fullness = ZMS_FG_ALMOST_FULL; fullness >= ZMS_FG_LOW; fullness--) {
		struct zms_block *block;

		list_for_each_entry(block, &class->fullness[fullness], list) {
			if (zms_block_freeze(block))
				return block;
		}
		if (fullness == ZMS_FG_LOW)
			break;
	}

	return NULL;
}

static struct zms_block *zms_alloc_block_locked(struct zms *zms,
						struct zms_class *class,
						gfp_t gfp)
{
	struct zms_block *block;
	size_t bitmap_size;
	unsigned int i;
	unsigned int partial = 0;
	unsigned int fallback = 0;
	unsigned int segments = 0;
	bool full_run = false;

	block = kzalloc(struct_size(block, blocks, class->pages_per_zspage), gfp);
	if (!block)
		return NULL;

	INIT_LIST_HEAD(&block->clean_node);
	block->pages = class->pages_per_zspage;
	block->slots = class->slots_per_zspage;
	bitmap_size = BITS_TO_LONGS(block->slots) * sizeof(unsigned long);
	block->bitmap = kvzalloc(bitmap_size, gfp);
	if (!block->bitmap)
		goto err_block;

	block->slot_handles = kvmalloc_array(block->slots,
					     sizeof(*block->slot_handles),
					     gfp | __GFP_ZERO);
	if (!block->slot_handles)
		goto err_block;

	zms_block_data_publish(block, kvzalloc(zms_block_bytes(block), gfp));
	if (!zms_block_data_load(block))
		goto err_block;

	if (zms_alloc_disk_run(zms, block->blocks, block->pages)) {
		if (block->pages > 1)
			full_run = true;
		segments = block->pages > 1 ? 1 : 0;
		i = block->pages;
	} else {
		if (block->pages > 1)
			partial = zms_alloc_disk_partial_run(zms,
							     block->blocks,
							     block->pages,
							     &segments);
		for (i = partial; i < block->pages; i++) {
			block->blocks[i] = zms_alloc_disk_block(zms);
			if (!block->blocks[i])
				goto err_allocated_blocks;
			fallback++;
			if (block->pages > 1)
				segments++;
		}
	}
	if (block->pages > 1) {
		if (full_run) {
			atomic_long_inc(&zms->alloc_run_successes);
			atomic_long_add(block->pages, &zms->alloc_run_success_pages);
		} else {
			atomic_long_inc(&zms->alloc_run_failures);
			atomic_long_add(partial, &zms->alloc_run_partial_pages);
			atomic_long_add(fallback, &zms->alloc_run_fallback_pages);
		}
		atomic_long_add(segments, &zms->alloc_run_segments);
	}
	atomic_long_inc(&zms->alloc_blocks);

	block->class_size = class->size;
	block->fullness = ZMS_FG_LOW;
	atomic_set(&block->pin_state, 0);
	INIT_LIST_HEAD(&block->list);
	init_waitqueue_head(&block->read_wait);
	return block;

err_allocated_blocks:
	while (i > 0) {
		i--;
		zms_free_disk_block(zms, block->blocks[i]);
	}
err_block:
	zms_free_block(zms, block);
	return NULL;
}

static unsigned long zms_alloc_run_attempts(const struct zms *zms)
{
	return atomic_long_read(&zms->alloc_run_successes) +
		atomic_long_read(&zms->alloc_run_failures);
}

static void zms_fullness_stats_add(struct zms *zms, enum zms_fullness fullness,
				   s64 pages)
{
	switch (fullness) {
	case ZMS_FG_LOW:
		zms_stat_add(zms, ZMS_STAT_PARTIAL_BLOCKS, pages);
		zms_stat_add(zms, ZMS_STAT_LOW_BLOCKS, pages);
		break;
	case ZMS_FG_MID:
		zms_stat_add(zms, ZMS_STAT_PARTIAL_BLOCKS, pages);
		zms_stat_add(zms, ZMS_STAT_MID_BLOCKS, pages);
		break;
	case ZMS_FG_ALMOST_FULL:
		zms_stat_add(zms, ZMS_STAT_PARTIAL_BLOCKS, pages);
		zms_stat_add(zms, ZMS_STAT_ALMOST_FULL_BLOCKS, pages);
		break;
	case ZMS_FG_FULL:
		zms_stat_add(zms, ZMS_STAT_FULL_BLOCKS, pages);
		break;
	default:
		break;
	}
}

static int zms_free_handle(struct zms *zms, unsigned long handle);
static bool zms_handle_pending_locked(struct zms *zms, unsigned long handle);
static unsigned long zms_handle_for_entry_locked(struct zms *zms,
						 struct zms_block *block,
						 unsigned long slot);
static int zms_move_slot_locked(struct zms *zms, struct zms_class *class,
				struct zms_block *source,
				struct zms_block *target,
				unsigned long src_slot, gfp_t gfp,
				struct zms_io *io);

static bool zms_free_empty_block_locked(struct zms *zms, struct zms_class *class,
					struct zms_block *block)
{
	unsigned int i;

	if (WARN_ON_ONCE(!block || block->used))
		return false;
	if (!zms_block_frozen(block) && !zms_block_freeze(block))
		return false;

	zms_remove_block_locked(zms, class, block);
	for (i = 0; i < block->pages; i++)
		zms_free_disk_block(zms, block->blocks[i]);
	zms_free_block(zms, block);
	return true;
}

static void zms_rollback_store_slot_locked(struct zms *zms,
					   struct zms_class *class,
					   struct zms_block *block,
					   unsigned long slot, size_t size)
{
	__clear_bit(slot, block->bitmap);
	block->slot_handles[slot] = 0;
	zms_stat_add(zms, ZMS_STAT_OBJECTS, -1);
	zms_stat_add(zms, ZMS_STAT_STORED_BYTES, -(s64)size);
	zms_stat_add(zms, ZMS_STAT_PACKED_BYTES, -(s64)class->size);
	block->used--;
}

static unsigned int zms_reclaim_pending(struct zms *zms,
					unsigned int max_handles)
{
	unsigned long handle = 1;
	unsigned int reclaimed = 0;

	while (!max_handles || reclaimed < max_handles) {
		unsigned long flags;

		spin_lock_irqsave(&zms->pending_lock, flags);
		handle = find_next_bit(zms->pending_free, zms->nr_handles + 1,
				       handle);
		if (handle > zms->nr_handles) {
			spin_unlock_irqrestore(&zms->pending_lock, flags);
			break;
		}
		spin_unlock_irqrestore(&zms->pending_lock, flags);

		if (!zms_free_handle(zms, handle))
			reclaimed++;
		handle++;
	}

	return reclaimed;
}

static struct zms_block *zms_find_block_locked(struct zms *zms,
					       struct zms_class *class,
					       gfp_t gfp)
{
	struct zms_block *block;
	unsigned int reclaimed;

	block = zms_find_available_block_locked(class);
	if (block)
		return block;

	mutex_unlock(&class->lock);
	reclaimed = zms_reclaim_pending(zms, ZMS_RECLAIM_BEFORE_ALLOC_MAX);
	mutex_lock(&class->lock);
	if (reclaimed) {
		atomic_long_inc(&zms->reclaim_before_alloc_calls);
		atomic_long_add(reclaimed, &zms->reclaim_before_alloc_handles);
		block = zms_find_available_block_locked(class);
		if (block)
			return block;
	}

	block = zms_alloc_block_locked(zms, class, gfp);
	if (!block)
		return NULL;
	if (WARN_ON_ONCE(!zms_block_freeze(block))) {
		unsigned int i;

		for (i = 0; i < block->pages; i++)
			zms_free_disk_block(zms, block->blocks[i]);
		zms_free_block(zms, block);
		return NULL;
	}

	return block;
}

static bool zms_handle_valid_locked(struct zms *zms, unsigned long handle)
{
	struct zms_handle_entry *entry;
	unsigned long flags;
	bool valid;

	if (!handle || handle > zms->nr_handles)
		return false;

	spin_lock_irqsave(zms_handle_lock(zms, handle), flags);
	entry = &zms->handles[handle];
	valid = zms_handle_active(entry);
	spin_unlock_irqrestore(zms_handle_lock(zms, handle), flags);
	return valid;
}

static bool zms_handle_pending_locked(struct zms *zms, unsigned long handle)
{
	bool pending;
	unsigned long flags;

	if (!handle || handle > zms->nr_handles)
		return false;

	spin_lock_irqsave(&zms->pending_lock, flags);
	pending = test_bit(handle, zms->pending_free);
	spin_unlock_irqrestore(&zms->pending_lock, flags);
	return pending;
}

static unsigned long *zms_alloc_bitmap(unsigned long bits, gfp_t gfp)
{
	size_t longs;

	if (!bits || bits > ULONG_MAX - (BITS_PER_LONG - 1))
		return NULL;
	longs = BITS_TO_LONGS(bits);
	if (longs > SIZE_MAX / sizeof(unsigned long))
		return NULL;

	return kvzalloc(longs * sizeof(unsigned long), gfp);
}

static bool zms_handles_size(unsigned long nr_handles, size_t elem_size,
			     size_t *bytes)
{
	if (!nr_handles || nr_handles == ULONG_MAX)
		return false;

	*bytes = array_size(nr_handles + 1, elem_size);
	return *bytes != SIZE_MAX;
}

static int zms_free_handle(struct zms *zms, unsigned long handle)
{
	struct zms_load_snapshot snapshot;
	struct zms_handle_entry *entry;
	struct zms_class *class;
	struct zms_block *block;
	unsigned long flags;
	unsigned int class_size;
	bool request_free;

	if (!handle || handle > zms->nr_handles)
		return -EINVAL;

	memset(&snapshot, 0, sizeof(snapshot));
	spin_lock_irqsave(zms_handle_lock(zms, handle), flags);
	entry = &zms->handles[handle];
	if (!entry->block) {
		entry->flags &= ~ZMS_HANDLE_PENDING_FREE;
		spin_unlock_irqrestore(zms_handle_lock(zms, handle), flags);
		zms_pending_stat_dec_if_set(zms, handle);
		return -ENOENT;
	}
	if (!(entry->flags & ZMS_HANDLE_PENDING_FREE)) {
		spin_unlock_irqrestore(zms_handle_lock(zms, handle), flags);
		zms_pending_stat_dec_if_set(zms, handle);
		return -ENOENT;
	}
	if (!zms_block_freeze(entry->block)) {
		block = entry->block;
		class_size = block->class_size;
		spin_unlock_irqrestore(zms_handle_lock(zms, handle), flags);

		class = zms_class_for_size(zms, class_size);
		if (WARN_ON_ONCE(!class))
			return -EIO;

		mutex_lock(&class->lock);
		request_free = false;
		spin_lock_irqsave(zms_handle_lock(zms, handle), flags);
		entry = &zms->handles[handle];
		if (entry->block == block &&
		    (entry->flags & ZMS_HANDLE_PENDING_FREE))
			request_free = true;
		spin_unlock_irqrestore(zms_handle_lock(zms, handle), flags);
		if (request_free)
			zms_block_request_cleanup(zms, block, ZMS_PIN_DEFER_FREE);
		mutex_unlock(&class->lock);
		return -EAGAIN;
	}
	snapshot.block = entry->block;
	snapshot.offset = entry->offset;
	snapshot.size = entry->size;
	snapshot.slot = entry->slot;
	snapshot.generation = entry->generation;
	snapshot.valid = true;
	spin_unlock_irqrestore(zms_handle_lock(zms, handle), flags);

	block = snapshot.block;
	class = zms_class_for_size(zms, block->class_size);
	if (WARN_ON_ONCE(!class)) {
		zms_block_unfreeze(zms, block);
		return -EIO;
	}

	mutex_lock(&class->lock);
	spin_lock_irqsave(zms_handle_lock(zms, handle), flags);
	entry = &zms->handles[handle];
	if (entry->block != block || entry->slot != snapshot.slot ||
	    entry->offset != snapshot.offset || entry->size != snapshot.size ||
	    entry->generation != snapshot.generation ||
	    !(entry->flags & ZMS_HANDLE_PENDING_FREE)) {
		spin_unlock_irqrestore(zms_handle_lock(zms, handle), flags);
		zms_block_unfreeze(zms, block);
		mutex_unlock(&class->lock);
		return -ENOENT;
	}
	entry->block = NULL;
	entry->offset = 0;
	entry->size = 0;
	entry->slot = 0;
	entry->flags = 0;
	entry->generation++;
	spin_unlock_irqrestore(zms_handle_lock(zms, handle), flags);
	zms_pending_stat_dec_if_set(zms, handle);

	__clear_bit(snapshot.slot, block->bitmap);
	block->slot_handles[snapshot.slot] = 0;
	zms_stat_add(zms, ZMS_STAT_OBJECTS, -1);
	zms_stat_add(zms, ZMS_STAT_STORED_BYTES, -(s64)snapshot.size);
	zms_stat_add(zms, ZMS_STAT_PACKED_BYTES, -(s64)class->size);

	block->used--;

	if (!block->used) {
		if (!zms_free_empty_block_locked(zms, class, block)) {
			mutex_unlock(&class->lock);
			return -EAGAIN;
		}
	} else {
		zms_fix_fullness_locked(zms, class, block);
		if (!block->dirty)
			zms_reconcile_clean_cache_locked(zms, block);
		zms_block_unfreeze(zms, block);
	}
	mutex_unlock(&class->lock);
	return 0;
}

static void zms_reclaim_pending_all(struct zms *zms)
{
	zms_reclaim_pending(zms, 0);
}

static struct zms_block *zms_compact_source_locked(struct zms_class *class)
{
	enum zms_fullness fullness;

	for (fullness = ZMS_FG_LOW; fullness <= ZMS_FG_MID; fullness++) {
		struct zms_block *block;

		list_for_each_entry(block, &class->fullness[fullness], list) {
			if (!zms_block_pinned(block))
				return block;
		}
	}

	return NULL;
}

static struct zms_block *zms_compact_target_locked(struct zms_class *class,
						   struct zms_block *source)
{
	int fullness;

	for (fullness = ZMS_FG_ALMOST_FULL; fullness >= ZMS_FG_LOW; fullness--) {
		struct zms_block *block;

		list_for_each_entry(block, &class->fullness[fullness], list) {
			if (block != source && !zms_block_pinned(block))
				return block;
		}
		if (fullness == ZMS_FG_LOW)
			break;
	}

	return NULL;
}

static unsigned long zms_handle_for_entry_locked(struct zms *zms,
						 struct zms_block *block,
						 unsigned long slot)
{
	if (!block || slot >= block->slots || !block->slot_handles)
		return 0;

	return block->slot_handles[slot];
}

static int zms_move_slot_locked(struct zms *zms, struct zms_class *class,
				struct zms_block *source,
				struct zms_block *target,
				unsigned long src_slot, gfp_t gfp,
				struct zms_io *io)
{
	struct zms_handle_entry *entry;
	void *source_data;
	void *target_data;
	unsigned long handle;
	unsigned long flags;
	unsigned long dst_slot;
	int ret;

	if (WARN_ON_ONCE(source == target))
		return -EINVAL;

	if (!zms_block_freeze(source))
		return -EAGAIN;
	if (!zms_block_freeze(target)) {
		zms_block_unfreeze(zms, source);
		return -EAGAIN;
	}

	ret = zms_read_frozen_block_unlocked(zms, class, source, gfp, io);
	if (ret)
		goto out_unpin;
	ret = zms_read_frozen_block_unlocked(zms, class, target, gfp, io);
	if (ret)
		goto out_unpin;
	zms_clean_cache_del(zms, source);
	zms_clean_cache_del(zms, target);
	source_data = zms_block_data_load(source);
	target_data = zms_block_data_load(target);
	if (WARN_ON_ONCE(!source_data || !target_data)) {
		ret = -EIO;
		goto out_unpin;
	}

	handle = zms_handle_for_entry_locked(zms, source, src_slot);
	if (WARN_ON_ONCE(!handle)) {
		ret = -EIO;
		goto out_unpin;
	}

	dst_slot = find_first_zero_bit(target->bitmap, target->slots);
	if (WARN_ON_ONCE(dst_slot >= target->slots)) {
		ret = -ENOSPC;
		goto out_unpin;
	}

	spin_lock_irqsave(zms_handle_lock(zms, handle), flags);
	entry = &zms->handles[handle];
	if (!zms_handle_active(entry) || entry->block != source ||
	    entry->slot != src_slot || entry->offset != src_slot * class->size) {
		bool pending = entry->flags & ZMS_HANDLE_PENDING_FREE;

		spin_unlock_irqrestore(zms_handle_lock(zms, handle), flags);
		ret = pending ? -EAGAIN : -ENOENT;
		goto out_unpin;
	}
	entry->block = target;
	entry->offset = dst_slot * class->size;
	entry->slot = dst_slot;
	spin_unlock_irqrestore(zms_handle_lock(zms, handle), flags);

	memcpy((char *)target_data + dst_slot * class->size,
	       (char *)source_data + src_slot * class->size,
	       class->size);
	memset((char *)source_data + src_slot * class->size, 0, class->size);

	__set_bit(dst_slot, target->bitmap);
	target->slot_handles[dst_slot] = handle;
	target->used++;
	zms_mark_dirty_locked(zms, target, false);
	zms_fix_fullness_locked(zms, class, target);

	__clear_bit(src_slot, source->bitmap);
	source->slot_handles[src_slot] = 0;
	source->used--;
	if (source->used)
		zms_fix_fullness_locked(zms, class, source);
	ret = 0;
out_unpin:
	zms_block_unfreeze(zms, target);
	zms_block_unfreeze(zms, source);
	return ret;
}

#define ZMS_COMPACT_TIME_LIMIT_MS 100ULL

static bool zms_compact_time_exceeded(u64 start_time_ms)
{
	u64 now_ms;

	if (!start_time_ms)
		return false;

	now_ms = ktime_to_ms(ktime_get_boottime());
	return now_ms - start_time_ms >= ZMS_COMPACT_TIME_LIMIT_MS;
}

static int zms_compact_class_locked(struct zms *zms, struct zms_class *class,
				    gfp_t gfp, struct zms_io *io, u64 start_time_ms)
{
	struct zms_block *source;
	struct zms_block *target;
	unsigned long slot;
	int ret;

	while ((source = zms_compact_source_locked(class))) {
		if (zms_compact_time_exceeded(start_time_ms))
			return -EAGAIN;

		target = zms_compact_target_locked(class, source);
		if (!target)
			break;

		for (;;) {
			if (zms_compact_time_exceeded(start_time_ms))
				return -EAGAIN;

			slot = find_first_bit(source->bitmap, source->slots);
			if (slot >= source->slots)
				break;
			if (target->used >= target->slots) {
				target = zms_compact_target_locked(class, source);
				if (!target)
					break;
			}
			ret = zms_move_slot_locked(zms, class, source, target,
						   slot, gfp, io);
			if (ret)
				return ret;
		}

		if (!source->used) {
			if (!zms_free_empty_block_locked(zms, class, source))
				return -EAGAIN;
		} else {
			if (!source->dirty)
				zms_drop_block_data_locked(zms, source);
			break;
		}
	}

	return zms_flush_class_locked(zms, class, gfp, io);
}

static void zms_free_workfn(struct work_struct *work)
{
	struct zms *zms = container_of(work, struct zms, free_work);

	zms_reclaim_pending_all(zms);
}

static void zms_kick_flush(struct zms *zms)
{
	if (!READ_ONCE(zms->ready) || READ_ONCE(zms->destroying))
		return;
	if (zms_dirty_pages(zms) <= zms->dirty_high_pages)
		return;

	queue_delayed_work(system_unbound_wq, &zms->flush_work,
			   msecs_to_jiffies(ZMS_DIRTY_FLUSH_DELAY_MS));
}

static void zms_flush_workfn(struct work_struct *work)
{
	struct zms *zms = container_of(to_delayed_work(work), struct zms,
				      flush_work);
	struct zms_io io;
	int ret;

	if (!READ_ONCE(zms->ready) || READ_ONCE(zms->destroying))
		return;

	ret = zms_flush_to(zms, zms->dirty_low_pages, GFP_NOIO, &io);
	if ((ret == -EAGAIN || (!ret &&
	     zms_dirty_pages(zms) > zms->dirty_low_pages)) &&
	    !READ_ONCE(zms->destroying))
		queue_delayed_work(system_unbound_wq, &zms->flush_work,
				   msecs_to_jiffies(ZMS_DIRTY_FLUSH_DELAY_MS));
}

struct zms *zms_create(struct block_device *bdev, unsigned long nr_blocks,
		       unsigned long nr_handles,
		       zms_account_write_pages_t account_write_pages)
{
	struct zms *zms;
	size_t block_bitmap_size;
	size_t handles_size;
	unsigned int i;

	if (!bdev || nr_blocks <= ZMS_BLOCK_RESERVED || !nr_handles)
		return NULL;

	zms = kzalloc(sizeof(*zms), GFP_KERNEL);
	if (!zms)
		return NULL;

	zms->bdev = bdev;
	zms->nr_blocks = nr_blocks;
	zms->nr_handles = nr_handles;
	zms->next_block = ZMS_BLOCK_RESERVED;
	zms->account_write_pages = account_write_pages;
	spin_lock_init(&zms->pending_lock);
	spin_lock_init(&zms->alloc_lock);
	spin_lock_init(&zms->clean_lock);
	INIT_LIST_HEAD(&zms->clean_lru);
	zms->clean_cache_max_pages = ZMS_CLEAN_CACHE_MAX_PAGES;
	INIT_WORK(&zms->free_work, zms_free_workfn);
	INIT_DELAYED_WORK(&zms->flush_work, zms_flush_workfn);
	zms_init_dirty_watermarks(zms);
	for (i = 0; i < ARRAY_SIZE(zms->handle_locks); i++)
		spin_lock_init(&zms->handle_locks[i]);
	for (i = 0; i < ARRAY_SIZE(zms->classes); i++) {
		enum zms_fullness fullness;

		mutex_init(&zms->classes[i].lock);
		zms->classes[i].size = (i + 1) * ZMS_CLASS_SIZE;
		zms->classes[i].pages_per_zspage =
			zms_calculate_zspage_pages(zms->classes[i].size);
		zms->classes[i].slots_per_zspage =
			zms->classes[i].pages_per_zspage * PAGE_SIZE /
			zms->classes[i].size;
		for (fullness = ZMS_FG_LOW; fullness < ZMS_NR_FULLNESS;
		     fullness++)
			INIT_LIST_HEAD(&zms->classes[i].fullness[fullness]);
	}

	if (percpu_counter_init_many(zms->statcounters, 0, GFP_KERNEL,
				     ZMS_NR_STAT_COUNTERS))
		goto err;
	zms->statcounters_ready = true;

	if (nr_blocks > ULONG_MAX - (BITS_PER_LONG - 1))
		goto err;
	if (BITS_TO_LONGS(nr_blocks) > SIZE_MAX / sizeof(unsigned long))
		goto err;
	block_bitmap_size = BITS_TO_LONGS(nr_blocks) * sizeof(unsigned long);
	zms->block_bitmap = kvzalloc(block_bitmap_size, GFP_KERNEL);
	if (!zms->block_bitmap)
		goto err;

	if (!zms_handles_size(nr_handles, sizeof(*zms->handles),
			      &handles_size))
		goto err;
	zms->handles = vzalloc(handles_size);
	if (!zms->handles)
		goto err;

	zms->pending_free = zms_alloc_bitmap(nr_handles + 1, GFP_KERNEL);
	if (!zms->pending_free)
		goto err;

	zms->ready = true;
	return zms;

err:
	zms_destroy(zms);
	return NULL;
}

int zms_set_nr_handles(struct zms *zms, unsigned long nr_handles)
{
	struct zms_handle_entry *handles;
	struct zms_handle_entry *old_handles;
	unsigned long *pending_free;
	unsigned long *old_pending_free;
	size_t handles_size;
	size_t pending_copy_size;
	unsigned long flags;

	if (!zms || !nr_handles)
		return -EINVAL;

	/*
	 * zram calls this while holding init_lock for write, so no ZMS I/O can
	 * race with replacing handles/pending_free.  The bucket lock below only
	 * keeps the local shrink check honest if this helper is reused later.
	 */
	if (nr_handles < zms->nr_handles) {
		unsigned long handle;

		for (handle = nr_handles + 1; handle <= zms->nr_handles;
		     handle++) {
			spin_lock_irqsave(zms_handle_lock(zms, handle), flags);
			if (zms->handles[handle].block) {
				spin_unlock_irqrestore(zms_handle_lock(zms, handle),
						       flags);
				return -EBUSY;
			}
			spin_unlock_irqrestore(zms_handle_lock(zms, handle), flags);
		}
	}

	if (!zms_handles_size(nr_handles, sizeof(*handles), &handles_size))
		return -EOVERFLOW;
	handles = vzalloc(handles_size);
	if (!handles)
		return -ENOMEM;
	pending_free = zms_alloc_bitmap(nr_handles + 1, GFP_KERNEL);
	if (!pending_free) {
		vfree(handles);
		return -ENOMEM;
	}

	memcpy(handles, zms->handles,
	       array_size(min(nr_handles, zms->nr_handles) + 1,
			  sizeof(*handles)));
	pending_copy_size = BITS_TO_LONGS(min(nr_handles, zms->nr_handles) + 1) *
		sizeof(unsigned long);
	spin_lock_irqsave(&zms->pending_lock, flags);
	memcpy(pending_free, zms->pending_free, pending_copy_size);
	old_pending_free = zms->pending_free;
	old_handles = zms->handles;
	zms->handles = handles;
	zms->pending_free = pending_free;
	zms->nr_handles = nr_handles;
	spin_unlock_irqrestore(&zms->pending_lock, flags);

	vfree(old_handles);
	kvfree(old_pending_free);

	return 0;
}

void zms_destroy(struct zms *zms)
{
	unsigned long flags;
	unsigned int i;
	struct zms_io io;

	if (!zms)
		return;

	spin_lock_irqsave(&zms->pending_lock, flags);
	zms->destroying = true;
	spin_unlock_irqrestore(&zms->pending_lock, flags);
	cancel_delayed_work_sync(&zms->flush_work);
	if (READ_ONCE(zms->ready))
		zms_flush_to(zms, 0, GFP_NOIO, &io);
	cancel_work_sync(&zms->free_work);
	for (i = 0; i < ARRAY_SIZE(zms->classes); i++) {
		enum zms_fullness fullness;

		for (fullness = ZMS_FG_LOW; fullness < ZMS_NR_FULLNESS;
		     fullness++) {
			struct zms_block *block;
			struct zms_block *tmp;

			mutex_lock(&zms->classes[i].lock);
				list_for_each_entry_safe(block, tmp,
							 &zms->classes[i].fullness[fullness],
							 list) {
					zms_remove_block_locked(zms, &zms->classes[i], block);
					zms_free_block(zms, block);
				}
			mutex_unlock(&zms->classes[i].lock);
		}
	}
	if (zms->statcounters_ready)
		percpu_counter_destroy_many(zms->statcounters,
					    ZMS_NR_STAT_COUNTERS);
	vfree(zms->handles);
	kvfree(zms->pending_free);
	kvfree(zms->block_bitmap);
	kfree(zms);
}

int zms_get_stats(struct zms *zms, struct zms_stats *stats)
{
	u64 load_total;

	if (!zms || !stats)
		return -EINVAL;

	memset(stats, 0, sizeof(*stats));
	stats->nr_blocks = zms->nr_blocks;
	stats->nr_handles = zms->nr_handles;
	stats->alloc_blocks = atomic_long_read(&zms->alloc_blocks);
	stats->alloc_run_successes = atomic_long_read(&zms->alloc_run_successes);
	stats->alloc_run_failures = atomic_long_read(&zms->alloc_run_failures);
	stats->read_merge_waits = atomic_long_read(&zms->read_merge_waits);
	stats->read_merge_wakeups = atomic_long_read(&zms->read_merge_wakeups);
	stats->read_merge_hits = atomic_long_read(&zms->read_merge_hits);
	stats->read_merge_mismatch = atomic_long_read(&zms->read_merge_mismatch);
	stats->read_merge_failures = atomic_long_read(&zms->read_merge_failures);
	if (zms_alloc_run_attempts(zms))
		stats->alloc_run_success_pct =
			stats->alloc_run_successes * 100 /
			zms_alloc_run_attempts(zms);
	stats->alloc_run_success_pages =
		atomic_long_read(&zms->alloc_run_success_pages);
	stats->alloc_run_partial_pages =
		atomic_long_read(&zms->alloc_run_partial_pages);
	stats->alloc_run_fallback_pages =
		atomic_long_read(&zms->alloc_run_fallback_pages);
	stats->alloc_run_segments = atomic_long_read(&zms->alloc_run_segments);
	if (stats->alloc_run_success_pages + stats->alloc_run_partial_pages +
	    stats->alloc_run_fallback_pages) {
		unsigned long contiguous = stats->alloc_run_success_pages +
					   stats->alloc_run_partial_pages;
		unsigned long total = contiguous +
				      stats->alloc_run_fallback_pages;

		stats->alloc_run_contiguous_page_pct =
			contiguous * 100 / total;
	}
	if (stats->alloc_run_segments) {
		unsigned long total = stats->alloc_run_success_pages +
				      stats->alloc_run_partial_pages +
				      stats->alloc_run_fallback_pages;

		stats->alloc_run_avg_segment_pages =
			total / stats->alloc_run_segments;
	}
	stats->reclaim_before_alloc_calls =
		atomic_long_read(&zms->reclaim_before_alloc_calls);
	stats->reclaim_before_alloc_handles =
		atomic_long_read(&zms->reclaim_before_alloc_handles);
	stats->load_resident_hits = atomic_long_read(&zms->load_resident_hits);
	stats->load_disk_misses = atomic_long_read(&zms->load_disk_misses);
	stats->clean_cache_pages = READ_ONCE(zms->clean_cache_pages);
	stats->clean_cache_evictions =
		atomic_long_read(&zms->clean_cache_evictions);
	stats->clean_cache_fullness_drops =
		atomic_long_read(&zms->clean_cache_fullness_drops);
	stats->clean_cache_demand_hits =
		atomic_long_read(&zms->clean_cache_demand_hits);
	stats->store_batch_calls = atomic_long_read(&zms->store_batch_calls);
	stats->store_batch_items = atomic_long_read(&zms->store_batch_items);
	stats->store_batch_resident_reuses =
		atomic_long_read(&zms->store_batch_resident_reuses);
	stats->store_batch_new_blocks =
		atomic_long_read(&zms->store_batch_new_blocks);
	load_total = (u64)stats->load_resident_hits + stats->load_disk_misses;
	if (load_total)
		stats->load_resident_hit_pct =
			div64_u64((u64)stats->load_resident_hits * 100,
				  load_total);
	stats->used_blocks = zms_stat_read_positive(zms, ZMS_STAT_USED_BLOCKS);
	stats->pending_free = zms_stat_read_positive(zms, ZMS_STAT_PENDING_FREE);
	stats->objects = zms_stat_read_positive(zms, ZMS_STAT_OBJECTS);
	stats->stored_bytes = zms_stat_read_positive(zms, ZMS_STAT_STORED_BYTES);
	stats->packed_bytes = zms_stat_read_positive(zms, ZMS_STAT_PACKED_BYTES);
	stats->physical_read_pages =
		atomic64_read(&zms->physical_read_pages);
	stats->physical_read_ios = atomic64_read(&zms->physical_read_ios);
	stats->physical_read_failed_pages =
		atomic64_read(&zms->physical_read_failed_pages);
	stats->physical_write_pages =
		atomic64_read(&zms->physical_write_pages);
	stats->physical_write_ios = atomic64_read(&zms->physical_write_ios);
	stats->physical_write_failed_pages =
		atomic64_read(&zms->physical_write_failed_pages);
	stats->bd_stat_read_pages = atomic64_read(&zms->bd_stat_read_pages);
	stats->bd_stat_write_pages = atomic64_read(&zms->bd_stat_write_pages);
	stats->partial_blocks =
		zms_stat_read_positive(zms, ZMS_STAT_PARTIAL_BLOCKS);
	stats->low_blocks = zms_stat_read_positive(zms, ZMS_STAT_LOW_BLOCKS);
	stats->mid_blocks = zms_stat_read_positive(zms, ZMS_STAT_MID_BLOCKS);
	stats->almost_full_blocks =
		zms_stat_read_positive(zms, ZMS_STAT_ALMOST_FULL_BLOCKS);
	stats->full_blocks = zms_stat_read_positive(zms, ZMS_STAT_FULL_BLOCKS);
	stats->dirty_blocks = zms_stat_read_positive(zms, ZMS_STAT_DIRTY_BLOCKS);
	stats->dirty_pages = zms_stat_read_positive(zms, ZMS_STAT_DIRTY_PAGES);
	stats->valid_classes = atomic_long_read(&zms->valid_classes);
	if (stats->nr_blocks >= stats->used_blocks)
		stats->free_blocks = stats->nr_blocks - stats->used_blocks;

	return 0;
}

static int zms_prepare_store_handle(struct zms *zms, unsigned long handle)
{
	unsigned int retries = 0;
	int ret;

retry:
	if (zms_handle_pending_locked(zms, handle)) {
		ret = zms_free_handle(zms, handle);
		if (ret == -EAGAIN && retries++ < ZMS_RECLAIM_BEFORE_ALLOC_MAX) {
			cond_resched();
			goto retry;
		}
	}
	if (zms_handle_pending_locked(zms, handle))
		return -EEXIST;

	return 0;
}

static int zms_store_locked(struct zms *zms, struct zms_class *class,
			    unsigned long handle, const void *src, size_t size,
			    gfp_t gfp, struct zms_io *io,
			    struct zms_block **batch_block)
{
	struct zms_block *block = batch_block ? *batch_block : NULL;
	void *data;
	unsigned long slot;
	int ret;

	if (zms_handle_valid_locked(zms, handle))
		return -EEXIST;

	if (!block) {
		bool resident;

		block = zms_find_block_locked(zms, class, gfp);
		if (!block)
			return -ENOSPC;
		resident = zms_block_data_load(block);
		if (batch_block) {
			if (!block->listed)
				atomic_long_inc(&zms->store_batch_new_blocks);
			else if (resident)
				atomic_long_inc(&zms->store_batch_resident_reuses);
		}

		ret = zms_read_frozen_block_unlocked(zms, class, block, gfp, io);
		if (ret) {
			zms_block_unfreeze(zms, block);
			return ret;
		}
		zms_clean_cache_del(zms, block);
	}

	slot = find_first_zero_bit(block->bitmap, block->slots);
	if (WARN_ON_ONCE(slot >= block->slots)) {
		if (batch_block)
			*batch_block = NULL;
		zms_block_unfreeze(zms, block);
		return -ENOSPC;
	}

	data = zms_block_data_load(block);
	if (WARN_ON_ONCE(!data)) {
		if (batch_block)
			*batch_block = NULL;
		zms_block_unfreeze(zms, block);
		return -EIO;
	}

	memcpy((char *)data + slot * class->size, src, size);
	if (class->size > size)
		memset((char *)data + slot * class->size + size, 0,
		       class->size - size);
	__set_bit(slot, block->bitmap);
	block->slot_handles[slot] = handle;
	block->used++;
	zms_stat_add(zms, ZMS_STAT_OBJECTS, 1);
	zms_stat_add(zms, ZMS_STAT_STORED_BYTES, size);
	zms_stat_add(zms, ZMS_STAT_PACKED_BYTES, class->size);
	zms_mark_dirty_locked(zms, block, false);
	if (!block->listed)
		zms_insert_block_locked(zms, class, block);
	else
		zms_fix_fullness_locked(zms, class, block);

	if (!zms_handle_publish_store(zms, handle, block, slot * class->size,
				      size, slot)) {
		bool empty;

		zms_rollback_store_slot_locked(zms, class, block, slot, size);
		empty = !block->used;
		if (!empty) {
			zms_fix_fullness_locked(zms, class, block);
			if (batch_block)
				*batch_block = block;
			else
				zms_block_unfreeze(zms, block);
		} else {
			if (batch_block)
				*batch_block = NULL;
			if (!zms_free_empty_block_locked(zms, class, block))
				return -EEXIST;
		}
		return -EEXIST;
	}

	zms_mark_dirty_locked(zms, block, true);
	if (batch_block && block->used < block->slots) {
		*batch_block = block;
	} else {
		if (batch_block)
			*batch_block = NULL;
		zms_block_unfreeze(zms, block);
	}
	return 0;
}

int zms_store(struct zms *zms, unsigned long handle, const void *src,
	      size_t size, gfp_t gfp, struct zms_io *io)
{
	struct zms_class *class;
	int ret;

	zms_io_clear(io);
	if (!zms || !src || !size || size > PAGE_SIZE ||
	    handle == 0 || handle > zms->nr_handles)
		return -EINVAL;

	class = zms_class_for_size(zms, size);
	if (!class)
		return -EINVAL;

	if (zms_dirty_pages(zms) > zms->dirty_hard_pages) {
		ret = zms_flush_to(zms, zms->dirty_high_pages, gfp, io);
		if (ret && ret != -EAGAIN)
			return ret;
		if (ret == -EAGAIN &&
		    zms_dirty_pages(zms) > zms->dirty_hard_pages)
			return ret;
	}

	ret = zms_prepare_store_handle(zms, handle);
	if (ret)
		return ret;

	mutex_lock(&class->lock);
	ret = zms_store_locked(zms, class, handle, src, size,
			       gfp, io, NULL);
	mutex_unlock(&class->lock);
	if (!ret)
		zms_kick_flush(zms);

	return ret;
}

int zms_store_batch(struct zms *zms, struct zms_store_item *items,
		    unsigned int nr, gfp_t gfp, struct zms_io *io)
{
	struct zms_class *class;
	struct zms_block *batch_block = NULL;
	unsigned int stored = 0;
	unsigned int i;
	int first_err = 0;
	int ret;

	zms_io_clear(io);
	if (!zms || !items || !nr)
		return -EINVAL;

	class = zms_class_for_size(zms, items[0].size);
	if (!class)
		return -EINVAL;
	for (i = 0; i < nr; i++) {
		items[i].ret = 0;
		if (!items[i].src || !items[i].size ||
		    items[i].size > PAGE_SIZE || !items[i].handle ||
		    items[i].handle > zms->nr_handles ||
		    zms_class_for_size(zms, items[i].size) != class) {
			first_err = -EINVAL;
			break;
		}
	}
	if (first_err) {
		for (i = 0; i < nr; i++)
			items[i].ret = first_err;
		return first_err;
	}

	if (zms_dirty_pages(zms) > zms->dirty_hard_pages) {
		ret = zms_flush_to(zms, zms->dirty_high_pages, gfp, io);
		if (ret && ret != -EAGAIN)
			goto fail_all;
		if (ret == -EAGAIN &&
		    zms_dirty_pages(zms) > zms->dirty_hard_pages)
			goto fail_all;
	}

	for (i = 0; i < nr; i++) {
		items[i].ret = zms_prepare_store_handle(zms, items[i].handle);
		if (items[i].ret && !first_err)
			first_err = items[i].ret;
	}

	mutex_lock(&class->lock);
	for (i = 0; i < nr; i++) {
		if (items[i].ret)
			continue;
		items[i].ret = zms_store_locked(zms, class, items[i].handle,
						items[i].src, items[i].size,
						gfp, io, &batch_block);
		if (!items[i].ret) {
			stored++;
			continue;
		}
		if (!first_err)
			first_err = items[i].ret;
		if (items[i].ret == -ENOSPC) {
			unsigned int j;

			for (j = i + 1; j < nr; j++) {
				if (!items[j].ret)
					items[j].ret = -ENOSPC;
			}
			break;
		}
	}
	if (batch_block)
		zms_block_unfreeze(zms, batch_block);
	mutex_unlock(&class->lock);

	atomic_long_inc(&zms->store_batch_calls);
	atomic_long_add(stored, &zms->store_batch_items);
	if (stored)
		zms_kick_flush(zms);
	return first_err;

fail_all:
	for (i = 0; i < nr; i++)
		items[i].ret = ret;

	return ret;
}

int zms_load(struct zms *zms, unsigned long handle, void *dst, size_t *size,
	     gfp_t gfp, struct zms_io *io)
{
	struct zms_load_ref ref;
	int ret;

	if (!dst || !size) {
		zms_io_clear(io);
		return -EINVAL;
	}

	ret = zms_load_ref(zms, handle, &ref, gfp, io);
	if (ret)
		return ret;

	memcpy(dst, ref.data, ref.size);
	*size = ref.size;
	zms_put_ref(zms, &ref);
	return 0;
}

static void zms_put_block_ref(struct zms *zms, struct zms_block *block)
{
	zms_block_ref_put(zms, block);
}

static int zms_try_load_cached_ref(struct zms *zms, unsigned long handle,
				   const struct zms_object_cookie *cookie,
				   struct zms_load_ref *ref,
				   bool account_load, bool touch_cache)
{
	struct zms_handle_entry *entry;
	struct zms_block *block = NULL;
	unsigned long flags;
	void *data;
	bool clean_cached = false;
	bool put = false;
	int ret = 0;

	if (!handle || handle > zms->nr_handles)
		return -EINVAL;

	spin_lock_irqsave(zms_handle_lock(zms, handle), flags);
	entry = &zms->handles[handle];
	if (!zms_handle_active(entry)) {
		ret = -ENOENT;
		goto out;
	}
	if (cookie && entry->generation != cookie->generation) {
		ret = -ESTALE;
		goto out;
	}
	block = entry->block;
	if (!zms_block_ref_tryget(block)) {
		ret = -EAGAIN;
		goto out;
	}
	data = zms_block_data_load(block);
	if (!data) {
		put = true;
		if (account_load)
			atomic_long_inc(&zms->load_disk_misses);
		ret = -EAGAIN;
		goto out;
	}
	clean_cached = READ_ONCE(block->clean_cached);

	ref->data = (char *)data + entry->offset;
	ref->size = entry->size;
	ref->private = block;
	ref->cookie.handle = handle;
	ref->cookie.generation = entry->generation;
	if (account_load)
		atomic_long_inc(&zms->load_resident_hits);
out:
	spin_unlock_irqrestore(zms_handle_lock(zms, handle), flags);
	if (!ret && clean_cached && account_load)
		atomic_long_inc(&zms->clean_cache_demand_hits);
	if (!ret && clean_cached && touch_cache)
		zms_clean_cache_touch(block);
	if (put)
		zms_put_block_ref(zms, block);
	return ret;
}

/*
 * Cache-only load: succeed iff the block data is already in RAM and not being
 * read from disk.  Never calls submit_bio, so it is safe to invoke directly
 * from the zram submit_bio path without the read workqueue indirection.
 *
 * Returns 0 on a cache hit (ref filled, block pinned), -EAGAIN when the data
 * needs disk I/O (caller must fall back to zms_load_ref via the workqueue),
 * -ENOENT for a pending-free / invalid handle, or -EINVAL on bad arguments.
 */
int zms_load_cached_ref(struct zms *zms, unsigned long handle,
			struct zms_load_ref *ref, struct zms_io *io)
{
	zms_io_clear(io);
	if (!zms || !ref)
		return -EINVAL;
	memset(ref, 0, sizeof(*ref));

	return zms_try_load_cached_ref(zms, handle, NULL, ref, true, true);
}

int zms_load_prefetch_ref(struct zms *zms,
			  const struct zms_object_cookie *cookie,
			  struct zms_load_ref *ref)
{
	if (!zms || !cookie || !ref || !cookie->handle)
		return -EINVAL;
	memset(ref, 0, sizeof(*ref));

	return zms_try_load_cached_ref(zms, cookie->handle, cookie, ref,
				       false, false);
}

bool zms_cookie_matches(struct zms *zms,
			const struct zms_object_cookie *cookie)
{
	struct zms_handle_entry *entry;
	unsigned long flags;
	bool match;

	if (!zms || !cookie || !cookie->handle ||
	    cookie->handle > zms->nr_handles)
		return false;

	spin_lock_irqsave(zms_handle_lock(zms, cookie->handle), flags);
	entry = &zms->handles[cookie->handle];
	match = zms_handle_active(entry) &&
		entry->generation == cookie->generation;
	spin_unlock_irqrestore(zms_handle_lock(zms, cookie->handle), flags);
	return match;
}

int zms_load_ref(struct zms *zms, unsigned long handle, struct zms_load_ref *ref,
			 gfp_t gfp, struct zms_io *io)
{
	struct zms_load_snapshot snapshot;
	struct zms_class *class;
	struct zms_block *block;
	void *data;
	bool owner = false;
	bool shrink_clean_cache = false;
	int ret;

	zms_io_clear(io);
	if (!zms || !ref)
		return -EINVAL;
	memset(ref, 0, sizeof(*ref));

	ret = zms_try_load_cached_ref(zms, handle, NULL, ref, false, true);
	if (!ret || ret != -EAGAIN)
		return ret;

retry:
	ret = zms_get_handle_ref(zms, handle, &snapshot);
	if (ret == -EAGAIN) {
		cond_resched();
		goto retry;
	}
	if (ret)
		return ret;

	block = snapshot.block;
	class = zms_class_for_size(zms, block->class_size);
	if (WARN_ON_ONCE(!class)) {
		zms_block_ref_put(zms, block);
		return -EIO;
	}

	mutex_lock(&class->lock);
	if (!block->reading) {
		data = zms_block_data_load(block);
		if (data) {
			if (!zms_handle_matches_snapshot(zms, handle, &snapshot)) {
				zms_block_ref_put(zms, block);
				mutex_unlock(&class->lock);
				return -ENOENT;
			}
			ref->data = (char *)data + snapshot.offset;
			ref->size = snapshot.size;
			ref->private = block;
			ref->cookie.handle = handle;
			ref->cookie.generation = snapshot.generation;
			mutex_unlock(&class->lock);
			return 0;
		}
	}

	if (block->reading) {
		atomic_long_inc(&zms->read_merge_waits);

		zms_wait_read_done(class, block);
		atomic_long_inc(&zms->read_merge_wakeups);
		ret = block->read_ret;
		if (ret) {
			atomic_long_inc(&zms->read_merge_failures);
			zms_block_ref_put(zms, block);
			mutex_unlock(&class->lock);
			return ret;
		}

		data = zms_block_data_load(block);
		if (!data ||
		    !zms_handle_matches_snapshot(zms, handle, &snapshot)) {
			atomic_long_inc(&zms->read_merge_mismatch);
			zms_block_ref_put(zms, block);
			mutex_unlock(&class->lock);
			return -ENOENT;
		}

		atomic_long_inc(&zms->read_merge_hits);
		ref->data = (char *)data + snapshot.offset;
		ref->size = snapshot.size;
		ref->private = block;
		ref->cookie.handle = handle;
		ref->cookie.generation = snapshot.generation;
		mutex_unlock(&class->lock);
		return 0;
	}

	block->reading = true;
	block->read_ret = 0;
	owner = true;
	mutex_unlock(&class->lock);

	ret = zms_read_block(zms, block, gfp, io, true);
	mutex_lock(&class->lock);
	block->read_ret = ret;
	block->reading = false;
	wake_up_all(&block->read_wait);

	if (!ret) {
		data = zms_block_data_load(block);
		if (!zms_handle_matches_snapshot(zms, handle, &snapshot)) {
			atomic_long_inc(&zms->read_merge_mismatch);
			zms_release_block_data_locked(zms, block);
			ret = -ENOENT;
			goto out_unpin;
		}
		if (WARN_ON_ONCE(!data)) {
			ret = -EIO;
			goto out_unpin;
		}

		ref->data = (char *)data + snapshot.offset;
		ref->size = snapshot.size;
		ref->private = block;
		ref->cookie.handle = handle;
		ref->cookie.generation = snapshot.generation;
		shrink_clean_cache = zms_clean_cache_add(zms, block);
		owner = false;
	}

out_unpin:
	if (owner)
		zms_block_ref_put(zms, block);
	mutex_unlock(&class->lock);

	if (ret == -EBUSY)
		goto retry;
	if (!ret && shrink_clean_cache)
		zms_clean_cache_shrink(zms, zms->clean_cache_max_pages);

	return ret;
}

void zms_put_ref(struct zms *zms, struct zms_load_ref *ref)
{
	struct zms_block *block;

	if (!zms || !ref || !ref->private)
		return;

	block = ref->private;
	zms_put_block_ref(zms, block);
	memset(ref, 0, sizeof(*ref));
}

int zms_load_batch(struct zms *zms, struct zms_load_item *items,
		   unsigned int nr, gfp_t gfp, struct zms_io *io)
{
	unsigned int i;

	zms_io_clear(io);
	if (!zms || !items || !nr)
		return -EINVAL;

	for (i = 0; i < nr; i++) {
		struct zms_io item_io;
		size_t size = 0;

		items[i].loaded_size = 0;
		items[i].ret = 0;
		if (!items[i].handle || !items[i].dst ||
		    !items[i].expected_size || items[i].expected_size > PAGE_SIZE) {
			items[i].ret = -EINVAL;
			continue;
		}
		items[i].ret = zms_load(zms, items[i].handle, items[i].dst,
					&size, gfp, &item_io);
		zms_io_accumulate(io, &item_io);
		if (items[i].ret)
			continue;
		if (size != items[i].expected_size) {
			items[i].ret = -EIO;
			items[i].loaded_size = 0;
			continue;
		}
		items[i].loaded_size = size;
	}

	return 0;
}

int zms_peek_ref_neighbors(struct zms *zms,
			   const struct zms_load_ref *source,
			   struct zms_object_cookie *objects,
			   unsigned int max_objects,
			   struct zms_prefetch_info *info)
{
	struct zms_block *block;
	unsigned int found = 0;
	unsigned int safe = 0;
	unsigned int slot;

	if (!zms || !source || !source->private || !source->cookie.handle ||
	    !objects || !max_objects || !info)
		return -EINVAL;
	memset(info, 0, sizeof(*info));

	block = source->private;
	if (!zms_cookie_matches(zms, &source->cookie) ||
	    !zms_block_data_load(block) || block->dirty ||
	    block->fullness < ZMS_FG_ALMOST_FULL)
		return 0;

	info->source_pages = block->pages;
	info->source_used = block->used;
	info->source_slots = block->slots;
	while (safe < max_objects && safe < block->used - 1) {
		unsigned int remaining = block->used - safe - 1;

		if (remaining * 100U / block->slots <= 66U)
			break;
		safe++;
	}
	info->safe_promotions = safe;
	if (!safe)
		return 0;

	for (slot = 0; slot < block->slots && found < max_objects; slot++) {
		unsigned long neighbor = block->slot_handles[slot];

		if (!neighbor || neighbor == source->cookie.handle)
			continue;
		if (neighbor > zms->nr_handles)
			continue;
		if (!zms_get_slot_cookie(zms, neighbor, block, slot,
					 &objects[found]))
			continue;

		found++;
	}

	return found;
}

void zms_free(struct zms *zms, unsigned long handle)
{
	struct zms_handle_entry *entry;
	unsigned long flags;
	bool queued = false;

	if (!zms)
		return;
	if (!handle || handle > zms->nr_handles)
		return;

	spin_lock_irqsave(&zms->pending_lock, flags);
	if (zms->destroying) {
		spin_unlock_irqrestore(&zms->pending_lock, flags);
		zms_handle_clear_pending_flag(zms, handle);
		return;
	}
	spin_unlock_irqrestore(&zms->pending_lock, flags);

	spin_lock_irqsave(zms_handle_lock(zms, handle), flags);
	entry = &zms->handles[handle];
	if (!entry->block) {
		spin_unlock_irqrestore(zms_handle_lock(zms, handle), flags);
		return;
	}
	entry->flags |= ZMS_HANDLE_PENDING_FREE;
	spin_unlock_irqrestore(zms_handle_lock(zms, handle), flags);

	spin_lock_irqsave(&zms->pending_lock, flags);
	if (zms->destroying) {
		spin_unlock_irqrestore(&zms->pending_lock, flags);
		zms_handle_clear_pending_flag(zms, handle);
		return;
	}
	if (!test_and_set_bit(handle, zms->pending_free)) {
		zms_stat_add(zms, ZMS_STAT_PENDING_FREE, 1);
		queued = true;
	}
	spin_unlock_irqrestore(&zms->pending_lock, flags);
	if (queued)
		schedule_work(&zms->free_work);
}

static int zms_flush_to(struct zms *zms, unsigned long target_pages,
			gfp_t gfp, struct zms_io *last_io)
{
	unsigned int retries = 0;
	unsigned int i;
	bool saw_pinned;
	int ret = 0;

	zms_io_clear(last_io);
	if (!zms)
		return -EINVAL;

	zms_reclaim_pending_all(zms);

retry:
	saw_pinned = false;
	for (i = 0; i < ARRAY_SIZE(zms->classes); i++) {
		if (target_pages && zms_dirty_pages(zms) <= target_pages)
			return 0;
		mutex_lock(&zms->classes[i].lock);
		ret = zms_flush_class_to_locked(zms, &zms->classes[i],
						target_pages, gfp, last_io);
		mutex_unlock(&zms->classes[i].lock);
		if (ret == -EAGAIN) {
			saw_pinned = true;
			ret = 0;
			continue;
		}
		if (ret)
			return ret;
	}
	if (target_pages && zms_dirty_pages(zms) <= target_pages)
		return 0;
	if (saw_pinned) {
		if (retries++ < ZMS_FLUSH_PINNED_RETRY_MAX) {
			schedule_timeout_uninterruptible(1);
			goto retry;
		}
		return -EAGAIN;
	}
	return ret;
}

int zms_flush_all(struct zms *zms, gfp_t gfp, struct zms_io *last_io)
{
	if (!zms) {
		zms_io_clear(last_io);
		return -EINVAL;
	}

	flush_delayed_work(&zms->flush_work);
	return zms_flush_to(zms, 0, gfp, last_io);
}

int zms_compact(struct zms *zms, gfp_t gfp, struct zms_io *io)
{
	unsigned int i;
	int ret = 0;
	u64 start_time_ms;

	zms_io_clear(io);
	if (!zms)
		return -EINVAL;

	zms_reclaim_pending_all(zms);
	start_time_ms = ktime_to_ms(ktime_get_boottime());

	for (i = 0; i < ARRAY_SIZE(zms->classes); i++) {
		mutex_lock(&zms->classes[i].lock);
		ret = zms_compact_class_locked(zms, &zms->classes[i], gfp, io,
					       start_time_ms);
		mutex_unlock(&zms->classes[i].lock);
		if (ret)
			return ret;
	}
	return ret;
}

MODULE_AUTHOR("whitewhale");
MODULE_DESCRIPTION("Crystal ZMS packed compressed-object backing store");
MODULE_LICENSE("GPL");
