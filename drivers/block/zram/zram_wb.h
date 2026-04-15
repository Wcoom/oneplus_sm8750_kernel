/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _ZRAM_WRITEBACK_H_
#define _ZRAM_WRITEBACK_H_

#include <linux/bio.h>
#include <linux/llist.h>
#include "zram_drv.h"

/* 定义最大合并数量 */
#define ZRAM_WB_MAX_BATCH_SIZE 64
/* 严格连续分配的最小单位（页） */
#define ZRAM_WB_MIN_ALLOC_UNIT 4

/* 单个页面请求的元数据 */
struct zram_wb_sub_req {
	struct zram_pp_slot *pps;
	unsigned long blk_idx;  /* 物理块索引 */
	unsigned long index;    /* ZRAM 逻辑索引 (table index) */
	unsigned long expected_handle;
	unsigned long expected_flags;
	u32 expected_size;
};

/* 
 * 批次请求结构
 * 这个结构体将驻留在 bio 的 front_pad 区域中
 */
struct zram_wb_batch_request {
	struct zram *zram;
	struct zram_pp_ctl *ppctl;
	struct bio *bio;
	struct llist_node node;
	
	/* 当前批次中包含的有效子请求数量 */
	unsigned int count;
	u64 reserved_wb_units;
	
	/* 记录每个页面的元数据，用于回调时释放资源 */
	struct zram_wb_sub_req sub_reqs[ZRAM_WB_MAX_BATCH_SIZE];
};

struct zram_wb_request_list {
	struct llist_head head;
};

#if IS_ENABLED(CONFIG_ZRAM_WRITEBACK)
unsigned long alloc_block_bdev_batch(struct zram *zram, int req_count, int *act_count);
void free_block_bdev(struct zram *zram, unsigned long blk_idx);
void free_block_bdev_range(struct zram *zram, unsigned long blk_idx, int count);

struct zram_wb_batch_request *alloc_wb_batch_request(struct zram *zram,
					     struct zram_pp_ctl *ctl,
					     unsigned long start_blk_idx,
					     gfp_t gfp_mask);

void zram_wb_submit_batch(struct zram_wb_batch_request *req);
void zram_wb_wait_for_idle(struct zram *zram);

int setup_zram_writeback(void);
void destroy_zram_writeback(void);
#else
inline unsigned long alloc_block_bdev_batch(struct zram *zram, int req_count, int *act_count) { return 0; }
inline void free_block_bdev(struct zram *zram, unsigned long blk_idx) {};
inline void free_block_bdev_range(struct zram *zram, unsigned long blk_idx, int count) {};
inline void zram_wb_submit_batch(struct zram_wb_batch_request *req) {}
inline void zram_wb_wait_for_idle(struct zram *zram) {}
inline int setup_zram_writeback(void) { return 0; }
inline void destroy_zram_writeback(void) {}
#endif

#endif /* _ZRAM_WRITEBACK_H_ */
