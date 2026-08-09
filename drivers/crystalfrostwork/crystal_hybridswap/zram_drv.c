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
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/cgroup.h>
#include <linux/memcontrol.h>
#include <linux/rcupdate.h>
#include <linux/sort.h>
#include <linux/ktime.h>
#include <linux/bio.h>
#include <linux/bitops.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/device.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/backing-dev.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/err.h>
#include <linux/wait_bit.h>
#include <linux/idr.h>
#include <linux/sysfs.h>
#include <linux/debugfs.h>
#include <linux/cpuhotplug.h>
#include <linux/part_stat.h>
#include <linux/sysms_finder.h>

#include "zram_drv.h"
#include "crystal_hybridswap_internal.h"
#include "sddc/crystal_sddc.h"

static DEFINE_IDR(zram_index_idr);
/* idr index must be protected */
static DEFINE_MUTEX(zram_index_mutex);

static int zram_major;
enum cpuhp_state zcomp_cpuhp_state = CPUHP_INVALID;
static const char *default_compressor = CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_DEF_COMP;
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
static struct workqueue_struct *zram_read_wq;
#endif

/* Module params (documentation at end) */
static unsigned int num_devices = 1;
/*
 * Pages that compress to sizes equals or greater than this are stored
 * uncompressed in memory.
 */
static size_t huge_class_size;

static const struct block_device_operations zram_devops;

static void zram_free_page(struct zram *zram, size_t index,
			   enum crystal_sddc_release_reason reason);
static int zram_read_page(struct zram *zram, struct page *page, u32 index,
			  struct bio *parent);
static int zram_read_compressed_page(struct zram *zram, u32 index,
				     void *dst, size_t *size);
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
#define ZRAM_BATCHIN_GROUP_MAX	32U
#define ZRAM_ZMS_PREFETCH_MAX	2U
#define ZRAM_PREFETCH_NEIGHBOR_SCAN_MAX	(ZRAM_ZMS_PREFETCH_MAX * 2U)
#define ZRAM_PREFETCH_DEFAULT_DEGREE	1U
#define ZRAM_PREFETCH_DEFAULT_TTL_MS	1000UL
#define ZRAM_PREFETCH_TTL_MIN_MS	100UL
#define ZRAM_PREFETCH_TTL_MAX_MS	5000UL

static unsigned int zram_read_wq_max_active(void)
{
	return min_t(unsigned int, max_t(unsigned int, 2 * num_online_cpus(), 1),
		     32);
}

static void zram_record_zms_io(struct zram *zram, u32 index,
			       const struct zms_io *io);
static bool zram_test_flag(struct zram *zram, u32 index,
			   enum zram_pageflags flag);
static void zram_set_flag(struct zram *zram, u32 index,
			  enum zram_pageflags flag);
static void zram_clear_flag(struct zram *zram, u32 index,
			    enum zram_pageflags flag);
struct zram_wb_snapshot;
static bool zram_writeback_snapshot_matches(struct zram *zram, u32 index,
						    const struct zram_wb_snapshot *snapshot);
static bool zram_wb_snapshot_matches(struct zram *zram, u32 index,
						 const struct zram_wb_snapshot *snapshot);
static void zram_zms_start_gc(struct zram *zram);
static void zram_zms_stop_gc(struct zram *zram);
static void zram_zms_maybe_prefetch(struct zram *zram, u32 index,
				    const struct zms_load_ref *source,
				    u64 memcg_id,
				    const struct zms_io *io);
#endif

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
struct zram_prefetch_candidate {
	struct zms_object_cookie cookie;
	u32 index;
	unsigned int score;
};

static void zram_prefetch_note_fault(struct zram *zram, u32 index, u64 memcg_id)
{
	u32 last_index;

	if (!zram || !memcg_id)
		return;

	last_index = READ_ONCE(zram->prefetch_last_fault_index);
	WRITE_ONCE(zram->prefetch_prev_fault_index, last_index);
	WRITE_ONCE(zram->prefetch_last_fault_index, index);
	WRITE_ONCE(zram->prefetch_last_fault_memcg_id, memcg_id);
	WRITE_ONCE(zram->prefetch_fault_valid, true);
}

static void zram_set_prefetched(struct zram *zram, u32 index)
{
	zram->table[index].prefetch_jiffies = jiffies;
	zram_set_flag(zram, index, ZRAM_PREFETCHED);
}

static bool zram_prefetch_expired(struct zram *zram, u32 index)
{
	unsigned long start = zram->table[index].prefetch_jiffies;

	return !start || time_after(jiffies,
				    start + msecs_to_jiffies(
					    READ_ONCE(zram->prefetch_ttl_ms)));
}

static bool zram_consume_prefetched(struct zram *zram, u32 index)
{
	bool expired;

	if (!zram_test_flag(zram, index, ZRAM_PREFETCHED))
		return false;

	expired = zram_prefetch_expired(zram, index);
	zram_clear_flag(zram, index, ZRAM_PREFETCHED);
	zram->table[index].prefetch_jiffies = 0;
	if (expired)
		atomic64_inc(&zram->stats.prefetch_stale_hits);
	else
		atomic64_inc(&zram->stats.prefetch_hits);

	return !expired;
}

static unsigned int zram_prefetch_distance_score(u32 cur_index, u32 neighbor_index)
{
	u32 dist = abs_diff(cur_index, neighbor_index);

	if (dist == 1)
		return 64;
	if (dist <= 4)
		return 32;
	if (dist <= 16)
		return 16;

	return 0;
}

static unsigned int zram_prefetch_direction_score(struct zram *zram, u32 cur_index,
						  u32 neighbor_index, u64 memcg_id)
{
	u64 last_memcg_id;
	u32 last_index;
	u32 prev_index;
	s64 dir;
	s64 neighbor_dir;
	u64 neighbor_dist;

	if (!READ_ONCE(zram->prefetch_fault_valid))
		return 0;

	last_memcg_id = READ_ONCE(zram->prefetch_last_fault_memcg_id);
	if (!memcg_id || last_memcg_id != memcg_id)
		return 0;

	last_index = READ_ONCE(zram->prefetch_last_fault_index);
	prev_index = READ_ONCE(zram->prefetch_prev_fault_index);

	if (last_index == cur_index)
		dir = (s64)cur_index - (s64)prev_index;
	else
		dir = (s64)cur_index - (s64)last_index;

	if (!dir)
		return 0;

	neighbor_dir = (s64)neighbor_index - (s64)cur_index;
	if (!neighbor_dir)
		return 0;
	if ((dir > 0 && neighbor_dir < 0) || (dir < 0 && neighbor_dir > 0))
		return 0;
	neighbor_dist = neighbor_dir < 0 ? -neighbor_dir : neighbor_dir;
	if (neighbor_dist > 16)
		return 0;
	if (neighbor_dist == 1)
		return 32;

	return 16;
}

static unsigned int zram_prefetch_score_neighbor(struct zram *zram, u32 cur_index,
						 u32 neighbor_index, u64 memcg_id)
{
	return zram_prefetch_distance_score(cur_index, neighbor_index) +
	       zram_prefetch_direction_score(zram, cur_index, neighbor_index,
					 memcg_id);
}

static bool zram_prefetch_insert_candidate(struct zram_prefetch_candidate *candidates,
					   u32 cur_index,
					   unsigned int *count,
					   const struct zram_prefetch_candidate *cand)
{
	unsigned int nr = *count;

	if (!cand->cookie.handle || !cand->score)
		return false;
	if (nr >= ZRAM_PREFETCH_NEIGHBOR_SCAN_MAX)
		return false;

	while (nr > 0) {
		struct zram_prefetch_candidate *prev = &candidates[nr - 1];

		if (prev->score > cand->score)
			break;
		if (prev->score == cand->score &&
		    abs_diff(prev->index, cur_index) <=
		    abs_diff(cand->index, cur_index))
			break;
		candidates[nr] = *prev;
		nr--;
	}

	candidates[nr] = *cand;
	*count += 1;
	return true;
}
#endif

bool zram_try_get(struct zram *zram)
{
	bool got = false;

	if (!zram)
		return false;

	spin_lock(&zram->ref_lock);
	if (!zram->removing) {
		refcount_inc(&zram->refcount);
		got = true;
	}
	spin_unlock(&zram->ref_lock);

	return got;
}

void zram_put(struct zram *zram)
{
	if (zram && refcount_dec_and_test(&zram->refcount))
		complete(&zram->ref_completion);
}

static void zram_begin_remove(struct zram *zram)
{
	spin_lock(&zram->ref_lock);
	zram->removing = true;
	spin_unlock(&zram->ref_lock);
}

static void zram_wait_for_refs(struct zram *zram)
{
	zram_put(zram);
	wait_for_completion(&zram->ref_completion);
}

static unsigned long zram_pages_snapshot(struct zram *zram)
{
	return zram->disksize >> PAGE_SHIFT;
}

static bool zram_valid_io_range(struct zram *zram, unsigned long index,
		unsigned long nr_pages)
{
	unsigned long total_pages = zram_pages_snapshot(zram);

	if (index > total_pages)
		return false;
	if (nr_pages > total_pages - index)
		return false;
	return true;
}

static int zram_slot_trylock(struct zram *zram, u32 index)
{
	return bit_spin_trylock(ZRAM_LOCK, &zram->table[index].flags);
}

static void zram_slot_lock(struct zram *zram, u32 index)
{
	bit_spin_lock(ZRAM_LOCK, &zram->table[index].flags);
}

static void zram_slot_unlock(struct zram *zram, u32 index)
{
	bit_spin_unlock(ZRAM_LOCK, &zram->table[index].flags);
}

static inline bool init_done(struct zram *zram)
{
	return zram->disksize;
}

static inline struct zram *dev_to_zram(struct device *dev)
{
	return (struct zram *)dev_to_disk(dev)->private_data;
}

static struct zram *zram_get_from_dev(struct device *dev)
{
	struct zram *zram;

	if (!dev)
		return NULL;

	zram = dev_to_zram(dev);
	return zram_try_get(zram) ? zram : NULL;
}

static unsigned long zram_get_handle(struct zram *zram, u32 index)
{
	return zram->table[index].handle;
}

static void zram_set_handle(struct zram *zram, u32 index, unsigned long handle)
{
	zram->table[index].handle = handle;
}

/* flag operations require table entry bit_spin_lock() being held */
static bool zram_test_flag(struct zram *zram, u32 index,
			enum zram_pageflags flag)
{
	return zram->table[index].flags & BIT(flag);
}

static void zram_set_flag(struct zram *zram, u32 index,
			enum zram_pageflags flag)
{
	zram->table[index].flags |= BIT(flag);
}

static void zram_clear_flag(struct zram *zram, u32 index,
			enum zram_pageflags flag)
{
	zram->table[index].flags &= ~BIT(flag);
}

static void zram_clear_flag_wake(struct zram *zram, u32 index,
				 enum zram_pageflags flag)
{
	zram_clear_flag(zram, index, flag);
	wake_up_bit(&zram->table[index].flags, flag);
}

static void zram_clear_prefetched(struct zram *zram, u32 index)
{
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
	if (zram_test_flag(zram, index, ZRAM_PREFETCHED)) {
		zram_clear_flag(zram, index, ZRAM_PREFETCHED);
		zram->table[index].prefetch_jiffies = 0;
		atomic64_inc(&zram->stats.prefetch_invalidated);
	}
#endif
}

static inline void zram_set_element(struct zram *zram, u32 index,
			unsigned long element)
{
	zram->table[index].element = element;
}

static unsigned long zram_get_element(struct zram *zram, u32 index)
{
	return zram->table[index].element;
}

static u64 zram_page_memcg_id(struct page *page)
{
#ifdef CONFIG_MEMCG
	struct mem_cgroup *memcg;
	u64 id = 0;

	if (!page)
		return 0;

	rcu_read_lock();
	memcg = page_memcg_check(page);
	if (memcg && memcg->css.cgroup)
		id = cgroup_id(memcg->css.cgroup);
	rcu_read_unlock();

	return id;
#else
	return 0;
#endif
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
			zram_test_flag(zram, index, ZRAM_WB) ||
			crystal_sddc_slot_allocated_locked(zram, index);
}

struct zram_wb_snapshot {
	struct crystal_sddc_snapshot sddc;
	unsigned long element;
	unsigned long flags;
	size_t size;
	u64 memcg_id;
};

static unsigned long zram_snapshot_flags(struct zram *zram, u32 index)
{
	return zram->table[index].flags &
	       ~(1UL << ZRAM_LOCK | 1UL << ZRAM_PREFETCHED);
}

static void zram_take_wb_snapshot(struct zram *zram, u32 index,
		struct zram_wb_snapshot *snapshot)
{
	snapshot->element = zram_get_element(zram, index);
	snapshot->flags = zram_snapshot_flags(zram, index);
	snapshot->size = zram_get_obj_size(zram, index);
	snapshot->memcg_id = zram->table[index].memcg_id;
	crystal_sddc_snapshot_locked(zram, index, &snapshot->sddc);
}

struct zram_memcg_account {
	u64 cgroup_id;
	bool allocated;
	bool wb;
	bool same;
	bool huge;
	bool sddc;
	size_t size;
};

static struct zram_memcg_stats_entry *
zram_memcg_stats_find_locked(struct zram *zram, u64 cgroup_id)
{
	struct zram_memcg_stats_entry *entry;

	hash_for_each_possible(zram->memcg_stats_table, entry, node, cgroup_id) {
		if (entry->cgroup_id == cgroup_id)
			return entry;
	}

	return NULL;
}

static bool zram_memcg_stats_ensure(struct zram *zram,
				    u64 cgroup_id, gfp_t gfp)
{
	struct zram_memcg_stats_entry *entry;
	struct zram_memcg_stats_entry *new_entry;
	unsigned long flags;

	if (!cgroup_id)
		return true;

	spin_lock_irqsave(&zram->memcg_stats_lock, flags);
	entry = zram_memcg_stats_find_locked(zram, cgroup_id);
	spin_unlock_irqrestore(&zram->memcg_stats_lock, flags);
	if (entry)
		return true;

	new_entry = kzalloc(sizeof(*new_entry), gfp);
	if (!new_entry)
		return false;
	new_entry->cgroup_id = cgroup_id;

	spin_lock_irqsave(&zram->memcg_stats_lock, flags);
	entry = zram_memcg_stats_find_locked(zram, cgroup_id);
	if (entry) {
		spin_unlock_irqrestore(&zram->memcg_stats_lock, flags);
		kfree(new_entry);
		return true;
	}
	hash_add(zram->memcg_stats_table, &new_entry->node, cgroup_id);
	spin_unlock_irqrestore(&zram->memcg_stats_lock, flags);

	return true;
}

static void zram_memcg_stats_update(atomic64_t *counter, u64 value, bool add)
{
	s64 cur;

	if (!value)
		return;
	if (add) {
		atomic64_add(value, counter);
		return;
	}

	cur = atomic64_read(counter);
	atomic64_set(counter, cur > value ? cur - value : 0);
}

static void zram_memcg_account_snapshot(struct zram *zram,
					u32 index,
					struct zram_memcg_account *account)
{
	memset(account, 0, sizeof(*account));
	if (!zram_allocated(zram, index))
		return;

	account->cgroup_id = zram->table[index].memcg_id;
	if (!account->cgroup_id)
		return;

	account->allocated = true;
	account->wb = zram_test_flag(zram, index, ZRAM_WB);
	account->same = zram_test_flag(zram, index, ZRAM_SAME);
	account->huge = zram_test_flag(zram, index, ZRAM_HUGE);
	account->sddc = crystal_sddc_accounted_size_locked(zram, index,
			&account->size);
	if (!account->sddc)
		account->size = zram_get_obj_size(zram, index);
}

static void zram_memcg_stats_apply(struct zram *zram,
				   const struct zram_memcg_account *account, bool add)
{
	struct zram_memcg_stats_entry *entry;
	unsigned long flags;
	u64 zram_compressed_size = 0;

	if (!account || !account->allocated || !account->cgroup_id)
		return;

	spin_lock_irqsave(&zram->memcg_stats_lock, flags);
	entry = zram_memcg_stats_find_locked(zram, account->cgroup_id);
	if (!entry) {
		spin_unlock_irqrestore(&zram->memcg_stats_lock, flags);
		return;
	}

	if (account->wb) {
		zram_memcg_stats_update(&entry->writeback_pages, 1, add);
		zram_memcg_stats_update(&entry->writeback_original_size,
					PAGE_SIZE, add);
		zram_memcg_stats_update(&entry->writeback_size,
					account->size ? account->size : PAGE_SIZE,
					add);
	} else {
		zram_memcg_stats_update(&entry->resident_pages, 1, add);
		zram_memcg_stats_update(&entry->zram_original_size, PAGE_SIZE, add);
		if (!account->same)
			zram_compressed_size = account->sddc ? account->size :
				(account->size ? account->size : PAGE_SIZE);
		zram_memcg_stats_update(&entry->zram_compressed_size,
					zram_compressed_size, add);
	}
	if (account->same)
		zram_memcg_stats_update(&entry->same_pages, 1, add);
	if (account->huge)
		zram_memcg_stats_update(&entry->huge_pages, 1, add);
	spin_unlock_irqrestore(&zram->memcg_stats_lock, flags);
}

static void zram_memcg_stats_add_current(struct zram *zram, u32 index)
{
	struct zram_memcg_account account;

	zram_memcg_account_snapshot(zram, index, &account);
	zram_memcg_stats_apply(zram, &account, true);
}

static void zram_memcg_stats_sub_current(struct zram *zram, u32 index)
{
	struct zram_memcg_account account;

	zram_memcg_account_snapshot(zram, index, &account);
	zram_memcg_stats_apply(zram, &account, false);
}

#if IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC)
void crystal_sddc_zram_account_sub_locked(struct zram *zram, u32 index)
{
	zram_memcg_stats_sub_current(zram, index);
}

void crystal_sddc_zram_account_add_locked(struct zram *zram, u32 index)
{
	zram_memcg_stats_add_current(zram, index);
}

void crystal_sddc_zram_ref_account(struct zram *zram, u64 memcg_id,
		size_t size, bool add)
{
	struct zram_memcg_stats_entry *entry;

	if (!memcg_id || !size)
		return;

	spin_lock(&zram->memcg_stats_lock);
	entry = zram_memcg_stats_find_locked(zram, memcg_id);
	if (entry)
		zram_memcg_stats_update(&entry->zram_compressed_size, size, add);
	spin_unlock(&zram->memcg_stats_lock);
}
#endif

static void zram_memcg_stats_clear_all(struct zram *zram)
{
	struct zram_memcg_stats_entry *entry;
	struct hlist_node *tmp;
	unsigned long flags;
	int bucket;

	spin_lock_irqsave(&zram->memcg_stats_lock, flags);
	hash_for_each_safe(zram->memcg_stats_table, bucket, tmp, entry, node) {
		hash_del(&entry->node);
		kfree(entry);
	}
	spin_unlock_irqrestore(&zram->memcg_stats_lock, flags);
}

static unsigned long zram_memcg_scan_cursor_get(struct zram *zram,
						u64 cgroup_id,
						unsigned long nr_pages)
{
	struct zram_memcg_stats_entry *entry;
	unsigned long cursor = 0;
	unsigned long flags;

	if (!cgroup_id || !nr_pages)
		return 0;

	spin_lock_irqsave(&zram->memcg_stats_lock, flags);
	entry = zram_memcg_stats_find_locked(zram, cgroup_id);
	if (entry && entry->writeback_scan_cursor < nr_pages)
		cursor = entry->writeback_scan_cursor;
	spin_unlock_irqrestore(&zram->memcg_stats_lock, flags);
	return cursor;
}

