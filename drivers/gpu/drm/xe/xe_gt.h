/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GT_H_
#define _XE_GT_H_

#include "xe_device_types.h"
#include "xe_hw_engine.h"

int xe_gt_alloc(struct xe_gt *gt);
int xe_gt_init(struct xe_gt *gt);
void xe_gt_fini(struct xe_gt *gt);

struct xe_hw_engine *xe_gt_hw_engine(struct xe_gt *gt,
				     enum xe_engine_class class,
				     uint16_t instance);

static inline struct xe_device * gt_to_xe(struct xe_gt *gt)
{
	return container_of(gt, struct xe_device, gt);
}

#endif	/* _XE_GT_H_ */
