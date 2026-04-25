// SPDX-License-Identifier: GPL-2.0-or-later

#define KMSG_COMPONENT "zram_wb"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/blkdev.h>

#include "zram_wb.h"

static struct bio_set zram_wb_bs;

/* 
 * front_pad: 在 bio 结构之前预留空间存放 zram_wb_request
 * 需要对齐以保证 bio 结构的正确对齐
 */
#define ZRAM_WB_FRONT_PAD \
	roundup(sizeof(struct zram_wb_request), __alignof__(struct bio))

/*
 * 从 bio 指针获取其前面的 zram_wb_request 结构
 * bio 分配时在其前面预留了 ZRAM_WB_FRONT_PAD 字节
 */
#define bio_to_zram_wb_request(bio) \
	((struct zram_wb_request *)((char *)(bio) - ZRAM_WB_FRONT_PAD))

unsigned long alloc_block_bdev(struct zram *zram)
{
	unsigned long blk_idx = 1;
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

void free_block_bdev(struct zram *zram, unsigned long blk_idx)
{
	int was_set;

	was_set = test_and_clear_bit(blk_idx, zram->bitmap);
	WARN_ON_ONCE(!was_set);
	atomic64_dec(&zram->stats.bd_count);
}

static void complete_wb_request(struct zram_wb_request *req)
{
	struct zram *zram = req->zram;
	struct zram_pp_slot *pps = req->pps;
	struct zram_pp_ctl *ctl = req->ppctl;
	unsigned long index = pps->index;
	unsigned long blk_idx = req->blk_idx;
	struct bio *bio = req->bio;

	if (bio->bi_status)
		goto out_err;

	atomic64_inc(&zram->stats.bd_writes);
	zram_slot_lock(zram, index);

	/*
	 * We release slot lock during writeback so slot can change
	 * under us: slot_free() or slot_free() and reallocation
	 * (zram_write_page()). In both cases slot loses
	 * ZRAM_PP_SLOT flag. No concurrent post-processing can set
	 * ZRAM_PP_SLOT on such slots until current post-processing
	 * finishes.
	 */
	if (!zram_test_flag(zram, index, ZRAM_PP_SLOT)) {
		zram_slot_unlock(zram, index);
		goto out_err;
	}

	zram_free_page(zram, index);
	zram_set_flag(zram, index, ZRAM_WB);
	zram_set_handle(zram, index, blk_idx);
	atomic64_inc(&zram->stats.pages_stored);
	spin_lock(&zram->wb_limit_lock);
	if (zram->wb_limit_enable && zram->bd_wb_limit > 0)
		zram->bd_wb_limit -=  1UL << (PAGE_SHIFT - 12);
	spin_unlock(&zram->wb_limit_lock);
	zram_slot_unlock(zram, index);
	goto end;

out_err:
	free_block_bdev(zram, blk_idx);
end:
	free_pp_slot(zram, pps);
	free_wb_request(req);

	if (atomic_dec_and_test(&ctl->num_pp_slots))
		complete(&ctl->all_done);
}

static void enqueue_wb_request(struct zram_wb_request_list *req_list,
			       struct zram_wb_request *req)
{
	/*
	 * The enqueue path comes from softirq context:
	 * blk_done_softirq -> bio_endio -> zram_writeback_end_io
	 * Use spin_lock_bh for locking.
	 */
	spin_lock_bh(&req_list->lock);
	list_add_tail(&req->node, &req_list->head);
	req_list->count++;
	spin_unlock_bh(&req_list->lock);
}

static struct zram_wb_request *dequeue_wb_request(
	struct zram_wb_request_list *req_list)
{
	struct zram_wb_request *req = NULL;

	spin_lock_bh(&req_list->lock);
	if (!list_empty(&req_list->head)) {
		req = list_first_entry(&req_list->head,
				       struct zram_wb_request,
				       node);
		list_del(&req->node);
		req_list->count--;
	}
	spin_unlock_bh(&req_list->lock);

	return req;
}

static void destroy_wb_request_list(struct zram_wb_request_list *req_list)
{
	struct zram_wb_request *req;

	while (!list_empty(&req_list->head)) {
		req = dequeue_wb_request(req_list);
		free_block_bdev(req->zram, req->blk_idx);
		free_pp_slot(req->zram, req->pps);
		free_wb_request(req);
	}
}

static void zram_wb_work(struct work_struct *work)
{
	struct zram *zram = container_of(work, struct zram, wb_work);

	while (1) {
		struct zram_wb_request *req;

		req = dequeue_wb_request(&zram->wb_req_list);
		if (!req)
			break;
		complete_wb_request(req);
	}
}

static void zram_writeback_end_io(struct bio *bio)
{
	struct zram_wb_request *req = bio_to_zram_wb_request(bio);

	enqueue_wb_request(&req->zram->wb_req_list, req);
	queue_work(req->zram->wb_wq, &req->zram->wb_work);
}

struct zram_wb_request *alloc_wb_request(struct zram *zram,
					 struct zram_pp_slot *pps,
					 struct zram_pp_ctl *ppctl,
					 unsigned long blk_idx)
{
	struct zram_wb_request *req;
	struct page *page;
	struct bio *bio;
	int err = 0;

	page = alloc_page(GFP_NOIO | __GFP_NOWARN);
	if (!page)
		return ERR_PTR(-ENOMEM);

	/*
	 * 使用 bioset 分配 bio,front_pad 空间会被自动分配在 bio 之前
	 * 需要 BIOSET_NEED_BVECS 标志,因为我们手动添加 page
	 */
	bio = bio_alloc_bioset(zram->bdev, 1, REQ_OP_WRITE, GFP_NOIO,
			       &zram_wb_bs);
	if (!bio) {
		err = -ENOMEM;
		goto out_free_page;
	}

	/* 通过宏访问 bio 前面的 zram_wb_request 结构 */
	req = bio_to_zram_wb_request(bio);
	req->zram = zram;
	req->pps = pps;
	req->ppctl = ppctl;
	req->blk_idx = blk_idx;
	req->bio = bio;

	bio->bi_iter.bi_sector = blk_idx * (PAGE_SIZE >> 9);
	__bio_add_page(bio, page, PAGE_SIZE, 0);
	bio->bi_end_io = zram_writeback_end_io;
	return req;

out_free_page:
	__free_page(page);
	return ERR_PTR(err);
}

void free_wb_request(struct zram_wb_request *req)
{
	struct bio *bio = req->bio;
	struct page *page = bio_first_page_all(bio);

	if (page)
		__free_page(page);
	/*
	 * bio_put 会自动释放 bio 以及其 front_pad 空间
	 * 不需要单独释放 req
	 */
	bio_put(bio);
}

int setup_zram_writeback(void)
{
	/*
	 * 初始化 bioset:
	 * - pool_size: 64,预分配的 bio 数量,用于减少分配开销
	 * - front_pad: ZRAM_WB_FRONT_PAD,在每个 bio 之前预留空间
	 * - flags: BIOSET_NEED_BVECS,需要 bio vecs 支持
	 * 
	 * 参考 dm-table.c 的做法,使用 front_pad 可以:
	 * 1. 避免为 zram_wb_request 单独分配内存
	 * 2. 提高缓存局部性(request 和 bio 内存连续)
	 * 3. 简化内存管理(一起分配一起释放)
	 */
	if (bioset_init(&zram_wb_bs, 64, ZRAM_WB_FRONT_PAD, BIOSET_NEED_BVECS)) {
		pr_err("Unable to init zram_wb_bs\n");
		return -1;
	}
	return 0;
}

int init_device_zram_writeback(struct zram *zram)
{
	spin_lock_init(&zram->wb_req_list.lock);
	INIT_LIST_HEAD(&zram->wb_req_list.head);
	zram->wb_req_list.count = 0;
	INIT_WORK(&zram->wb_work, zram_wb_work);

	zram->wb_wq = alloc_workqueue("zram_wb/%s",
				      WQ_MEM_RECLAIM | WQ_UNBOUND, 1,
				      zram->disk->disk_name);
	if (!zram->wb_wq) {
		pr_err("Unable to create per-device zram writeback workqueue for %s\n",
		       zram->disk->disk_name);
		return -ENOMEM;
	}
	return 0;
}

void destroy_device_zram_writeback(struct zram *zram)
{
	if (!zram->wb_wq)
		return;

	flush_workqueue(zram->wb_wq);
	destroy_workqueue(zram->wb_wq);
	zram->wb_wq = NULL;
	destroy_wb_request_list(&zram->wb_req_list);
	INIT_LIST_HEAD(&zram->wb_req_list.head);
	zram->wb_req_list.count = 0;
}

void destroy_zram_writeback(void)
{
	bioset_exit(&zram_wb_bs);
}
