// SPDX-License-Identifier: GPL-2.0-or-later

#define KMSG_COMPONENT "zram_wb"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/blkdev.h>
#include <linux/bitops.h>

#include "zram_wb.h"

static struct task_struct *wb_thread;
static DECLARE_WAIT_QUEUE_HEAD(wb_wq);
static struct zram_wb_request_list wb_submit_req_list;
static struct zram_wb_request_list wb_complete_req_list;
static struct bio_set zram_wb_bs;

static void enqueue_wb_request(struct zram_wb_request_list *req_list,
			       struct zram_wb_batch_request *req);

static void zram_wb_put_inflight(struct zram *zram)
{
	if (atomic_dec_and_test(&zram->wb_inflight))
		wake_up_all(&zram->wb_done_wait);
}

void zram_wb_submit_batch(struct zram_wb_batch_request *req)
{
	atomic_inc(&req->zram->wb_inflight);
	enqueue_wb_request(&wb_submit_req_list, req);
	wake_up(&wb_wq);
}

void zram_wb_wait_for_idle(struct zram *zram)
{
	wait_event(zram->wb_done_wait, !atomic_read(&zram->wb_inflight));
}

static void zram_wb_release_bio_pages(struct zram *zram, struct bio *bio)
{
	struct bio_vec *bv;
	struct bvec_iter_all iter;

	bio_for_each_segment_all(bv, bio, iter)
		mempool_free(bv->bv_page, zram->wb_page_pool);
}

/* 
 * front_pad: 在 bio 结构之前预留空间存放 zram_wb_batch_request
 * 这个结构现在比较大 (包含数组)，必须确保 bio 对齐
 */
#define ZRAM_WB_FRONT_PAD \
	roundup(sizeof(struct zram_wb_batch_request), __alignof__(struct bio))

/*
 * 从 bio 指针获取其前面的 zram_wb_batch_request 结构
 */
#define bio_to_wb_batch(bio) \
	((struct zram_wb_batch_request *)((char *)(bio) - ZRAM_WB_FRONT_PAD))

/*
 * O(1) 复杂度的后备设备块批量分配
 * req_count: 请求分配的块数 (例如 64)
 * act_count: 输出参数，实际分配的块数
 */
unsigned long alloc_block_bdev_batch(struct zram *zram, int req_count, int *act_count)
{
	unsigned long blk_idx;
	unsigned long start;
	int count = 0;
	int align_pages;
	unsigned long align_mask = 0;

	spin_lock(&zram->bitmap_lock);
	start = max(zram->bitmap_last_free_hint, 1UL);
	align_pages = (128 * 1024) / PAGE_SIZE;
	if (align_pages < 1)
		align_pages = 1;
	if (align_pages > req_count)
		align_pages = req_count;

	while (align_pages > 1 && (align_pages & (align_pages - 1)))
		align_pages &= (align_pages - 1);

	if (align_pages > 1)
		align_mask = align_pages - 1;

	blk_idx = bitmap_find_next_zero_area(zram->bitmap, zram->nr_pages,
					     start, req_count,
					     align_mask);
	if (blk_idx >= zram->nr_pages && start > 1) {
		blk_idx = bitmap_find_next_zero_area(zram->bitmap, zram->nr_pages,
						     1, req_count,
						     align_mask);
	}

	if (blk_idx < zram->nr_pages) {
		count = req_count;
		bitmap_set(zram->bitmap, blk_idx, count);
		zram->bitmap_last_free_hint = blk_idx + count;
		goto out_unlock;
	}

	/* Try full contiguous extent without alignment constraints. */
	blk_idx = bitmap_find_next_zero_area(zram->bitmap, zram->nr_pages,
					     start, req_count, 0);
	if (blk_idx >= zram->nr_pages && start > 1)
		blk_idx = bitmap_find_next_zero_area(zram->bitmap, zram->nr_pages,
						     1, req_count, 0);

	if (blk_idx < zram->nr_pages) {
		count = req_count;
		bitmap_set(zram->bitmap, blk_idx, count);
		zram->bitmap_last_free_hint = blk_idx + count;
		goto out_unlock;
	}

	/* 寻找第一个空闲块（不要求对齐），强制从索引 1 开始以避开 block 0 */
	blk_idx = bitmap_find_next_zero_area(zram->bitmap, zram->nr_pages,
					     start, 1, 0);

	if (blk_idx >= zram->nr_pages && zram->bitmap_last_free_hint > 1) {
		blk_idx = bitmap_find_next_zero_area(zram->bitmap, zram->nr_pages,
						     1, 1, 0);
	}

	if (blk_idx < zram->nr_pages) {
		unsigned long search_end;
		unsigned long next_used_idx;

		/*
		 * Fallback path: derive free run length by finding the next used bit
		 * instead of probing each bit in a loop under bitmap_lock.
		 */
		search_end = min(zram->nr_pages, blk_idx + (unsigned long)req_count);
		next_used_idx = find_next_bit(zram->bitmap, search_end, blk_idx + 1);
		count = (int)(next_used_idx - blk_idx);
		
		bitmap_set(zram->bitmap, blk_idx, count);
		zram->bitmap_last_free_hint = blk_idx + count;
	} else {
		blk_idx = 0;
	}

	out_unlock:
	spin_unlock(&zram->bitmap_lock);

	if (blk_idx) {
		atomic64_add(count, &zram->stats.bd_count);
		if (act_count)
			*act_count = count;
	} else {
		if (act_count)
			*act_count = 0;
	}

	return blk_idx;
}

