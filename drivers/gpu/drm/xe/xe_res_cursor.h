/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * Copyright 2020 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Christian König
 */

#ifndef __XE_RES_CURSOR_H__
#define __XE_RES_CURSOR_H__

#include <linux/scatterlist.h>

#include <drm/drm_mm.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_range_manager.h>
#include <drm/ttm/ttm_resource.h>
#include <drm/ttm/ttm_tt.h>

#include "xe_bo.h"
#include "xe_macros.h"
#include "xe_ttm_vram_mgr.h"

/* state back for walking over vram_mgr and gtt_mgr allocations */
struct xe_res_cursor {
	u64 start;
	u64 size;
	u64 remaining;
	void *node;
	u32 mem_type;
	const dma_addr_t *dma_address;
	struct scatterlist *sgl;
};

/**
 * xe_res_first - initialize a xe_res_cursor
 *
 * @res: TTM resource object to walk
 * @start: Start of the range
 * @size: Size of the range
 * @cur: cursor object to initialize
 *
 * Start walking over the range of allocations between @start and @size.
 */
static inline void xe_res_first(struct ttm_resource *res,
				u64 start, u64 size,
				struct xe_res_cursor *cur)
{
	struct drm_buddy_block *block;
	struct list_head *head, *next;

	cur->dma_address = NULL;
	cur->sgl = NULL;
	if (!res)
		goto fallback;

	XE_BUG_ON(start + size > res->num_pages << PAGE_SHIFT);

	cur->mem_type = res->mem_type;

	switch (cur->mem_type) {
	case XE_PL_VRAM0:
	case XE_PL_VRAM1:
		head = &to_xe_ttm_vram_mgr_resource(res)->blocks;

		block = list_first_entry_or_null(head,
						 struct drm_buddy_block,
						 link);
		if (!block)
			goto fallback;

		while (start >= xe_ttm_vram_mgr_block_size(block)) {
			start -= xe_ttm_vram_mgr_block_size(block);

			next = block->link.next;
			if (next != head)
				block = list_entry(next, struct drm_buddy_block,
						   link);
		}

		cur->start = xe_ttm_vram_mgr_block_start(block) + start;
		cur->size = min(xe_ttm_vram_mgr_block_size(block) - start,
				size);
		cur->remaining = size;
		cur->node = block;
		break;
	default:
		goto fallback;
	}

	return;

fallback:
	cur->start = start;
	cur->size = size;
	cur->remaining = size;
	cur->node = NULL;
	XE_WARN_ON(res && start + size > res->num_pages << PAGE_SHIFT);
	return;
}

static inline void __xe_res_dma_next(struct xe_res_cursor *cur)
{
	const u64 *dma = cur->dma_address + (cur->start >> PAGE_SHIFT);
	pgoff_t last_idx = cur->remaining >> PAGE_SHIFT;
	pgoff_t idx = 0;

	while (idx < last_idx && dma[idx] == dma[0] + (idx << PAGE_SHIFT))
		idx++;

	cur->size = idx << PAGE_SHIFT;
}

static inline void __xe_res_sg_next(struct xe_res_cursor *cur)
{
	struct scatterlist *sgl = cur->sgl;
	u64 start = cur->start;

	while (start >= sg_dma_len(sgl)) {
		start -= sg_dma_len(sgl);
		sgl = sg_next(sgl);
		XE_BUG_ON(!sgl);
	}

	cur->start = start;
	cur->size = sg_dma_len(sgl) - start;
	cur->sgl = sgl;
}

/**
 * xe_res_first_dma - initialize a xe_res_cursor with array of dma addresses
 *
 * @dma_address: array of dma addresses to walk
 * @start: Start of the range
 * @size: Size of the range
 * @cur: cursor object to initialize
 *
 * Start walking over the range of allocations between @start and @size.
 */
static inline void xe_res_first_dma(const dma_addr_t *dma_address,
				    u64 start, u64 size,
				    struct xe_res_cursor *cur)
{
	XE_BUG_ON(!IS_ALIGNED(start, PAGE_SIZE) ||
		  !IS_ALIGNED(size, PAGE_SIZE));
	cur->node = NULL;
	cur->start = start;
	cur->remaining = size;
	cur->size = 0;
	cur->dma_address = dma_address;
	cur->sgl = NULL;
	__xe_res_dma_next(cur);
}

/**
 * xe_res_first_sg - initialize a xe_res_cursor with a scatter gather table
 *
 * @sg: scatter gather table to walk
 * @start: Start of the range
 * @size: Size of the range
 * @cur: cursor object to initialize
 *
 * Start walking over the range of allocations between @start and @size.
 */
static inline void xe_res_first_sg(const struct sg_table *sg,
				   u64 start, u64 size,
				   struct xe_res_cursor *cur)
{
	XE_BUG_ON(!IS_ALIGNED(start, PAGE_SIZE) ||
		  !IS_ALIGNED(size, PAGE_SIZE));
	cur->node = NULL;
	cur->start = start;
	cur->remaining = size;
	cur->size = 0;
	cur->sgl = sg->sgl;
	cur->dma_address = NULL;
	__xe_res_sg_next(cur);
}

/**
 * xe_res_next - advance the cursor
 *
 * @cur: the cursor to advance
 * @size: number of bytes to move forward
 *
 * Move the cursor @size bytes forwrad, walking to the next node if necessary.
 */
static inline void xe_res_next(struct xe_res_cursor *cur, u64 size)
{
	struct drm_buddy_block *block;
	struct list_head *next;

	XE_BUG_ON(size > cur->remaining);

	cur->remaining -= size;
	if (!cur->remaining)
		return;

	cur->size -= size;
	if (cur->size) {
		cur->start += size;
		return;
	} else if (cur->dma_address) {
		cur->start += size;
		__xe_res_dma_next(cur);
		return;
	} else if (cur->sgl) {
		cur->start += size;
		__xe_res_sg_next(cur);
		return;
	}

	switch (cur->mem_type) {
	case XE_PL_VRAM0:
	case XE_PL_VRAM1:
		block = cur->node;

		next = block->link.next;
		block = list_entry(next, struct drm_buddy_block, link);

		cur->node = block;
		cur->start = xe_ttm_vram_mgr_block_start(block);
		cur->size = min(xe_ttm_vram_mgr_block_size(block),
				cur->remaining);
		break;
	default:
		return;
	}
}

/**
 * xe_res_dma - return dma address of cursor at current position
 *
 * @cur: the cursor to return the dma address from
 */
static inline u64 xe_res_dma(const struct xe_res_cursor *cur)
{
	return cur->dma_address ? cur->dma_address[cur->start >> PAGE_SHIFT] :
		cur->sgl ? sg_dma_address(cur->sgl) + cur->start : cur->start;
}
#endif