static void zram_memcg_scan_cursor_set(struct zram *zram, u64 cgroup_id,
				       unsigned long cursor,
				       unsigned long nr_pages)
{
	struct zram_memcg_stats_entry *entry;
	unsigned long flags;

	if (!cgroup_id || !nr_pages)
		return;
	if (cursor >= nr_pages)
		cursor = 0;

	spin_lock_irqsave(&zram->memcg_stats_lock, flags);
	entry = zram_memcg_stats_find_locked(zram, cgroup_id);
	if (entry)
		entry->writeback_scan_cursor = cursor;
	spin_unlock_irqrestore(&zram->memcg_stats_lock, flags);
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

static inline void zram_set_priority(struct zram *zram, u32 index, u32 prio)
{
	prio &= ZRAM_COMP_PRIORITY_MASK;
	/*
	 * Clear previous priority value first, in case if we recompress
	 * further an already recompressed page
	 */
	zram->table[index].flags &= ~(ZRAM_COMP_PRIORITY_MASK <<
				      ZRAM_COMP_PRIORITY_BIT1);
	zram->table[index].flags |= (prio << ZRAM_COMP_PRIORITY_BIT1);
}

static inline u32 zram_get_priority(struct zram *zram, u32 index)
{
	u32 prio = zram->table[index].flags >> ZRAM_COMP_PRIORITY_BIT1;

	return prio & ZRAM_COMP_PRIORITY_MASK;
}

static void zram_accessed(struct zram *zram, u32 index)
{
	zram_clear_flag(zram, index, ZRAM_IDLE);
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_TRACK_ENTRY_ACTIME
	zram->table[index].ac_time = ktime_get_boottime();
#endif
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

#if IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC)
bool crystal_sddc_zram_memory_limit_ok(struct zram *zram)
{
	unsigned long pages = zs_get_total_pages(zram->mem_pool);

	update_used_max(zram, pages);
	return !zram->limit_pages || pages <= zram->limit_pages;
}
#endif

static void zram_atomic64_update_max(atomic64_t *max, s64 val)
{
	s64 old;

	if (val <= 0)
		return;

	old = atomic64_read(max);
	while (old < val) {
		s64 prev = atomic64_cmpxchg(max, old, val);

		if (prev == old)
			break;
		old = prev;
	}
}

static s64 zram_dev_id(struct zram *zram)
{
	if (!zram || !zram->disk)
		return -1;
	return zram->disk->first_minor;
}

static s64 zram_ns_to_s64(u64 ns)
{
	return ns > S64_MAX ? S64_MAX : (s64)ns;
}

static u64 zram_pages_to_bytes_u64(u64 pages)
{
	if (pages > (U64_MAX >> PAGE_SHIFT))
		return U64_MAX;

	return pages << PAGE_SHIFT;
}

static inline void zram_fill_page(void *ptr, unsigned long len,
				  unsigned long value)
{
	WARN_ON_ONCE(!IS_ALIGNED(len, sizeof(unsigned long)));
	memset_l(ptr, value, len / sizeof(unsigned long));
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
	u64 disksize;

	down_read(&zram->init_lock);
	disksize = zram->disksize;
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%llu\n", disksize);
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
 * Callers should hold the zram init lock in read mode
 */
static void mark_idle(struct zram *zram, ktime_t cutoff)
{
	int is_idle = 1;
	unsigned long nr_pages = zram_pages_snapshot(zram);
	unsigned long index;

	for (index = 0; index < nr_pages; index++) {
		/*
		 * Do not mark ZRAM_UNDER_WB slot as ZRAM_IDLE to close race.
		 * See the comment in writeback_store.
		 *
		 * Also do not mark ZRAM_SAME slots as ZRAM_IDLE, because no
		 * post-processing (recompress, writeback) happens to the
		 * ZRAM_SAME slot.
		 *
		 * And ZRAM_WB slots simply cannot be ZRAM_IDLE.
		 */
		zram_slot_lock(zram, index);
		if (!zram_allocated(zram, index) ||
		    zram_test_flag(zram, index, ZRAM_WB) ||
		    zram_test_flag(zram, index, ZRAM_UNDER_WB) ||
		    zram_test_flag(zram, index, ZRAM_SAME)) {
			zram_slot_unlock(zram, index);
			continue;
		}

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_TRACK_ENTRY_ACTIME
		is_idle = !cutoff ||
			ktime_after(cutoff, zram->table[index].ac_time);
#endif
		if (is_idle)
			zram_set_flag(zram, index, ZRAM_IDLE);
		else
			zram_clear_flag(zram, index, ZRAM_IDLE);
		zram_slot_unlock(zram, index);
	}
}

static ssize_t idle_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	ktime_t cutoff_time = 0;
	ssize_t rv = -EINVAL;

	if (!sysfs_streq(buf, "all")) {
		/*
		 * If it did not parse as 'all' try to treat it as an integer
		 * when we have memory tracking enabled.
		 */
		u64 age_sec;

		if (IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_TRACK_ENTRY_ACTIME) &&
		    !kstrtoull(buf, 0, &age_sec))
			cutoff_time = ktime_sub(ktime_get_boottime(),
					ns_to_ktime(age_sec * NSEC_PER_SEC));
		else
			goto out;
	}

	down_read(&zram->init_lock);
	if (!init_done(zram))
		goto out_unlock;

	/*
	 * A cutoff_time of 0 marks everything as idle, this is the
	 * "all" behavior.
	 */
	mark_idle(zram, cutoff_time);
	rv = len;

out_unlock:
	up_read(&zram->init_lock);
out:
	return rv;
}

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
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

	zram_zms_stop_gc(zram);
	zms_destroy(zram->zms);
	zram->zms = NULL;
	bdev = zram->bdev;
	blkdev_put(bdev, zram);
	/* hope filp_close flush all of IO */
	filp_close(zram->backing_dev, NULL);
	zram->backing_dev = NULL;
	zram->bdev = NULL;
	zram->disk->fops = &zram_devops;
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

static bool zram_has_backing_pages(struct zram *zram)
{
	unsigned long nr_zram_pages = zram_pages_snapshot(zram);
	unsigned long index;

	for (index = 0; index < nr_zram_pages; index++) {
		bool written;

		zram_slot_lock(zram, index);
		written = zram_test_flag(zram, index, ZRAM_WB);
		zram_slot_unlock(zram, index);
		if (written)
			return true;
		cond_resched();
	}

	return false;
}

static int zram_set_backing_dev(struct zram *zram, const char *buf, size_t len,
					bool allow_late)
{
	char *file_name, *path;
	size_t copy;
	struct file *backing_dev = NULL;
	struct inode *inode;
	struct address_space *mapping;
	u64 backing_pages;
	unsigned long nr_pages;
	struct block_device *bdev = NULL;
	struct zms *zms = NULL;
	bool late_bind;
	int err;

	if (!zram || !buf)
		return -EINVAL;

	file_name = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!file_name)
		return -ENOMEM;

	down_write(&zram->init_lock);
	late_bind = init_done(zram);
	if (late_bind && !allow_late) {
		pr_info("Can't setup backing device for initialized device\n");
		err = -EBUSY;
		chs_log(CHS_LOG_WARN,
			"backing_dev bind failed initialized ret=%d\n", err);
		goto out;
	}

	if (zram->backing_dev) {
		err = -EBUSY;
		chs_log(CHS_LOG_WARN,
			"backing_dev bind failed already configured late=%d ret=%d\n",
			late_bind, err);
		goto out;
	}

	if (late_bind && zram_has_backing_pages(zram)) {
		err = -EBUSY;
		chs_log(CHS_LOG_ERR,
			"late backing_dev bind failed existing writeback slots ret=%d\n",
			err);
		goto out;
	}

	copy = min(len, (size_t)PATH_MAX - 1);
	memcpy(file_name, buf, copy);
	file_name[copy] = '\0';
	path = strim(file_name);
	if (!path[0]) {
		err = -EINVAL;
		chs_log(CHS_LOG_ERR, "backing_dev bind failed empty path ret=%d\n",
			err);
		goto out;
	}

	backing_dev = filp_open_block(path, O_RDWR | O_LARGEFILE, 0);
	if (IS_ERR(backing_dev)) {
		err = PTR_ERR(backing_dev);
		chs_log(CHS_LOG_ERR,
			"backing_dev open failed path=%s late=%d ret=%d\n",
			path, late_bind, err);
		backing_dev = NULL;
		goto out;
	}

	mapping = backing_dev->f_mapping;
	inode = mapping->host;

	/* Support only block device in this moment */
	if (!S_ISBLK(inode->i_mode)) {
		err = -ENOTBLK;
		chs_log(CHS_LOG_ERR,
			"backing_dev bind failed non-block path=%s late=%d ret=%d\n",
			path, late_bind, err);
		goto out;
	}

	bdev = blkdev_get_by_dev(inode->i_rdev, BLK_OPEN_READ | BLK_OPEN_WRITE,
				 zram, NULL);
	if (IS_ERR(bdev)) {
		err = PTR_ERR(bdev);
		chs_log(CHS_LOG_ERR,
			"backing_dev get failed path=%s late=%d ret=%d\n",
			path, late_bind, err);
		bdev = NULL;
		goto out;
	}

	backing_pages = i_size_read(inode) >> PAGE_SHIFT;
	/* Refuse to use zero sized device (also prevents self reference) */
	if (!backing_pages) {
		err = -EINVAL;
		chs_log(CHS_LOG_ERR,
			"backing_dev bind failed zero-size path=%s late=%d ret=%d\n",
			path, late_bind, err);
		goto out;
	}
	if (backing_pages > ULONG_MAX) {
		err = -EOVERFLOW;
		chs_log(CHS_LOG_ERR,
			"backing_dev pages overflow path=%s pages=%llu late=%d ret=%d\n",
			path, backing_pages, late_bind, err);
		goto out;
	}
	nr_pages = (unsigned long)backing_pages;

	err = set_blocksize(bdev, PAGE_SIZE);
	if (err) {
		chs_log(CHS_LOG_ERR,
			"backing_dev set blocksize failed path=%s late=%d ret=%d\n",
			path, late_bind, err);
		goto out;
	}

	zms = zms_create(bdev, nr_pages,
			 late_bind ? zram_pages_snapshot(zram) : 1,
			 crystal_hybridswap_account_physical_write_pages);
	if (!zms) {
		err = -ENOMEM;
		chs_log(CHS_LOG_ERR,
			"backing_dev zms alloc failed path=%s pages=%lu late=%d ret=%d\n",
			path, nr_pages, late_bind, err);
		goto out;
	}

	zram->bdev = bdev;
	zram->backing_dev = backing_dev;
	zram->zms = zms;
	zram_zms_start_gc(zram);
	up_write(&zram->init_lock);

	pr_info("setup backing device %s%s\n", path,
		late_bind ? " by late bind" : "");
	chs_log(CHS_LOG_INFO,
		"%s backing_dev bind success path=%s pages=%lu\n",
		late_bind ? "late" : "early", path, nr_pages);
	kfree(file_name);

	return 0;
out:
	zms_destroy(zms);

	if (bdev)
		blkdev_put(bdev, zram);

	if (backing_dev)
		filp_close(backing_dev, NULL);

	up_write(&zram->init_lock);

	kfree(file_name);

	return err;
}

int zram_bind_backing_dev(struct device *dev, const char *buf, size_t len)
{
	struct zram *zram;
	int ret;

	zram = zram_get_from_dev(dev);
	if (!zram)
		return -ENODEV;

	ret = zram_set_backing_dev(zram, buf, len, true);
	zram_put(zram);
	return ret;
}

static ssize_t backing_dev_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram;
	int err;

	zram = zram_get_from_dev(dev);
	if (!zram)
		return -ENODEV;
	err = zram_set_backing_dev(zram, buf, len, false);
	zram_put(zram);
	return err ? err : len;
}

#define PAGE_WB_SIG "page_index="

#define PAGE_WRITEBACK			0
#define HUGE_WRITEBACK			(1<<0)
#define IDLE_WRITEBACK			(1<<1)
#define INCOMPRESSIBLE_WRITEBACK	(1<<2)
#define HYBRIDSWAP_NORMAL_WRITEBACK	(1<<3)
#define ZRAM_WB_BATCH_MAX		32
#define ZRAM_ZMS_WB_MIN_FREE_BLOCKS \
	(ZRAM_WB_BATCH_MAX * ZMS_MAX_PAGES_PER_ZSPAGE)
#define ZRAM_AUTO_WB_COLD_AGE_MS	60000ULL
#define ZRAM_ZMS_GC_PERIODIC_INTERVAL	(15 * HZ)
#define ZRAM_ZMS_GC_MIN_USED_BLOCKS	128UL
#define ZRAM_ZMS_GC_PARTIAL_PCT		5UL
#define ZRAM_ZMS_GC_CACHED_BLOCKS	1024UL
#define ZRAM_ZMS_GC_FREE_PCT		10UL
#define ZRAM_ZMS_GC_CONTINUE_DELAY_MS	100UL

struct zram_wb_item {
	u32 index;
	void *data;
	size_t size;
	unsigned long handle;
	struct zram_wb_snapshot snapshot;
	struct crystal_sddc_wb_capture native;
	bool native_sddc;
};

static int zram_wb_item_class_cmp(const void *lhs, const void *rhs)
{
	const struct zram_wb_item *a = lhs;
	const struct zram_wb_item *b = rhs;
	unsigned int a_class = zms_size_to_class(a->size);
	unsigned int b_class = zms_size_to_class(b->size);

	if (a_class != b_class)
		return a_class < b_class ? -1 : 1;
	if (a->index == b->index)
		return 0;
	return a->index < b->index ? -1 : 1;
}

static bool zram_zms_gc_should_run(const struct zms_stats *stats)
{
	unsigned long partial_pct;
	unsigned long free_pct;

	if (crystal_hybridswap_system_sleeping())
		return false;

	if (!check_charging_state())
		return false;

	if (!stats || !stats->nr_blocks || !stats->used_blocks)
		return false;

	if (stats->used_blocks < ZRAM_ZMS_GC_MIN_USED_BLOCKS)
		return false;

	if (stats->pending_free)
		return true;

	if (stats->dirty_blocks)
		return true;

	if (!stats->partial_blocks)
		return false;

	partial_pct = stats->partial_blocks * 100 / stats->used_blocks;
	if (partial_pct >= ZRAM_ZMS_GC_PARTIAL_PCT)
		return true;

	free_pct = stats->free_blocks * 100 / stats->nr_blocks;
	return free_pct <= ZRAM_ZMS_GC_FREE_PCT;
}

static int zram_zms_gc_run_locked(struct zram *zram, const char *reason)
{
	struct zms_stats stats;
	struct zms_io io;
	int ret;
	int flush_ret;

	if (!zram->zms)
		return 0;

	if (crystal_hybridswap_system_sleeping())
		return 0;

	ret = zms_get_stats(zram->zms, &stats);
	if (ret)
		return ret;

	if (!zram_zms_gc_should_run(&stats))
		return 0;

	if (stats.dirty_blocks) {
		flush_ret = zms_flush_all(zram->zms, GFP_NOIO, &io);
		zram_record_zms_io(zram, 0, &io);
		if (flush_ret) {
			chs_log_ratelimited(CHS_LOG_WARN,
					    "zms gc flush failed reason=%s dirty=%lu ret=%d\n",
					    reason, stats.dirty_blocks,
					    flush_ret);
			return flush_ret;
		}
	}

	ret = zms_compact(zram->zms, GFP_NOIO, &io);
	zram_record_zms_io(zram, 0, &io);
	chs_log_ratelimited(ret == -EAGAIN ? CHS_LOG_INFO :
			    (ret ? CHS_LOG_WARN : CHS_LOG_INFO),
			    "zms gc %s reason=%s used=%lu free=%lu partial=%lu pending=%lu dirty=%lu ret=%d\n",
			    ret == -EAGAIN ? "continue" :
			    (ret ? "failed" : "done"), reason,
			    stats.used_blocks, stats.free_blocks,
			    stats.partial_blocks, stats.pending_free,
			    stats.dirty_blocks, ret);
	return ret;
}

static void zram_zms_gc_workfn(struct work_struct *work)
{
	struct zram *zram = container_of(work, struct zram, zms_gc_work);
	int ret = 0;

	if (READ_ONCE(zram->zms_gc_stopping))
		goto out;

	if (down_read_trylock(&zram->init_lock)) {
		if (!READ_ONCE(zram->zms_gc_stopping) && init_done(zram))
			ret = zram_zms_gc_run_locked(zram, "scheduled");
		up_read(&zram->init_lock);
	}

out:
	atomic_set(&zram->zms_gc_pending, 0);
	if (ret == -EAGAIN && !READ_ONCE(zram->zms_gc_stopping)) {
		queue_delayed_work(system_unbound_wq,
				   &zram->zms_gc_periodic_work,
				   msecs_to_jiffies(ZRAM_ZMS_GC_CONTINUE_DELAY_MS));
	}
}

static void zram_zms_gc_periodic_workfn(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct zram *zram = container_of(dwork, struct zram,
					 zms_gc_periodic_work);

	if (READ_ONCE(zram->zms_gc_stopping))
		return;

	if (down_read_trylock(&zram->init_lock)) {
		if (!READ_ONCE(zram->zms_gc_stopping) && init_done(zram))
			zram_zms_gc_run_locked(zram, "periodic");
		up_read(&zram->init_lock);
	}

	if (!READ_ONCE(zram->zms_gc_stopping))
		queue_delayed_work(system_unbound_wq,
				   &zram->zms_gc_periodic_work,
				   ZRAM_ZMS_GC_PERIODIC_INTERVAL);
}

static void zram_zms_schedule_gc(struct zram *zram)
{
	if (READ_ONCE(zram->zms_gc_stopping))
		return;

	if (crystal_hybridswap_system_sleeping())
		return;

	if (!zram->zms)
		return;

	if (atomic_cmpxchg(&zram->zms_gc_pending, 0, 1) == 0)
		queue_work(system_unbound_wq, &zram->zms_gc_work);
}

static void zram_zms_start_gc(struct zram *zram)
{
	WRITE_ONCE(zram->zms_gc_stopping, false);
	atomic_set(&zram->zms_gc_pending, 0);
	queue_delayed_work(system_unbound_wq, &zram->zms_gc_periodic_work,
			   ZRAM_ZMS_GC_PERIODIC_INTERVAL);
}

static void zram_zms_stop_gc(struct zram *zram)
{
	WRITE_ONCE(zram->zms_gc_stopping, true);
	cancel_delayed_work_sync(&zram->zms_gc_periodic_work);
	cancel_work_sync(&zram->zms_gc_work);
	atomic_set(&zram->zms_gc_pending, 0);
}

static int zram_parse_writeback_mode(struct zram *zram, const char *buf,
				     bool internal, int *mode, unsigned long *index,
				     unsigned long *nr_pages)
{
	if (sysfs_streq(buf, "idle")) {
		*mode = IDLE_WRITEBACK;
	} else if (sysfs_streq(buf, "huge")) {
		*mode = HUGE_WRITEBACK;
	} else if (sysfs_streq(buf, "huge_idle")) {
		*mode = IDLE_WRITEBACK | HUGE_WRITEBACK;
	} else if (sysfs_streq(buf, "incompressible")) {
		*mode = INCOMPRESSIBLE_WRITEBACK;
	} else if (internal && sysfs_streq(buf, CHS_INTERNAL_WB_MODE)) {
		*mode = HYBRIDSWAP_NORMAL_WRITEBACK;
	} else {
		if (strncmp(buf, PAGE_WB_SIG, sizeof(PAGE_WB_SIG) - 1))
			return -EINVAL;

		if (kstrtoul(buf + sizeof(PAGE_WB_SIG) - 1, 10, index))
			return -EINVAL;

		*nr_pages = 1;
		*mode = PAGE_WRITEBACK;
		return 0;
	}

	*index = 0;
	*nr_pages = 0;
	return 0;
}

static void zram_writeback_clear_under_wb(struct zram *zram, u32 index)
{
	zram_slot_lock(zram, index);
	zram_clear_flag_wake(zram, index, ZRAM_UNDER_WB);
	zram_clear_flag(zram, index, ZRAM_IDLE);
	zram_slot_unlock(zram, index);
	crystal_sddc_requeue_observation(zram, index);
}

static bool zram_prefetch_expired_locked(struct zram *zram, u32 index)
{
	return zram_test_flag(zram, index, ZRAM_PREFETCHED) &&
	       zram_prefetch_expired(zram, index);
}

static void zram_reclaim_prefetched_locked(struct zram *zram, u32 index)
{
	if (!zram_prefetch_expired_locked(zram, index))
		return;

	zram_clear_flag(zram, index, ZRAM_PREFETCHED);
	zram->table[index].prefetch_jiffies = 0;
	atomic64_inc(&zram->stats.prefetch_expired);
	atomic64_inc(&zram->stats.prefetch_reclaimed);
}

static void zram_record_auto_wb_age(struct zram *zram, s64 age_ms)
{
	s64 old;

	if (age_ms < 0)
		return;

	atomic64_set(&zram->stats.auto_wb_last_age_ms, age_ms);
	old = atomic64_read(&zram->stats.auto_wb_max_age_ms);
	while (old < age_ms) {
		s64 prev = atomic64_cmpxchg(&zram->stats.auto_wb_max_age_ms,
					    old, age_ms);

		if (prev == old)
			break;
		old = prev;
	}
}

static bool zram_auto_writeback_page_cold(struct zram *zram, u32 index,
					  ktime_t now)
{
	if (zram_prefetch_expired_locked(zram, index))
		return true;

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_TRACK_ENTRY_ACTIME
	ktime_t ac_time;
	s64 age_ms;

	if (zram_test_flag(zram, index, ZRAM_IDLE)) {
		atomic64_inc(&zram->stats.auto_wb_idle_selected);
		return true;
	}

	ac_time = zram->table[index].ac_time;
	if (!ac_time) {
		atomic64_inc(&zram->stats.auto_wb_unknown_age_skipped);
		return false;
	}

	age_ms = ktime_ms_delta(now, ac_time);
	if (age_ms < 0) {
		atomic64_inc(&zram->stats.auto_wb_unknown_age_skipped);
		return false;
	}

	zram_record_auto_wb_age(zram, age_ms);
	if (age_ms < ZRAM_AUTO_WB_COLD_AGE_MS)
		return false;

	zram_set_flag(zram, index, ZRAM_IDLE);
	atomic64_inc(&zram->stats.auto_wb_age_selected);
	return true;
#else
	return zram_test_flag(zram, index, ZRAM_IDLE);
#endif
}

static bool zram_writeback_snapshot_matches(struct zram *zram, u32 index,
		const struct zram_wb_snapshot *snapshot)
{
	if (!zram_allocated(zram, index))
		return false;
	if (zram_test_flag(zram, index, ZRAM_WB) ||
	    zram_test_flag(zram, index, ZRAM_SAME) ||
	    !zram_test_flag(zram, index, ZRAM_UNDER_WB) ||
	    !zram_test_flag(zram, index, ZRAM_IDLE))
		return false;
	if (zram_get_handle(zram, index) != snapshot->element)
		return false;
	if (zram_get_obj_size(zram, index) != snapshot->size)
		return false;
	if (zram->table[index].memcg_id != snapshot->memcg_id)
		return false;

	return zram_snapshot_flags(zram, index) == snapshot->flags &&
		crystal_sddc_snapshot_matches_locked(zram, index,
			&snapshot->sddc);
}

static int zram_writeback_alloc_buffers(struct zram_wb_item *items,
					unsigned int nr)
{
	unsigned int i;

	for (i = 0; i < nr; i++) {
		items[i].data = (void *)__get_free_page(GFP_KERNEL);
		if (!items[i].data)
			goto fail;
	}

	return 0;

fail:
	while (i--)
		free_page((unsigned long)items[i].data);
	return -ENOMEM;
}

static void zram_writeback_free_buffers(struct zram_wb_item *items,
					unsigned int nr)
{
	unsigned int i;

	for (i = 0; i < nr; i++) {
		if (items[i].data)
			free_page((unsigned long)items[i].data);
	}
}

static void zram_writeback_abort_native(struct zram *zram,
		struct zram_wb_item *item)
{
	if (!item || !item->native_sddc)
		return;
	crystal_sddc_native_wb_abort(zram, &item->native);
	item->native_sddc = false;
}

static void zram_writeback_finish_native(struct zram_wb_item *item)
{
	if (!item || !item->native_sddc)
		return;
	crystal_sddc_native_wb_finish(&item->native);
	item->native_sddc = false;
}

static unsigned int zram_writeback_batch_limit(struct zram *zram,
					       unsigned long max_pages,
					       unsigned long written)
{
	unsigned int limit = ZRAM_WB_BATCH_MAX;

	if (max_pages && max_pages - written < limit)
		limit = max_pages - written;

	spin_lock(&zram->wb_limit_lock);
	if (zram->wb_limit_enable && zram->bd_wb_limit < limit)
		limit = zram->bd_wb_limit;
	spin_unlock(&zram->wb_limit_lock);

	return limit;
}

static int zram_zms_writeback_check_space(struct zram *zram,
					  unsigned int batch_limit,
					  struct zms_stats *stats,
					  unsigned long *min_free)
{
	unsigned long needed;
	int ret;

	if (!batch_limit)
		return -EDQUOT;

	needed = max_t(unsigned long, ZMS_MAX_PAGES_PER_ZSPAGE,
		       (unsigned long)batch_limit * ZMS_MAX_PAGES_PER_ZSPAGE);
	if (min_free)
		*min_free = needed;

	ret = zms_get_stats(zram->zms, stats);
	if (ret)
		return ret;

	/*
	 * zms_stat free_blocks includes the permanently reserved block 0.  Keep
	 * strictly more than one worst-case batch available, otherwise automatic
	 * writeback can spin into an ENOSPC storm near the end of the device.
	 */
	if (stats->free_blocks <= needed)
		return -ENOSPC;

	return 0;
}