/* 批量释放辅助函数，使用 spinlock 保护 */
void free_block_bdev_range(struct zram *zram, unsigned long blk_idx, int count)
{
	spin_lock(&zram->bitmap_lock);
	bitmap_clear(zram->bitmap, blk_idx, count);
	if (blk_idx < zram->bitmap_last_free_hint)
		zram->bitmap_last_free_hint = blk_idx;
	spin_unlock(&zram->bitmap_lock);

	atomic64_sub(count, &zram->stats.bd_count);
}

/* 保持原有单块释放函数的兼容性 */
void free_block_bdev(struct zram *zram, unsigned long blk_idx)
{
	free_block_bdev_range(zram, blk_idx, 1);
}


/*
 * 处理完成的 BIO 批次
 * 这是一个核心函数，负责批量释放资源
 */
static bool wb_slot_can_commit(struct zram *zram,
			      struct zram_wb_sub_req *sub)
{
	unsigned long flags = zram->table[sub->index].flags;

	if (!(flags & BIT(ZRAM_PP_SLOT)))
		return false;

	if (flags & BIT(ZRAM_WB))
		return false;

	if (!!(flags & BIT(ZRAM_SAME)) != !!(sub->expected_flags & BIT(ZRAM_SAME)))
		return false;

	if (!!(flags & BIT(ZRAM_HUGE)) != !!(sub->expected_flags & BIT(ZRAM_HUGE)))
		return false;

	if (zram->table[sub->index].handle != sub->expected_handle)
		return false;

	if (!(sub->expected_flags & BIT(ZRAM_SAME)) &&
	    (zram->table[sub->index].flags & (BIT(ZRAM_FLAG_SHIFT) - 1)) !=
	    sub->expected_size)
		return false;

	return true;
}

