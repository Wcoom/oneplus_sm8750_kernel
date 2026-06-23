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
#include <linux/bitmap.h>
#include <linux/blkdev.h>
#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/percpu_counter.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/vmalloc.h>

#include "zms.h"

#define ZMS_ALIGN_SHIFT		4
#define ZMS_ALIGN		(1U << ZMS_ALIGN_SHIFT)
#define ZMS_MIN_SIZE		ZMS_ALIGN
#define ZMS_CLASS_SIZE		ZMS_ALIGN
#define ZMS_MAX_CLASSES		(PAGE_SIZE / ZMS_CLASS_SIZE)
#define ZMS_BLOCK_RESERVED	1UL
#define ZMS_RECLAIM_BEFORE_ALLOC_MAX	64U
#define ZMS_READ_KEEPALIVE_MS		125ULL
#define ZMS_ACTIVE_AFFINITY_SLOTS	4U
#define ZMS_REPAIR_ON_FREE_MAX_MOVES	8U
#define ZMS_STAT_COUNTER_BATCH		32

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
	ZMS_STAT_CACHED_BLOCKS,
	ZMS_NR_STAT_COUNTERS,
};

struct zms_affinity_tag {
	u64 memcg_id;
	u16 size_class;
	bool valid;
	bool mixed;
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
	bool listed;
	bool cached;
	bool deferred_free;
	bool reading;
	bool drop_data;
	u64 cache_expires_ms;
	int read_ret;
	unsigned int pins;
	struct zms_affinity_tag affinity;
	struct list_head list;
	struct list_head cache_list;
	wait_queue_head_t read_wait;
	unsigned long blocks[];
};

struct zms_active_affinity {
	struct zms_block *block;
	struct zms_affinity_tag tag;
};

struct zms_class {
	unsigned int size;
	unsigned int pages_per_zspage;
	unsigned int slots_per_zspage;
	unsigned int listed_blocks;
	struct list_head fullness[ZMS_NR_FULLNESS];
	struct zms_active_affinity active[ZMS_ACTIVE_AFFINITY_SLOTS];
};

struct zms_handle_entry {
	struct zms_block *block;
	unsigned int offset;
	unsigned int size;
	unsigned int slot;
};

struct zms_load_snapshot {
	struct zms_block *block;
	unsigned int offset;
	unsigned int size;
	unsigned int slot;
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
	unsigned long alloc_blocks;
	unsigned long alloc_run_successes;
	unsigned long alloc_run_failures;
	unsigned long reclaim_before_alloc_calls;
	unsigned long reclaim_before_alloc_handles;
	unsigned long cache_hits;
	unsigned long cache_misses;
	unsigned long cache_expired;
	unsigned long cache_keep_ms;
	unsigned long read_merge_waits;
	unsigned long read_merge_wakeups;
	unsigned long read_merge_hits;
	unsigned long read_merge_mismatch;
	unsigned long read_merge_failures;
	unsigned long repair_on_free_calls;
	unsigned long repair_on_free_moves;
	unsigned long repair_on_free_source_frees;
	unsigned long repair_on_free_skips;
	unsigned long affinity_exact_hits;
	unsigned long affinity_memcg_hits;
	unsigned long affinity_active_hits;
	unsigned long affinity_active_misses;
	unsigned long affinity_fallbacks;
	unsigned long affinity_mixed_blocks;
	unsigned long valid_classes;
	spinlock_t pending_lock;
	struct mutex lock;
	struct work_struct free_work;
	struct delayed_work cache_prune_work;
	struct list_head cache_blocks;
	bool destroying;
	bool statcounters_ready;
};

static unsigned int zms_size_to_class(size_t size)
{
	size_t aligned = ALIGN(max_t(size_t, size, ZMS_MIN_SIZE), ZMS_ALIGN);

	return (aligned / ZMS_CLASS_SIZE) - 1;
}

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

static unsigned int zms_block_bytes(const struct zms_block *block)
{
	return block->pages << PAGE_SHIFT;
}

static void zms_pin_block_locked(struct zms_block *block)
{
	block->pins++;
}

static bool zms_block_pinned(const struct zms_block *block)
{
	return block->pins != 0;
}

