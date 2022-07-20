// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021-2022 Intel Corporation
 * Copyright (C) 2021-2002 Red Hat
 */

#include <drm/drm_managed.h>

#include <drm/ttm/ttm_range_manager.h>
#include <drm/ttm/ttm_placement.h>

#include "xe_bo.h"
#include "xe_device.h"
#include "xe_gt.h"
#include "xe_res_cursor.h"
#include "xe_ttm_vram_mgr.h"

static inline struct xe_ttm_vram_mgr *
to_vram_mgr(struct ttm_resource_manager *man)
{
	return container_of(man, struct xe_ttm_vram_mgr, manager);
}

static inline struct xe_gt *
mgr_to_gt(struct xe_ttm_vram_mgr *mgr)
{
	return mgr->gt;
}

static void xe_ttm_vram_mgr_virt_start(struct ttm_resource *mem,
				       struct drm_mm_node *node)
{
	unsigned long start;

	start = node->start + node->size;
	if (start > mem->num_pages)
		start -= mem->num_pages;
	else
		start = 0;
	mem->start = max(mem->start, start);
}

static int xe_ttm_vram_mgr_new(struct ttm_resource_manager *man,
			       struct ttm_buffer_object *tbo,
			       const struct ttm_place *place,
			       struct ttm_resource **res)
{
	unsigned long lpfn, num_nodes, pages_per_node, pages_left, pages;
	struct xe_ttm_vram_mgr *mgr = to_vram_mgr(man);
	u64 mem_bytes;
	struct ttm_range_mgr_node *node;
	struct drm_mm *mm = &mgr->mm;
	enum drm_mm_insert_mode mode;
	unsigned i;
	int r;

	lpfn = place->lpfn;
	if (!lpfn)
		lpfn = man->size;

	mem_bytes = tbo->base.size;

	if (place->flags & TTM_PL_FLAG_CONTIGUOUS) {
		pages_per_node = ~0ul;
		num_nodes = 1;
	} else {
		/* default to 2MB */
		pages_per_node = 2UL << (20UL - PAGE_SHIFT);
		pages_per_node = max_t(u32, pages_per_node,
				       tbo->page_alignment);
		num_nodes = DIV_ROUND_UP_ULL(PFN_UP(mem_bytes), pages_per_node);
	}

	/* bail out quickly if there's likely not enough VRAM for this BO */
	if (man->size << PAGE_SHIFT < ttm_resource_manager_usage(man) + mem_bytes)
		return -ENOSPC;

	node = kvmalloc(struct_size(node, mm_nodes, num_nodes),
			GFP_KERNEL | __GFP_ZERO);
	if (!node)
		return -ENOMEM;

	ttm_resource_init(tbo, place, &node->base);

	mode = DRM_MM_INSERT_BEST;
	if (place->flags & TTM_PL_FLAG_TOPDOWN)
		mode = DRM_MM_INSERT_HIGH;

	pages_left = node->base.num_pages;

	/* Limit maximum size to 2GB due to SG table limitations */
	pages = min(pages_left, 2UL << (30 - PAGE_SHIFT));

	i = 0;
	spin_lock(&mgr->lock);
	while (pages_left) {
		u32 alignment = tbo->page_alignment;

		if (pages >= pages_per_node)
			alignment = pages_per_node;

		r = drm_mm_insert_node_in_range(mm, &node->mm_nodes[i], pages,
						alignment, 0, place->fpfn,
						lpfn, mode);
		if (unlikely(r)) {
			if (pages > pages_per_node) {
				if (is_power_of_2(pages))
					pages = pages / 2;
				else
					pages = rounddown_pow_of_two(pages);
				continue;
			}
			goto error_free;
		}

		xe_ttm_vram_mgr_virt_start(&node->base, &node->mm_nodes[i]);
		pages_left -= pages;
		++i;

		if (pages > pages_left)
			pages = pages_left;
	}
	spin_unlock(&mgr->lock);

	if (i == 1)
		node->base.placement |= TTM_PL_FLAG_CONTIGUOUS;

	*res = &node->base;
	return 0;

error_free:
	while (i--)
		drm_mm_remove_node(&node->mm_nodes[i]);
	spin_unlock(&mgr->lock);
	ttm_resource_fini(man, &node->base);
	kvfree(node);
	return r;
}

static void xe_ttm_vram_mgr_del(struct ttm_resource_manager *man,
				struct ttm_resource *res)
{
	struct ttm_range_mgr_node *node = to_ttm_range_mgr_node(res);
	struct xe_ttm_vram_mgr *mgr = to_vram_mgr(man);
	u64 usage = 0;
	int i, pages;

	spin_lock(&mgr->lock);
	for (i = 0, pages = res->num_pages; pages;
	     pages -= node->mm_nodes[i].size, ++i) {
		struct drm_mm_node *mm = &node->mm_nodes[i];

		drm_mm_remove_node(mm);
		usage += mm->size << PAGE_SHIFT;
	}

	spin_unlock(&mgr->lock);

	ttm_resource_fini(man, res);

	kvfree(node);
}

