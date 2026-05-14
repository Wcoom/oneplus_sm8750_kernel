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
#include <linux/idr.h>
#include <linux/sysfs.h>
#include <linux/debugfs.h>
#include <linux/cpuhotplug.h>
#include <linux/part_stat.h>

#include "zram_drv.h"
#include "crystal_hybridswap_internal.h"

static DEFINE_IDR(zram_index_idr);
/* idr index must be protected */
static DEFINE_MUTEX(zram_index_mutex);

static int zram_major;
enum cpuhp_state zcomp_cpuhp_state = CPUHP_INVALID;
static const char *default_compressor = CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_DEF_COMP;

/* Module params (documentation at end) */
static unsigned int num_devices = 1;
/*
 * Pages that compress to sizes equals or greater than this are stored
 * uncompressed in memory.
 */
static size_t huge_class_size;

static const struct block_device_operations zram_devops;

static void zram_free_page(struct zram *zram, size_t index);
static int zram_read_page(struct zram *zram, struct page *page, u32 index,
			  struct bio *parent);

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
			zram_test_flag(zram, index, ZRAM_WB);
}

struct zram_memcg_account {
	u64 cgroup_id;
	bool allocated;
	bool wb;
	bool same;
	bool huge;
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
		zram_memcg_stats_update(&entry->writeback_size, PAGE_SIZE, add);
	} else {
		zram_memcg_stats_update(&entry->resident_pages, 1, add);
		zram_memcg_stats_update(&entry->zram_original_size, PAGE_SIZE, add);
		if (!account->same)
			zram_compressed_size = account->size ? account->size : PAGE_SIZE;
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

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
static s64 zram_ns_to_s64(u64 ns)
{
	return ns > S64_MAX ? S64_MAX : (s64)ns;
}

static void zram_record_bdev_read(struct zram *zram, u32 index,
		unsigned long entry, u64 ns, int ret, bool sync)
{
	sector_t sector = entry * (PAGE_SIZE >> 9);
	s64 ns_s64 = zram_ns_to_s64(ns);

	if (!sync) {
		/*
		 * Async backing reads are chained to the parent zram bio. Avoid
		 * wrapping the child bi_end_io here so parent completion semantics stay
		 * unchanged; async counters therefore record submit/setup result only.
		 */
		atomic64_inc(&zram->stats.bd_read_async_ios);
		atomic64_set(&zram->stats.bd_read_last_ret, ret);
		if (ret) {
			atomic64_inc(&zram->stats.bd_read_failures);
			chs_log_ratelimited(CHS_LOG_WARN,
				"backing read submit failed dev=%s index=%u blk=%lu sector=%llu latency_ns=%llu ret=%d sync=%d\n",
				zram->disk ? zram->disk->disk_name : "unknown",
				index, entry, (unsigned long long)sector,
				(unsigned long long)ns, ret, sync);
		}
		return;
	}

	atomic64_inc(&zram->stats.bd_reads);
	atomic64_inc(&zram->stats.bd_read_sync_ios);
	atomic64_add(ns_s64, &zram->stats.bd_read_total_ns);
	zram_atomic64_update_max(&zram->stats.bd_read_max_ns, ns_s64);
	atomic64_set(&zram->stats.bd_read_last_ret, ret);
	if (ret) {
		atomic64_inc(&zram->stats.bd_read_failures);
		chs_log_ratelimited(CHS_LOG_WARN,
			"backing read failed dev=%s index=%u blk=%lu sector=%llu latency_ns=%llu ret=%d sync=%d\n",
			zram->disk ? zram->disk->disk_name : "unknown", index,
			entry, (unsigned long long)sector,
			(unsigned long long)ns, ret, sync);
	}
	if (ns >= CHS_ZRAM_SLOW_IO_NS) {
		atomic64_inc(&zram->stats.bd_read_slow_ios);
		atomic64_set(&zram->stats.bd_read_last_slow_dev,
			zram_dev_id(zram));
		atomic64_set(&zram->stats.bd_read_last_slow_sector, sector);
		atomic64_set(&zram->stats.bd_read_last_slow_index, index);
		atomic64_set(&zram->stats.bd_read_last_slow_ret, ret);
		atomic64_set(&zram->stats.bd_read_last_slow_ns, ns_s64);
		chs_log_ratelimited(CHS_LOG_WARN,
			"slow backing read dev=%s index=%u blk=%lu sector=%llu latency_ns=%llu ret=%d\n",
			zram->disk ? zram->disk->disk_name : "unknown", index,
			entry, (unsigned long long)sector,
			(unsigned long long)ns, ret);
	}
}

static void zram_record_bdev_write(struct zram *zram, u32 index,
		unsigned long entry, u64 ns, int ret)
{
	sector_t sector = entry * (PAGE_SIZE >> 9);
	s64 ns_s64 = zram_ns_to_s64(ns);

	atomic64_inc(&zram->stats.bd_write_ios);
	atomic64_add(ns_s64, &zram->stats.bd_write_total_ns);
	zram_atomic64_update_max(&zram->stats.bd_write_max_ns, ns_s64);
	atomic64_set(&zram->stats.bd_write_last_ret, ret);
	if (ret) {
		atomic64_inc(&zram->stats.bd_write_failures);
		chs_log_ratelimited(CHS_LOG_WARN,
			"backing write failed dev=%s index=%u blk=%lu sector=%llu latency_ns=%llu ret=%d\n",
			zram->disk ? zram->disk->disk_name : "unknown", index,
			entry, (unsigned long long)sector,
			(unsigned long long)ns, ret);
	}
	if (ns >= CHS_ZRAM_SLOW_IO_NS) {
		atomic64_inc(&zram->stats.bd_write_slow_ios);
		atomic64_set(&zram->stats.bd_write_last_slow_dev,
			zram_dev_id(zram));
		atomic64_set(&zram->stats.bd_write_last_slow_sector, sector);
		atomic64_set(&zram->stats.bd_write_last_slow_index, index);
		atomic64_set(&zram->stats.bd_write_last_slow_ret, ret);
		atomic64_set(&zram->stats.bd_write_last_slow_ns, ns_s64);
		chs_log_ratelimited(CHS_LOG_WARN,
			"slow backing write dev=%s index=%u blk=%lu sector=%llu latency_ns=%llu ret=%d\n",
			zram->disk ? zram->disk->disk_name : "unknown", index,
			entry, (unsigned long long)sector,
			(unsigned long long)ns, ret);
	}
}
#endif

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

	bdev = zram->bdev;
	blkdev_put(bdev, zram);
	/* hope filp_close flush all of IO */
	filp_close(zram->backing_dev, NULL);
	zram->backing_dev = NULL;
	zram->bdev = NULL;
	zram->disk->fops = &zram_devops;
	kvfree(zram->bitmap);
	zram->bitmap = NULL;
	zram->nr_pages = 0;
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
	size_t bitmap_sz;
	u64 backing_pages;
	unsigned long bitmap_longs;
	unsigned long nr_pages, *bitmap = NULL;
	struct block_device *bdev = NULL;
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

	if (nr_pages > ULONG_MAX - (BITS_PER_LONG - 1)) {
		err = -EOVERFLOW;
		chs_log(CHS_LOG_ERR,
			"backing_dev bitmap size overflow path=%s pages=%lu late=%d ret=%d\n",
			path, nr_pages, late_bind, err);
		goto out;
	}
	bitmap_longs = BITS_TO_LONGS(nr_pages);
	if (bitmap_longs > SIZE_MAX / sizeof(*bitmap)) {
		err = -EOVERFLOW;
		chs_log(CHS_LOG_ERR,
			"backing_dev bitmap bytes overflow path=%s pages=%lu late=%d ret=%d\n",
			path, nr_pages, late_bind, err);
		goto out;
	}
	bitmap_sz = bitmap_longs * sizeof(*bitmap);
	bitmap = kvzalloc(bitmap_sz, GFP_KERNEL);
	if (!bitmap) {
		err = -ENOMEM;
		chs_log(CHS_LOG_ERR,
			"backing_dev bitmap alloc failed path=%s pages=%lu late=%d ret=%d\n",
			path, nr_pages, late_bind, err);
		goto out;
	}

	zram->bdev = bdev;
	zram->backing_dev = backing_dev;
	zram->bitmap = bitmap;
	zram->nr_pages = nr_pages;
	up_write(&zram->init_lock);

	pr_info("setup backing device %s%s\n", path,
		late_bind ? " by late bind" : "");
	chs_log(CHS_LOG_INFO,
		"%s backing_dev bind success path=%s pages=%lu\n",
		late_bind ? "late" : "early", path, nr_pages);
	kfree(file_name);

	return 0;
out:
	kvfree(bitmap);

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

static unsigned long alloc_block_bdev(struct zram *zram)
{
	unsigned long blk_idx = 1;

	if (!zram->bitmap || zram->nr_pages <= 1)
		return 0;
retry:
	/* skip 0 bit to confuse zram.handle = 0 */
	blk_idx = find_next_zero_bit(zram->bitmap, zram->nr_pages, blk_idx);
	if (blk_idx == zram->nr_pages)
		return 0;

	if (test_and_set_bit(blk_idx, zram->bitmap))
		goto retry;

	atomic64_inc(&zram->stats.bd_count);
	return blk_idx;
}

static void free_block_bdev(struct zram *zram, unsigned long blk_idx)
{
	int was_set;

	if (!zram->bitmap || !blk_idx || blk_idx >= zram->nr_pages) {
		WARN_ON_ONCE(1);
		return;
	}

	was_set = test_and_clear_bit(blk_idx, zram->bitmap);
	WARN_ON_ONCE(!was_set);
	atomic64_dec(&zram->stats.bd_count);
}

static int read_from_bdev_async(struct zram *zram, struct page *page,
			unsigned long entry, struct bio *parent)
{
	struct bio *bio;

	bio = bio_alloc(zram->bdev, 1, REQ_OP_READ, GFP_NOIO);
	if (!bio)
		return -ENOMEM;

	bio->bi_iter.bi_sector = entry * (PAGE_SIZE >> 9);
	__bio_add_page(bio, page, PAGE_SIZE, 0);
	bio_chain(bio, parent);
	submit_bio(bio);
	return 0;
}

#define PAGE_WB_SIG "page_index="

#define PAGE_WRITEBACK			0
#define HUGE_WRITEBACK			(1<<0)
#define IDLE_WRITEBACK			(1<<1)
#define INCOMPRESSIBLE_WRITEBACK	(1<<2)
#define HYBRIDSWAP_NORMAL_WRITEBACK	(1<<3)

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

static int zram_writeback_pages(struct zram *zram, int mode,
				unsigned long index, unsigned long nr_pages,
				unsigned long max_pages, u64 target_cgroup_id,
				struct crystal_hybridswap_writeback_stats *wb_stats)
{
	struct bio bio;
	struct bio_vec bio_vec;
	struct page *page;
	int ret = 0;
	int err;
	unsigned long blk_idx = 0;
	unsigned long written = 0;
	unsigned long scanned = 0;
	unsigned long eligible = 0;
	unsigned long unknown_or_filtered = 0;
	bool per_memcg_force = target_cgroup_id != 0;
	u64 wb_memcg_id;

	if (wb_stats)
		memset(wb_stats, 0, sizeof(*wb_stats));

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		ret = -EINVAL;
		chs_log_ratelimited(CHS_LOG_WARN,
				    "writeback skip init_not_done mode=0x%x ret=%d\n",
				    mode, ret);
		goto release_init_lock;
	}

	if (!zram->backing_dev) {
		ret = -ENXIO;
		chs_log_ratelimited(CHS_LOG_WARN,
				    "writeback skip no backing_dev mode=0x%x index=%lu nr_pages=%lu max_pages=%lu ret=%d\n",
				    mode, index, nr_pages, max_pages, ret);
		goto release_init_lock;
	}

	if (!nr_pages)
		nr_pages = zram_pages_snapshot(zram);
	if (!zram_valid_io_range(zram, index, nr_pages)) {
		ret = -EINVAL;
		chs_log_ratelimited(CHS_LOG_WARN,
			"writeback skip invalid range mode=0x%x index=%lu nr_pages=%lu ret=%d\n",
			mode, index, nr_pages, ret);
		goto release_init_lock;
	}

	page = alloc_page(GFP_KERNEL);
	if (!page) {
		ret = -ENOMEM;
		chs_log(CHS_LOG_ERR, "writeback alloc page failed ret=%d\n", ret);
		goto release_init_lock;
	}

	for (; nr_pages != 0; index++, nr_pages--) {
		if (max_pages && written >= max_pages)
			break;
		scanned++;

		zram_slot_lock(zram, index);
		if (!zram_allocated(zram, index))
			goto next;

		if (zram_test_flag(zram, index, ZRAM_WB) ||
				zram_test_flag(zram, index, ZRAM_SAME) ||
				zram_test_flag(zram, index, ZRAM_UNDER_WB))
			goto next;

		if (target_cgroup_id) {
			u64 slot_memcg_id = zram->table[index].memcg_id;

			if (!slot_memcg_id || slot_memcg_id != target_cgroup_id) {
				unknown_or_filtered++;
				goto next;
			}
		}

		if (mode & IDLE_WRITEBACK &&
		    !zram_test_flag(zram, index, ZRAM_IDLE))
			goto next;
		if (mode & HUGE_WRITEBACK &&
		    !zram_test_flag(zram, index, ZRAM_HUGE))
			goto next;
		if (mode & INCOMPRESSIBLE_WRITEBACK &&
		    !zram_test_flag(zram, index, ZRAM_INCOMPRESSIBLE))
			goto next;
		if (!per_memcg_force && (mode & HYBRIDSWAP_NORMAL_WRITEBACK) &&
		    (zram_test_flag(zram, index, ZRAM_HUGE) ||
		     zram_test_flag(zram, index, ZRAM_INCOMPRESSIBLE)))
			goto next;

		eligible++;

		spin_lock(&zram->wb_limit_lock);
		if (zram->wb_limit_enable && !zram->bd_wb_limit) {
			spin_unlock(&zram->wb_limit_lock);
			ret = -EDQUOT;
			zram_slot_unlock(zram, index);
			chs_log_ratelimited(CHS_LOG_WARN,
					    "writeback skip limit exhausted mode=0x%x index=%lu written=%lu ret=%d\n",
					    mode, index, written, ret);
			break;
		}
		spin_unlock(&zram->wb_limit_lock);

		/*
		 * Clearing ZRAM_UNDER_WB is duty of caller.
		 * IOW, zram_free_page never clear it.
		 */
		zram_set_flag(zram, index, ZRAM_UNDER_WB);
		/* Need for hugepage writeback racing */
		zram_set_flag(zram, index, ZRAM_IDLE);
		zram_slot_unlock(zram, index);

		if (!blk_idx) {
			blk_idx = alloc_block_bdev(zram);
			if (!blk_idx) {
				ret = -ENOSPC;
				zram_slot_lock(zram, index);
				zram_clear_flag(zram, index, ZRAM_UNDER_WB);
				zram_clear_flag(zram, index, ZRAM_IDLE);
				zram_slot_unlock(zram, index);
				chs_log_ratelimited(CHS_LOG_WARN,
						    "writeback skip no backing space mode=0x%x index=%lu written=%lu ret=%d\n",
						    mode, index, written, ret);
				break;
			}
		}

		if (zram_read_page(zram, page, index, NULL)) {
			zram_slot_lock(zram, index);
			zram_clear_flag(zram, index, ZRAM_UNDER_WB);
			zram_clear_flag(zram, index, ZRAM_IDLE);
			zram_slot_unlock(zram, index);
			continue;
		}

		bio_init(&bio, zram->bdev, &bio_vec, 1,
			 REQ_OP_WRITE | REQ_SYNC);
		bio.bi_iter.bi_sector = blk_idx * (PAGE_SIZE >> 9);
		__bio_add_page(&bio, page, PAGE_SIZE, 0);

		/*
		 * Safe baseline: one page per BIO keeps slot lifetime and backing
		 * block ownership simple. TODO: aggregate adjacent blocks only after
		 * multi-page snapshot/UNDER_WB validation preserves current semantics.
		 */
		{
			u64 write_start = ktime_get_ns();

			err = submit_bio_wait(&bio);
			zram_record_bdev_write(zram, index, blk_idx,
				ktime_get_ns() - write_start, err);
		}
		if (err) {
			zram_slot_lock(zram, index);
			zram_clear_flag(zram, index, ZRAM_UNDER_WB);
			zram_clear_flag(zram, index, ZRAM_IDLE);
			zram_slot_unlock(zram, index);
			chs_log_ratelimited(CHS_LOG_ERR,
					    "writeback bio error mode=0x%x index=%lu blk=%lu ret=%d\n",
					    mode, index, blk_idx, err);
			/*
			 * BIO errors are not fatal, we continue and simply
			 * attempt to writeback the remaining objects (pages).
			 * At the same time we need to signal user-space that
			 * some writes (at least one, but also could be all of
			 * them) were not successful and we do so by returning
			 * the most recent BIO error.
			 */
			ret = err;
			continue;
		}

		atomic64_inc(&zram->stats.bd_writes);
		/*
		 * We released zram_slot_lock so need to check if the slot was
		 * changed. If there is freeing for the slot, we can catch it
		 * easily by zram_allocated.
		 * A subtle case is the slot is freed/reallocated/marked as
		 * ZRAM_IDLE again. To close the race, idle_store doesn't
		 * mark ZRAM_IDLE once it found the slot was ZRAM_UNDER_WB.
		 * Thus, we could close the race by checking ZRAM_IDLE bit.
		 */
		zram_slot_lock(zram, index);
		if (!zram_allocated(zram, index) ||
			  !zram_test_flag(zram, index, ZRAM_IDLE)) {
			zram_clear_flag(zram, index, ZRAM_UNDER_WB);
			zram_clear_flag(zram, index, ZRAM_IDLE);
			goto next;
		}

		wb_memcg_id = zram->table[index].memcg_id;
		zram_free_page(zram, index);
		zram_clear_flag(zram, index, ZRAM_UNDER_WB);
		zram_set_flag(zram, index, ZRAM_WB);
		zram_set_element(zram, index, blk_idx);
		zram->table[index].memcg_id = wb_memcg_id;
		blk_idx = 0;
		written++;
		atomic64_inc(&zram->stats.pages_stored);
		zram_memcg_stats_add_current(zram, index);
		spin_lock(&zram->wb_limit_lock);
		if (zram->wb_limit_enable && zram->bd_wb_limit > 0)
			zram->bd_wb_limit -=  1UL << (PAGE_SHIFT - 12);
		spin_unlock(&zram->wb_limit_lock);
next:
		zram_slot_unlock(zram, index);
	}

	if (blk_idx)
		free_block_bdev(zram, blk_idx);
	__free_page(page);
release_init_lock:
	up_read(&zram->init_lock);

	if (wb_stats) {
		wb_stats->scanned_pages = scanned;
		wb_stats->eligible_pages = eligible;
		wb_stats->written_pages = written;
		wb_stats->unknown_or_filtered_pages = unknown_or_filtered;
	}

	if (written)
		crystal_hybridswap_account_writeback_pages(written);

	if (ret)
		chs_log_ratelimited(CHS_LOG_WARN,
				    "writeback finished with error mode=0x%x target_cgroup_id=%llu scan_scope=%s scanned=%lu eligible=%lu written=%lu unknown_or_filtered=%lu max_pages=%lu ret=%d\n",
				    mode, target_cgroup_id,
				    target_cgroup_id ? "per_memcg_best_effort" : "global",
				    scanned, eligible, written, unknown_or_filtered,
				    max_pages, ret);
	else if (written)
		chs_log_ratelimited(CHS_LOG_INFO,
				    "writeback success mode=0x%x target_cgroup_id=%llu scan_scope=%s scanned=%lu eligible=%lu written=%lu unknown_or_filtered=%lu requested=%lu\n",
				    mode, target_cgroup_id,
				    target_cgroup_id ? "per_memcg_best_effort" : "global",
				    scanned, eligible, written, unknown_or_filtered,
				    max_pages);
	else
		chs_log_ratelimited(CHS_LOG_INFO,
				    "writeback skipped no matching pages mode=0x%x target_cgroup_id=%llu scan_scope=%s scanned=%lu eligible=%lu unknown_or_filtered=%lu requested=%lu reason=no_matching_pages\n",
				    mode, target_cgroup_id,
				    target_cgroup_id ? "per_memcg_best_effort" : "global",
				    scanned, eligible, unknown_or_filtered, max_pages);

	if (ret)
		return ret;
	if (!written)
		return 0;
	return written > INT_MAX ? INT_MAX : (int)written;
}