static void zram_record_zms_io(struct zram *zram, u32 index,
			       const struct zms_io *io)
{
	sector_t sector;
	s64 ns_s64;

	if (!io || !io->submitted)
		return;

	if (io->read_submitted) {
		sector = io->read_last_block * (PAGE_SIZE >> 9);
		ns_s64 = zram_ns_to_s64(io->read_total_latency_ns);
		atomic64_add(io->read_ios,
			     &zram->stats.bd_read_sync_ios);
		atomic64_add(ns_s64, &zram->stats.bd_read_total_ns);
		zram_atomic64_update_max(&zram->stats.bd_read_max_ns,
					  zram_ns_to_s64(io->read_max_latency_ns));
		atomic64_set(&zram->stats.bd_read_last_ret,
			     io->read_last_ret);
		atomic64_add(io->read_failed, &zram->stats.bd_read_failures);
		if (io->read_failed)
			chs_log_ratelimited(CHS_LOG_WARN,
				"backing read failed dev=%s index=%u blk=%lu sector=%llu latency_ns=%llu ret=%d sync=%d\n",
				zram->disk ? zram->disk->disk_name : "unknown",
				index, io->read_last_block,
				(unsigned long long)sector,
				(unsigned long long)io->read_last_latency_ns,
				io->read_last_ret, true);
		if (io->read_max_latency_ns >= CHS_ZRAM_SLOW_IO_NS) {
			atomic64_inc(&zram->stats.bd_read_slow_ios);
			atomic64_set(&zram->stats.bd_read_last_slow_dev,
				zram_dev_id(zram));
			atomic64_set(&zram->stats.bd_read_last_slow_sector,
				sector);
			atomic64_set(&zram->stats.bd_read_last_slow_index,
				index);
			atomic64_set(&zram->stats.bd_read_last_slow_ret,
				io->read_last_ret);
			atomic64_set(&zram->stats.bd_read_last_slow_ns,
				zram_ns_to_s64(io->read_max_latency_ns));
		}
	}
	if (io->write_submitted) {
		sector = io->write_last_block * (PAGE_SIZE >> 9);
		ns_s64 = zram_ns_to_s64(io->write_total_latency_ns);
		atomic64_add(io->write_ios, &zram->stats.bd_write_ios);
		atomic64_add(ns_s64, &zram->stats.bd_write_total_ns);
		zram_atomic64_update_max(&zram->stats.bd_write_max_ns,
					  zram_ns_to_s64(io->write_max_latency_ns));
		atomic64_set(&zram->stats.bd_write_last_ret,
			     io->write_last_ret);
		atomic64_add(io->write_failed,
			     &zram->stats.bd_write_failures);
		if (io->write_failed)
			chs_log_ratelimited(CHS_LOG_WARN,
				"backing write failed dev=%s index=%u blk=%lu sector=%llu latency_ns=%llu ret=%d\n",
				zram->disk ? zram->disk->disk_name : "unknown",
				index, io->write_last_block,
				(unsigned long long)sector,
				(unsigned long long)io->write_last_latency_ns,
				io->write_last_ret);
		if (io->write_max_latency_ns >= CHS_ZRAM_SLOW_IO_NS) {
			atomic64_inc(&zram->stats.bd_write_slow_ios);
			atomic64_set(&zram->stats.bd_write_last_slow_dev,
				zram_dev_id(zram));
			atomic64_set(&zram->stats.bd_write_last_slow_sector,
				sector);
			atomic64_set(&zram->stats.bd_write_last_slow_index,
				index);
			atomic64_set(&zram->stats.bd_write_last_slow_ret,
				io->write_last_ret);
			atomic64_set(&zram->stats.bd_write_last_slow_ns,
				zram_ns_to_s64(io->write_max_latency_ns));
		}
	}
}

static int zram_writeback_pages(struct zram *zram, int mode,
				unsigned long index, unsigned long nr_pages,
				unsigned long max_pages, u64 target_cgroup_id,
				bool auto_req,
				struct crystal_hybridswap_writeback_stats *wb_stats)
{
	struct zram_wb_item *items = NULL;
	int ret = 0;
	int err;
	unsigned long written = 0;
	unsigned long scanned = 0;
	unsigned long eligible = 0;
	unsigned long unknown_or_filtered = 0;
	unsigned long device_pages = 0;
	unsigned long scan_start = index;
	bool stop_writeback = false;
	bool per_memcg_force = target_cgroup_id != 0;
	bool cursor_active = false;
	bool auto_normal = auto_req && (mode & HYBRIDSWAP_NORMAL_WRITEBACK);
	ktime_t auto_now = 0;
	unsigned int i;

	if (wb_stats)
		memset(wb_stats, 0, sizeof(*wb_stats));

	if (crystal_hybridswap_system_sleeping()) {
		ret = -EBUSY;
		chs_log_ratelimited(CHS_LOG_INFO,
				    "writeback skip system_sleeping mode=0x%x index=%lu nr_pages=%lu max_pages=%lu ret=%d\n",
				    mode, index, nr_pages, max_pages, ret);
		goto out_stats;
	}

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		ret = -EINVAL;
		chs_log_ratelimited(CHS_LOG_WARN,
				    "writeback skip init_not_done mode=0x%x ret=%d\n",
				    mode, ret);
		goto release_init_lock;
	}

	if (!zram->backing_dev || !zram->zms) {
		ret = -ENXIO;
		chs_log_ratelimited(CHS_LOG_WARN,
				    "writeback skip no backing_dev mode=0x%x index=%lu nr_pages=%lu max_pages=%lu ret=%d\n",
				    mode, index, nr_pages, max_pages, ret);
		goto release_init_lock;
	}

	device_pages = zram_pages_snapshot(zram);
	if (!nr_pages)
		nr_pages = device_pages;
	if (!zram_valid_io_range(zram, index, nr_pages)) {
		ret = -EINVAL;
		chs_log_ratelimited(CHS_LOG_WARN,
			"writeback skip invalid range mode=0x%x index=%lu nr_pages=%lu ret=%d\n",
			mode, index, nr_pages, ret);
		goto release_init_lock;
	}
	if (per_memcg_force && index == 0 && nr_pages == device_pages) {
		cursor_active = true;
		index = zram_memcg_scan_cursor_get(zram, target_cgroup_id, device_pages);
		scan_start = index;
	}

	if (auto_normal) {
		auto_now = ktime_get_boottime();
		atomic64_set(&zram->stats.auto_wb_cold_age_ms,
			     ZRAM_AUTO_WB_COLD_AGE_MS);
	}

	{
		struct zms_stats stats = {};
		unsigned long min_free = ZRAM_ZMS_WB_MIN_FREE_BLOCKS;
		unsigned int batch_limit;

		batch_limit = zram_writeback_batch_limit(zram, max_pages, 0);
		if (nr_pages < batch_limit)
			batch_limit = nr_pages;
		ret = zram_zms_writeback_check_space(zram, batch_limit,
						     &stats, &min_free);
		if (ret) {
			chs_log_ratelimited(ret == -ENOSPC ?
					    CHS_LOG_WARN : CHS_LOG_ERR,
				"writeback skip zms no space mode=0x%x index=%lu nr_pages=%lu max_pages=%lu used=%lu free=%lu min_free=%lu partial=%lu dirty=%lu ret=%d\n",
				mode, index, nr_pages, max_pages,
				stats.used_blocks, stats.free_blocks, min_free,
				stats.partial_blocks, stats.dirty_blocks, ret);
			goto release_init_lock;
		}
	}

	items = kcalloc(ZRAM_WB_BATCH_MAX, sizeof(*items), GFP_KERNEL);
	if (!items) {
		ret = -ENOMEM;
		chs_log(CHS_LOG_ERR,
			"writeback batch alloc items failed ret=%d\n", ret);
		goto release_init_lock;
	}

	ret = zram_writeback_alloc_buffers(items, ZRAM_WB_BATCH_MAX);
	if (ret) {
		chs_log(CHS_LOG_ERR,
			"writeback batch alloc buffers failed ret=%d\n", ret);
		goto release_items;
	}

	while (nr_pages != 0) {
		unsigned int batch_limit;
		unsigned int batch_count = 0;

		if (crystal_hybridswap_system_sleeping()) {
			ret = -EBUSY;
			chs_log_ratelimited(CHS_LOG_INFO,
				"writeback stop system_sleeping mode=0x%x scanned=%lu eligible=%lu written=%lu ret=%d\n",
				mode, scanned, eligible, written, ret);
			break;
		}

		if (max_pages && written >= max_pages)
			break;

		batch_limit = zram_writeback_batch_limit(zram, max_pages,
							 written);
		if (!batch_limit) {
			ret = -EDQUOT;
			chs_log_ratelimited(CHS_LOG_WARN,
				"writeback skip limit exhausted mode=0x%x index=%lu written=%lu ret=%d\n",
				mode, index, written, ret);
			break;
		}

		for (; nr_pages != 0 && batch_count < batch_limit; nr_pages--) {
			u32 cur_index = index;
			u64 slot_memcg_id;

			index++;
			if (cursor_active && index >= device_pages)
				index = 0;

			scanned++;
			if (auto_normal)
				atomic64_inc(&zram->stats.auto_wb_scan_pages);

			zram_slot_lock(zram, cur_index);
			if (!zram_allocated(zram, cur_index))
				goto scan_next;

			if (zram_test_flag(zram, cur_index, ZRAM_WB) ||
			    zram_test_flag(zram, cur_index, ZRAM_SAME) ||
			    zram_test_flag(zram, cur_index, ZRAM_UNDER_WB))
				goto scan_next;

			slot_memcg_id = zram->table[cur_index].memcg_id;
			if (target_cgroup_id) {
				if (!slot_memcg_id ||
				    slot_memcg_id != target_cgroup_id) {
					unknown_or_filtered++;
					goto scan_next;
				}
			}

			if (mode & IDLE_WRITEBACK &&
			    !zram_test_flag(zram, cur_index, ZRAM_IDLE))
				goto scan_next;
			if (mode & HUGE_WRITEBACK &&
			    !zram_test_flag(zram, cur_index, ZRAM_HUGE))
				goto scan_next;
			if (mode & INCOMPRESSIBLE_WRITEBACK &&
			    !zram_test_flag(zram, cur_index, ZRAM_INCOMPRESSIBLE))
				goto scan_next;
			if (!per_memcg_force &&
			    (mode & HYBRIDSWAP_NORMAL_WRITEBACK) &&
			    (zram_test_flag(zram, cur_index, ZRAM_HUGE) ||
			     zram_test_flag(zram, cur_index, ZRAM_INCOMPRESSIBLE))) {
				if (auto_normal)
					atomic64_inc(
						&zram->stats.auto_wb_kind_skipped);
				goto scan_next;
			}

			if (auto_normal &&
			    !zram_auto_writeback_page_cold(zram, cur_index,
							   auto_now)) {
				atomic64_inc(&zram->stats.auto_wb_hot_skipped);
				goto scan_next;
			}

			/*
			 * Clearing ZRAM_UNDER_WB is duty of caller.
			 * IOW, zram_free_page never clear it.
			 */
			zram_set_flag(zram, cur_index, ZRAM_UNDER_WB);
			/* Need for hugepage writeback racing */
			zram_set_flag(zram, cur_index, ZRAM_IDLE);
			zram_take_wb_snapshot(zram, cur_index,
				&items[batch_count].snapshot);
			items[batch_count].size = 0;
			if (crystal_sddc_slot_allocated_locked(zram, cur_index)) {
				zram_slot_unlock(zram, cur_index);
				if (crystal_sddc_native_wb_kind(
					    items[batch_count].snapshot.sddc.kind)) {
					err = crystal_sddc_native_wb_capture(zram,
						cur_index,
						&items[batch_count].snapshot.sddc,
						items[batch_count].data,
						&items[batch_count].size,
						&items[batch_count].native);
					if (!err)
						items[batch_count].native_sddc = true;
					else
						/*
						 * A native admission miss is opportunistic; retain
						 * the established flatten fallback.
						 */
						err = crystal_sddc_flatten(zram, cur_index,
							&items[batch_count].snapshot.sddc,
							items[batch_count].data,
							&items[batch_count].size);
				} else {
					err = crystal_sddc_flatten(zram, cur_index,
						&items[batch_count].snapshot.sddc,
						items[batch_count].data,
						&items[batch_count].size);
				}
			} else {
				err = zram_read_compressed_page(zram, cur_index,
					items[batch_count].data,
					&items[batch_count].size);
				zram_slot_unlock(zram, cur_index);
			}
			if (!err && (!items[batch_count].size ||
				     items[batch_count].size > PAGE_SIZE)) {
				chs_log_ratelimited(CHS_LOG_ERR,
						    "writeback capture invalid size mode=0x%x index=%u size=%zu sddc_kind=%u native=%u\n",
					    mode, cur_index, items[batch_count].size,
					    items[batch_count].snapshot.sddc.kind,
					    items[batch_count].native_sddc);
				err = -EIO;
				if (!ret)
					ret = err;
			}
			if (err) {
				zram_writeback_abort_native(zram,
					&items[batch_count]);
				zram_writeback_clear_under_wb(zram, cur_index);
				continue;
			}

			items[batch_count].index = cur_index;
			items[batch_count].handle = (unsigned long)cur_index + 1;
			batch_count++;
			eligible++;
			if (auto_normal)
				atomic64_inc(&zram->stats.auto_wb_cold_selected);
			continue;

scan_next:
			zram_slot_unlock(zram, cur_index);
		}

		if (!batch_count)
			continue;

		sort(items, batch_count, sizeof(*items), zram_wb_item_class_cmp,
		     NULL);
		for (i = 0; i < batch_count; ) {
			struct zms_store_item store_items[ZRAM_WB_BATCH_MAX];
			unsigned int store_map[ZRAM_WB_BATCH_MAX];
			unsigned int group_end = i + 1;
			unsigned int store_count = 0;
			unsigned int j;
			struct zms_io io;

			while (group_end < batch_count &&
			       zms_size_to_class(items[group_end].size) ==
			       zms_size_to_class(items[i].size))
				group_end++;

			for (j = i; j < group_end; j++) {
				struct zram_wb_item *item = &items[j];
				u32 cur_index = item->index;

				zram_slot_lock(zram, cur_index);
				if (!zram_writeback_snapshot_matches(zram, cur_index,
								     &item->snapshot)) {
					zram_clear_flag_wake(zram, cur_index,
							     ZRAM_UNDER_WB);
					zram_clear_flag(zram, cur_index, ZRAM_IDLE);
					zram_slot_unlock(zram, cur_index);
					crystal_sddc_requeue_observation(zram,
						cur_index);
					zram_writeback_abort_native(zram, item);
					item->handle = 0;
					continue;
				}
				zram_slot_unlock(zram, cur_index);

				store_map[store_count] = j;
				store_items[store_count].handle = item->handle;
				store_items[store_count].src = item->data;
				store_items[store_count].size = item->size;
				store_items[store_count].ret = -EIO;
				store_count++;
			}

			if (!store_count) {
				i = group_end;
				continue;
			}

			err = zms_store_batch(zram->zms, store_items, store_count,
					      GFP_NOIO, &io);
			zram_record_zms_io(zram, items[store_map[0]].index, &io);
			if (err && !ret)
				ret = err;
			for (j = 0; j < store_count; j++) {
				struct zram_wb_item *item = &items[store_map[j]];
				u32 cur_index = item->index;
				unsigned long handle = item->handle;
				u64 wb_memcg_id;
				u32 prio;
				bool huge;
				bool incompressible;

				if (store_items[j].ret) {
					if (!ret)
						ret = store_items[j].ret;
					zram_writeback_abort_native(zram, item);
					zram_writeback_clear_under_wb(zram, cur_index);
					item->handle = 0;
					chs_log_ratelimited(CHS_LOG_ERR,
							    "writeback zms batch store error mode=0x%x index=%u handle=%lu size=%zu ret=%d\n",
						    mode, cur_index, handle, item->size,
						    store_items[j].ret);
					if (store_items[j].ret == -ENOSPC)
						stop_writeback = true;
					continue;
				}

				zram_slot_lock(zram, cur_index);
				if (!zram_writeback_snapshot_matches(zram, cur_index,
								     &item->snapshot)) {
					zram_clear_flag_wake(zram, cur_index,
							     ZRAM_UNDER_WB);
					zram_clear_flag(zram, cur_index, ZRAM_IDLE);
					zram_slot_unlock(zram, cur_index);
					crystal_sddc_requeue_observation(zram,
						cur_index);
					zms_free(zram->zms, item->handle);
					zram_writeback_abort_native(zram, item);
					item->handle = 0;
					continue;
				}

				wb_memcg_id = zram->table[cur_index].memcg_id;
				prio = zram_get_priority(zram, cur_index);
				huge = zram_test_flag(zram, cur_index, ZRAM_HUGE) ||
					(item->snapshot.sddc.kind != CRYSTAL_SDDC_NONE &&
					 item->size == PAGE_SIZE);
				incompressible = zram_test_flag(zram, cur_index,
								ZRAM_INCOMPRESSIBLE);
				if (item->native_sddc &&
				    !crystal_sddc_native_wb_install_locked(zram,
						cur_index, &item->snapshot.sddc,
						&item->native)) {
					zram_slot_unlock(zram, cur_index);
					zms_free(zram->zms, item->handle);
					zram_writeback_abort_native(zram, item);
					zram_writeback_clear_under_wb(zram, cur_index);
					item->handle = 0;
					if (!ret)
						ret = -EAGAIN;
					continue;
				}
				zram_reclaim_prefetched_locked(zram, cur_index);
				zram_free_page(zram, cur_index,
					       CRYSTAL_SDDC_RELEASE_WRITEBACK);
				if (item->native_sddc)
					crystal_sddc_native_wb_finalize_locked(zram,
						cur_index);
				zram_clear_flag_wake(zram, cur_index, ZRAM_UNDER_WB);
				zram_set_flag(zram, cur_index, ZRAM_WB);
				if (huge) {
					zram_set_flag(zram, cur_index, ZRAM_HUGE);
					atomic64_inc(&zram->stats.huge_pages);
				}
				if (incompressible)
					zram_set_flag(zram, cur_index,
						      ZRAM_INCOMPRESSIBLE);
				zram_set_element(zram, cur_index, item->handle);
				zram_set_obj_size(zram, cur_index, item->size);
				zram_set_priority(zram, cur_index, prio);
				zram->table[cur_index].memcg_id = wb_memcg_id;
				item->handle = 0;
				written++;
				atomic64_inc(&zram->stats.pages_stored);
				atomic64_inc(&zram->stats.bd_count);
				atomic64_inc(&zram->stats.bd_writes);
				atomic64_add(item->size,
					     &zram->stats.bd_compr_data_size);
				zram_memcg_stats_add_current(zram, cur_index);
				zram_writeback_finish_native(item);
				spin_lock(&zram->wb_limit_lock);
				if (zram->wb_limit_enable && zram->bd_wb_limit > 0)
					zram->bd_wb_limit -=
						1UL << (PAGE_SHIFT - 12);
				spin_unlock(&zram->wb_limit_lock);
				zram_slot_unlock(zram, cur_index);
			}

			if (stop_writeback) {
				chs_log_ratelimited(CHS_LOG_WARN,
						    "writeback stop zms no space mode=0x%x scanned=%lu eligible=%lu written=%lu ret=%d\n",
					    mode, scanned, eligible, written, ret);
				break;
			}
			i = group_end;
		}

		for (i = 0; i < batch_count; i++) {
			zram_writeback_abort_native(zram, &items[i]);
			if (items[i].handle)
				zram_writeback_clear_under_wb(zram,
							      items[i].index);
			items[i].index = 0;
			items[i].size = 0;
			items[i].handle = 0;
		}

		if (stop_writeback)
			break;
	}

	zram_writeback_free_buffers(items, ZRAM_WB_BATCH_MAX);
release_items:
	kfree(items);
release_init_lock:
	if (cursor_active && scanned)
		zram_memcg_scan_cursor_set(zram, target_cgroup_id, index, device_pages);
	if (written || ret == -ENOSPC)
		zram_zms_schedule_gc(zram);
	up_read(&zram->init_lock);

	if (wb_stats) {
		wb_stats->scanned_pages = scanned;
		wb_stats->eligible_pages = eligible;
		wb_stats->written_pages = written;
		wb_stats->unknown_or_filtered_pages = unknown_or_filtered;
	}

	if (ret)
		chs_log_ratelimited(CHS_LOG_WARN,
				    "writeback finished with error mode=0x%x target_cgroup_id=%llu scan_scope=%s scan_start=%lu scan_next=%lu scanned=%lu eligible=%lu written=%lu unknown_or_filtered=%lu max_pages=%lu ret=%d\n",
				    mode, target_cgroup_id,
				    target_cgroup_id ? "per_memcg_best_effort" : "global",
				    scan_start, index, scanned, eligible, written,
				    unknown_or_filtered,
				    max_pages, ret);
	else if (written)
		chs_log_ratelimited(CHS_LOG_INFO,
				    "writeback success mode=0x%x target_cgroup_id=%llu scan_scope=%s scan_start=%lu scan_next=%lu scanned=%lu eligible=%lu written=%lu unknown_or_filtered=%lu requested=%lu\n",
				    mode, target_cgroup_id,
				    target_cgroup_id ? "per_memcg_best_effort" : "global",
				    scan_start, index, scanned, eligible, written,
				    unknown_or_filtered,
				    max_pages);
	else
		chs_log_ratelimited(CHS_LOG_INFO,
				    "writeback skipped no matching pages mode=0x%x target_cgroup_id=%llu scan_scope=%s scan_start=%lu scan_next=%lu scanned=%lu eligible=%lu unknown_or_filtered=%lu requested=%lu reason=no_matching_pages\n",
				    mode, target_cgroup_id,
				    target_cgroup_id ? "per_memcg_best_effort" : "global",
				    scan_start, index, scanned, eligible,
				    unknown_or_filtered, max_pages);

	if (ret)
		return ret;
	if (!written)
		return 0;
	return written > INT_MAX ? INT_MAX : (int)written;

out_stats:
	if (wb_stats) {
		wb_stats->scanned_pages = scanned;
		wb_stats->eligible_pages = eligible;
		wb_stats->written_pages = written;
		wb_stats->unknown_or_filtered_pages = unknown_or_filtered;
	}

	return ret;
}

int zram_writeback_device_ext(struct device *dev, const char *mode,
					       unsigned long nr_pages,
					       bool auto_req,
		struct crystal_hybridswap_writeback_stats *stats)
{
	struct zram *zram;
	unsigned long index;
	unsigned long scan_pages;
	int wb_mode;
	int ret;

	if (!dev || !mode)
		return -EINVAL;

	if (stats)
		memset(stats, 0, sizeof(*stats));

	zram = dev_to_zram(dev);
	ret = zram_parse_writeback_mode(zram, mode, true, &wb_mode, &index,
					&scan_pages);
	if (ret) {
		chs_log(CHS_LOG_ERR,
			"writeback parse failed mode=%s nr_pages=%lu ret=%d\n",
			mode, nr_pages, ret);
		return ret;
	}

	chs_log_ratelimited(CHS_LOG_DEBUG,
			    "writeback request mode=%s parsed=0x%x index=%lu scan_pages=%lu requested=%lu\n",
			    mode, wb_mode, index, scan_pages, nr_pages);
	ret = zram_writeback_pages(zram, wb_mode, index, scan_pages, nr_pages,
				   0, auto_req, stats);
	if (!ret) {
		chs_log_ratelimited(CHS_LOG_INFO,
				    "writeback no data mode=%s parsed=0x%x requested=%lu scan_pages=%lu reason=no_matching_pages ret=%d\n",
				    mode, wb_mode, nr_pages, scan_pages, -ENODATA);
		return -ENODATA;
	}
	if (ret > 0)
		chs_log_ratelimited(CHS_LOG_INFO,
				    "writeback completed mode=%s requested=%lu written=%d\n",
				    mode, nr_pages, ret);
	return ret;
}

int zram_writeback_device(struct device *dev, const char *mode,
					       unsigned long nr_pages,
		struct crystal_hybridswap_writeback_stats *stats)
{
	return zram_writeback_device_ext(dev, mode, nr_pages, false, stats);
}

int zram_force_writeback_device_ext(struct device *dev, const char *mode,
				       unsigned long nr_pages, u64 target_cgroup_id,
				       bool auto_req,
				       struct crystal_hybridswap_writeback_stats *stats)
{
	struct zram *zram;
	unsigned long index;
	unsigned long scan_pages;
	unsigned long max_pages;
	int wb_mode;
	int ret;

	if (!dev || !mode)
		return -EINVAL;

	zram = dev_to_zram(dev);
	ret = zram_parse_writeback_mode(zram, mode, true, &wb_mode, &index,
					&scan_pages);
	if (ret) {
		chs_log(CHS_LOG_ERR,
			"force writeback parse failed mode=%s target_cgroup_id=%llu ret=%d\n",
			mode, target_cgroup_id, ret);
		return ret;
	}

