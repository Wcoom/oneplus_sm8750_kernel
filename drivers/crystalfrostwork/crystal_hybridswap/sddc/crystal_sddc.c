// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/atomic.h>
#include <linux/blkdev.h>
#include <linux/byteorder/generic.h>
#include <linux/highmem.h>
#include <linux/idr.h>
#include <linux/jhash.h>
#include <linux/mempool.h>
#include <linux/mm.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>
#include <linux/zsmalloc.h>
#include <linux/bit_spinlock.h>

#if IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_KUNIT_TEST)
#include <kunit/test.h>
#endif

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
#define CRYSTAL_SDDC_DELTA_GAIN_MIN	8
#define CRYSTAL_SDDC_DELTA_MAGIC	0x43444453U
#define CRYSTAL_SDDC_DELTA_VERSION	1
#define CRYSTAL_SDDC_WORKSPACE_MIN	2

enum crystal_sddc_sample_kind {
	CRYSTAL_SDDC_SAMPLE_HEAD,
	CRYSTAL_SDDC_SAMPLE_TAIL,
};

struct crystal_sddc_slot {
	u64 mutation_seq;
	struct crystal_sddc_cookie ref;
	u32 accounted_size;
	u32 saved_size;
	u8 kind;
};

struct crystal_sddc_delta_header {
	__le32 magic;
	__le16 version;
	__le16 header_size;
	__le32 ref_id;
	__le32 ref_generation;
	__le32 ref_size;
	__le32 target_size;
};

struct crystal_sddc_workspace {
	void *ordinary;
	void *ref_data;
	void *wire;
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
	atomic64_t refs;
	atomic64_t ref_bytes;
	atomic64_t aliases;
	atomic64_t deltas;
	atomic64_t delta_bytes;
	atomic64_t alias_attempts;
	atomic64_t alias_hits;
	atomic64_t delta_attempts;
	atomic64_t delta_hits;
	atomic64_t saved_bytes;
	atomic64_t saved_bytes_total;
	atomic64_t conversion_failures;
	atomic64_t decode_failures;
	atomic64_t flatten_failures;
	atomic64_t limit_rejects;
	atomic64_t pending_max;
};

struct crystal_sddc_ref {
	struct work_struct free_work;
	struct crystal_sddc *sddc;
	refcount_t refs;
	struct crystal_sddc_cookie cookie;
	unsigned long handle;
	u64 memcg_id;
	u32 size;
	u8 prio;
};

struct crystal_sddc {
	struct zram *zram;
	struct crystal_sddc_slot *slots;
	struct crystal_sddc_bucket *exact_index;
	struct crystal_sddc_bucket *sample_index;
	struct workqueue_struct *workqueue;
	struct workqueue_struct *free_workqueue;
	mempool_t *workspace_pool;
	unsigned long nr_slots;
	spinlock_t state_lock;
	spinlock_t index_lock;
	struct ida ref_ids;
	struct xarray refs;
	atomic_t next_generation;
	atomic_t active_ops;
	wait_queue_head_t active_wait;
	unsigned int pending;
	bool stopping;
	struct crystal_sddc_stats stats;
};

struct crystal_sddc_observe_work {
	struct work_struct work;
	struct crystal_sddc *sddc;
	struct crystal_sddc_job_key key;
};

struct crystal_sddc_source {
	struct crystal_sddc_job_key key;
	struct crystal_sddc_cookie cookie;
	struct crystal_sddc_ref *ref;
	bool ordinary;
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

static void crystal_sddc_set_obj_size(struct zram *zram, u32 index,
		size_t size)
{
	unsigned long flags = zram->table[index].flags >> ZRAM_FLAG_SHIFT;

	zram->table[index].flags = (flags << ZRAM_FLAG_SHIFT) | size;
}

static bool crystal_sddc_test_flag(struct zram *zram, u32 index,
		enum zram_pageflags flag)
{
	return zram->table[index].flags & BIT(flag);
}

static void crystal_sddc_clear_storage_flags(struct zram *zram, u32 index)
{
	if (crystal_sddc_test_flag(zram, index, ZRAM_HUGE)) {
		zram->table[index].flags &= ~BIT(ZRAM_HUGE);
		atomic64_dec(&zram->stats.huge_pages);
	}
	if (crystal_sddc_test_flag(zram, index, ZRAM_INCOMPRESSIBLE))
		zram->table[index].flags &= ~BIT(ZRAM_INCOMPRESSIBLE);
}

static void crystal_sddc_slot_lock(struct zram *zram, u32 index)
{
	bit_spin_lock(ZRAM_LOCK, &zram->table[index].flags);
}

static void crystal_sddc_slot_unlock(struct zram *zram, u32 index)
{
	bit_spin_unlock(ZRAM_LOCK, &zram->table[index].flags);
}

static bool crystal_sddc_cookie_equal(const struct crystal_sddc_cookie *a,
		const struct crystal_sddc_cookie *b)
{
	return a->id == b->id && a->generation == b->generation;
}

static struct crystal_sddc *crystal_sddc_manager_get(struct zram *zram)
{
	struct crystal_sddc *sddc;

	spin_lock(&zram->sddc_lock);
	sddc = zram->sddc;
	if (!sddc || READ_ONCE(sddc->stopping))
		sddc = NULL;
	else
		atomic_inc(&sddc->active_ops);
	spin_unlock(&zram->sddc_lock);
	return sddc;
}

static void crystal_sddc_manager_put(struct crystal_sddc *sddc)
{
	if (atomic_dec_and_test(&sddc->active_ops))
		wake_up_all(&sddc->active_wait);
}

static void crystal_sddc_atomic64_update_max(atomic64_t *value, s64 candidate)
{
	s64 old = atomic64_read(value);

	while (old < candidate) {
		s64 previous = atomic64_cmpxchg(value, old, candidate);

		if (previous == old)
			break;
		old = previous;
	}
}

static void *crystal_sddc_workspace_alloc(gfp_t gfp_mask, void *pool_data)
{
	struct crystal_sddc_workspace *workspace;

	(void)pool_data;
	workspace = kmalloc(sizeof(*workspace), gfp_mask);
	if (!workspace)
		return NULL;
	workspace->ordinary = kmalloc(PAGE_SIZE, gfp_mask);
	workspace->ref_data = kmalloc(PAGE_SIZE, gfp_mask);
	workspace->wire = kmalloc(PAGE_SIZE, gfp_mask);
	if (workspace->ordinary && workspace->ref_data && workspace->wire)
		return workspace;

	kfree(workspace->wire);
	kfree(workspace->ref_data);
	kfree(workspace->ordinary);
	kfree(workspace);
	return NULL;
}

static void crystal_sddc_workspace_free(void *element, void *pool_data)
{
	struct crystal_sddc_workspace *workspace = element;

	(void)pool_data;
	kfree(workspace->wire);
	kfree(workspace->ref_data);
	kfree(workspace->ordinary);
	kfree(workspace);
}

static void crystal_sddc_ref_release(struct crystal_sddc_ref *ref)
{
	struct crystal_sddc *sddc = ref->sddc;

	zs_free(sddc->zram->mem_pool, ref->handle);
	atomic64_sub(ref->size, &sddc->zram->stats.compr_data_size);
	crystal_sddc_zram_ref_account(sddc->zram, ref->memcg_id, ref->size,
			false);
	ida_free(&sddc->ref_ids, ref->cookie.id);
	atomic64_dec(&sddc->stats.refs);
	atomic64_sub(ref->size, &sddc->stats.ref_bytes);
	kfree(ref);
}

static void crystal_sddc_ref_free_workfn(struct work_struct *work)
{
	struct crystal_sddc_ref *ref = container_of(work,
			struct crystal_sddc_ref, free_work);

	crystal_sddc_ref_release(ref);
}

static void crystal_sddc_ref_queue_free(struct crystal_sddc_ref *ref)
{
	WARN_ON_ONCE(!queue_work(ref->sddc->free_workqueue, &ref->free_work));
}

static struct crystal_sddc_ref *
crystal_sddc_ref_alloc(struct crystal_sddc *sddc)
{
	struct crystal_sddc_ref *ref;
	u32 generation;
	int id;
	int ret;

	ref = kzalloc(sizeof(*ref), GFP_KERNEL);
	if (!ref)
		return ERR_PTR(-ENOMEM);

	id = ida_alloc_min(&sddc->ref_ids, 1, GFP_KERNEL);
	if (id < 0) {
		kfree(ref);
		return ERR_PTR(id);
	}

	ret = xa_reserve(&sddc->refs, id, GFP_KERNEL);
	if (ret) {
		ida_free(&sddc->ref_ids, id);
		kfree(ref);
		return ERR_PTR(ret);
	}

	generation = (u32)atomic_inc_return(&sddc->next_generation);
	if (!generation)
		generation = (u32)atomic_inc_return(&sddc->next_generation);

	INIT_WORK(&ref->free_work, crystal_sddc_ref_free_workfn);
	ref->sddc = sddc;
	ref->cookie.id = id;
	ref->cookie.generation = generation;
	return ref;
}

static void crystal_sddc_ref_abort(struct crystal_sddc_ref *ref)
{
	struct crystal_sddc *sddc;

	if (!ref)
		return;
	sddc = ref->sddc;
	xa_release(&sddc->refs, ref->cookie.id);
	ida_free(&sddc->ref_ids, ref->cookie.id);
	kfree(ref);
}

static bool crystal_sddc_ref_publish(struct crystal_sddc *sddc,
		struct crystal_sddc_ref *ref)
{
	XA_STATE(xas, &sddc->refs, 0);
	unsigned long flags;
	bool published = false;

