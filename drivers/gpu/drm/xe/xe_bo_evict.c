// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_bo.h"
#include "xe_bo_evict.h"
#include "xe_device.h"
#include "xe_ggtt.h"
#include "xe_gt.h"

int xe_bo_evict_all(struct xe_device *xe)
{
	struct ttm_operation_ctx ctx = {
		.interruptible = false,
		.no_wait_gpu = false,
		.force_alloc = true
	};
	struct ttm_device *bdev = &xe->ttm;
	struct ttm_resource_manager *man;
	struct ww_acquire_ctx ww;
	struct xe_bo *bo, *next;
	int ret;

	if (!IS_DGFX(xe))
		return 0;

	/* User memory */
	man = ttm_manager_type(bdev, TTM_PL_VRAM);
	ret = ttm_resource_manager_evict_all(bdev, man);
	if (ret)
		return ret;

	/*
	 * Wait for all user BO to be evicted as those evictions depend on the
	 * memory moved below.
	 */
	xe_gt_migrate_wait(to_gt(xe));

	/* Kernel memory */
	spin_lock(&xe->pinned.lock);
	list_for_each_entry_safe(bo, next, &xe->pinned.present, pinned_link) {
		spin_unlock(&xe->pinned.lock);

		xe_bo_lock(bo, &ww, 0, false);
		ret = ttm_bo_evict(&bo->ttm, &ctx);
		xe_bo_unlock(bo, &ww);
		if (ret)
			return ret;

		spin_lock(&xe->pinned.lock);
		list_move_tail(&bo->pinned_link, &xe->pinned.evicted);
	}
	spin_unlock(&xe->pinned.lock);

	return 0;
}

int xe_bo_restore_all(struct xe_device *xe)
{
	struct ww_acquire_ctx ww;
	struct xe_bo *bo, *next;
	int ret;

	if (!IS_DGFX(xe))
		return 0;

	spin_lock(&xe->pinned.lock);
	list_for_each_entry_safe(bo, next, &xe->pinned.evicted, pinned_link) {
		spin_unlock(&xe->pinned.lock);

		xe_bo_lock(bo, &ww, 0, false);
		ret = xe_bo_validate(bo, NULL);
		xe_bo_unlock(bo, &ww);
		if (ret)
			return ret;

		if (bo->flags & XE_BO_CREATE_GGTT_BIT)
			xe_ggtt_map_bo(to_gt(xe)->mem.ggtt, bo);

		/*
		 * We expect validate to trigger a move VRAM and our move code
		 * should setup the iosys map.
		 */
		XE_BUG_ON(iosys_map_is_null(&bo->vmap));
		XE_BUG_ON( bo->ttm.resource->mem_type != TTM_PL_VRAM);

		spin_lock(&xe->pinned.lock);
		list_move_tail(&bo->pinned_link, &xe->pinned.present);
	}
	spin_unlock(&xe->pinned.lock);

	return 0;
}
