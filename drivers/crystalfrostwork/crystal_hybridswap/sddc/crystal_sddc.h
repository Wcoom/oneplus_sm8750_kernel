/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _CRYSTAL_SDDC_H_
#define _CRYSTAL_SDDC_H_

#include <linux/kconfig.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>

struct crystal_sddc;
struct page;
struct zram;

enum crystal_sddc_kind {
	CRYSTAL_SDDC_NONE,
	CRYSTAL_SDDC_REF,
	CRYSTAL_SDDC_ALIAS,
	CRYSTAL_SDDC_DELTA,
};

enum crystal_sddc_release_reason {
	CRYSTAL_SDDC_RELEASE_OTHER,
	CRYSTAL_SDDC_RELEASE_WRITEBACK,
	CRYSTAL_SDDC_RELEASE_REWRITE,
	CRYSTAL_SDDC_RELEASE_RECOMPRESS,
	CRYSTAL_SDDC_RELEASE_DISCARD,
	CRYSTAL_SDDC_RELEASE_NOTIFY_FREE,
	CRYSTAL_SDDC_RELEASE_RESET,
};

struct crystal_sddc_cookie {
	u32 id;
	u32 generation;
};

struct crystal_sddc_snapshot {
	u64 mutation_seq;
	struct crystal_sddc_cookie ref;
	u32 ref_size;
	u32 target_size;
	u32 page_hash;
	u8 kind;
};

/*
 * A native ZMS writeback capture owns a temporary SDDC manager operation
 * until it is either committed or aborted. The private pointers are kept
 * opaque so the zram layer cannot manipulate resident reference ownership.
 */
struct crystal_sddc_wb_capture {
	void *private;
	void *manager;
	u32 index;
	struct crystal_sddc_cookie ref;
	u32 ref_size;
	u32 target_size;
	u32 wire_size;
	u32 page_hash;
	u8 kind;
};

/*
 * A native ZMS writeback read pins the resident reference while zram drops
 * the slot lock for backend IO.  The fields stay opaque to zram except for
 * zero-initialisation and hand-off back into crystal_sddc.
 */
struct crystal_sddc_wb_ref {
	void *private;
	void *manager;
	struct crystal_sddc_cookie ref;
	u32 ref_size;
	u32 target_size;
	u32 wire_size;
	u32 page_hash;
	u8 kind;
};

struct crystal_sddc_job_key {
	u64 mutation_seq;
	unsigned long handle;
	u32 index;
	u32 size;
	u8 prio;
};

struct crystal_sddc_stats_snapshot {
	u64 queued;
	u64 coalesced;
	u64 dropped;
	u64 ineligible;
	u64 shutdown_discarded;
	u64 worker_runs;
	u64 observed;
	u64 stale;
	u64 indexed;
	u64 refs;
	u64 ref_bytes;
	u64 wb_ref_pins;
	u64 wb_ref_pin_bytes;
	u64 wb_ref_pin_max;
	u64 wb_ref_pin_bytes_max;
	u64 wb_ref_pin_events;
	u64 wb_ref_unpin_events;
	u64 wb_deltas;
	u64 wb_delta_bytes;
	u64 wb_aliases;
	u64 wb_alias_bytes;
	u64 aliases;
	u64 deltas;
	u64 delta_bytes;
	u64 alias_attempts;
	u64 alias_hits;
	u64 delta_attempts;
	u64 delta_hits;
	u64 delta_matches;
	u64 delta_small_rejects;
	u64 delta_no_gain;
	u64 delta_match_bytes_max;
	u64 saved_bytes;
	u64 saved_bytes_total;
	u64 released_aliases;
	u64 released_deltas;
	u64 released_saved_bytes;
	u64 released_writeback_saved_bytes;
	u64 released_rewrite_saved_bytes;
	u64 released_recompress_saved_bytes;
	u64 released_discard_saved_bytes;
	u64 released_notify_free_saved_bytes;
	u64 released_reset_saved_bytes;
	u64 released_other_saved_bytes;
	u64 conversion_failures;
	u64 decode_failures;
	u64 flatten_failures;
	u64 integrity_checks;
	u64 integrity_failures;
	u64 integrity_skipped;
	u64 integrity_hash_failures;
	u64 limit_rejects;
	u64 pending_max;
	u32 pending;
	bool enabled;
};

