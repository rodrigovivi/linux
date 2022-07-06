// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <drm/ttm/ttm_placement.h>

#include "xe_device.h"
#include "xe_pm.h"
#include "xe_gt.h"
#include "xe_ggtt.h"
#include "xe_irq.h"

/**
 * DOC: Xe Power Management
 *
 * Xe PM shall be guided by the simplicity.
 * Use the simplest hook options whenever possible.
 * Let's not reinvent the runtime_pm references and hooks.
 * Shall have a clear separation of display and gt underneath this component.
 *
 * What's next:
 *
 * For now s2idle and s3 are only working in integrated devices. The next step
 * is to iterate through all VRAM's BO backing them up into the system memory
 * before allowing the system suspend.
 *
 * Also runtime_pm needs to be here from the beginning.
 *
 * RC6/RPS are also critical PM features. Let's start with GuCRC and GuC SLPC
 * and no wait boost. Frequency optimizations should come on a next stage.
 */

/**
   xe_pm_suspend - Helper for System suspend, i.e. S0->S3 / S0->S2idle
   @xe: xe device instance
 */
int xe_pm_suspend(struct xe_device *xe)
{
	int err;

	/* FIXME: We need to add vram eviction and restore before
	 * we can enable the suspend/resume in DGFX.
	 * At this point even s2idle is not working with GuC loading failure
	 * Ideally when implementing we should use evict all as stated below
	 * however we will have to have a list_head to restore the items as
	 * we don't have a ttm helper for restore_all
	 *
	 * struct ttm_resource_manager *man;
	 * man = ttm_manager_type(&xe->ttm, TTM_PL_VRAM);
	 * ttm_resource_manager_evict_all(&xe->ttm, TTM_PL_VRAM);
	 */
	if (IS_DGFX(xe))
		return -ENODEV;

	err = xe_gt_suspend(to_gt(xe));
	if (err)
		return err;

	xe_irq_suspend(xe);

	return 0;
}

/**
   xe_pm_suspend - Helper for System resume S3->S0 / S2idle->S0
   @xe: xe device instance
 */
int xe_pm_resume(struct xe_device *xe)
{
	xe_irq_resume(xe);

	xe_gt_resume(to_gt(xe));

	return 0;
}