	/*
	 * Manual per-memcg force swapout passes CHS_FORCE_GLOBAL_SCAN_PAGES as
	 * a best-effort full-scan sentinel.  Automatic per-memcg policy passes a
	 * bounded nr_pages budget; honor it to keep one policy round bounded.
	 */
	max_pages = nr_pages;
	if (target_cgroup_id && nr_pages == CHS_FORCE_GLOBAL_SCAN_PAGES)
		max_pages = 0;
	chs_log_ratelimited(CHS_LOG_DEBUG,
			    "force writeback request mode=%s parsed=0x%x target_cgroup_id=%llu scan_pages=%lu max_pages=%lu\n",
			    mode, wb_mode, target_cgroup_id, scan_pages, max_pages);
	ret = zram_writeback_pages(zram, wb_mode, index, scan_pages, max_pages,
				   target_cgroup_id, auto_req, stats);
	if (!ret)
		return -ENODATA;
	return ret;
}

int zram_force_writeback_device(struct device *dev, const char *mode,
				       unsigned long nr_pages, u64 target_cgroup_id,
				       struct crystal_hybridswap_writeback_stats *stats)
{
	return zram_force_writeback_device_ext(dev, mode, nr_pages,
					       target_cgroup_id, false, stats);
}

int crystal_hybridswap_zram_pressure_snapshot(struct device *dev,
		struct crystal_hybridswap_zram_pressure *snapshot)
{
	struct zram *zram;
	u64 stored;
	u64 same;
	u64 writeback = 0;
	u64 resident;
	u64 total;

	if (!dev || !snapshot)
		return -EINVAL;

	memset(snapshot, 0, sizeof(*snapshot));
	zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		up_read(&zram->init_lock);
		return -ENODEV;
	}

	stored = atomic64_read(&zram->stats.pages_stored);
	same = atomic64_read(&zram->stats.same_pages);
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
	writeback = atomic64_read(&zram->stats.bd_count);
	snapshot->backing_dev = zram->backing_dev != NULL;
	spin_lock(&zram->wb_limit_lock);
	snapshot->wb_limit_enabled = zram->wb_limit_enable;
	snapshot->wb_limit_pages = zram->bd_wb_limit;
	snapshot->wb_limit_exhausted = zram->wb_limit_enable &&
		!zram->bd_wb_limit;
	spin_unlock(&zram->wb_limit_lock);
#endif
	total = zram->disksize >> PAGE_SHIFT;
	snapshot->device_id = zram_dev_id(zram);
	up_read(&zram->init_lock);

	resident = stored;
	if (resident > writeback)
		resident -= writeback;
	else
		resident = 0;
	if (resident > same)
		resident -= same;
	else
		resident = 0;

	snapshot->stored_pages = stored;
	snapshot->same_pages = same;
	snapshot->writeback_pages = writeback;
	snapshot->resident_pages = resident;
	snapshot->total_pages = total;
	snapshot->resident_ratio = crystal_hybridswap_u64_percent(resident, total);
	snapshot->valid = total > 0;

	return snapshot->valid ? 0 : -ENODATA;
}

int crystal_hybridswap_zram_io_stats(struct device *dev,
		struct crystal_hybridswap_zram_io_stats *stats)
{
	struct zram *zram;

	if (!dev || !stats)
		return -EINVAL;

	memset(stats, 0, sizeof(*stats));
	zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		up_read(&zram->init_lock);
		return -ENODEV;
	}

	stats->devices_count = 1;
	stats->last_device_index = zram_dev_id(zram);
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
	{
		struct zms_stats zms_stats;

		memset(&zms_stats, 0, sizeof(zms_stats));
		if (zram->zms && !zms_get_stats(zram->zms, &zms_stats)) {
			u64 physical_write_bytes;

			physical_write_bytes =
				zram_pages_to_bytes_u64(zms_stats.physical_write_pages);
			stats->bd_physical_read_pages =
				zms_stats.physical_read_pages;
			stats->bd_physical_read_ios_count =
				zms_stats.physical_read_ios;
			stats->bd_physical_read_failed_pages =
				zms_stats.physical_read_failed_pages;
			stats->bd_physical_write_pages =
				zms_stats.physical_write_pages;
			stats->bd_physical_write_bytes =
				physical_write_bytes;
			stats->bd_physical_write_ios_count =
				zms_stats.physical_write_ios;
			stats->bd_physical_write_failed_pages =
				zms_stats.physical_write_failed_pages;
		}
	}
	stats->bd_pages = atomic64_read(&zram->stats.bd_count);
	stats->bd_compressed_bytes =
		atomic64_read(&zram->stats.bd_compr_data_size);
	stats->bd_read_pages = atomic64_read(&zram->stats.bd_reads);
	stats->bd_write_pages = atomic64_read(&zram->stats.bd_writes);
	stats->bd_read_sync_ios_count =
		atomic64_read(&zram->stats.bd_read_sync_ios);
	stats->bd_read_async_ios_count =
		atomic64_read(&zram->stats.bd_read_async_ios);
	stats->bd_read_failures_count =
		atomic64_read(&zram->stats.bd_read_failures);
	stats->bd_read_last_ret = atomic64_read(&zram->stats.bd_read_last_ret);
	stats->bd_read_total_ns = atomic64_read(&zram->stats.bd_read_total_ns);
	stats->bd_read_max_ns = atomic64_read(&zram->stats.bd_read_max_ns);
	stats->bd_read_slow_ios_count =
		atomic64_read(&zram->stats.bd_read_slow_ios);
	stats->bd_read_last_slow_device_index =
		atomic64_read(&zram->stats.bd_read_last_slow_dev);
	stats->bd_read_last_slow_sector_index =
		atomic64_read(&zram->stats.bd_read_last_slow_sector);
	stats->bd_read_last_slow_slot_index =
		atomic64_read(&zram->stats.bd_read_last_slow_index);
	stats->bd_read_last_slow_ret =
		atomic64_read(&zram->stats.bd_read_last_slow_ret);
	stats->bd_read_last_slow_ns =
		atomic64_read(&zram->stats.bd_read_last_slow_ns);
	stats->bd_write_ios_count = atomic64_read(&zram->stats.bd_write_ios);
	stats->bd_write_failures_count =
		atomic64_read(&zram->stats.bd_write_failures);
	stats->bd_write_last_ret = atomic64_read(&zram->stats.bd_write_last_ret);
	stats->bd_write_total_ns = atomic64_read(&zram->stats.bd_write_total_ns);
	stats->bd_write_max_ns = atomic64_read(&zram->stats.bd_write_max_ns);
	stats->bd_write_slow_ios_count =
		atomic64_read(&zram->stats.bd_write_slow_ios);
	stats->bd_write_last_slow_device_index =
		atomic64_read(&zram->stats.bd_write_last_slow_dev);
	stats->bd_write_last_slow_sector_index =
		atomic64_read(&zram->stats.bd_write_last_slow_sector);
	stats->bd_write_last_slow_slot_index =
		atomic64_read(&zram->stats.bd_write_last_slow_index);
	stats->bd_write_last_slow_ret =
		atomic64_read(&zram->stats.bd_write_last_slow_ret);
	stats->bd_write_last_slow_ns =
		atomic64_read(&zram->stats.bd_write_last_slow_ns);
	stats->batchin_runs_count = atomic64_read(&zram->stats.batchin_runs);
	stats->batchin_pages = atomic64_read(&zram->stats.batchin_pages);
	stats->batchin_failures_count =
		atomic64_read(&zram->stats.batchin_failures);
	stats->batchin_no_data_count =
		atomic64_read(&zram->stats.batchin_no_data);
	stats->batchin_filtered_pages =
		atomic64_read(&zram->stats.batchin_filtered_pages);
	stats->batchin_read_errors_count =
		atomic64_read(&zram->stats.batchin_read_errors);
	stats->batchin_prepare_errors_count =
		atomic64_read(&zram->stats.batchin_prepare_errors);
	stats->batchin_snapshot_mismatch_pages =
		atomic64_read(&zram->stats.batchin_snapshot_mismatch);
	stats->batchin_total_ns = atomic64_read(&zram->stats.batchin_total_ns);
	stats->batchin_max_ns = atomic64_read(&zram->stats.batchin_max_ns);
	stats->batchin_slow_runs_count =
		atomic64_read(&zram->stats.batchin_slow_runs);
	stats->batchin_last_ret = atomic64_read(&zram->stats.batchin_last_ret);
	stats->batchin_zms_batches =
		atomic64_read(&zram->stats.batchin_zms_batches);
	stats->batchin_zms_items =
		atomic64_read(&zram->stats.batchin_zms_items);
	stats->batchin_zms_read_ios =
		atomic64_read(&zram->stats.batchin_zms_read_ios);
	stats->prefetch_runs = atomic64_read(&zram->stats.prefetch_runs);
	stats->prefetch_candidates =
		atomic64_read(&zram->stats.prefetch_candidates);
	stats->prefetch_submitted =
		atomic64_read(&zram->stats.prefetch_submitted);
	stats->prefetch_moved = atomic64_read(&zram->stats.prefetch_moved);
	stats->prefetch_skipped =
		atomic64_read(&zram->stats.prefetch_skipped);
	stats->prefetch_read_errors =
		atomic64_read(&zram->stats.prefetch_read_errors);
	stats->prefetch_prepare_errors =
		atomic64_read(&zram->stats.prefetch_prepare_errors);
	stats->prefetch_snapshot_mismatch =
		atomic64_read(&zram->stats.prefetch_snapshot_mismatch);
	stats->prefetch_no_data =
		atomic64_read(&zram->stats.prefetch_no_data);
	stats->prefetch_alloc_failures =
		atomic64_read(&zram->stats.prefetch_alloc_failures);
	stats->prefetch_cached_attempts =
		atomic64_read(&zram->stats.prefetch_cached_attempts);
	stats->prefetch_cached_hits =
		atomic64_read(&zram->stats.prefetch_cached_hits);
	stats->prefetch_cached_misses =
		atomic64_read(&zram->stats.prefetch_cached_misses);
	stats->prefetch_safe_limited =
		atomic64_read(&zram->stats.prefetch_safe_limited);
	stats->prefetch_promote_bytes =
		atomic64_read(&zram->stats.prefetch_promote_bytes);
	stats->prefetch_cookie_mismatch =
		atomic64_read(&zram->stats.prefetch_cookie_mismatch);
	stats->prefetch_payload_buffer_failures =
		atomic64_read(&zram->stats.prefetch_payload_buffer_failures);
	stats->prefetch_promote_alloc_failures =
		atomic64_read(&zram->stats.prefetch_promote_alloc_failures);
	stats->prefetch_zms_read_ios =
		atomic64_read(&zram->stats.prefetch_zms_read_ios);
	stats->prefetch_hits = atomic64_read(&zram->stats.prefetch_hits);
	stats->prefetch_stale_hits =
		atomic64_read(&zram->stats.prefetch_stale_hits);
	stats->prefetch_expired = atomic64_read(&zram->stats.prefetch_expired);
	stats->prefetch_reclaimed =
		atomic64_read(&zram->stats.prefetch_reclaimed);
	stats->prefetch_invalidated =
		atomic64_read(&zram->stats.prefetch_invalidated);
	stats->prefetch_wasted = stats->prefetch_reclaimed +
		stats->prefetch_invalidated;
	stats->prefetch_hit_pct = stats->prefetch_moved ?
		mul_u64_u64_div_u64(stats->prefetch_hits, 100,
				    stats->prefetch_moved) : 0;
	stats->prefetch_waste_pct = stats->prefetch_moved ?
		mul_u64_u64_div_u64(stats->prefetch_wasted, 100,
				    stats->prefetch_moved) : 0;
#endif
	up_read(&zram->init_lock);

	return 0;
}

int crystal_hybridswap_zram_memcg_stats(struct device *dev, u64 cgroup_id,
		struct crystal_hybridswap_memcg_zram_stats *stats)
{
	struct zram_memcg_stats_entry *entry;
	struct zram *zram;
	unsigned long flags;

	if (!dev || !stats || !cgroup_id)
		return -EINVAL;

	memset(stats, 0, sizeof(*stats));
	zram = dev_to_zram(dev);
	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		up_read(&zram->init_lock);
		return -ENODEV;
	}

	spin_lock_irqsave(&zram->memcg_stats_lock, flags);
	entry = zram_memcg_stats_find_locked(zram, cgroup_id);
	if (entry) {
		stats->resident_pages = atomic64_read(&entry->resident_pages);
		stats->writeback_pages = atomic64_read(&entry->writeback_pages);
		stats->same_pages = atomic64_read(&entry->same_pages);
		stats->huge_pages = atomic64_read(&entry->huge_pages);
		stats->zram_compressed_size =
			atomic64_read(&entry->zram_compressed_size);
		stats->zram_original_size =
			atomic64_read(&entry->zram_original_size);
		stats->writeback_size = atomic64_read(&entry->writeback_size);
		stats->writeback_original_size =
			atomic64_read(&entry->writeback_original_size);
	}
	spin_unlock_irqrestore(&zram->memcg_stats_lock, flags);
	up_read(&zram->init_lock);

	stats->matched_pages = stats->resident_pages + stats->writeback_pages;
	return stats->matched_pages ? 0 : -ENODATA;
}

static ssize_t writeback_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	unsigned long nr_pages;
	unsigned long index;
	int mode;
	int ret;

	ret = zram_parse_writeback_mode(zram, buf, false, &mode, &index, &nr_pages);
	if (ret)
		return ret;

	ret = zram_writeback_pages(zram, mode, index, nr_pages, 0, 0, false,
				   NULL);
	return ret < 0 ? ret : len;
}

struct zram_work {
	struct work_struct work;
	struct zram *zram;
	unsigned long handle;
	size_t size;
	u64 memcg_id;
	u32 index;
	u32 prio;
	struct crystal_sddc_snapshot sddc_snapshot;
	struct crystal_sddc_wb_ref *sddc_wb_ref;
	bool native_sddc;
	struct page *page;
	int error;
};

static int zram_decode_zms_payload(struct zram *zram, struct page *page,
				   const void *buf, size_t stored_size,
				   size_t expected_size, u32 prio)
{
	struct zcomp_strm *zstrm;
	void *dst;
	int ret;

	if (!page || !buf || !stored_size || stored_size > PAGE_SIZE ||
	    stored_size != expected_size)
		return -EIO;

	if (stored_size == PAGE_SIZE) {
		dst = kmap_atomic(page);
		copy_page(dst, buf);
		kunmap_atomic(dst);
		return 0;
	}

	zstrm = zcomp_stream_get(zram->comps[prio]);
	dst = kmap_atomic(page);
	ret = zcomp_decompress(zstrm, buf, stored_size, dst);
	kunmap_atomic(dst);
	zcomp_stream_put(zram->comps[prio]);

	return ret;
}

static int zram_finish_zms_ref_read(struct zram *zram, struct page *page,
				    struct zms_load_ref *ref,
				    const struct zms_io *io,
				    size_t size, u32 prio,
				    u32 index, u64 memcg_id,
				    const struct crystal_sddc_snapshot *sddc_snapshot,
				    struct crystal_sddc_wb_ref *sddc_wb_ref)
{
	int ret;

	zram_record_zms_io(zram, index, io);
	if (ref->size != size)
		ret = -EIO;
	else if (sddc_snapshot &&
		 crystal_sddc_native_wb_kind(sddc_snapshot->kind)) {
		if (sddc_wb_ref)
			ret = crystal_sddc_native_wb_restore_pinned(zram, page,
					sddc_snapshot, sddc_wb_ref,
					ref->data, ref->size);
		else
			ret = crystal_sddc_native_wb_restore_page(zram, page,
					sddc_snapshot, ref->data, ref->size);
	} else
		ret = zram_decode_zms_payload(zram, page, ref->data, ref->size,
				      size, prio);
	if (!ret && (!sddc_snapshot ||
			!crystal_sddc_native_wb_kind(sddc_snapshot->kind)))
		zram_zms_maybe_prefetch(zram, index, ref, memcg_id, io);
	zms_put_ref(zram->zms, ref);

	if (!ret)
		atomic64_inc(&zram->stats.bd_reads);
	if (!ret)
		zram_prefetch_note_fault(zram, index, memcg_id);
	return ret;
}

static int zram_read_from_zms(struct zram *zram, struct page *page,
				      unsigned long handle, size_t size, u32 prio,
				      u32 index, u64 memcg_id,
				      const struct crystal_sddc_snapshot *sddc_snapshot,
				      struct crystal_sddc_wb_ref *sddc_wb_ref)
{
	struct zms_load_ref ref;
	struct zms_io io;
	int ret;

	if (!zram->zms || !handle || !size || size > PAGE_SIZE)
		return -EIO;

	ret = zms_load_ref(zram->zms, handle, &ref, GFP_NOIO, &io);
	if (ret) {
		zram_record_zms_io(zram, index, &io);
		return ret;
	}

	return zram_finish_zms_ref_read(zram, page, &ref, &io, size,
				       prio, index, memcg_id, sddc_snapshot,
				       sddc_wb_ref);
}

static void zram_zms_sync_read(struct work_struct *work)
{
	struct zram_work *zw = container_of(work, struct zram_work, work);

	zw->error = zram_read_from_zms(zw->zram, zw->page, zw->handle,
				       zw->size, zw->prio, zw->index,
				       zw->memcg_id,
				       zw->native_sddc ? &zw->sddc_snapshot : NULL,
				       zw->sddc_wb_ref);
}

/*
 * Block layer wants one ->submit_bio to be active at a time. ZMS may need a
 * backing read, so keep the old zram rule and perform WBed reads from a worker.
 */
static int read_from_zms_sync(struct zram *zram, struct page *page,
			      unsigned long handle, size_t size, u32 prio,
			      u32 index, u64 memcg_id,
			      const struct crystal_sddc_snapshot *sddc_snapshot,
			      struct crystal_sddc_wb_ref *sddc_wb_ref)
{
	struct zram_work work;

	work.page = page;
	work.zram = zram;
	work.handle = handle;
	work.size = size;
	work.prio = prio;
	work.index = index;
	work.memcg_id = memcg_id;
	work.native_sddc = sddc_snapshot &&
		crystal_sddc_native_wb_kind(sddc_snapshot->kind);
	if (work.native_sddc)
		work.sddc_snapshot = *sddc_snapshot;
	work.sddc_wb_ref = sddc_wb_ref;

	INIT_WORK_ONSTACK(&work.work, zram_zms_sync_read);
	queue_work(zram_read_wq, &work.work);
	flush_work(&work.work);
	destroy_work_on_stack(&work.work);
	return work.error;
}

#else
static inline void reset_bdev(struct zram *zram) {};
static int read_from_zms_sync(struct zram *zram, struct page *page,
			      unsigned long handle, size_t size, u32 prio,
			      u32 index, u64 memcg_id,
			      const struct crystal_sddc_snapshot *sddc_snapshot,
			      struct crystal_sddc_wb_ref *sddc_wb_ref)
{
	(void)sddc_snapshot;
	(void)sddc_wb_ref;
	return -EIO;
}
#endif

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_MEMORY_TRACKING

static struct dentry *zram_debugfs_root;

static void zram_debugfs_create(void)
{
	zram_debugfs_root = debugfs_create_dir("zram", NULL);
}

static void zram_debugfs_destroy(void)
{
	debugfs_remove_recursive(zram_debugfs_root);
}

static int zram_debugfs_block_state_open(struct inode *inode,
		struct file *file)
{
	struct zram *zram = inode->i_private;

	if (!zram_try_get(zram))
		return -ENODEV;
	file->private_data = zram;
	return 0;
}

static int zram_debugfs_block_state_release(struct inode *inode,
		struct file *file)
{
	zram_put(file->private_data);
	file->private_data = NULL;
	return 0;
}

static ssize_t read_block_state(struct file *file, char __user *buf,
			size_t count, loff_t *ppos)
{
	char *kbuf;
	ssize_t written = 0;
	struct zram *zram = file->private_data;
	unsigned long nr_pages, index;
	loff_t pos;
	struct timespec64 ts;
	const size_t entry_size = 64;
	size_t out_size;

	if (!count)
		return 0;
	if (!zram)
		return -ENODEV;

	pos = *ppos;
	if (pos < 0)
		return -EINVAL;

	out_size = min_t(size_t, count, PAGE_SIZE);
	kbuf = kvmalloc(out_size, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		up_read(&zram->init_lock);
		kvfree(kbuf);
		return -EINVAL;
	}
	nr_pages = zram_pages_snapshot(zram);
	if ((u64)pos >= (u64)nr_pages)
		goto out_unlock;

	for (index = (unsigned long)pos; index < nr_pages; index++) {
		int copied;

		if (out_size - written < entry_size)
			break;

		zram_slot_lock(zram, index);
		if (!zram_allocated(zram, index))
			goto next;

		ts = ktime_to_timespec64(zram->table[index].ac_time);
		copied = scnprintf(kbuf + written, out_size - written,
			"%12lu %12lld.%06lu %c%c%c%c%c%c\n",
			index, (s64)ts.tv_sec,
			ts.tv_nsec / NSEC_PER_USEC,
			zram_test_flag(zram, index, ZRAM_SAME) ? 's' : '.',
			zram_test_flag(zram, index, ZRAM_WB) ? 'w' : '.',
			zram_test_flag(zram, index, ZRAM_HUGE) ? 'h' : '.',
			zram_test_flag(zram, index, ZRAM_IDLE) ? 'i' : '.',
			zram_get_priority(zram, index) ? 'r' : '.',
			zram_test_flag(zram, index,
				       ZRAM_INCOMPRESSIBLE) ? 'n' : '.');

		if (copied <= 0 || copied >= out_size - written) {
			zram_slot_unlock(zram, index);
			break;
		}
		written += copied;
next:
		zram_slot_unlock(zram, index);
		*ppos = (loff_t)index + 1;
	}

out_unlock:
	up_read(&zram->init_lock);
	if (copy_to_user(buf, kbuf, written))
		written = -EFAULT;
	kvfree(kbuf);

	return written;
}

static const struct file_operations proc_zram_block_state_op = {
	.open = zram_debugfs_block_state_open,
	.read = read_block_state,
	.release = zram_debugfs_block_state_release,
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

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_EMPTY_APIS
/*
 * zram ABI compatibility-only no-op.  It reports the current per-cpu stream
 * count and accepts writes without changing compressor stream configuration.
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
#endif

static void comp_algorithm_set(struct zram *zram, u32 prio, const char *alg)
{
	/* Do not free statically defined compression algorithms */
	if (zram->comp_algs[prio] != default_compressor)
		kfree(zram->comp_algs[prio]);

	zram->comp_algs[prio] = alg;
}

static ssize_t __comp_algorithm_show(struct zram *zram, u32 prio, char *buf)
{
	ssize_t sz;

	down_read(&zram->init_lock);
	sz = zcomp_available_show(zram->comp_algs[prio], buf);
	up_read(&zram->init_lock);

	return sz;
}

static int __comp_algorithm_store(struct zram *zram, u32 prio, const char *buf)
{
	char *compressor;
	size_t sz;

	sz = strlen(buf);
	if (sz >= CRYPTO_MAX_ALG_NAME)
		return -E2BIG;

	compressor = kstrdup(buf, GFP_KERNEL);
	if (!compressor)
		return -ENOMEM;

	/* ignore trailing newline */
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

	comp_algorithm_set(zram, prio, compressor);
	up_write(&zram->init_lock);
	return 0;
}