#if IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC)
int crystal_sddc_create(struct zram *zram, unsigned long nr_pages);
void crystal_sddc_stop(struct zram *zram);
void crystal_sddc_destroy(struct zram *zram);

enum crystal_sddc_kind crystal_sddc_slot_free_locked(struct zram *zram,
		u32 index, enum crystal_sddc_release_reason reason);
void crystal_sddc_slot_stored_locked(struct zram *zram, u32 index);
void crystal_sddc_job_key_locked(struct zram *zram, u32 index,
		struct crystal_sddc_job_key *key);
void crystal_sddc_queue_observation(struct zram *zram,
		const struct crystal_sddc_job_key *key);
void crystal_sddc_requeue_observation(struct zram *zram, u32 index);
bool crystal_sddc_slot_allocated_locked(struct zram *zram, u32 index);
bool crystal_sddc_accounted_size_locked(struct zram *zram, u32 index,
		size_t *size);
void crystal_sddc_snapshot_locked(struct zram *zram, u32 index,
		struct crystal_sddc_snapshot *snapshot);
bool crystal_sddc_snapshot_matches_locked(struct zram *zram, u32 index,
		const struct crystal_sddc_snapshot *snapshot);
int crystal_sddc_read_page(struct zram *zram, struct page *page, u32 index);
/* Called with the zram slot locked; always drops that lock before return. */
int crystal_sddc_read_page_locked(struct zram *zram, struct page *page,
		u32 index);
int crystal_sddc_flatten(struct zram *zram, u32 index,
		const struct crystal_sddc_snapshot *snapshot, void *dst,
		size_t *size);
bool crystal_sddc_native_wb_kind(u8 kind);
int crystal_sddc_native_wb_capture(struct zram *zram, u32 index,
		const struct crystal_sddc_snapshot *snapshot, void *dst,
		size_t *size, struct crystal_sddc_wb_capture *capture);
bool crystal_sddc_native_wb_install_locked(struct zram *zram, u32 index,
		const struct crystal_sddc_snapshot *snapshot,
		struct crystal_sddc_wb_capture *capture);
void crystal_sddc_native_wb_finalize_locked(struct zram *zram, u32 index);
void crystal_sddc_native_wb_release(struct crystal_sddc_wb_capture *capture);
void crystal_sddc_native_wb_free_locked(struct zram *zram, u32 index);
bool crystal_sddc_native_wb_pin_locked(struct zram *zram, u32 index,
		const struct crystal_sddc_snapshot *snapshot,
		struct crystal_sddc_wb_ref *wb_ref);
void crystal_sddc_native_wb_put_ref(struct crystal_sddc_wb_ref *wb_ref);
int crystal_sddc_native_wb_restore_page(struct zram *zram, struct page *page,
		u32 index, const struct crystal_sddc_snapshot *snapshot,
		const void *wire, size_t wire_size);
int crystal_sddc_native_wb_restore_pinned(struct zram *zram, struct page *page,
		u32 index, const struct crystal_sddc_snapshot *snapshot,
		struct crystal_sddc_wb_ref *wb_ref,
		const void *wire, size_t wire_size);
void crystal_sddc_zram_account_sub_locked(struct zram *zram, u32 index);
void crystal_sddc_zram_account_add_locked(struct zram *zram, u32 index);
void crystal_sddc_zram_ref_account(struct zram *zram, u64 memcg_id,
		size_t size, bool add);
bool crystal_sddc_zram_memory_limit_ok(struct zram *zram);
void crystal_sddc_get_stats(struct zram *zram,
		struct crystal_sddc_stats_snapshot *stats);
bool crystal_sddc_debug_snapshot(struct zram *zram,
		struct crystal_sddc_stats_snapshot *stats);
#else
static inline int crystal_sddc_create(struct zram *zram,
		unsigned long nr_pages)
{
	return 0;
}

static inline void crystal_sddc_stop(struct zram *zram) { }
static inline void crystal_sddc_destroy(struct zram *zram) { }
static inline enum crystal_sddc_kind
crystal_sddc_slot_free_locked(struct zram *zram, u32 index,
		enum crystal_sddc_release_reason reason)
{
	return CRYSTAL_SDDC_NONE;
}
static inline void crystal_sddc_slot_stored_locked(struct zram *zram,
		u32 index) { }
