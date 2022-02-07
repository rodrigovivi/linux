/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#ifndef _XE_HW_ENGINE_H_
#define _XE_HW_ENGINE_H_

#include "xe_hw_engine_types.h"

int xe_hw_engine_init(struct xe_gt *gt, struct xe_hw_engine *hwe,
		      enum xe_hw_engine_id id);
void xe_hw_engine_finish(struct xe_hw_engine *hwe);

static inline bool xe_hw_engine_is_valid(struct xe_hw_engine *hwe)
{
	return hwe->name;
}

void xe_hw_engine_handle_irq(struct xe_hw_engine *hwe, uint16_t intr_vec);

#endif /* _XE_ENGINE_H_ */
