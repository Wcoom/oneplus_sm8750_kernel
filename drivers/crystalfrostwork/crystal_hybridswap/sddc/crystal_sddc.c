// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/atomic.h>
#include <linux/blkdev.h>
#include <linux/highmem.h>
#include <linux/jhash.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/zsmalloc.h>
#include <linux/bit_spinlock.h>

#include "../crystal_hybridswap_internal.h"
#include "../zram_drv.h"
#include "crystal_sddc.h"

#define CRYSTAL_SDDC_HASH_BITS		12
#define CRYSTAL_SDDC_HASH_BUCKETS	(1U << CRYSTAL_SDDC_HASH_BITS)
#define CRYSTAL_SDDC_BUCKET_WAYS	4
#define CRYSTAL_SDDC_SAMPLE_SIZE	16
#define CRYSTAL_SDDC_INDEX_MIN_SIZE	256
#define CRYSTAL_SDDC_MAX_PENDING	1024
#define CRYSTAL_SDDC_HASH_SEED		0x1e35a7bdU

enum crystal_sddc_sample_kind {
	CRYSTAL_SDDC_SAMPLE_HEAD,
	CRYSTAL_SDDC_SAMPLE_TAIL,
};

struct crystal_sddc_slot {
	u64 mutation_seq;
};

struct crystal_sddc_candidate {
	u64 mutation_seq;
	u32 index;
	u8 sample_kind;
};

struct crystal_sddc_bucket {
	struct crystal_sddc_candidate cells[CRYSTAL_SDDC_BUCKET_WAYS];
	u8 next;
};

struct crystal_sddc_stats {
	atomic64_t queued;
	atomic64_t dropped;
	atomic64_t observed;
	atomic64_t stale;
	atomic64_t indexed;
};

struct crystal_sddc {
	struct zram *zram;
	struct crystal_sddc_slot *slots;
	struct crystal_sddc_bucket *exact_index;
	struct crystal_sddc_bucket *sample_index;
	struct workqueue_struct *workqueue;
	unsigned long nr_slots;
	spinlock_t state_lock;
	spinlock_t index_lock;
	unsigned int pending;
	bool stopping;
	struct crystal_sddc_stats stats;
};

struct crystal_sddc_observe_work {
	struct work_struct work;
	struct crystal_sddc *sddc;
	struct crystal_sddc_job_key key;
};

static size_t crystal_sddc_obj_size(struct zram *zram, u32 index)
{
	return zram->table[index].flags & (BIT(ZRAM_FLAG_SHIFT) - 1);
}

static u32 crystal_sddc_priority(struct zram *zram, u32 index)
{
	u32 prio = zram->table[index].flags >> ZRAM_COMP_PRIORITY_BIT1;

	return prio & ZRAM_COMP_PRIORITY_MASK;
}

static bool crystal_sddc_test_flag(struct zram *zram, u32 index,
		enum zram_pageflags flag)
{
	return zram->table[index].flags & BIT(flag);
}

static void crystal_sddc_slot_lock(struct zram *zram, u32 index)
{
	bit_spin_lock(ZRAM_LOCK, &zram->table[index].flags);
}

static void crystal_sddc_slot_unlock(struct zram *zram, u32 index)
{
	bit_spin_unlock(ZRAM_LOCK, &zram->table[index].flags);
}

static bool crystal_sddc_job_matches_locked(struct crystal_sddc *sddc,
		const struct crystal_sddc_job_key *key)
{
	struct zram *zram = sddc->zram;

	if (key->index >= sddc->nr_slots)
		return false;
	if (sddc->slots[key->index].mutation_seq != key->mutation_seq)
		return false;
	if (zram->table[key->index].handle != key->handle)
		return false;
	if (crystal_sddc_obj_size(zram, key->index) != key->size)
		return false;
	if (crystal_sddc_priority(zram, key->index) != key->prio)
		return false;
	if (!key->handle || key->size < CRYSTAL_SDDC_SAMPLE_SIZE ||
	    key->size > PAGE_SIZE)
		return false;
	if (crystal_sddc_test_flag(zram, key->index, ZRAM_SAME) ||
	    crystal_sddc_test_flag(zram, key->index, ZRAM_WB) ||
	    crystal_sddc_test_flag(zram, key->index, ZRAM_UNDER_WB))
		return false;

	return key->prio == ZRAM_PRIMARY_COMP &&
		zram->comps[key->prio] &&
		zcomp_supports_delta(zram->comps[key->prio]);
}