	xas_set(&xas, ref->cookie.id);
	xa_lock_irqsave(&sddc->refs, flags);
	if (xa_is_zero(xas_load(&xas))) {
		xas_store(&xas, ref);
		published = !xas_error(&xas);
	}
	xa_unlock_irqrestore(&sddc->refs, flags);
	return published;
}

static struct crystal_sddc_ref *
crystal_sddc_ref_pin(struct crystal_sddc *sddc,
		const struct crystal_sddc_cookie *cookie)
{
	struct crystal_sddc_ref *ref;
	unsigned long flags;

	if (!cookie->id || !cookie->generation)
		return NULL;

	xa_lock_irqsave(&sddc->refs, flags);
	ref = xa_load(&sddc->refs, cookie->id);
	if (!ref || !crystal_sddc_cookie_equal(&ref->cookie, cookie) ||
	    !refcount_inc_not_zero(&ref->refs))
		ref = NULL;
	xa_unlock_irqrestore(&sddc->refs, flags);
	return ref;
}

static void crystal_sddc_ref_put(struct crystal_sddc_ref *ref)
{
	struct crystal_sddc *sddc;
	unsigned long flags;
	bool dead = false;

	if (!ref)
		return;
	sddc = ref->sddc;

	xa_lock_irqsave(&sddc->refs, flags);
	if (refcount_dec_and_test(&ref->refs)) {
		WARN_ON_ONCE(__xa_erase(&sddc->refs, ref->cookie.id) != ref);
		dead = true;
	}
	xa_unlock_irqrestore(&sddc->refs, flags);

	if (dead)
		crystal_sddc_ref_queue_free(ref);
}

static void crystal_sddc_ref_drop_cookie(struct crystal_sddc *sddc,
		const struct crystal_sddc_cookie *cookie)
{
	struct crystal_sddc_ref *ref;
	unsigned long flags;
	bool dead = false;

	xa_lock_irqsave(&sddc->refs, flags);
	ref = xa_load(&sddc->refs, cookie->id);
	if (WARN_ON_ONCE(!ref ||
			 !crystal_sddc_cookie_equal(&ref->cookie, cookie))) {
		xa_unlock_irqrestore(&sddc->refs, flags);
		return;
	}
	if (refcount_dec_and_test(&ref->refs)) {
		WARN_ON_ONCE(__xa_erase(&sddc->refs, ref->cookie.id) != ref);
		dead = true;
	}
	xa_unlock_irqrestore(&sddc->refs, flags);

	if (dead)
		crystal_sddc_ref_queue_free(ref);
}

static void crystal_sddc_copy_ref(struct crystal_sddc_ref *ref, void *dst)
{
	void *src;

	src = zs_map_object(ref->sddc->zram->mem_pool, ref->handle, ZS_MM_RO);
	memcpy(dst, src, ref->size);
	zs_unmap_object(ref->sddc->zram->mem_pool, ref->handle);
}

static bool crystal_sddc_job_matches_locked(struct crystal_sddc *sddc,
		const struct crystal_sddc_job_key *key)
{
	struct zram *zram = sddc->zram;

	if (key->index >= sddc->nr_slots)
		return false;
	if (sddc->slots[key->index].mutation_seq != key->mutation_seq)
		return false;
	if (sddc->slots[key->index].kind != CRYSTAL_SDDC_NONE)
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

static unsigned int crystal_sddc_index_candidates(
		struct crystal_sddc_bucket *index, u32 hash, u8 sample_kind,
		struct crystal_sddc_candidate *candidates, unsigned int count,
		unsigned int max)
{
	struct crystal_sddc_bucket *bucket;
	unsigned int i;
	unsigned int j;

	bucket = &index[hash & (CRYSTAL_SDDC_HASH_BUCKETS - 1)];
	for (i = 0; i < CRYSTAL_SDDC_BUCKET_WAYS && count < max; i++) {
		struct crystal_sddc_candidate *candidate = &bucket->cells[i];
		bool duplicate = false;

		if (!candidate->mutation_seq ||
		    candidate->sample_kind != sample_kind)
			continue;
		for (j = 0; j < count; j++) {
			if (candidates[j].index == candidate->index) {
				if (candidate->mutation_seq >
				    candidates[j].mutation_seq)
					candidates[j] = *candidate;
				duplicate = true;
				break;
			}
		}
		if (!duplicate)
			candidates[count++] = *candidate;
	}

	return count;
}

static bool crystal_sddc_source_snapshot(struct crystal_sddc *sddc,
		const struct crystal_sddc_candidate *candidate,
		const struct crystal_sddc_job_key *target, void *payload,
		struct crystal_sddc_source *source)
{
	struct crystal_sddc_slot *slot;
	struct zram *zram = sddc->zram;
	void *src;

	memset(source, 0, sizeof(*source));
	if (candidate->index == target->index ||
	    candidate->index >= sddc->nr_slots)
		return false;

	crystal_sddc_slot_lock(zram, candidate->index);
	slot = &sddc->slots[candidate->index];
	if (slot->mutation_seq != candidate->mutation_seq ||
	    crystal_sddc_test_flag(zram, candidate->index, ZRAM_SAME) ||
	    crystal_sddc_test_flag(zram, candidate->index, ZRAM_WB) ||
	    crystal_sddc_test_flag(zram, candidate->index, ZRAM_UNDER_WB))
		goto unlock;

	source->key.index = candidate->index;
	source->key.mutation_seq = candidate->mutation_seq;
	if (slot->kind == CRYSTAL_SDDC_REF) {
		source->cookie = slot->ref;
		source->ref = crystal_sddc_ref_pin(sddc, &slot->ref);
		if (!source->ref)
			goto unlock;
		source->key.size = source->ref->size;
		source->key.prio = source->ref->prio;
		crystal_sddc_slot_unlock(zram, candidate->index);
		if (source->key.prio != target->prio) {
			crystal_sddc_ref_put(source->ref);
			source->ref = NULL;
			return false;
		}
		crystal_sddc_copy_ref(source->ref, payload);
		return true;
	}

	if (slot->kind != CRYSTAL_SDDC_NONE)
		goto unlock;
	source->key.handle = zram->table[candidate->index].handle;
	source->key.size = crystal_sddc_obj_size(zram, candidate->index);
	source->key.prio = crystal_sddc_priority(zram, candidate->index);
	if (!source->key.handle || source->key.size < CRYSTAL_SDDC_SAMPLE_SIZE ||
	    source->key.size > PAGE_SIZE || source->key.prio != target->prio)
		goto unlock;

	src = zs_map_object(zram->mem_pool, source->key.handle, ZS_MM_RO);
	memcpy(payload, src, source->key.size);
	zs_unmap_object(zram->mem_pool, source->key.handle);
	source->ordinary = true;
	crystal_sddc_slot_unlock(zram, candidate->index);
	return true;

unlock:
	crystal_sddc_slot_unlock(zram, candidate->index);
	return false;
}

static struct crystal_sddc_ref *
crystal_sddc_promote_source(struct crystal_sddc *sddc,
		struct crystal_sddc_source *source)
{
	struct crystal_sddc_ref *ref;
	struct crystal_sddc_slot *slot;
	struct zram *zram = sddc->zram;

	if (source->ref)
		return source->ref;
	if (!source->ordinary)
		return NULL;

	ref = crystal_sddc_ref_alloc(sddc);
	if (IS_ERR(ref))
		return NULL;

	crystal_sddc_slot_lock(zram, source->key.index);
	slot = &sddc->slots[source->key.index];
	if (slot->mutation_seq != source->key.mutation_seq ||
	    slot->kind != CRYSTAL_SDDC_NONE ||
	    zram->table[source->key.index].handle != source->key.handle ||
	    crystal_sddc_obj_size(zram, source->key.index) != source->key.size ||
	    crystal_sddc_priority(zram, source->key.index) != source->key.prio ||
	    crystal_sddc_test_flag(zram, source->key.index, ZRAM_SAME) ||
	    crystal_sddc_test_flag(zram, source->key.index, ZRAM_WB) ||
	    crystal_sddc_test_flag(zram, source->key.index, ZRAM_UNDER_WB))
		goto fail_unlock;

	ref->handle = source->key.handle;
	ref->memcg_id = zram->table[source->key.index].memcg_id;
	ref->size = source->key.size;
	ref->prio = source->key.prio;
	refcount_set(&ref->refs, 2);
	if (!crystal_sddc_ref_publish(sddc, ref))
		goto fail_unlock;

	crystal_sddc_zram_account_sub_locked(zram, source->key.index);
	crystal_sddc_clear_storage_flags(zram, source->key.index);
	zram->table[source->key.index].handle = 0;
	crystal_sddc_set_obj_size(zram, source->key.index, 0);
	slot->kind = CRYSTAL_SDDC_REF;
	slot->ref = ref->cookie;
	slot->accounted_size = 0;
	slot->saved_size = 0;
	crystal_sddc_zram_account_add_locked(zram, source->key.index);
	crystal_sddc_zram_ref_account(zram, ref->memcg_id, ref->size, true);
	atomic64_inc(&sddc->stats.refs);
	atomic64_add(ref->size, &sddc->stats.ref_bytes);
	crystal_sddc_slot_unlock(zram, source->key.index);
	source->ref = ref;
	source->cookie = ref->cookie;
	return ref;

fail_unlock:
	crystal_sddc_slot_unlock(zram, source->key.index);
	crystal_sddc_ref_abort(ref);
	return NULL;
}

static bool crystal_sddc_commit_target(struct crystal_sddc *sddc,
		const struct crystal_sddc_job_key *target,
		struct crystal_sddc_ref *ref, enum crystal_sddc_kind kind,
		unsigned long new_handle, u32 new_size)
{
	struct zram *zram = sddc->zram;
	unsigned long old_handle;
	u32 old_size;
	u32 saved_size;
	bool committed = false;

	crystal_sddc_slot_lock(zram, target->index);
	if (!crystal_sddc_job_matches_locked(sddc, target))
		goto unlock;

	old_handle = zram->table[target->index].handle;
	old_size = crystal_sddc_obj_size(zram, target->index);
	saved_size = old_size > new_size ? old_size - new_size : 0;
	crystal_sddc_zram_account_sub_locked(zram, target->index);
	crystal_sddc_clear_storage_flags(zram, target->index);
	zs_free(zram->mem_pool, old_handle);
	atomic64_sub(old_size, &zram->stats.compr_data_size);
	zram->table[target->index].handle = new_handle;
	crystal_sddc_set_obj_size(zram, target->index, new_size);
	sddc->slots[target->index].kind = kind;
	sddc->slots[target->index].ref = ref->cookie;
	sddc->slots[target->index].accounted_size = new_size;
	sddc->slots[target->index].saved_size = saved_size;
	if (new_size)
		atomic64_add(new_size, &zram->stats.compr_data_size);
	crystal_sddc_zram_account_add_locked(zram, target->index);
	atomic64_add(saved_size, &sddc->stats.saved_bytes);
	atomic64_add(saved_size, &sddc->stats.saved_bytes_total);
	if (kind == CRYSTAL_SDDC_ALIAS) {
		atomic64_inc(&sddc->stats.aliases);
	} else if (kind == CRYSTAL_SDDC_DELTA) {
		atomic64_inc(&sddc->stats.deltas);
		atomic64_add(new_size, &sddc->stats.delta_bytes);
	}
	committed = true;

unlock:
	crystal_sddc_slot_unlock(zram, target->index);
	return committed;
}

static bool crystal_sddc_try_alias(struct crystal_sddc *sddc,
		const struct crystal_sddc_job_key *target, const void *target_data,
		const struct crystal_sddc_candidate *candidate, void *ref_data)
{
	struct crystal_sddc_source source;
	struct crystal_sddc_ref *ref;

	if (!crystal_sddc_source_snapshot(sddc, candidate, target, ref_data,
			&source))
		return false;
	if (source.key.size != target->size ||
	    memcmp(ref_data, target_data, target->size))
		goto put_source;

	ref = crystal_sddc_promote_source(sddc, &source);
	if (!ref) {
		atomic64_inc(&sddc->stats.conversion_failures);
		goto put_source;
	}
	if (crystal_sddc_commit_target(sddc, target, ref,
			CRYSTAL_SDDC_ALIAS, 0, 0)) {
		return true;
	}

	crystal_sddc_ref_put(ref);
	atomic64_inc(&sddc->stats.conversion_failures);
	return false;

put_source:
	crystal_sddc_ref_put(source.ref);
	return false;
}

static bool crystal_sddc_try_delta(struct crystal_sddc *sddc,
		const struct crystal_sddc_job_key *target, const void *target_data,
		const struct crystal_sddc_candidate *candidate, void *ref_data)
{
	struct crystal_sddc_delta_header header;
	struct crystal_sddc_source source;
	struct crystal_sddc_ref *ref;
	struct zcomp_strm *zstrm;
	unsigned long handle;
	unsigned int delta_len;
	unsigned int out_limit;
	u32 wire_size;
	void *delta = NULL;
	void *dst;
	int ret;

	if (target->size <= sizeof(header) + CRYSTAL_SDDC_DELTA_GAIN_MIN ||
	    !crystal_sddc_source_snapshot(sddc, candidate, target, ref_data,
			&source))
		return false;
	if ((source.key.size == PAGE_SIZE) != (target->size == PAGE_SIZE))
		goto put_source;

	out_limit = target->size - sizeof(header) -
		CRYSTAL_SDDC_DELTA_GAIN_MIN;
	zstrm = zcomp_stream_get(sddc->zram->comps[target->prio]);
	delta_len = 2 * PAGE_SIZE;
	ret = zcomp_compress_delta(zstrm, ref_data, source.key.size,
			target_data, target->size, &delta_len, out_limit);
	if (!ret && delta_len && delta_len <= out_limit)
		delta = kmemdup(zstrm->buffer, delta_len, GFP_NOWAIT);
	zcomp_stream_put(sddc->zram->comps[target->prio]);
	if (!delta) {
		if (!ret && delta_len && delta_len <= out_limit)
			atomic64_inc(&sddc->stats.conversion_failures);
		goto put_source;
	}

	wire_size = sizeof(header) + delta_len;
	if (zs_lookup_class_index(sddc->zram->mem_pool, wire_size) >=
	    zs_lookup_class_index(sddc->zram->mem_pool, target->size))
		goto free_delta_put;
	handle = zs_malloc(sddc->zram->mem_pool, wire_size,
			GFP_NOIO | __GFP_HIGHMEM | __GFP_MOVABLE | __GFP_CMA);
	if (IS_ERR_VALUE(handle)) {
		atomic64_inc(&sddc->stats.conversion_failures);
		goto free_delta_put;
	}
	if (!crystal_sddc_zram_memory_limit_ok(sddc->zram)) {
		atomic64_inc(&sddc->stats.limit_rejects);
		atomic64_inc(&sddc->stats.conversion_failures);
		goto free_handle;
	}

	ref = crystal_sddc_promote_source(sddc, &source);
	if (!ref) {
		atomic64_inc(&sddc->stats.conversion_failures);
		goto free_handle;
	}
	header.magic = cpu_to_le32(CRYSTAL_SDDC_DELTA_MAGIC);
	header.version = cpu_to_le16(CRYSTAL_SDDC_DELTA_VERSION);
	header.header_size = cpu_to_le16(sizeof(header));
	header.ref_id = cpu_to_le32(ref->cookie.id);
	header.ref_generation = cpu_to_le32(ref->cookie.generation);
	header.ref_size = cpu_to_le32(ref->size);
	header.target_size = cpu_to_le32(target->size);
	dst = zs_map_object(sddc->zram->mem_pool, handle, ZS_MM_WO);
	memcpy(dst, &header, sizeof(header));
	memcpy(dst + sizeof(header), delta, delta_len);
	zs_unmap_object(sddc->zram->mem_pool, handle);

	if (crystal_sddc_commit_target(sddc, target, ref,
			CRYSTAL_SDDC_DELTA, handle, wire_size)) {
		kfree(delta);
		return true;
	}

	crystal_sddc_ref_put(ref);
	source.ref = NULL;
	atomic64_inc(&sddc->stats.conversion_failures);
free_handle:
	zs_free(sddc->zram->mem_pool, handle);
free_delta_put:
	crystal_sddc_ref_put(source.ref);
	kfree(delta);
	return false;

put_source:
	crystal_sddc_ref_put(source.ref);
	return false;
}

static bool crystal_sddc_try_convert(struct crystal_sddc *sddc,
		const struct crystal_sddc_job_key *target, const void *target_data,
		u32 exact_hash, u32 head_hash, u32 tail_hash)
{
	struct crystal_sddc_candidate candidates[CRYSTAL_SDDC_BUCKET_WAYS * 2];
	void *ref_data;
	unsigned int count = 0;
	unsigned int i;
	bool converted = false;

	ref_data = kmalloc(PAGE_SIZE, GFP_NOIO | __GFP_NOWARN);
	if (!ref_data)
		return false;

	spin_lock(&sddc->index_lock);
	count = crystal_sddc_index_candidates(sddc->exact_index, exact_hash,
			CRYSTAL_SDDC_SAMPLE_HEAD, candidates, count,
			ARRAY_SIZE(candidates));
	spin_unlock(&sddc->index_lock);
	for (i = 0; i < count; i++) {
		atomic64_inc(&sddc->stats.alias_attempts);
		if (crystal_sddc_try_alias(sddc, target, target_data,
				&candidates[i], ref_data)) {
			atomic64_inc(&sddc->stats.alias_hits);
			converted = true;
			goto out;
		}
	}

	count = 0;
	spin_lock(&sddc->index_lock);
	count = crystal_sddc_index_candidates(sddc->sample_index, head_hash,
			CRYSTAL_SDDC_SAMPLE_HEAD, candidates, count,
			ARRAY_SIZE(candidates));
	count = crystal_sddc_index_candidates(sddc->sample_index, tail_hash,
			CRYSTAL_SDDC_SAMPLE_TAIL, candidates, count,
			ARRAY_SIZE(candidates));
	spin_unlock(&sddc->index_lock);
	for (i = 0; i < count; i++) {
		atomic64_inc(&sddc->stats.delta_attempts);
		if (crystal_sddc_try_delta(sddc, target, target_data,
				&candidates[i], ref_data)) {
			atomic64_inc(&sddc->stats.delta_hits);
			converted = true;
			goto out;
		}
	}

out:
	kfree(ref_data);
	return converted;
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
	bool converted;

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
	converted = crystal_sddc_try_convert(sddc, key, payload, exact_hash,
			head_hash, tail_hash);

	if (!converted) {
		spin_lock(&sddc->index_lock);
		crystal_sddc_index_insert(sddc->exact_index, exact_hash,
				key->index, key->mutation_seq,
				CRYSTAL_SDDC_SAMPLE_HEAD);
		if (key->size > CRYSTAL_SDDC_INDEX_MIN_SIZE) {
			crystal_sddc_index_insert(sddc->sample_index, head_hash,
					key->index, key->mutation_seq,
					CRYSTAL_SDDC_SAMPLE_HEAD);
			crystal_sddc_index_insert(sddc->sample_index, tail_hash,
					key->index, key->mutation_seq,
					CRYSTAL_SDDC_SAMPLE_TAIL);
		}
		spin_unlock(&sddc->index_lock);
		atomic64_inc(&sddc->stats.indexed);
	}

	atomic64_inc(&sddc->stats.observed);

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
	crystal_sddc_manager_put(sddc);
}

int crystal_sddc_create(struct zram *zram, unsigned long nr_pages)
{
	struct crystal_sddc *sddc;
	int ret = -ENOMEM;

	if (!zram)
		return -EINVAL;
	lockdep_assert_held(&zram->sddc_lifecycle_lock);
	lockdep_assert_held_write(&zram->init_lock);
	if (zram->sddc)
		return -EINVAL;
	if (PAGE_SHIFT != 12 || !nr_pages ||
	    !zram->comps[ZRAM_PRIMARY_COMP] ||
	    !zcomp_supports_delta(zram->comps[ZRAM_PRIMARY_COMP]))
		return 0;

	sddc = kzalloc(sizeof(*sddc), GFP_KERNEL);
	if (!sddc)
		return -ENOMEM;
	ida_init(&sddc->ref_ids);
	xa_init(&sddc->refs);

	sddc->slots = vzalloc(array_size(nr_pages, sizeof(*sddc->slots)));
	sddc->exact_index = vzalloc(array_size(CRYSTAL_SDDC_HASH_BUCKETS,
					       sizeof(*sddc->exact_index)));
	sddc->sample_index = vzalloc(array_size(CRYSTAL_SDDC_HASH_BUCKETS,
							sizeof(*sddc->sample_index)));
	if (!sddc->slots || !sddc->exact_index || !sddc->sample_index)
		goto fail;

	sddc->workspace_pool = mempool_create(CRYSTAL_SDDC_WORKSPACE_MIN,
			crystal_sddc_workspace_alloc, crystal_sddc_workspace_free,
			NULL);
	if (!sddc->workspace_pool)
		goto fail;

	sddc->workqueue = alloc_ordered_workqueue("%s-sddc",
				WQ_MEM_RECLAIM | WQ_FREEZABLE, zram->disk->disk_name);
	if (!sddc->workqueue)
		goto fail;
	sddc->free_workqueue = alloc_workqueue("%s-sddc-free",
			WQ_MEM_RECLAIM | WQ_UNBOUND | WQ_HIGHPRI, 0,
			zram->disk->disk_name);
	if (!sddc->free_workqueue)
		goto fail;

	sddc->zram = zram;
	sddc->nr_slots = nr_pages;
	spin_lock_init(&sddc->state_lock);
	spin_lock_init(&sddc->index_lock);
	atomic_set(&sddc->active_ops, 0);
	init_waitqueue_head(&sddc->active_wait);
	spin_lock(&zram->sddc_lock);
	WARN_ON_ONCE(zram->sddc);
	WRITE_ONCE(zram->sddc, sddc);
	spin_unlock(&zram->sddc_lock);
	return 0;

fail:
	if (sddc->free_workqueue)
		destroy_workqueue(sddc->free_workqueue);
	if (sddc->workqueue)
		destroy_workqueue(sddc->workqueue);
	mempool_destroy(sddc->workspace_pool);
	vfree(sddc->sample_index);
	vfree(sddc->exact_index);
	vfree(sddc->slots);
	xa_destroy(&sddc->refs);
	ida_destroy(&sddc->ref_ids);
	kfree(sddc);
	return ret;
}

void crystal_sddc_stop(struct zram *zram)
{
	struct crystal_sddc *sddc;

	if (!zram)
		return;
	lockdep_assert_held(&zram->sddc_lifecycle_lock);
	spin_lock(&zram->sddc_lock);
	sddc = READ_ONCE(zram->sddc);
	if (sddc)
		WRITE_ONCE(sddc->stopping, true);
	spin_unlock(&zram->sddc_lock);
	if (!sddc)
		return;

	flush_workqueue(sddc->workqueue);
	wait_event(sddc->active_wait, !atomic_read(&sddc->active_ops));
	WARN_ON_ONCE(READ_ONCE(sddc->pending));
	flush_workqueue(sddc->free_workqueue);
}

void crystal_sddc_destroy(struct zram *zram)
{
	struct crystal_sddc *sddc;

	if (!zram)
		return;
	lockdep_assert_held(&zram->sddc_lifecycle_lock);
	lockdep_assert_held_write(&zram->init_lock);
	sddc = zram->sddc;
	if (!sddc)
		return;

	WARN_ON_ONCE(!READ_ONCE(sddc->stopping));
	WARN_ON_ONCE(atomic_read(&sddc->active_ops));
	WARN_ON_ONCE(READ_ONCE(sddc->pending));
	spin_lock(&zram->sddc_lock);
	WARN_ON_ONCE(zram->sddc != sddc);
	WRITE_ONCE(zram->sddc, NULL);
	spin_unlock(&zram->sddc_lock);
	flush_workqueue(sddc->free_workqueue);
	WARN_ON_ONCE(!xa_empty(&sddc->refs));
	destroy_workqueue(sddc->workqueue);
	destroy_workqueue(sddc->free_workqueue);
	mempool_destroy(sddc->workspace_pool);
	vfree(sddc->sample_index);
	vfree(sddc->exact_index);
	vfree(sddc->slots);
	xa_destroy(&sddc->refs);
	ida_destroy(&sddc->ref_ids);
	kfree(sddc);
}

enum crystal_sddc_kind crystal_sddc_slot_free_locked(struct zram *zram,
		u32 index)
{
	struct crystal_sddc *sddc = zram->sddc;
	struct crystal_sddc_cookie cookie;
	enum crystal_sddc_kind kind;
	u32 accounted_size;
	u32 saved_size;

	if (!sddc || index >= sddc->nr_slots)
		return CRYSTAL_SDDC_NONE;

	kind = sddc->slots[index].kind;
	cookie = sddc->slots[index].ref;
	accounted_size = sddc->slots[index].accounted_size;
	saved_size = sddc->slots[index].saved_size;
	sddc->slots[index].kind = CRYSTAL_SDDC_NONE;
	sddc->slots[index].accounted_size = 0;
	sddc->slots[index].saved_size = 0;
	memset(&sddc->slots[index].ref, 0,
	       sizeof(sddc->slots[index].ref));
	sddc->slots[index].mutation_seq++;
	if (kind == CRYSTAL_SDDC_ALIAS)
		atomic64_dec(&sddc->stats.aliases);
	else if (kind == CRYSTAL_SDDC_DELTA) {
		atomic64_dec(&sddc->stats.deltas);
		atomic64_sub(accounted_size, &sddc->stats.delta_bytes);
	}
	atomic64_sub(saved_size, &sddc->stats.saved_bytes);
	if (kind != CRYSTAL_SDDC_NONE)
		crystal_sddc_ref_drop_cookie(sddc, &cookie);

	return kind;
}

void crystal_sddc_slot_stored_locked(struct zram *zram, u32 index)
{
	struct crystal_sddc *sddc = zram->sddc;

	if (sddc && index < sddc->nr_slots) {
		sddc->slots[index].kind = CRYSTAL_SDDC_NONE;
		sddc->slots[index].accounted_size = 0;
		sddc->slots[index].saved_size = 0;
		memset(&sddc->slots[index].ref, 0,
		       sizeof(sddc->slots[index].ref));
		sddc->slots[index].mutation_seq++;
	}
}

void crystal_sddc_job_key_locked(struct zram *zram, u32 index,
		struct crystal_sddc_job_key *key)
{
	struct crystal_sddc *sddc = zram->sddc;
	unsigned long handle;
	u32 size;
	u32 prio;

	memset(key, 0, sizeof(*key));
	if (!sddc || index >= sddc->nr_slots ||
	    sddc->slots[index].kind != CRYSTAL_SDDC_NONE ||
	    crystal_sddc_test_flag(zram, index, ZRAM_SAME) ||
	    crystal_sddc_test_flag(zram, index, ZRAM_WB) ||
	    crystal_sddc_test_flag(zram, index, ZRAM_UNDER_WB))
		return;

	handle = zram->table[index].handle;
	size = crystal_sddc_obj_size(zram, index);
	prio = crystal_sddc_priority(zram, index);
	if (!handle || size < CRYSTAL_SDDC_SAMPLE_SIZE || size > PAGE_SIZE ||
	    prio != ZRAM_PRIMARY_COMP || !zram->comps[prio] ||
	    !zcomp_supports_delta(zram->comps[prio]))
		return;

	key->index = index;
	key->mutation_seq = sddc->slots[index].mutation_seq;
	key->handle = handle;
	key->size = size;
	key->prio = prio;
}

void crystal_sddc_requeue_observation(struct zram *zram, u32 index)
{
	struct crystal_sddc_job_key key;
	struct crystal_sddc *sddc;

	if (!zram)
		return;
	sddc = crystal_sddc_manager_get(zram);
	if (!sddc || index >= sddc->nr_slots)
		goto put_manager;

	crystal_sddc_slot_lock(zram, index);
	crystal_sddc_job_key_locked(zram, index, &key);
	crystal_sddc_slot_unlock(zram, index);
	crystal_sddc_manager_put(sddc);
	crystal_sddc_queue_observation(zram, &key);
	return;

put_manager:
	if (sddc)
		crystal_sddc_manager_put(sddc);
}

bool crystal_sddc_slot_allocated_locked(struct zram *zram, u32 index)
{
	struct crystal_sddc *sddc = zram->sddc;

	return sddc && index < sddc->nr_slots &&
		sddc->slots[index].kind != CRYSTAL_SDDC_NONE;
}

bool crystal_sddc_slot_managed_locked(struct zram *zram, u32 index)
{
	return crystal_sddc_slot_allocated_locked(zram, index);
}

bool crystal_sddc_accounted_size_locked(struct zram *zram, u32 index,
		size_t *size)
{
	struct crystal_sddc *sddc = zram->sddc;

	if (!sddc || index >= sddc->nr_slots ||
	    sddc->slots[index].kind == CRYSTAL_SDDC_NONE)
		return false;
	*size = sddc->slots[index].accounted_size;
	return true;
}

void crystal_sddc_snapshot_locked(struct zram *zram, u32 index,
		struct crystal_sddc_snapshot *snapshot)
{
	struct crystal_sddc *sddc = zram->sddc;

	memset(snapshot, 0, sizeof(*snapshot));
	if (!sddc || index >= sddc->nr_slots)
		return;
	snapshot->mutation_seq = sddc->slots[index].mutation_seq;
	snapshot->ref = sddc->slots[index].ref;
	snapshot->kind = sddc->slots[index].kind;
}

bool crystal_sddc_snapshot_matches_locked(struct zram *zram, u32 index,
		const struct crystal_sddc_snapshot *snapshot)
{
	struct crystal_sddc_snapshot current_snapshot;

	crystal_sddc_snapshot_locked(zram, index, &current_snapshot);
	return current_snapshot.mutation_seq == snapshot->mutation_seq &&
		current_snapshot.kind == snapshot->kind &&
		crystal_sddc_cookie_equal(&current_snapshot.ref, &snapshot->ref);
}

static bool crystal_sddc_delta_header_valid(
		const struct crystal_sddc_ref *ref,
		const struct crystal_sddc_delta_header *header, u32 wire_size)
{
	if (!ref || !header || wire_size <= sizeof(*header) ||
	    wire_size > PAGE_SIZE)
		return false;
	if (le32_to_cpu(header->magic) != CRYSTAL_SDDC_DELTA_MAGIC ||
	    le16_to_cpu(header->version) != CRYSTAL_SDDC_DELTA_VERSION ||
	    le16_to_cpu(header->header_size) != sizeof(*header) ||
	    le32_to_cpu(header->ref_size) != ref->size ||
	    !le32_to_cpu(header->target_size) ||
	    le32_to_cpu(header->target_size) > PAGE_SIZE)
		return false;
	return le32_to_cpu(header->ref_id) == ref->cookie.id &&
		le32_to_cpu(header->ref_generation) == ref->cookie.generation;
}

static int crystal_sddc_restore_ordinary(struct crystal_sddc *sddc,
		struct crystal_sddc_ref *ref, enum crystal_sddc_kind kind,
		const void *wire, u32 wire_size, const void *ref_data, void *dst,
		size_t *size)
{
	const struct crystal_sddc_delta_header *header = wire;
	struct zcomp_strm *zstrm;
	unsigned int restored_size;
	int ret;

	if (kind == CRYSTAL_SDDC_REF || kind == CRYSTAL_SDDC_ALIAS) {
		memcpy(dst, ref_data, ref->size);
		*size = ref->size;
		return 0;
	}
	if (kind != CRYSTAL_SDDC_DELTA ||
	    !crystal_sddc_delta_header_valid(ref, header, wire_size))
		return -EIO;
	if (!sddc->zram->comps[ref->prio] ||
	    !zcomp_supports_delta(sddc->zram->comps[ref->prio]))
		return -EIO;

	zstrm = zcomp_stream_get(sddc->zram->comps[ref->prio]);
	restored_size = PAGE_SIZE;
	ret = zcomp_decompress_delta(zstrm,
			(const u8 *)wire + sizeof(*header),
			wire_size - sizeof(*header), ref_data, ref->size, dst,
			&restored_size);
	zcomp_stream_put(sddc->zram->comps[ref->prio]);
	if (ret || restored_size != le32_to_cpu(header->target_size))
		return -EIO;

	*size = restored_size;
	return 0;
}

static int crystal_sddc_capture_locked(struct crystal_sddc *sddc, u32 index,
		const struct crystal_sddc_snapshot *expected,
		enum crystal_sddc_kind *kind, struct crystal_sddc_ref **ref,
		void *wire, u32 *wire_size)
{
	struct crystal_sddc_slot *slot;
	struct zram *zram = sddc->zram;
	size_t object_size;
	void *src;

	if (index >= sddc->nr_slots)
		return -EAGAIN;
	slot = &sddc->slots[index];
	if (expected &&
	    (slot->mutation_seq != expected->mutation_seq ||
	     slot->kind != expected->kind ||
	     !crystal_sddc_cookie_equal(&slot->ref, &expected->ref)))
		return -EAGAIN;
	if (slot->kind == CRYSTAL_SDDC_NONE)
		return -EAGAIN;
	if (crystal_sddc_test_flag(zram, index, ZRAM_WB) ||
	    (!expected && crystal_sddc_test_flag(zram, index, ZRAM_UNDER_WB)))
		return -EAGAIN;

	*kind = slot->kind;
	*ref = crystal_sddc_ref_pin(sddc, &slot->ref);
	if (!*ref)
		return -EIO;
	if ((*ref)->size < CRYSTAL_SDDC_SAMPLE_SIZE ||
	    (*ref)->size > PAGE_SIZE || (*ref)->prio >= ZRAM_MAX_COMPS) {
		crystal_sddc_ref_put(*ref);
		*ref = NULL;
		return -EIO;
	}

	*wire_size = 0;
	object_size = crystal_sddc_obj_size(zram, index);
	if (*kind == CRYSTAL_SDDC_REF || *kind == CRYSTAL_SDDC_ALIAS) {
		if (zram->table[index].handle || object_size) {
			crystal_sddc_ref_put(*ref);
			*ref = NULL;
			return -EIO;
		}
		return 0;
	}
	if (*kind != CRYSTAL_SDDC_DELTA) {
		crystal_sddc_ref_put(*ref);
		*ref = NULL;
		return -EIO;
	}
	if (!zram->table[index].handle ||
	    object_size <= sizeof(struct crystal_sddc_delta_header) ||
	    object_size > PAGE_SIZE) {
		crystal_sddc_ref_put(*ref);
		*ref = NULL;
		return -EIO;
	}

	*wire_size = object_size;
	src = zs_map_object(zram->mem_pool, zram->table[index].handle, ZS_MM_RO);
	memcpy(wire, src, *wire_size);
	zs_unmap_object(zram->mem_pool, zram->table[index].handle);
	return 0;
}

int crystal_sddc_read_page(struct zram *zram, struct page *page, u32 index)
{
	struct crystal_sddc_ref *ref = NULL;
	struct crystal_sddc *sddc;
	struct crystal_sddc_workspace *workspace = NULL;
	struct zcomp_strm *zstrm;
	enum crystal_sddc_kind kind;
	void *dst;
	size_t ordinary_size;
	u32 wire_size;
	int ret;

	sddc = crystal_sddc_manager_get(zram);
	if (!sddc)
		return -EAGAIN;
	workspace = mempool_alloc(sddc->workspace_pool, GFP_NOIO);
	if (!workspace) {
		ret = -ENOMEM;
		goto out;
	}

	crystal_sddc_slot_lock(zram, index);
	ret = crystal_sddc_capture_locked(sddc, index, NULL, &kind, &ref,
			workspace->wire, &wire_size);
	crystal_sddc_slot_unlock(zram, index);
	if (ret)
		goto out;

	crystal_sddc_copy_ref(ref, workspace->ref_data);
	ret = crystal_sddc_restore_ordinary(sddc, ref, kind, workspace->wire,
			wire_size, workspace->ref_data, workspace->ordinary,
			&ordinary_size);
	if (ret)
		goto decode_error;

	dst = kmap_local_page(page);
	if (ordinary_size == PAGE_SIZE) {
		memcpy(dst, workspace->ordinary, PAGE_SIZE);
		ret = 0;
	} else {
		if (!zram->comps[ref->prio]) {
			ret = -EIO;
			goto unmap;
		}
		zstrm = zcomp_stream_get(zram->comps[ref->prio]);
		ret = zcomp_decompress(zstrm, workspace->ordinary,
				ordinary_size, dst);
		zcomp_stream_put(zram->comps[ref->prio]);
		if (ret)
			ret = -EIO;
	}
unmap:
	kunmap_local(dst);
	if (ret)
		goto decode_error;
	goto out;

decode_error:
	atomic64_inc(&sddc->stats.decode_failures);
out:
	crystal_sddc_ref_put(ref);
	if (workspace)
		mempool_free(workspace, sddc->workspace_pool);
	crystal_sddc_manager_put(sddc);
	return ret;
}

int crystal_sddc_flatten(struct zram *zram, u32 index,
		const struct crystal_sddc_snapshot *snapshot, void *dst,
		size_t *size)
{
	struct crystal_sddc_ref *ref = NULL;
	struct crystal_sddc *sddc;
	struct crystal_sddc_workspace *workspace = NULL;
	enum crystal_sddc_kind kind;
	u32 wire_size;
	int ret;

	if (!zram || !snapshot || !dst || !size)
		return -EINVAL;
	sddc = crystal_sddc_manager_get(zram);
	if (!sddc)
		return -EAGAIN;
	workspace = mempool_alloc(sddc->workspace_pool, GFP_NOIO);
	if (!workspace) {
		ret = -ENOMEM;
		goto out;
	}

	crystal_sddc_slot_lock(zram, index);
	ret = crystal_sddc_capture_locked(sddc, index, snapshot, &kind, &ref,
			workspace->wire, &wire_size);
	crystal_sddc_slot_unlock(zram, index);
	if (ret)
		goto out;

	crystal_sddc_copy_ref(ref, workspace->ref_data);
	ret = crystal_sddc_restore_ordinary(sddc, ref, kind, workspace->wire,
			wire_size, workspace->ref_data, dst, size);
	if (ret)
		atomic64_inc(&sddc->stats.decode_failures);

out:
	if (ret && ret != -EAGAIN)
		atomic64_inc(&sddc->stats.flatten_failures);
	crystal_sddc_ref_put(ref);
	if (workspace)
		mempool_free(workspace, sddc->workspace_pool);
	crystal_sddc_manager_put(sddc);
	return ret;
}

void crystal_sddc_get_stats(struct zram *zram,
		struct crystal_sddc_stats_snapshot *stats)
{
	struct crystal_sddc *sddc;

	if (!stats)
		return;
	memset(stats, 0, sizeof(*stats));
	if (!zram)
		return;
	sddc = crystal_sddc_manager_get(zram);
	if (!sddc)
		return;

	stats->enabled = true;
	stats->queued = atomic64_read(&sddc->stats.queued);
	stats->dropped = atomic64_read(&sddc->stats.dropped);
	stats->observed = atomic64_read(&sddc->stats.observed);
	stats->stale = atomic64_read(&sddc->stats.stale);
	stats->indexed = atomic64_read(&sddc->stats.indexed);
	stats->refs = atomic64_read(&sddc->stats.refs);
	stats->ref_bytes = atomic64_read(&sddc->stats.ref_bytes);
	stats->aliases = atomic64_read(&sddc->stats.aliases);
	stats->deltas = atomic64_read(&sddc->stats.deltas);
	stats->delta_bytes = atomic64_read(&sddc->stats.delta_bytes);
	stats->alias_attempts = atomic64_read(&sddc->stats.alias_attempts);
	stats->alias_hits = atomic64_read(&sddc->stats.alias_hits);
	stats->delta_attempts = atomic64_read(&sddc->stats.delta_attempts);
	stats->delta_hits = atomic64_read(&sddc->stats.delta_hits);
	stats->saved_bytes = atomic64_read(&sddc->stats.saved_bytes);
	stats->saved_bytes_total =
		atomic64_read(&sddc->stats.saved_bytes_total);
	stats->conversion_failures =
		atomic64_read(&sddc->stats.conversion_failures);
	stats->decode_failures = atomic64_read(&sddc->stats.decode_failures);
	stats->flatten_failures = atomic64_read(&sddc->stats.flatten_failures);
	stats->limit_rejects = atomic64_read(&sddc->stats.limit_rejects);
	stats->pending_max = atomic64_read(&sddc->stats.pending_max);
	spin_lock(&sddc->state_lock);
	stats->pending = sddc->pending;
	spin_unlock(&sddc->state_lock);
	crystal_sddc_manager_put(sddc);
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
	sddc = crystal_sddc_manager_get(zram);
	if (!sddc)
		return;

	observe = kmalloc(sizeof(*observe), GFP_NOWAIT | __GFP_NOWARN);
	if (!observe) {
		atomic64_inc(&sddc->stats.dropped);
		goto put_manager;
	}
	if (!zram_try_get(zram))
		goto free_observe;

	INIT_WORK(&observe->work, crystal_sddc_observe_workfn);
	observe->sddc = sddc;
	observe->key = *key;

	spin_lock(&sddc->state_lock);
	if (!READ_ONCE(sddc->stopping) &&
	    sddc->pending < CRYSTAL_SDDC_MAX_PENDING) {
		sddc->pending++;
		crystal_sddc_atomic64_update_max(&sddc->stats.pending_max,
				sddc->pending);
		atomic_inc(&sddc->active_ops);
		queued = queue_work(sddc->workqueue, &observe->work);
		if (WARN_ON_ONCE(!queued)) {
			sddc->pending--;
			crystal_sddc_manager_put(sddc);
		}
	}
	spin_unlock(&sddc->state_lock);

	if (queued) {
		atomic64_inc(&sddc->stats.queued);
		crystal_sddc_manager_put(sddc);
		return;
	}

	zram_put(zram);
	atomic64_inc(&sddc->stats.dropped);
free_observe:
	kfree(observe);
put_manager:
	crystal_sddc_manager_put(sddc);
}

#if IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_KUNIT_TEST)

#define CRYSTAL_SDDC_TEST_SLOTS	2

struct crystal_sddc_test_ctx {
	struct zram zram;
	struct crystal_sddc sddc;
	struct crystal_sddc_slot slots[CRYSTAL_SDDC_TEST_SLOTS];
	struct zram_table_entry table[CRYSTAL_SDDC_TEST_SLOTS];
	struct zcomp comp;
};

static int crystal_sddc_test_compress_delta(struct zcomp_strm *zstrm,
		const void *ref, unsigned int ref_len, const void *src,
		unsigned int src_len, void *dst, unsigned int *dst_len,
		unsigned int out_limit)
{
	return -EOPNOTSUPP;
}

static int crystal_sddc_test_decompress_delta(struct zcomp_strm *zstrm,
		const void *src, unsigned int src_len, const void *ref,
		unsigned int ref_len, void *dst, unsigned int *dst_len)
{
	return -EOPNOTSUPP;
}

static const struct zcomp_backend_ops crystal_sddc_test_ops = {
	.compress_delta = crystal_sddc_test_compress_delta,
	.decompress_delta = crystal_sddc_test_decompress_delta,
};

static int crystal_sddc_state_test_init(struct kunit *test)
{
	struct crystal_sddc_test_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->zram.table = ctx->table;
	ctx->zram.sddc = &ctx->sddc;
	ctx->comp.ops = &crystal_sddc_test_ops;
	ctx->zram.comps[ZRAM_PRIMARY_COMP] = &ctx->comp;
	spin_lock_init(&ctx->zram.sddc_lock);

	ctx->sddc.zram = &ctx->zram;
	ctx->sddc.slots = ctx->slots;
	ctx->sddc.nr_slots = CRYSTAL_SDDC_TEST_SLOTS;
	spin_lock_init(&ctx->sddc.state_lock);
	spin_lock_init(&ctx->sddc.index_lock);
	ida_init(&ctx->sddc.ref_ids);
	xa_init(&ctx->sddc.refs);
	atomic_set(&ctx->sddc.active_ops, 0);
	init_waitqueue_head(&ctx->sddc.active_wait);

	test->priv = ctx;
	return 0;
}

static void crystal_sddc_state_test_exit(struct kunit *test)
{
	struct crystal_sddc_test_ctx *ctx = test->priv;

	mempool_destroy(ctx->sddc.workspace_pool);
	xa_destroy(&ctx->sddc.refs);
	ida_destroy(&ctx->sddc.ref_ids);
}

static bool crystal_sddc_test_job_matches(struct crystal_sddc_test_ctx *ctx,
		const struct crystal_sddc_job_key *key)
{
	bool matches;

	crystal_sddc_slot_lock(&ctx->zram, key->index);
	matches = crystal_sddc_job_matches_locked(&ctx->sddc, key);
	crystal_sddc_slot_unlock(&ctx->zram, key->index);
	return matches;
}

static bool crystal_sddc_test_snapshot_matches(
		struct crystal_sddc_test_ctx *ctx, u32 index,
		const struct crystal_sddc_snapshot *snapshot)
{
	bool matches;

	crystal_sddc_slot_lock(&ctx->zram, index);
	matches = crystal_sddc_snapshot_matches_locked(&ctx->zram, index,
			snapshot);
	crystal_sddc_slot_unlock(&ctx->zram, index);
	return matches;
}

static void crystal_sddc_ref_publish_test(struct kunit *test)
{
	struct crystal_sddc_test_ctx *ctx = test->priv;
	struct crystal_sddc_ref ref = {
		.sddc = &ctx->sddc,
		.cookie = { .id = 17, .generation = 5 },
	};
	void *entry;
	int ret;

	ret = xa_reserve(&ctx->sddc.refs, ref.cookie.id, GFP_KERNEL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	entry = xa_load(&ctx->sddc.refs, ref.cookie.id);
	KUNIT_ASSERT_TRUE(test, xa_is_zero(entry));

	KUNIT_EXPECT_TRUE(test, crystal_sddc_ref_publish(&ctx->sddc, &ref));
	KUNIT_EXPECT_PTR_EQ(test, xa_load(&ctx->sddc.refs, ref.cookie.id),
			&ref);
	KUNIT_EXPECT_FALSE(test, crystal_sddc_ref_publish(&ctx->sddc, &ref));
	KUNIT_EXPECT_PTR_EQ(test, xa_load(&ctx->sddc.refs, ref.cookie.id),
			&ref);
	xa_erase(&ctx->sddc.refs, ref.cookie.id);
}

static void crystal_sddc_ref_generation_test(struct kunit *test)
{
	struct crystal_sddc_test_ctx *ctx = test->priv;
	struct crystal_sddc_ref ref = {
		.sddc = &ctx->sddc,
		.cookie = { .id = 23, .generation = 11 },
	};
	struct crystal_sddc_cookie stale = ref.cookie;
	struct crystal_sddc_ref *pinned;
	int ret;

	refcount_set(&ref.refs, 1);
	ret = xa_reserve(&ctx->sddc.refs, ref.cookie.id, GFP_KERNEL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_TRUE(test,
			crystal_sddc_ref_publish(&ctx->sddc, &ref));

	stale.generation++;
	KUNIT_EXPECT_NULL(test, crystal_sddc_ref_pin(&ctx->sddc, &stale));
	KUNIT_EXPECT_EQ(test, refcount_read(&ref.refs), 1U);

	pinned = crystal_sddc_ref_pin(&ctx->sddc, &ref.cookie);
	KUNIT_ASSERT_PTR_EQ(test, pinned, &ref);
	KUNIT_EXPECT_EQ(test, refcount_read(&ref.refs), 2U);
	crystal_sddc_ref_put(pinned);
	KUNIT_EXPECT_EQ(test, refcount_read(&ref.refs), 1U);
	xa_erase(&ctx->sddc.refs, ref.cookie.id);
}

static void crystal_sddc_snapshot_identity_test(struct kunit *test)
{
	struct crystal_sddc_test_ctx *ctx = test->priv;
	struct crystal_sddc_slot *slot = &ctx->slots[0];
	struct crystal_sddc_snapshot snapshot;

	slot->mutation_seq = 41;
	slot->kind = CRYSTAL_SDDC_DELTA;
	slot->ref.id = 9;
	slot->ref.generation = 3;
	crystal_sddc_slot_lock(&ctx->zram, 0);
	crystal_sddc_snapshot_locked(&ctx->zram, 0, &snapshot);
	crystal_sddc_slot_unlock(&ctx->zram, 0);
	KUNIT_ASSERT_TRUE(test,
			crystal_sddc_test_snapshot_matches(ctx, 0, &snapshot));

	slot->mutation_seq++;
	KUNIT_EXPECT_FALSE(test,
			crystal_sddc_test_snapshot_matches(ctx, 0, &snapshot));
	slot->mutation_seq = snapshot.mutation_seq;
	slot->kind = CRYSTAL_SDDC_ALIAS;
	KUNIT_EXPECT_FALSE(test,
			crystal_sddc_test_snapshot_matches(ctx, 0, &snapshot));
	slot->kind = snapshot.kind;
	slot->ref.generation++;
	KUNIT_EXPECT_FALSE(test,
			crystal_sddc_test_snapshot_matches(ctx, 0, &snapshot));
	slot->ref = snapshot.ref;
	slot->ref.id++;
	KUNIT_EXPECT_FALSE(test,
			crystal_sddc_test_snapshot_matches(ctx, 0, &snapshot));
	slot->ref = snapshot.ref;
	KUNIT_EXPECT_TRUE(test,
			crystal_sddc_test_snapshot_matches(ctx, 0, &snapshot));
}

static void crystal_sddc_slot_owner_test(struct kunit *test)
{
	struct crystal_sddc_test_ctx *ctx = test->priv;
	struct crystal_sddc_slot *slot = &ctx->slots[0];
	struct crystal_sddc_ref ref = {
		.sddc = &ctx->sddc,
		.cookie = { .id = 31, .generation = 7 },
	};
	enum crystal_sddc_kind kind;
	int ret;

	refcount_set(&ref.refs, 3);
	ret = xa_reserve(&ctx->sddc.refs, ref.cookie.id, GFP_KERNEL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_TRUE(test,
			crystal_sddc_ref_publish(&ctx->sddc, &ref));
	slot->mutation_seq = 12;
	slot->kind = CRYSTAL_SDDC_DELTA;
	slot->ref = ref.cookie;
	slot->accounted_size = 80;
	slot->saved_size = 64;
	atomic64_set(&ctx->sddc.stats.deltas, 1);
	atomic64_set(&ctx->sddc.stats.delta_bytes, 80);
	atomic64_set(&ctx->sddc.stats.saved_bytes, 64);

	crystal_sddc_slot_lock(&ctx->zram, 0);
	kind = crystal_sddc_slot_free_locked(&ctx->zram, 0);
	crystal_sddc_slot_unlock(&ctx->zram, 0);
	KUNIT_EXPECT_EQ(test, kind, CRYSTAL_SDDC_DELTA);
	KUNIT_EXPECT_EQ(test, refcount_read(&ref.refs), 2U);
	KUNIT_EXPECT_EQ(test, atomic64_read(&ctx->sddc.stats.deltas),
			(s64)0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&ctx->sddc.stats.delta_bytes),
			(s64)0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&ctx->sddc.stats.saved_bytes),
			(s64)0);

	crystal_sddc_slot_lock(&ctx->zram, 0);
	kind = crystal_sddc_slot_free_locked(&ctx->zram, 0);
	crystal_sddc_slot_unlock(&ctx->zram, 0);
	KUNIT_EXPECT_EQ(test, kind, CRYSTAL_SDDC_NONE);
	KUNIT_EXPECT_EQ(test, refcount_read(&ref.refs), 2U);
	KUNIT_EXPECT_EQ(test, atomic64_read(&ctx->sddc.stats.delta_bytes),
			(s64)0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&ctx->sddc.stats.saved_bytes),
			(s64)0);
	xa_erase(&ctx->sddc.refs, ref.cookie.id);
}

static void crystal_sddc_stale_job_key_test(struct kunit *test)
{
	struct crystal_sddc_test_ctx *ctx = test->priv;
	struct crystal_sddc_job_key key = {
		.mutation_seq = 19,
		.handle = 0x1234,
		.index = 0,
		.size = 512,
		.prio = ZRAM_PRIMARY_COMP,
	};

	ctx->slots[0].mutation_seq = key.mutation_seq;
	ctx->table[0].handle = key.handle;
	crystal_sddc_set_obj_size(&ctx->zram, 0, key.size);
	KUNIT_ASSERT_TRUE(test, crystal_sddc_test_job_matches(ctx, &key));

	ctx->slots[0].mutation_seq++;
	KUNIT_EXPECT_FALSE(test, crystal_sddc_test_job_matches(ctx, &key));
	ctx->slots[0].mutation_seq = key.mutation_seq;
	ctx->table[0].handle++;
	KUNIT_EXPECT_FALSE(test, crystal_sddc_test_job_matches(ctx, &key));
	ctx->table[0].handle = key.handle;
	ctx->slots[0].kind = CRYSTAL_SDDC_ALIAS;
	KUNIT_EXPECT_FALSE(test, crystal_sddc_test_job_matches(ctx, &key));
}

static void crystal_sddc_flatten_snapshot_test(struct kunit *test)
{
	struct crystal_sddc_test_ctx *ctx = test->priv;
	struct crystal_sddc_snapshot snapshot;
	void *dst;
	size_t size = 123;
	int ret;

	ctx->sddc.workspace_pool = mempool_create(1,
			crystal_sddc_workspace_alloc, crystal_sddc_workspace_free,
			NULL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->sddc.workspace_pool);
	ctx->slots[0].mutation_seq = 27;
	ctx->slots[0].kind = CRYSTAL_SDDC_ALIAS;
	ctx->slots[0].ref.id = 2;
	ctx->slots[0].ref.generation = 4;
	crystal_sddc_slot_lock(&ctx->zram, 0);
	crystal_sddc_snapshot_locked(&ctx->zram, 0, &snapshot);
	crystal_sddc_slot_unlock(&ctx->zram, 0);
	ctx->slots[0].mutation_seq++;
	dst = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dst);

	ret = crystal_sddc_flatten(&ctx->zram, 0, &snapshot, dst, &size);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	KUNIT_EXPECT_EQ(test, size, (size_t)123);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->sddc.active_ops), 0);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&ctx->sddc.stats.flatten_failures), (s64)0);
}

