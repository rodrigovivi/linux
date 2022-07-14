// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <drm/ttm/ttm_placement.h>

#include "xe_bo.h"
#include "xe_bo_evict.h"
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

	/* FIXME: Super racey... */
	err = xe_bo_evict_all(xe);
	if (err)
		return err;

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
	int err;

	/*
	 * This only restores pinned memory which is the memory required for the
	 * GT(s) to resume.
	 */
	err = xe_bo_restore_all(xe);
	if (err)
		return err;

	xe_irq_resume(xe);
	xe_gt_resume(to_gt(xe));

	return 0;
}