static ssize_t comp_algorithm_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return __comp_algorithm_show(zram, ZRAM_PRIMARY_COMP, buf);
}

static ssize_t comp_algorithm_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf,
				    size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	int ret;

	ret = __comp_algorithm_store(zram, ZRAM_PRIMARY_COMP, buf);
	return ret ? ret : len;
}

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_MULTI_COMP
static ssize_t recomp_algorithm_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t sz = 0;
	u32 prio;

	for (prio = ZRAM_SECONDARY_COMP; prio < ZRAM_MAX_COMPS; prio++) {
		if (!zram->comp_algs[prio])
			continue;

		sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2, "#%d: ", prio);
		sz += __comp_algorithm_show(zram, prio, buf + sz);
	}

	return sz;
}

static ssize_t recomp_algorithm_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	int prio = ZRAM_SECONDARY_COMP;
	char *args, *param, *val;
	char *alg = NULL;
	int ret;

	args = skip_spaces(buf);
	while (*args) {
		args = next_arg(args, &param, &val);

		if (!val || !*val)
			return -EINVAL;

		if (!strcmp(param, "algo")) {
			alg = val;
			continue;
		}

		if (!strcmp(param, "priority")) {
			ret = kstrtoint(val, 10, &prio);
			if (ret)
				return ret;
			continue;
		}
	}

	if (!alg)
		return -EINVAL;

	if (prio < ZRAM_SECONDARY_COMP || prio >= ZRAM_MAX_COMPS)
		return -EINVAL;

	ret = __comp_algorithm_store(zram, prio, alg);
	return ret ? ret : len;
}
#endif

static ssize_t compact_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t ret = len;

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		up_read(&zram->init_lock);
		return -EINVAL;
	}

	zs_compact(zram->mem_pool);
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
	if (zram->zms && !crystal_hybridswap_system_sleeping()) {
		struct zms_io io;
		int err;

		do {
			err = zms_compact(zram->zms, GFP_NOIO, &io);
			zram_record_zms_io(zram, 0, &io);
		} while (err == -EAGAIN);
		if (err)
			ret = err;
	}
#endif
	up_read(&zram->init_lock);

	return ret;
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
			(u64)atomic64_read(&zram->stats.notify_free));
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

	orig_size = atomic64_read(&zram->stats.pages_stored);
	max_used = atomic_long_read(&zram->stats.max_used_pages);

	ret = scnprintf(buf, PAGE_SIZE,
			"%8llu %8llu %8llu %8lu %8ld %8llu %8lu %8llu %8llu\n",
			orig_size << PAGE_SHIFT,
			(u64)atomic64_read(&zram->stats.compr_data_size),
			mem_used << PAGE_SHIFT,
			zram->limit_pages << PAGE_SHIFT,
			max_used << PAGE_SHIFT,
			(u64)atomic64_read(&zram->stats.same_pages),
			atomic_long_read(&pool_stats.pages_compacted),
			(u64)atomic64_read(&zram->stats.huge_pages),
			(u64)atomic64_read(&zram->stats.huge_pages_since));
	up_read(&zram->init_lock);

	return ret;
}

static ssize_t sddc_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct crystal_sddc_stats_snapshot stats;
	struct zram *zram = dev_to_zram(dev);

	crystal_sddc_get_stats(zram, &stats);
	return sysfs_emit(buf,
		"enabled: %u\n"
		"queued: %llu\n"
		"coalesced: %llu\n"
		"dropped: %llu\n"
		"ineligible: %llu\n"
		"shutdown_discarded: %llu\n"
		"worker_runs: %llu\n"
		"pending: %u\n"
		"pending_max: %llu\n"
		"observed: %llu\n"
		"stale: %llu\n"
		"indexed: %llu\n"
		"refs: %llu\n"
		"ref_bytes: %llu\n"
		"wb_deltas: %llu\n"
		"wb_delta_bytes: %llu\n"
		"wb_aliases: %llu\n"
		"wb_alias_bytes: %llu\n"
		"aliases: %llu\n"
		"deltas: %llu\n"
		"delta_bytes: %llu\n"
		"alias_attempts: %llu\n"
		"alias_hits: %llu\n"
		"delta_attempts: %llu\n"
		"delta_hits: %llu\n"
		"delta_matches: %llu\n"
		"delta_small_rejects: %llu\n"
		"delta_no_gain: %llu\n"
		"delta_match_bytes_max: %llu\n"
		"saved_bytes: %llu\n"
		"saved_bytes_total: %llu\n"
		"released_aliases: %llu\n"
		"released_deltas: %llu\n"
		"released_saved_bytes: %llu\n"
		"released_writeback_saved_bytes: %llu\n"
		"released_rewrite_saved_bytes: %llu\n"
		"released_recompress_saved_bytes: %llu\n"
		"released_discard_saved_bytes: %llu\n"
		"released_notify_free_saved_bytes: %llu\n"
		"released_reset_saved_bytes: %llu\n"
		"released_other_saved_bytes: %llu\n"
		"conversion_failures: %llu\n"
		"decode_failures: %llu\n"
		"flatten_failures: %llu\n"
		"limit_rejects: %llu\n",
		stats.enabled, stats.queued, stats.coalesced, stats.dropped,
		stats.ineligible, stats.shutdown_discarded, stats.worker_runs,
		stats.pending, stats.pending_max, stats.observed, stats.stale,
		stats.indexed,
		stats.refs, stats.ref_bytes, stats.wb_deltas,
		stats.wb_delta_bytes, stats.wb_aliases,
		stats.wb_alias_bytes, stats.aliases, stats.deltas,
		stats.delta_bytes, stats.alias_attempts, stats.alias_hits,
		stats.delta_attempts, stats.delta_hits, stats.delta_matches,
		stats.delta_small_rejects, stats.delta_no_gain,
		stats.delta_match_bytes_max, stats.saved_bytes,
		stats.saved_bytes_total, stats.released_aliases,
		stats.released_deltas, stats.released_saved_bytes,
		stats.released_writeback_saved_bytes,
		stats.released_rewrite_saved_bytes,
		stats.released_recompress_saved_bytes,
		stats.released_discard_saved_bytes,
		stats.released_notify_free_saved_bytes,
		stats.released_reset_saved_bytes,
		stats.released_other_saved_bytes, stats.conversion_failures,
		stats.decode_failures, stats.flatten_failures,
		stats.limit_rejects);
}

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
#define FOUR_K(x) ((x) * (1 << (PAGE_SHIFT - 12)))
static ssize_t bd_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	struct zms_stats stats = {};
	ssize_t ret;

	down_read(&zram->init_lock);
	if (init_done(zram) && zram->zms)
		zms_get_stats(zram->zms, &stats);
	ret = scnprintf(buf, PAGE_SIZE, "%8llu %8llu %8llu\n",
			FOUR_K((u64)stats.used_blocks),
			FOUR_K(stats.bd_stat_read_pages),
			FOUR_K(stats.bd_stat_write_pages));
	up_read(&zram->init_lock);

	return ret;
}

static ssize_t zms_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	struct zms_stats stats;
	u64 physical_write_bytes;
	ssize_t ret;
	int err = 0;

	down_read(&zram->init_lock);
	if (!init_done(zram) || !zram->zms) {
		ret = sysfs_emit(buf,
			"enabled: 0\n"
			"ret: 0\n");
		goto out;
	}

	err = zms_get_stats(zram->zms, &stats);
	if (err) {
		ret = sysfs_emit(buf,
			"enabled: 1\n"
			"ret: %d\n", err);
		goto out;
	}

	physical_write_bytes =
		zram_pages_to_bytes_u64(stats.physical_write_pages);

	ret = sysfs_emit(buf,
		"enabled: 1\n"
		"ret: 0\n"
		"nr_blocks: %lu\n"
		"used_blocks: %lu\n"
		"free_blocks: %lu\n"
		"nr_handles: %lu\n"
		"objects: %lu\n"
		"stored_bytes: %llu\n"
		"packed_bytes: %llu\n"
		"physical_read_pages: %llu\n"
		"physical_read_ios: %llu\n"
		"physical_read_failed_pages: %llu\n"
		"physical_write_pages: %llu\n"
		"physical_write_bytes: %llu\n"
		"physical_write_ios: %llu\n"
		"physical_write_failed_pages: %llu\n"
		"bd_stat_read_pages: %llu\n"
		"bd_stat_write_pages: %llu\n"
		"partial_blocks: %lu\n"
		"low_blocks: %lu\n"
		"mid_blocks: %lu\n"
		"almost_full_blocks: %lu\n"
		"full_blocks: %lu\n"
		"dirty_blocks: %lu\n"
		"dirty_pages: %lu\n"
		"read_merge_waits: %lu\n"
		"read_merge_wakeups: %lu\n"
		"read_merge_hits: %lu\n"
		"read_merge_mismatch: %lu\n"
		"read_merge_failures: %lu\n"
		"alloc_blocks: %lu\n"
		"alloc_run_successes: %lu\n"
		"alloc_run_failures: %lu\n"
		"alloc_run_success_pct: %lu\n"
		"alloc_run_success_pages: %lu\n"
		"alloc_run_partial_pages: %lu\n"
		"alloc_run_fallback_pages: %lu\n"
		"alloc_run_contiguous_page_pct: %lu\n"
		"alloc_run_segments: %lu\n"
		"alloc_run_avg_segment_pages: %lu\n"
		"reclaim_before_alloc_calls: %lu\n"
		"reclaim_before_alloc_handles: %lu\n"
		"load_resident_hits: %lu\n"
		"load_disk_misses: %lu\n"
		"load_resident_hit_pct: %lu\n"
		"store_batch_calls: %lu\n"
		"store_batch_items: %lu\n"
		"store_batch_resident_reuses: %lu\n"
		"store_batch_new_blocks: %lu\n"
		"pending_free: %lu\n"
		"empty_blocks: %lu\n"
		"valid_classes: %lu\n",
		stats.nr_blocks,
		stats.used_blocks,
		stats.free_blocks,
		stats.nr_handles,
		stats.objects,
		(unsigned long long)stats.stored_bytes,
		(unsigned long long)stats.packed_bytes,
		(unsigned long long)stats.physical_read_pages,
		(unsigned long long)stats.physical_read_ios,
		(unsigned long long)stats.physical_read_failed_pages,
		(unsigned long long)stats.physical_write_pages,
		(unsigned long long)physical_write_bytes,
		(unsigned long long)stats.physical_write_ios,
		(unsigned long long)stats.physical_write_failed_pages,
		(unsigned long long)stats.bd_stat_read_pages,
		(unsigned long long)stats.bd_stat_write_pages,
		stats.partial_blocks,
		stats.low_blocks,
		stats.mid_blocks,
		stats.almost_full_blocks,
		stats.full_blocks,
		stats.dirty_blocks,
		stats.dirty_pages,
		stats.read_merge_waits,
		stats.read_merge_wakeups,
		stats.read_merge_hits,
		stats.read_merge_mismatch,
		stats.read_merge_failures,
		stats.alloc_blocks,
		stats.alloc_run_successes,
		stats.alloc_run_failures,
		stats.alloc_run_success_pct,
		stats.alloc_run_success_pages,
		stats.alloc_run_partial_pages,
		stats.alloc_run_fallback_pages,
		stats.alloc_run_contiguous_page_pct,
		stats.alloc_run_segments,
		stats.alloc_run_avg_segment_pages,
		stats.reclaim_before_alloc_calls,
		stats.reclaim_before_alloc_handles,
		stats.load_resident_hits,
		stats.load_disk_misses,
		stats.load_resident_hit_pct,
		stats.store_batch_calls,
		stats.store_batch_items,
		stats.store_batch_resident_reuses,
		stats.store_batch_new_blocks,
		stats.pending_free,
		stats.empty_blocks,
		stats.valid_classes);
out:
	up_read(&zram->init_lock);
	return ret;
}

static ssize_t writeback_cold_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t ret;

	down_read(&zram->init_lock);
	ret = sysfs_emit(buf,
		"enabled: %d\n"
		"cold_age_ms: %llu\n"
		"auto_scan_pages: %llu\n"
		"cold_selected: %llu\n"
		"hot_skipped: %llu\n"
		"kind_skipped: %llu\n"
		"idle_selected: %llu\n"
		"age_selected: %llu\n"
		"unknown_age_skipped: %llu\n"
		"last_age_ms: %lld\n"
		"max_age_ms: %lld\n",
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_TRACK_ENTRY_ACTIME
		1,
#else
		0,
#endif
		(u64)atomic64_read(&zram->stats.auto_wb_cold_age_ms),
		(u64)atomic64_read(&zram->stats.auto_wb_scan_pages),
		(u64)atomic64_read(&zram->stats.auto_wb_cold_selected),
		(u64)atomic64_read(&zram->stats.auto_wb_hot_skipped),
		(u64)atomic64_read(&zram->stats.auto_wb_kind_skipped),
		(u64)atomic64_read(&zram->stats.auto_wb_idle_selected),
		(u64)atomic64_read(&zram->stats.auto_wb_age_selected),
		(u64)atomic64_read(&zram->stats.auto_wb_unknown_age_skipped),
		(s64)atomic64_read(&zram->stats.auto_wb_last_age_ms),
		(s64)atomic64_read(&zram->stats.auto_wb_max_age_ms));
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
			"version: %d\n%8llu %8llu\n"
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
			"zms_writeback_compressed_size: %llu\n"
#endif
			,
			version,
			(u64)atomic64_read(&zram->stats.writestall),
			(u64)atomic64_read(&zram->stats.miss_free)
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
			, (u64)atomic64_read(&zram->stats.bd_compr_data_size)
#endif
			);
	up_read(&zram->init_lock);

	return ret;
}

static DEVICE_ATTR_RO(io_stat);
static DEVICE_ATTR_RO(mm_stat);
static DEVICE_ATTR_RO(sddc_stat);
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
static DEVICE_ATTR_RO(bd_stat);
static DEVICE_ATTR_RO(zms_stat);
static DEVICE_ATTR_RO(writeback_cold_stat);
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
		zram_free_page(zram, index, CRYSTAL_SDDC_RELEASE_RESET);
	crystal_sddc_destroy(zram);
	zram_memcg_stats_clear_all(zram);

	zs_destroy_pool(zram->mem_pool);
	vfree(zram->table);
	zram->table = NULL;
}

static bool zram_meta_alloc(struct zram *zram, u64 disksize)
{
	size_t num_pages;

	num_pages = disksize >> PAGE_SHIFT;
	zram->table = vzalloc(array_size(num_pages, sizeof(*zram->table)));
	if (!zram->table)
		return false;

	zram->mem_pool = zs_create_pool(zram->disk->disk_name);
	if (!zram->mem_pool) {
		vfree(zram->table);
		zram->table = NULL;
		return false;
	}

	if (!huge_class_size)
		huge_class_size = zs_huge_class_size(zram->mem_pool);
	return true;
}

/*
 * To protect concurrent access to the same index entry,
 * caller should hold this table index entry's bit_spinlock to
 * indicate this index entry is accessing.
 */
static void zram_free_page(struct zram *zram, size_t index,
			   enum crystal_sddc_release_reason reason)
{
	enum crystal_sddc_kind sddc_kind;
	unsigned long handle;
	size_t size;

	zram_memcg_stats_sub_current(zram, index);
	if (zram_test_flag(zram, index, ZRAM_WB))
		crystal_sddc_native_wb_free_locked(zram, index);
	sddc_kind = crystal_sddc_slot_free_locked(zram, index, reason);
	zram->table[index].memcg_id = 0;
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_TRACK_ENTRY_ACTIME
	zram->table[index].ac_time = 0;
#endif
	if (zram_test_flag(zram, index, ZRAM_IDLE))
		zram_clear_flag(zram, index, ZRAM_IDLE);
	zram_clear_prefetched(zram, index);

	if (zram_test_flag(zram, index, ZRAM_HUGE)) {
		zram_clear_flag(zram, index, ZRAM_HUGE);
		atomic64_dec(&zram->stats.huge_pages);
	}

	if (zram_test_flag(zram, index, ZRAM_INCOMPRESSIBLE))
		zram_clear_flag(zram, index, ZRAM_INCOMPRESSIBLE);

	zram_set_priority(zram, index, 0);

	if (zram_test_flag(zram, index, ZRAM_WB)) {
		handle = zram_get_element(zram, index);
		size = zram_get_obj_size(zram, index);
		zram_clear_flag(zram, index, ZRAM_WB);
		if (zram->zms) {
			zms_free(zram->zms, handle);
			atomic64_dec(&zram->stats.bd_count);
			atomic64_sub(size ? size : PAGE_SIZE,
				     &zram->stats.bd_compr_data_size);
		}
		goto out;
	}

	/*
	 * No memory is allocated for same element filled pages.
	 * Simply clear same page flag.
	 */
	if (zram_test_flag(zram, index, ZRAM_SAME)) {
		zram_clear_flag(zram, index, ZRAM_SAME);
		atomic64_dec(&zram->stats.same_pages);
		goto out;
	}

	handle = zram_get_handle(zram, index);
	if (!handle && sddc_kind == CRYSTAL_SDDC_NONE)
		return;
	if (!handle)
		goto out;

	zs_free(zram->mem_pool, handle);

	atomic64_sub(zram_get_obj_size(zram, index),
			&zram->stats.compr_data_size);
out:
	atomic64_dec(&zram->stats.pages_stored);
	zram_set_handle(zram, index, 0);
	zram_set_obj_size(zram, index, 0);
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
	zram->table[index].prefetch_jiffies = 0;
#endif
	WARN_ON_ONCE(zram->table[index].flags &
		~(1UL << ZRAM_LOCK | 1UL << ZRAM_UNDER_WB |
		  1UL << ZRAM_PREFETCHED));
}

/*
 * Reads (decompresses if needed) a page from zspool (zsmalloc).
 * Corresponding ZRAM slot should be locked.
 */
static int zram_read_from_zspool(struct zram *zram, struct page *page,
				 u32 index)
{
	struct zcomp_strm *zstrm;
	unsigned long handle;
	unsigned int size;
	void *src, *dst;
	u32 prio;
	int ret;

	handle = zram_get_handle(zram, index);
	if (!handle || zram_test_flag(zram, index, ZRAM_SAME)) {
		unsigned long value;
		void *mem;

		value = handle ? zram_get_element(zram, index) : 0;
		mem = kmap_atomic(page);
		zram_fill_page(mem, PAGE_SIZE, value);
		kunmap_atomic(mem);
		return 0;
	}

	size = zram_get_obj_size(zram, index);

	if (size != PAGE_SIZE) {
		prio = zram_get_priority(zram, index);
		zstrm = zcomp_stream_get(zram->comps[prio]);
	}

	src = zs_map_object(zram->mem_pool, handle, ZS_MM_RO);
	if (size == PAGE_SIZE) {
		dst = kmap_atomic(page);
		copy_page(dst, src);
		kunmap_atomic(dst);
		ret = 0;
	} else {
		dst = kmap_atomic(page);
		ret = zcomp_decompress(zstrm, src, size, dst);
		kunmap_atomic(dst);
		zcomp_stream_put(zram->comps[prio]);
	}
	zs_unmap_object(zram->mem_pool, handle);
	return ret;
}

static int zram_read_compressed_page(struct zram *zram, u32 index,
				     void *dst, size_t *size)
{
	unsigned long handle;
	unsigned int obj_size;
	void *src;

	if (!dst || !size)
		return -EINVAL;
	if (zram_test_flag(zram, index, ZRAM_WB) ||
	    zram_test_flag(zram, index, ZRAM_SAME))
		return -EINVAL;

	handle = zram_get_handle(zram, index);
	if (!handle)
		return -ENOENT;

	obj_size = zram_get_obj_size(zram, index);
	if (!obj_size || obj_size > PAGE_SIZE)
		return -EINVAL;

	src = zs_map_object(zram->mem_pool, handle, ZS_MM_RO);
	memcpy(dst, src, obj_size);
	zs_unmap_object(zram->mem_pool, handle);
	*size = obj_size;
	return 0;
}

static int zram_read_page(struct zram *zram, struct page *page, u32 index,
			  struct bio *parent)
{
	unsigned long handle;
	struct zms_load_ref ref;
	struct zms_io io;
	struct zram_wb_snapshot wb_snapshot;
	struct crystal_sddc_wb_ref wb_ref;
	size_t size;
	u64 memcg_id;
	u32 prio;
	bool native_sddc;
	bool native_sddc_pinned;
	bool retry_read;
	int ret;

retry:
	memset(&wb_ref, 0, sizeof(wb_ref));
	native_sddc_pinned = false;
	zram_slot_lock(zram, index);
	if (zram_test_flag(zram, index, ZRAM_UNDER_WB)) {
		zram_slot_unlock(zram, index);
		wait_on_bit_io(&zram->table[index].flags, ZRAM_UNDER_WB,
			       TASK_UNINTERRUPTIBLE);
		goto retry;
	}

	if (!zram_test_flag(zram, index, ZRAM_WB)) {
		zram_consume_prefetched(zram, index);
		if (crystal_sddc_slot_allocated_locked(zram, index)) {
			zram_slot_unlock(zram, index);
			ret = crystal_sddc_read_page(zram, page, index);
			if (ret == -EAGAIN)
				goto retry;
		} else {
			/* Ordinary zspool reads retain the slot lock. */
			ret = zram_read_from_zspool(zram, page, index);
			zram_slot_unlock(zram, index);
		}
	} else {
		/*
		 * The slot should be unlocked before reading from the backing
		 * device. Snapshot the ZMS handle while locked so a concurrent
		 * slot rewrite cannot redirect this read to a different object.
		 */
		retry_read = false;
		zram_set_flag(zram, index, ZRAM_UNDER_WB);
		handle = zram_get_element(zram, index);
		size = zram_get_obj_size(zram, index);
		prio = zram_get_priority(zram, index);
		memcg_id = zram->table[index].memcg_id;
		zram_take_wb_snapshot(zram, index, &wb_snapshot);
		native_sddc = crystal_sddc_native_wb_kind(
				wb_snapshot.sddc.kind);
		if (native_sddc)
			native_sddc_pinned = crystal_sddc_native_wb_pin_locked(
					zram, index, &wb_snapshot.sddc, &wb_ref);
		zram_slot_unlock(zram, index);

		if (native_sddc && !native_sddc_pinned)
			ret = -EAGAIN;
		else if (zram->zms && handle && size && size <= PAGE_SIZE)
			ret = zms_load_cached_ref(zram->zms, handle, &ref, &io);
		else
			ret = -EAGAIN;
		if (!ret) {
			ret = zram_finish_zms_ref_read(zram, page, &ref, &io,
					       size, prio, index,
					       memcg_id,
					       native_sddc ?
					       &wb_snapshot.sddc : NULL,
					       native_sddc_pinned ?
					       &wb_ref : NULL);
		} else if (ret == -EAGAIN) {
			if (native_sddc && !native_sddc_pinned)
				ret = -EAGAIN;
			else
				ret = read_from_zms_sync(zram, page, handle,
							 size, prio, index,
							 memcg_id,
							 native_sddc ?
							 &wb_snapshot.sddc : NULL,
							 native_sddc_pinned ?
							 &wb_ref : NULL);
		}

		zram_slot_lock(zram, index);
		if (native_sddc &&
		    !zram_wb_snapshot_matches(zram, index, &wb_snapshot))
			retry_read = true;
		if (zram_test_flag(zram, index, ZRAM_UNDER_WB))
			zram_clear_flag_wake(zram, index, ZRAM_UNDER_WB);
		zram_slot_unlock(zram, index);
		crystal_sddc_native_wb_put_ref(&wb_ref);
		crystal_sddc_requeue_observation(zram, index);
		if (retry_read)
			goto retry;
	}