static void xe_ttm_vram_mgr_debug(struct ttm_resource_manager *man,
				  struct drm_printer *printer)
{
	struct xe_ttm_vram_mgr *mgr = to_vram_mgr(man);
	spin_lock(&mgr->lock);
	drm_mm_print(&mgr->mm, printer);
	spin_unlock(&mgr->lock);

	drm_printf(printer, "man size:%llu pages\n",
		   man->size);
}

static const struct ttm_resource_manager_func xe_ttm_vram_mgr_func = {
	.alloc	= xe_ttm_vram_mgr_new,
	.free	= xe_ttm_vram_mgr_del,
	.debug	= xe_ttm_vram_mgr_debug
};

static void ttm_vram_mgr_fini(struct drm_device *drm, void *arg)
{
	struct xe_ttm_vram_mgr *mgr = arg;
	struct xe_device *xe = gt_to_xe(mgr->gt);
	struct ttm_resource_manager *man = &mgr->manager;
	int err;

	ttm_resource_manager_set_used(man, false);

	err = ttm_resource_manager_evict_all(&xe->ttm, man);
	if (err)
		return;

	spin_lock(&mgr->lock);
	drm_mm_takedown(&mgr->mm);
	spin_unlock(&mgr->lock);

	ttm_resource_manager_cleanup(man);
	ttm_set_driver_manager(&xe->ttm, TTM_PL_VRAM, NULL);
}

int xe_ttm_vram_mgr_init(struct xe_gt *gt, struct xe_ttm_vram_mgr *mgr)
{
	struct xe_device *xe = gt_to_xe(gt);
	struct ttm_resource_manager *man = &mgr->manager;
	int err;

	mgr->gt = gt;
	man->func = &xe_ttm_vram_mgr_func;

	ttm_resource_manager_init(man, &xe->ttm, gt->mem.vram.size >> PAGE_SHIFT);

	drm_mm_init(&mgr->mm, 0, man->size);
	spin_lock_init(&mgr->lock);
	ttm_set_driver_manager(&xe->ttm, TTM_PL_VRAM, &mgr->manager);
	ttm_resource_manager_set_used(man, true);

	err = drmm_add_action_or_reset(&xe->drm, ttm_vram_mgr_fini, mgr);
	if (err)
		return err;

	return 0;
}

int xe_ttm_vram_mgr_alloc_sgt(struct xe_device *xe,
			      struct ttm_resource *res,
			      u64 offset, u64 length,
			      struct device *dev,
			      enum dma_data_direction dir,
			      struct sg_table **sgt)
{
	struct xe_res_cursor cursor;
	struct scatterlist *sg;
	int num_entries = 0;
	int i, r;

	*sgt = kmalloc(sizeof(**sgt), GFP_KERNEL);
	if (!*sgt)
		return -ENOMEM;

	/* Determine the number of DRM_BUDDY blocks to export */
	xe_res_first(res, offset, length, &cursor);
	while (cursor.remaining) {
		num_entries++;
		xe_res_next(&cursor, cursor.size);
	}

	r = sg_alloc_table(*sgt, num_entries, GFP_KERNEL);
	if (r)
		goto error_free;

	/* Initialize scatterlist nodes of sg_table */
	for_each_sgtable_sg((*sgt), sg, i)
		sg->length = 0;

	/*
	 * Walk down DRM_BUDDY blocks to populate scatterlist nodes
	 * @note: Use iterator api to get first the DRM_BUDDY block
	 * and the number of bytes from it. Access the following
	 * DRM_BUDDY block(s) if more buffer needs to exported
	 */
	xe_res_first(res, offset, length, &cursor);
	for_each_sgtable_sg((*sgt), sg, i) {
		phys_addr_t phys = cursor.start + to_gt(xe)->mem.vram.io_start;
		size_t size = cursor.size;
		dma_addr_t addr;

		addr = dma_map_resource(dev, phys, size, dir,
					DMA_ATTR_SKIP_CPU_SYNC);
		r = dma_mapping_error(dev, addr);
		if (r)
			goto error_unmap;

		sg_set_page(sg, NULL, size, 0);
		sg_dma_address(sg) = addr;
		sg_dma_len(sg) = size;

		xe_res_next(&cursor, cursor.size);
	}

	return 0;

error_unmap:
	for_each_sgtable_sg((*sgt), sg, i) {
		if (!sg->length)
			continue;

		dma_unmap_resource(dev, sg->dma_address,
				   sg->length, dir,
				   DMA_ATTR_SKIP_CPU_SYNC);
	}
	sg_free_table(*sgt);

error_free:
	kfree(*sgt);
	return r;
}

void xe_ttm_vram_mgr_free_sgt(struct device *dev, enum dma_data_direction dir,
			      struct sg_table *sgt)
{
	struct scatterlist *sg;
	int i;

	for_each_sgtable_sg(sgt, sg, i)
		dma_unmap_resource(dev, sg->dma_address,
				   sg->length, dir,
				   DMA_ATTR_SKIP_CPU_SYNC);
	sg_free_table(sgt);
	kfree(sgt);
}
