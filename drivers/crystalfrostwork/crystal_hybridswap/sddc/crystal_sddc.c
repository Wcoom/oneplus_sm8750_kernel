// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/blkdev.h>
#include <linux/byteorder/generic.h>
#include <linux/highmem.h>
#include <linux/idr.h>
#include <linux/jhash.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>
#include <linux/zsmalloc.h>
#include <linux/bit_spinlock.h>
#include <asm/unaligned.h>

#if IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_KUNIT_TEST)
#include <kunit/test.h>
#endif

#include "../crystal_hybridswap_internal.h"
#include "../zram_drv.h"
#include "crystal_sddc.h"

/*
 * 17-bit buckets keep the Huawei-style 8-way table effective on large zram
 * devices. 16-bit buckets are easy to saturate once millions of 4K slots start
 * rotating; the extra table memory is small compared with the DELTA savings on
 * 12/16GB devices.
 */
#define CRYSTAL_SDDC_HASH_BITS		17
#define CRYSTAL_SDDC_HASH_BUCKETS	(1U << CRYSTAL_SDDC_HASH_BITS)
#define CRYSTAL_SDDC_BUCKET_WAYS	8
#define CRYSTAL_SDDC_SAMPLE_SIZE	16
#define CRYSTAL_SDDC_INDEX_MIN_SIZE	256
#define CRYSTAL_SDDC_OBSERVE_WORKS	2
#define CRYSTAL_SDDC_HASH_SEED		0x1e35a7bdU
#define CRYSTAL_SDDC_DELTA_GAIN_MIN	8
#define CRYSTAL_SDDC_DELTA_MIN_SIZE	(PAGE_SIZE / 8)
#define CRYSTAL_SDDC_SIMILARITY_MIN	16
#define CRYSTAL_SDDC_DELTA_MAGIC	0x43444453U
#define CRYSTAL_SDDC_ALIAS_MAGIC	0x41444453U
#define CRYSTAL_SDDC_ALIAS_V2_MAGIC	0x32414453U
#define CRYSTAL_SDDC_DELTA_VERSION	1
#define CRYSTAL_SDDC_ALIAS_V2_VERSION	2
#define CRYSTAL_SDDC_ALIAS_SIZE_BITS	13
#define CRYSTAL_SDDC_ALIAS_SIZE_MASK	((1U << CRYSTAL_SDDC_ALIAS_SIZE_BITS) - 1)
#define CRYSTAL_SDDC_ALIAS_TARGET_SHIFT	CRYSTAL_SDDC_ALIAS_SIZE_BITS
#define CRYSTAL_SDDC_ALIAS_VERSION_SHIFT	(CRYSTAL_SDDC_ALIAS_SIZE_BITS * 2)
#define CRYSTAL_SDDC_OBSERVE_BUDGET	64

#define CRYSTAL_SDDC_CELL_ID_BITS	24
#define CRYSTAL_SDDC_CELL_ID_MASK	((1U << CRYSTAL_SDDC_CELL_ID_BITS) - 1)
#define CRYSTAL_SDDC_CELL_SAMPLE_TAIL	BIT(24)
#define CRYSTAL_SDDC_CELL_REF		BIT(25)
#define CRYSTAL_SDDC_CELL_TAG_SHIFT	26
#define CRYSTAL_SDDC_CELL_TAG_MASK	(0x3fU << CRYSTAL_SDDC_CELL_TAG_SHIFT)
#define CRYSTAL_SDDC_MAX_SLOTS		CRYSTAL_SDDC_CELL_ID_MASK
#define CRYSTAL_SDDC_NO_SLOT		U32_MAX

enum crystal_sddc_sample_kind {
	CRYSTAL_SDDC_SAMPLE_HEAD = BIT(0),
	CRYSTAL_SDDC_SAMPLE_TAIL = BIT(1),
};

struct crystal_sddc_ref;

struct crystal_sddc_slot_state {
	struct crystal_sddc_cookie ref;
	/*
	 * The slot state owns one resident reference for as long as the state is
	 * installed.  Readers already hold the zram slot lock when they observe
	 * this pointer, so they can pin the current reference directly instead of
	 * taking the global refs XArray lock to rediscover it by cookie.  This
	 * mirrors Huawei's proxy-table read path: the current slot/proxy identity
	 * owns the backing object lifetime, and the read path only takes a
	 * temporary refcount pin before dropping the slot lock.
	 */
	struct crystal_sddc_ref *ref_obj;
	u16 accounted_size;
	u16 saved_size;
	u16 ref_size;
	u16 target_size;
	u32 page_hash;
	u8 kind;
};

static_assert(PAGE_SIZE <= U16_MAX);
static_assert(sizeof(struct crystal_sddc_slot_state) <= 32);

struct crystal_sddc_delta_header {
	__le32 magic;
	__le16 version;
	__le16 header_size;
	__le32 ref_id;
	__le32 ref_generation;
	__le32 ref_size;
	__le32 target_size;
};

struct crystal_sddc_alias_header_v2 {
	__le32 magic;
	__le32 ref_id;
	__le32 ref_generation;
	__le32 packed_sizes;
};

static_assert(sizeof(struct crystal_sddc_alias_header_v2) == 16);

struct crystal_sddc_workspace {
	void *ordinary;
	void *ref_data;
	void *wire;
};

struct crystal_sddc_candidate {
	struct crystal_sddc_bucket *bucket;
	u32 cell;
	u32 index;
	u8 way;
	u8 sample_kind;
};

struct crystal_sddc_bucket {
	/* Zero is empty. Non-zero cells pack a 24-bit slot/ref id, sample side,
	 * source type and hash tag in the same spirit as Huawei's SDDC table. */
	u32 cells[CRYSTAL_SDDC_BUCKET_WAYS];
};

static_assert(sizeof(struct crystal_sddc_bucket) ==
	      sizeof(u32) * CRYSTAL_SDDC_BUCKET_WAYS);

struct crystal_sddc_ranked_candidate {
	struct crystal_sddc_candidate candidate;
	u32 match_bytes;
	u32 ref_count;
	u32 source_size;
};

struct crystal_sddc;

/* A second work item lets the drain continue while the first callback is
 * still returning from the workqueue.
 */
struct crystal_sddc_observe_work {
	struct work_struct work;
	struct crystal_sddc *sddc;
	unsigned int id;
};

struct crystal_sddc_stats {
	atomic64_t queued;
	atomic64_t coalesced;
	atomic64_t dropped;
	atomic64_t ineligible;
	atomic64_t shutdown_discarded;
	atomic64_t worker_runs;
	atomic64_t observed;
	atomic64_t stale;
	atomic64_t indexed;
	atomic64_t refs;
	atomic64_t ref_bytes;
	atomic64_t wb_ref_pins;
	atomic64_t wb_ref_pin_bytes;
	atomic64_t wb_ref_pin_max;
	atomic64_t wb_ref_pin_bytes_max;
	atomic64_t wb_ref_pin_events;
	atomic64_t wb_ref_unpin_events;
	atomic64_t wb_deltas;
	atomic64_t wb_delta_bytes;
	atomic64_t wb_aliases;
	atomic64_t wb_alias_bytes;
	atomic64_t aliases;
	atomic64_t deltas;
	atomic64_t delta_bytes;
	atomic64_t alias_attempts;
	atomic64_t alias_hits;
	atomic64_t delta_attempts;
	atomic64_t delta_hits;
	atomic64_t delta_matches;
	atomic64_t delta_small_rejects;
	atomic64_t delta_no_gain;
	atomic64_t delta_match_bytes_max;
	atomic64_t saved_bytes;
	atomic64_t saved_bytes_total;
	atomic64_t released_aliases;
	atomic64_t released_deltas;
	atomic64_t released_saved_bytes;
	atomic64_t released_writeback_saved_bytes;
	atomic64_t released_rewrite_saved_bytes;
	atomic64_t released_recompress_saved_bytes;
	atomic64_t released_discard_saved_bytes;
	atomic64_t released_notify_free_saved_bytes;
	atomic64_t released_reset_saved_bytes;
	atomic64_t released_other_saved_bytes;
	atomic64_t conversion_failures;
	atomic64_t decode_failures;
	atomic64_t flatten_failures;
	atomic64_t integrity_checks;
	atomic64_t integrity_failures;
	atomic64_t integrity_skipped;
	atomic64_t integrity_hash_failures;
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

/*
 * Native ZMS DELTA/ALIAS objects keep their immutable reference resident.
 * Keep this owner state separate from resident slot_states so removing the
 * slot representation cannot drop the backend dependency prematurely.
 */
struct crystal_sddc_wb_state {
	u64 mutation_seq;
	struct crystal_sddc_cookie ref;
	struct crystal_sddc_ref *ref_obj;
	u32 ref_size;
	u32 target_size;
	u32 wire_size;
	u32 page_hash;
	u8 kind;
	bool accounted;
};

struct crystal_sddc {
	struct zram *zram;
	u64 *mutation_seq;
	struct xarray slot_states;
	struct xarray wb_states;
	struct crystal_sddc_bucket *exact_index;
	struct crystal_sddc_bucket *sample_index;
	struct workqueue_struct *workqueue;
	struct workqueue_struct *free_workqueue;
	struct crystal_sddc_observe_work observe_work[CRYSTAL_SDDC_OBSERVE_WORKS];
	struct crystal_sddc_workspace *observe_workspace;
	unsigned long *observe_pending;
	unsigned long nr_slots;
	spinlock_t state_lock;
	spinlock_t index_lock;
	spinlock_t active_lock;
	struct ida ref_ids;
	struct xarray refs;
	atomic_t next_generation;
	atomic_t active_ops;
	wait_queue_head_t active_wait;
	unsigned long observe_cursor;
	unsigned int observe_work_cursor;
	unsigned int pending;
	bool observe_worker_active;
	bool stopping;
	struct crystal_sddc_stats stats;
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
	__acquires(bitlock)
{
	bit_spin_lock(ZRAM_LOCK, &zram->table[index].flags);
}

static void crystal_sddc_slot_unlock(struct zram *zram, u32 index)
	__releases(bitlock)
{
	bit_spin_unlock(ZRAM_LOCK, &zram->table[index].flags);
}

static bool crystal_sddc_cookie_equal(const struct crystal_sddc_cookie *a,
		const struct crystal_sddc_cookie *b)
{
	return a->id == b->id && a->generation == b->generation;
}

static bool crystal_sddc_managed_kind(u8 kind)
{
	return kind == CRYSTAL_SDDC_REF ||
		kind == CRYSTAL_SDDC_ALIAS ||
		kind == CRYSTAL_SDDC_DELTA;
}

static bool crystal_sddc_ref_cookie_valid(
		const struct crystal_sddc_cookie *cookie)
{
	return cookie && cookie->id &&
		cookie->id <= CRYSTAL_SDDC_CELL_ID_MASK &&
		cookie->generation;
}

static bool crystal_sddc_ref_valid_common(struct crystal_sddc *sddc,
		const struct crystal_sddc_ref *ref)
{
	return sddc && sddc->zram && ref && ref->sddc == sddc &&
		crystal_sddc_ref_cookie_valid(&ref->cookie) &&
		ref->handle && ref->size >= CRYSTAL_SDDC_SAMPLE_SIZE &&
		ref->size <= PAGE_SIZE && ref->prio < ZRAM_MAX_COMPS &&
		refcount_read(&ref->refs);
}

static bool crystal_sddc_size_fits_state(u32 size)
{
	return size <= PAGE_SIZE;
}

static u16 crystal_sddc_state_size(u32 size)
{
	WARN_ON_ONCE(!crystal_sddc_size_fits_state(size));
	return (u16)size;
}

static bool crystal_sddc_state_sizes_fit(u32 accounted_size, u32 saved_size,
		u32 ref_size, u32 target_size)
{
	return crystal_sddc_size_fits_state(accounted_size) &&
		crystal_sddc_size_fits_state(saved_size) &&
		crystal_sddc_size_fits_state(ref_size) &&
		crystal_sddc_size_fits_state(target_size);
}

static bool crystal_sddc_ref_valid_for_kind(struct crystal_sddc *sddc,
		const struct crystal_sddc_ref *ref, u8 kind)
{
	struct zcomp *comp;

	if (!crystal_sddc_ref_valid_common(sddc, ref))
		return false;

	switch (kind) {
	case CRYSTAL_SDDC_REF:
	case CRYSTAL_SDDC_ALIAS:
		comp = sddc->zram->comps[ref->prio];
		return comp || ref->size == PAGE_SIZE;
	case CRYSTAL_SDDC_DELTA:
		comp = sddc->zram->comps[ref->prio];
		return comp && zcomp_supports_delta(comp);
	default:
		return false;
	}
}

static bool crystal_sddc_slot_state_storage_valid_locked(
		struct crystal_sddc *sddc, u32 index,
		const struct crystal_sddc_slot_state *state)
{
	struct zram *zram;
	size_t object_size;

	if (!sddc || !state || index >= sddc->nr_slots ||
	    !crystal_sddc_managed_kind(state->kind) ||
	    !state->ref_obj ||
	    !crystal_sddc_ref_cookie_valid(&state->ref) ||
	    state->accounted_size > PAGE_SIZE || state->saved_size > PAGE_SIZE ||
	    state->ref_size < CRYSTAL_SDDC_SAMPLE_SIZE ||
	    state->ref_size > PAGE_SIZE ||
	    state->target_size < CRYSTAL_SDDC_SAMPLE_SIZE ||
	    state->target_size > PAGE_SIZE)
		return false;
	if (!crystal_sddc_ref_valid_for_kind(sddc, state->ref_obj,
			state->kind) ||
	    !crystal_sddc_cookie_equal(&state->ref_obj->cookie, &state->ref) ||
	    state->ref_obj->size != state->ref_size)
		return false;

	zram = sddc->zram;
	if (!zram || !READ_ONCE(zram->table) ||
	    crystal_sddc_test_flag(zram, index, ZRAM_SAME))
		return false;

	object_size = crystal_sddc_obj_size(zram, index);
	switch (state->kind) {
	case CRYSTAL_SDDC_REF:
		return !zram->table[index].handle && !object_size &&
			!state->accounted_size && !state->saved_size &&
			state->target_size == state->ref_size;
	case CRYSTAL_SDDC_ALIAS:
		return !zram->table[index].handle && !object_size &&
			!state->accounted_size &&
			state->target_size == state->ref_size;
	case CRYSTAL_SDDC_DELTA:
		return zram->table[index].handle &&
			object_size > sizeof(struct crystal_sddc_delta_header) &&
			object_size <= PAGE_SIZE &&
			state->accounted_size == object_size;
	default:
		return false;
	}
}

static bool crystal_sddc_target_storage_valid(enum crystal_sddc_kind kind,
		unsigned long handle, u32 size)
{
	switch (kind) {
	case CRYSTAL_SDDC_ALIAS:
		return !handle && !size;
	case CRYSTAL_SDDC_DELTA:
		return handle && size > sizeof(struct crystal_sddc_delta_header) &&
			size <= PAGE_SIZE;
	default:
		return false;
	}
}

/* The caller holds the corresponding zram slot lock. */
static struct crystal_sddc_slot_state *
crystal_sddc_state_load_locked(struct crystal_sddc *sddc, u32 index)
{
	struct crystal_sddc_slot_state *state;

	state = xa_load(&sddc->slot_states, index);
	if (!state || xa_is_err(state) || xa_is_zero(state))
		return NULL;
	return state;
}

/* Unlike xa_load(), this preserves XA_ZERO_ENTRY so a slot free cannot
 * accidentally consume a reservation belonging to an in-flight conversion. */
static void *crystal_sddc_state_load_raw(struct crystal_sddc *sddc,
		u32 index)
{
	XA_STATE(xas, &sddc->slot_states, index);
	unsigned long flags;
	void *entry;

	xa_lock_irqsave(&sddc->slot_states, flags);
	entry = xas_load(&xas);
	xa_unlock_irqrestore(&sddc->slot_states, flags);
	return entry;
}

static void *crystal_sddc_xa_erase_if(struct xarray *xa,
		unsigned long index, void *expected);

static void *crystal_sddc_wb_state_load_raw(struct crystal_sddc *sddc,
		u32 index)
{
	XA_STATE(xas, &sddc->wb_states, index);
	unsigned long flags;
	void *entry;

	xa_lock_irqsave(&sddc->wb_states, flags);
	entry = xas_load(&xas);
	xa_unlock_irqrestore(&sddc->wb_states, flags);
	return entry;
}

static bool crystal_sddc_wb_state_install_locked(struct crystal_sddc *sddc,
		u32 index, struct crystal_sddc_wb_state *state)
{
	XA_STATE(xas, &sddc->wb_states, index);
	unsigned long flags;
	bool installed = false;

	xa_lock_irqsave(&sddc->wb_states, flags);
	if (xas_load(&xas) == XA_ZERO_ENTRY) {
		xas_store(&xas, state);
		installed = !xas_error(&xas);
	}
	xa_unlock_irqrestore(&sddc->wb_states, flags);
	return installed;
}

static void *crystal_sddc_wb_state_erase_locked(struct crystal_sddc *sddc,
		u32 index)
{
	XA_STATE(xas, &sddc->wb_states, index);
	unsigned long flags;
	void *entry;
	void *removed = NULL;

	/*
	 * The slot lock serializes the zram transition; keep the XArray
	 * comparison and erase atomic so a late reservation cannot be consumed.
	 */
	xa_lock_irqsave(&sddc->wb_states, flags);
	entry = xas_load(&xas);
	if (entry && entry != XA_ZERO_ENTRY && !xa_is_err(entry)) {
		xas_store(&xas, NULL);
		if (!xas_error(&xas))
			removed = entry;
	}
	xa_unlock_irqrestore(&sddc->wb_states, flags);
	return removed;
}

/*
 * Release an XArray reservation without treating a concurrent state as ours.
 * The normal xa_* API hides XA_ZERO_ENTRY, so use the advanced API while
 * holding the array lock and make the test-and-release one operation.
 */
static bool crystal_sddc_xa_release_reservation(struct xarray *xa,
		unsigned long index)
{
	XA_STATE(xas, xa, index);
	unsigned long flags;
	bool released = false;

	xa_lock_irqsave(xa, flags);
	if (xas_load(&xas) == XA_ZERO_ENTRY) {
		xas_store(&xas, NULL);
		released = !xas_error(&xas);
	}
	xa_unlock_irqrestore(xa, flags);

	return released;
}

/* Remove an XArray entry only if it is still the value observed by the
 * caller.  Slot locks do not cover the reservation abort path, so a plain
 * xa_erase() after a raw load could otherwise remove a newer state owner. */
static void *crystal_sddc_xa_erase_if(struct xarray *xa,
		unsigned long index, void *expected)
{
	XA_STATE(xas, xa, index);
	unsigned long flags;
	void *removed = NULL;

	if (!expected)
		return NULL;

	xa_lock_irqsave(xa, flags);
	if (xas_load(&xas) == expected) {
		xas_store(&xas, NULL);
		if (!xas_error(&xas))
			removed = expected;
	}
	xa_unlock_irqrestore(xa, flags);

	return removed;
}

static struct crystal_sddc_slot_state *
crystal_sddc_state_prepare(struct crystal_sddc *sddc, u32 index)
{
	struct crystal_sddc_slot_state *state;
	int ret;

	state = kzalloc(sizeof(*state), GFP_NOIO | __GFP_NOWARN);
	if (!state)
		return NULL;

	/* xa_insert(NULL) conditionally creates XA_ZERO_ENTRY and reports
	 * -EBUSY if another state or reservation already owns this slot. */
	ret = xa_insert(&sddc->slot_states, index, NULL,
			GFP_NOIO | __GFP_NOWARN);
	if (ret) {
		kfree(state);
		return NULL;
	}

	return state;
}

static bool crystal_sddc_state_identity_changed(
		struct crystal_sddc *sddc, u32 index, u64 mutation_seq)
{
	struct zram *zram = sddc->zram;
	bool changed;

	if (!zram || !READ_ONCE(zram->table) || index >= sddc->nr_slots)
		return false;

	crystal_sddc_slot_lock(zram, index);
	changed = sddc->mutation_seq[index] != mutation_seq;
	crystal_sddc_slot_unlock(zram, index);
	return changed;
}

static void crystal_sddc_state_abort(struct crystal_sddc *sddc, u32 index,
		u64 mutation_seq, struct crystal_sddc_slot_state *state)
{
	void *entry;

	if (!state)
		return;

	/* A failed publication leaves the reservation in place.  Do not erase a
	 * state node (or a newer owner's reservation) if the caller is late. */
	if (crystal_sddc_xa_release_reservation(&sddc->slot_states, index)) {
		/* A rewrite can observe our reservation and therefore cannot enqueue
		 * its latest key.  Requeue only when the identity actually changed;
		 * ordinary conversion rejection must not create a retry loop. */
		bool identity_changed = crystal_sddc_state_identity_changed(sddc,
				index, mutation_seq);

		kfree(state);
		if (identity_changed && sddc->workqueue)
			crystal_sddc_requeue_observation(sddc->zram, index);
		return;
	}

	entry = crystal_sddc_state_load_raw(sddc, index);
	if (entry == state) {
		/* The caller must not abort a published value.  Keeping it alive is
		 * safer than freeing memory still reachable from the XArray. */
		WARN_ON_ONCE(1);
		return;
	}
	/* A different owner may have won the slot after our reservation was
	 * removed.  Our private object was never published and can be freed. */
	if (entry && entry != XA_ZERO_ENTRY)
		WARN_ON_ONCE(1);
	kfree(state);
}

/* A prior xa_insert(NULL) makes this non-allocating under the slot lock. */
static bool crystal_sddc_state_install_locked(struct crystal_sddc *sddc,
		u32 index, struct crystal_sddc_slot_state *state)
{
	XA_STATE(xas, &sddc->slot_states, index);
	unsigned long flags;
	bool installed = false;

	if (WARN_ON_ONCE(!state))
		return false;

	/* Consume only our reservation; never overwrite a concurrent node. */
	xa_lock_irqsave(&sddc->slot_states, flags);
	if (xas_load(&xas) == XA_ZERO_ENTRY) {
		xas_store(&xas, state);
		installed = !xas_error(&xas);
	}
	xa_unlock_irqrestore(&sddc->slot_states, flags);

	return installed;
}

static void crystal_sddc_mutation_advance_locked(struct crystal_sddc *sddc,
		u32 index)
{
	if (!++sddc->mutation_seq[index])
		sddc->mutation_seq[index]++;
}

static struct crystal_sddc *crystal_sddc_manager_get(struct zram *zram)
{
	struct crystal_sddc *sddc;

