/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#ifndef _XE_HW_ENGINE_H_
#define _XE_HW_ENGINE_H_

#include "xe_hw_engine_types.h"

struct drm_printer;

int xe_hw_engine_init(struct xe_gt *gt, struct xe_hw_engine *hwe,
		      enum xe_hw_engine_id id);
void xe_hw_engine_handle_irq(struct xe_hw_engine *hwe, uint16_t intr_vec);
void xe_hw_engine_print_state(struct xe_hw_engine *hwe, struct drm_printer *p);

static inline bool xe_hw_engine_is_valid(struct xe_hw_engine *hwe)
{
	return hwe->name;
}

#endif /* _XE_ENGINE_H_ */