static void complete_wb_batch(struct zram_wb_batch_request *req)
{
	struct zram *zram = req->zram;
	struct zram_pp_ctl *ctl = req->ppctl;
	struct bio *bio = req->bio;
	bool io_error = bio->bi_status != BLK_STS_OK;
	int i;
	int success_count = 0;
	u64 used_wb_units = 0;
	u64 refund_wb_units = 0;
	unsigned long rollback_start = 0;
	int rollback_count = 0;

	if (unlikely(io_error)) {
		for (i = 0; i < req->count; i++) {
			struct zram_wb_sub_req *sub = &req->sub_reqs[i];
			unsigned long blk_idx = sub->blk_idx;
			struct zram_pp_slot *pps = sub->pps;

			if (!rollback_count) {
				rollback_start = blk_idx;
				rollback_count = 1;
			} else if (blk_idx == rollback_start + rollback_count) {
				rollback_count++;
			} else {
				free_block_bdev_range(zram, rollback_start, rollback_count);
				rollback_start = blk_idx;
				rollback_count = 1;
			}

			free_pp_slot(zram, pps);
		}

		if (rollback_count)
			free_block_bdev_range(zram, rollback_start, rollback_count);

		goto finalize_batch;
	}

	/* 遍历批次中的每一个子请求 */
	for (i = 0; i < req->count; i++) {
		struct zram_wb_sub_req *sub = &req->sub_reqs[i];
		unsigned long index = sub->index;
		unsigned long blk_idx = sub->blk_idx;
		struct zram_pp_slot *pps = sub->pps;

		/* 锁定槽位进行状态变更 */
		zram_slot_lock(zram, index);
		if (!wb_slot_can_commit(zram, sub)) {
			zram_slot_unlock(zram, index);
			goto handle_err;
		}

		/* 成功路径：释放内存页，设置写回标志 */
		zram_free_page(zram, index);
		zram_set_flag(zram, index, ZRAM_WB);
		zram_set_flag(zram, index, ZRAM_WB_READ_ONCE);
		zram_set_handle(zram, index, blk_idx);
		atomic64_inc(&zram->stats.bd_writes);

		zram_clear_flag(zram, index, ZRAM_PP_SLOT);
		zram_slot_unlock(zram, index);
		
		success_count++;
		kfree(pps);
		continue;

handle_err:
		/* 失败路径：回滚块分配，保留 ZRAM 内存页 */
		free_block_bdev(zram, blk_idx);
		free_pp_slot(zram, pps);
	}

finalize_batch:

	if (success_count > 0) {
		percpu_counter_add(&zram->stats.pages_stored, success_count);
	}

	used_wb_units = (u64)success_count * (1ULL << (PAGE_SHIFT - 12));
	if (req->reserved_wb_units > used_wb_units)
		refund_wb_units = req->reserved_wb_units - used_wb_units;

	if (refund_wb_units) {
		spin_lock(&zram->wb_limit_lock);
		zram->bd_wb_limit += refund_wb_units;
		spin_unlock(&zram->wb_limit_lock);
	}

	/*
	 * 重要：ctl->num_pp_slots 记录了待处理的总数
	 * 此时减少当前批次处理的数量 (req->count)
	 * 异步 Shrinker 模式下 ctl 为 NULL。
	 */
	if (ctl) {
		if (atomic_sub_and_test(req->count, &ctl->num_pp_slots))
			complete(&ctl->all_done);
	}

	/* 释放 BIO 及其挂载的所有 pages */
	zram_wb_release_bio_pages(zram, bio);
	/* bio_put 会释放 bio 内存以及 front_pad */
	bio_put(bio);
	zram_wb_put_inflight(zram);
}

static void enqueue_wb_request(struct zram_wb_request_list *req_list,
			       struct zram_wb_batch_request *req)
{
	llist_add(&req->node, &req_list->head);
}

static struct llist_node *dequeue_wb_requests(
	struct zram_wb_request_list *req_list)
{
	return llist_del_all(&req_list->head);
}

static void complete_wb_requests(struct llist_node *head)
{
	struct llist_node *node, *next;

	head = llist_reverse_order(head);
	llist_for_each_safe(node, next, head) {
		struct zram_wb_batch_request *req;

		req = llist_entry(node, struct zram_wb_batch_request, node);
		complete_wb_batch(req);
	}
}

static void submit_wb_requests(struct llist_node *head)
{
	struct blk_plug plug;
	struct llist_node *node, *next;

	blk_start_plug(&plug);
	head = llist_reverse_order(head);
	llist_for_each_safe(node, next, head) {
		struct zram_wb_batch_request *req;

		req = llist_entry(node, struct zram_wb_batch_request, node);
		if (unlikely(atomic_read(&req->zram->quiescing))) {
			req->bio->bi_status = BLK_STS_IOERR;
			complete_wb_batch(req);
			continue;
		}

		submit_bio(req->bio);
	}
	blk_finish_plug(&plug);
}

