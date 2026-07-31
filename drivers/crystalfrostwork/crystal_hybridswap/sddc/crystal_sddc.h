/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _CRYSTAL_SDDC_H_
#define _CRYSTAL_SDDC_H_

#include <linux/kconfig.h>
#include <linux/types.h>

struct crystal_sddc;
struct zram;

struct crystal_sddc_job_key {
	u64 mutation_seq;
	unsigned long handle;
	u32 index;
	u32 size;
	u8 prio;
};

#if IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC)
int crystal_sddc_create(struct zram *zram, unsigned long nr_pages);
void crystal_sddc_stop(struct zram *zram);
void crystal_sddc_destroy(struct zram *zram);

void crystal_sddc_slot_free_locked(struct zram *zram, u32 index);
void crystal_sddc_slot_stored_locked(struct zram *zram, u32 index);
void crystal_sddc_job_key_locked(struct zram *zram, u32 index,
		struct crystal_sddc_job_key *key);
void crystal_sddc_queue_observation(struct zram *zram,
		const struct crystal_sddc_job_key *key);
#else
static inline int crystal_sddc_create(struct zram *zram,
		unsigned long nr_pages)
{
	return 0;
}

static inline void crystal_sddc_stop(struct zram *zram) { }
static inline void crystal_sddc_destroy(struct zram *zram) { }
static inline void crystal_sddc_slot_free_locked(struct zram *zram,
		u32 index) { }
static inline void crystal_sddc_slot_stored_locked(struct zram *zram,
		u32 index) { }
static inline void crystal_sddc_job_key_locked(struct zram *zram, u32 index,
		struct crystal_sddc_job_key *key) { }
static inline void crystal_sddc_queue_observation(struct zram *zram,
		const struct crystal_sddc_job_key *key) { }
#endif

#endif /* _CRYSTAL_SDDC_H_ */