static void crystal_sddc_index_insert(struct crystal_sddc_bucket *index,
		u32 hash, u32 slot_index, u64 mutation_seq, u8 sample_kind)
{
	struct crystal_sddc_bucket *bucket;
	struct crystal_sddc_candidate *candidate;

	bucket = &index[hash & (CRYSTAL_SDDC_HASH_BUCKETS - 1)];
	candidate = &bucket->cells[bucket->next];
	candidate->index = slot_index;
	candidate->mutation_seq = mutation_seq;
	candidate->sample_kind = sample_kind;
	bucket->next = (bucket->next + 1) % CRYSTAL_SDDC_BUCKET_WAYS;
}

static void crystal_sddc_observe_workfn(struct work_struct *work)
{
	struct crystal_sddc_observe_work *observe = container_of(work,
			struct crystal_sddc_observe_work, work);
	struct crystal_sddc *sddc = observe->sddc;
	const struct crystal_sddc_job_key *key = &observe->key;
	struct zram *zram = sddc->zram;
	u32 exact_hash;
	u32 head_hash;
	u32 tail_hash;
	void *payload = NULL;
	void *src;

	down_read(&zram->init_lock);
	if (zram->sddc != sddc || !zram->table ||
	    key->index >= (zram->disksize >> PAGE_SHIFT))
		goto out_unlock;

	payload = kmalloc(PAGE_SIZE, GFP_NOIO | __GFP_NOWARN);
	if (!payload) {
		atomic64_inc(&sddc->stats.dropped);
		goto out_unlock;
	}

	crystal_sddc_slot_lock(zram, key->index);
	if (!crystal_sddc_job_matches_locked(sddc, key)) {
		crystal_sddc_slot_unlock(zram, key->index);
		atomic64_inc(&sddc->stats.stale);
		goto out_unlock;
	}

	src = zs_map_object(zram->mem_pool, key->handle, ZS_MM_RO);
	memcpy(payload, src, key->size);
	zs_unmap_object(zram->mem_pool, key->handle);
	crystal_sddc_slot_unlock(zram, key->index);

	exact_hash = jhash(payload, key->size,
			CRYSTAL_SDDC_HASH_SEED ^ key->size);
	head_hash = jhash(payload, CRYSTAL_SDDC_SAMPLE_SIZE,
			CRYSTAL_SDDC_HASH_SEED);
	tail_hash = jhash(payload + key->size - CRYSTAL_SDDC_SAMPLE_SIZE,
			CRYSTAL_SDDC_SAMPLE_SIZE, CRYSTAL_SDDC_HASH_SEED);

	spin_lock(&sddc->index_lock);
	crystal_sddc_index_insert(sddc->exact_index, exact_hash, key->index,
			key->mutation_seq, CRYSTAL_SDDC_SAMPLE_HEAD);
	if (key->size > CRYSTAL_SDDC_INDEX_MIN_SIZE) {
		crystal_sddc_index_insert(sddc->sample_index, head_hash,
				key->index, key->mutation_seq,
				CRYSTAL_SDDC_SAMPLE_HEAD);
		crystal_sddc_index_insert(sddc->sample_index, tail_hash,
				key->index, key->mutation_seq,
				CRYSTAL_SDDC_SAMPLE_TAIL);
	}
	spin_unlock(&sddc->index_lock);

	atomic64_inc(&sddc->stats.observed);
	atomic64_inc(&sddc->stats.indexed);

out_unlock:
	kfree(payload);
	up_read(&zram->init_lock);
	spin_lock(&sddc->state_lock);
	WARN_ON_ONCE(!sddc->pending);
	if (sddc->pending)
		sddc->pending--;
	spin_unlock(&sddc->state_lock);
	zram_put(zram);
	kfree(observe);
}