int zram_writeback_device(struct device *dev, const char *mode,
					       unsigned long nr_pages,
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
					 0, stats);
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

int zram_force_writeback_device(struct device *dev, const char *mode,
				       unsigned long nr_pages, u64 target_cgroup_id,
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
					 target_cgroup_id, stats);
	if (!ret)
		return -ENODATA;
	return ret;
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
	stats->bd_pages = atomic64_read(&zram->stats.bd_count);
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

	ret = zram_writeback_pages(zram, mode, index, nr_pages, 0, 0, NULL);
	return ret < 0 ? ret : len;
}

struct zram_work {
	struct work_struct work;
	struct zram *zram;
	unsigned long entry;
	struct page *page;
	int error;
};

static void zram_sync_read(struct work_struct *work)
{
	struct zram_work *zw = container_of(work, struct zram_work, work);
	struct bio_vec bv;
	struct bio bio;

	bio_init(&bio, zw->zram->bdev, &bv, 1, REQ_OP_READ);
	bio.bi_iter.bi_sector = zw->entry * (PAGE_SIZE >> 9);
	__bio_add_page(&bio, zw->page, PAGE_SIZE, 0);
	zw->error = submit_bio_wait(&bio);
}

/*
 * Block layer want one ->submit_bio to be active at a time, so if we use
 * chained IO with parent IO in same context, it's a deadlock. To avoid that,
 * use a worker thread context.
 */