static void destroy_wb_request_list(struct zram_wb_request_list *req_list)
{
	struct llist_node *head, *node, *next;

	while ((head = dequeue_wb_requests(req_list)) != NULL) {
		head = llist_reverse_order(head);
		llist_for_each_safe(node, next, head) {
			struct zram_wb_batch_request *req;
			int i;

			req = llist_entry(node, struct zram_wb_batch_request, node);
			for (i = 0; i < req->count; i++) {
				free_block_bdev(req->zram, req->sub_reqs[i].blk_idx);
				free_pp_slot(req->zram, req->sub_reqs[i].pps);
			}

			/* Free pages and bio */
			zram_wb_release_bio_pages(req->zram, req->bio);
			bio_put(req->bio);
			zram_wb_put_inflight(req->zram);
		}
	}
}

static bool wb_ready_to_run(void)
{
	return !llist_empty(&wb_submit_req_list.head) ||
	       !llist_empty(&wb_complete_req_list.head);
}

static int wb_thread_func(void *data)
{
	unsigned int nofs_flags;

	nofs_flags = memalloc_noreclaim_save();

	while (!kthread_should_stop()) {
		wait_event(wb_wq, wb_ready_to_run() || kthread_should_stop());

		while (1) {
			struct llist_node *head;
			bool did_work = false;

			head = dequeue_wb_requests(&wb_complete_req_list);
			if (head) {
				did_work = true;
				complete_wb_requests(head);
			}

			head = dequeue_wb_requests(&wb_submit_req_list);
			if (head) {
				did_work = true;
				submit_wb_requests(head);
			}

			if (!did_work)
				break;
		}
	}

	memalloc_noreclaim_restore(nofs_flags);
	return 0;
}

static void zram_writeback_end_io(struct bio *bio)
{
	struct zram_wb_batch_request *req = bio_to_wb_batch(bio);
	enqueue_wb_request(&wb_complete_req_list, req);
	wake_up(&wb_wq);
}

/*
 * 外部接口：分配一个新的批次请求
 */
struct zram_wb_batch_request *alloc_wb_batch_request(struct zram *zram,
						     struct zram_pp_ctl *ctl,
						     unsigned long start_blk_idx,
						     gfp_t gfp_mask)
{
	struct bio *bio;
	struct zram_wb_batch_request *req;

	/*
	 * 使用 bioset 分配 bio。
	 * ZRAM_WB_MAX_BATCH_SIZE 定义了 bio_vec 的最大数量。
	 * front_pad 会自动被 bio_alloc 分配在 bio 之前。
	 */
	bio = bio_alloc_bioset(zram->bdev, ZRAM_WB_MAX_BATCH_SIZE,
			       REQ_OP_WRITE, gfp_mask,
			       &zram_wb_bs);
	if (!bio)
		return NULL;

	/* 获取前面预留的结构体 */
	req = bio_to_wb_batch(bio);
	req->zram = zram;
	req->ppctl = ctl;
	req->bio = bio;
	req->node.next = NULL;
	req->count = 0; /* 初始计数为 0 */
	req->reserved_wb_units = 0;

	/* 设置 bio 的起始扇区和回调 */
	bio->bi_iter.bi_sector = start_blk_idx * (PAGE_SIZE >> 9);
	bio->bi_end_io = zram_writeback_end_io;
	
	return req;
}

int setup_zram_writeback(void)
{
	/*
	 * 初始化 bioset:
	 * pool_size: 64 (缓冲池大小)
	 * front_pad: ZRAM_WB_FRONT_PAD (包含我们的 zram_wb_batch_request)
	 */
	if (bioset_init(&zram_wb_bs, 64, ZRAM_WB_FRONT_PAD, BIOSET_NEED_BVECS)) {
		pr_err("Unable to init zram_wb_bs\n");
		return -1;
	}

	init_llist_head(&wb_submit_req_list.head);
	init_llist_head(&wb_complete_req_list.head);

	wb_thread = kthread_run(wb_thread_func, NULL, "zram_wb_thread");
	if (IS_ERR(wb_thread)) {
		pr_err("Unable to create zram_wb_thread\n");
		bioset_exit(&zram_wb_bs); 
		return -1;
	}
	return 0;
}

void destroy_zram_writeback(void)
{
	if (wb_thread) {
		wake_up_all(&wb_wq);
		kthread_stop(wb_thread);
		wb_thread = NULL;
	}
	destroy_wb_request_list(&wb_submit_req_list);
	destroy_wb_request_list(&wb_complete_req_list);
	bioset_exit(&zram_wb_bs);
}
