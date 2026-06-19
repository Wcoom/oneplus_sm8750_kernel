/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _CRYSTAL_HYBRIDSWAP_ZMS_H_
#define _CRYSTAL_HYBRIDSWAP_ZMS_H_

#include <linux/gfp_types.h>
#include <linux/types.h>

struct block_device;
struct zms;

#define ZMS_MAX_PAGES_PER_ZSPAGE 8U

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
	unsigned long full_blocks;
	unsigned long dirty_blocks;
	unsigned long cached_blocks;
	unsigned long empty_blocks;
	unsigned long valid_classes;
	unsigned long alloc_blocks;
	unsigned long alloc_run_successes;
	unsigned long alloc_run_failures;
	unsigned long alloc_run_success_pct;
	unsigned long reclaim_before_alloc_calls;
	unsigned long reclaim_before_alloc_handles;
	u64 stored_bytes;
	u64 packed_bytes;
};

struct zms *zms_create(struct block_device *bdev, unsigned long nr_blocks,
		       unsigned long nr_handles);
int zms_set_nr_handles(struct zms *zms, unsigned long nr_handles);
void zms_destroy(struct zms *zms);
int zms_get_stats(struct zms *zms, struct zms_stats *stats);

int zms_store(struct zms *zms, unsigned long handle, const void *src,
	      size_t size, gfp_t gfp, struct zms_io *io);
int zms_load(struct zms *zms, unsigned long handle, void *dst, size_t *size,
	     gfp_t gfp, struct zms_io *io);
void zms_free(struct zms *zms, unsigned long handle);
int zms_flush_all(struct zms *zms, gfp_t gfp, struct zms_io *last_io);
int zms_compact(struct zms *zms, gfp_t gfp, struct zms_io *io);

#endif
