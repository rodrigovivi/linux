/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef _XE_DEVFREQ_H_
#define _XE_DEVFREQ_H_

struct xe_gt;

#ifdef CONFIG_PM_DEVFREQ
void xe_devfreq_init(struct xe_gt *gt);
void xe_devfreq_fini(struct xe_gt *gt);
#else
static inline void xe_devfreq_init(struct xe_gt *gt)
{
}
static inline void xe_devfreq_fini(struct xe_gt *gt)
{
}
#endif

#endif