static int read_from_bdev_sync(struct zram *zram, struct page *page,
				unsigned long entry, u32 index)
{
	struct zram_work work;
	u64 start = ktime_get_ns();
	u64 delta;

	work.page = page;
	work.zram = zram;
	work.entry = entry;

	INIT_WORK_ONSTACK(&work.work, zram_sync_read);
	queue_work(system_unbound_wq, &work.work);
	flush_work(&work.work);
	destroy_work_on_stack(&work.work);

	delta = ktime_get_ns() - start;
	zram_record_bdev_read(zram, index, entry, delta, work.error, true);
	return work.error;
}

static int read_from_bdev(struct zram *zram, struct page *page,
			unsigned long entry, struct bio *parent, u32 index)
{
	int ret;

	if (!parent) {
		if (WARN_ON_ONCE(!IS_ENABLED(ZRAM_PARTIAL_IO)))
			return -EIO;
		return read_from_bdev_sync(zram, page, entry, index);
	}
	atomic64_inc(&zram->stats.bd_reads);
	ret = read_from_bdev_async(zram, page, entry, parent);
	/* Async diagnostics below record submit/setup result, not completion. */
	zram_record_bdev_read(zram, index, entry, 0, ret, false);
	return ret;
}
#else
static inline void reset_bdev(struct zram *zram) {};
static int read_from_bdev(struct zram *zram, struct page *page,
			unsigned long entry, struct bio *parent, u32 index)
{
	return -EIO;
}