static void crystal_sddc_delta_header_test(struct kunit *test)
{
	struct crystal_sddc_ref ref = {
		.cookie = { .id = 37, .generation = 13 },
		.size = 768,
	};
	struct crystal_sddc_delta_header header = {
		.magic = cpu_to_le32(CRYSTAL_SDDC_DELTA_MAGIC),
		.version = cpu_to_le16(CRYSTAL_SDDC_DELTA_VERSION),
		.header_size = cpu_to_le16(sizeof(header)),
		.ref_id = cpu_to_le32(ref.cookie.id),
		.ref_generation = cpu_to_le32(ref.cookie.generation),
		.ref_size = cpu_to_le32(ref.size),
		.target_size = cpu_to_le32(640),
	};
	struct crystal_sddc_delta_header bad;
	u32 wire_size = sizeof(header) + 8;

	KUNIT_ASSERT_TRUE(test,
			crystal_sddc_delta_header_valid(&ref, &header, wire_size));

	bad = header;
	bad.magic = cpu_to_le32(CRYSTAL_SDDC_DELTA_MAGIC ^ 1);
	KUNIT_EXPECT_FALSE(test,
			crystal_sddc_delta_header_valid(&ref, &bad, wire_size));
	bad = header;
	bad.version = cpu_to_le16(CRYSTAL_SDDC_DELTA_VERSION + 1);
	KUNIT_EXPECT_FALSE(test,
			crystal_sddc_delta_header_valid(&ref, &bad, wire_size));
	bad = header;
	bad.header_size = cpu_to_le16(sizeof(bad) - 1);
	KUNIT_EXPECT_FALSE(test,
			crystal_sddc_delta_header_valid(&ref, &bad, wire_size));
	bad = header;
	bad.ref_id = cpu_to_le32(ref.cookie.id + 1);
	KUNIT_EXPECT_FALSE(test,
			crystal_sddc_delta_header_valid(&ref, &bad, wire_size));
	bad = header;
	bad.ref_generation = cpu_to_le32(ref.cookie.generation + 1);
	KUNIT_EXPECT_FALSE(test,
			crystal_sddc_delta_header_valid(&ref, &bad, wire_size));
	bad = header;
	bad.ref_size = cpu_to_le32(ref.size + 1);
	KUNIT_EXPECT_FALSE(test,
			crystal_sddc_delta_header_valid(&ref, &bad, wire_size));
	bad = header;
	bad.target_size = 0;
	KUNIT_EXPECT_FALSE(test,
			crystal_sddc_delta_header_valid(&ref, &bad, wire_size));
	bad = header;
	bad.target_size = cpu_to_le32(PAGE_SIZE + 1);
	KUNIT_EXPECT_FALSE(test,
			crystal_sddc_delta_header_valid(&ref, &bad, wire_size));
	KUNIT_EXPECT_FALSE(test, crystal_sddc_delta_header_valid(&ref, &header,
			sizeof(header)));
	KUNIT_EXPECT_FALSE(test, crystal_sddc_delta_header_valid(&ref, &header,
			PAGE_SIZE + 1));
}