static bool zms_block_clean_full(const struct zms_block *block)
{
	return block->data && !block->dirty && block->used == block->slots;
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

static void zms_fullness_stats_add(struct zms *zms, enum zms_fullness fullness,
				   s64 pages);
static void zms_clear_dirty_locked(struct zms *zms, struct zms_block *block);

static bool zms_hint_valid(const struct zms_write_hint *hint)
{
	return hint && hint->size_class;
}

static void zms_affinity_tag_clear(struct zms_affinity_tag *tag)
{
	if (tag)
		memset(tag, 0, sizeof(*tag));
}

static bool zms_affinity_tag_equal(const struct zms_affinity_tag *tag,
				   const struct zms_write_hint *hint)
{
	if (!tag || !tag->valid || !zms_hint_valid(hint))
		return false;

	return tag->memcg_id == hint->memcg_id &&
	       tag->size_class == hint->size_class;
}

static bool zms_affinity_tag_memcg_match(const struct zms_affinity_tag *tag,
					 const struct zms_write_hint *hint)
{
	if (!tag || !tag->valid || !zms_hint_valid(hint))
		return false;
	if (!tag->memcg_id || !hint->memcg_id)
		return false;

	return tag->memcg_id == hint->memcg_id &&
	       tag->size_class == hint->size_class;
}

static void zms_block_update_affinity_locked(struct zms *zms,
					     struct zms_block *block,
					     const struct zms_write_hint *hint)
{
	if (!block)
		return;

	if (!block->used) {
		zms_affinity_tag_clear(&block->affinity);
		return;
	}

	if (!zms_hint_valid(hint)) {
		if (block->affinity.valid && !block->affinity.mixed)
			zms->affinity_mixed_blocks++;
		block->affinity.valid = false;
		block->affinity.mixed = true;
		return;
	}

	if (!block->affinity.valid && !block->affinity.mixed) {
		block->affinity.memcg_id = hint->memcg_id;
		block->affinity.size_class = hint->size_class;
		block->affinity.valid = true;
		return;
	}

	if (block->affinity.valid &&
	    zms_affinity_tag_equal(&block->affinity, hint))
		return;

	if (block->affinity.valid && !block->affinity.mixed)
		zms->affinity_mixed_blocks++;
	block->affinity.valid = false;
	block->affinity.mixed = true;
}

static void zms_class_drop_active_block_locked(struct zms_class *class,
					       struct zms_block *block)
{
	unsigned int i;

	if (!class || !block)
		return;

	for (i = 0; i < ARRAY_SIZE(class->active); i++) {
		if (class->active[i].block == block) {
			class->active[i].block = NULL;
			zms_affinity_tag_clear(&class->active[i].tag);
		}
	}
}

static void zms_class_promote_active_block_locked(struct zms_class *class,
						  struct zms_block *block)
{
	unsigned int i;

	if (!class || !block)
		return;

	for (i = 0; i < ARRAY_SIZE(class->active); i++) {
		if (class->active[i].block == block) {
			struct zms_active_affinity hit = class->active[i];

			while (i > 0) {
				class->active[i] = class->active[i - 1];
				i--;
			}
			class->active[0] = hit;
			return;
		}
	}

	for (i = ARRAY_SIZE(class->active) - 1; i > 0; i--)
		class->active[i] = class->active[i - 1];
	class->active[0].block = block;
	class->active[0].tag = block->affinity;
}

static u64 zms_now_ms(void)
{
	return ktime_to_ms(ktime_get_boottime());
}

static void zms_cache_remove_locked(struct zms *zms, struct zms_block *block)
{
	if (!block->cached)
		return;

	list_del_init(&block->cache_list);
	block->cached = false;
	block->cache_expires_ms = 0;
	zms_stat_add(zms, ZMS_STAT_CACHED_BLOCKS, -(s64)block->pages);
}

static void zms_cache_release_data_locked(struct zms *zms, struct zms_block *block)
{
	zms_cache_remove_locked(zms, block);
	if (zms_block_pinned(block)) {
		block->drop_data = true;
		return;
	}

	if (block->data) {
		kvfree(block->data);
		block->data = NULL;
	}
	block->drop_data = false;
}

static void zms_cache_forget_block_locked(struct zms *zms,
					  struct zms_block *block)
{
	zms_cache_remove_locked(zms, block);
}

static void zms_cache_keep_locked(struct zms *zms, struct zms_block *block)
{
	if (!zms_block_clean_full(block))
		return;

	if (!block->cached) {
		list_add_tail(&block->cache_list, &zms->cache_blocks);
		zms_stat_add(zms, ZMS_STAT_CACHED_BLOCKS, block->pages);
	} else {
		list_move_tail(&block->cache_list, &zms->cache_blocks);
	}
	block->cached = true;
	block->cache_expires_ms = zms_now_ms() + ZMS_READ_KEEPALIVE_MS;
	zms->cache_keep_ms = ZMS_READ_KEEPALIVE_MS;
}

static bool zms_cache_expired_locked(struct zms_block *block, u64 now_ms)
{
	return block->cached && block->cache_expires_ms &&
	       now_ms >= block->cache_expires_ms;
}

static void zms_cache_prune_locked(struct zms *zms, struct zms_block *block,
				   u64 now_ms)
{
	if (!zms_cache_expired_locked(block, now_ms))
		return;

	zms->cache_expired++;
	if (zms_block_pinned(block)) {
		zms_cache_remove_locked(zms, block);
		block->drop_data = true;
		return;
	}

	if (!block->dirty && block->used == block->slots) {
		zms_cache_release_data_locked(zms, block);
		return;
	}

	zms_cache_remove_locked(zms, block);
}

static void zms_cache_touch_locked(struct zms *zms, struct zms_block *block)
{
	u64 now_ms;

	if (!block->cached)
		return;

	now_ms = zms_now_ms();
	zms_cache_prune_locked(zms, block, now_ms);
	if (!block->cached || !block->data)
		return;

	zms->cache_hits++;
	block->cache_expires_ms = now_ms + ZMS_READ_KEEPALIVE_MS;
}

static void zms_cache_miss_locked(struct zms *zms, const struct zms_block *block)
{
	if (!block->data && block->used == block->slots && !block->dirty)
		zms->cache_misses++;
}

static void zms_unpin_block_locked(struct zms *zms, struct zms_block *block)
{
	bool schedule_free;

	if (WARN_ON_ONCE(!block->pins))
		return;

	block->pins--;
	if (!block->pins && block->drop_data && !block->cached) {
		kvfree(block->data);
		block->data = NULL;
		block->drop_data = false;
	}
	schedule_free = !block->pins && block->deferred_free;
	block->deferred_free = false;
	if (schedule_free)
		schedule_work(&zms->free_work);
}

static void zms_wait_read_done(struct zms *zms, struct zms_block *block)
{
	DEFINE_WAIT(wait);

	for (;;) {
		prepare_to_wait(&block->read_wait, &wait, TASK_UNINTERRUPTIBLE);
		if (!block->reading)
			break;
		mutex_unlock(&zms->lock);
		schedule();
		mutex_lock(&zms->lock);
	}
	finish_wait(&block->read_wait, &wait);
}

static void zms_io_record(struct zms_io *io, unsigned int op,
			  unsigned long block,
			  unsigned int pages, u64 latency_ns, int ret)
{
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

static unsigned int zms_contiguous_run(const struct zms_block *block,
				       unsigned int start)
{
	unsigned int run = 1;

	while (start + run < block->pages &&
	       block->blocks[start + run] == block->blocks[start] + run)
		run++;

	return run;
}

static int zms_submit_run(struct zms *zms, struct zms_block *block,
			  unsigned int page_idx, unsigned int nr_pages,
			  unsigned int op, gfp_t gfp, struct zms_io *io)
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
			void *data = (char *)block->data +
				(cur << PAGE_SHIFT);
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
		zms_io_record(io, op, block->blocks[page_idx + done],
			      submitted, ktime_get_ns() - start, ret);
		bio_uninit(&bio);
		if (ret)
			return ret;
		done += submitted;
	}

	return 0;
}

static int zms_submit_block(struct zms *zms, struct zms_block *block,
			    unsigned int op, gfp_t gfp, struct zms_io *io)
{
	unsigned int i;
	int ret;

	for (i = 0; i < block->pages; ) {
		unsigned int run = zms_contiguous_run(block, i);

		ret = zms_submit_run(zms, block, i, run, op, gfp, io);
		if (ret)
			return ret;
		i += run;
	}

	return 0;
}

static int zms_write_block(struct zms *zms, struct zms_block *block,
			   gfp_t gfp, struct zms_io *io)
{
	int ret;

	if (WARN_ON_ONCE(!block->data))
		return -EIO;

	ret = zms_submit_block(zms, block, REQ_OP_WRITE, gfp, io);
	if (!ret) {
		zms_clear_dirty_locked(zms, block);
		zms_cache_release_data_locked(zms, block);
	}
	return ret;
}