static void free_block_bdev(struct zram *zram, unsigned long blk_idx) {};
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

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		up_read(&zram->init_lock);
		return -EINVAL;
	}

	zs_compact(zram->mem_pool);
	up_read(&zram->init_lock);

	return len;
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

#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
#define FOUR_K(x) ((x) * (1 << (PAGE_SHIFT - 12)))
static ssize_t bd_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t ret;

	down_read(&zram->init_lock);
	ret = scnprintf(buf, PAGE_SIZE, "%8llu %8llu %8llu\n",
			FOUR_K((u64)atomic64_read(&zram->stats.bd_count)),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_reads)),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_writes)));
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
			"version: %d\n%8llu %8llu\n",
			version,
			(u64)atomic64_read(&zram->stats.writestall),
			(u64)atomic64_read(&zram->stats.miss_free));
	up_read(&zram->init_lock);

	return ret;
}

static DEVICE_ATTR_RO(io_stat);
static DEVICE_ATTR_RO(mm_stat);
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
static DEVICE_ATTR_RO(bd_stat);
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
		zram_free_page(zram, index);
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
static void zram_free_page(struct zram *zram, size_t index)
{
	unsigned long handle;

	zram_memcg_stats_sub_current(zram, index);
	zram->table[index].memcg_id = 0;
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_TRACK_ENTRY_ACTIME
	zram->table[index].ac_time = 0;
#endif
	if (zram_test_flag(zram, index, ZRAM_IDLE))
		zram_clear_flag(zram, index, ZRAM_IDLE);

	if (zram_test_flag(zram, index, ZRAM_HUGE)) {
		zram_clear_flag(zram, index, ZRAM_HUGE);
		atomic64_dec(&zram->stats.huge_pages);
	}

	if (zram_test_flag(zram, index, ZRAM_INCOMPRESSIBLE))
		zram_clear_flag(zram, index, ZRAM_INCOMPRESSIBLE);

	zram_set_priority(zram, index, 0);

	if (zram_test_flag(zram, index, ZRAM_WB)) {
		zram_clear_flag(zram, index, ZRAM_WB);
		free_block_bdev(zram, zram_get_element(zram, index));
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
	if (!handle)
		return;

	zs_free(zram->mem_pool, handle);

	atomic64_sub(zram_get_obj_size(zram, index),
			&zram->stats.compr_data_size);
out:
	atomic64_dec(&zram->stats.pages_stored);
	zram_set_handle(zram, index, 0);
	zram_set_obj_size(zram, index, 0);
	WARN_ON_ONCE(zram->table[index].flags &
		~(1UL << ZRAM_LOCK | 1UL << ZRAM_UNDER_WB));
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

static int zram_read_page(struct zram *zram, struct page *page, u32 index,
			  struct bio *parent)
{
	unsigned long entry;
	int ret;

	zram_slot_lock(zram, index);
	if (!zram_test_flag(zram, index, ZRAM_WB)) {
		/* Slot should be locked through out the function call */
		ret = zram_read_from_zspool(zram, page, index);
		zram_slot_unlock(zram, index);
	} else {
		/*
		 * The slot should be unlocked before reading from the backing
		 * device.  Snapshot the backing block while locked so a concurrent
		 * slot rewrite cannot redirect this read to a different block.
		 */
		entry = zram_get_element(zram, index);
		zram_slot_unlock(zram, index);

		ret = read_from_bdev(zram, page, entry, parent, index);
	}

	/* Should NEVER happen. Return bio error if it does. */
	if (WARN_ON(ret < 0))
		pr_err("Decompression failed! err=%d, page=%u\n", ret, index);

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
};

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
	return ret;
}

static void zram_commit_prepared_page(struct zram *zram, u32 index,
					      struct zram_prepared_page *prep,
					      u64 memcg_id)
{
	/*
	 * Free memory associated with this sector
	 * before overwriting unused sectors.
	 */
	zram_free_page(zram, index);

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
		atomic64_add(prep->comp_len, &zram->stats.compr_data_size);
	}

	zram->table[index].memcg_id = memcg_id;

	/* Update stats */
	atomic64_inc(&zram->stats.pages_stored);
	zram_memcg_stats_add_current(zram, index);
}