static inline void crystal_sddc_job_key_locked(struct zram *zram, u32 index,
		struct crystal_sddc_job_key *key) { }
static inline void crystal_sddc_queue_observation(struct zram *zram,
		const struct crystal_sddc_job_key *key) { }
static inline void crystal_sddc_requeue_observation(struct zram *zram,
		u32 index) { }
static inline bool crystal_sddc_slot_allocated_locked(struct zram *zram,
		u32 index)
{
	return false;
}

static inline bool crystal_sddc_accounted_size_locked(struct zram *zram,
		u32 index, size_t *size)
{
	return false;
}

static inline void crystal_sddc_snapshot_locked(struct zram *zram, u32 index,
		struct crystal_sddc_snapshot *snapshot)
{
	memset(snapshot, 0, sizeof(*snapshot));
}

static inline bool crystal_sddc_snapshot_matches_locked(struct zram *zram,
		u32 index, const struct crystal_sddc_snapshot *snapshot)
{
	return true;
}

static inline int crystal_sddc_read_page(struct zram *zram,
		struct page *page, u32 index)
{
	return -EAGAIN;
}

static inline int crystal_sddc_read_page_locked(struct zram *zram,
		struct page *page, u32 index)
{
	return -EAGAIN;
}

static inline int crystal_sddc_flatten(struct zram *zram, u32 index,
		const struct crystal_sddc_snapshot *snapshot, void *dst,
		size_t *size)
{
	return -EOPNOTSUPP;
}

static inline bool crystal_sddc_native_wb_kind(u8 kind)
{
	return false;
}

static inline int crystal_sddc_native_wb_capture(struct zram *zram, u32 index,
		const struct crystal_sddc_snapshot *snapshot, void *dst,
		size_t *size, struct crystal_sddc_wb_capture *capture)
{
	if (size)
		*size = 0;
	if (capture)
		memset(capture, 0, sizeof(*capture));
	return -EOPNOTSUPP;
}

static inline bool crystal_sddc_native_wb_install_locked(struct zram *zram,
		u32 index, const struct crystal_sddc_snapshot *snapshot,
		struct crystal_sddc_wb_capture *capture)
{
	return false;
}

static inline void crystal_sddc_native_wb_finalize_locked(struct zram *zram,
		u32 index) { }

static inline void crystal_sddc_native_wb_release(
		struct crystal_sddc_wb_capture *capture) { }

static inline void crystal_sddc_native_wb_free_locked(struct zram *zram,
		u32 index) { }

static inline bool crystal_sddc_native_wb_pin_locked(struct zram *zram,
		u32 index, const struct crystal_sddc_snapshot *snapshot,
		struct crystal_sddc_wb_ref *wb_ref)
{
	if (wb_ref)
		memset(wb_ref, 0, sizeof(*wb_ref));
	return false;
}

static inline void crystal_sddc_native_wb_put_ref(
		struct crystal_sddc_wb_ref *wb_ref)
{
	if (wb_ref)
		memset(wb_ref, 0, sizeof(*wb_ref));
}

static inline int crystal_sddc_native_wb_restore_page(struct zram *zram,
		struct page *page, u32 index,
		const struct crystal_sddc_snapshot *snapshot,
		const void *wire, size_t wire_size)
{
	return -EOPNOTSUPP;
}

static inline int crystal_sddc_native_wb_restore_pinned(struct zram *zram,
		struct page *page, u32 index,
		const struct crystal_sddc_snapshot *snapshot,
		struct crystal_sddc_wb_ref *wb_ref,
		const void *wire, size_t wire_size)
{
	return -EOPNOTSUPP;
}

static inline void crystal_sddc_get_stats(struct zram *zram,
		struct crystal_sddc_stats_snapshot *stats)
{
	memset(stats, 0, sizeof(*stats));
}

static inline bool crystal_sddc_debug_snapshot(struct zram *zram,
		struct crystal_sddc_stats_snapshot *stats)
{
	memset(stats, 0, sizeof(*stats));
	return false;
}
#endif

#endif /* _CRYSTAL_SDDC_H_ */
