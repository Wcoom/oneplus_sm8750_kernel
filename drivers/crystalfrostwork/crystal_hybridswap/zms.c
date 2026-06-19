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
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <linux/vmalloc.h>

#include "zms.h"

#define ZMS_ALIGN_SHIFT		4
#define ZMS_ALIGN		(1U << ZMS_ALIGN_SHIFT)
#define ZMS_MIN_SIZE		ZMS_ALIGN
#define ZMS_CLASS_SIZE		ZMS_ALIGN
#define ZMS_MAX_CLASSES		(PAGE_SIZE / ZMS_CLASS_SIZE)
#define ZMS_BLOCK_RESERVED	1UL
#define ZMS_COMPACT_SOURCE_PCT	30U
#define ZMS_COMPACT_TARGET_PCT	80U
#define ZMS_RECLAIM_BEFORE_ALLOC_MAX	64U

struct zms_block {
	unsigned int class_size;
	unsigned int pages;
	unsigned int slots;
	unsigned int used;
	unsigned long *bitmap;
	void *data;
	bool dirty;
	struct list_head list;
	unsigned long blocks[];
};

struct zms_class {
	unsigned int size;
	unsigned int pages_per_zspage;
	unsigned int slots_per_zspage;
	struct list_head partial;
	struct list_head full;
};

struct zms_handle_entry {
	struct zms_block *block;
	unsigned int offset;
	unsigned int size;
	unsigned int slot;
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
	unsigned long alloc_blocks;
	unsigned long alloc_run_successes;
	unsigned long alloc_run_failures;
	unsigned long reclaim_before_alloc_calls;
	unsigned long reclaim_before_alloc_handles;
	spinlock_t pending_lock;
	struct mutex lock;
	struct work_struct free_work;
	bool destroying;
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
		block->dirty = false;
		if (block->used == block->slots) {
			kvfree(block->data);
			block->data = NULL;
		}
	}
	return ret;
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
}

static void zms_free_block(struct zms *zms, struct zms_block *block)
{
	if (!block)
		return;

	kvfree(block->bitmap);
	if (block->data)
		kvfree(block->data);
	kfree(block);
}

static void zms_move_block_to_class_locked(struct zms_block *block,
					   struct zms_class *class)
{
	if (block->used == block->slots)
		list_move_tail(&block->list, &class->full);
	else
		list_move_tail(&block->list, &class->partial);
}

static unsigned int zms_block_used_pct(const struct zms_block *block)
{
	if (!block->slots)
		return 0;

	return (block->used * 100U) / block->slots;
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
	INIT_LIST_HEAD(&block->list);
	list_add_tail(&block->list, &class->partial);
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

static void zms_free_handle_locked(struct zms *zms, unsigned long handle);

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
		spin_unlock_irqrestore(&zms->pending_lock, flags);

		zms_free_handle_locked(zms, handle);
		handle++;
		reclaimed++;
	}

	return reclaimed;
}

static struct zms_block *zms_find_block_locked(struct zms *zms,
					       struct zms_class *class,
					       gfp_t gfp)
{
	struct zms_block *block;
	unsigned int reclaimed;

	if (!list_empty(&class->partial))
		return list_first_entry(&class->partial, struct zms_block, list);

	reclaimed = zms_reclaim_pending_locked(zms,
					       ZMS_RECLAIM_BEFORE_ALLOC_MAX);
	if (reclaimed) {
		zms->reclaim_before_alloc_calls++;
		zms->reclaim_before_alloc_handles += reclaimed;
		if (!list_empty(&class->partial))
			return list_first_entry(&class->partial,
						struct zms_block, list);
	}

	block = zms_alloc_block_locked(zms, class, gfp);
	if (!block)
		return NULL;

	return block;
}

static int zms_prepare_writable_block(struct zms *zms, struct zms_block *block,
				      gfp_t gfp, struct zms_io *io)
{
	if (block->data)
		return 0;

	return zms_read_block(zms, block, gfp, io);
}

static bool zms_handle_valid_locked(struct zms *zms, unsigned long handle)
{
	return handle > 0 && handle <= zms->nr_handles &&
		zms->handles[handle].block;
}

static bool zms_take_pending(struct zms *zms, unsigned long handle)
{
	bool pending;
	unsigned long flags;

	if (!handle || handle > zms->nr_handles)
		return false;

	spin_lock_irqsave(&zms->pending_lock, flags);
	pending = test_and_clear_bit(handle, zms->pending_free);
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

	if (!zms_handle_valid_locked(zms, handle))
		return;

	entry = &zms->handles[handle];
	block = entry->block;
	class = zms_class_for_size(zms, block->class_size);
	if (WARN_ON_ONCE(!class)) {
		entry->block = NULL;
		return;
	}

	__clear_bit(entry->slot, block->bitmap);
	entry->block = NULL;
	entry->offset = 0;
	entry->size = 0;
	entry->slot = 0;

	if (block->used == block->slots) {
		block->used--;
		zms_move_block_to_class_locked(block, class);
	} else {
		block->used--;
	}

	if (!block->used) {
		unsigned int i;

		list_del(&block->list);
		for (i = 0; i < block->pages; i++)
			zms_free_disk_block_locked(zms, block->blocks[i]);
		zms_free_block(zms, block);
	}
}