static int zram_write_page_with_memcg(struct zram *zram, struct page *page,
		u32 index, u64 memcg_id)
{
	struct zram_prepared_page prep;
	int ret;

	if (!zram_memcg_stats_ensure(zram, memcg_id, GFP_NOIO))
		return -ENOMEM;

	ret = zram_prepare_page(zram, page, &prep);
	if (ret)
		return ret;

	zram_slot_lock(zram, index);
	zram_commit_prepared_page(zram, index, &prep, memcg_id);
	zram_slot_unlock(zram, index);
	return 0;
}

static int zram_write_page(struct zram *zram, struct page *page, u32 index)
{
	return zram_write_page_with_memcg(zram, page, index,
					 zram_page_memcg_id(page));
}

struct zram_wb_snapshot {
	unsigned long element;
	unsigned long flags;
	size_t size;
	u64 memcg_id;
};

static unsigned long zram_snapshot_flags(struct zram *zram, u32 index)
{
	return zram->table[index].flags & ~(1UL << ZRAM_LOCK);
}

static void zram_take_wb_snapshot(struct zram *zram, u32 index,
		struct zram_wb_snapshot *snapshot)
{
	snapshot->element = zram_get_element(zram, index);
	snapshot->flags = zram_snapshot_flags(zram, index);
	snapshot->size = zram_get_obj_size(zram, index);
	snapshot->memcg_id = zram->table[index].memcg_id;
}

