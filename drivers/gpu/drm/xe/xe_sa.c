// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_sa.h"

#include <linux/kernel.h>
#include <drm/drm_managed.h>
#include "xe_bo.h"
#include "xe_device.h"
#include "xe_gt.h"

static void xe_sa_bo_manager_fini(struct drm_device *drm, void *arg)
{
	struct xe_sa_manager *sa_manager = arg;

	if (sa_manager->bo == NULL) {
		drm_err(drm, "no bo for sa manager\n");
		return;
	}

	drm_suballoc_manager_fini(&sa_manager->base);

	xe_bo_unpin_map_no_vm(sa_manager->bo);
	sa_manager->bo = NULL;
}

int xe_sa_bo_manager_init(struct xe_gt *gt,
			  struct xe_sa_manager *sa_manager,
			  u32 size, u32 align)
{
	struct xe_device *xe = gt_to_xe(gt);
	struct xe_bo *bo;

	sa_manager->bo = NULL;

	bo = xe_bo_create_pin_map(xe, NULL, size, ttm_bo_type_kernel,
				  XE_BO_CREATE_VRAM_IF_DGFX(xe) |
				  XE_BO_CREATE_GGTT_BIT);
	if (IS_ERR(bo)) {
		drm_err(&xe->drm, "failed to allocate bo for sa manager: %ld\n", PTR_ERR(bo));
		return PTR_ERR(bo);
	}
	sa_manager->bo = bo;

	drm_suballoc_manager_init(&sa_manager->base, size, align);
	sa_manager->gpu_addr = xe_bo_ggtt_addr(bo);

	if (bo->vmap.is_iomem) {
		sa_manager->cpu_ptr = kzalloc(sa_manager->base.size,
					      GFP_KERNEL);
		if (!sa_manager->cpu_ptr) {
			xe_bo_unpin_map_no_vm(sa_manager->bo);
			sa_manager->bo = NULL;
			return -ENOMEM;
		}
	} else {
		sa_manager->cpu_ptr = bo->vmap.vaddr;
		memset(sa_manager->cpu_ptr, 0, sa_manager->base.size);
	}

	return drmm_add_action_or_reset(&xe->drm, xe_sa_bo_manager_fini, sa_manager);

}

struct drm_suballoc *xe_sa_bo_new(struct xe_sa_manager *sa_manager,
				  unsigned size)
{
	return drm_suballoc_new(&sa_manager->base, size);
}

void xe_sa_bo_flush_write(struct drm_suballoc *sa_bo)
{
	struct xe_sa_manager *sa_manager = to_xe_sa_manager(sa_bo->manager);

	if (!sa_manager->bo->vmap.is_iomem)
		return;

	iosys_map_memcpy_to(&sa_manager->bo->vmap, sa_bo->soffset,
			    sa_manager->cpu_ptr + sa_bo->soffset,
			    sa_bo->eoffset - sa_bo->soffset);
}

void xe_sa_bo_free(struct drm_suballoc *sa_bo,
		   struct dma_fence *fence, s32 idx)
{
	drm_suballoc_free(sa_bo, fence, idx);
}