static int zms_flush_class_locked(struct zms *zms, struct zms_class *class,
				  gfp_t gfp, struct zms_io *io)
{
	enum zms_fullness fullness;

	for (fullness = ZMS_FG_LOW; fullness < ZMS_NR_FULLNESS; fullness++) {
		struct zms_block *block;

		list_for_each_entry(block, &class->fullness[fullness], list) {
			int ret;

			if (!block->dirty)
				continue;
			ret = zms_write_block(zms, block, gfp, io);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int zms_read_block(struct zms *zms, struct zms_block *block,
			  gfp_t gfp, struct zms_io *io)
{
	bool allocated = false;
	int ret;

	if (!block->data) {
		block->data = kvzalloc(zms_block_bytes(block), gfp);
		if (!block->data)
			return -ENOMEM;
		allocated = true;
	}

	ret = zms_submit_block(zms, block, REQ_OP_READ, gfp, io);
	if (ret && allocated) {
		kvfree(block->data);
		block->data = NULL;
	}
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

static void zms_free_disk_block_locked(struct zms *zms, unsigned long block)
{
	if (WARN_ON_ONCE(block < ZMS_BLOCK_RESERVED || block >= zms->nr_blocks))
		return;

	WARN_ON_ONCE(!test_and_clear_bit(block, zms->block_bitmap));
	zms_stat_add(zms, ZMS_STAT_USED_BLOCKS, -1);
}

static void zms_clear_dirty_locked(struct zms *zms, struct zms_block *block)
{
	if (!block || !block->dirty)
		return;

	zms_stat_add(zms, ZMS_STAT_DIRTY_BLOCKS, -(s64)block->pages);
	block->dirty = false;
}

static void zms_free_block(struct zms *zms, struct zms_block *block)
{
	struct zms_class *class;

	if (!block)
		return;

	class = zms_class_for_size(zms, block->class_size);
	if (class)
		zms_class_drop_active_block_locked(class, block);
	zms_clear_dirty_locked(zms, block);
	zms_cache_forget_block_locked(zms, block);
	kvfree(block->bitmap);
	kvfree(block->slot_handles);
	if (block->data)
		kvfree(block->data);
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
		zms->valid_classes++;
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
		WARN_ON_ONCE(!zms->valid_classes);
		if (zms->valid_classes)
			zms->valid_classes--;
	}
}

static void zms_fix_fullness_locked(struct zms *zms, struct zms_class *class,
				    struct zms_block *block)
{
	enum zms_fullness fullness;

	if (!block->used)
		return;
	if (WARN_ON_ONCE(!block->listed))
		return;

	fullness = zms_block_fullness(block);
	if (fullness == block->fullness)
		return;

	zms_fullness_stats_add(zms, block->fullness, -(s64)block->pages);
	list_move_tail(&block->list, &class->fullness[fullness]);
	block->fullness = fullness;
	zms_fullness_stats_add(zms, block->fullness, block->pages);
}

static struct zms_block *zms_find_available_block_locked(struct zms_class *class)
{
	int fullness;

	for (fullness = ZMS_FG_ALMOST_FULL; fullness >= ZMS_FG_LOW; fullness--) {
		struct zms_block *block;

		list_for_each_entry(block, &class->fullness[fullness], list) {
			if (!zms_block_pinned(block))
				return block;
		}
		if (fullness == ZMS_FG_LOW)
			break;
	}

	return NULL;
}

static struct zms_block *zms_find_active_affinity_block_locked(struct zms *zms,
						       struct zms_class *class,
						       const struct zms_write_hint *hint)
{
	struct zms_block *memcg_match = NULL;
	unsigned int i;

	if (!zms_hint_valid(hint))
		return NULL;

	for (i = 0; i < ARRAY_SIZE(class->active); i++) {
		struct zms_active_affinity *active = &class->active[i];
		struct zms_block *block = active->block;

		if (!block)
			continue;
		if (!block->listed || block->fullness == ZMS_FG_FULL ||
		    zms_block_pinned(block)) {
			active->block = NULL;
			zms_affinity_tag_clear(&active->tag);
			continue;
		}
		active->tag = block->affinity;
		if (zms_affinity_tag_equal(&active->tag, hint)) {
			zms->affinity_exact_hits++;
			zms->affinity_active_hits++;
			zms_class_promote_active_block_locked(class, block);
			return block;
		}
		if (!memcg_match &&
		    zms_affinity_tag_memcg_match(&active->tag, hint))
			memcg_match = block;
	}

	if (memcg_match) {
		zms->affinity_memcg_hits++;
		zms->affinity_active_hits++;
		zms_class_promote_active_block_locked(class, memcg_match);
		return memcg_match;
	}

	zms->affinity_active_misses++;
	return NULL;
}

static struct zms_block *zms_alloc_block_locked(struct zms *zms,
						struct zms_class *class,
						gfp_t gfp)
{
	struct zms_block *block;
	size_t bitmap_size;
	unsigned int i;

	block = kzalloc(struct_size(block, blocks, class->pages_per_zspage), gfp);
	if (!block)
		return NULL;

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

	block->data = kvzalloc(zms_block_bytes(block), gfp);
	if (!block->data)
		goto err_block;

	if (zms_alloc_disk_run_locked(zms, block->blocks, block->pages)) {
		if (block->pages > 1)
			zms->alloc_run_successes++;
		i = block->pages;
	} else {
		if (block->pages > 1)
			zms->alloc_run_failures++;
		for (i = 0; i < block->pages; i++) {
			block->blocks[i] = zms_alloc_disk_block_locked(zms);
			if (!block->blocks[i])
				goto err_allocated_blocks;
		}
	}
	zms->alloc_blocks++;

	block->class_size = class->size;
	block->fullness = ZMS_FG_LOW;
	block->cache_expires_ms = 0;
	INIT_LIST_HEAD(&block->list);
	INIT_LIST_HEAD(&block->cache_list);
	init_waitqueue_head(&block->read_wait);
	return block;

err_allocated_blocks:
	while (i > 0) {
		i--;
		zms_free_disk_block_locked(zms, block->blocks[i]);
	}
err_block:
	zms_free_block(zms, block);
	return NULL;
}

static unsigned long zms_alloc_run_attempts(const struct zms *zms)
{
	return zms->alloc_run_successes + zms->alloc_run_failures;
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

static void zms_free_handle_locked(struct zms *zms, unsigned long handle);
static bool zms_handle_pending_locked(struct zms *zms, unsigned long handle);
static unsigned long zms_handle_for_entry_locked(struct zms *zms,
						 struct zms_block *block,
						 unsigned long slot);
static int zms_move_slot_locked(struct zms *zms, struct zms_class *class,
				struct zms_block *source,
				struct zms_block *target,
				unsigned long src_slot, gfp_t gfp,
				struct zms_io *io);

static void zms_free_empty_block_locked(struct zms *zms, struct zms_class *class,
					struct zms_block *block)
{
	unsigned int i;

	if (WARN_ON_ONCE(!block || block->used))
		return;

	zms_affinity_tag_clear(&block->affinity);
	zms_remove_block_locked(zms, class, block);
	for (i = 0; i < block->pages; i++)
		zms_free_disk_block_locked(zms, block->blocks[i]);
	zms_free_block(zms, block);
}

static bool zms_repair_on_free_candidate_locked(struct zms_block *block)
{
	if (!block || !block->used || !block->listed || !block->data)
		return false;
	if (zms_block_pinned(block))
		return false;
	if (block->fullness == ZMS_FG_LOW)
		return true;

	return block->used <= ZMS_REPAIR_ON_FREE_MAX_MOVES;
}

static bool zms_repair_target_usable_locked(const struct zms_block *target,
					    const struct zms_block *source)
{
	return target && target != source && target->listed &&
	       target->used < target->slots && target->data &&
	       !zms_block_pinned(target);
}

static struct zms_block *zms_repair_target_locked(struct zms_class *class,
						  struct zms_block *source)
{
	int fullness;

	for (fullness = ZMS_FG_ALMOST_FULL; fullness >= ZMS_FG_LOW; fullness--) {
		struct zms_block *block;

		list_for_each_entry(block, &class->fullness[fullness], list) {
			if (zms_repair_target_usable_locked(block, source))
				return block;
		}
		if (fullness == ZMS_FG_LOW)
			break;
	}

	return NULL;
}

static unsigned long zms_repair_source_slot_locked(struct zms *zms,
						   struct zms_block *source)
{
	unsigned long slot;

	for (slot = find_first_bit(source->bitmap, source->slots);
	     slot < source->slots;
	     slot = find_next_bit(source->bitmap, source->slots, slot + 1)) {
		unsigned long slot_handle;

		slot_handle = zms_handle_for_entry_locked(zms, source, slot);
		if (!slot_handle)
			continue;
		if (zms_handle_pending_locked(zms, slot_handle))
			continue;
		return slot;
	}

	return source->slots;
}

static void zms_repair_on_free_locked(struct zms *zms, struct zms_class *class,
				      struct zms_block *source)
{
	struct zms_block *target = NULL;
	unsigned int moved = 0;

	if (!zms_repair_on_free_candidate_locked(source))
		return;

	zms->repair_on_free_calls++;
	while (source->used && moved < ZMS_REPAIR_ON_FREE_MAX_MOVES) {
		unsigned long slot;
		int ret;

		if (!target || !zms_repair_target_usable_locked(target, source))
			target = zms_repair_target_locked(class, source);
		if (!target)
			break;

		slot = zms_repair_source_slot_locked(zms, source);
		if (slot >= source->slots)
			break;

		ret = zms_move_slot_locked(zms, class, source, target, slot,
					   GFP_NOIO, NULL);
		if (ret)
			break;

		moved++;
		zms->repair_on_free_moves++;
		if (target->used == target->slots)
			target = NULL;
	}

	if (!moved)
		zms->repair_on_free_skips++;
	if (!source->used) {
		zms->repair_on_free_source_frees++;
		zms_free_empty_block_locked(zms, class, source);
	}
}

static unsigned int zms_reclaim_pending_locked(struct zms *zms,
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
		__clear_bit(handle, zms->pending_free);
		zms_stat_add(zms, ZMS_STAT_PENDING_FREE, -1);
		spin_unlock_irqrestore(&zms->pending_lock, flags);

		zms_free_handle_locked(zms, handle);
		handle++;
		reclaimed++;
	}

	return reclaimed;
}

static struct zms_block *zms_find_block_locked(struct zms *zms,
					       struct zms_class *class,
					       const struct zms_write_hint *hint,
					       gfp_t gfp)
{
	struct zms_block *block;
	unsigned int reclaimed;
	bool affinity_missed = false;

	block = zms_find_active_affinity_block_locked(zms, class, hint);
	if (block)
		return block;
	affinity_missed = zms_hint_valid(hint);

	block = zms_find_available_block_locked(class);
	if (block) {
		if (affinity_missed)
			zms->affinity_fallbacks++;
		return block;
	}

	reclaimed = zms_reclaim_pending_locked(zms,
					       ZMS_RECLAIM_BEFORE_ALLOC_MAX);
	if (reclaimed) {
		zms->reclaim_before_alloc_calls++;
		zms->reclaim_before_alloc_handles += reclaimed;
		block = zms_find_active_affinity_block_locked(zms, class, hint);
		if (block)
			return block;
		block = zms_find_available_block_locked(class);
		if (block) {
			if (affinity_missed)
				zms->affinity_fallbacks++;
			return block;
		}
	}

	if (affinity_missed)
		zms->affinity_fallbacks++;
	block = zms_alloc_block_locked(zms, class, gfp);
	if (!block)
		return NULL;

	return block;
}

static int zms_prepare_writable_block(struct zms *zms, struct zms_block *block,
				      gfp_t gfp, struct zms_io *io)
{
	if (zms_block_pinned(block))
		return -EBUSY;

	if (block->data)
		return 0;

	return zms_read_block(zms, block, gfp, io);
}

static bool zms_handle_valid_locked(struct zms *zms, unsigned long handle)
{
	return handle > 0 && handle <= zms->nr_handles &&
		zms->handles[handle].block;
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

static bool zms_take_pending(struct zms *zms, unsigned long handle)
{
	bool pending;
	unsigned long flags;

	if (!handle || handle > zms->nr_handles)
		return false;

	spin_lock_irqsave(&zms->pending_lock, flags);
	pending = test_and_clear_bit(handle, zms->pending_free);
	if (pending)
		zms_stat_add(zms, ZMS_STAT_PENDING_FREE, -1);
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

static void zms_free_handle_locked(struct zms *zms, unsigned long handle)
{
	struct zms_handle_entry *entry;
	struct zms_class *class;
	struct zms_block *block;
	unsigned long flags;

	if (!zms_handle_valid_locked(zms, handle))
		return;

	entry = &zms->handles[handle];
	block = entry->block;
	class = zms_class_for_size(zms, block->class_size);
	if (WARN_ON_ONCE(!class)) {
		entry->block = NULL;
		return;
	}

	if (zms_block_pinned(block)) {
		spin_lock_irqsave(&zms->pending_lock, flags);
		if (!zms->destroying) {
			if (!test_and_set_bit(handle, zms->pending_free))
				zms_stat_add(zms, ZMS_STAT_PENDING_FREE, 1);
			block->deferred_free = true;
		}
		spin_unlock_irqrestore(&zms->pending_lock, flags);
		return;
	}

	__clear_bit(entry->slot, block->bitmap);
	block->slot_handles[entry->slot] = 0;
	zms_stat_add(zms, ZMS_STAT_OBJECTS, -1);
	zms_stat_add(zms, ZMS_STAT_STORED_BYTES, -(s64)entry->size);
	zms_stat_add(zms, ZMS_STAT_PACKED_BYTES, -(s64)class->size);
	entry->block = NULL;
	entry->offset = 0;
	entry->size = 0;
	entry->slot = 0;

	block->used--;

	if (!block->used) {
		zms_free_empty_block_locked(zms, class, block);
	} else {
		zms_cache_forget_block_locked(zms, block);
		zms_fix_fullness_locked(zms, class, block);
		zms_repair_on_free_locked(zms, class, block);
	}
}

static void zms_reclaim_pending(struct zms *zms)
{
	mutex_lock(&zms->lock);
	zms_reclaim_pending_locked(zms, 0);
	mutex_unlock(&zms->lock);
}

static void zms_cache_prune_workfn(struct work_struct *work)
{
	struct zms *zms = container_of(to_delayed_work(work), struct zms,
				       cache_prune_work);
	struct zms_block *block;
	struct zms_block *tmp;
	u64 now_ms;

	if (READ_ONCE(zms->destroying))
		return;

	mutex_lock(&zms->lock);
	now_ms = zms_now_ms();
	list_for_each_entry_safe(block, tmp, &zms->cache_blocks, cache_list)
		zms_cache_prune_locked(zms, block, now_ms);
	mutex_unlock(&zms->lock);

	if (!READ_ONCE(zms->destroying))
		schedule_delayed_work(&zms->cache_prune_work,
				      msecs_to_jiffies(ZMS_READ_KEEPALIVE_MS));
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
	unsigned long handle;
	unsigned long dst_slot;
	int ret;

	if (WARN_ON_ONCE(source == target))
		return -EINVAL;

	ret = zms_prepare_writable_block(zms, source, gfp, io);
	if (ret)
		return ret;
	ret = zms_prepare_writable_block(zms, target, gfp, io);
	if (ret)
		return ret;

	handle = zms_handle_for_entry_locked(zms, source, src_slot);
	if (WARN_ON_ONCE(!handle))
		return -EIO;

	dst_slot = find_first_zero_bit(target->bitmap, target->slots);
	if (WARN_ON_ONCE(dst_slot >= target->slots))
		return -ENOSPC;

	entry = &zms->handles[handle];
	memcpy((char *)target->data + dst_slot * class->size,
	       (char *)source->data + src_slot * class->size,
	       class->size);
	memset((char *)source->data + src_slot * class->size, 0, class->size);

	__set_bit(dst_slot, target->bitmap);
	target->slot_handles[dst_slot] = handle;
	target->used++;
	zms_class_drop_active_block_locked(class, target);
	zms_affinity_tag_clear(&target->affinity);
	target->affinity.mixed = true;
	zms_cache_forget_block_locked(zms, target);
	if (!target->dirty)
		zms_stat_add(zms, ZMS_STAT_DIRTY_BLOCKS, target->pages);
	target->dirty = true;
	zms_fix_fullness_locked(zms, class, target);

	__clear_bit(src_slot, source->bitmap);
	source->slot_handles[src_slot] = 0;
	source->used--;
	zms_class_drop_active_block_locked(class, source);
	if (source->used) {
		zms_affinity_tag_clear(&source->affinity);
		source->affinity.mixed = true;
	} else {
		zms_affinity_tag_clear(&source->affinity);
	}
	zms_cache_forget_block_locked(zms, source);
	if (!source->dirty)
		zms_stat_add(zms, ZMS_STAT_DIRTY_BLOCKS, source->pages);
	source->dirty = true;
	if (source->used)
		zms_fix_fullness_locked(zms, class, source);

	entry->block = target;
	entry->offset = dst_slot * class->size;
	entry->slot = dst_slot;
	return 0;
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
			zms_free_empty_block_locked(zms, class, source);
		} else {
			break;
		}
	}

	return 0;
}

static void zms_free_workfn(struct work_struct *work)
{
	struct zms *zms = container_of(work, struct zms, free_work);

	zms_reclaim_pending(zms);
}

struct zms *zms_create(struct block_device *bdev, unsigned long nr_blocks,
		       unsigned long nr_handles)
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
	zms->cache_keep_ms = ZMS_READ_KEEPALIVE_MS;
	spin_lock_init(&zms->pending_lock);
	mutex_init(&zms->lock);
	INIT_WORK(&zms->free_work, zms_free_workfn);
	INIT_DELAYED_WORK(&zms->cache_prune_work, zms_cache_prune_workfn);
	INIT_LIST_HEAD(&zms->cache_blocks);
	for (i = 0; i < ARRAY_SIZE(zms->classes); i++) {
		enum zms_fullness fullness;

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

	schedule_delayed_work(&zms->cache_prune_work,
			      msecs_to_jiffies(ZMS_READ_KEEPALIVE_MS));

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

	mutex_lock(&zms->lock);
	if (nr_handles < zms->nr_handles) {
		unsigned long handle;

		for (handle = nr_handles + 1; handle <= zms->nr_handles;
		     handle++) {
			if (zms->handles[handle].block) {
				mutex_unlock(&zms->lock);
				return -EBUSY;
			}
		}
	}

	if (!zms_handles_size(nr_handles, sizeof(*handles), &handles_size)) {
		mutex_unlock(&zms->lock);
		return -EOVERFLOW;
	}
	handles = vzalloc(handles_size);
	if (!handles) {
		mutex_unlock(&zms->lock);
		return -ENOMEM;
	}
	pending_free = zms_alloc_bitmap(nr_handles + 1, GFP_KERNEL);
	if (!pending_free) {
		vfree(handles);
		mutex_unlock(&zms->lock);
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
	mutex_unlock(&zms->lock);

	return 0;
}

void zms_destroy(struct zms *zms)
{
	unsigned long flags;
	unsigned int i;

	if (!zms)
		return;

	spin_lock_irqsave(&zms->pending_lock, flags);
	zms->destroying = true;
	spin_unlock_irqrestore(&zms->pending_lock, flags);
	cancel_work_sync(&zms->free_work);
	cancel_delayed_work_sync(&zms->cache_prune_work);
	for (i = 0; i < ARRAY_SIZE(zms->classes); i++) {
		enum zms_fullness fullness;

		for (fullness = ZMS_FG_LOW; fullness < ZMS_NR_FULLNESS;
		     fullness++) {
			struct zms_block *block;
			struct zms_block *tmp;

			list_for_each_entry_safe(block, tmp,
						 &zms->classes[i].fullness[fullness],
						 list) {
				zms_remove_block_locked(zms, &zms->classes[i], block);
				zms_free_block(zms, block);
			}
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
	if (!zms || !stats)
		return -EINVAL;

	memset(stats, 0, sizeof(*stats));
	stats->nr_blocks = zms->nr_blocks;
	stats->nr_handles = zms->nr_handles;
	stats->alloc_blocks = zms->alloc_blocks;
	stats->alloc_run_successes = zms->alloc_run_successes;
	stats->alloc_run_failures = zms->alloc_run_failures;
	stats->cache_hits = zms->cache_hits;
	stats->cache_misses = zms->cache_misses;
	stats->cache_expired = zms->cache_expired;
	stats->cache_keep_ms = zms->cache_keep_ms;
	stats->read_merge_waits = zms->read_merge_waits;
	stats->read_merge_wakeups = zms->read_merge_wakeups;
	stats->read_merge_hits = zms->read_merge_hits;
	stats->read_merge_mismatch = zms->read_merge_mismatch;
	stats->read_merge_failures = zms->read_merge_failures;
	stats->repair_on_free_calls = zms->repair_on_free_calls;
	stats->repair_on_free_moves = zms->repair_on_free_moves;
	stats->repair_on_free_source_frees =
		zms->repair_on_free_source_frees;
	stats->repair_on_free_skips = zms->repair_on_free_skips;
	stats->affinity_exact_hits = zms->affinity_exact_hits;
	stats->affinity_memcg_hits = zms->affinity_memcg_hits;
	stats->affinity_active_hits = zms->affinity_active_hits;
	stats->affinity_active_misses = zms->affinity_active_misses;
	stats->affinity_fallbacks = zms->affinity_fallbacks;
	stats->affinity_mixed_blocks = zms->affinity_mixed_blocks;
	if (zms_alloc_run_attempts(zms))
		stats->alloc_run_success_pct =
			zms->alloc_run_successes * 100 /
			zms_alloc_run_attempts(zms);
	stats->reclaim_before_alloc_calls = zms->reclaim_before_alloc_calls;
	stats->reclaim_before_alloc_handles =
		zms->reclaim_before_alloc_handles;
	stats->used_blocks = zms_stat_read_positive(zms, ZMS_STAT_USED_BLOCKS);
	stats->pending_free = zms_stat_read_positive(zms, ZMS_STAT_PENDING_FREE);
	stats->objects = zms_stat_read_positive(zms, ZMS_STAT_OBJECTS);
	stats->stored_bytes = zms_stat_read_positive(zms, ZMS_STAT_STORED_BYTES);
	stats->packed_bytes = zms_stat_read_positive(zms, ZMS_STAT_PACKED_BYTES);
	stats->partial_blocks =
		zms_stat_read_positive(zms, ZMS_STAT_PARTIAL_BLOCKS);
	stats->low_blocks = zms_stat_read_positive(zms, ZMS_STAT_LOW_BLOCKS);
	stats->mid_blocks = zms_stat_read_positive(zms, ZMS_STAT_MID_BLOCKS);
	stats->almost_full_blocks =
		zms_stat_read_positive(zms, ZMS_STAT_ALMOST_FULL_BLOCKS);
	stats->full_blocks = zms_stat_read_positive(zms, ZMS_STAT_FULL_BLOCKS);
	stats->dirty_blocks = zms_stat_read_positive(zms, ZMS_STAT_DIRTY_BLOCKS);
	stats->cached_blocks = zms_stat_read_positive(zms, ZMS_STAT_CACHED_BLOCKS);
	stats->valid_classes = READ_ONCE(zms->valid_classes);
	if (stats->nr_blocks >= stats->used_blocks)
		stats->free_blocks = stats->nr_blocks - stats->used_blocks;

	return 0;
}

int zms_store(struct zms *zms, unsigned long handle, const void *src,
	      size_t size, gfp_t gfp, struct zms_io *io)
{
	return zms_store_with_hint(zms, handle, src, size, NULL, gfp, io);
}

int zms_store_with_hint(struct zms *zms, unsigned long handle, const void *src,
			size_t size, const struct zms_write_hint *hint,
			gfp_t gfp, struct zms_io *io)
{
	struct zms_handle_entry *entry;
	struct zms_class *class;
	struct zms_block *block;
	unsigned long slot;
	int ret;

	zms_io_clear(io);
	if (!zms || !src || !size || size > PAGE_SIZE ||
	    handle == 0 || handle > zms->nr_handles)
		return -EINVAL;

	class = zms_class_for_size(zms, size);
	if (!class)
		return -EINVAL;

	mutex_lock(&zms->lock);
	if (zms_take_pending(zms, handle))
		zms_free_handle_locked(zms, handle);
	if (zms_handle_valid_locked(zms, handle)) {
		mutex_unlock(&zms->lock);
		return -EEXIST;
	}

	block = zms_find_block_locked(zms, class, hint, gfp);
	if (!block) {
		mutex_unlock(&zms->lock);
		return -ENOSPC;
	}

	ret = zms_prepare_writable_block(zms, block, gfp, io);
	if (ret) {
		mutex_unlock(&zms->lock);
		return ret;
	}

	slot = find_first_zero_bit(block->bitmap, block->slots);
	if (WARN_ON_ONCE(slot >= block->slots)) {
		mutex_unlock(&zms->lock);
		return -ENOSPC;
	}

	memcpy((char *)block->data + slot * class->size, src, size);
	if (class->size > size)
		memset((char *)block->data + slot * class->size + size, 0,
		       class->size - size);
	__set_bit(slot, block->bitmap);
	block->slot_handles[slot] = handle;
	block->used++;
	zms_stat_add(zms, ZMS_STAT_OBJECTS, 1);
	zms_stat_add(zms, ZMS_STAT_STORED_BYTES, size);
	zms_stat_add(zms, ZMS_STAT_PACKED_BYTES, class->size);
	zms_block_update_affinity_locked(zms, block, hint);
	zms_cache_forget_block_locked(zms, block);
	if (!block->dirty)
		zms_stat_add(zms, ZMS_STAT_DIRTY_BLOCKS, block->pages);
	block->dirty = true;
	if (!block->listed)
		zms_insert_block_locked(zms, class, block);
	else
		zms_fix_fullness_locked(zms, class, block);
	zms_class_promote_active_block_locked(class, block);

	entry = &zms->handles[handle];
	entry->block = block;
	entry->offset = slot * class->size;
	entry->size = size;
	entry->slot = slot;

	ret = 0;
	if (block->used == block->slots) {
		ret = zms_write_block(zms, block, gfp, io);
		if (ret) {
			unsigned int block_page;

			entry->block = NULL;
			entry->offset = 0;
			entry->size = 0;
			entry->slot = 0;
			__clear_bit(slot, block->bitmap);
			block->slot_handles[slot] = 0;
			zms_stat_add(zms, ZMS_STAT_OBJECTS, -1);
			zms_stat_add(zms, ZMS_STAT_STORED_BYTES, -(s64)size);
			zms_stat_add(zms, ZMS_STAT_PACKED_BYTES, -(s64)class->size);
			block->used--;
			if (block->used) {
				zms_fix_fullness_locked(zms, class, block);
				zms_class_promote_active_block_locked(class, block);
			} else {
				zms_class_drop_active_block_locked(class, block);
				zms_affinity_tag_clear(&block->affinity);
				zms_remove_block_locked(zms, class, block);
				for (block_page = 0; block_page < block->pages;
				     block_page++)
					zms_free_disk_block_locked(zms,
								   block->blocks[block_page]);
				zms_free_block(zms, block);
			}
		}
	}
	mutex_unlock(&zms->lock);

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

int zms_load_ref(struct zms *zms, unsigned long handle, struct zms_load_ref *ref,
		 gfp_t gfp, struct zms_io *io)
{
	struct zms_handle_entry snapshot;
	struct zms_block *block;
	bool owner = false;
	int ret;

	zms_io_clear(io);
	if (!zms || !ref)
		return -EINVAL;
	memset(ref, 0, sizeof(*ref));

retry:
	mutex_lock(&zms->lock);
	if (zms_take_pending(zms, handle)) {
		zms_free_handle_locked(zms, handle);
		mutex_unlock(&zms->lock);
		return -ENOENT;
	}
	if (!zms_handle_valid_locked(zms, handle)) {
		mutex_unlock(&zms->lock);
		return -ENOENT;
	}

	snapshot = zms->handles[handle];
	block = snapshot.block;
	if (!block->reading) {
		zms_cache_touch_locked(zms, block);
		if (block->data) {
			zms_pin_block_locked(block);
			ref->data = (char *)block->data + snapshot.offset;
			ref->size = snapshot.size;
			ref->private = block;
			mutex_unlock(&zms->lock);
			return 0;
		}
	}

	if (block->reading) {
		zms_pin_block_locked(block);
		zms->read_merge_waits++;

		zms_wait_read_done(zms, block);
		zms->read_merge_wakeups++;
		ret = block->read_ret;
		if (ret) {
			zms->read_merge_failures++;
			zms_unpin_block_locked(zms, block);
			mutex_unlock(&zms->lock);
			return ret;
		}

		if (zms_handle_pending_locked(zms, handle) ||
		    !zms_handle_valid_locked(zms, handle) ||
		    zms->handles[handle].block != block ||
		    zms->handles[handle].slot != snapshot.slot ||
		    zms->handles[handle].offset != snapshot.offset ||
		    zms->handles[handle].size != snapshot.size ||
		    !block->data) {
			zms->read_merge_mismatch++;
			zms_unpin_block_locked(zms, block);
			mutex_unlock(&zms->lock);
			return -ENOENT;
		}

		zms->read_merge_hits++;
		ref->data = (char *)block->data + snapshot.offset;
		ref->size = snapshot.size;
		ref->private = block;
		mutex_unlock(&zms->lock);
		return 0;
	}

	zms_cache_miss_locked(zms, block);
	zms_pin_block_locked(block);
	block->reading = true;
	block->read_ret = 0;
	owner = true;
	mutex_unlock(&zms->lock);

	ret = zms_read_block(zms, block, gfp, io);
	mutex_lock(&zms->lock);
	block->read_ret = ret;
	block->reading = false;
	wake_up_all(&block->read_wait);

	if (!ret) {
		if (zms_handle_pending_locked(zms, handle) ||
		    !zms_handle_valid_locked(zms, handle) ||
		    zms->handles[handle].block != block ||
		    zms->handles[handle].slot != snapshot.slot ||
		    zms->handles[handle].offset != snapshot.offset ||
		    zms->handles[handle].size != snapshot.size) {
			zms->read_merge_mismatch++;
			zms_cache_release_data_locked(zms, block);
			ret = -ENOENT;
			goto out_unpin;
		}

		ref->data = (char *)block->data + snapshot.offset;
		ref->size = snapshot.size;
		ref->private = block;
		zms_cache_keep_locked(zms, block);
		owner = false;
	}

out_unpin:
	if (owner)
		zms_unpin_block_locked(zms, block);
	mutex_unlock(&zms->lock);

	if (ret == -EBUSY)
		goto retry;

	return ret;
}

void zms_put_ref(struct zms *zms, struct zms_load_ref *ref)
{
	struct zms_block *block;

	if (!zms || !ref || !ref->private)
		return;

	block = ref->private;
	mutex_lock(&zms->lock);
	zms_unpin_block_locked(zms, block);
	mutex_unlock(&zms->lock);

	memset(ref, 0, sizeof(*ref));
}

int zms_load_batch(struct zms *zms, struct zms_load_item *items,
		   unsigned int nr, gfp_t gfp, struct zms_io *io)
{
	struct zms_load_snapshot stack_snapshots[16];
	struct zms_load_snapshot *snapshots = stack_snapshots;
	unsigned int i;

	zms_io_clear(io);
	if (!zms || !items || !nr)
		return -EINVAL;
	if (nr > ARRAY_SIZE(stack_snapshots)) {
		snapshots = kvmalloc_array(nr, sizeof(*snapshots), gfp);
		if (!snapshots)
			return -ENOMEM;
	}
	memset(snapshots, 0, array_size(nr, sizeof(*snapshots)));

	for (i = 0; i < nr; i++) {
		items[i].loaded_size = 0;
		items[i].ret = 0;
		if (!items[i].handle || !items[i].dst ||
		    !items[i].expected_size || items[i].expected_size > PAGE_SIZE)
			items[i].ret = -EINVAL;
	}

	mutex_lock(&zms->lock);
	for (i = 0; i < nr; i++) {
		struct zms_handle_entry entry;

		if (items[i].ret)
			continue;
		if (zms_take_pending(zms, items[i].handle)) {
			zms_free_handle_locked(zms, items[i].handle);
			items[i].ret = -ENOENT;
			continue;
		}
		if (!zms_handle_valid_locked(zms, items[i].handle)) {
			items[i].ret = -ENOENT;
			continue;
		}

		entry = zms->handles[items[i].handle];
		if (entry.block->reading) {
			items[i].ret = -EAGAIN;
			continue;
		}
		if (items[i].expected_size && entry.size != items[i].expected_size) {
			items[i].ret = -EIO;
			continue;
		}

		snapshots[i].block = entry.block;
		snapshots[i].offset = entry.offset;
		snapshots[i].size = entry.size;
		snapshots[i].valid = true;
	}

	for (i = 0; i < nr; i++) {
		struct zms_block *block;
		bool had_data;
		unsigned int j;

		if (!snapshots[i].valid)
			continue;
		block = snapshots[i].block;
		had_data = block->data;
		if (!had_data) {
			int err;

			err = zms_read_block(zms, block, gfp, io);
			if (err) {
				for (j = i; j < nr; j++) {
					if (snapshots[j].valid &&
					    snapshots[j].block == block) {
						items[j].ret = err;
						snapshots[j].valid = false;
					}
				}
				continue;
			}
		}

		for (j = i; j < nr; j++) {
			if (!snapshots[j].valid || snapshots[j].block != block)
				continue;
			if (items[j].ret)
				continue;
			memcpy(items[j].dst,
			       (char *)block->data + snapshots[j].offset,
			       snapshots[j].size);
			items[j].loaded_size = snapshots[j].size;
			items[j].ret = 0;
			snapshots[j].valid = false;
		}

		if (!had_data)
			zms_cache_release_data_locked(zms, block);
	}
	mutex_unlock(&zms->lock);

	if (snapshots != stack_snapshots)
		kvfree(snapshots);

	return 0;
}

int zms_peek_neighbors(struct zms *zms, unsigned long handle,
		       unsigned long *handles, unsigned int max_handles)
{
	struct zms_handle_entry entry;
	struct zms_block *block;
	unsigned int found = 0;
	unsigned int slot;

	if (!zms || !handle || !handles || !max_handles)
		return -EINVAL;

	mutex_lock(&zms->lock);
	if (zms_handle_pending_locked(zms, handle)) {
		mutex_unlock(&zms->lock);
		return -ENOENT;
	}
	if (!zms_handle_valid_locked(zms, handle)) {
		mutex_unlock(&zms->lock);
		return -ENOENT;
	}

	entry = zms->handles[handle];
	block = entry.block;
	for (slot = 0; slot < block->slots && found < max_handles; slot++) {
		unsigned long neighbor = block->slot_handles[slot];

		if (!neighbor || neighbor == handle)
			continue;
		if (neighbor > zms->nr_handles)
			continue;
		if (zms_handle_pending_locked(zms, neighbor))
			continue;
		if (!zms_handle_valid_locked(zms, neighbor))
			continue;
		if (zms->handles[neighbor].block != block ||
		    zms->handles[neighbor].slot != slot)
			continue;

		handles[found++] = neighbor;
	}
	mutex_unlock(&zms->lock);

	return found;
}

void zms_free(struct zms *zms, unsigned long handle)
{
	unsigned long flags;

	if (!zms)
		return;
	if (!handle || handle > zms->nr_handles)
		return;

	spin_lock_irqsave(&zms->pending_lock, flags);
	if (zms->destroying) {
		spin_unlock_irqrestore(&zms->pending_lock, flags);
		return;
	}
	if (!test_and_set_bit(handle, zms->pending_free))
		zms_stat_add(zms, ZMS_STAT_PENDING_FREE, 1);
	spin_unlock_irqrestore(&zms->pending_lock, flags);
	schedule_work(&zms->free_work);
}

int zms_flush_all(struct zms *zms, gfp_t gfp, struct zms_io *last_io)
{
	unsigned int i;
	int ret = 0;

	zms_io_clear(last_io);
	if (!zms)
		return -EINVAL;

	zms_reclaim_pending(zms);

	mutex_lock(&zms->lock);
	for (i = 0; i < ARRAY_SIZE(zms->classes); i++) {
		ret = zms_flush_class_locked(zms, &zms->classes[i], gfp,
					     last_io);
		if (ret)
			goto out;
	}
out:
	mutex_unlock(&zms->lock);
	return ret;
}

int zms_compact(struct zms *zms, gfp_t gfp, struct zms_io *io)
{
	unsigned int i;
	int ret = 0;
	u64 start_time_ms;

	zms_io_clear(io);
	if (!zms)
		return -EINVAL;

	zms_reclaim_pending(zms);
	start_time_ms = ktime_to_ms(ktime_get_boottime());

	mutex_lock(&zms->lock);
	for (i = 0; i < ARRAY_SIZE(zms->classes); i++) {
		ret = zms_compact_class_locked(zms, &zms->classes[i], gfp, io,
					       start_time_ms);
		if (ret)
			goto out;
	}

	for (i = 0; i < ARRAY_SIZE(zms->classes); i++) {
		if (zms_compact_time_exceeded(start_time_ms)) {
			ret = -EAGAIN;
			goto out;
		}
		ret = zms_flush_class_locked(zms, &zms->classes[i], gfp, io);
		if (ret)
			goto out;
	}
out:
	mutex_unlock(&zms->lock);
	return ret;
}

MODULE_AUTHOR("whitewhale");
MODULE_DESCRIPTION("Crystal ZMS packed compressed-object backing store");
MODULE_LICENSE("GPL");