static void zram_clear_under_wb(struct zram *zram, u32 index)
{
	zram_slot_lock(zram, index);
	if (zram_test_flag(zram, index, ZRAM_UNDER_WB))
		zram_clear_flag(zram, index, ZRAM_UNDER_WB);
	zram_slot_unlock(zram, index);
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
	return zram_snapshot_flags(zram, index) == snapshot->flags;
}

int zram_batchin_device(struct device *dev, unsigned long max_pages,
		u64 target_cgroup_id,
		struct crystal_hybridswap_batchin_stats *batchin_stats)
{
	struct zram *zram;
	struct page *page;
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

	if (!zram->backing_dev) {
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

	nr_pages = zram_pages_snapshot(zram);
	for (index = 0; index < nr_pages; index++) {
		struct zram_prepared_page prep;
		struct zram_wb_snapshot snapshot;
		unsigned long blk_idx;
		u64 memcg_id;
		int err;

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
		memcg_id = zram->table[index].memcg_id;
		if (target_cgroup_id && memcg_id != target_cgroup_id) {
			zram_slot_unlock(zram, index);
			filtered++;
			cond_resched();
			continue;
		}
		matched++;
		blk_idx = zram_get_element(zram, index);
		zram_set_flag(zram, index, ZRAM_UNDER_WB);
		zram_take_wb_snapshot(zram, index, &snapshot);
		zram_slot_unlock(zram, index);

		/*
		 * Keep batch-in as one synchronous page read per backing block. This
		 * conservative path matches per-slot snapshot validation; future IO
		 * aggregation must preserve the same mismatch handling.
		 */
		err = read_from_bdev_sync(zram, page, blk_idx, index);
		if (err) {
			ret = err;
			read_errors++;
			zram_clear_under_wb(zram, index);
			chs_log_ratelimited(CHS_LOG_ERR,
				"batchin read error dev=%s index=%lu blk=%lu target_cgroup_id=%llu ret=%d\n",
				zram->disk ? zram->disk->disk_name : "unknown",
				index, blk_idx, target_cgroup_id, err);
			cond_resched();
			continue;
		}

		err = zram_prepare_page(zram, page, &prep);
		if (err) {
			ret = err;
			prepare_errors++;
			zram_clear_under_wb(zram, index);
			chs_log_ratelimited(CHS_LOG_ERR,
				"batchin compress error dev=%s index=%lu blk=%lu target_cgroup_id=%llu ret=%d\n",
				zram->disk ? zram->disk->disk_name : "unknown",
				index, blk_idx, target_cgroup_id, err);
			break;
		}

		zram_slot_lock(zram, index);
		if (!zram_wb_snapshot_matches(zram, index, &snapshot)) {
			if (zram_test_flag(zram, index, ZRAM_UNDER_WB))
				zram_clear_flag(zram, index, ZRAM_UNDER_WB);
			zram_slot_unlock(zram, index);
			zram_cleanup_prepared_page(zram, &prep);
			snapshot_mismatch++;
			skipped++;
			chs_log_ratelimited(CHS_LOG_WARN,
				"batchin snapshot mismatch dev=%s index=%lu blk=%lu target_cgroup_id=%llu\n",
				zram->disk ? zram->disk->disk_name : "unknown",
				index, blk_idx, target_cgroup_id);
			cond_resched();
			continue;
		}

		zram_clear_flag(zram, index, ZRAM_UNDER_WB);
		zram_commit_prepared_page(zram, index, &prep, memcg_id);
		zram_slot_unlock(zram, index);
		moved++;
		cond_resched();
	}

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
	zram_free_page(zram, index);
	zram_set_handle(zram, index, handle_new);
	zram_set_obj_size(zram, index, comp_len_new);
	zram_set_priority(zram, index, prio);
	zram->table[index].memcg_id = memcg_id;

	atomic64_add(comp_len_new, &zram->stats.compr_data_size);
	atomic64_inc(&zram->stats.pages_stored);
	zram_memcg_stats_add_current(zram, index);

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
		zram_free_page(zram, index);
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

	do {
		u32 index = iter.bi_sector >> SECTORS_PER_PAGE_SHIFT;
		u32 offset = (iter.bi_sector & (SECTORS_PER_PAGE - 1)) <<
				SECTOR_SHIFT;
		struct bio_vec bv = bio_iter_iovec(bio, iter);

		bv.bv_len = min_t(u32, bv.bv_len, PAGE_SIZE - offset);

		if (zram_bvec_write(zram, &bv, index, offset) < 0) {
			atomic64_inc(&zram->stats.failed_writes);
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

/*
 * Handler function for all zram I/O requests.
 */
static void zram_submit_bio(struct bio *bio)
{
	struct zram *zram = bio->bi_bdev->bd_disk->private_data;

	switch (bio_op(bio)) {
	case REQ_OP_READ:
		zram_bio_read(zram, bio);
		break;
	case REQ_OP_WRITE:
		zram_bio_write(zram, bio);
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

	zram_free_page(zram, index);
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
	reset_bdev(zram);

	comp_algorithm_set(zram, ZRAM_PRIMARY_COMP, default_compressor);
	up_write(&zram->init_lock);
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
	zram->disksize = disksize;
	set_capacity_and_notify(zram->disk, zram->disksize >> SECTOR_SHIFT);
	up_write(&zram->init_lock);

	return len;

out_free_comps:
	zram_destroy_comps(zram);
	zram_meta_free(zram, disksize);
out_unlock:
	up_write(&zram->init_lock);
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
#endif
	&dev_attr_io_stat.attr,
	&dev_attr_mm_stat.attr,
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
	&dev_attr_bd_stat.attr,
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
	spin_lock_init(&zram->ref_lock);
	spin_lock_init(&zram->memcg_stats_lock);
	hash_init(zram->memcg_stats_table);
	refcount_set(&zram->refcount, 1);
	init_completion(&zram->ref_completion);
	zram->removing = false;
#ifdef CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_WRITEBACK
	spin_lock_init(&zram->wb_limit_lock);
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

	ret = class_register(&zram_control_class);
	if (ret) {
		pr_err("Unable to register zram-control class\n");
		cpuhp_remove_multi_state(zcomp_cpuhp_state);
		return ret;
	}

	zram_debugfs_create();
	zram_major = register_blkdev(0, "zram");
	if (zram_major <= 0) {
		pr_err("Unable to get major number\n");
		zram_debugfs_destroy();
		class_unregister(&zram_control_class);
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