static void crystal_sddc_manager_admission_test(struct kunit *test)
{
	struct crystal_sddc_test_ctx *ctx = test->priv;
	struct crystal_sddc *held;

	held = crystal_sddc_manager_get(&ctx->zram);
	KUNIT_ASSERT_PTR_EQ(test, held, &ctx->sddc);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->sddc.active_ops), 1);

	spin_lock(&ctx->zram.sddc_lock);
	WRITE_ONCE(ctx->sddc.stopping, true);
	spin_unlock(&ctx->zram.sddc_lock);
	KUNIT_EXPECT_NULL(test, crystal_sddc_manager_get(&ctx->zram));
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->sddc.active_ops), 1);

	crystal_sddc_manager_put(held);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->sddc.active_ops), 0);
	KUNIT_EXPECT_NULL(test, crystal_sddc_manager_get(&ctx->zram));
}

static struct kunit_case crystal_sddc_state_test_cases[] = {
	KUNIT_CASE(crystal_sddc_ref_publish_test),
	KUNIT_CASE(crystal_sddc_ref_generation_test),
	KUNIT_CASE(crystal_sddc_snapshot_identity_test),
	KUNIT_CASE(crystal_sddc_slot_owner_test),
	KUNIT_CASE(crystal_sddc_stale_job_key_test),
	KUNIT_CASE(crystal_sddc_flatten_snapshot_test),
	KUNIT_CASE(crystal_sddc_delta_header_test),
	KUNIT_CASE(crystal_sddc_manager_admission_test),
	{}
};

static struct kunit_suite crystal_sddc_state_test_suite = {
	.name = "crystal-sddc-state",
	.init = crystal_sddc_state_test_init,
	.exit = crystal_sddc_state_test_exit,
	.test_cases = crystal_sddc_state_test_cases,
};

kunit_test_suite(crystal_sddc_state_test_suite);

#endif /* CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_KUNIT_TEST */
