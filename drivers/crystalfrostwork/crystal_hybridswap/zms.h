/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _CRYSTAL_HYBRIDSWAP_ZMS_H_
#define _CRYSTAL_HYBRIDSWAP_ZMS_H_

#include <linux/gfp_types.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/types.h>

struct block_device;
struct zms;

#define ZMS_MAX_PAGES_PER_ZSPAGE 8U
#define ZMS_ALIGN_SHIFT 4
#define ZMS_ALIGN (1U << ZMS_ALIGN_SHIFT)
#define ZMS_MIN_SIZE ZMS_ALIGN
#define ZMS_CLASS_SIZE ZMS_ALIGN

static inline unsigned int zms_size_to_class(size_t size)
{
	size_t aligned = ALIGN(max_t(size_t, size, ZMS_MIN_SIZE), ZMS_ALIGN);

	return (aligned / ZMS_CLASS_SIZE) - 1;
}

static inline unsigned int zms_hint_size_class(size_t size)
{
	if (!size || size > PAGE_SIZE)
		return 0;

	return zms_size_to_class(size) + 1;
}

struct zms_io {
	bool submitted;
	bool write;
	unsigned long block_index;
	u64 latency_ns;
	int ret;
	unsigned int read_submitted;
	unsigned int read_ios;
	unsigned int read_failed;
	u64 read_total_latency_ns;
	u64 read_max_latency_ns;
	unsigned long read_last_block;
	u64 read_last_latency_ns;
	int read_last_ret;
	unsigned int write_submitted;
	unsigned int write_ios;
	unsigned int write_failed;
	u64 write_total_latency_ns;
	u64 write_max_latency_ns;
	unsigned long write_last_block;
	u64 write_last_latency_ns;
	int write_last_ret;
};

struct zms_stats {
	unsigned long nr_blocks;
	unsigned long nr_handles;
	unsigned long used_blocks;
	unsigned long free_blocks;
	unsigned long objects;
	unsigned long pending_free;
	unsigned long partial_blocks;
	unsigned long low_blocks;
	unsigned long mid_blocks;
	unsigned long almost_full_blocks;
	unsigned long full_blocks;
	unsigned long dirty_blocks;
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
	unsigned long affinity_active_hits;
	unsigned long affinity_active_misses;
	unsigned long affinity_fallbacks;
	unsigned long affinity_mixed_blocks;
	unsigned long empty_blocks;
	unsigned long valid_classes;
	unsigned long alloc_blocks;
	unsigned long alloc_run_successes;
	unsigned long alloc_run_failures;
	unsigned long alloc_run_success_pct;
	unsigned long alloc_run_success_pages;
	unsigned long alloc_run_partial_pages;
	unsigned long alloc_run_fallback_pages;
	unsigned long alloc_run_contiguous_page_pct;
	unsigned long alloc_run_segments;
	unsigned long alloc_run_avg_segment_pages;
	unsigned long reclaim_before_alloc_calls;
	unsigned long reclaim_before_alloc_handles;
	u64 stored_bytes;
	u64 packed_bytes;
};

struct zms_load_item {
	unsigned long handle;
	void *dst;
	size_t expected_size;
	size_t loaded_size;
	int ret;
};

struct zms_load_ref {
	const void *data;
	size_t size;
	void *private;
};

struct zms_write_hint {
	u64 memcg_id;
	u16 size_class;
};

struct zms *zms_create(struct block_device *bdev, unsigned long nr_blocks,
			       unsigned long nr_handles);
int zms_set_nr_handles(struct zms *zms, unsigned long nr_handles);
void zms_destroy(struct zms *zms);
int zms_get_stats(struct zms *zms, struct zms_stats *stats);

int zms_store(struct zms *zms, unsigned long handle, const void *src,
	      size_t size, gfp_t gfp, struct zms_io *io);
int zms_store_with_hint(struct zms *zms, unsigned long handle, const void *src,
			size_t size, const struct zms_write_hint *hint,
			gfp_t gfp, struct zms_io *io);
int zms_load(struct zms *zms, unsigned long handle, void *dst, size_t *size,
	     gfp_t gfp, struct zms_io *io);
int zms_load_ref(struct zms *zms, unsigned long handle, struct zms_load_ref *ref,
		 gfp_t gfp, struct zms_io *io);
void zms_put_ref(struct zms *zms, struct zms_load_ref *ref);
int zms_load_batch(struct zms *zms, struct zms_load_item *items,
		   unsigned int nr, gfp_t gfp, struct zms_io *io);
int zms_peek_neighbors(struct zms *zms, unsigned long handle,
		       unsigned long *handles, unsigned int max_handles);
void zms_free(struct zms *zms, unsigned long handle);
int zms_flush_all(struct zms *zms, gfp_t gfp, struct zms_io *last_io);
int zms_compact(struct zms *zms, gfp_t gfp, struct zms_io *io);

#endif