int crystal_sddc_create(struct zram *zram, unsigned long nr_pages)
{
	struct crystal_sddc *sddc;
	int ret = -ENOMEM;

	if (!zram || zram->sddc)
		return -EINVAL;
	if (SZ_4K != PAGE_SIZE || !nr_pages ||
	    !zram->comps[ZRAM_PRIMARY_COMP] ||
	    !zcomp_supports_delta(zram->comps[ZRAM_PRIMARY_COMP]))
		return 0;

	sddc = kzalloc(sizeof(*sddc), GFP_KERNEL);
	if (!sddc)
		return -ENOMEM;

	sddc->slots = vzalloc(array_size(nr_pages, sizeof(*sddc->slots)));
	sddc->exact_index = vzalloc(array_size(CRYSTAL_SDDC_HASH_BUCKETS,
					       sizeof(*sddc->exact_index)));
	sddc->sample_index = vzalloc(array_size(CRYSTAL_SDDC_HASH_BUCKETS,
						sizeof(*sddc->sample_index)));
	if (!sddc->slots || !sddc->exact_index || !sddc->sample_index)
		goto fail;

	sddc->workqueue = alloc_ordered_workqueue("%s-sddc",
			WQ_MEM_RECLAIM | WQ_FREEZABLE, zram->disk->disk_name);
	if (!sddc->workqueue)
		goto fail;

	sddc->zram = zram;
	sddc->nr_slots = nr_pages;
	spin_lock_init(&sddc->state_lock);
	spin_lock_init(&sddc->index_lock);
	zram->sddc = sddc;
	return 0;

fail:
	if (sddc->workqueue)
		destroy_workqueue(sddc->workqueue);
	vfree(sddc->sample_index);
	vfree(sddc->exact_index);
	vfree(sddc->slots);
	kfree(sddc);
	return ret;
}

void crystal_sddc_stop(struct zram *zram)
{
	struct crystal_sddc *sddc;

	if (!zram)
		return;
	sddc = READ_ONCE(zram->sddc);
	if (!sddc)
		return;

	spin_lock(&sddc->state_lock);
	sddc->stopping = true;
	spin_unlock(&sddc->state_lock);
	flush_workqueue(sddc->workqueue);
}

void crystal_sddc_destroy(struct zram *zram)
{
	struct crystal_sddc *sddc;

	if (!zram)
		return;
	sddc = zram->sddc;
	if (!sddc)
		return;

	crystal_sddc_stop(zram);
	WARN_ON_ONCE(READ_ONCE(sddc->pending));
	WRITE_ONCE(zram->sddc, NULL);
	destroy_workqueue(sddc->workqueue);
	vfree(sddc->sample_index);
	vfree(sddc->exact_index);
	vfree(sddc->slots);
	kfree(sddc);
}

void crystal_sddc_slot_free_locked(struct zram *zram, u32 index)
{
	struct crystal_sddc *sddc = zram->sddc;

	if (sddc && index < sddc->nr_slots)
		sddc->slots[index].mutation_seq++;
}

void crystal_sddc_slot_stored_locked(struct zram *zram, u32 index)
{
	struct crystal_sddc *sddc = zram->sddc;

	if (sddc && index < sddc->nr_slots)
		sddc->slots[index].mutation_seq++;
}

void crystal_sddc_job_key_locked(struct zram *zram, u32 index,
		struct crystal_sddc_job_key *key)
{
	struct crystal_sddc *sddc = zram->sddc;

	memset(key, 0, sizeof(*key));
	if (!sddc || index >= sddc->nr_slots)
		return;

	key->index = index;
	key->mutation_seq = sddc->slots[index].mutation_seq;
	key->handle = zram->table[index].handle;
	key->size = crystal_sddc_obj_size(zram, index);
	key->prio = crystal_sddc_priority(zram, index);
}

void crystal_sddc_queue_observation(struct zram *zram,
		const struct crystal_sddc_job_key *key)
{
	struct crystal_sddc_observe_work *observe;
	struct crystal_sddc *sddc;
	bool queued = false;

	if (!zram || !key || !key->handle ||
	    key->size < CRYSTAL_SDDC_SAMPLE_SIZE || key->size > PAGE_SIZE)
		return;
	sddc = READ_ONCE(zram->sddc);
	if (!sddc)
		return;

	observe = kmalloc(sizeof(*observe), GFP_NOWAIT | __GFP_NOWARN);
	if (!observe) {
		atomic64_inc(&sddc->stats.dropped);
		return;
	}
	if (!zram_try_get(zram))
		goto free_observe;

	INIT_WORK(&observe->work, crystal_sddc_observe_workfn);
	observe->sddc = sddc;
	observe->key = *key;

	spin_lock(&sddc->state_lock);
	if (!sddc->stopping && sddc->pending < CRYSTAL_SDDC_MAX_PENDING) {
		sddc->pending++;
		queued = queue_work(sddc->workqueue, &observe->work);
		if (WARN_ON_ONCE(!queued))
			sddc->pending--;
	}
	spin_unlock(&sddc->state_lock);

	if (queued) {
		atomic64_inc(&sddc->stats.queued);
		return;
	}

	zram_put(zram);
	atomic64_inc(&sddc->stats.dropped);
free_observe:
	kfree(observe);
}