	/* Return the bio error without flooding the log with warning stacks. */
	if (unlikely(ret < 0))
		pr_err_ratelimited("read failed err=%d, page=%u\n", ret, index);

	return ret;
}

/*
 * Use a temporary buffer to decompress the page, as the decompressor
 * always expects a full page for the output.
 */
static int zram_bvec_read_partial(struct zram *zram, struct bio_vec *bvec,
				  u32 index, int offset)
{
	struct page *page = alloc_page(GFP_NOIO);
	int ret;

	if (!page)
		return -ENOMEM;
	ret = zram_read_page(zram, page, index, NULL);
	if (likely(!ret))
		memcpy_to_bvec(bvec, page_address(page) + offset);
	__free_page(page);
	return ret;
}

static int zram_bvec_read(struct zram *zram, struct bio_vec *bvec,
			  u32 index, int offset, struct bio *bio)
{
	if (is_partial_io(bvec))
		return zram_bvec_read_partial(zram, bvec, index, offset);
	return zram_read_page(zram, bvec->bv_page, index, bio);
}

struct zram_prepared_page {
	unsigned long handle;
	unsigned long element;
	unsigned int comp_len;
	enum zram_pageflags flags;
	u32 prio;
	bool incompressible;
};

struct zram_batchin_item {
	struct zram_wb_snapshot snapshot;
	struct zram_prepared_page prep;
	struct crystal_sddc_wb_ref native_ref;
	unsigned long index;
	u64 memcg_id;
	u32 prio;
	bool native_sddc;
};

struct zram_prefetch_item {
	struct zram_wb_snapshot snapshot;
	struct zram_prepared_page prep;
	struct zms_object_cookie cookie;
	size_t payload_offset;
	unsigned long index;
	u64 memcg_id;
	u32 prio;
	bool payload_valid;
};

struct zram_zms_prefetch_work {
	struct work_struct work;
	struct zram *zram;
	unsigned int count;
	struct zram_prefetch_item items[ZRAM_ZMS_PREFETCH_MAX];
};

static void zram_batchin_put_native(struct zram_batchin_item *item)
{
	if (!item || !item->native_sddc)
		return;
	crystal_sddc_native_wb_put_ref(&item->native_ref);
	item->native_sddc = false;
}

static void zram_cleanup_prepared_page(struct zram *zram,
					      struct zram_prepared_page *prep)
{
	if (!prep || prep->flags == ZRAM_SAME)
		return;
	if (prep->handle && !IS_ERR_VALUE(prep->handle))
		zs_free(zram->mem_pool, prep->handle);
}

static int zram_prepare_page(struct zram *zram, struct page *page,
				     struct zram_prepared_page *prep)
{
	int ret = 0;
	unsigned long alloced_pages;
	unsigned long handle = -ENOMEM;
	unsigned int comp_len = 0;
	void *src, *dst, *mem;
	struct zcomp_strm *zstrm;
	unsigned long element = 0;

	memset(prep, 0, sizeof(*prep));
	mem = kmap_atomic(page);
	if (page_same_filled(mem, &element)) {
		kunmap_atomic(mem);
		prep->flags = ZRAM_SAME;
		prep->element = element;
		return 0;
	}
	kunmap_atomic(mem);

compress_again:
	zstrm = zcomp_stream_get(zram->comps[ZRAM_PRIMARY_COMP]);
	src = kmap_atomic(page);
	ret = zcomp_compress(zstrm, src, &comp_len);
	kunmap_atomic(src);

	if (unlikely(ret)) {
		zcomp_stream_put(zram->comps[ZRAM_PRIMARY_COMP]);
		pr_err("Compression failed! err=%d\n", ret);
		if (!IS_ERR_VALUE(handle))
			zs_free(zram->mem_pool, handle);
		zram_cleanup_prepared_page(zram, prep);
		return ret;
	}

	if (comp_len >= huge_class_size)
		comp_len = PAGE_SIZE;
	/*
	 * handle allocation has 2 paths:
	 * a) fast path is executed with preemption disabled (for
	 *  per-cpu streams) and has __GFP_DIRECT_RECLAIM bit clear,
	 *  since we can't sleep;
	 * b) slow path enables preemption and attempts to allocate
	 *  the page with __GFP_DIRECT_RECLAIM bit set. we have to
	 *  put per-cpu compression stream and, thus, to re-do
	 *  the compression once handle is allocated.
	 *
	 * if we have a 'non-null' handle here then we are coming
	 * from the slow path and handle has already been allocated.
	 */
	if (IS_ERR_VALUE(handle))
		handle = zs_malloc(zram->mem_pool, comp_len,
				__GFP_KSWAPD_RECLAIM |
				__GFP_NOWARN |
				__GFP_HIGHMEM |
				__GFP_MOVABLE |
				__GFP_CMA);
	if (IS_ERR_VALUE(handle)) {
		zcomp_stream_put(zram->comps[ZRAM_PRIMARY_COMP]);
		atomic64_inc(&zram->stats.writestall);
		handle = zs_malloc(zram->mem_pool, comp_len,
				GFP_NOIO | __GFP_HIGHMEM |
				__GFP_MOVABLE | __GFP_CMA);
		if (IS_ERR_VALUE(handle))
			return PTR_ERR((void *)handle);

		if (comp_len != PAGE_SIZE)
			goto compress_again;
		/*
		 * If the page is not compressible, you need to acquire the
		 * lock and execute the code below. The zcomp_stream_get()
		 * call is needed to disable the cpu hotplug and grab the
		 * zstrm buffer back. It is necessary that the dereferencing
		 * of the zstrm variable below occurs correctly.
		 */
		zstrm = zcomp_stream_get(zram->comps[ZRAM_PRIMARY_COMP]);
	}

	alloced_pages = zs_get_total_pages(zram->mem_pool);
	update_used_max(zram, alloced_pages);

	if (zram->limit_pages && alloced_pages > zram->limit_pages) {
		zcomp_stream_put(zram->comps[ZRAM_PRIMARY_COMP]);
		zs_free(zram->mem_pool, handle);
		return -ENOMEM;
	}

	dst = zs_map_object(zram->mem_pool, handle, ZS_MM_WO);

	src = zstrm->buffer;
	if (comp_len == PAGE_SIZE)
		src = kmap_atomic(page);
	memcpy(dst, src, comp_len);
	if (comp_len == PAGE_SIZE)
		kunmap_atomic(src);

	zcomp_stream_put(zram->comps[ZRAM_PRIMARY_COMP]);
	zs_unmap_object(zram->mem_pool, handle);
	prep->handle = handle;
	prep->comp_len = comp_len;
	prep->prio = ZRAM_PRIMARY_COMP;
	return ret;
}

static int zram_prepare_stored_payload(struct zram *zram, const void *src,
				       size_t size, u32 prio,
				       unsigned long snapshot_flags,
				       struct zram_prepared_page *prep)
{
	unsigned long alloced_pages;
	unsigned long handle;
	void *dst;

	if (!src || !size || size > PAGE_SIZE || !prep)
		return -EINVAL;

	memset(prep, 0, sizeof(*prep));
	handle = zs_malloc(zram->mem_pool, size,
			   __GFP_KSWAPD_RECLAIM | __GFP_NOWARN |
			   __GFP_HIGHMEM | __GFP_MOVABLE | __GFP_CMA);
	if (IS_ERR_VALUE(handle)) {
		atomic64_inc(&zram->stats.writestall);
		handle = zs_malloc(zram->mem_pool, size,
				   GFP_NOIO | __GFP_HIGHMEM |
				   __GFP_MOVABLE | __GFP_CMA);
		if (IS_ERR_VALUE(handle))
			return PTR_ERR((void *)handle);
	}

	alloced_pages = zs_get_total_pages(zram->mem_pool);
	update_used_max(zram, alloced_pages);
	if (zram->limit_pages && alloced_pages > zram->limit_pages) {
		zs_free(zram->mem_pool, handle);
		return -ENOMEM;
	}

	dst = zs_map_object(zram->mem_pool, handle, ZS_MM_WO);
	memcpy(dst, src, size);
	zs_unmap_object(zram->mem_pool, handle);

	prep->handle = handle;
	prep->comp_len = size;
	prep->prio = prio;
	prep->incompressible = snapshot_flags &
		(1UL << ZRAM_INCOMPRESSIBLE);
	return 0;
}

static void zram_commit_prepared_page(struct zram *zram, u32 index,
					      struct zram_prepared_page *prep,
					      u64 memcg_id)
{
	/*
	 * Free memory associated with this sector
	 * before overwriting unused sectors.
	 */
	zram_free_page(zram, index, CRYSTAL_SDDC_RELEASE_REWRITE);

	if (prep->comp_len == PAGE_SIZE) {
		zram_set_flag(zram, index, ZRAM_HUGE);
		atomic64_inc(&zram->stats.huge_pages);
		atomic64_inc(&zram->stats.huge_pages_since);
	}

	if (prep->flags) {
		zram_set_flag(zram, index, prep->flags);
		zram_set_element(zram, index, prep->element);
		if (prep->flags == ZRAM_SAME)
			atomic64_inc(&zram->stats.same_pages);
	}  else {
		zram_set_handle(zram, index, prep->handle);
		zram_set_obj_size(zram, index, prep->comp_len);
		zram_set_priority(zram, index, prep->prio);
		atomic64_add(prep->comp_len, &zram->stats.compr_data_size);
	}
	if (prep->incompressible)
		zram_set_flag(zram, index, ZRAM_INCOMPRESSIBLE);

	zram->table[index].memcg_id = memcg_id;

	/* Update stats */
	atomic64_inc(&zram->stats.pages_stored);
	zram_memcg_stats_add_current(zram, index);
	crystal_sddc_slot_stored_locked(zram, index);
}

static int zram_write_page_with_memcg(struct zram *zram, struct page *page,
		u32 index, u64 memcg_id)
{
	struct crystal_sddc_job_key sddc_key;
	struct zram_prepared_page prep;
	int ret;

	if (!zram_memcg_stats_ensure(zram, memcg_id, GFP_NOIO))
		return -ENOMEM;

	ret = zram_prepare_page(zram, page, &prep);
	if (ret)
		return ret;

retry_lock:
	zram_slot_lock(zram, index);
	if (zram_test_flag(zram, index, ZRAM_UNDER_WB)) {
		zram_slot_unlock(zram, index);
		wait_on_bit_io(&zram->table[index].flags, ZRAM_UNDER_WB,
			       TASK_UNINTERRUPTIBLE);
		goto retry_lock;
	}
	zram_commit_prepared_page(zram, index, &prep, memcg_id);
	crystal_sddc_job_key_locked(zram, index, &sddc_key);
	zram_slot_unlock(zram, index);
	crystal_sddc_queue_observation(zram, &sddc_key);
	return 0;
}

static int zram_write_page(struct zram *zram, struct page *page, u32 index)
{
	return zram_write_page_with_memcg(zram, page, index,
					 zram_page_memcg_id(page));
}

static void zram_clear_under_wb(struct zram *zram, u32 index)
{
	zram_slot_lock(zram, index);
	if (zram_test_flag(zram, index, ZRAM_UNDER_WB))
		zram_clear_flag_wake(zram, index, ZRAM_UNDER_WB);
	zram_slot_unlock(zram, index);
	crystal_sddc_requeue_observation(zram, index);
}

static bool zram_wb_snapshot_matches(struct zram *zram, u32 index,
		const struct zram_wb_snapshot *snapshot)
{
	if (!zram_test_flag(zram, index, ZRAM_WB) ||
	    !zram_test_flag(zram, index, ZRAM_UNDER_WB))
		return false;
	if (zram_get_element(zram, index) != snapshot->element)
		return false;
	if (zram_get_obj_size(zram, index) != snapshot->size)
		return false;
	if (zram->table[index].memcg_id != snapshot->memcg_id)
		return false;
	return zram_snapshot_flags(zram, index) == snapshot->flags &&
		crystal_sddc_snapshot_matches_locked(zram, index,
			&snapshot->sddc);
}

static bool zram_prefetch_snapshot_matches(struct zram *zram, u32 index,
		const struct zram_wb_snapshot *snapshot)
{
	if (!zram_test_flag(zram, index, ZRAM_WB) ||
	    zram_test_flag(zram, index, ZRAM_UNDER_WB))
		return false;
	if (zram_get_element(zram, index) != snapshot->element)
		return false;
	if (zram_get_obj_size(zram, index) != snapshot->size)
		return false;
	if (zram->table[index].memcg_id != snapshot->memcg_id)
		return false;
	return zram_snapshot_flags(zram, index) == snapshot->flags &&
		crystal_sddc_snapshot_matches_locked(zram, index,
			&snapshot->sddc);
}

static int zram_batchin_flush_items(struct zram *zram,
		struct zram_batchin_item *items, struct zms_load_item *loads,
		unsigned int count, struct page *page, u64 target_cgroup_id,
		unsigned long *moved, unsigned long *skipped,
		unsigned long *read_errors, unsigned long *prepare_errors,
		unsigned long *snapshot_mismatch, bool account_batchin,
		bool mark_prefetched)
{
	struct zms_io io;
	unsigned int i;
	int ret;
	int first_err = 0;

	if (!count)
		return 0;

	ret = zms_load_batch(zram->zms, loads, count, GFP_NOIO, &io);
	zram_record_zms_io(zram, items[0].index, &io);
	if (account_batchin) {
		atomic64_inc(&zram->stats.batchin_zms_batches);
		atomic64_add(count, &zram->stats.batchin_zms_items);
		atomic64_add(io.read_ios, &zram->stats.batchin_zms_read_ios);
	}
	if (ret)
		first_err = ret;

	for (i = 0; i < count; i++) {
		struct crystal_sddc_job_key sddc_key;
		bool snapshot_changed;
		int err = loads[i].ret;

		if (!err) {
			if (crystal_sddc_native_wb_kind(
				    items[i].snapshot.sddc.kind)) {
				if (items[i].native_sddc)
					err = crystal_sddc_native_wb_restore_pinned(
						zram, page,
						&items[i].snapshot.sddc,
						&items[i].native_ref,
						loads[i].dst,
						loads[i].loaded_size);
				else
					err = crystal_sddc_native_wb_restore_page(
						zram, page,
						&items[i].snapshot.sddc,
						loads[i].dst,
						loads[i].loaded_size);
			} else
				err = zram_decode_zms_payload(zram, page,
						      loads[i].dst,
						      loads[i].loaded_size,
						      loads[i].expected_size,
						      items[i].prio);
		}
		zram_batchin_put_native(&items[i]);
		if (err) {
			zram_slot_lock(zram, items[i].index);
			snapshot_changed = !zram_wb_snapshot_matches(zram,
					items[i].index, &items[i].snapshot);
			if (zram_test_flag(zram, items[i].index,
					   ZRAM_UNDER_WB))
				zram_clear_flag_wake(zram, items[i].index,
						     ZRAM_UNDER_WB);
			zram_slot_unlock(zram, items[i].index);
			crystal_sddc_requeue_observation(zram, items[i].index);
			if (snapshot_changed) {
				(*snapshot_mismatch)++;
				(*skipped)++;
				continue;
			}
			if (!first_err)
				first_err = err;
			(*read_errors)++;
			chs_log_ratelimited(CHS_LOG_ERR,
				"batchin read error dev=%s index=%lu handle=%lu target_cgroup_id=%llu ret=%d\n",
				zram->disk ? zram->disk->disk_name : "unknown",
				items[i].index, items[i].snapshot.element,
				target_cgroup_id, err);
			continue;
		}

		err = zram_prepare_page(zram, page, &items[i].prep);
		if (err) {
			if (!first_err)
				first_err = err;
			(*prepare_errors)++;
			zram_clear_under_wb(zram, items[i].index);
			chs_log_ratelimited(CHS_LOG_ERR,
				"batchin compress error dev=%s index=%lu handle=%lu target_cgroup_id=%llu ret=%d\n",
				zram->disk ? zram->disk->disk_name : "unknown",
				items[i].index, items[i].snapshot.element,
				target_cgroup_id, err);
			i++;
			goto clear_remaining;
		}
		atomic64_inc(&zram->stats.bd_reads);

		zram_slot_lock(zram, items[i].index);
		if (!zram_wb_snapshot_matches(zram, items[i].index,
					      &items[i].snapshot)) {
			if (zram_test_flag(zram, items[i].index, ZRAM_UNDER_WB))
				zram_clear_flag_wake(zram, items[i].index, ZRAM_UNDER_WB);
			zram_slot_unlock(zram, items[i].index);
			crystal_sddc_requeue_observation(zram, items[i].index);
			zram_cleanup_prepared_page(zram, &items[i].prep);
			(*snapshot_mismatch)++;
			(*skipped)++;
			chs_log_ratelimited(CHS_LOG_WARN,
				"batchin snapshot mismatch dev=%s index=%lu handle=%lu target_cgroup_id=%llu\n",
				zram->disk ? zram->disk->disk_name : "unknown",
				items[i].index, items[i].snapshot.element,
				target_cgroup_id);
			continue;
		}

		zram_clear_flag_wake(zram, items[i].index, ZRAM_UNDER_WB);
		zram_commit_prepared_page(zram, items[i].index, &items[i].prep,
					  items[i].memcg_id);
		if (mark_prefetched)
			zram_set_prefetched(zram, items[i].index);
		crystal_sddc_job_key_locked(zram, items[i].index, &sddc_key);
		zram_slot_unlock(zram, items[i].index);
		crystal_sddc_queue_observation(zram, &sddc_key);
		(*moved)++;
		cond_resched();
	}

	return first_err;

clear_remaining:
	for (; i < count; i++) {
		zram_batchin_put_native(&items[i]);
		zram_clear_under_wb(zram, items[i].index);
	}

	return first_err;
}

static int zram_prefetch_selected_items(struct zram *zram,
					struct zram_prefetch_item *items,
					unsigned int count, unsigned long *moved,
					unsigned long *skipped,
					unsigned long *read_errors,
					unsigned long *prepare_errors,
					unsigned long *snapshot_mismatch)
{
	struct zms_load_item loads[ZRAM_ZMS_PREFETCH_MAX] = { };
	struct zms_load_ref refs[ZRAM_ZMS_PREFETCH_MAX] = { };
	bool ref_valid[ZRAM_ZMS_PREFETCH_MAX] = { };
	unsigned int load_map[ZRAM_ZMS_PREFETCH_MAX] = { };
	size_t payload_bytes = 0;
	void *payload_buf;
	unsigned int i;
	unsigned int load_count = 0;
	int first_err = 0;

	if (!count)
		return -ENODATA;
	if (WARN_ON_ONCE(count > ZRAM_ZMS_PREFETCH_MAX))
		return -EINVAL;

	for (i = 0; i < count; i++) {
		items[i].payload_valid = false;
		items[i].payload_offset = payload_bytes;
		if (!items[i].snapshot.size ||
		    items[i].snapshot.size > PAGE_SIZE ||
		    payload_bytes > SIZE_MAX - items[i].snapshot.size)
			return -EINVAL;
		payload_bytes += items[i].snapshot.size;
	}
	if (WARN_ON_ONCE(payload_bytes > ZRAM_ZMS_PREFETCH_MAX * PAGE_SIZE))
		return -EINVAL;

	payload_buf = kvmalloc(payload_bytes, GFP_NOIO);
	if (!payload_buf) {
		atomic64_inc(&zram->stats.prefetch_payload_buffer_failures);
		return -ENOMEM;
	}

	for (i = 0; i < count; i++) {
		int ret;

		atomic64_inc(&zram->stats.prefetch_cached_attempts);
		ret = zms_load_prefetch_ref(zram->zms, &items[i].cookie,
					    &refs[i]);
		if (ret) {
			if (ret == -EAGAIN) {
				atomic64_inc(&zram->stats.prefetch_cached_misses);
				load_map[load_count] = i;
				loads[load_count].handle = items[i].cookie.handle;
				loads[load_count].dst =
					(char *)payload_buf + items[i].payload_offset;
				loads[load_count].expected_size =
					items[i].snapshot.size;
				load_count++;
				continue;
			} else if (ret == -ESTALE || ret == -ENOENT) {
				atomic64_inc(&zram->stats.prefetch_cookie_mismatch);
			} else {
				(*read_errors)++;
				if (!first_err)
					first_err = ret;
			}
			(*skipped)++;
			continue;
		}
		atomic64_inc(&zram->stats.prefetch_cached_hits);
		if (refs[i].size != items[i].snapshot.size) {
			zms_put_ref(zram->zms, &refs[i]);
			atomic64_inc(&zram->stats.prefetch_cookie_mismatch);
			(*skipped)++;
			continue;
		}
		ref_valid[i] = true;
	}

	if (load_count) {
		struct zms_io io;
		unsigned int j;
		int ret;

		ret = zms_load_batch(zram->zms, loads, load_count, GFP_NOIO, &io);
		zram_record_zms_io(zram, items[0].index, &io);
		if (ret && !first_err)
			first_err = ret;

		for (j = 0; j < load_count; j++) {
			i = load_map[j];
			if (loads[j].ret) {
				if (!first_err)
					first_err = loads[j].ret;
				(*read_errors)++;
				(*skipped)++;
				continue;
			}
			if (loads[j].loaded_size != items[i].snapshot.size) {
				if (!first_err)
					first_err = -EIO;
				(*read_errors)++;
				(*skipped)++;
				continue;
			}
			items[i].payload_valid = true;
		}
	}

	for (i = 0; i < count; i++) {
		if (!ref_valid[i])
			continue;
		memcpy((char *)payload_buf + items[i].payload_offset,
		       refs[i].data, refs[i].size);
		items[i].payload_valid = true;
	}
	for (i = 0; i < count; i++) {
		if (ref_valid[i])
			zms_put_ref(zram->zms, &refs[i]);
	}

	for (i = 0; i < count; i++) {
		struct crystal_sddc_job_key sddc_key;
		int ret;

		if (!items[i].payload_valid)
			continue;
		ret = zram_prepare_stored_payload(zram,
				(char *)payload_buf + items[i].payload_offset,
				items[i].snapshot.size, items[i].prio,
				items[i].snapshot.flags, &items[i].prep);
		if (ret) {
			atomic64_inc(&zram->stats.prefetch_promote_alloc_failures);
			(*prepare_errors)++;
			(*skipped)++;
			if (!first_err)
				first_err = ret;
			continue;
		}

		zram_slot_lock(zram, items[i].index);
		if (!zram_prefetch_snapshot_matches(zram, items[i].index,
						    &items[i].snapshot)) {
			zram_slot_unlock(zram, items[i].index);
			zram_cleanup_prepared_page(zram, &items[i].prep);
			(*snapshot_mismatch)++;
			(*skipped)++;
			continue;
		}
		if (!zms_cookie_matches(zram->zms, &items[i].cookie)) {
			zram_slot_unlock(zram, items[i].index);
			zram_cleanup_prepared_page(zram, &items[i].prep);
			atomic64_inc(&zram->stats.prefetch_cookie_mismatch);
			(*skipped)++;
			continue;
		}

		zram_commit_prepared_page(zram, items[i].index, &items[i].prep,
					  items[i].memcg_id);
		zram_set_prefetched(zram, items[i].index);
		crystal_sddc_job_key_locked(zram, items[i].index, &sddc_key);
		zram_slot_unlock(zram, items[i].index);
		crystal_sddc_queue_observation(zram, &sddc_key);
		atomic64_inc(&zram->stats.bd_reads);
		atomic64_add(items[i].snapshot.size,
			     &zram->stats.prefetch_promote_bytes);
		(*moved)++;
		cond_resched();
	}

	kvfree(payload_buf);
	return first_err;
}