static void zms_reclaim_pending(struct zms *zms)
{
	mutex_lock(&zms->lock);
	zms_reclaim_pending_locked(zms, 0);
	mutex_unlock(&zms->lock);
}

static struct zms_block *zms_compact_source_locked(struct zms_class *class)
{
	struct zms_block *block;
	struct zms_block *best = NULL;
	unsigned int best_used = UINT_MAX;

	list_for_each_entry(block, &class->partial, list) {
		if (!block->used || zms_block_used_pct(block) > ZMS_COMPACT_SOURCE_PCT)
			continue;
		if (block->used < best_used) {
			best = block;
			best_used = block->used;
		}
	}

	return best;
}

static struct zms_block *zms_compact_target_locked(struct zms_class *class,
						   struct zms_block *source)
{
	struct zms_block *block;
	struct zms_block *best = NULL;
	unsigned int best_used = 0;

	list_for_each_entry(block, &class->partial, list) {
		if (block == source || block->used >= block->slots)
			continue;
		if (block->used < best_used)
			continue;
		best = block;
		best_used = block->used;
		if (zms_block_used_pct(block) >= ZMS_COMPACT_TARGET_PCT)
			break;
	}

	return best;
}

static unsigned long zms_handle_for_entry_locked(struct zms *zms,
						 struct zms_block *block,
						 unsigned long slot)
{
	unsigned long handle;

	for (handle = 1; handle <= zms->nr_handles; handle++) {
		struct zms_handle_entry *entry = &zms->handles[handle];

		if (entry->block == block && entry->slot == slot)
			return handle;
	}

	return 0;
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
	target->used++;
	target->dirty = true;
	if (target->used == target->slots)
		zms_move_block_to_class_locked(target, class);

	__clear_bit(src_slot, source->bitmap);
	source->used--;
	source->dirty = true;
	if (source->used)
		zms_move_block_to_class_locked(source, class);

	entry->block = target;
	entry->offset = dst_slot * class->size;
	entry->slot = dst_slot;
	return 0;
}

