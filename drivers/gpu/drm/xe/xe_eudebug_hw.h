/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023-2025 Intel Corporation
 */

#ifndef _XE_EUDEBUG_HW_H_
#define _XE_EUDEBUG_HW_H_

#include <linux/types.h>

struct xe_eudebug;
struct xe_hw_engine;
struct xe_gt;

#if IS_ENABLED(CONFIG_DRM_XE_EUDEBUG)

void xe_eudebug_hw_init(struct xe_eudebug *d);
void xe_eudebug_init_hw_engine(struct xe_hw_engine *hwe, bool enable);

long xe_eudebug_eu_control(struct xe_eudebug *d, const u64 arg);

struct xe_exec_queue *xe_gt_runalone_active_queue_get(struct xe_gt *gt,
						      struct xe_hw_engine **active_hwe,
						      int *lrc_idx);

#endif /* CONFIG_DRM_XE_EUDEBUG */

#endif /* _XE_EUDEBUG_HW_H_ */