static void zram_zms_prefetch_workfn(struct work_struct *work)
{
	struct zram_zms_prefetch_work *prefetch =
		container_of(work, struct zram_zms_prefetch_work, work);
	struct zram *zram = prefetch->zram;
	unsigned long moved = 0;
	unsigned long skipped = 0;
	unsigned long read_errors = 0;
	unsigned long prepare_errors = 0;
	unsigned long snapshot_mismatch = 0;
	int ret = -ENODEV;

	atomic64_inc(&zram->stats.prefetch_runs);
	down_read(&zram->init_lock);
	if (init_done(zram) && zram->zms &&
	    READ_ONCE(zram->prefetch_enable) &&
	    !crystal_hybridswap_system_sleeping())
		ret = zram_prefetch_selected_items(zram, prefetch->items,
						   prefetch->count, &moved,
						   &skipped, &read_errors,
						   &prepare_errors,
						   &snapshot_mismatch);
	else {
		skipped = prefetch->count;
	}
	up_read(&zram->init_lock);

	atomic64_add(moved, &zram->stats.prefetch_moved);
	atomic64_add(skipped, &zram->stats.prefetch_skipped);
	atomic64_add(read_errors, &zram->stats.prefetch_read_errors);
	atomic64_add(prepare_errors, &zram->stats.prefetch_prepare_errors);
	atomic64_add(snapshot_mismatch,
		     &zram->stats.prefetch_snapshot_mismatch);
	if (ret == -ENODATA || (!moved && !read_errors && !prepare_errors))
		atomic64_inc(&zram->stats.prefetch_no_data);
	if (ret == -ENOMEM)
		atomic64_inc(&zram->stats.prefetch_alloc_failures);
	zram_put(zram);
	kfree(prefetch);
}

static bool zram_zms_prefetch_prepare_item(struct zram *zram,
					   struct zram_prefetch_item *item,
					   u32 index,
					   const struct zms_object_cookie *cookie,
					   u64 memcg_id)
{
	bool ok = false;

	zram_slot_lock(zram, index);
	if (!zram_test_flag(zram, index, ZRAM_WB) ||
	    zram_test_flag(zram, index, ZRAM_UNDER_WB))
		goto out;
	if (zram_get_element(zram, index) != cookie->handle)
		goto out;
	if (!zram_get_obj_size(zram, index) ||
	    zram_get_obj_size(zram, index) > PAGE_SIZE)
		goto out;
	if (!memcg_id || zram->table[index].memcg_id != memcg_id)
		goto out;

	memset(item, 0, sizeof(*item));
	item->index = index;
	item->memcg_id = zram->table[index].memcg_id;
	item->prio = zram_get_priority(zram, index);
	item->cookie = *cookie;
	zram_take_wb_snapshot(zram, index, &item->snapshot);
	if (crystal_sddc_native_wb_kind(item->snapshot.sddc.kind))
		goto out;
	ok = true;

out:
	zram_slot_unlock(zram, index);
	return ok;
}

static void zram_zms_maybe_prefetch(struct zram *zram, u32 index,
				    const struct zms_load_ref *source,
				    u64 memcg_id,
				    const struct zms_io *io)
{
	struct zram_zms_prefetch_work *prefetch;
	struct zram_prefetch_candidate candidates[ZRAM_PREFETCH_NEIGHBOR_SCAN_MAX];
	struct zms_object_cookie neighbors[ZRAM_PREFETCH_NEIGHBOR_SCAN_MAX];
	struct zms_prefetch_info info;
	unsigned long nr_pages;
	unsigned int nr_candidates = 0;
	unsigned int promotion_limit;
	int nr_neighbors;
	int i;

	if (!zram || !zram->zms || !source || !source->cookie.handle ||
	    !memcg_id || !io || !io->read_ios)
		return;
	if (!READ_ONCE(zram->prefetch_enable) ||
	    !READ_ONCE(zram->prefetch_degree))
		return;
	if (crystal_hybridswap_system_sleeping())
		return;
	if (!down_read_trylock(&zram->init_lock))
		return;
	if (!init_done(zram) || !zram->zms) {
		up_read(&zram->init_lock);
		return;
	}

	nr_neighbors = zms_peek_ref_neighbors(zram->zms, source, neighbors,
					      ARRAY_SIZE(neighbors), &info);
	if (nr_neighbors <= 0) {
		up_read(&zram->init_lock);
		return;
	}

	prefetch = kzalloc(sizeof(*prefetch), GFP_ATOMIC);
	if (!prefetch) {
		atomic64_inc(&zram->stats.prefetch_alloc_failures);
		up_read(&zram->init_lock);
		return;
	}

	nr_pages = zram_pages_snapshot(zram);
	for (i = 0; i < nr_neighbors; i++) {
		unsigned long neighbor = neighbors[i].handle;
		u32 neighbor_index;

		if (!neighbor)
			continue;
		if (neighbor == source->cookie.handle)
			continue;
		if (neighbor - 1 > U32_MAX)
			continue;
		neighbor_index = (u32)(neighbor - 1);
		if (neighbor_index == index || neighbor_index >= nr_pages)
			continue;

		atomic64_inc(&zram->stats.prefetch_candidates);
		if (!zram_prefetch_insert_candidate(candidates, index, &nr_candidates,
			&(struct zram_prefetch_candidate) {
				.cookie = neighbors[i],
				.index = neighbor_index,
				.score = zram_prefetch_score_neighbor(zram, index,
								      neighbor_index,
								      memcg_id),
			}))
			atomic64_inc(&zram->stats.prefetch_skipped);
	}
	promotion_limit = min_t(unsigned int,
				READ_ONCE(zram->prefetch_degree),
				info.safe_promotions);
	if (nr_candidates > promotion_limit)
		atomic64_add(nr_candidates - promotion_limit,
			     &zram->stats.prefetch_safe_limited);

	for (i = 0; i < nr_candidates &&
		    prefetch->count < promotion_limit; i++) {
		if (zram_zms_prefetch_prepare_item(zram,
				&prefetch->items[prefetch->count],
				candidates[i].index, &candidates[i].cookie,
				memcg_id))
			prefetch->count++;
		else
			atomic64_inc(&zram->stats.prefetch_skipped);
	}

	if (!prefetch->count) {
		kfree(prefetch);
		up_read(&zram->init_lock);
		return;
	}
	if (!zram_try_get(zram)) {
		kfree(prefetch);
		up_read(&zram->init_lock);
		return;
	}

	prefetch->zram = zram;
	INIT_WORK(&prefetch->work, zram_zms_prefetch_workfn);
	atomic64_add(prefetch->count, &zram->stats.prefetch_submitted);
	queue_work(system_unbound_wq, &prefetch->work);
	up_read(&zram->init_lock);
}

int zram_batchin_device(struct device *dev, unsigned long max_pages,
		u64 target_cgroup_id,
		struct crystal_hybridswap_batchin_stats *batchin_stats)
{
	struct zram *zram;
	struct page *page;
	struct zram_batchin_item *items;
	struct zms_load_item *loads;
	void *payloads;
	unsigned long nr_pages;
	unsigned long index;
	unsigned long scanned = 0;
	unsigned long matched = 0;
	unsigned long moved = 0;
	unsigned long skipped = 0;
	unsigned long filtered = 0;
	unsigned long read_errors = 0;
	unsigned long prepare_errors = 0;
	unsigned long snapshot_mismatch = 0;
	unsigned int batch_count = 0;
	u64 start_ns = ktime_get_ns();
	int ret = 0;

	if (batchin_stats)
		memset(batchin_stats, 0, sizeof(*batchin_stats));

	if (!dev)
		return -EINVAL;

	zram = dev_to_zram(dev);
	atomic64_inc(&zram->stats.batchin_runs);
	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		ret = -EINVAL;
		chs_log_ratelimited(CHS_LOG_WARN,
			"batchin skip init_not_done target_cgroup_id=%llu ret=%d\n",
			target_cgroup_id, ret);
		goto release_init_lock;
	}

	if (!zram->backing_dev || !zram->zms) {
		ret = -ENODEV;
		chs_log_ratelimited(CHS_LOG_WARN,
			"batchin skip no backing_dev target_cgroup_id=%llu max_pages=%lu ret=%d\n",
			target_cgroup_id, max_pages, ret);
		goto release_init_lock;
	}

	page = alloc_page(GFP_KERNEL);
	if (!page) {
		ret = -ENOMEM;
		chs_log(CHS_LOG_ERR, "batchin alloc page failed ret=%d\n", ret);
		goto release_init_lock;
	}
	items = kcalloc(ZRAM_BATCHIN_GROUP_MAX, sizeof(*items), GFP_KERNEL);
	if (!items) {
		ret = -ENOMEM;
		goto free_page;
	}
	loads = kcalloc(ZRAM_BATCHIN_GROUP_MAX, sizeof(*loads), GFP_KERNEL);
	if (!loads) {
		ret = -ENOMEM;
		goto free_items;
	}
	payloads = kvmalloc_array(ZRAM_BATCHIN_GROUP_MAX, PAGE_SIZE, GFP_KERNEL);
	if (!payloads) {
		ret = -ENOMEM;
		goto free_loads;
	}

	nr_pages = zram_pages_snapshot(zram);
	for (index = 0; index < nr_pages; index++) {
		if (max_pages && moved >= max_pages)
			break;
		scanned++;

		zram_slot_lock(zram, index);
		if (!zram_test_flag(zram, index, ZRAM_WB) ||
		    zram_test_flag(zram, index, ZRAM_UNDER_WB)) {
			zram_slot_unlock(zram, index);
			skipped++;
			cond_resched();
			continue;
		}
		if (target_cgroup_id &&
		    zram->table[index].memcg_id != target_cgroup_id) {
			zram_slot_unlock(zram, index);
			filtered++;
			cond_resched();
			continue;
		}
		matched++;
		memset(&items[batch_count], 0, sizeof(items[batch_count]));
		items[batch_count].index = index;
		items[batch_count].memcg_id = zram->table[index].memcg_id;
		items[batch_count].prio = zram_get_priority(zram, index);
		zram_set_flag(zram, index, ZRAM_UNDER_WB);
		zram_take_wb_snapshot(zram, index,
				      &items[batch_count].snapshot);
		if (crystal_sddc_native_wb_kind(
			    items[batch_count].snapshot.sddc.kind)) {
			if (!crystal_sddc_native_wb_pin_locked(zram, index,
					&items[batch_count].snapshot.sddc,
					&items[batch_count].native_ref)) {
				zram_clear_flag_wake(zram, index, ZRAM_UNDER_WB);
				zram_slot_unlock(zram, index);
				skipped++;
				cond_resched();
				continue;
			}
			items[batch_count].native_sddc = true;
		}
		zram_slot_unlock(zram, index);
		loads[batch_count].handle = items[batch_count].snapshot.element;
		loads[batch_count].dst = (char *)payloads +
			batch_count * PAGE_SIZE;
		loads[batch_count].expected_size =
			items[batch_count].snapshot.size;
		batch_count++;
		if (batch_count == ZRAM_BATCHIN_GROUP_MAX ||
		    (max_pages && moved + batch_count >= max_pages)) {
			int err;

				err = zram_batchin_flush_items(zram, items, loads,
					batch_count, page, target_cgroup_id, &moved,
					&skipped, &read_errors, &prepare_errors,
					&snapshot_mismatch, true, false);
			if (err && !ret)
				ret = err;
			batch_count = 0;
			if (err)
				break;
		}
		cond_resched();
	}

	if (!ret && batch_count) {
		int err;

		err = zram_batchin_flush_items(zram, items, loads, batch_count,
			page, target_cgroup_id, &moved, &skipped, &read_errors,
			&prepare_errors, &snapshot_mismatch, true, false);
		if (err && !ret)
			ret = err;
	}

	kvfree(payloads);
free_loads:
	kfree(loads);
free_items:
	kfree(items);
free_page:
	__free_page(page);
release_init_lock:
	up_read(&zram->init_lock);

	if (batchin_stats) {
		batchin_stats->scanned_pages = scanned;
		batchin_stats->matched_pages = matched;
		batchin_stats->moved_pages = moved;
		batchin_stats->skipped_pages = skipped;
		batchin_stats->filtered_pages = filtered;
		batchin_stats->read_errors = read_errors;
		batchin_stats->prepare_errors = prepare_errors;
		batchin_stats->snapshot_mismatch = snapshot_mismatch;
		batchin_stats->last_error = ret;
	}

	{
		u64 delta = ktime_get_ns() - start_ns;
		s64 delta_s64 = zram_ns_to_s64(delta);
		int final_ret = ret && !moved ? ret : (moved ? (int)moved : -ENODATA);

		atomic64_add(moved, &zram->stats.batchin_pages);
		atomic64_add(filtered, &zram->stats.batchin_filtered_pages);
		atomic64_add(read_errors, &zram->stats.batchin_read_errors);
		atomic64_add(prepare_errors, &zram->stats.batchin_prepare_errors);
		atomic64_add(snapshot_mismatch,
			&zram->stats.batchin_snapshot_mismatch);
		atomic64_add(delta_s64, &zram->stats.batchin_total_ns);
		zram_atomic64_update_max(&zram->stats.batchin_max_ns, delta_s64);
		atomic64_set(&zram->stats.batchin_last_ret, final_ret);
		if (final_ret == -ENODATA)
			atomic64_inc(&zram->stats.batchin_no_data);
		else if (final_ret < 0 || read_errors || prepare_errors)
			atomic64_inc(&zram->stats.batchin_failures);
		if (delta >= CHS_ZRAM_SLOW_WORK_NS) {
			atomic64_inc(&zram->stats.batchin_slow_runs);
			chs_log_ratelimited(CHS_LOG_WARN,
				"slow batchin dev=%s target_cgroup_id=%llu scanned_pages=%lu matched_pages=%lu moved_pages=%lu filtered_pages=%lu read_errors=%lu prepare_errors=%lu snapshot_mismatch_pages=%lu latency_ns=%llu ret=%d\n",
				zram->disk ? zram->disk->disk_name : "unknown",
				target_cgroup_id, scanned, matched, moved, filtered,
				read_errors, prepare_errors, snapshot_mismatch,
				(unsigned long long)delta, final_ret);
		}
	}

	if (ret && !moved)
		chs_log_ratelimited(CHS_LOG_WARN,
			"batchin finished with error dev=%s target_cgroup_id=%llu scope=%s scanned_pages=%lu matched_pages=%lu moved_pages=%lu skipped_pages=%lu filtered_pages=%lu read_errors=%lu prepare_errors=%lu snapshot_mismatch_pages=%lu requested_pages=%lu ret=%d\n",
			zram->disk ? zram->disk->disk_name : "unknown",
			target_cgroup_id,
			target_cgroup_id ? "per_memcg" : "global_best_effort",
			scanned, matched, moved, skipped, filtered,
			read_errors, prepare_errors, snapshot_mismatch, max_pages, ret);
	else if (moved)
		chs_log_ratelimited(ret ? CHS_LOG_WARN : CHS_LOG_INFO,
			"batchin success dev=%s target_cgroup_id=%llu scope=%s scanned_pages=%lu matched_pages=%lu moved_pages=%lu skipped_pages=%lu filtered_pages=%lu read_errors=%lu prepare_errors=%lu snapshot_mismatch_pages=%lu requested_pages=%lu last_error=%d\n",
			zram->disk ? zram->disk->disk_name : "unknown",
			target_cgroup_id,
			target_cgroup_id ? "per_memcg" : "global_best_effort",
			scanned, matched, moved, skipped, filtered,
			read_errors, prepare_errors, snapshot_mismatch, max_pages, ret);
	else
		chs_log_ratelimited(CHS_LOG_INFO,
			"batchin skipped no writeback pages dev=%s target_cgroup_id=%llu scope=%s scanned_pages=%lu matched_pages=%lu filtered_pages=%lu requested_pages=%lu reason=no_writeback_pages\n",
			zram->disk ? zram->disk->disk_name : "unknown",
			target_cgroup_id,
			target_cgroup_id ? "per_memcg" : "global_best_effort",
			scanned, matched, filtered, max_pages);

	if (ret && !moved)
		return ret;
	if (!moved)
		return -ENODATA;
	return moved > INT_MAX ? INT_MAX : (int)moved;
}

/*
 * This is a partial IO. Read the full page before writing the changes.
 */
static int zram_bvec_write_partial(struct zram *zram, struct bio_vec *bvec,
				   u32 index, int offset)
{
	struct page *page = alloc_page(GFP_NOIO);
	u64 memcg_id;
	int ret;

	if (!page)
		return -ENOMEM;

	memcg_id = zram_page_memcg_id(bvec->bv_page);
	if (!memcg_id) {
		/*
		 * Preserve the existing slot memcg for legacy partial writes whose
		 * source page cannot provide one. Keeping this as a plain slot sample
		 * avoids changing RMW semantics; correctness relies on the block layer
		 * same-sector IO serialization that already orders the old-page read
		 * with this fallback metadata.
		 */
		zram_slot_lock(zram, index);
		memcg_id = zram->table[index].memcg_id;
		zram_slot_unlock(zram, index);
	}

	/*
	 * Partial writes merge into this temporary full page. Force synchronous
	 * old-page reads so WBed data is present before memcpy/commit/free.
	 */
	ret = zram_read_page(zram, page, index, NULL);
	if (!ret) {
		memcpy_from_bvec(page_address(page) + offset, bvec);
		ret = zram_write_page_with_memcg(zram, page, index, memcg_id);
	}
	__free_page(page);
	return ret;
}

static int zram_bvec_write(struct zram *zram, struct bio_vec *bvec,
			   u32 index, int offset)
{
	if (is_partial_io(bvec))
		return zram_bvec_write_partial(zram, bvec, index, offset);
	return zram_write_page(zram, bvec->bv_page, index);
}

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_MULTI_COMP
/*
 * This function will decompress (unless it's ZRAM_HUGE) the page and then
 * attempt to compress it using provided compression algorithm priority
 * (which is potentially more effective).
 *
 * Corresponding ZRAM slot should be locked.
 */
static int zram_recompress(struct zram *zram, u32 index, struct page *page,
			   u32 threshold, u32 prio, u32 prio_max)
{
	struct zcomp_strm *zstrm = NULL;
	unsigned long handle_old;
	unsigned long handle_new;
	u64 memcg_id;
	unsigned int comp_len_old;
	unsigned int comp_len_new;
	unsigned int class_index_old;
	unsigned int class_index_new;
	u32 num_recomps = 0;
	void *src, *dst;
	int ret;

	if (crystal_sddc_slot_allocated_locked(zram, index))
		return 0;

	handle_old = zram_get_handle(zram, index);
	if (!handle_old)
		return -EINVAL;

	comp_len_old = zram_get_obj_size(zram, index);
	/*
	 * Do not recompress objects that are already "small enough".
	 */
	if (comp_len_old < threshold)
		return 0;

	ret = zram_read_from_zspool(zram, page, index);
	if (ret)
		return ret;

	/*
	 * We touched this entry so mark it as non-IDLE. This makes sure that
	 * we don't preserve IDLE flag and don't incorrectly pick this entry
	 * for different post-processing type (e.g. writeback).
	 */
	zram_clear_flag(zram, index, ZRAM_IDLE);

	class_index_old = zs_lookup_class_index(zram->mem_pool, comp_len_old);
	/*
	 * Iterate the secondary comp algorithms list (in order of priority)
	 * and try to recompress the page.
	 */
	for (; prio < prio_max; prio++) {
		if (!zram->comps[prio])
			continue;

		/*
		 * Skip if the object is already re-compressed with a higher
		 * priority algorithm (or same algorithm).
		 */
		if (prio <= zram_get_priority(zram, index))
			continue;

		num_recomps++;
		zstrm = zcomp_stream_get(zram->comps[prio]);
		src = kmap_atomic(page);
		ret = zcomp_compress(zstrm, src, &comp_len_new);
		kunmap_atomic(src);

		if (ret) {
			zcomp_stream_put(zram->comps[prio]);
			return ret;
		}

		class_index_new = zs_lookup_class_index(zram->mem_pool,
							comp_len_new);

		/* Continue until we make progress */
		if (class_index_new >= class_index_old ||
		    (threshold && comp_len_new >= threshold)) {
			zcomp_stream_put(zram->comps[prio]);
			continue;
		}

		/* Recompression was successful so break out */
		break;
	}

	/*
	 * We did not try to recompress, e.g. when we have only one
	 * secondary algorithm and the page is already recompressed
	 * using that algorithm
	 */
	if (!zstrm)
		return 0;

	if (class_index_new >= class_index_old) {
		/*
		 * Secondary algorithms failed to re-compress the page
		 * in a way that would save memory, mark the object as
		 * incompressible so that we will not try to compress
		 * it again.
		 *
		 * We need to make sure that all secondary algorithms have
		 * failed, so we test if the number of recompressions matches
		 * the number of active secondary algorithms.
		 */
		if (num_recomps == zram->num_active_comps - 1)
			zram_set_flag(zram, index, ZRAM_INCOMPRESSIBLE);
		return 0;
	}

	/* Successful recompression but above threshold */
	if (threshold && comp_len_new >= threshold)
		return 0;

	/*
	 * No direct reclaim (slow path) for handle allocation and no
	 * re-compression attempt (unlike in zram_write_bvec()) since
	 * we already have stored that object in zsmalloc. If we cannot
	 * alloc memory for recompressed object then we bail out and
	 * simply keep the old (existing) object in zsmalloc.
	 */
	handle_new = zs_malloc(zram->mem_pool, comp_len_new,
			       __GFP_KSWAPD_RECLAIM |
			       __GFP_NOWARN |
			       __GFP_HIGHMEM |
			       __GFP_MOVABLE);
	if (IS_ERR_VALUE(handle_new)) {
		zcomp_stream_put(zram->comps[prio]);
		return PTR_ERR((void *)handle_new);
	}

	dst = zs_map_object(zram->mem_pool, handle_new, ZS_MM_WO);
	memcpy(dst, zstrm->buffer, comp_len_new);
	zcomp_stream_put(zram->comps[prio]);

	zs_unmap_object(zram->mem_pool, handle_new);

	memcg_id = zram->table[index].memcg_id;
	zram_free_page(zram, index, CRYSTAL_SDDC_RELEASE_RECOMPRESS);
	zram_set_handle(zram, index, handle_new);
	zram_set_obj_size(zram, index, comp_len_new);
	zram_set_priority(zram, index, prio);
	zram->table[index].memcg_id = memcg_id;

	atomic64_add(comp_len_new, &zram->stats.compr_data_size);
	atomic64_inc(&zram->stats.pages_stored);
	zram_memcg_stats_add_current(zram, index);
	crystal_sddc_slot_stored_locked(zram, index);

	return 0;
}

#define RECOMPRESS_IDLE		(1 << 0)
#define RECOMPRESS_HUGE		(1 << 1)