static int zms_compact_class_locked(struct zms *zms, struct zms_class *class,
				    gfp_t gfp, struct zms_io *io)
{
	struct zms_block *source;
	struct zms_block *target;
	unsigned long slot;
	int ret;

	while ((source = zms_compact_source_locked(class))) {
		target = zms_compact_target_locked(class, source);
		if (!target)
			break;

		for (;;) {
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
			list_del(&source->list);
			for (slot = 0; slot < source->pages; slot++)
				zms_free_disk_block_locked(zms, source->blocks[slot]);
			zms_free_block(zms, source);
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
	spin_lock_init(&zms->pending_lock);
	mutex_init(&zms->lock);
	INIT_WORK(&zms->free_work, zms_free_workfn);
	for (i = 0; i < ARRAY_SIZE(zms->classes); i++) {
		zms->classes[i].size = (i + 1) * ZMS_CLASS_SIZE;
		zms->classes[i].pages_per_zspage =
			zms_calculate_zspage_pages(zms->classes[i].size);
		zms->classes[i].slots_per_zspage =
			zms->classes[i].pages_per_zspage * PAGE_SIZE /
			zms->classes[i].size;
		INIT_LIST_HEAD(&zms->classes[i].partial);
		INIT_LIST_HEAD(&zms->classes[i].full);
	}

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
	for (i = 0; i < ARRAY_SIZE(zms->classes); i++) {
		struct zms_block *block;
		struct zms_block *tmp;

		list_for_each_entry_safe(block, tmp, &zms->classes[i].partial,
					 list) {
			list_del(&block->list);
			zms_free_block(zms, block);
		}
		list_for_each_entry_safe(block, tmp, &zms->classes[i].full,
					 list) {
			list_del(&block->list);
			zms_free_block(zms, block);
		}
	}

	vfree(zms->handles);
	kvfree(zms->pending_free);
	kvfree(zms->block_bitmap);
	kfree(zms);
}

int zms_get_stats(struct zms *zms, struct zms_stats *stats)
{
	unsigned int i;

	if (!zms || !stats)
		return -EINVAL;

	memset(stats, 0, sizeof(*stats));
	mutex_lock(&zms->lock);
	stats->nr_blocks = zms->nr_blocks;
	stats->nr_handles = zms->nr_handles;
	stats->alloc_blocks = zms->alloc_blocks;
	stats->alloc_run_successes = zms->alloc_run_successes;
	stats->alloc_run_failures = zms->alloc_run_failures;
	if (zms_alloc_run_attempts(zms))
		stats->alloc_run_success_pct =
			zms->alloc_run_successes * 100 /
			zms_alloc_run_attempts(zms);
	stats->reclaim_before_alloc_calls = zms->reclaim_before_alloc_calls;
	stats->reclaim_before_alloc_handles =
		zms->reclaim_before_alloc_handles;
	stats->used_blocks = bitmap_weight(zms->block_bitmap, zms->nr_blocks);
	stats->pending_free = bitmap_weight(zms->pending_free,
					    zms->nr_handles + 1);
	if (stats->nr_blocks >= stats->used_blocks)
		stats->free_blocks = stats->nr_blocks - stats->used_blocks;

	for (i = 0; i < ARRAY_SIZE(zms->classes); i++) {
		struct zms_class *class = &zms->classes[i];
		struct zms_block *block;
		bool class_used = false;

		list_for_each_entry(block, &class->partial, list) {
			stats->partial_blocks += block->pages;
			stats->objects += block->used;
			stats->packed_bytes += (u64)block->used * class->size;
			if (block->dirty)
				stats->dirty_blocks += block->pages;
			if (block->data)
				stats->cached_blocks += block->pages;
			if (!block->used)
				stats->empty_blocks += block->pages;
			class_used = true;
		}
		list_for_each_entry(block, &class->full, list) {
			stats->full_blocks += block->pages;
			stats->objects += block->used;
			stats->packed_bytes += (u64)block->used * class->size;
			if (block->dirty)
				stats->dirty_blocks += block->pages;
			if (block->data)
				stats->cached_blocks += block->pages;
			if (!block->used)
				stats->empty_blocks += block->pages;
			class_used = true;
		}
		if (class_used)
			stats->valid_classes++;
	}

	for (i = 1; i <= zms->nr_handles; i++) {
		if (zms->handles[i].block)
			stats->stored_bytes += zms->handles[i].size;
	}
	mutex_unlock(&zms->lock);

	return 0;
}

int zms_store(struct zms *zms, unsigned long handle, const void *src,
	      size_t size, gfp_t gfp, struct zms_io *io)
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

	block = zms_find_block_locked(zms, class, gfp);
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
	block->used++;
	block->dirty = true;
	if (block->used == block->slots)
		zms_move_block_to_class_locked(block, class);

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
			block->used--;
			if (block->used)
				zms_move_block_to_class_locked(block, class);
			if (!block->used) {
				list_del(&block->list);
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
	struct zms_handle_entry snapshot;
	struct zms_block *block;
	int ret;

	zms_io_clear(io);
	if (!zms || !dst || !size)
		return -EINVAL;

	mutex_lock(&zms->lock);
	if (zms_take_pending(zms, handle))
		zms_free_handle_locked(zms, handle);
	if (!zms_handle_valid_locked(zms, handle)) {
		mutex_unlock(&zms->lock);
		return -ENOENT;
	}

	snapshot = zms->handles[handle];
	block = snapshot.block;
	if (block->data) {
		memcpy(dst, (char *)block->data + snapshot.offset, snapshot.size);
		*size = snapshot.size;
		mutex_unlock(&zms->lock);
		return 0;
	}

	ret = zms_read_block(zms, block, gfp, io);
	if (!ret) {
		memcpy(dst, (char *)block->data + snapshot.offset, snapshot.size);
		*size = snapshot.size;
		if (!block->dirty && block->used == block->slots) {
			kvfree(block->data);
			block->data = NULL;
		}
	}
	mutex_unlock(&zms->lock);

	return ret;
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
	__set_bit(handle, zms->pending_free);
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
		struct zms_block *block;

		list_for_each_entry(block, &zms->classes[i].partial, list) {
			if (!block->dirty)
				continue;
			ret = zms_write_block(zms, block, gfp, last_io);
			if (ret)
				goto out;
		}
		list_for_each_entry(block, &zms->classes[i].full, list) {
			if (!block->dirty)
				continue;
			ret = zms_write_block(zms, block, gfp, last_io);
			if (ret)
				goto out;
		}
	}
out:
	mutex_unlock(&zms->lock);
	return ret;
}

int zms_compact(struct zms *zms, gfp_t gfp, struct zms_io *io)
{
	unsigned int i;
	int ret = 0;

	zms_io_clear(io);
	if (!zms)
		return -EINVAL;

	zms_reclaim_pending(zms);

	mutex_lock(&zms->lock);
	for (i = 0; i < ARRAY_SIZE(zms->classes); i++) {
		ret = zms_compact_class_locked(zms, &zms->classes[i], gfp, io);
		if (ret)
			goto out;
	}

	for (i = 0; i < ARRAY_SIZE(zms->classes); i++) {
		struct zms_block *block;

		list_for_each_entry(block, &zms->classes[i].partial, list) {
			if (!block->dirty)
				continue;
			ret = zms_write_block(zms, block, gfp, io);
			if (ret)
				goto out;
		}
		list_for_each_entry(block, &zms->classes[i].full, list) {
			if (!block->dirty)
				continue;
			ret = zms_write_block(zms, block, gfp, io);
			if (ret)
				goto out;
		}
	}
out:
	mutex_unlock(&zms->lock);
	return ret;
}

MODULE_AUTHOR("whitewhale");
MODULE_DESCRIPTION("Crystal ZMS packed compressed-object backing store");
MODULE_LICENSE("GPL");