	spin_lock(&zram->sddc_lock);
	sddc = zram->sddc;
	if (!sddc || READ_ONCE(sddc->stopping))
		sddc = NULL;
	else {
		/* zram->sddc_lock serializes admission with stop setting
		 * ->stopping, so no new manager user can appear after the stop
		 * path starts waiting for active_ops to drain.  The put side still
		 * uses active_lock to keep the final wakeup ordered with teardown,
		 * but taking that second global lock here only adds contention to
		 * the resident zram read fault path.
		 */
		atomic_inc(&sddc->active_ops);
	}
	spin_unlock(&zram->sddc_lock);
	return sddc;
}

static void crystal_sddc_manager_put(struct crystal_sddc *sddc)
{
	bool idle;

	/* Keep the final decrement and wakeup inside active_lock.  Teardown
	 * confirms idle while taking the same lock, so it cannot free @sddc while
	 * this function is still touching its waitqueue. */
	spin_lock(&sddc->active_lock);
	idle = atomic_dec_and_test(&sddc->active_ops);
	if (idle)
		wake_up_all(&sddc->active_wait);
	spin_unlock(&sddc->active_lock);
}

static void crystal_sddc_manager_wait_idle(struct crystal_sddc *sddc)
{
	for (;;) {
		wait_event(sddc->active_wait,
			!atomic_read(&sddc->active_ops));

		/* A waiter may observe zero while the final put still owns
		 * active_lock and is finishing wake_up_all(). */
		spin_lock(&sddc->active_lock);
		if (!atomic_read(&sddc->active_ops)) {
			spin_unlock(&sddc->active_lock);
			return;
		}
		spin_unlock(&sddc->active_lock);
	}
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

static void crystal_sddc_native_wb_account_pin(struct crystal_sddc *sddc,
		const struct crystal_sddc_wb_state *state)
{
	s64 pins;
	s64 pin_bytes;

	if (!sddc || !state)
		return;

	pins = atomic64_inc_return(&sddc->stats.wb_ref_pins);
	pin_bytes = atomic64_add_return(state->ref_size,
			&sddc->stats.wb_ref_pin_bytes);
	atomic64_inc(&sddc->stats.wb_ref_pin_events);
	crystal_sddc_atomic64_update_max(&sddc->stats.wb_ref_pin_max, pins);
	crystal_sddc_atomic64_update_max(&sddc->stats.wb_ref_pin_bytes_max,
			pin_bytes);
}

static void crystal_sddc_native_wb_unaccount_pin(struct crystal_sddc *sddc,
		const struct crystal_sddc_wb_state *state)
{
	if (!sddc || !state || !state->accounted)
		return;

	atomic64_dec(&sddc->stats.wb_ref_pins);
	atomic64_sub(state->ref_size, &sddc->stats.wb_ref_pin_bytes);
	atomic64_inc(&sddc->stats.wb_ref_unpin_events);
}

static const char *crystal_sddc_native_wb_disk_name(struct crystal_sddc *sddc)
{
	if (!sddc || !sddc->zram || !sddc->zram->disk)
		return "unknown";
	return sddc->zram->disk->disk_name;
}

static void crystal_sddc_native_wb_log_pin(struct crystal_sddc *sddc,
		u32 index, const struct crystal_sddc_wb_state *state,
		const char *event)
{
	if (!sddc || !state || !event)
		return;

	chs_log_ratelimited(CHS_LOG_INFO,
		"sddc_native_debug event=%s dev=%s index=%u kind=%u ref=%u:%u ref_size=%u target_size=%u wire_size=%u accounted=%u wb_ref_pins=%lld wb_ref_pin_bytes=%lld wb_ref_pin_max=%lld wb_ref_pin_bytes_max=%lld refs=%lld ref_bytes=%lld wb_deltas=%lld wb_aliases=%lld wb_delta_bytes=%lld wb_alias_bytes=%lld\n",
		event, crystal_sddc_native_wb_disk_name(sddc), index,
		state->kind, state->ref.id, state->ref.generation,
		state->ref_size, state->target_size, state->wire_size,
		state->accounted,
		(long long)atomic64_read(&sddc->stats.wb_ref_pins),
		(long long)atomic64_read(&sddc->stats.wb_ref_pin_bytes),
		(long long)atomic64_read(&sddc->stats.wb_ref_pin_max),
		(long long)atomic64_read(&sddc->stats.wb_ref_pin_bytes_max),
		(long long)atomic64_read(&sddc->stats.refs),
		(long long)atomic64_read(&sddc->stats.ref_bytes),
		(long long)atomic64_read(&sddc->stats.wb_deltas),
		(long long)atomic64_read(&sddc->stats.wb_aliases),
		(long long)atomic64_read(&sddc->stats.wb_delta_bytes),
		(long long)atomic64_read(&sddc->stats.wb_alias_bytes));
}

static u32 crystal_sddc_page_hash(const void *page_data)
{
	u32 hash = jhash(page_data, PAGE_SIZE,
			 CRYSTAL_SDDC_HASH_SEED ^ PAGE_SIZE);

	/*
	 * Reserve zero for "hash not initialised". jhash may theoretically
	 * return zero, so normalise it to keep diagnostic coverage unambiguous.
	 */
	return hash ?: 1;
}

static int crystal_sddc_hash_stream_page(struct crystal_sddc *sddc,
		const void *stream, u32 stream_size, u32 prio,
		void *scratch_page, u32 *hash)
{
	struct zcomp_strm *zstrm;
	struct zcomp *comp;
	int ret;

	if (!sddc || !sddc->zram || !stream || !hash ||
	    stream_size < CRYSTAL_SDDC_SAMPLE_SIZE ||
	    stream_size > PAGE_SIZE || prio >= ZRAM_MAX_COMPS)
		return -EIO;

	if (stream_size == PAGE_SIZE) {
		*hash = crystal_sddc_page_hash(stream);
		return 0;
	}

	if (!scratch_page)
		return -EIO;
	comp = sddc->zram->comps[prio];
	if (!comp)
		return -EIO;

	zstrm = zcomp_stream_get(comp);
	if (!zstrm)
		return -ENOMEM;
	ret = zcomp_decompress(zstrm, stream, stream_size, scratch_page);
	zcomp_stream_put(comp);
	if (ret)
		return -EIO;

	*hash = crystal_sddc_page_hash(scratch_page);
	return 0;
}

static void crystal_sddc_check_page_hash(struct crystal_sddc *sddc,
		u32 index, u64 mutation_seq, const struct crystal_sddc_ref *ref,
		enum crystal_sddc_kind kind, u32 ref_size, u32 target_size,
		u32 wire_size, u32 expected_hash, const void *page_data,
		const char *path)
{
	u32 actual_hash;

	if (!sddc || !page_data)
		return;
	if (!expected_hash) {
		atomic64_inc(&sddc->stats.integrity_skipped);
		return;
	}

	atomic64_inc(&sddc->stats.integrity_checks);
	actual_hash = crystal_sddc_page_hash(page_data);
	if (actual_hash == expected_hash)
		return;

	atomic64_inc(&sddc->stats.integrity_failures);
	chs_log_ratelimited(CHS_LOG_ERR,
		"sddc_integrity mismatch path=%s dev=%s index=%u kind=%u ref=%u:%u ref_size=%u target_size=%u wire_size=%u mutation_seq=%llu expected_hash=0x%08x actual_hash=0x%08x ref_refs=%u\n",
		path ? path : "unknown", crystal_sddc_native_wb_disk_name(sddc),
		index, kind, ref ? ref->cookie.id : 0,
		ref ? ref->cookie.generation : 0, ref_size, target_size,
		wire_size, mutation_seq, expected_hash, actual_hash,
		ref ? refcount_read(&ref->refs) : 0);
}

static struct crystal_sddc_workspace *
crystal_sddc_workspace_alloc(gfp_t gfp_mask)
{
	struct crystal_sddc_workspace *workspace;

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

static void crystal_sddc_workspace_free(struct crystal_sddc_workspace *workspace)
{
	if (!workspace)
		return;
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

	/* Conversion runs from a reclaim-capable workqueue.  Keep allocation
	 * below the zram reclaim path and treat failure as an ordinary miss. */
	ref = kzalloc(sizeof(*ref), GFP_NOIO | __GFP_NOWARN);
	if (!ref)
		return ERR_PTR(-ENOMEM);

	id = ida_alloc_range(&sddc->ref_ids, 1, CRYSTAL_SDDC_CELL_ID_MASK,
			GFP_NOIO | __GFP_NOWARN);
	if (id < 0) {
		kfree(ref);
		return ERR_PTR(id);
	}

	ret = xa_insert(&sddc->refs, id, NULL, GFP_NOIO | __GFP_NOWARN);
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
	/* Only the allocator's reservation may be released here.  A failed
	 * publication must never erase a newer entry reusing the same ID. */
	if (WARN_ON_ONCE(!crystal_sddc_xa_release_reservation(&sddc->refs,
			ref->cookie.id)))
		return;
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
	if (xas_load(&xas) == XA_ZERO_ENTRY) {
		xas_store(&xas, ref);
		published = !xas_error(&xas);
	}
	xa_unlock_irqrestore(&sddc->refs, flags);
	return published;
}

static struct crystal_sddc_ref *
crystal_sddc_ref_pin_id(struct crystal_sddc *sddc, u32 id)
{
	struct crystal_sddc_ref *ref;
	unsigned long flags;

	if (!sddc || !id || id > CRYSTAL_SDDC_CELL_ID_MASK)
		return NULL;

	xa_lock_irqsave(&sddc->refs, flags);
	ref = xa_load(&sddc->refs, id);
	if (!ref || xa_is_err(ref) || xa_is_zero(ref) || ref->sddc != sddc ||
	    ref->cookie.id != id ||
	    !crystal_sddc_ref_cookie_valid(&ref->cookie) || !ref->handle ||
	    !refcount_inc_not_zero(&ref->refs))
		ref = NULL;
	xa_unlock_irqrestore(&sddc->refs, flags);
	return ref;
}

static struct crystal_sddc_ref *
crystal_sddc_ref_pin(struct crystal_sddc *sddc,
		const struct crystal_sddc_cookie *cookie)
{
	struct crystal_sddc_ref *ref;
	unsigned long flags;

	if (!sddc || !crystal_sddc_ref_cookie_valid(cookie))
		return NULL;

	xa_lock_irqsave(&sddc->refs, flags);
	ref = xa_load(&sddc->refs, cookie->id);
	if (!ref || xa_is_err(ref) || xa_is_zero(ref) || ref->sddc != sddc ||
	    !crystal_sddc_cookie_equal(&ref->cookie, cookie) || !ref->handle ||
	    !refcount_inc_not_zero(&ref->refs))
		ref = NULL;
	xa_unlock_irqrestore(&sddc->refs, flags);
	return ref;
}

static struct crystal_sddc_ref *
crystal_sddc_slot_state_pin_ref_locked(struct crystal_sddc *sddc,
		const struct crystal_sddc_slot_state *state, u8 kind)
{
	struct crystal_sddc_ref *ref;

	if (!state || state->kind != kind ||
	    !crystal_sddc_ref_cookie_valid(&state->ref) ||
	    state->ref_size < CRYSTAL_SDDC_SAMPLE_SIZE ||
	    state->ref_size > PAGE_SIZE ||
	    state->target_size < CRYSTAL_SDDC_SAMPLE_SIZE ||
	    state->target_size > PAGE_SIZE)
		return NULL;

	ref = state->ref_obj;
	if (!crystal_sddc_ref_valid_for_kind(sddc, ref, kind) ||
	    !crystal_sddc_cookie_equal(&ref->cookie, &state->ref) ||
	    ref->size != state->ref_size ||
	    ((kind == CRYSTAL_SDDC_REF || kind == CRYSTAL_SDDC_ALIAS) &&
	     state->target_size != state->ref_size) ||
	    !refcount_inc_not_zero(&ref->refs))
		return NULL;

	return ref;
}

static struct crystal_sddc_ref *
crystal_sddc_wb_state_pin_ref_locked(struct crystal_sddc *sddc,
		const struct crystal_sddc_wb_state *state, u8 kind)
{
	struct crystal_sddc_ref *ref;

	if (!state || state->kind != kind ||
	    !crystal_sddc_ref_cookie_valid(&state->ref) ||
	    state->ref_size < CRYSTAL_SDDC_SAMPLE_SIZE ||
	    state->ref_size > PAGE_SIZE ||
	    state->target_size < CRYSTAL_SDDC_SAMPLE_SIZE ||
	    state->target_size > PAGE_SIZE)
		return NULL;

	ref = state->ref_obj;
	if (!crystal_sddc_ref_valid_for_kind(sddc, ref, kind) ||
	    !crystal_sddc_cookie_equal(&ref->cookie, &state->ref) ||
	    ref->size != state->ref_size ||
	    ((kind == CRYSTAL_SDDC_ALIAS || kind == CRYSTAL_SDDC_REF) &&
	     state->target_size != state->ref_size) ||
	    !refcount_inc_not_zero(&ref->refs))
		return NULL;

	return ref;
}

static void crystal_sddc_ref_put(struct crystal_sddc_ref *ref)
{
	struct crystal_sddc *sddc;
	unsigned long flags;
	bool dead = false;

	if (!ref)
		return;
	if (refcount_dec_not_one(&ref->refs))
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

/* ref_pin() contributes one temporary reference.  Index heuristics must
 * compare the resident owner/consumer count, not the snapshot's pin. */
static u32 crystal_sddc_ref_count_without_pin(
		const struct crystal_sddc_ref *ref)
{
	u32 count = refcount_read(&ref->refs);

	return count ? count - 1 : 0;
}

static void crystal_sddc_ref_drop_cookie(struct crystal_sddc *sddc,
		const struct crystal_sddc_cookie *cookie)
{
	struct crystal_sddc_ref *ref;
	unsigned long flags;
	bool dead = false;

	if (WARN_ON_ONCE(!sddc || !crystal_sddc_ref_cookie_valid(cookie)))
		return;

	xa_lock_irqsave(&sddc->refs, flags);
	ref = xa_load(&sddc->refs, cookie->id);
	if (WARN_ON_ONCE(!ref || xa_is_err(ref) || xa_is_zero(ref) ||
			 ref->sddc != sddc ||
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

static void crystal_sddc_slot_state_drop_ref(struct crystal_sddc *sddc,
		struct crystal_sddc_slot_state *state)
{
	struct crystal_sddc_ref *ref;

	if (!state || !crystal_sddc_managed_kind(state->kind))
		return;

	ref = state->ref_obj;
	state->ref_obj = NULL;
	if (!ref) {
		crystal_sddc_ref_drop_cookie(sddc, &state->ref);
		return;
	}

	WARN_ON_ONCE(ref->sddc != sddc ||
		      !crystal_sddc_cookie_equal(&ref->cookie, &state->ref));
	crystal_sddc_ref_put(ref);
}

static void crystal_sddc_copy_ref(struct crystal_sddc_ref *ref, void *dst)
{
	void *src;

	src = zs_map_object(ref->sddc->zram->mem_pool, ref->handle, ZS_MM_RO);
	memcpy(dst, src, ref->size);
	zs_unmap_object(ref->sddc->zram->mem_pool, ref->handle);
}

static bool crystal_sddc_job_matches_locked(struct crystal_sddc *sddc,
		const struct crystal_sddc_job_key *key, bool own_reservation)
{
	struct zram *zram = sddc->zram;
	void *state_entry;

	if (key->index >= sddc->nr_slots)
		return false;
	if (sddc->mutation_seq[key->index] != key->mutation_seq)
		return false;
	state_entry = crystal_sddc_state_load_raw(sddc, key->index);
	/* A conversion reserves its target before taking this lock.  That
	 * reservation is valid only for the caller which explicitly owns it;
	 * ordinary observation validation must reject every reservation. */
	if (own_reservation) {
		if (state_entry != XA_ZERO_ENTRY)
			return false;
	} else if (state_entry) {
		return false;
	}
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

static u32 crystal_sddc_index_hash_tag(u32 hash)
{
	return (hash >> CRYSTAL_SDDC_HASH_BITS) &
		(CRYSTAL_SDDC_CELL_TAG_MASK >> CRYSTAL_SDDC_CELL_TAG_SHIFT);
}

static u32 crystal_sddc_index_cell_id(u32 cell)
{
	return cell & CRYSTAL_SDDC_CELL_ID_MASK;
}

static bool crystal_sddc_index_cell_ref(u32 cell)
{
	return cell & CRYSTAL_SDDC_CELL_REF;
}

static u8 crystal_sddc_index_cell_sample(u32 cell)
{
	return cell & CRYSTAL_SDDC_CELL_SAMPLE_TAIL ?
		CRYSTAL_SDDC_SAMPLE_TAIL : CRYSTAL_SDDC_SAMPLE_HEAD;
}

static u32 crystal_sddc_index_cell_tag(u32 cell)
{
	return (cell & CRYSTAL_SDDC_CELL_TAG_MASK) >>
		CRYSTAL_SDDC_CELL_TAG_SHIFT;
}

static bool crystal_sddc_equal_u64(const u8 *a, const u8 *b)
{
	return !(get_unaligned((const u64 *)a) ^
		 get_unaligned((const u64 *)b));
}

static bool crystal_sddc_equal_sample16(const u8 *a, const u8 *b)
{
	return crystal_sddc_equal_u64(a, b) &&
		crystal_sddc_equal_u64(a + sizeof(u64), b + sizeof(u64));
}

static u32 crystal_sddc_index_slot_cell(u32 index, u32 hash, u8 sample_kind)
{
	u32 id = index + 1;

	if (!id || id > CRYSTAL_SDDC_CELL_ID_MASK)
		return 0;

	return id |
		(sample_kind == CRYSTAL_SDDC_SAMPLE_TAIL ?
		 CRYSTAL_SDDC_CELL_SAMPLE_TAIL : 0) |
		(crystal_sddc_index_hash_tag(hash) <<
		 CRYSTAL_SDDC_CELL_TAG_SHIFT);
}

static u32 crystal_sddc_index_ref_cell(u32 old_cell, u32 ref_id)
{
	if (!ref_id || ref_id > CRYSTAL_SDDC_CELL_ID_MASK)
		return 0;

	return (old_cell & (CRYSTAL_SDDC_CELL_SAMPLE_TAIL |
			    CRYSTAL_SDDC_CELL_TAG_MASK)) |
		CRYSTAL_SDDC_CELL_REF | ref_id;
}

static u32 crystal_sddc_index_slot(u32 cell)
{
	return crystal_sddc_index_cell_id(cell) - 1;
}

static bool crystal_sddc_candidate_same_source(
		const struct crystal_sddc_candidate *candidate, u32 cell)
{
	return (candidate->cell & (CRYSTAL_SDDC_CELL_REF |
				   CRYSTAL_SDDC_CELL_ID_MASK)) ==
		(cell & (CRYSTAL_SDDC_CELL_REF |
			 CRYSTAL_SDDC_CELL_ID_MASK));
}

static void crystal_sddc_index_quality(struct crystal_sddc *sddc,
		u32 cell, u32 *ref_count, u32 *size)
{
	struct zram *zram = sddc->zram;
	struct crystal_sddc_slot_state *state;
	struct crystal_sddc_ref *ref = NULL;
	void *entry;
	u32 index;

	*ref_count = 0;
	*size = 0;
	if (!cell)
		return;

	if (crystal_sddc_index_cell_ref(cell)) {
		ref = crystal_sddc_ref_pin_id(sddc,
				crystal_sddc_index_cell_id(cell));
		if (crystal_sddc_ref_valid_common(sddc, ref)) {
			*ref_count = crystal_sddc_ref_count_without_pin(ref);
			*size = ref->size;
		}
		crystal_sddc_ref_put(ref);
		return;
	}

	index = crystal_sddc_index_slot(cell);
	if (index >= sddc->nr_slots || !zram->table) {
		/* An unresolvable cell is protected from eviction.  It will be
		 * rejected by source validation and naturally replaced later. */
		*ref_count = U32_MAX;
		*size = U32_MAX;
		return;
	}

	/* Insertion runs under index_lock, while publication runs under a slot
	 * lock and then takes index_lock.  Never wait for the slot here: a
	 * blocking acquisition would invert that order and can deadlock against
	 * a concurrent publication.  A busy cell is simply protected for this
	 * insertion attempt and will be reconsidered on a later replacement. */
	if (!bit_spin_trylock(ZRAM_LOCK, &zram->table[index].flags)) {
		*ref_count = U32_MAX;
		*size = U32_MAX;
		return;
	}
	entry = crystal_sddc_state_load_raw(sddc, index);
	if (entry == XA_ZERO_ENTRY || xa_is_err(entry)) {
		*ref_count = U32_MAX;
		*size = U32_MAX;
		bit_spin_unlock(ZRAM_LOCK, &zram->table[index].flags);
		return;
	}
	if (entry) {
		state = entry;
		if (state->kind == CRYSTAL_SDDC_REF &&
		    crystal_sddc_slot_state_storage_valid_locked(sddc,
			    index, state))
			ref = crystal_sddc_slot_state_pin_ref_locked(sddc,
					state, CRYSTAL_SDDC_REF);
	}
	if (crystal_sddc_ref_valid_common(sddc, ref)) {
		*ref_count = crystal_sddc_ref_count_without_pin(ref);
		*size = ref->size;
	} else if (!entry) {
		*size = crystal_sddc_obj_size(zram, index);
	}
	bit_spin_unlock(ZRAM_LOCK, &zram->table[index].flags);
	crystal_sddc_ref_put(ref);
}

/* Huawei's table keeps highly shared references resident even when a new
 * object has the same sample score.  Ordinary objects (ref_count == 0) and
 * one-use references are allowed to rotate on a tie so the table does not
 * become permanently pinned by its first occupants. */
static bool crystal_sddc_index_candidate_worse(u32 incumbent_ref,
		u32 incumbent_size, u32 candidate_ref, u32 candidate_size)
{
	if (candidate_ref != incumbent_ref)
		return candidate_ref < incumbent_ref;
	if (candidate_size != incumbent_size)
		return candidate_size < incumbent_size;
	return incumbent_ref <= 2;
}

static void crystal_sddc_index_insert(struct crystal_sddc *sddc,
		struct crystal_sddc_bucket *index, u32 hash, u8 sample_kind,
		u32 slot_index)
{
	struct crystal_sddc_bucket *bucket;
	u32 replacement = U32_MAX;
	u32 replacement_ref = 0;
	u32 replacement_size = 0;
	u32 cell;
	unsigned int i;

	if (slot_index >= sddc->nr_slots)
		return;
	cell = crystal_sddc_index_slot_cell(slot_index, hash, sample_kind);
	if (!cell)
		return;

	bucket = &index[hash & (CRYSTAL_SDDC_HASH_BUCKETS - 1)];
	for (i = 0; i < CRYSTAL_SDDC_BUCKET_WAYS; i++) {
		u32 ref_count;
		u32 size;

		if (bucket->cells[i] == cell)
			return;
		if (!bucket->cells[i]) {
			replacement = i;
			replacement_ref = 0;
			replacement_size = 0;
			break;
		}
		crystal_sddc_index_quality(sddc, bucket->cells[i], &ref_count,
				&size);
		if (replacement == U32_MAX ||
			crystal_sddc_index_candidate_worse(replacement_ref,
				replacement_size, ref_count, size)) {
			replacement = i;
			replacement_ref = ref_count;
			replacement_size = size;
		}
	}
	/* A busy or otherwise unresolvable cell is protected.  If every way is
	 * protected, defer this insertion rather than evicting one blindly. */
	if (replacement == U32_MAX || replacement_ref == U32_MAX)
		return;
	bucket->cells[replacement] = cell;
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
		u32 cell = bucket->cells[i];
		bool duplicate = false;

		if (!cell)
			continue;
		if (crystal_sddc_index_cell_tag(cell) !=
		    crystal_sddc_index_hash_tag(hash))
			continue;
		if (sample_kind &&
		    crystal_sddc_index_cell_sample(cell) != sample_kind)
			continue;
		for (j = 0; j < count; j++) {
			if (crystal_sddc_candidate_same_source(&candidates[j],
					cell)) {
				candidates[j].sample_kind |= sample_kind;
				duplicate = true;
				break;
			}
		}
		if (!duplicate) {
			candidates[count].bucket = bucket;
			candidates[count].cell = cell;
			candidates[count].index =
				crystal_sddc_index_cell_ref(cell) ?
				CRYSTAL_SDDC_NO_SLOT :
				crystal_sddc_index_slot(cell);
			candidates[count].way = i;
			candidates[count].sample_kind = sample_kind ?
				sample_kind : CRYSTAL_SDDC_SAMPLE_HEAD;
			count++;
		}
	}

	return count;
}

static u32 crystal_sddc_head_match_bytes(const u8 *target, u32 target_size,
		const u8 *source, u32 source_size)
{
	u32 common;
	u32 score = CRYSTAL_SDDC_SAMPLE_SIZE;
	u32 offset;

	if (!crystal_sddc_equal_sample16(target, source))
		return 0;
	common = min(target_size, source_size);
	for (offset = CRYSTAL_SDDC_SAMPLE_SIZE;
	     offset + sizeof(u64) <= common; offset += sizeof(u64)) {
		if (crystal_sddc_equal_u64(target + offset, source + offset))
			score += sizeof(u64);
	}
	return score;
}

static u32 crystal_sddc_tail_match_bytes(const u8 *target, u32 target_size,
		const u8 *source, u32 source_size)
{
	u32 target_offset = target_size - CRYSTAL_SDDC_SAMPLE_SIZE;
	u32 source_offset = source_size - CRYSTAL_SDDC_SAMPLE_SIZE;
	u32 score = CRYSTAL_SDDC_SAMPLE_SIZE;

	if (!crystal_sddc_equal_sample16(target + target_offset,
					 source + source_offset))
		return 0;
	while (target_offset >= sizeof(u64) && source_offset >= sizeof(u64)) {
		target_offset -= sizeof(u64);
		source_offset -= sizeof(u64);
		if (crystal_sddc_equal_u64(target + target_offset,
					   source + source_offset))
			score += sizeof(u64);
	}
	return score;
}

static u32 crystal_sddc_match_bytes(const void *target, u32 target_size,
		const void *source, u32 source_size, u8 sample_kind)
{
	u32 score = 0;

	if (target_size < CRYSTAL_SDDC_SAMPLE_SIZE ||
	    source_size < CRYSTAL_SDDC_SAMPLE_SIZE)
		return 0;
	if (sample_kind & CRYSTAL_SDDC_SAMPLE_HEAD)
		score = crystal_sddc_head_match_bytes(target, target_size, source,
				source_size);
	if (sample_kind & CRYSTAL_SDDC_SAMPLE_TAIL)
		score = max(score, crystal_sddc_tail_match_bytes(target,
				target_size, source, source_size));
	return score;
}

static bool crystal_sddc_sample_eligible(u32 size)
{
	return size > CRYSTAL_SDDC_INDEX_MIN_SIZE && size <= PAGE_SIZE;
}

static void
crystal_sddc_rank_data(const struct crystal_sddc_job_key *target,
		       const void *target_data,
		       const struct crystal_sddc_candidate *candidate,
		       const void *source_data, u32 source_size, u32 ref_count,
		       struct crystal_sddc_ranked_candidate *ranked)
{
	u32 match_bytes = 0;

	if ((candidate->sample_kind & CRYSTAL_SDDC_SAMPLE_HEAD) &&
	    source_size == target->size &&
	    !memcmp(source_data, target_data, target->size))
		match_bytes = target->size;
	else if ((source_size == PAGE_SIZE) == (target->size == PAGE_SIZE))
		match_bytes = crystal_sddc_match_bytes(target_data,
						       target->size,
						       source_data, source_size,
						       candidate->sample_kind);

	ranked->candidate = *candidate;
	ranked->match_bytes = match_bytes;
	ranked->ref_count = ref_count;
	ranked->source_size = source_size;
}

static bool
crystal_sddc_rank_candidate(struct crystal_sddc *sddc,
			    const struct crystal_sddc_job_key *target,
			    const void *target_data,
			    const struct crystal_sddc_candidate *candidate,
			    struct crystal_sddc_ranked_candidate *ranked)
{
	struct crystal_sddc_slot_state *state;
	struct crystal_sddc_ref *ref;
	struct zram *zram = sddc->zram;
	unsigned long handle;
	void *state_entry;
	u32 source_size;
	u32 source_prio;
	void *source_data;

	if (crystal_sddc_index_cell_ref(candidate->cell)) {
		ref = crystal_sddc_ref_pin_id(sddc,
				crystal_sddc_index_cell_id(candidate->cell));
		if (!ref)
			return false;
		if (!crystal_sddc_ref_valid_common(sddc, ref) ||
		    ref->prio != target->prio) {
			crystal_sddc_ref_put(ref);
			return false;
		}

		source_data =
			zs_map_object(zram->mem_pool, ref->handle, ZS_MM_RO);
		crystal_sddc_rank_data(target, target_data, candidate,
				       source_data, ref->size,
				       crystal_sddc_ref_count_without_pin(ref),
				       ranked);
		zs_unmap_object(zram->mem_pool, ref->handle);
		crystal_sddc_ref_put(ref);
		return true;
	}

	if (candidate->index == target->index ||
	    candidate->index >= sddc->nr_slots)
		return false;

	crystal_sddc_slot_lock(zram, candidate->index);
	state_entry = crystal_sddc_state_load_raw(sddc, candidate->index);
	if (state_entry == XA_ZERO_ENTRY || xa_is_err(state_entry) ||
	    crystal_sddc_test_flag(zram, candidate->index, ZRAM_SAME) ||
	    crystal_sddc_test_flag(zram, candidate->index, ZRAM_WB) ||
	    crystal_sddc_test_flag(zram, candidate->index, ZRAM_UNDER_WB))
		goto unlock;

	state = state_entry;
	if (state && state->kind == CRYSTAL_SDDC_REF) {
		if (!crystal_sddc_slot_state_storage_valid_locked(sddc,
				candidate->index, state))
			goto unlock;
		ref = crystal_sddc_slot_state_pin_ref_locked(sddc, state,
				CRYSTAL_SDDC_REF);
		crystal_sddc_slot_unlock(zram, candidate->index);
		if (!ref)
			return false;
		if (!crystal_sddc_ref_valid_common(sddc, ref) ||
		    ref->prio != target->prio) {
			crystal_sddc_ref_put(ref);
			return false;
		}

		source_data =
			zs_map_object(zram->mem_pool, ref->handle, ZS_MM_RO);
		crystal_sddc_rank_data(target, target_data, candidate,
				       source_data, ref->size,
				       crystal_sddc_ref_count_without_pin(ref),
				       ranked);
		zs_unmap_object(zram->mem_pool, ref->handle);
		crystal_sddc_ref_put(ref);
		return true;
	}
	if (state)
		goto unlock;

	handle = zram->table[candidate->index].handle;
	source_size = crystal_sddc_obj_size(zram, candidate->index);
	source_prio = crystal_sddc_priority(zram, candidate->index);
	if (!handle || source_size < CRYSTAL_SDDC_SAMPLE_SIZE ||
	    source_size > PAGE_SIZE || source_prio != target->prio)
		goto unlock;

	source_data = zs_map_object(zram->mem_pool, handle, ZS_MM_RO);
	crystal_sddc_rank_data(target, target_data, candidate, source_data,
			       source_size, 0, ranked);
	zs_unmap_object(zram->mem_pool, handle);
	crystal_sddc_slot_unlock(zram, candidate->index);
	return true;

unlock:
	crystal_sddc_slot_unlock(zram, candidate->index);
	return false;
}

static int crystal_sddc_ranked_candidate_cmp(const void *left,
		const void *right)
{
	const struct crystal_sddc_ranked_candidate *a = left;
	const struct crystal_sddc_ranked_candidate *b = right;

	if (a->match_bytes != b->match_bytes)
		return a->match_bytes < b->match_bytes ? 1 : -1;
	if (a->ref_count != b->ref_count)
		return a->ref_count < b->ref_count ? 1 : -1;
	if (a->source_size != b->source_size)
		return a->source_size < b->source_size ? 1 : -1;
	return 0;
}

static unsigned int crystal_sddc_rank_candidates(struct crystal_sddc *sddc,
		const struct crystal_sddc_job_key *target, const void *target_data,
		struct crystal_sddc_candidate *candidates, unsigned int count,
		struct crystal_sddc_ranked_candidate *ranked)
{
	unsigned int i;
	unsigned int ranked_count = 0;

	for (i = 0; i < count; i++) {
		if (crystal_sddc_rank_candidate(sddc, target, target_data,
				&candidates[i], &ranked[ranked_count]))
			ranked_count++;
	}
	sort(ranked, ranked_count, sizeof(*ranked),
		crystal_sddc_ranked_candidate_cmp, NULL);
	return ranked_count;
}

static bool crystal_sddc_delta_target_eligible(u32 size)
{
	return size > CRYSTAL_SDDC_DELTA_MIN_SIZE &&
		size > sizeof(struct crystal_sddc_delta_header) +
		CRYSTAL_SDDC_DELTA_GAIN_MIN;
}

static bool crystal_sddc_source_snapshot(struct crystal_sddc *sddc,
		const struct crystal_sddc_candidate *candidate,
		const struct crystal_sddc_job_key *target, void *payload,
		struct crystal_sddc_source *source)
{
	struct crystal_sddc_slot_state *state;
	struct zram *zram = sddc->zram;
	void *state_entry;
	void *src;

	memset(source, 0, sizeof(*source));
	source->key.index = CRYSTAL_SDDC_NO_SLOT;
	if (crystal_sddc_index_cell_ref(candidate->cell)) {
		source->ref = crystal_sddc_ref_pin_id(sddc,
				crystal_sddc_index_cell_id(candidate->cell));
		if (!source->ref)
			return false;
		source->cookie = source->ref->cookie;
		source->key.size = source->ref->size;
		source->key.prio = source->ref->prio;
		if (!crystal_sddc_ref_valid_common(sddc, source->ref) ||
		    source->key.prio != target->prio) {
			crystal_sddc_ref_put(source->ref);
			source->ref = NULL;
			return false;
		}
		crystal_sddc_copy_ref(source->ref, payload);
		return true;
	}

	if (candidate->index == target->index ||
	    candidate->index >= sddc->nr_slots)
		return false;

	crystal_sddc_slot_lock(zram, candidate->index);
	state_entry = crystal_sddc_state_load_raw(sddc, candidate->index);
	if (state_entry == XA_ZERO_ENTRY || xa_is_err(state_entry))
		goto unlock;
	state = state_entry;
	if (crystal_sddc_test_flag(zram, candidate->index, ZRAM_SAME) ||
	    crystal_sddc_test_flag(zram, candidate->index, ZRAM_WB) ||
	    crystal_sddc_test_flag(zram, candidate->index, ZRAM_UNDER_WB))
		goto unlock;

	source->key.index = candidate->index;
	source->key.mutation_seq = sddc->mutation_seq[candidate->index];
	if (state && state->kind == CRYSTAL_SDDC_REF) {
		if (!crystal_sddc_slot_state_storage_valid_locked(sddc,
				candidate->index, state))
			goto unlock;
		source->cookie = state->ref;
		source->ref = crystal_sddc_slot_state_pin_ref_locked(sddc,
				state, CRYSTAL_SDDC_REF);
		if (!source->ref)
			goto unlock;
		source->key.size = source->ref->size;
		source->key.prio = source->ref->prio;
		crystal_sddc_slot_unlock(zram, candidate->index);
		if (!crystal_sddc_ref_valid_common(sddc, source->ref) ||
		    source->key.prio != target->prio) {
			crystal_sddc_ref_put(source->ref);
			source->ref = NULL;
			return false;
		}
		crystal_sddc_copy_ref(source->ref, payload);
		return true;
	}

	if (state)
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
		struct crystal_sddc_source *source,
		struct crystal_sddc_slot_state **prepared_state,
		u32 page_hash)
{
	struct crystal_sddc_ref *ref;
	struct crystal_sddc_slot_state *state;
	struct zram *zram = sddc->zram;

	if (source->ref)
		return source->ref;
	if (!source->ordinary || !prepared_state || !*prepared_state)
		return NULL;
	if (!source->key.handle ||
	    source->key.size < CRYSTAL_SDDC_SAMPLE_SIZE ||
	    source->key.size > PAGE_SIZE ||
	    source->key.prio >= ZRAM_MAX_COMPS)
		return NULL;
	state = *prepared_state;

	ref = crystal_sddc_ref_alloc(sddc);
	if (IS_ERR(ref))
		return NULL;

	crystal_sddc_slot_lock(zram, source->key.index);
	if (sddc->mutation_seq[source->key.index] !=
			source->key.mutation_seq ||
		crystal_sddc_state_load_raw(sddc, source->key.index) !=
				XA_ZERO_ENTRY ||
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
	state->kind = CRYSTAL_SDDC_REF;
	state->ref = ref->cookie;
	state->ref_obj = ref;
	state->ref_size = crystal_sddc_state_size(ref->size);
	state->target_size = crystal_sddc_state_size(ref->size);
	state->page_hash = page_hash;
	crystal_sddc_zram_account_sub_locked(zram, source->key.index);
	if (!crystal_sddc_state_install_locked(sddc, source->key.index,
			state))
		goto restore_account;
	if (!crystal_sddc_ref_publish(sddc, ref))
		goto fail_state;

	crystal_sddc_clear_storage_flags(zram, source->key.index);
	zram->table[source->key.index].handle = 0;
	crystal_sddc_set_obj_size(zram, source->key.index, 0);
	crystal_sddc_zram_account_add_locked(zram, source->key.index);
	crystal_sddc_zram_ref_account(zram, ref->memcg_id, ref->size, true);
	atomic64_inc(&sddc->stats.refs);
	atomic64_add(ref->size, &sddc->stats.ref_bytes);
	crystal_sddc_slot_unlock(zram, source->key.index);
	*prepared_state = NULL;
	source->ref = ref;
	source->cookie = ref->cookie;
	return ref;

fail_state:
	state->ref_obj = NULL;
	if (WARN_ON_ONCE(!crystal_sddc_xa_erase_if(&sddc->slot_states,
			source->key.index, state)))
		goto restore_account;
	*prepared_state = NULL;
	kfree(state);
restore_account:
	crystal_sddc_zram_account_add_locked(zram, source->key.index);
fail_unlock:
	crystal_sddc_slot_unlock(zram, source->key.index);
	crystal_sddc_ref_abort(ref);
	return NULL;
}

static void crystal_sddc_index_promote_candidate(struct crystal_sddc *sddc,
		const struct crystal_sddc_candidate *candidate,
		const struct crystal_sddc_source *source,
		const struct crystal_sddc_ref *ref)
{
	u32 cell;

	if (!candidate || !source || !ref || !candidate->bucket ||
	    candidate->way >= CRYSTAL_SDDC_BUCKET_WAYS ||
	    crystal_sddc_index_cell_ref(candidate->cell) ||
	    source->key.index == CRYSTAL_SDDC_NO_SLOT ||
	    crystal_sddc_index_cell_id(candidate->cell) != source->key.index + 1)
		return;

	cell = crystal_sddc_index_ref_cell(candidate->cell, ref->cookie.id);
	if (!cell)
		return;

	spin_lock(&sddc->index_lock);
	if (candidate->bucket->cells[candidate->way] == candidate->cell)
		candidate->bucket->cells[candidate->way] = cell;
	spin_unlock(&sddc->index_lock);
}

static bool crystal_sddc_commit_target(struct crystal_sddc *sddc,
		const struct crystal_sddc_job_key *target,
		struct crystal_sddc_ref *ref, enum crystal_sddc_kind kind,
		unsigned long new_handle, u32 new_size,
		struct crystal_sddc_slot_state **prepared_state,
		u32 page_hash)
{
	struct crystal_sddc_slot_state *state;
	struct zram *zram = sddc->zram;
	unsigned long old_handle;
	u32 old_size;
	u32 saved_size;
	u32 target_size;
	bool committed = false;

	if (!prepared_state || !*prepared_state)
		return false;
	state = *prepared_state;
	if (!crystal_sddc_ref_valid_common(sddc, ref) ||
	    !crystal_sddc_target_storage_valid(kind, new_handle, new_size))
		return false;

	crystal_sddc_slot_lock(zram, target->index);
	if (!crystal_sddc_job_matches_locked(sddc, target, true))
		goto unlock;

	old_handle = zram->table[target->index].handle;
	old_size = crystal_sddc_obj_size(zram, target->index);
	saved_size = old_size > new_size ? old_size - new_size : 0;
	target_size = kind == CRYSTAL_SDDC_ALIAS ? ref->size : target->size;
	if (!crystal_sddc_state_sizes_fit(new_size, saved_size, ref->size,
			target_size))
		goto unlock;
	state->kind = kind;
	state->ref = ref->cookie;
	state->ref_obj = ref;
	state->accounted_size = crystal_sddc_state_size(new_size);
	state->saved_size = crystal_sddc_state_size(saved_size);
	state->ref_size = crystal_sddc_state_size(ref->size);
	state->target_size = crystal_sddc_state_size(target_size);
	state->page_hash = page_hash;
	if (!crystal_sddc_ref_valid_for_kind(sddc, ref, kind) ||
	    !crystal_sddc_target_storage_valid(kind, new_handle, new_size))
		goto unlock;
	crystal_sddc_zram_account_sub_locked(zram, target->index);
	if (!crystal_sddc_state_install_locked(sddc, target->index, state))
		goto restore_account;
	crystal_sddc_clear_storage_flags(zram, target->index);
	zs_free(zram->mem_pool, old_handle);
	atomic64_sub(old_size, &zram->stats.compr_data_size);
	zram->table[target->index].handle = new_handle;
	crystal_sddc_set_obj_size(zram, target->index, new_size);
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
	*prepared_state = NULL;
	committed = true;
	goto unlock;

restore_account:
	crystal_sddc_zram_account_add_locked(zram, target->index);

unlock:
	crystal_sddc_slot_unlock(zram, target->index);
	return committed;
}

static bool crystal_sddc_equal_ordered(const void *target, u32 target_size,
		const void *source, u32 source_size)
{
	const u8 *target_data = target;
	const u8 *source_data = source;
	u32 head;
	u32 tail;
	u32 middle;

	if (source_size != target_size)
		return false;

	head = min_t(u32, target_size, CRYSTAL_SDDC_SAMPLE_SIZE);
	if (head && memcmp(target_data, source_data, head))
		return false;
	if (target_size == head)
		return true;

	tail = min_t(u32, target_size - head, CRYSTAL_SDDC_SAMPLE_SIZE);
	if (tail && memcmp(target_data + target_size - tail,
			   source_data + source_size - tail, tail))
		return false;

	middle = target_size - head - tail;
	return !middle || !memcmp(target_data + head, source_data + head,
				  middle);
}

static bool crystal_sddc_try_alias_from_source(struct crystal_sddc *sddc,
		const struct crystal_sddc_job_key *target,
		const struct crystal_sddc_candidate *candidate,
		struct crystal_sddc_source *source, u32 page_hash)
{
	struct crystal_sddc_slot_state *source_state = NULL;
	struct crystal_sddc_slot_state *target_state = NULL;
	struct crystal_sddc_ref *ref;
	bool committed = false;

	target_state = crystal_sddc_state_prepare(sddc, target->index);
	if (!target_state)
		goto allocation_failed;
	if (source->ordinary) {
		source_state = crystal_sddc_state_prepare(sddc,
				source->key.index);
		if (!source_state)
			goto allocation_failed;
	}

	ref = crystal_sddc_promote_source(sddc, source, &source_state,
			page_hash);
	if (!ref) {
		atomic64_inc(&sddc->stats.conversion_failures);
		goto out;
	}
	if (crystal_sddc_commit_target(sddc, target, ref,
			CRYSTAL_SDDC_ALIAS, 0, 0, &target_state,
			page_hash)) {
		crystal_sddc_index_promote_candidate(sddc, candidate, source,
				ref);
		source->ref = NULL;
		committed = true;
		goto out;
	}

	atomic64_inc(&sddc->stats.conversion_failures);
	goto out;

allocation_failed:
	atomic64_inc(&sddc->stats.conversion_failures);
out:
	crystal_sddc_state_abort(sddc, source->key.index,
			source->key.mutation_seq, source_state);
	crystal_sddc_state_abort(sddc, target->index, target->mutation_seq,
			target_state);
	return committed;
}

static bool crystal_sddc_try_alias(struct crystal_sddc *sddc,
		const struct crystal_sddc_job_key *target, const void *target_data,
		const struct crystal_sddc_candidate *candidate,
		struct crystal_sddc_workspace *workspace)
{
	struct crystal_sddc_source source;
	u32 page_hash = 0;
	bool committed = false;
	int ret;

	if (!crystal_sddc_source_snapshot(sddc, candidate, target,
			workspace->ref_data, &source))
		return false;
	if (!crystal_sddc_equal_ordered(target_data, target->size,
			workspace->ref_data, source.key.size))
		goto out;

	ret = crystal_sddc_hash_stream_page(sddc, target_data, target->size,
			target->prio, workspace->wire, &page_hash);
	if (ret) {
		atomic64_inc(&sddc->stats.integrity_hash_failures);
		page_hash = 0;
	}
	committed = crystal_sddc_try_alias_from_source(sddc, target,
			candidate, &source, page_hash);
out:
	crystal_sddc_ref_put(source.ref);
	return committed;
}

static bool crystal_sddc_try_delta_from_source(struct crystal_sddc *sddc,
		const struct crystal_sddc_job_key *target, const void *target_data,
		const struct crystal_sddc_candidate *candidate,
		struct crystal_sddc_source *source,
		struct crystal_sddc_workspace *workspace)
{
	struct crystal_sddc_delta_header header;
	struct crystal_sddc_slot_state *source_state = NULL;
	struct crystal_sddc_slot_state *target_state = NULL;
	struct crystal_sddc_ref *ref;
	struct zcomp_strm *zstrm;
	unsigned long handle;
	unsigned int delta_len;
	unsigned int out_limit;
	u32 wire_size;
	u32 source_page_hash = 0;
	u32 target_page_hash = 0;
	void *delta = (u8 *)workspace->wire + sizeof(header);
	void *dst;
	bool committed = false;
	int ret;

	if (!crystal_sddc_delta_target_eligible(target->size))
		return false;
	if ((source->key.size == PAGE_SIZE) != (target->size == PAGE_SIZE))
		goto out;

	target_state = crystal_sddc_state_prepare(sddc, target->index);
	if (!target_state)
		goto allocation_failed;
	if (source->ordinary) {
		source_state = crystal_sddc_state_prepare(sddc,
				source->key.index);
		if (!source_state)
			goto allocation_failed;
	}

	ret = crystal_sddc_hash_stream_page(sddc, workspace->ref_data,
			source->key.size, source->key.prio, workspace->wire,
			&source_page_hash);
	if (ret) {
		atomic64_inc(&sddc->stats.integrity_hash_failures);
		source_page_hash = 0;
	}
	ret = crystal_sddc_hash_stream_page(sddc, target_data, target->size,
			target->prio, workspace->wire, &target_page_hash);
	if (ret) {
		atomic64_inc(&sddc->stats.integrity_hash_failures);
		target_page_hash = 0;
	}

	out_limit = target->size - sizeof(header);
	zstrm = zcomp_stream_get(sddc->zram->comps[target->prio]);
	if (!zstrm)
		goto out;
	delta_len = 2 * PAGE_SIZE;
	ret = zcomp_compress_delta(zstrm, workspace->ref_data, source->key.size,
			target_data, target->size, &delta_len, out_limit);
	if (!ret && delta_len && delta_len <= out_limit)
		memcpy(delta, zstrm->buffer, delta_len);
	else
		delta = NULL;
	zcomp_stream_put(sddc->zram->comps[target->prio]);
	if (!delta) {
		atomic64_inc(&sddc->stats.delta_no_gain);
		goto out;
	}

	wire_size = sizeof(header) + delta_len;
	if (zs_lookup_class_index(sddc->zram->mem_pool, wire_size) >=
	    zs_lookup_class_index(sddc->zram->mem_pool, target->size)) {
		atomic64_inc(&sddc->stats.delta_no_gain);
		goto out;
	}
	handle = zs_malloc(sddc->zram->mem_pool, wire_size,
			GFP_NOIO | __GFP_HIGHMEM | __GFP_MOVABLE | __GFP_CMA);
	if (IS_ERR_VALUE(handle)) {
		atomic64_inc(&sddc->stats.conversion_failures);
		goto out;
	}
	if (!crystal_sddc_zram_memory_limit_ok(sddc->zram)) {
		atomic64_inc(&sddc->stats.limit_rejects);
		atomic64_inc(&sddc->stats.conversion_failures);
		goto free_handle;
	}

	ref = crystal_sddc_promote_source(sddc, source, &source_state,
			source_page_hash);
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
	memcpy(workspace->wire, &header, sizeof(header));
	dst = zs_map_object(sddc->zram->mem_pool, handle, ZS_MM_WO);
	memcpy(dst, workspace->wire, wire_size);
	zs_unmap_object(sddc->zram->mem_pool, handle);

	if (crystal_sddc_commit_target(sddc, target, ref,
			CRYSTAL_SDDC_DELTA, handle, wire_size, &target_state,
			target_page_hash)) {
		crystal_sddc_index_promote_candidate(sddc, candidate, source,
				ref);
		source->ref = NULL;
		committed = true;
		goto out;
	}

	atomic64_inc(&sddc->stats.conversion_failures);
free_handle:
	zs_free(sddc->zram->mem_pool, handle);
out:
	crystal_sddc_state_abort(sddc, source->key.index,
			source->key.mutation_seq, source_state);
	crystal_sddc_state_abort(sddc, target->index, target->mutation_seq,
			target_state);
	return committed;

allocation_failed:
	atomic64_inc(&sddc->stats.conversion_failures);
	goto out;
}

static bool crystal_sddc_try_sample(struct crystal_sddc *sddc,
		const struct crystal_sddc_job_key *target, const void *target_data,
		const struct crystal_sddc_ranked_candidate *ranked,
		struct crystal_sddc_workspace *workspace,
		bool *delta_tried, bool *delta_hit)
{
	struct crystal_sddc_source source;
	u32 page_hash = 0;
	bool converted = false;
	int ret;

	*delta_tried = false;
	*delta_hit = false;

	if (!crystal_sddc_source_snapshot(sddc, &ranked->candidate, target,
			workspace->ref_data, &source))
		return false;

	if (crystal_sddc_equal_ordered(target_data, target->size,
			workspace->ref_data, source.key.size)) {
		atomic64_inc(&sddc->stats.alias_attempts);
		ret = crystal_sddc_hash_stream_page(sddc, target_data,
				target->size, target->prio, workspace->wire,
				&page_hash);
		if (ret) {
			atomic64_inc(&sddc->stats.integrity_hash_failures);
			page_hash = 0;
		}
		if (crystal_sddc_try_alias_from_source(sddc, target,
				&ranked->candidate, &source, page_hash)) {
			atomic64_inc(&sddc->stats.alias_hits);
			converted = true;
		}
		goto out;
	}

	*delta_tried = true;
	converted = crystal_sddc_try_delta_from_source(sddc, target,
			target_data, &ranked->candidate, &source, workspace);
	*delta_hit = converted;

out:
	crystal_sddc_ref_put(source.ref);
	return converted;
}

static bool crystal_sddc_try_convert(struct crystal_sddc *sddc,
		const struct crystal_sddc_job_key *target, const void *target_data,
		u32 exact_hash, u32 head_hash, u32 tail_hash,
		struct crystal_sddc_workspace *workspace)
{
	struct crystal_sddc_candidate candidates[CRYSTAL_SDDC_BUCKET_WAYS * 2];
	struct crystal_sddc_ranked_candidate ranked[ARRAY_SIZE(candidates)];
	unsigned int count = 0;
	unsigned int i;
	bool converted = false;

	spin_lock(&sddc->index_lock);
	count = crystal_sddc_index_candidates(sddc->exact_index, exact_hash,
			0, candidates, count,
				ARRAY_SIZE(candidates));
	spin_unlock(&sddc->index_lock);
	for (i = 0; i < count; i++) {
		atomic64_inc(&sddc->stats.alias_attempts);
		if (crystal_sddc_try_alias(sddc, target, target_data,
				&candidates[i], workspace)) {
			atomic64_inc(&sddc->stats.alias_hits);
			converted = true;
			goto out;
		}
	}
	if (!crystal_sddc_delta_target_eligible(target->size)) {
		atomic64_inc(&sddc->stats.delta_small_rejects);
		goto out;
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
	count = crystal_sddc_rank_candidates(sddc, target, target_data,
			candidates, count, ranked);
	for (i = 0; i < count; i++) {
		bool delta_tried;
		bool delta_hit;

		if (ranked[i].match_bytes < CRYSTAL_SDDC_SIMILARITY_MIN)
			break;
		if (crystal_sddc_try_sample(sddc, target, target_data,
				&ranked[i], workspace, &delta_tried,
				&delta_hit)) {
			if (delta_tried) {
				atomic64_inc(&sddc->stats.delta_matches);
				crystal_sddc_atomic64_update_max(
					&sddc->stats.delta_match_bytes_max,
					ranked[i].match_bytes);
				atomic64_inc(&sddc->stats.delta_attempts);
			}
			if (delta_hit)
				atomic64_inc(&sddc->stats.delta_hits);
			converted = true;
			goto out;
		}
		if (delta_tried) {
			atomic64_inc(&sddc->stats.delta_matches);
			crystal_sddc_atomic64_update_max(
				&sddc->stats.delta_match_bytes_max,
				ranked[i].match_bytes);
			atomic64_inc(&sddc->stats.delta_attempts);
			if (delta_hit)
				atomic64_inc(&sddc->stats.delta_hits);
		}
	}

out:
	return converted;
}

/* state_lock serializes the bitmap and the idle-to-running transition. */
static bool crystal_sddc_observe_mark_locked(struct crystal_sddc *sddc,
		u32 index)
{
	if (WARN_ON_ONCE(index >= sddc->nr_slots))
		return false;
	if (test_and_set_bit(index, sddc->observe_pending)) {
		atomic64_inc(&sddc->stats.coalesced);
		return false;
	}

	sddc->pending++;
	crystal_sddc_atomic64_update_max(&sddc->stats.pending_max,
			sddc->pending);
	return true;
}

static bool crystal_sddc_observe_activate_locked(struct crystal_sddc *sddc)
{
	if (sddc->observe_worker_active)
		return false;

	sddc->observe_worker_active = true;
	return true;
}

static void crystal_sddc_observe_deactivate_locked(struct crystal_sddc *sddc, unsigned int work_id)
{
	sddc->observe_work_cursor = work_id ^ 1;
	sddc->observe_worker_active = false;
}

static unsigned int crystal_sddc_observe_discard_locked(
		struct crystal_sddc *sddc)
{
	unsigned int discarded = sddc->pending;

	if (discarded)
		atomic64_add(discarded, &sddc->stats.shutdown_discarded);
	bitmap_zero(sddc->observe_pending, (unsigned int)sddc->nr_slots);
	sddc->pending = 0;
	return discarded;
}

static bool crystal_sddc_observe_take_locked(struct crystal_sddc *sddc,
		u32 *index)
{
	unsigned long next;

	if (!sddc->pending)
		return false;

	next = find_next_bit(sddc->observe_pending, sddc->nr_slots,
			sddc->observe_cursor);
	if (next >= sddc->nr_slots && sddc->observe_cursor)
		next = find_first_bit(sddc->observe_pending, sddc->nr_slots);
	if (WARN_ON_ONCE(next >= sddc->nr_slots)) {
		/* Keep the bitmap/count invariant recoverable even if corruption or
		 * an unexpected find-bit result leaves no consumable slot. */
		bitmap_zero(sddc->observe_pending, (unsigned int)sddc->nr_slots);
		sddc->pending = 0;
		return false;
	}

	clear_bit(next, sddc->observe_pending);
	sddc->pending--;
	sddc->observe_cursor = next + 1;
	if (sddc->observe_cursor >= sddc->nr_slots)
		sddc->observe_cursor = 0;
	*index = next;
	return true;
}

static void crystal_sddc_observe_index(struct crystal_sddc *sddc,
		u32 index, struct crystal_sddc_workspace *workspace)
{
	struct crystal_sddc_job_key key;
	struct zram *zram = sddc->zram;
	u32 exact_hash;
	u32 head_hash;
	u32 tail_hash;
	void *payload = workspace->ordinary;
	void *src;
	bool converted;
	bool still_matches;

	down_read(&zram->init_lock);
	if (zram->sddc != sddc || !zram->table ||
	    index >= (zram->disksize >> PAGE_SHIFT))
		goto out_unlock;

	crystal_sddc_slot_lock(zram, index);
	crystal_sddc_job_key_locked(zram, index, &key);
	if (!key.handle) {
		crystal_sddc_slot_unlock(zram, index);
		atomic64_inc(&sddc->stats.ineligible);
		goto out_unlock;
	}

	src = zs_map_object(zram->mem_pool, key.handle, ZS_MM_RO);
	memcpy(payload, src, key.size);
	zs_unmap_object(zram->mem_pool, key.handle);
	crystal_sddc_slot_unlock(zram, index);

	exact_hash = jhash(payload, key.size,
			CRYSTAL_SDDC_HASH_SEED ^ key.size);
	head_hash = jhash(payload, CRYSTAL_SDDC_SAMPLE_SIZE,
			CRYSTAL_SDDC_HASH_SEED);
	tail_hash = jhash(payload + key.size - CRYSTAL_SDDC_SAMPLE_SIZE,
			CRYSTAL_SDDC_SAMPLE_SIZE, CRYSTAL_SDDC_HASH_SEED);
	converted = crystal_sddc_try_convert(sddc, &key, payload, exact_hash,
			head_hash, tail_hash, workspace);

	if (!converted) {
		/* A conversion can race a rewrite after the initial snapshot.  Do not
		 * publish an empty or managed slot.  Later source validation resolves
		 * the latest representation and makes stale cells harmless. */
		crystal_sddc_slot_lock(zram, index);
		still_matches = crystal_sddc_job_matches_locked(sddc, &key, false);
		if (!still_matches) {
			crystal_sddc_slot_unlock(zram, index);
			atomic64_inc(&sddc->stats.stale);
			goto observed;
		}
		spin_lock(&sddc->index_lock);
		crystal_sddc_index_insert(sddc, sddc->exact_index, exact_hash,
				CRYSTAL_SDDC_SAMPLE_HEAD, key.index);
		if (crystal_sddc_sample_eligible(key.size)) {
			crystal_sddc_index_insert(sddc, sddc->sample_index, head_hash,
					CRYSTAL_SDDC_SAMPLE_HEAD, key.index);
			crystal_sddc_index_insert(sddc, sddc->sample_index, tail_hash,
					CRYSTAL_SDDC_SAMPLE_TAIL, key.index);
		}
		spin_unlock(&sddc->index_lock);
		/* Keep validation and publication in one slot critical section.  A
		 * rewrite cannot invalidate the identity between the final check and
		 * insertion of its candidate cell. */
		crystal_sddc_slot_unlock(zram, index);
		atomic64_inc(&sddc->stats.indexed);
	}

observed:
	atomic64_inc(&sddc->stats.observed);

out_unlock:
	up_read(&zram->init_lock);
}

static bool crystal_sddc_observe_handoff(
		struct crystal_sddc_observe_work *observe)
{
	struct crystal_sddc *sddc = observe->sddc;
	bool pending;

	spin_lock(&sddc->state_lock);
	pending = !READ_ONCE(sddc->stopping) && sddc->pending;
	spin_unlock(&sddc->state_lock);
	if (!pending)
		return false;

	/* Transfer this callback's lifetime token to the other embedded item;
	 * the current callback remains running until its caller returns. */
	return queue_work(sddc->workqueue,
			&sddc->observe_work[observe->id ^ 1].work);
}

static void crystal_sddc_observe_workfn(struct work_struct *work)
{
	struct crystal_sddc_observe_work *observe = container_of(work,
			struct crystal_sddc_observe_work, work);
	struct crystal_sddc *sddc = observe->sddc;
	struct zram *zram = sddc->zram;
	struct crystal_sddc_workspace *workspace;
	u32 index;
	unsigned int budget = CRYSTAL_SDDC_OBSERVE_BUDGET;

	atomic64_inc(&sddc->stats.worker_runs);
	workspace = sddc->observe_workspace;
	for (;;) {
		spin_lock(&sddc->state_lock);
		if (!crystal_sddc_observe_take_locked(sddc, &index)) {
			/* Commit to exiting while admission is serialized.  A later
			 * producer will queue the alternate item instead of relying on
			 * this callback while it is returning.
			 */
			crystal_sddc_observe_deactivate_locked(sddc, observe->id);
			spin_unlock(&sddc->state_lock);
			break;
		}
		spin_unlock(&sddc->state_lock);

		crystal_sddc_observe_index(sddc, index, workspace);
		cond_resched();
		if (!--budget) {
			bool stopping;

			/* Bound each callback so a continuously rewritten device cannot
			 * starve freezer/reset progress.  A successful handoff transfers
			 * this callback's lifetime token and zram reference. */
			if (crystal_sddc_observe_handoff(observe)) {
				return;
			}
			spin_lock(&sddc->state_lock);
			stopping = READ_ONCE(sddc->stopping);
			if (stopping) {
				crystal_sddc_observe_discard_locked(sddc);
				crystal_sddc_observe_deactivate_locked(sddc, observe->id);
			}
			spin_unlock(&sddc->state_lock);
			if (stopping)
				break;
			/* A failed handoff means the alternate item is already busy.
			 * Retain this drain's lifetime ownership and continue.
			 */
			budget = CRYSTAL_SDDC_OBSERVE_BUDGET;
		}
	}

	/* Drop the manager token before the device reference.  zram removal
	 * may wake as soon as the latter reaches zero and destroy @sddc; use
	 * only the saved device pointer after releasing the manager token. */
	crystal_sddc_manager_put(sddc);
	zram_put(zram);
}

int crystal_sddc_create(struct zram *zram, unsigned long nr_pages)
{
	struct crystal_sddc *sddc;
	unsigned int i;

	if (!zram)
		return -EINVAL;
	lockdep_assert_held(&zram->sddc_lifecycle_lock);
	lockdep_assert_held_write(&zram->init_lock);
	if (zram->sddc)
		return -EINVAL;
	if (PAGE_SHIFT != 12 || !nr_pages ||
	    nr_pages > CRYSTAL_SDDC_MAX_SLOTS ||
	    !zram->comps[ZRAM_PRIMARY_COMP] ||
	    !zcomp_supports_delta(zram->comps[ZRAM_PRIMARY_COMP]))
		return 0;

	sddc = kzalloc(sizeof(*sddc), GFP_KERNEL);
	if (!sddc) {
		pr_warn("%s: SDDC manager allocation failed; using ordinary compression\n",
			zram->disk->disk_name);
		return 0;
	}
	ida_init(&sddc->ref_ids);
	xa_init(&sddc->refs);
	xa_init(&sddc->slot_states);
	xa_init(&sddc->wb_states);
	sddc->zram = zram;
	sddc->nr_slots = nr_pages;
	spin_lock_init(&sddc->state_lock);
	spin_lock_init(&sddc->index_lock);
	spin_lock_init(&sddc->active_lock);
	atomic_set(&sddc->active_ops, 0);
	init_waitqueue_head(&sddc->active_wait);
	for (i = 0; i < CRYSTAL_SDDC_OBSERVE_WORKS; i++) {
		sddc->observe_work[i].sddc = sddc;
		sddc->observe_work[i].id = i;
		INIT_WORK(&sddc->observe_work[i].work,
			crystal_sddc_observe_workfn);
	}

	sddc->mutation_seq = vzalloc(array_size(nr_pages,
						sizeof(*sddc->mutation_seq)));
	sddc->observe_pending = bitmap_zalloc((unsigned int)nr_pages,
						 GFP_KERNEL | __GFP_NOWARN);
	sddc->exact_index = vzalloc(array_size(CRYSTAL_SDDC_HASH_BUCKETS,
					       sizeof(*sddc->exact_index)));
	sddc->sample_index = vzalloc(array_size(CRYSTAL_SDDC_HASH_BUCKETS,
							sizeof(*sddc->sample_index)));
	if (!sddc->mutation_seq || !sddc->observe_pending ||
	    !sddc->exact_index || !sddc->sample_index)
		goto fail;

	sddc->observe_workspace = crystal_sddc_workspace_alloc(GFP_KERNEL);
	if (!sddc->observe_workspace)
		goto fail;

	/* Huawei runs SDDC recompression from a reclaim-capable worker.  Keep the
	 * ordered/budgeted hand-off, but do not let freezer state postpone the
	 * index while memory pressure is actively building.  reset still closes
	 * admission and flushes this queue before taking init_lock for write. */
	sddc->workqueue = alloc_ordered_workqueue("%s-sddc",
				WQ_MEM_RECLAIM, zram->disk->disk_name);
	if (!sddc->workqueue)
		goto fail;
	sddc->free_workqueue = alloc_workqueue("%s-sddc-free",
			WQ_MEM_RECLAIM | WQ_UNBOUND | WQ_HIGHPRI, 0,
			zram->disk->disk_name);
	if (!sddc->free_workqueue)
		goto fail;

	spin_lock(&zram->sddc_lock);
	WARN_ON_ONCE(zram->sddc);
	WRITE_ONCE(zram->sddc, sddc);
	spin_unlock(&zram->sddc_lock);
	return 0;

fail:
	pr_warn("%s: SDDC metadata allocation failed; using ordinary compression\n",
		zram->disk->disk_name);
	if (sddc->free_workqueue)
		destroy_workqueue(sddc->free_workqueue);
	if (sddc->workqueue)
		destroy_workqueue(sddc->workqueue);
	crystal_sddc_workspace_free(sddc->observe_workspace);
	bitmap_free(sddc->observe_pending);
	vfree(sddc->sample_index);
	vfree(sddc->exact_index);
	vfree(sddc->mutation_seq);
	xa_destroy(&sddc->slot_states);
	xa_destroy(&sddc->wb_states);
	xa_destroy(&sddc->refs);
	ida_destroy(&sddc->ref_ids);
	kfree(sddc);
	/* SDDC is opportunistic; retain ordinary LZ4KD zram on allocation loss. */
	return 0;
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

	spin_lock(&sddc->state_lock);
	crystal_sddc_observe_discard_locked(sddc);
	spin_unlock(&sddc->state_lock);
	flush_workqueue(sddc->workqueue);
	crystal_sddc_manager_wait_idle(sddc);
	WARN_ON_ONCE(READ_ONCE(sddc->pending));
	flush_workqueue(sddc->free_workqueue);
}

/* The normal zram teardown path frees every slot before reaching destroy().
 * Keep the invariant failure recoverable nevertheless: xarray nodes do not
 * own the heap values stored in them, and ref values may still own zsmalloc
 * handles.  At this point active operations and free work are quiescent. */
static void crystal_sddc_destroy_metadata(struct crystal_sddc *sddc)
{
	struct crystal_sddc_slot_state *state;
	struct crystal_sddc_wb_state *wb_state;
	struct crystal_sddc_ref *ref;
	struct crystal_sddc_ref *ref_obj;
	unsigned long index;

	xa_for_each(&sddc->slot_states, index, state) {
		if (state == XA_ZERO_ENTRY) {
			xa_erase(&sddc->slot_states, index);
			continue;
		}
		if (!state || xa_is_err(state))
			continue;
		if (xa_erase(&sddc->slot_states, index) == state) {
			crystal_sddc_slot_state_drop_ref(sddc, state);
			kfree(state);
		}
	}
	xa_for_each(&sddc->wb_states, index, wb_state) {
		if (wb_state == XA_ZERO_ENTRY) {
			xa_erase(&sddc->wb_states, index);
			continue;
		}
		if (!wb_state || xa_is_err(wb_state))
			continue;
		if (xa_erase(&sddc->wb_states, index) == wb_state) {
			ref_obj = wb_state->ref_obj;
			wb_state->ref_obj = NULL;
			crystal_sddc_ref_put(ref_obj);
			kfree(wb_state);
		}
	}

	/*
	 * Any remaining ref is an orphan left by a failed teardown invariant. No
	 * active SDDC operation can still use it here, so collapse its owner count
	 * to one and use the normal deferred release path.
	 */
	xa_for_each(&sddc->refs, index, ref) {
		if (!ref || ref == XA_ZERO_ENTRY || xa_is_err(ref))
			continue;
		WARN_ON_ONCE(refcount_read(&ref->refs) != 1);
		refcount_set(&ref->refs, 1);
		crystal_sddc_ref_put(ref);
	}
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
	WARN_ON_ONCE(!xa_empty(&sddc->slot_states));
	WARN_ON_ONCE(!xa_empty(&sddc->wb_states));
	WARN_ON_ONCE(!xa_empty(&sddc->refs));
	crystal_sddc_destroy_metadata(sddc);
	flush_workqueue(sddc->free_workqueue);
	destroy_workqueue(sddc->workqueue);
	destroy_workqueue(sddc->free_workqueue);
	bitmap_free(sddc->observe_pending);
	crystal_sddc_workspace_free(sddc->observe_workspace);
	vfree(sddc->sample_index);
	vfree(sddc->exact_index);
	vfree(sddc->mutation_seq);
	xa_destroy(&sddc->slot_states);
	xa_destroy(&sddc->wb_states);
	xa_destroy(&sddc->refs);
	ida_destroy(&sddc->ref_ids);
	kfree(sddc);
}

static void crystal_sddc_account_release(struct crystal_sddc *sddc,
		enum crystal_sddc_kind kind, u32 saved_size,
		enum crystal_sddc_release_reason reason)
{
	if (kind == CRYSTAL_SDDC_ALIAS)
		atomic64_inc(&sddc->stats.released_aliases);
	else if (kind == CRYSTAL_SDDC_DELTA)
		atomic64_inc(&sddc->stats.released_deltas);
	else
		return;

	if (!saved_size)
		return;
	atomic64_add(saved_size, &sddc->stats.released_saved_bytes);
	switch (reason) {
	case CRYSTAL_SDDC_RELEASE_WRITEBACK:
		atomic64_add(saved_size,
			     &sddc->stats.released_writeback_saved_bytes);
		break;
	case CRYSTAL_SDDC_RELEASE_REWRITE:
		atomic64_add(saved_size,
			     &sddc->stats.released_rewrite_saved_bytes);
		break;
	case CRYSTAL_SDDC_RELEASE_RECOMPRESS:
		atomic64_add(saved_size,
			     &sddc->stats.released_recompress_saved_bytes);
		break;
	case CRYSTAL_SDDC_RELEASE_DISCARD:
		atomic64_add(saved_size,
			     &sddc->stats.released_discard_saved_bytes);
		break;
	case CRYSTAL_SDDC_RELEASE_NOTIFY_FREE:
		atomic64_add(saved_size,
			     &sddc->stats.released_notify_free_saved_bytes);
		break;
	case CRYSTAL_SDDC_RELEASE_RESET:
		atomic64_add(saved_size,
			     &sddc->stats.released_reset_saved_bytes);
		break;
	case CRYSTAL_SDDC_RELEASE_OTHER:
	default:
		atomic64_add(saved_size,
			     &sddc->stats.released_other_saved_bytes);
		break;
	}
}

enum crystal_sddc_kind crystal_sddc_slot_free_locked(struct zram *zram,
		u32 index, enum crystal_sddc_release_reason reason)
{
	struct crystal_sddc *sddc = zram->sddc;
	struct crystal_sddc_slot_state *state;
	enum crystal_sddc_kind kind;
	u32 accounted_size;
	u32 saved_size;

	if (!sddc || index >= sddc->nr_slots)
		return CRYSTAL_SDDC_NONE;

	state = crystal_sddc_state_load_raw(sddc, index);
	if (xa_is_err(state)) {
		WARN_ON_ONCE(1);
		state = NULL;
	} else if (xa_is_zero(state)) {
		state = NULL;
	} else if (state) {
		state = crystal_sddc_xa_erase_if(&sddc->slot_states, index,
				state);
		if (!state) {
			WARN_ON_ONCE(1);
		}
	}
	if (state) {
		kind = state->kind;
		accounted_size = state->accounted_size;
		saved_size = state->saved_size;
	} else {
		kind = CRYSTAL_SDDC_NONE;
		accounted_size = 0;
		saved_size = 0;
	}
	crystal_sddc_mutation_advance_locked(sddc, index);
	if (kind == CRYSTAL_SDDC_ALIAS)
		atomic64_dec(&sddc->stats.aliases);
	else if (kind == CRYSTAL_SDDC_DELTA) {
		atomic64_dec(&sddc->stats.deltas);
		atomic64_sub(accounted_size, &sddc->stats.delta_bytes);
	}
	if (crystal_sddc_managed_kind(kind)) {
		atomic64_sub(saved_size, &sddc->stats.saved_bytes);
		crystal_sddc_account_release(sddc, kind, saved_size, reason);
		crystal_sddc_slot_state_drop_ref(sddc, state);
	} else if (WARN_ON_ONCE(kind != CRYSTAL_SDDC_NONE)) {
		kind = CRYSTAL_SDDC_NONE;
	}
	kfree(state);

	return kind;
}

void crystal_sddc_slot_stored_locked(struct zram *zram, u32 index)
{
	struct crystal_sddc *sddc = zram->sddc;
	void *entry;

	if (sddc && index < sddc->nr_slots) {
		entry = crystal_sddc_state_load_raw(sddc, index);
		WARN_ON_ONCE(entry && entry != XA_ZERO_ENTRY);
		crystal_sddc_mutation_advance_locked(sddc, index);
	}
}

void crystal_sddc_job_key_locked(struct zram *zram, u32 index,
		struct crystal_sddc_job_key *key)
{
	struct crystal_sddc *sddc = zram->sddc;
	void *state_entry;
	unsigned long handle;
	u32 size;
	u32 prio;

	memset(key, 0, sizeof(*key));
	if (!sddc || index >= sddc->nr_slots)
		return;
	/* Treat an in-flight XA_ZERO_ENTRY reservation as ineligible too.  The
	 * owner will requeue the slot after aborting, so producers must not build
	 * keys that can only fail reservation admission. */
	state_entry = crystal_sddc_state_load_raw(sddc, index);
	if (state_entry ||
	    crystal_sddc_test_flag(zram, index, ZRAM_SAME) ||
	    crystal_sddc_test_flag(zram, index, ZRAM_WB) ||
	    crystal_sddc_test_flag(zram, index, ZRAM_UNDER_WB))
		return;

	handle = zram->table[index].handle;
	size = crystal_sddc_obj_size(zram, index);
	prio = crystal_sddc_priority(zram, index);
	/* Small streams do not enter the asynchronous SDDC index.  This keeps
	 * index pressure and observation CPU focused on objects with useful
	 * sharing/delta potential, matching the original admission heuristic. */
	if (!handle || size <= CRYSTAL_SDDC_INDEX_MIN_SIZE || size > PAGE_SIZE ||
	    prio != ZRAM_PRIMARY_COMP || !zram->comps[prio] ||
	    !zcomp_supports_delta(zram->comps[prio]))
		return;

	key->index = index;
	key->mutation_seq = sddc->mutation_seq[index];
	key->handle = handle;
	key->size = size;
	key->prio = prio;
}

void crystal_sddc_requeue_observation(struct zram *zram, u32 index)
{
	struct crystal_sddc_job_key key;
	struct crystal_sddc *sddc;

	/* The manager token protects SDDC metadata, while this extra reference
	 * keeps the device object alive across the second admission attempt. */
	if (!zram || !zram_try_get(zram))
		return;
	sddc = crystal_sddc_manager_get(zram);
	if (!sddc || index >= sddc->nr_slots) {
		if (sddc)
			crystal_sddc_manager_put(sddc);
		zram_put(zram);
		return;
	}

	crystal_sddc_slot_lock(zram, index);
	crystal_sddc_job_key_locked(zram, index, &key);
	crystal_sddc_slot_unlock(zram, index);
	crystal_sddc_manager_put(sddc);
	crystal_sddc_queue_observation(zram, &key);
	zram_put(zram);
}

bool crystal_sddc_slot_allocated_locked(struct zram *zram, u32 index)
{
	struct crystal_sddc *sddc = zram->sddc;

	return sddc && index < sddc->nr_slots &&
		crystal_sddc_state_load_locked(sddc, index);
}

bool crystal_sddc_accounted_size_locked(struct zram *zram, u32 index,
		size_t *size)
{
	struct crystal_sddc *sddc = zram->sddc;
	struct crystal_sddc_slot_state *state;

	if (!sddc || index >= sddc->nr_slots)
		return false;
	state = crystal_sddc_state_load_locked(sddc, index);
	if (!state)
		return false;
	*size = state->accounted_size;
	return true;
}

void crystal_sddc_snapshot_locked(struct zram *zram, u32 index,
		struct crystal_sddc_snapshot *snapshot)
{
	struct crystal_sddc *sddc = zram->sddc;
	struct crystal_sddc_slot_state *state;
	struct crystal_sddc_wb_state *wb_state;

	memset(snapshot, 0, sizeof(*snapshot));
	if (!sddc || index >= sddc->nr_slots)
		return;
	snapshot->mutation_seq = sddc->mutation_seq[index];
	state = crystal_sddc_state_load_locked(sddc, index);
	if (state) {
		snapshot->ref = state->ref;
		snapshot->ref_size = state->ref_size;
		snapshot->target_size = state->target_size;
		snapshot->page_hash = state->page_hash;
		snapshot->kind = state->kind;
		return;
	}
	if (!IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_ZMS_NATIVE))
		return;
	/*
	 * A native backend state is sparse and can outlive the resident slot
	 * state by one mutation transition. It is part of the slot identity for
	 * WB snapshot validation, but an in-flight XA reservation is not.
	 */
	wb_state = crystal_sddc_wb_state_load_raw(sddc, index);
	if (!wb_state || wb_state == XA_ZERO_ENTRY || xa_is_err(wb_state))
		return;
	snapshot->mutation_seq = wb_state->mutation_seq;
	snapshot->ref = wb_state->ref;
	snapshot->ref_size = wb_state->ref_size;
	snapshot->target_size = wb_state->target_size;
	snapshot->page_hash = wb_state->page_hash;
	snapshot->kind = wb_state->kind;
}

bool crystal_sddc_snapshot_matches_locked(struct zram *zram, u32 index,
		const struct crystal_sddc_snapshot *snapshot)
{
	struct crystal_sddc_snapshot current_snapshot;

	crystal_sddc_snapshot_locked(zram, index, &current_snapshot);
	return current_snapshot.mutation_seq == snapshot->mutation_seq &&
		current_snapshot.kind == snapshot->kind &&
		current_snapshot.ref_size == snapshot->ref_size &&
		current_snapshot.target_size == snapshot->target_size &&
		crystal_sddc_cookie_equal(&current_snapshot.ref, &snapshot->ref);
}

static __le32 crystal_sddc_alias_v2_pack_sizes(u32 ref_size,
		u32 target_size)
{
	return cpu_to_le32(ref_size |
		(target_size << CRYSTAL_SDDC_ALIAS_TARGET_SHIFT) |
		(CRYSTAL_SDDC_ALIAS_V2_VERSION <<
		 CRYSTAL_SDDC_ALIAS_VERSION_SHIFT));
}

static u32 crystal_sddc_alias_v2_ref_size(
		const struct crystal_sddc_alias_header_v2 *header)
{
	return le32_to_cpu(header->packed_sizes) & CRYSTAL_SDDC_ALIAS_SIZE_MASK;
}

static u32 crystal_sddc_alias_v2_target_size(
		const struct crystal_sddc_alias_header_v2 *header)
{
	return (le32_to_cpu(header->packed_sizes) >>
		CRYSTAL_SDDC_ALIAS_TARGET_SHIFT) & CRYSTAL_SDDC_ALIAS_SIZE_MASK;
}

static u32 crystal_sddc_alias_v2_version(
		const struct crystal_sddc_alias_header_v2 *header)
{
	return le32_to_cpu(header->packed_sizes) >>
		CRYSTAL_SDDC_ALIAS_VERSION_SHIFT;
}

static bool crystal_sddc_alias_v2_header_valid(
		const struct crystal_sddc_ref *ref,
		const struct crystal_sddc_alias_header_v2 *header,
		u32 wire_size)
{
	u32 ref_size;
	u32 target_size;

	if (!ref || !header || wire_size != sizeof(*header) ||
	    le32_to_cpu(header->magic) != CRYSTAL_SDDC_ALIAS_V2_MAGIC ||
	    crystal_sddc_alias_v2_version(header) !=
		    CRYSTAL_SDDC_ALIAS_V2_VERSION)
		return false;

	ref_size = crystal_sddc_alias_v2_ref_size(header);
	target_size = crystal_sddc_alias_v2_target_size(header);
	return ref_size == ref->size && target_size == ref_size &&
		ref_size >= CRYSTAL_SDDC_SAMPLE_SIZE && ref_size <= PAGE_SIZE &&
		le32_to_cpu(header->ref_id) == ref->cookie.id &&
		le32_to_cpu(header->ref_generation) == ref->cookie.generation;
}

static bool crystal_sddc_native_wb_v1_header_valid(
		const struct crystal_sddc_ref *ref,
		const struct crystal_sddc_delta_header *header, u32 wire_size,
		enum crystal_sddc_kind kind)
{
	u32 magic;
	u32 target_size;

	if (!ref || !header || wire_size > PAGE_SIZE)
		return false;
	if (kind == CRYSTAL_SDDC_DELTA) {
		if (wire_size <= sizeof(*header))
			return false;
		magic = CRYSTAL_SDDC_DELTA_MAGIC;
	} else if (kind == CRYSTAL_SDDC_ALIAS) {
		if (wire_size != sizeof(*header))
			return false;
		magic = CRYSTAL_SDDC_ALIAS_MAGIC;
	} else {
		return false;
	}

	target_size = le32_to_cpu(header->target_size);
	if (le32_to_cpu(header->magic) != magic ||
	    le16_to_cpu(header->version) != CRYSTAL_SDDC_DELTA_VERSION ||
	    le16_to_cpu(header->header_size) != sizeof(*header) ||
	    le32_to_cpu(header->ref_size) != ref->size ||
	    target_size < CRYSTAL_SDDC_SAMPLE_SIZE ||
	    target_size > PAGE_SIZE)
		return false;
	if (kind == CRYSTAL_SDDC_ALIAS && target_size != ref->size)
		return false;
	return le32_to_cpu(header->ref_id) == ref->cookie.id &&
		le32_to_cpu(header->ref_generation) == ref->cookie.generation;
}

static bool crystal_sddc_native_wb_header_valid(
		const struct crystal_sddc_ref *ref, const void *wire,
		u32 wire_size, enum crystal_sddc_kind kind)
{
	if (kind == CRYSTAL_SDDC_ALIAS &&
	    wire_size == sizeof(struct crystal_sddc_alias_header_v2))
		return crystal_sddc_alias_v2_header_valid(ref, wire, wire_size);

	return crystal_sddc_native_wb_v1_header_valid(ref, wire, wire_size,
			kind);
}

static u32 crystal_sddc_native_wb_target_size(const void *wire,
		u32 wire_size, enum crystal_sddc_kind kind)
{
	if (kind == CRYSTAL_SDDC_ALIAS &&
	    wire_size == sizeof(struct crystal_sddc_alias_header_v2))
		return crystal_sddc_alias_v2_target_size(wire);

	return le32_to_cpu(((const struct crystal_sddc_delta_header *)wire)->
			target_size);
}

static bool crystal_sddc_native_wb_wire_size_valid(
		enum crystal_sddc_kind kind, u32 wire_size)
{
	if (kind == CRYSTAL_SDDC_DELTA)
		return wire_size > sizeof(struct crystal_sddc_delta_header) &&
			wire_size <= PAGE_SIZE;
	if (kind == CRYSTAL_SDDC_ALIAS)
		return wire_size == sizeof(struct crystal_sddc_alias_header_v2) ||
			wire_size == sizeof(struct crystal_sddc_delta_header);
	return false;
}

static bool crystal_sddc_delta_header_valid(
		const struct crystal_sddc_ref *ref,
		const struct crystal_sddc_delta_header *header, u32 wire_size)
{
	return crystal_sddc_native_wb_header_valid(ref, header, wire_size,
			CRYSTAL_SDDC_DELTA);
}

static bool crystal_sddc_delta_ref_valid(struct crystal_sddc *sddc,
		const struct crystal_sddc_ref *ref);

static int crystal_sddc_restore(struct crystal_sddc *sddc,
				struct crystal_sddc_ref *ref,
				enum crystal_sddc_kind kind, const void *wire,
				u32 wire_size, void *dst, size_t *size,
				bool full_page)
{
	const struct crystal_sddc_delta_header *header = wire;
	struct zcomp *comp = NULL;
	struct zcomp_strm *zstrm = NULL;
	const void *restored_stream = NULL;
	unsigned int stream_size;
	size_t output_size;
	void *ref_data;
	bool delta;
	int ret = 0;

	if (!dst || !size || !crystal_sddc_ref_valid_for_kind(sddc, ref, kind))
		return -EIO;
	delta = kind == CRYSTAL_SDDC_DELTA;
	if (!delta && kind != CRYSTAL_SDDC_REF && kind != CRYSTAL_SDDC_ALIAS)
		return -EIO;
	if (delta && !crystal_sddc_delta_header_valid(ref, header, wire_size))
		return -EIO;

	if (delta || (full_page && ref->size != PAGE_SIZE)) {
		comp = sddc->zram->comps[ref->prio];
		if (!comp || (delta && !zcomp_supports_delta(comp)))
			return -EIO;
		zstrm = zcomp_stream_get(comp);
		if (!zstrm)
			return -ENOMEM;
	}

	ref_data = zs_map_object(sddc->zram->mem_pool, ref->handle, ZS_MM_RO);
	if (!delta) {
		if (zstrm)
			ret = zcomp_decompress(zstrm, ref_data, ref->size, dst);
		else
			memcpy(dst, ref_data, ref->size);
		zs_unmap_object(sddc->zram->mem_pool, ref->handle);
		output_size = full_page ? PAGE_SIZE : ref->size;
		goto out;
	}

	ret = zcomp_decompress_delta_borrowed(zstrm,
			(const u8 *)wire + sizeof(*header),
			wire_size - sizeof(*header), ref_data, ref->size,
			&restored_stream, &stream_size);
	zs_unmap_object(sddc->zram->mem_pool, ref->handle);
	if (ret || !restored_stream ||
	    stream_size != le32_to_cpu(header->target_size)) {
		ret = -EIO;
		goto out;
	}

	/* Consume backend-owned storage before zcomp_stream_put() below. */
	if (!full_page) {
		memcpy(dst, restored_stream, stream_size);
		output_size = stream_size;
	} else if (stream_size == PAGE_SIZE) {
		memcpy(dst, restored_stream, PAGE_SIZE);
		output_size = PAGE_SIZE;
	} else {
		ret = zcomp_decompress(zstrm, restored_stream, stream_size, dst);
		output_size = PAGE_SIZE;
	}

out:
	if (zstrm)
		zcomp_stream_put(comp);
	if (ret)
		return -EIO;
	*size = output_size;
	return 0;
}

static int crystal_sddc_capture_locked(struct crystal_sddc *sddc, u32 index,
		const struct crystal_sddc_snapshot *expected,
		enum crystal_sddc_kind *kind, struct crystal_sddc_ref **ref,
		void *wire, u32 *wire_size, u32 *page_hash,
		u32 *target_size, u64 *mutation_seq)
{
	struct crystal_sddc_slot_state *state;
	struct zram *zram = sddc->zram;
	size_t object_size;
	void *src;

	if (index >= sddc->nr_slots)
		return -EAGAIN;
	state = crystal_sddc_state_load_locked(sddc, index);
	if (expected &&
	    (sddc->mutation_seq[index] != expected->mutation_seq ||
	     !state || state->kind != expected->kind ||
	     state->ref_size != expected->ref_size ||
	     state->target_size != expected->target_size ||
	     !crystal_sddc_cookie_equal(&state->ref, &expected->ref)))
		return -EAGAIN;
	if (!state)
		return -EAGAIN;
	if (crystal_sddc_test_flag(zram, index, ZRAM_WB) ||
	    (!expected && crystal_sddc_test_flag(zram, index, ZRAM_UNDER_WB)))
		return -EAGAIN;
	if (!crystal_sddc_slot_state_storage_valid_locked(sddc, index, state))
		return -EIO;

	*kind = state->kind;
	*ref = crystal_sddc_slot_state_pin_ref_locked(sddc, state, *kind);
	if (!*ref)
		return -EIO;

	if (page_hash)
		*page_hash = state->page_hash;
	if (target_size)
		*target_size = state->target_size;
	if (mutation_seq)
		*mutation_seq = sddc->mutation_seq[index];
	*wire_size = 0;
	object_size = crystal_sddc_obj_size(zram, index);
	if (*kind == CRYSTAL_SDDC_REF || *kind == CRYSTAL_SDDC_ALIAS) {
		if (state->target_size != (*ref)->size) {
			crystal_sddc_ref_put(*ref);
			*ref = NULL;
			return -EIO;
		}
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
	if (!crystal_sddc_delta_header_valid(*ref, wire, *wire_size) ||
	    le32_to_cpu(((struct crystal_sddc_delta_header *)wire)->target_size) !=
		    state->target_size) {
		crystal_sddc_ref_put(*ref);
		*ref = NULL;
		*wire_size = 0;
		return -EIO;
	}
	return 0;
}

int crystal_sddc_read_page(struct zram *zram, struct page *page, u32 index)
{
	struct crystal_sddc_ref *ref = NULL;
	struct crystal_sddc *sddc;
	enum crystal_sddc_kind kind;
	void *dst;
	size_t size;
	u32 wire_size;
	u32 page_hash = 0;
	u32 target_size = 0;
	u64 mutation_seq = 0;
	int ret;

	sddc = crystal_sddc_manager_get(zram);
	if (!sddc)
		return -EAGAIN;

	dst = kmap_local_page(page);
	crystal_sddc_slot_lock(zram, index);
	ret = crystal_sddc_capture_locked(sddc, index, NULL, &kind, &ref, dst,
					  &wire_size, &page_hash,
					  &target_size, &mutation_seq);
	crystal_sddc_slot_unlock(zram, index);
	if (!ret)
		ret = crystal_sddc_restore(sddc, ref, kind, dst, wire_size, dst,
					   &size, true);
	if (!ret) {
		if (size == PAGE_SIZE)
			crystal_sddc_check_page_hash(sddc, index, mutation_seq,
				ref, kind, ref->size, target_size, wire_size,
				page_hash, dst, "resident_read");
		else
			atomic64_inc(&sddc->stats.integrity_skipped);
	}
	kunmap_local(dst);
	if (ret && ret != -EAGAIN)
		atomic64_inc(&sddc->stats.decode_failures);
	crystal_sddc_ref_put(ref);
	crystal_sddc_manager_put(sddc);
	return ret;
}

int crystal_sddc_read_page_locked(struct zram *zram, struct page *page,
		u32 index)
{
	struct crystal_sddc_ref *ref = NULL;
	struct crystal_sddc *sddc;
	enum crystal_sddc_kind kind;
	void *dst;
	size_t size;
	u32 wire_size;
	u32 page_hash = 0;
	u32 target_size = 0;
	u64 mutation_seq = 0;
	int ret;

	if (!zram)
		return -EINVAL;
	if (!page) {
		crystal_sddc_slot_unlock(zram, index);
		return -EINVAL;
	}
	sddc = crystal_sddc_manager_get(zram);
	if (!sddc) {
		crystal_sddc_slot_unlock(zram, index);
		return -EAGAIN;
	}

	dst = kmap_local_page(page);
	ret = crystal_sddc_capture_locked(sddc, index, NULL, &kind, &ref, dst,
					  &wire_size, &page_hash,
					  &target_size, &mutation_seq);
	crystal_sddc_slot_unlock(zram, index);
	kunmap_local(dst);
	if (ret)
		goto out;

	dst = kmap_local_page(page);
	ret = crystal_sddc_restore(sddc, ref, kind, dst, wire_size, dst, &size,
					 true);
	if (!ret) {
		if (size == PAGE_SIZE)
			crystal_sddc_check_page_hash(sddc, index, mutation_seq,
				ref, kind, ref->size, target_size, wire_size,
				page_hash, dst, "resident_read_locked");
		else
			atomic64_inc(&sddc->stats.integrity_skipped);
	}
	kunmap_local(dst);

out:
	if (ret && ret != -EAGAIN)
		atomic64_inc(&sddc->stats.decode_failures);
	crystal_sddc_ref_put(ref);
	crystal_sddc_manager_put(sddc);
	return ret;
}

int crystal_sddc_flatten(struct zram *zram, u32 index,
			 const struct crystal_sddc_snapshot *snapshot,
			 void *dst, size_t *size)
{
	struct crystal_sddc_ref *ref = NULL;
	struct crystal_sddc *sddc;
	enum crystal_sddc_kind kind;
	u32 wire_size;
	int ret;

	if (!zram || !snapshot || !dst || !size)
		return -EINVAL;
	sddc = crystal_sddc_manager_get(zram);
	if (!sddc)
		return -EAGAIN;

	crystal_sddc_slot_lock(zram, index);
	ret = crystal_sddc_capture_locked(sddc, index, snapshot, &kind, &ref,
					  dst, &wire_size, NULL, NULL, NULL);
	crystal_sddc_slot_unlock(zram, index);
	if (ret) {
		if (ret != -EAGAIN)
			atomic64_inc(&sddc->stats.decode_failures);
		goto out;
	}

	ret = crystal_sddc_restore(sddc, ref, kind, dst, wire_size, dst, size,
				   false);
	if (ret)
		atomic64_inc(&sddc->stats.decode_failures);

out:
	if (ret && ret != -EAGAIN)
		atomic64_inc(&sddc->stats.flatten_failures);
	crystal_sddc_ref_put(ref);
	crystal_sddc_manager_put(sddc);
	return ret;
}

bool crystal_sddc_native_wb_kind(u8 kind)
{
	return IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_ZMS_NATIVE) &&
		(kind == CRYSTAL_SDDC_DELTA ||
		 kind == CRYSTAL_SDDC_ALIAS);
}

static void crystal_sddc_native_wb_capture_abort_internal(
		struct crystal_sddc *sddc, u32 index,
		struct crystal_sddc_wb_state *state)
{
	struct crystal_sddc_ref *ref;

	if (!state)
		return;
	if (crystal_sddc_xa_release_reservation(&sddc->wb_states, index)) {
		ref = state->ref_obj;
		state->ref_obj = NULL;
		crystal_sddc_ref_put(ref);
		kfree(state);
		return;
	}
	/*
	 * A published state is owned by the slot and must not be reclaimed by a
	 * late abort. Do not free @state when another owner already consumed the
	 * reservation; doing so would turn a duplicate abort into a UAF.
	 */
	WARN_ON_ONCE(1);
}

static bool crystal_sddc_alias_ref_valid(struct crystal_sddc *sddc,
		const struct crystal_sddc_ref *ref)
{
	return crystal_sddc_ref_valid_for_kind(sddc, ref, CRYSTAL_SDDC_ALIAS);
}

static bool crystal_sddc_delta_ref_valid(struct crystal_sddc *sddc,
		const struct crystal_sddc_ref *ref)
{
	return crystal_sddc_ref_valid_for_kind(sddc, ref, CRYSTAL_SDDC_DELTA);
}

int crystal_sddc_native_wb_capture(struct zram *zram, u32 index,
		const struct crystal_sddc_snapshot *snapshot, void *dst,
		size_t *size, struct crystal_sddc_wb_capture *capture)
{
	struct crystal_sddc *sddc;
	struct crystal_sddc_slot_state *slot_state;
	struct crystal_sddc_wb_state *wb_state;
	struct crystal_sddc_ref *ref = NULL;
	const struct crystal_sddc_delta_header *header;
	struct crystal_sddc_alias_header_v2 alias_header;
	void *src;
	u32 target_size;
	u8 kind;
	int ret = -EAGAIN;

	if (capture)
		memset(capture, 0, sizeof(*capture));
	if (size)
		*size = 0;
	kind = snapshot ? snapshot->kind : CRYSTAL_SDDC_NONE;
	if (!zram || !snapshot || !dst || !size || !capture ||
	    !crystal_sddc_native_wb_kind(kind))
		return -EOPNOTSUPP;

	sddc = crystal_sddc_manager_get(zram);
	if (!sddc || index >= sddc->nr_slots)
		goto out_manager;

	/*
	 * Reserve before taking the slot lock. Publication then becomes a
	 * non-allocating operation in the final writeback transaction.
	 */
	wb_state = kzalloc(sizeof(*wb_state), GFP_NOIO | __GFP_NOWARN);
	if (!wb_state) {
		ret = -ENOMEM;
		goto out_manager;
	}
	ret = xa_insert(&sddc->wb_states, index, NULL,
			GFP_NOIO | __GFP_NOWARN);
	if (ret) {
		kfree(wb_state);
		goto out_manager;
	}

	crystal_sddc_slot_lock(zram, index);
	if (!crystal_sddc_snapshot_matches_locked(zram, index, snapshot))
		goto abort_reservation;
	slot_state = crystal_sddc_state_load_locked(sddc, index);
	if (!slot_state || slot_state->kind != kind)
		goto abort_reservation;
	if (!crystal_sddc_slot_state_storage_valid_locked(sddc, index,
			slot_state))
		goto abort_reservation;
	ref = crystal_sddc_slot_state_pin_ref_locked(sddc, slot_state, kind);
	if (!ref || ref->size != slot_state->ref_size)
		goto abort_ref;
	if (kind == CRYSTAL_SDDC_DELTA) {
		if (!crystal_sddc_delta_ref_valid(sddc, ref))
			goto abort_ref;
		if (!zram->table[index].handle ||
		    crystal_sddc_obj_size(zram, index) <= sizeof(*header) ||
		    crystal_sddc_obj_size(zram, index) > PAGE_SIZE)
			goto abort_ref;

		*size = crystal_sddc_obj_size(zram, index);
		src = zs_map_object(zram->mem_pool, zram->table[index].handle,
				ZS_MM_RO);
		header = src;
		if (!crystal_sddc_delta_header_valid(ref, header, *size)) {
			zs_unmap_object(zram->mem_pool, zram->table[index].handle);
			goto abort_ref;
		}
		target_size = le32_to_cpu(header->target_size);
		if (target_size != slot_state->target_size) {
			zs_unmap_object(zram->mem_pool, zram->table[index].handle);
			goto abort_ref;
		}
		memcpy(dst, src, *size);
		zs_unmap_object(zram->mem_pool, zram->table[index].handle);
	} else {
		if (!crystal_sddc_alias_ref_valid(sddc, ref) ||
		    zram->table[index].handle ||
		    crystal_sddc_obj_size(zram, index))
			goto abort_ref;
		target_size = ref->size;
		alias_header.magic = cpu_to_le32(CRYSTAL_SDDC_ALIAS_V2_MAGIC);
		alias_header.ref_id = cpu_to_le32(ref->cookie.id);
		alias_header.ref_generation = cpu_to_le32(ref->cookie.generation);
		alias_header.packed_sizes = crystal_sddc_alias_v2_pack_sizes(
				ref->size, target_size);
		*size = sizeof(alias_header);
		memcpy(dst, &alias_header, *size);
		if (!crystal_sddc_native_wb_header_valid(ref, dst, *size, kind))
			goto abort_ref;
	}

	wb_state->mutation_seq = snapshot->mutation_seq;
	wb_state->ref = ref->cookie;
	wb_state->ref_obj = ref;
	wb_state->ref_size = ref->size;
	wb_state->target_size = target_size;
	wb_state->wire_size = *size;
	wb_state->page_hash = slot_state->page_hash;
	wb_state->kind = kind;
	capture->private = wb_state;
	capture->manager = sddc;
	capture->index = index;
	capture->ref = ref->cookie;
	capture->ref_size = ref->size;
	capture->target_size = wb_state->target_size;
	capture->wire_size = *size;
	capture->page_hash = wb_state->page_hash;
	capture->kind = kind;
	crystal_sddc_slot_unlock(zram, index);
	return 0;

abort_ref:
	crystal_sddc_ref_put(ref);
abort_reservation:
	*size = 0;
	crystal_sddc_slot_unlock(zram, index);
	crystal_sddc_native_wb_capture_abort_internal(sddc, index, wb_state);
out_manager:
	if (sddc)
		crystal_sddc_manager_put(sddc);
	return ret;
}

bool crystal_sddc_native_wb_install_locked(struct zram *zram, u32 index,
		const struct crystal_sddc_snapshot *snapshot,
		struct crystal_sddc_wb_capture *capture)
{
	struct crystal_sddc *sddc;
	struct crystal_sddc_slot_state *slot_state;
	struct crystal_sddc_wb_state *state;
	u8 kind;

	if (!zram || !snapshot || !capture || !capture->private ||
		!crystal_sddc_native_wb_kind(snapshot->kind))
		return false;
	kind = snapshot->kind;
	sddc = capture->manager;
	state = capture->private;
	if (!sddc || sddc != zram->sddc || !READ_ONCE(zram->table) ||
		index >= sddc->nr_slots ||
		capture->index != index || capture->kind != kind ||
		state->kind != kind ||
		!crystal_sddc_cookie_equal(&capture->ref, &snapshot->ref) ||
		(snapshot->ref_size && snapshot->ref_size != capture->ref_size) ||
		(snapshot->target_size &&
		 snapshot->target_size != capture->target_size) ||
		state->wire_size != capture->wire_size ||
		state->ref_size != capture->ref_size ||
		state->target_size != capture->target_size ||
		!state->ref_obj ||
		!crystal_sddc_ref_valid_for_kind(sddc, state->ref_obj, kind) ||
		state->ref_obj->size != state->ref_size ||
		!crystal_sddc_cookie_equal(&state->ref, &capture->ref) ||
		!crystal_sddc_cookie_equal(&state->ref_obj->cookie,
			&capture->ref) ||
		!crystal_sddc_native_wb_wire_size_valid(kind,
			capture->wire_size))
		return false;
	slot_state = crystal_sddc_state_load_locked(sddc, index);
	if (!slot_state || slot_state->kind != kind ||
	    !crystal_sddc_cookie_equal(&slot_state->ref, &capture->ref) ||
	    slot_state->ref_obj != state->ref_obj ||
	    slot_state->ref_size != capture->ref_size ||
	    slot_state->target_size != capture->target_size ||
	    !crystal_sddc_slot_state_storage_valid_locked(sddc, index,
		    slot_state))
		return false;
	if (kind == CRYSTAL_SDDC_DELTA) {
		if (crystal_sddc_obj_size(zram, index) != capture->wire_size)
			return false;
	} else if (zram->table[index].handle ||
		   crystal_sddc_obj_size(zram, index)) {
		return false;
	}
	if (!crystal_sddc_snapshot_matches_locked(zram, index, snapshot))
		return false;
	if (!crystal_sddc_wb_state_install_locked(sddc, index, state))
		return false;
	capture->private = NULL;
	return true;
}

void crystal_sddc_native_wb_finalize_locked(struct zram *zram, u32 index)
{
	struct crystal_sddc *sddc;
	struct crystal_sddc_wb_state *state;

	if (!zram || !zram->sddc)
		return;
	sddc = zram->sddc;
	if (index >= sddc->nr_slots)
		return;
	state = crystal_sddc_wb_state_load_raw(sddc, index);
	if (!state || state == XA_ZERO_ENTRY || xa_is_err(state) ||
		!crystal_sddc_native_wb_kind(state->kind) || !state->ref_obj)
		return;
	state->mutation_seq = sddc->mutation_seq[index];
	if (!state->accounted) {
		state->accounted = true;
		crystal_sddc_native_wb_account_pin(sddc, state);
		if (state->kind == CRYSTAL_SDDC_DELTA) {
			atomic64_inc(&sddc->stats.wb_deltas);
			atomic64_add(state->wire_size,
				     &sddc->stats.wb_delta_bytes);
		} else {
			atomic64_inc(&sddc->stats.wb_aliases);
			atomic64_add(state->wire_size,
				     &sddc->stats.wb_alias_bytes);
		}
		crystal_sddc_native_wb_log_pin(sddc, index, state, "finalize");
	}
}

void crystal_sddc_native_wb_abort(struct zram *zram,
		struct crystal_sddc_wb_capture *capture)
{
	struct crystal_sddc *sddc;
	struct crystal_sddc_wb_state *state;

	if (!capture)
		return;
	sddc = capture->manager;
	state = capture->private;
	if (sddc && state)
		crystal_sddc_native_wb_capture_abort_internal(sddc,
				capture->index, state);
	if (sddc)
		crystal_sddc_manager_put(sddc);
	memset(capture, 0, sizeof(*capture));
}

void crystal_sddc_native_wb_finish(struct crystal_sddc_wb_capture *capture)
{
	struct crystal_sddc *sddc;

	if (!capture)
		return;
	sddc = capture->manager;
	if (sddc && capture->private)
		crystal_sddc_native_wb_capture_abort_internal(sddc,
				capture->index, capture->private);
	if (sddc)
		crystal_sddc_manager_put(sddc);
	memset(capture, 0, sizeof(*capture));
}

void crystal_sddc_native_wb_free_locked(struct zram *zram, u32 index)
{
	struct crystal_sddc *sddc;
	struct crystal_sddc_wb_state *state;

	if (!zram || !zram->sddc)
		return;
	sddc = zram->sddc;
	if (index >= sddc->nr_slots)
		return;
	state = crystal_sddc_wb_state_erase_locked(sddc, index);
	if (!state)
		return;
	if (state->kind == CRYSTAL_SDDC_DELTA && state->accounted) {
		atomic64_dec(&sddc->stats.wb_deltas);
		atomic64_sub(state->wire_size, &sddc->stats.wb_delta_bytes);
	} else if (state->kind == CRYSTAL_SDDC_ALIAS && state->accounted) {
		atomic64_dec(&sddc->stats.wb_aliases);
		atomic64_sub(state->wire_size, &sddc->stats.wb_alias_bytes);
	}
	crystal_sddc_native_wb_unaccount_pin(sddc, state);
	crystal_sddc_native_wb_log_pin(sddc, index, state, "free");
	crystal_sddc_ref_put(state->ref_obj);
	kfree(state);
}

bool crystal_sddc_native_wb_pin_locked(struct zram *zram, u32 index,
		const struct crystal_sddc_snapshot *snapshot,
		struct crystal_sddc_wb_ref *wb_ref)
{
	struct crystal_sddc *sddc;
	struct crystal_sddc_wb_state *state;
	struct crystal_sddc_ref *ref;
	u8 kind;

	if (wb_ref)
		memset(wb_ref, 0, sizeof(*wb_ref));
	if (!zram || !snapshot || !wb_ref ||
	    !crystal_sddc_native_wb_kind(snapshot->kind))
		return false;
	kind = snapshot->kind;

	sddc = crystal_sddc_manager_get(zram);
	if (!sddc)
		return false;
	if (index >= sddc->nr_slots ||
	    !crystal_sddc_test_flag(zram, index, ZRAM_WB) ||
	    !crystal_sddc_test_flag(zram, index, ZRAM_UNDER_WB) ||
	    !crystal_sddc_snapshot_matches_locked(zram, index, snapshot))
		goto fail;

	state = crystal_sddc_wb_state_load_raw(sddc, index);
	if (!state || state == XA_ZERO_ENTRY || xa_is_err(state) ||
	    state->kind != kind || !state->ref_obj ||
	    !crystal_sddc_cookie_equal(&state->ref, &snapshot->ref) ||
	    state->ref_size != snapshot->ref_size ||
	    state->target_size != snapshot->target_size ||
	    state->wire_size > PAGE_SIZE ||
	    state->wire_size != crystal_sddc_obj_size(zram, index))
		goto fail;
	if (!crystal_sddc_native_wb_wire_size_valid(kind, state->wire_size))
		goto fail;

	ref = crystal_sddc_wb_state_pin_ref_locked(sddc, state, kind);
	if (!ref)
		goto fail;
	if (ref != state->ref_obj ||
	    state->ref_size != ref->size ||
	    snapshot->ref_size != ref->size ||
	    snapshot->target_size != state->target_size ||
	    (kind == CRYSTAL_SDDC_DELTA &&
	     !crystal_sddc_delta_ref_valid(sddc, ref)) ||
	    (kind == CRYSTAL_SDDC_ALIAS &&
	     !crystal_sddc_alias_ref_valid(sddc, ref))) {
		crystal_sddc_ref_put(ref);
		goto fail;
	}

	wb_ref->private = ref;
	wb_ref->manager = sddc;
	wb_ref->ref = ref->cookie;
	wb_ref->ref_size = ref->size;
	wb_ref->target_size = state->target_size;
	wb_ref->wire_size = state->wire_size;
	wb_ref->page_hash = state->page_hash;
	wb_ref->kind = kind;
	return true;

fail:
	crystal_sddc_manager_put(sddc);
	return false;
}

void crystal_sddc_native_wb_put_ref(struct crystal_sddc_wb_ref *wb_ref)
{
	struct crystal_sddc *sddc;
	struct crystal_sddc_ref *ref;

	if (!wb_ref)
		return;
	sddc = wb_ref->manager;
	ref = wb_ref->private;
	crystal_sddc_ref_put(ref);
	if (sddc)
		crystal_sddc_manager_put(sddc);
	memset(wb_ref, 0, sizeof(*wb_ref));
}

static int crystal_sddc_native_wb_restore_ref(struct crystal_sddc *sddc,
		struct crystal_sddc_ref *ref, struct page *page, u32 index,
		const struct crystal_sddc_snapshot *snapshot,
		const void *wire, size_t wire_size)
{
	void *dst;
	size_t size = 0;
	u32 target_size;
	int ret;
	u8 kind;

	if (!sddc || !ref || !page || !snapshot || !wire ||
	    !crystal_sddc_native_wb_kind(snapshot->kind))
		return -EOPNOTSUPP;
	kind = snapshot->kind;
	if ((kind == CRYSTAL_SDDC_DELTA &&
	     !crystal_sddc_delta_ref_valid(sddc, ref)) ||
	    (kind == CRYSTAL_SDDC_ALIAS &&
	     !crystal_sddc_alias_ref_valid(sddc, ref))) {
		ret = -EIO;
		goto out;
	}
	if (!crystal_sddc_cookie_equal(&ref->cookie, &snapshot->ref)) {
		ret = -EIO;
		goto out;
	}
	if (snapshot->ref_size && snapshot->ref_size != ref->size) {
		ret = -EIO;
		goto out;
	}
	if (wire_size > U32_MAX ||
	    !crystal_sddc_native_wb_header_valid(ref, wire, (u32)wire_size,
		    kind)) {
		ret = -EIO;
		goto out;
	}
	target_size = crystal_sddc_native_wb_target_size(wire,
			(u32)wire_size, kind);
	if ((snapshot->target_size &&
	     snapshot->target_size != target_size)) {
		ret = -EIO;
		goto out;
	}
	dst = kmap_local_page(page);
	ret = crystal_sddc_restore(sddc, ref, kind, wire, wire_size, dst,
			&size, true);
	if (!ret) {
		if (size == PAGE_SIZE)
			crystal_sddc_check_page_hash(sddc, index,
				snapshot->mutation_seq, ref, kind, ref->size,
				target_size, (u32)wire_size, snapshot->page_hash,
				dst, "native_wb_read");
		else
			atomic64_inc(&sddc->stats.integrity_skipped);
	}
	kunmap_local(dst);
out:
	if (ret && ret != -EAGAIN)
		atomic64_inc(&sddc->stats.decode_failures);
	return ret;
}

int crystal_sddc_native_wb_restore_page(struct zram *zram, struct page *page,
		u32 index, const struct crystal_sddc_snapshot *snapshot,
		const void *wire, size_t wire_size)
{
	struct crystal_sddc *sddc;
	struct crystal_sddc_ref *ref = NULL;
	int ret;

	if (!zram || !page || !snapshot || !wire ||
	    !crystal_sddc_native_wb_kind(snapshot->kind))
		return -EOPNOTSUPP;
	sddc = crystal_sddc_manager_get(zram);
	if (!sddc)
		return -EAGAIN;
	if (index >= sddc->nr_slots) {
		ret = -EAGAIN;
		goto out;
	}
	ref = crystal_sddc_ref_pin(sddc, &snapshot->ref);
	if (!ref) {
		ret = -EAGAIN;
		goto out;
	}
	ret = crystal_sddc_native_wb_restore_ref(sddc, ref, page, index,
			snapshot, wire, wire_size);
	crystal_sddc_ref_put(ref);
out:
	crystal_sddc_manager_put(sddc);
	return ret;
}

int crystal_sddc_native_wb_restore_pinned(struct zram *zram, struct page *page,
		u32 index, const struct crystal_sddc_snapshot *snapshot,
		struct crystal_sddc_wb_ref *wb_ref,
		const void *wire, size_t wire_size)
{
	struct crystal_sddc *sddc;
	struct crystal_sddc_ref *ref;

	if (!zram || !page || !snapshot || !wb_ref || !wire ||
	    !crystal_sddc_native_wb_kind(snapshot->kind))
		return -EOPNOTSUPP;
	sddc = wb_ref->manager;
	ref = wb_ref->private;
	if (!sddc || sddc->zram != zram || !ref ||
	    index >= sddc->nr_slots ||
	    wb_ref->kind != snapshot->kind ||
	    !crystal_sddc_cookie_equal(&wb_ref->ref, &snapshot->ref) ||
	    (wb_ref->ref_size && snapshot->ref_size &&
	     wb_ref->ref_size != snapshot->ref_size) ||
	    (wb_ref->target_size && snapshot->target_size &&
	     wb_ref->target_size != snapshot->target_size) ||
	    (wb_ref->wire_size && wire_size != wb_ref->wire_size))
		return -EAGAIN;

	return crystal_sddc_native_wb_restore_ref(sddc, ref, page, index,
			snapshot, wire, wire_size);
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
	stats->coalesced = atomic64_read(&sddc->stats.coalesced);
	stats->dropped = atomic64_read(&sddc->stats.dropped);
	stats->ineligible = atomic64_read(&sddc->stats.ineligible);
	stats->shutdown_discarded =
		atomic64_read(&sddc->stats.shutdown_discarded);
	stats->worker_runs = atomic64_read(&sddc->stats.worker_runs);
	stats->observed = atomic64_read(&sddc->stats.observed);
	stats->stale = atomic64_read(&sddc->stats.stale);
	stats->indexed = atomic64_read(&sddc->stats.indexed);
	stats->refs = atomic64_read(&sddc->stats.refs);
	stats->ref_bytes = atomic64_read(&sddc->stats.ref_bytes);
	stats->wb_ref_pins = atomic64_read(&sddc->stats.wb_ref_pins);
	stats->wb_ref_pin_bytes = atomic64_read(&sddc->stats.wb_ref_pin_bytes);
	stats->wb_ref_pin_max = atomic64_read(&sddc->stats.wb_ref_pin_max);
	stats->wb_ref_pin_bytes_max =
		atomic64_read(&sddc->stats.wb_ref_pin_bytes_max);
	stats->wb_ref_pin_events =
		atomic64_read(&sddc->stats.wb_ref_pin_events);
	stats->wb_ref_unpin_events =
		atomic64_read(&sddc->stats.wb_ref_unpin_events);
	stats->wb_deltas = atomic64_read(&sddc->stats.wb_deltas);
	stats->wb_delta_bytes = atomic64_read(&sddc->stats.wb_delta_bytes);
	stats->wb_aliases = atomic64_read(&sddc->stats.wb_aliases);
	stats->wb_alias_bytes = atomic64_read(&sddc->stats.wb_alias_bytes);
	stats->aliases = atomic64_read(&sddc->stats.aliases);
	stats->deltas = atomic64_read(&sddc->stats.deltas);
	stats->delta_bytes = atomic64_read(&sddc->stats.delta_bytes);
	stats->alias_attempts = atomic64_read(&sddc->stats.alias_attempts);
	stats->alias_hits = atomic64_read(&sddc->stats.alias_hits);
	stats->delta_attempts = atomic64_read(&sddc->stats.delta_attempts);
	stats->delta_hits = atomic64_read(&sddc->stats.delta_hits);
	stats->delta_matches = atomic64_read(&sddc->stats.delta_matches);
	stats->delta_small_rejects =
		atomic64_read(&sddc->stats.delta_small_rejects);
	stats->delta_no_gain = atomic64_read(&sddc->stats.delta_no_gain);
	stats->delta_match_bytes_max =
		atomic64_read(&sddc->stats.delta_match_bytes_max);
	stats->saved_bytes = atomic64_read(&sddc->stats.saved_bytes);
	stats->saved_bytes_total =
		atomic64_read(&sddc->stats.saved_bytes_total);
	stats->released_aliases = atomic64_read(&sddc->stats.released_aliases);
	stats->released_deltas = atomic64_read(&sddc->stats.released_deltas);
	stats->released_saved_bytes =
		atomic64_read(&sddc->stats.released_saved_bytes);
	stats->released_writeback_saved_bytes =
		atomic64_read(&sddc->stats.released_writeback_saved_bytes);
	stats->released_rewrite_saved_bytes =
		atomic64_read(&sddc->stats.released_rewrite_saved_bytes);
	stats->released_recompress_saved_bytes =
		atomic64_read(&sddc->stats.released_recompress_saved_bytes);
	stats->released_discard_saved_bytes =
		atomic64_read(&sddc->stats.released_discard_saved_bytes);
	stats->released_notify_free_saved_bytes =
		atomic64_read(&sddc->stats.released_notify_free_saved_bytes);
	stats->released_reset_saved_bytes =
		atomic64_read(&sddc->stats.released_reset_saved_bytes);
	stats->released_other_saved_bytes =
		atomic64_read(&sddc->stats.released_other_saved_bytes);
	stats->conversion_failures =
		atomic64_read(&sddc->stats.conversion_failures);
	stats->decode_failures = atomic64_read(&sddc->stats.decode_failures);
	stats->flatten_failures = atomic64_read(&sddc->stats.flatten_failures);
	stats->integrity_checks =
		atomic64_read(&sddc->stats.integrity_checks);
	stats->integrity_failures =
		atomic64_read(&sddc->stats.integrity_failures);
	stats->integrity_skipped =
		atomic64_read(&sddc->stats.integrity_skipped);
	stats->integrity_hash_failures =
		atomic64_read(&sddc->stats.integrity_hash_failures);
	stats->limit_rejects = atomic64_read(&sddc->stats.limit_rejects);
	stats->pending_max = atomic64_read(&sddc->stats.pending_max);
	spin_lock(&sddc->state_lock);
	stats->pending = sddc->pending;
	spin_unlock(&sddc->state_lock);
	crystal_sddc_manager_put(sddc);
}

bool crystal_sddc_debug_snapshot(struct zram *zram,
		struct crystal_sddc_stats_snapshot *stats)
{
	if (!stats)
		return false;

	crystal_sddc_get_stats(zram, stats);
	return stats->enabled;
}

void crystal_sddc_queue_observation(struct zram *zram,
		const struct crystal_sddc_job_key *key)
{
	struct crystal_sddc *sddc;
	u32 index;
	unsigned int first_work;
	unsigned int i;
	bool queued_work = false;
	bool worker_zram_ref = false;

	if (!zram || !key || !key->handle ||
	    key->size < CRYSTAL_SDDC_SAMPLE_SIZE || key->size > PAGE_SIZE)
		return;
	/* Pin the device before looking up its manager.  The caller may be a
	 * best-effort notification path without its own zram lifetime token. */
	if (!zram_try_get(zram))
		return;
	worker_zram_ref = true;
	sddc = crystal_sddc_manager_get(zram);
	if (!sddc)
		goto put_zram;
	index = key->index;
	if (index >= sddc->nr_slots) {
		atomic64_inc(&sddc->stats.dropped);
		goto put_manager;
	}

	spin_lock(&sddc->state_lock);
	if (READ_ONCE(sddc->stopping))
		goto reject_locked;
	if (!crystal_sddc_observe_mark_locked(sddc, index))
		goto coalesced_locked;
	atomic64_inc(&sddc->stats.queued);
	if (!crystal_sddc_observe_activate_locked(sddc))
		goto active_locked;

	/* Only the idle-to-active producer wakes the drain.  The worker token is
	 * separate from this producer's token and follows budget handoffs.
	 */
	first_work = sddc->observe_work_cursor++ % CRYSTAL_SDDC_OBSERVE_WORKS;
	for (i = 0; i < CRYSTAL_SDDC_OBSERVE_WORKS; i++) {
		unsigned int work_index = (first_work + i) %
			CRYSTAL_SDDC_OBSERVE_WORKS;

		/* The callback drops this operation reference when it returns. */
		spin_lock(&sddc->active_lock);
		atomic_inc(&sddc->active_ops);
		spin_unlock(&sddc->active_lock);
		if (queue_work(sddc->workqueue,
				&sddc->observe_work[work_index].work)) {
			queued_work = true;
			sddc->observe_work_cursor = (work_index + 1) %
				CRYSTAL_SDDC_OBSERVE_WORKS;
			break;
		}
		/* A false result means that item is already pending or running;
		 * undo the reference reserved for this failed attempt. */
		crystal_sddc_manager_put(sddc);
	}
	/* If both items report busy, a callback from the preceding drain is still
	 * pending or returning and owns the newly set bit.
	 */
active_locked:
	spin_unlock(&sddc->state_lock);

	/* Release the manager token before the device reference.  Dropping the
	 * latter can complete zram removal, which may destroy @sddc. */
	crystal_sddc_manager_put(sddc);
	if (!queued_work)
		zram_put(zram);
	return;

coalesced_locked:
	spin_unlock(&sddc->state_lock);
	crystal_sddc_manager_put(sddc);
	if (worker_zram_ref)
		zram_put(zram);
	return;

reject_locked:
	spin_unlock(&sddc->state_lock);
	atomic64_inc(&sddc->stats.dropped);
	crystal_sddc_manager_put(sddc);
	if (worker_zram_ref)
		zram_put(zram);
	return;

put_manager:
	crystal_sddc_manager_put(sddc);
	if (worker_zram_ref)
		zram_put(zram);
	return;

put_zram:
	if (worker_zram_ref)
		zram_put(zram);
}

#if IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_KUNIT_TEST)

#define CRYSTAL_SDDC_TEST_SLOTS	2
#define CRYSTAL_SDDC_TEST_PAGE_HASH	0x13579bdfU

struct crystal_sddc_test_ctx {
	struct zram zram;
	struct crystal_sddc sddc;
	u64 mutation_seq[CRYSTAL_SDDC_TEST_SLOTS];
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
	ctx->sddc.mutation_seq = ctx->mutation_seq;
	ctx->sddc.nr_slots = CRYSTAL_SDDC_TEST_SLOTS;
	spin_lock_init(&ctx->sddc.state_lock);
	spin_lock_init(&ctx->sddc.index_lock);
	spin_lock_init(&ctx->sddc.active_lock);
	ida_init(&ctx->sddc.ref_ids);
	xa_init(&ctx->sddc.refs);
	xa_init(&ctx->sddc.slot_states);
	xa_init(&ctx->sddc.wb_states);
	atomic_set(&ctx->sddc.active_ops, 0);
	init_waitqueue_head(&ctx->sddc.active_wait);
	ctx->sddc.observe_pending = bitmap_zalloc(CRYSTAL_SDDC_TEST_SLOTS,
			GFP_KERNEL);
	if (!ctx->sddc.observe_pending)
		return -ENOMEM;

	test->priv = ctx;
	return 0;
}

static void crystal_sddc_state_test_exit(struct kunit *test)
{
	struct crystal_sddc_test_ctx *ctx = test->priv;
	struct crystal_sddc_slot_state *state;
	struct crystal_sddc_wb_state *wb_state;
	unsigned long index;

	if (!ctx)
		return;

	bitmap_free(ctx->sddc.observe_pending);
	xa_for_each(&ctx->sddc.slot_states, index, state) {
		state = xa_erase(&ctx->sddc.slot_states, index);
		if (state && state != XA_ZERO_ENTRY && !xa_is_err(state))
			kfree(state);
	}
	xa_for_each(&ctx->sddc.wb_states, index, wb_state) {
		wb_state = xa_erase(&ctx->sddc.wb_states, index);
		if (wb_state && wb_state != XA_ZERO_ENTRY && !xa_is_err(wb_state))
			kfree(wb_state);
	}
	xa_destroy(&ctx->sddc.slot_states);
	xa_destroy(&ctx->sddc.wb_states);
	xa_destroy(&ctx->sddc.refs);
	ida_destroy(&ctx->sddc.ref_ids);
}

static struct crystal_sddc_slot_state *
crystal_sddc_test_install_state(struct crystal_sddc_test_ctx *ctx, u32 index,
		enum crystal_sddc_kind kind)
{
	struct crystal_sddc_slot_state *state;
	int ret;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;
	state->kind = kind;
	state->ref_size = CRYSTAL_SDDC_INDEX_MIN_SIZE;
	state->target_size = CRYSTAL_SDDC_INDEX_MIN_SIZE;
	state->page_hash = CRYSTAL_SDDC_TEST_PAGE_HASH;
	ret = xa_insert(&ctx->sddc.slot_states, index, state, GFP_KERNEL);
	if (ret) {
		kfree(state);
		return NULL;
	}
	return state;
}

static void crystal_sddc_observe_bitmap_test(struct kunit *test)
{
	struct crystal_sddc_test_ctx *ctx = test->priv;
	u32 index;

	ctx->sddc.observe_worker_active = true;
	KUNIT_EXPECT_TRUE(test,
		crystal_sddc_observe_mark_locked(&ctx->sddc, 1));
	KUNIT_EXPECT_FALSE(test,
		crystal_sddc_observe_mark_locked(&ctx->sddc, 1));
	KUNIT_EXPECT_EQ(test, ctx->sddc.pending, 1U);
	KUNIT_EXPECT_EQ(test,
		atomic64_read(&ctx->sddc.stats.coalesced), (s64)1);

	KUNIT_ASSERT_TRUE(test,
		crystal_sddc_observe_take_locked(&ctx->sddc, &index));
	KUNIT_EXPECT_EQ(test, index, (u32)1);
	KUNIT_EXPECT_EQ(test, ctx->sddc.pending, 0U);
	KUNIT_EXPECT_FALSE(test, test_bit(1, ctx->sddc.observe_pending));
	/* Taking the last bit does not end the callback.  The worker commits its
	 * active-to-idle transition after observing the empty queue under lock.
	 */
	KUNIT_EXPECT_TRUE(test, ctx->sddc.observe_worker_active);
	KUNIT_EXPECT_FALSE(test,
		crystal_sddc_observe_take_locked(&ctx->sddc, &index));
	KUNIT_EXPECT_TRUE(test, ctx->sddc.observe_worker_active);
	crystal_sddc_observe_deactivate_locked(&ctx->sddc, 0);
	KUNIT_EXPECT_FALSE(test, ctx->sddc.observe_worker_active);
	KUNIT_EXPECT_EQ(test, ctx->sddc.observe_work_cursor, 1U);
	KUNIT_EXPECT_TRUE(test, crystal_sddc_observe_activate_locked(&ctx->sddc));
	KUNIT_EXPECT_FALSE(test, crystal_sddc_observe_activate_locked(&ctx->sddc));

	/* The cursor wraps and a second request for a consumed slot is
	 * represented by a new bit, not by an extra queue object. */
	ctx->sddc.observe_worker_active = true;
	KUNIT_EXPECT_TRUE(test,
		crystal_sddc_observe_mark_locked(&ctx->sddc, 0));
	KUNIT_EXPECT_TRUE(test,
		crystal_sddc_observe_mark_locked(&ctx->sddc, 1));
	KUNIT_ASSERT_TRUE(test,
		crystal_sddc_observe_take_locked(&ctx->sddc, &index));
	KUNIT_EXPECT_EQ(test, index, (u32)0);
	KUNIT_ASSERT_TRUE(test,
		crystal_sddc_observe_take_locked(&ctx->sddc, &index));
	KUNIT_EXPECT_EQ(test, index, (u32)1);
	KUNIT_EXPECT_EQ(test, ctx->sddc.pending, 0U);
}

static void crystal_sddc_observe_discard_test(struct kunit *test)
{
	struct crystal_sddc_test_ctx *ctx = test->priv;

	KUNIT_EXPECT_TRUE(test,
		crystal_sddc_observe_mark_locked(&ctx->sddc, 0));
	KUNIT_EXPECT_TRUE(test,
		crystal_sddc_observe_mark_locked(&ctx->sddc, 1));
	KUNIT_EXPECT_EQ(test,
		crystal_sddc_observe_discard_locked(&ctx->sddc), 2U);
	KUNIT_EXPECT_EQ(test, ctx->sddc.pending, 0U);
	KUNIT_EXPECT_TRUE(test,
		bitmap_empty(ctx->sddc.observe_pending, CRYSTAL_SDDC_TEST_SLOTS));
	KUNIT_EXPECT_EQ(test,
		atomic64_read(&ctx->sddc.stats.shutdown_discarded), (s64)2);
}

static void crystal_sddc_index_quality_test(struct kunit *test)
{
	struct crystal_sddc_ref ref = { };

	refcount_set(&ref.refs, 3);
	KUNIT_EXPECT_EQ(test, crystal_sddc_ref_count_without_pin(&ref), 2U);
	refcount_set(&ref.refs, 1);
	KUNIT_EXPECT_EQ(test, crystal_sddc_ref_count_without_pin(&ref), 0U);

	/* Lower reference count and smaller payloads are the first eviction
	 * candidates; a highly shared reference survives an exact tie. */
	KUNIT_EXPECT_TRUE(test,
		crystal_sddc_index_candidate_worse(4, 512, 3, 1024));
	KUNIT_EXPECT_FALSE(test,
		crystal_sddc_index_candidate_worse(3, 512, 4, 256));
	KUNIT_EXPECT_TRUE(test,
		crystal_sddc_index_candidate_worse(3, 512, 3, 256));
	KUNIT_EXPECT_FALSE(test,
		crystal_sddc_index_candidate_worse(3, 512, 3, 1024));
	KUNIT_EXPECT_FALSE(test,
		crystal_sddc_index_candidate_worse(3, 512, 3, 512));
	KUNIT_EXPECT_TRUE(test,
		crystal_sddc_index_candidate_worse(2, 512, 2, 512));
}

static void crystal_sddc_similarity_test(struct kunit *test)
{
	u8 target[64];
	u8 shifted[72];
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(target); i++)
		target[i] = i;
	memset(shifted, 0xa5, sizeof(shifted));
	memcpy(shifted + 8, target, sizeof(target));

	KUNIT_EXPECT_EQ(test,
		crystal_sddc_match_bytes(target, sizeof(target), target,
			sizeof(target), CRYSTAL_SDDC_SAMPLE_HEAD),
		(u32)sizeof(target));
	KUNIT_EXPECT_EQ(test,
		crystal_sddc_match_bytes(target, sizeof(target), target,
			sizeof(target), CRYSTAL_SDDC_SAMPLE_TAIL),
		(u32)sizeof(target));
	KUNIT_EXPECT_EQ(test,
		crystal_sddc_match_bytes(target, sizeof(target), shifted,
			sizeof(shifted), CRYSTAL_SDDC_SAMPLE_TAIL),
		(u32)sizeof(target));
	KUNIT_EXPECT_EQ(test,
		crystal_sddc_match_bytes(target, sizeof(target), shifted,
			sizeof(shifted), CRYSTAL_SDDC_SAMPLE_HEAD |
			CRYSTAL_SDDC_SAMPLE_TAIL),
		(u32)sizeof(target));
	KUNIT_EXPECT_EQ(test,
		crystal_sddc_match_bytes(target, sizeof(target), shifted,
			sizeof(shifted), CRYSTAL_SDDC_SAMPLE_HEAD), 0U);
}

static void crystal_sddc_sample_eligibility_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test,
		crystal_sddc_sample_eligible(CRYSTAL_SDDC_INDEX_MIN_SIZE));
	KUNIT_EXPECT_TRUE(test,
		crystal_sddc_sample_eligible(CRYSTAL_SDDC_INDEX_MIN_SIZE + 1));
	KUNIT_EXPECT_TRUE(test, crystal_sddc_sample_eligible(PAGE_SIZE));
	KUNIT_EXPECT_FALSE(test, crystal_sddc_sample_eligible(PAGE_SIZE + 1));
}

static void crystal_sddc_delta_admission_test(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test,
		crystal_sddc_delta_target_eligible(CRYSTAL_SDDC_INDEX_MIN_SIZE + 1));
	KUNIT_EXPECT_FALSE(test,
		crystal_sddc_delta_target_eligible(CRYSTAL_SDDC_DELTA_MIN_SIZE));
	KUNIT_EXPECT_TRUE(test,
		crystal_sddc_delta_target_eligible(CRYSTAL_SDDC_DELTA_MIN_SIZE + 1));
}

static bool crystal_sddc_test_job_matches(struct crystal_sddc_test_ctx *ctx,
		const struct crystal_sddc_job_key *key)
{
	bool matches;

	crystal_sddc_slot_lock(&ctx->zram, key->index);
	matches = crystal_sddc_job_matches_locked(&ctx->sddc, key, false);
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
	int ret;

	ret = xa_reserve(&ctx->sddc.refs, ref.cookie.id, GFP_KERNEL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_NULL(test, xa_load(&ctx->sddc.refs, ref.cookie.id));
	KUNIT_ASSERT_FALSE(test, xa_empty(&ctx->sddc.refs));

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
		.handle = 0x1234,
		.size = CRYSTAL_SDDC_INDEX_MIN_SIZE,
		.prio = ZRAM_PRIMARY_COMP,
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

static void crystal_sddc_ref_reservation_pin_test(struct kunit *test)
{
	struct crystal_sddc_test_ctx *ctx = test->priv;
	struct crystal_sddc_cookie cookie = {
		.id = 29,
		.generation = 3,
	};
	int ret;

	ret = xa_reserve(&ctx->sddc.refs, cookie.id, GFP_KERNEL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_PTR_EQ(test,
		crystal_sddc_ref_pin_id(&ctx->sddc, cookie.id), NULL);
	KUNIT_ASSERT_PTR_EQ(test,
		crystal_sddc_ref_pin(&ctx->sddc, &cookie), NULL);
	xa_erase(&ctx->sddc.refs, cookie.id);
}

static void crystal_sddc_slot_state_ref_obj_pin_test(struct kunit *test)
{
	struct crystal_sddc_test_ctx *ctx = test->priv;
	struct crystal_sddc_ref ref = {
		.sddc = &ctx->sddc,
		.cookie = { .id = 30, .generation = 4 },
		.handle = 0x1234,
		.size = CRYSTAL_SDDC_INDEX_MIN_SIZE,
		.prio = ZRAM_PRIMARY_COMP,
	};
	struct crystal_sddc_slot_state state = {
		.ref = ref.cookie,
		.ref_obj = &ref,
		.ref_size = ref.size,
		.target_size = ref.size,
		.page_hash = CRYSTAL_SDDC_TEST_PAGE_HASH,
		.kind = CRYSTAL_SDDC_REF,
	};
	struct crystal_sddc_ref *pinned;

	refcount_set(&ref.refs, 2);
	KUNIT_ASSERT_TRUE(test, xa_empty(&ctx->sddc.refs));

	pinned = crystal_sddc_slot_state_pin_ref_locked(&ctx->sddc, &state,
			CRYSTAL_SDDC_REF);
	KUNIT_ASSERT_PTR_EQ(test, pinned, &ref);
	KUNIT_EXPECT_EQ(test, refcount_read(&ref.refs), 3U);
	crystal_sddc_ref_put(pinned);
	KUNIT_EXPECT_EQ(test, refcount_read(&ref.refs), 2U);

	state.ref.generation++;
	KUNIT_EXPECT_NULL(test,
		crystal_sddc_slot_state_pin_ref_locked(&ctx->sddc, &state,
			CRYSTAL_SDDC_REF));
	state.ref = ref.cookie;
	state.ref_size++;
	KUNIT_EXPECT_NULL(test,
		crystal_sddc_slot_state_pin_ref_locked(&ctx->sddc, &state,
			CRYSTAL_SDDC_REF));
	state.ref_size = ref.size;
	state.target_size--;
	KUNIT_EXPECT_NULL(test,
		crystal_sddc_slot_state_pin_ref_locked(&ctx->sddc, &state,
			CRYSTAL_SDDC_REF));
	state.target_size = ref.size;
	state.kind = CRYSTAL_SDDC_ALIAS;
	KUNIT_EXPECT_NULL(test,
		crystal_sddc_slot_state_pin_ref_locked(&ctx->sddc, &state,
			CRYSTAL_SDDC_REF));
	KUNIT_EXPECT_EQ(test, refcount_read(&ref.refs), 2U);
}

static void crystal_sddc_snapshot_identity_test(struct kunit *test)
{
	struct crystal_sddc_test_ctx *ctx = test->priv;
	struct crystal_sddc_slot_state *state;
	struct crystal_sddc_snapshot snapshot;

	ctx->mutation_seq[0] = 41;
	state = crystal_sddc_test_install_state(ctx, 0,
			CRYSTAL_SDDC_DELTA);
	KUNIT_ASSERT_NOT_NULL(test, state);
	state->ref.id = 9;
	state->ref.generation = 3;
	crystal_sddc_slot_lock(&ctx->zram, 0);
	crystal_sddc_snapshot_locked(&ctx->zram, 0, &snapshot);
	crystal_sddc_slot_unlock(&ctx->zram, 0);
	KUNIT_ASSERT_TRUE(test,
			crystal_sddc_test_snapshot_matches(ctx, 0, &snapshot));

	ctx->mutation_seq[0]++;
	KUNIT_EXPECT_FALSE(test,
			crystal_sddc_test_snapshot_matches(ctx, 0, &snapshot));
	ctx->mutation_seq[0] = snapshot.mutation_seq;
	state->kind = CRYSTAL_SDDC_ALIAS;
	KUNIT_EXPECT_FALSE(test,
			crystal_sddc_test_snapshot_matches(ctx, 0, &snapshot));
	state->kind = snapshot.kind;
	state->ref.generation++;
	KUNIT_EXPECT_FALSE(test,
			crystal_sddc_test_snapshot_matches(ctx, 0, &snapshot));
	state->ref = snapshot.ref;
	state->ref.id++;
	KUNIT_EXPECT_FALSE(test,
			crystal_sddc_test_snapshot_matches(ctx, 0, &snapshot));
	state->ref = snapshot.ref;
	state->page_hash++;
	KUNIT_EXPECT_TRUE(test,
			crystal_sddc_test_snapshot_matches(ctx, 0, &snapshot));
	state->page_hash = snapshot.page_hash;
	KUNIT_EXPECT_TRUE(test,
			crystal_sddc_test_snapshot_matches(ctx, 0, &snapshot));
}

static void crystal_sddc_slot_owner_test(struct kunit *test)
{
	struct crystal_sddc_test_ctx *ctx = test->priv;
	struct crystal_sddc_slot_state *state;
	struct crystal_sddc_ref ref = {
		.sddc = &ctx->sddc,
		.cookie = { .id = 31, .generation = 7 },
		.handle = 0x1234,
		.size = CRYSTAL_SDDC_INDEX_MIN_SIZE,
		.prio = ZRAM_PRIMARY_COMP,
	};
	enum crystal_sddc_kind kind;
	int ret;

	refcount_set(&ref.refs, 3);
	ret = xa_reserve(&ctx->sddc.refs, ref.cookie.id, GFP_KERNEL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_TRUE(test,
			crystal_sddc_ref_publish(&ctx->sddc, &ref));
	ctx->mutation_seq[0] = 12;
	state = crystal_sddc_test_install_state(ctx, 0,
			CRYSTAL_SDDC_DELTA);
	KUNIT_ASSERT_NOT_NULL(test, state);
	state->ref = ref.cookie;
	state->accounted_size = 80;
	state->saved_size = 64;
	state->ref_size = ref.size;
	state->target_size = CRYSTAL_SDDC_INDEX_MIN_SIZE;
	atomic64_set(&ctx->sddc.stats.deltas, 1);
	atomic64_set(&ctx->sddc.stats.delta_bytes, 80);
	atomic64_set(&ctx->sddc.stats.saved_bytes, 64);

	crystal_sddc_slot_lock(&ctx->zram, 0);
	kind = crystal_sddc_slot_free_locked(&ctx->zram, 0,
			CRYSTAL_SDDC_RELEASE_OTHER);
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
	kind = crystal_sddc_slot_free_locked(&ctx->zram, 0,
			CRYSTAL_SDDC_RELEASE_OTHER);
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

	ctx->mutation_seq[0] = key.mutation_seq;
	ctx->table[0].handle = key.handle;
	crystal_sddc_set_obj_size(&ctx->zram, 0, key.size);
	KUNIT_ASSERT_TRUE(test, crystal_sddc_test_job_matches(ctx, &key));

	ctx->mutation_seq[0]++;
	KUNIT_EXPECT_FALSE(test, crystal_sddc_test_job_matches(ctx, &key));
	ctx->mutation_seq[0] = key.mutation_seq;
	ctx->table[0].handle++;
	KUNIT_EXPECT_FALSE(test, crystal_sddc_test_job_matches(ctx, &key));
	ctx->table[0].handle = key.handle;
	KUNIT_ASSERT_NOT_NULL(test, crystal_sddc_test_install_state(ctx,
			0, CRYSTAL_SDDC_ALIAS));
	KUNIT_EXPECT_FALSE(test, crystal_sddc_test_job_matches(ctx, &key));
}

static void crystal_sddc_sparse_reservation_test(struct kunit *test)
{
	struct crystal_sddc_test_ctx *ctx = test->priv;
	struct crystal_sddc_slot_state *state;
	struct crystal_sddc_snapshot snapshot;
	u64 reservation_seq = 73;

	ctx->mutation_seq[0] = reservation_seq;
	KUNIT_ASSERT_TRUE(test, xa_empty(&ctx->sddc.slot_states));
	crystal_sddc_slot_lock(&ctx->zram, 0);
	crystal_sddc_snapshot_locked(&ctx->zram, 0, &snapshot);
	crystal_sddc_slot_unlock(&ctx->zram, 0);
	KUNIT_EXPECT_EQ(test, snapshot.mutation_seq, (u64)73);
	KUNIT_EXPECT_EQ(test, snapshot.kind, (u8)CRYSTAL_SDDC_NONE);

	state = crystal_sddc_state_prepare(&ctx->sddc, 0);
	KUNIT_ASSERT_NOT_NULL(test, state);
	KUNIT_EXPECT_FALSE(test, xa_empty(&ctx->sddc.slot_states));
	KUNIT_EXPECT_NULL(test, xa_load(&ctx->sddc.slot_states, 0));
	crystal_sddc_slot_lock(&ctx->zram, 0);
	KUNIT_EXPECT_EQ(test,
		crystal_sddc_slot_free_locked(&ctx->zram, 0,
			CRYSTAL_SDDC_RELEASE_OTHER),
		CRYSTAL_SDDC_NONE);
	crystal_sddc_slot_unlock(&ctx->zram, 0);
	/* A free must leave an in-flight reservation for its owner to abort;
	 * otherwise a later conversion could reuse it. */
	KUNIT_EXPECT_FALSE(test, xa_empty(&ctx->sddc.slot_states));
	KUNIT_EXPECT_TRUE(test, crystal_sddc_state_identity_changed(&ctx->sddc,
			0, reservation_seq));
	crystal_sddc_state_abort(&ctx->sddc, 0, reservation_seq, state);
	KUNIT_EXPECT_TRUE(test, xa_empty(&ctx->sddc.slot_states));
}

static void crystal_sddc_wb_sparse_state_test(struct kunit *test)
{
	struct crystal_sddc_test_ctx *ctx = test->priv;
	struct crystal_sddc_wb_state *state;
	struct crystal_sddc_snapshot snapshot;
	void *entry;
	int ret;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, state);
	state->mutation_seq = 17;
	state->ref.id = 5;
	state->ref.generation = 9;
	state->ref_size = 768;
	state->target_size = 1024;
	state->wire_size = 512;
	state->page_hash = CRYSTAL_SDDC_TEST_PAGE_HASH;
	state->kind = CRYSTAL_SDDC_DELTA;

	ret = xa_insert(&ctx->sddc.wb_states, 0, NULL, GFP_KERNEL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_PTR_EQ(test,
		crystal_sddc_wb_state_load_raw(&ctx->sddc, 0), XA_ZERO_ENTRY);
	crystal_sddc_slot_lock(&ctx->zram, 0);
	KUNIT_EXPECT_TRUE(test,
		crystal_sddc_wb_state_install_locked(&ctx->sddc, 0, state));
	crystal_sddc_snapshot_locked(&ctx->zram, 0, &snapshot);
	crystal_sddc_slot_unlock(&ctx->zram, 0);
	KUNIT_EXPECT_EQ(test, snapshot.mutation_seq, (u64)17);
	KUNIT_EXPECT_EQ(test, snapshot.kind, (u8)CRYSTAL_SDDC_DELTA);
	KUNIT_EXPECT_EQ(test, snapshot.ref_size, (u32)768);
	KUNIT_EXPECT_EQ(test, snapshot.target_size, (u32)1024);
	KUNIT_EXPECT_EQ(test, snapshot.page_hash,
			(u32)CRYSTAL_SDDC_TEST_PAGE_HASH);
	crystal_sddc_slot_lock(&ctx->zram, 0);
	KUNIT_EXPECT_TRUE(test,
		crystal_sddc_snapshot_matches_locked(&ctx->zram, 0, &snapshot));
	crystal_sddc_slot_unlock(&ctx->zram, 0);

	entry = crystal_sddc_wb_state_erase_locked(&ctx->sddc, 0);
	KUNIT_EXPECT_PTR_EQ(test, entry, state);
	KUNIT_EXPECT_TRUE(test, xa_empty(&ctx->sddc.wb_states));
	kfree(state);
}

static void crystal_sddc_wb_abort_reservation_test(struct kunit *test)
{
	struct crystal_sddc_test_ctx *ctx = test->priv;
	struct crystal_sddc_wb_state *state;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, state);
	KUNIT_ASSERT_EQ(test,
		xa_insert(&ctx->sddc.wb_states, 1, NULL, GFP_KERNEL), 0);
	crystal_sddc_native_wb_capture_abort_internal(&ctx->sddc, 1, state);
	KUNIT_EXPECT_TRUE(test, xa_empty(&ctx->sddc.wb_states));
}

static void crystal_sddc_reservation_job_test(struct kunit *test)
{
	struct crystal_sddc_test_ctx *ctx = test->priv;
	struct crystal_sddc_slot_state *state;
	struct crystal_sddc_job_key key = {
		.index = 0,
		.mutation_seq = 91,
		.handle = 0x4321,
		.size = 512,
		.prio = ZRAM_PRIMARY_COMP,
	};

	ctx->mutation_seq[0] = key.mutation_seq;
	ctx->table[0].handle = key.handle;
	crystal_sddc_set_obj_size(&ctx->zram, 0, key.size);
	state = crystal_sddc_state_prepare(&ctx->sddc, key.index);
	KUNIT_ASSERT_NOT_NULL(test, state);

	crystal_sddc_slot_lock(&ctx->zram, key.index);
	KUNIT_EXPECT_FALSE(test,
			crystal_sddc_job_matches_locked(&ctx->sddc, &key, false));
	KUNIT_EXPECT_TRUE(test,
			crystal_sddc_job_matches_locked(&ctx->sddc, &key, true));
	crystal_sddc_slot_unlock(&ctx->zram, key.index);
	KUNIT_EXPECT_FALSE(test, crystal_sddc_state_identity_changed(&ctx->sddc,
			key.index, key.mutation_seq));

	crystal_sddc_state_abort(&ctx->sddc, key.index, key.mutation_seq, state);
}

static void crystal_sddc_mutation_wrap_test(struct kunit *test)
{
	struct crystal_sddc_test_ctx *ctx = test->priv;

	ctx->mutation_seq[0] = U64_MAX;
	crystal_sddc_mutation_advance_locked(&ctx->sddc, 0);
	KUNIT_EXPECT_EQ(test, ctx->mutation_seq[0], (u64)1);
	crystal_sddc_mutation_advance_locked(&ctx->sddc, 0);
	KUNIT_EXPECT_EQ(test, ctx->mutation_seq[0], (u64)2);
}

static void crystal_sddc_flatten_snapshot_test(struct kunit *test)
{
	struct crystal_sddc_test_ctx *ctx = test->priv;
	struct crystal_sddc_slot_state *state;
	struct crystal_sddc_snapshot snapshot;
	void *dst;
	size_t size = 123;
	int ret;

	ctx->mutation_seq[0] = 27;
	state = crystal_sddc_test_install_state(ctx, 0,
			CRYSTAL_SDDC_ALIAS);
	KUNIT_ASSERT_NOT_NULL(test, state);
	state->ref.id = 2;
	state->ref.generation = 4;
	crystal_sddc_slot_lock(&ctx->zram, 0);
	crystal_sddc_snapshot_locked(&ctx->zram, 0, &snapshot);
	crystal_sddc_slot_unlock(&ctx->zram, 0);
	ctx->mutation_seq[0]++;
	dst = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dst);

	ret = crystal_sddc_flatten(&ctx->zram, 0, &snapshot, dst, &size);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	KUNIT_EXPECT_EQ(test, size, (size_t)123);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->sddc.active_ops), 0);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&ctx->sddc.stats.flatten_failures), (s64)0);

	crystal_sddc_slot_lock(&ctx->zram, 0);
	crystal_sddc_snapshot_locked(&ctx->zram, 0, &snapshot);
	crystal_sddc_slot_unlock(&ctx->zram, 0);
	ret = crystal_sddc_flatten(&ctx->zram, 0, &snapshot, dst, &size);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->sddc.active_ops), 0);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&ctx->sddc.stats.decode_failures), (s64)1);
	KUNIT_EXPECT_EQ(test,
			atomic64_read(&ctx->sddc.stats.flatten_failures), (s64)1);
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
	struct crystal_sddc_delta_header alias = {
		.magic = cpu_to_le32(CRYSTAL_SDDC_ALIAS_MAGIC),
		.version = cpu_to_le16(CRYSTAL_SDDC_DELTA_VERSION),
		.header_size = cpu_to_le16(sizeof(alias)),
		.ref_id = cpu_to_le32(ref.cookie.id),
		.ref_generation = cpu_to_le32(ref.cookie.generation),
		.ref_size = cpu_to_le32(ref.size),
		.target_size = cpu_to_le32(ref.size),
	};
	struct crystal_sddc_alias_header_v2 alias_v2 = {
		.magic = cpu_to_le32(CRYSTAL_SDDC_ALIAS_V2_MAGIC),
		.ref_id = cpu_to_le32(ref.cookie.id),
		.ref_generation = cpu_to_le32(ref.cookie.generation),
		.packed_sizes = crystal_sddc_alias_v2_pack_sizes(ref.size,
				ref.size),
	};
	struct crystal_sddc_alias_header_v2 bad_alias_v2;
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
	KUNIT_EXPECT_TRUE(test, crystal_sddc_native_wb_header_valid(&ref,
			&alias, sizeof(alias), CRYSTAL_SDDC_ALIAS));
	KUNIT_EXPECT_EQ(test, crystal_sddc_native_wb_target_size(&alias,
			sizeof(alias), CRYSTAL_SDDC_ALIAS), ref.size);
	bad = alias;
	bad.magic = cpu_to_le32(CRYSTAL_SDDC_DELTA_MAGIC);
	KUNIT_EXPECT_FALSE(test, crystal_sddc_native_wb_header_valid(&ref,
			&bad, sizeof(bad), CRYSTAL_SDDC_ALIAS));
	bad = alias;
	bad.target_size = cpu_to_le32(ref.size - 1);
	KUNIT_EXPECT_FALSE(test, crystal_sddc_native_wb_header_valid(&ref,
			&bad, sizeof(bad), CRYSTAL_SDDC_ALIAS));
	KUNIT_EXPECT_FALSE(test, crystal_sddc_native_wb_header_valid(&ref,
			&alias, sizeof(alias) + 1, CRYSTAL_SDDC_ALIAS));

	KUNIT_ASSERT_TRUE(test, crystal_sddc_native_wb_header_valid(&ref,
			&alias_v2, sizeof(alias_v2), CRYSTAL_SDDC_ALIAS));
	KUNIT_EXPECT_EQ(test, crystal_sddc_native_wb_target_size(&alias_v2,
			sizeof(alias_v2), CRYSTAL_SDDC_ALIAS), ref.size);
	bad_alias_v2 = alias_v2;
	bad_alias_v2.magic = cpu_to_le32(CRYSTAL_SDDC_ALIAS_V2_MAGIC ^ 1);
	KUNIT_EXPECT_FALSE(test, crystal_sddc_native_wb_header_valid(&ref,
			&bad_alias_v2, sizeof(bad_alias_v2),
			CRYSTAL_SDDC_ALIAS));
	bad_alias_v2 = alias_v2;
	bad_alias_v2.ref_generation = cpu_to_le32(ref.cookie.generation + 1);
	KUNIT_EXPECT_FALSE(test, crystal_sddc_native_wb_header_valid(&ref,
			&bad_alias_v2, sizeof(bad_alias_v2),
			CRYSTAL_SDDC_ALIAS));
	bad_alias_v2 = alias_v2;
	bad_alias_v2.packed_sizes = crystal_sddc_alias_v2_pack_sizes(ref.size,
			ref.size - 1);
	KUNIT_EXPECT_FALSE(test, crystal_sddc_native_wb_header_valid(&ref,
			&bad_alias_v2, sizeof(bad_alias_v2),
			CRYSTAL_SDDC_ALIAS));
	bad_alias_v2 = alias_v2;
	bad_alias_v2.packed_sizes = cpu_to_le32(
		le32_to_cpu(alias_v2.packed_sizes) +
		(1U << CRYSTAL_SDDC_ALIAS_VERSION_SHIFT));
	KUNIT_EXPECT_FALSE(test, crystal_sddc_native_wb_header_valid(&ref,
			&bad_alias_v2, sizeof(bad_alias_v2),
			CRYSTAL_SDDC_ALIAS));
	KUNIT_EXPECT_FALSE(test, crystal_sddc_native_wb_header_valid(&ref,
			&alias_v2, sizeof(alias_v2) + 1,
			CRYSTAL_SDDC_ALIAS));
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
	KUNIT_CASE(crystal_sddc_ref_reservation_pin_test),
	KUNIT_CASE(crystal_sddc_slot_state_ref_obj_pin_test),
	KUNIT_CASE(crystal_sddc_snapshot_identity_test),
	KUNIT_CASE(crystal_sddc_slot_owner_test),
	KUNIT_CASE(crystal_sddc_stale_job_key_test),
	KUNIT_CASE(crystal_sddc_sparse_reservation_test),
	KUNIT_CASE(crystal_sddc_wb_sparse_state_test),
	KUNIT_CASE(crystal_sddc_wb_abort_reservation_test),
	KUNIT_CASE(crystal_sddc_reservation_job_test),
	KUNIT_CASE(crystal_sddc_mutation_wrap_test),
	KUNIT_CASE(crystal_sddc_flatten_snapshot_test),
	KUNIT_CASE(crystal_sddc_delta_header_test),
	KUNIT_CASE(crystal_sddc_manager_admission_test),
	KUNIT_CASE(crystal_sddc_observe_bitmap_test),
	KUNIT_CASE(crystal_sddc_observe_discard_test),
	KUNIT_CASE(crystal_sddc_index_quality_test),
	KUNIT_CASE(crystal_sddc_similarity_test),
	KUNIT_CASE(crystal_sddc_sample_eligibility_test),
	KUNIT_CASE(crystal_sddc_delta_admission_test),
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