static ssize_t recompress_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t len)
{
	u32 prio = ZRAM_SECONDARY_COMP, prio_max = ZRAM_MAX_COMPS;
	struct zram *zram = dev_to_zram(dev);
	unsigned long nr_pages;
	char *args, *param, *val, *algo = NULL;
	u32 mode = 0, threshold = 0;
	unsigned long index;
	struct page *page;
	ssize_t ret;

	args = skip_spaces(buf);
	while (*args) {
		args = next_arg(args, &param, &val);

		if (!val || !*val)
			return -EINVAL;

		if (!strcmp(param, "type")) {
			if (!strcmp(val, "idle"))
				mode = RECOMPRESS_IDLE;
			if (!strcmp(val, "huge"))
				mode = RECOMPRESS_HUGE;
			if (!strcmp(val, "huge_idle"))
				mode = RECOMPRESS_IDLE | RECOMPRESS_HUGE;
			continue;
		}

		if (!strcmp(param, "threshold")) {
			/*
			 * We will re-compress only idle objects equal or
			 * greater in size than watermark.
			 */
			ret = kstrtouint(val, 10, &threshold);
			if (ret)
				return ret;
			continue;
		}

		if (!strcmp(param, "algo")) {
			algo = val;
			continue;
		}
	}

	if (threshold >= huge_class_size)
		return -EINVAL;

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		ret = -EINVAL;
		goto release_init_lock;
	}

	if (algo) {
		bool found = false;

		for (; prio < ZRAM_MAX_COMPS; prio++) {
			if (!zram->comp_algs[prio])
				continue;

			if (!strcmp(zram->comp_algs[prio], algo)) {
				prio_max = min(prio + 1, ZRAM_MAX_COMPS);
				found = true;
				break;
			}
		}

		if (!found) {
			ret = -EINVAL;
			goto release_init_lock;
		}
	}

	nr_pages = zram_pages_snapshot(zram);
	page = alloc_page(GFP_KERNEL);
	if (!page) {
		ret = -ENOMEM;
		goto release_init_lock;
	}

	ret = len;
	for (index = 0; index < nr_pages; index++) {
		int err = 0;

		zram_slot_lock(zram, index);

		if (!zram_allocated(zram, index))
			goto next;

		if (mode & RECOMPRESS_IDLE &&
		    !zram_test_flag(zram, index, ZRAM_IDLE))
			goto next;

		if (mode & RECOMPRESS_HUGE &&
		    !zram_test_flag(zram, index, ZRAM_HUGE))
			goto next;

		if (zram_test_flag(zram, index, ZRAM_WB) ||
		    zram_test_flag(zram, index, ZRAM_UNDER_WB) ||
		    zram_test_flag(zram, index, ZRAM_SAME) ||
		    zram_test_flag(zram, index, ZRAM_INCOMPRESSIBLE))
			goto next;

		err = zram_recompress(zram, index, page, threshold,
				      prio, prio_max);
next:
		zram_slot_unlock(zram, index);
		if (err) {
			ret = err;
			break;
		}

		cond_resched();
	}

	__free_page(page);

release_init_lock:
	up_read(&zram->init_lock);
	return ret;
}
#endif

static void zram_bio_discard(struct zram *zram, struct bio *bio)
{
	size_t n = bio->bi_iter.bi_size;
	u32 index = bio->bi_iter.bi_sector >> SECTORS_PER_PAGE_SHIFT;
	u32 offset = (bio->bi_iter.bi_sector & (SECTORS_PER_PAGE - 1)) <<
			SECTOR_SHIFT;

	/*
	 * zram manages data in physical block size units. Because logical block
	 * size isn't identical with physical block size on some arch, we
	 * could get a discard request pointing to a specific offset within a
	 * certain physical block.  Although we can handle this request by
	 * reading that physiclal block and decompressing and partially zeroing
	 * and re-compressing and then re-storing it, this isn't reasonable
	 * because our intent with a discard request is to save memory.  So
	 * skipping this logical block is appropriate here.
	 */
	if (offset) {
		if (n <= (PAGE_SIZE - offset))
			return;

		n -= (PAGE_SIZE - offset);
		index++;
	}

	while (n >= PAGE_SIZE) {
		zram_slot_lock(zram, index);
		if (zram_test_flag(zram, index, ZRAM_UNDER_WB)) {
			zram_slot_unlock(zram, index);
			wait_on_bit_io(&zram->table[index].flags, ZRAM_UNDER_WB,
				       TASK_UNINTERRUPTIBLE);
			continue;
		}
		zram_free_page(zram, index, CRYSTAL_SDDC_RELEASE_DISCARD);
		zram_slot_unlock(zram, index);
		atomic64_inc(&zram->stats.notify_free);
		index++;
		n -= PAGE_SIZE;
	}

	bio_endio(bio);
}

static void zram_bio_read(struct zram *zram, struct bio *bio)
{
	unsigned long start_time = bio_start_io_acct(bio);
	struct bvec_iter iter = bio->bi_iter;

	if (!iter.bi_size) {
		bio_end_io_acct(bio, start_time);
		bio_endio(bio);
		return;
	}

	do {
		u32 index = iter.bi_sector >> SECTORS_PER_PAGE_SHIFT;
		u32 offset = (iter.bi_sector & (SECTORS_PER_PAGE - 1)) <<
				SECTOR_SHIFT;
		struct bio_vec bv = bio_iter_iovec(bio, iter);

		bv.bv_len = min_t(u32, bv.bv_len, PAGE_SIZE - offset);

		if (zram_bvec_read(zram, &bv, index, offset, bio) < 0) {
			atomic64_inc(&zram->stats.failed_reads);
			bio->bi_status = BLK_STS_IOERR;
			break;
		}
		flush_dcache_page(bv.bv_page);

		zram_slot_lock(zram, index);
		zram_accessed(zram, index);
		zram_slot_unlock(zram, index);

		bio_advance_iter_single(bio, &iter, bv.bv_len);
	} while (iter.bi_size);

	bio_end_io_acct(bio, start_time);
	bio_endio(bio);
}

static void zram_bio_write(struct zram *zram, struct bio *bio)
{
	unsigned long start_time = bio_start_io_acct(bio);
	struct bvec_iter iter = bio->bi_iter;
	int ret;

	if (!iter.bi_size) {
		bio_end_io_acct(bio, start_time);
		bio_endio(bio);
		return;
	}

	do {
		u32 index = iter.bi_sector >> SECTORS_PER_PAGE_SHIFT;
		u32 offset = (iter.bi_sector & (SECTORS_PER_PAGE - 1)) <<
				SECTOR_SHIFT;
		struct bio_vec bv = bio_iter_iovec(bio, iter);

		bv.bv_len = min_t(u32, bv.bv_len, PAGE_SIZE - offset);

		ret = zram_bvec_write(zram, &bv, index, offset);
		if (ret < 0) {
			atomic64_inc(&zram->stats.failed_writes);
			chs_log_ratelimited(CHS_LOG_WARN,
				"zram write failed dev=%s index=%u sector=%llu len=%u offset=%u ret=%d\n",
				zram->disk ? zram->disk->disk_name : "unknown",
				index, (unsigned long long)iter.bi_sector,
				bv.bv_len, offset, ret);
			bio->bi_status = BLK_STS_IOERR;
			break;
		}

		zram_slot_lock(zram, index);
		zram_accessed(zram, index);
		zram_slot_unlock(zram, index);

		bio_advance_iter_single(bio, &iter, bv.bv_len);
	} while (iter.bi_size);

	bio_end_io_acct(bio, start_time);
	bio_endio(bio);
}

static int zram_flush(struct zram *zram)
{
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
	struct zms_io io;
	int ret = 0;

	down_read(&zram->init_lock);
	if (init_done(zram) && zram->zms &&
	    !crystal_hybridswap_system_sleeping()) {
		ret = zms_flush_all(zram->zms, GFP_NOIO, &io);
		zram_record_zms_io(zram, 0, &io);
	}
	up_read(&zram->init_lock);

	return ret;
#else
	return 0;
#endif
}

static void zram_bio_flush(struct zram *zram, struct bio *bio)
{
	int ret = zram_flush(zram);

	if (ret)
		bio->bi_status = errno_to_blk_status(ret);
	bio_endio(bio);
}

/*
 * Handler function for all zram I/O requests.
 */
static void zram_submit_bio(struct bio *bio)
{
	struct zram *zram = bio->bi_bdev->bd_disk->private_data;
	int ret;

	if (bio->bi_opf & REQ_PREFLUSH) {
		ret = zram_flush(zram);
		if (ret) {
			bio->bi_status = errno_to_blk_status(ret);
			bio_endio(bio);
			return;
		}

		bio->bi_opf &= ~REQ_PREFLUSH;
		if (!bio->bi_iter.bi_size) {
			bio_endio(bio);
			return;
		}
	}

	switch (bio_op(bio)) {
	case REQ_OP_READ:
		zram_bio_read(zram, bio);
		break;
	case REQ_OP_WRITE:
		zram_bio_write(zram, bio);
		break;
	case REQ_OP_FLUSH:
		zram_bio_flush(zram, bio);
		break;
	case REQ_OP_DISCARD:
	case REQ_OP_WRITE_ZEROES:
		zram_bio_discard(zram, bio);
		break;
	default:
		WARN_ON_ONCE(1);
		bio_endio(bio);
	}
}

static void zram_slot_free_notify(struct block_device *bdev,
				unsigned long index)
{
	struct zram *zram;

	zram = bdev->bd_disk->private_data;

	atomic64_inc(&zram->stats.notify_free);
	if (!zram_slot_trylock(zram, index)) {
		atomic64_inc(&zram->stats.miss_free);
		return;
	}

	if (zram_test_flag(zram, index, ZRAM_UNDER_WB)) {
		atomic64_inc(&zram->stats.miss_free);
		zram_slot_unlock(zram, index);
		return;
	}

	zram_free_page(zram, index, CRYSTAL_SDDC_RELEASE_NOTIFY_FREE);
	zram_slot_unlock(zram, index);
}

static void zram_destroy_comps(struct zram *zram)
{
	u32 prio;

	for (prio = 0; prio < ZRAM_MAX_COMPS; prio++) {
		struct zcomp *comp = zram->comps[prio];

		zram->comps[prio] = NULL;
		if (!comp)
			continue;
		zcomp_destroy(comp);
		zram->num_active_comps--;
	}

	for (prio = ZRAM_PRIMARY_COMP; prio < ZRAM_MAX_COMPS; prio++) {
		/* Do not free statically defined compression algorithms */
		if (zram->comp_algs[prio] != default_compressor)
			kfree(zram->comp_algs[prio]);
		zram->comp_algs[prio] = NULL;
	}
}

static void zram_reset_device(struct zram *zram)
{
	u64 disksize;

	mutex_lock(&zram->sddc_lifecycle_lock);
	crystal_sddc_stop(zram);
	down_write(&zram->init_lock);

	disksize = zram->disksize;
	zram->limit_pages = 0;

	set_capacity_and_notify(zram->disk, 0);
	part_stat_set_all(zram->disk->part0, 0);

	/* I/O operation under all of CPU are done so let's free */
	zram_meta_free(zram, disksize);
	zram->disksize = 0;
	zram_destroy_comps(zram);
	memset(&zram->stats, 0, sizeof(zram->stats));
	zram->prefetch_last_fault_index = 0;
	zram->prefetch_prev_fault_index = 0;
	zram->prefetch_last_fault_memcg_id = 0;
	zram->prefetch_fault_valid = false;
	reset_bdev(zram);

	comp_algorithm_set(zram, ZRAM_PRIMARY_COMP, default_compressor);
	up_write(&zram->init_lock);
	mutex_unlock(&zram->sddc_lifecycle_lock);
}

static ssize_t disksize_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	u64 disksize, nr_pages;
	struct zcomp *comp;
	struct zram *zram = dev_to_zram(dev);
	int err;
	u32 prio;

	disksize = memparse(buf, NULL);
	if (!disksize)
		return -EINVAL;
	if (disksize > U64_MAX - (PAGE_SIZE - 1))
		return -EOVERFLOW;

	mutex_lock(&zram->sddc_lifecycle_lock);
	down_write(&zram->init_lock);
	if (init_done(zram)) {
		pr_info("Cannot change disksize for initialized device\n");
		err = -EBUSY;
		goto out_unlock;
	}

	disksize = PAGE_ALIGN(disksize);
	if (!disksize) {
		err = -EINVAL;
		goto out_unlock;
	}
	nr_pages = disksize >> PAGE_SHIFT;
	if (!nr_pages || nr_pages > (u64)ULONG_MAX ||
	    nr_pages > (u64)SIZE_MAX / sizeof(*zram->table) ||
	    nr_pages > (u64)UINT_MAX + 1) {
		err = -EOVERFLOW;
		goto out_unlock;
	}
	if (!zram_meta_alloc(zram, disksize)) {
		err = -ENOMEM;
		goto out_unlock;
	}
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
	if (zram->zms) {
		err = zms_set_nr_handles(zram->zms, nr_pages);
		if (err)
			goto out_free_meta;
	}
#endif

	for (prio = 0; prio < ZRAM_MAX_COMPS; prio++) {
		if (!zram->comp_algs[prio])
			continue;

		comp = zcomp_create(zram->comp_algs[prio]);
		if (IS_ERR(comp)) {
			pr_err("Cannot initialise %s compressing backend\n",
			       zram->comp_algs[prio]);
			err = PTR_ERR(comp);
			goto out_free_comps;
		}

		zram->comps[prio] = comp;
		zram->num_active_comps++;
	}
	err = crystal_sddc_create(zram, nr_pages);
	if (err)
		goto out_free_comps;
	zram->disksize = disksize;
	set_capacity_and_notify(zram->disk, zram->disksize >> SECTOR_SHIFT);
	up_write(&zram->init_lock);
	mutex_unlock(&zram->sddc_lifecycle_lock);

	return len;

out_free_comps:
	zram_destroy_comps(zram);
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
out_free_meta:
#endif
	zram_meta_free(zram, disksize);
out_unlock:
	up_write(&zram->init_lock);
	mutex_unlock(&zram->sddc_lifecycle_lock);
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

	/* Make sure all the pending I/O are finished */
	sync_blockdev(disk->part0);
	zram_reset_device(zram);

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

static const struct block_device_operations zram_devops = {
	.open = zram_open,
	.submit_bio = zram_submit_bio,
	.swap_slot_free_notify = zram_slot_free_notify,
	.owner = THIS_MODULE
};

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
static ssize_t prefetch_enable_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(zram->prefetch_enable));
}

static ssize_t prefetch_enable_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;
	WRITE_ONCE(zram->prefetch_enable, enable);
	return len;
}

static ssize_t prefetch_degree_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(zram->prefetch_degree));
}

static ssize_t prefetch_degree_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	unsigned int degree;
	int ret;

	ret = kstrtouint(buf, 10, &degree);
	if (ret)
		return ret;
	if (degree > ZRAM_ZMS_PREFETCH_MAX)
		return -EINVAL;
	WRITE_ONCE(zram->prefetch_degree, degree);
	return len;
}

static ssize_t prefetch_ttl_ms_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return sysfs_emit(buf, "%lu\n", READ_ONCE(zram->prefetch_ttl_ms));
}

static ssize_t prefetch_ttl_ms_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	unsigned long ttl_ms;
	int ret;

	ret = kstrtoul(buf, 10, &ttl_ms);
	if (ret)
		return ret;
	if (ttl_ms < ZRAM_PREFETCH_TTL_MIN_MS ||
	    ttl_ms > ZRAM_PREFETCH_TTL_MAX_MS)
		return -EINVAL;
	WRITE_ONCE(zram->prefetch_ttl_ms, ttl_ms);
	return len;
}
#endif

static DEVICE_ATTR_WO(compact);
static DEVICE_ATTR_RW(disksize);
static DEVICE_ATTR_RO(initstate);
static DEVICE_ATTR_WO(reset);
static DEVICE_ATTR_WO(mem_limit);
static DEVICE_ATTR_WO(mem_used_max);
static DEVICE_ATTR_WO(idle);
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_EMPTY_APIS
static DEVICE_ATTR_RW(max_comp_streams);
#endif
static DEVICE_ATTR_RW(comp_algorithm);
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
static DEVICE_ATTR_RW(backing_dev);
static DEVICE_ATTR_WO(writeback);
static DEVICE_ATTR_RW(writeback_limit);
static DEVICE_ATTR_RW(writeback_limit_enable);
static DEVICE_ATTR_RW(prefetch_enable);
static DEVICE_ATTR_RW(prefetch_degree);
static DEVICE_ATTR_RW(prefetch_ttl_ms);
#endif
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_MULTI_COMP
static DEVICE_ATTR_RW(recomp_algorithm);
static DEVICE_ATTR_WO(recompress);
#endif

static struct attribute *zram_disk_attrs[] = {
	&dev_attr_disksize.attr,
	&dev_attr_initstate.attr,
	&dev_attr_reset.attr,
	&dev_attr_compact.attr,
	&dev_attr_mem_limit.attr,
	&dev_attr_mem_used_max.attr,
	&dev_attr_idle.attr,
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_LEGACY_EMPTY_APIS
	&dev_attr_max_comp_streams.attr,
#endif
	&dev_attr_comp_algorithm.attr,
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
	&dev_attr_backing_dev.attr,
	&dev_attr_writeback.attr,
	&dev_attr_writeback_limit.attr,
	&dev_attr_writeback_limit_enable.attr,
	&dev_attr_prefetch_enable.attr,
	&dev_attr_prefetch_degree.attr,
	&dev_attr_prefetch_ttl_ms.attr,
#endif
	&dev_attr_io_stat.attr,
	&dev_attr_mm_stat.attr,
	&dev_attr_sddc_stat.attr,
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
	&dev_attr_bd_stat.attr,
	&dev_attr_zms_stat.attr,
	&dev_attr_writeback_cold_stat.attr,
#endif
	&dev_attr_debug_stat.attr,
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_MULTI_COMP
	&dev_attr_recomp_algorithm.attr,
	&dev_attr_recompress.attr,
#endif
	NULL,
};

ATTRIBUTE_GROUPS(zram_disk);

/*
 * Allocate and initialize new zram device. the function returns
 * '>= 0' device_id upon success, and negative value otherwise.
 */
static int zram_add(void)
{
	struct zram *zram;
	int ret, device_id;

	zram = kzalloc(sizeof(struct zram), GFP_KERNEL);
	if (!zram)
		return -ENOMEM;

	ret = idr_alloc(&zram_index_idr, zram, 0, 0, GFP_KERNEL);
	if (ret < 0)
		goto out_free_dev;
	device_id = ret;

	init_rwsem(&zram->init_lock);
	mutex_init(&zram->sddc_lifecycle_lock);
	spin_lock_init(&zram->sddc_lock);
	spin_lock_init(&zram->ref_lock);
	spin_lock_init(&zram->memcg_stats_lock);
	hash_init(zram->memcg_stats_table);
	refcount_set(&zram->refcount, 1);
	init_completion(&zram->ref_completion);
	zram->removing = false;
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
	spin_lock_init(&zram->wb_limit_lock);
	INIT_WORK(&zram->zms_gc_work, zram_zms_gc_workfn);
	INIT_DELAYED_WORK(&zram->zms_gc_periodic_work,
			  zram_zms_gc_periodic_workfn);
	atomic_set(&zram->zms_gc_pending, 0);
	zram->zms_gc_stopping = true;
	zram->prefetch_enable = true;
	zram->prefetch_degree = ZRAM_PREFETCH_DEFAULT_DEGREE;
	zram->prefetch_ttl_ms = ZRAM_PREFETCH_DEFAULT_TTL_MS;
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
	snprintf(zram->disk->disk_name, DISK_NAME_LEN, "zram%d", device_id);

	comp_algorithm_set(zram, ZRAM_PRIMARY_COMP, default_compressor);

	/* Actual capacity set using sysfs (/sys/block/zram<id>/disksize */
	set_capacity(zram->disk, 0);
	/* zram devices sort of resembles non-rotational disks */
	blk_queue_flag_set(QUEUE_FLAG_NONROT, zram->disk->queue);
	blk_queue_flag_set(QUEUE_FLAG_SYNCHRONOUS, zram->disk->queue);
	blk_queue_write_cache(zram->disk->queue, true, false);

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
	 * size.  So we can't ensure that all discarded logical blocks are
	 * zeroed.
	 */
	if (ZRAM_LOGICAL_BLOCK_SIZE == PAGE_SIZE)
		blk_queue_max_write_zeroes_sectors(zram->disk->queue, UINT_MAX);

	blk_queue_flag_set(QUEUE_FLAG_STABLE_WRITES, zram->disk->queue);
	ret = device_add_disk(NULL, zram->disk, zram_disk_groups);
	if (ret)
		goto out_cleanup_disk;

	ret = crystal_hybridswap_private_zram_register(disk_to_dev(zram->disk),
		zram);
	if (ret)
		goto out_del_disk;

	zram_debugfs_register(zram);
	pr_info("Added device: %s\n", zram->disk->disk_name);
	return device_id;

out_del_disk:
	del_gendisk(zram->disk);
out_cleanup_disk:
	put_disk(zram->disk);
out_free_idr:
	idr_remove(&zram_index_idr, device_id);
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

	zram_begin_remove(zram);
	zram_debugfs_unregister(zram);
	crystal_hybridswap_private_zram_unregister(disk_to_dev(zram->disk));
	zram_wait_for_refs(zram);

	if (claimed) {
		/*
		 * If we were claimed by reset_store(), del_gendisk() will
		 * wait until reset_store() is done, so nothing need to do.
		 */
		;
	} else {
		/* Make sure all the pending I/O are finished */
		sync_blockdev(zram->disk->part0);
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
	zram_reset_device(zram);

	put_disk(zram->disk);
	kfree(zram);
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
	cpuhp_remove_multi_state(zcomp_cpuhp_state);
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
	if (zram_read_wq) {
		destroy_workqueue(zram_read_wq);
		zram_read_wq = NULL;
	}
#endif
}

int zram_driver_init(void)
{
	int ret;

	BUILD_BUG_ON(__NR_ZRAM_PAGEFLAGS > BITS_PER_LONG);

	ret = cpuhp_setup_state_multi(CPUHP_BP_PREPARE_DYN,
				      "block/zram:prepare",
				      zcomp_cpu_up_prepare,
				      zcomp_cpu_dead);
	if (ret < 0)
		return ret;
	zcomp_cpuhp_state = ret;

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
	zram_read_wq = alloc_workqueue("zram-read",
				       WQ_UNBOUND | WQ_HIGHPRI |
				       WQ_MEM_RECLAIM,
				       zram_read_wq_max_active());
	if (!zram_read_wq) {
		cpuhp_remove_multi_state(zcomp_cpuhp_state);
		return -ENOMEM;
	}
#endif

	ret = class_register(&zram_control_class);
	if (ret) {
		pr_err("Unable to register zram-control class\n");
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
		destroy_workqueue(zram_read_wq);
		zram_read_wq = NULL;
#endif
		cpuhp_remove_multi_state(zcomp_cpuhp_state);
		return ret;
	}

	zram_debugfs_create();
	zram_major = register_blkdev(0, "zram");
	if (zram_major <= 0) {
		pr_err("Unable to get major number\n");
		zram_debugfs_destroy();
		class_unregister(&zram_control_class);
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
		destroy_workqueue(zram_read_wq);
		zram_read_wq = NULL;
#endif
		cpuhp_remove_multi_state(zcomp_cpuhp_state);
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

	return 0;

out_error:
	destroy_devices();
	return ret;
}

void zram_driver_exit(void)
{
	destroy_devices();
}

module_param(num_devices, uint, 0);
MODULE_PARM_DESC(num_devices, "Number of pre-created zram devices");
