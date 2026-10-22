/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023-2025 Intel Corporation
 */

#ifndef _XE_EUDEBUG_H_
#define _XE_EUDEBUG_H_

#include <linux/types.h>

struct xe_device;

#if IS_ENABLED(CONFIG_DRM_XE_EUDEBUG)

void xe_eudebug_init_early(struct xe_device *xe);
bool xe_eudebug_is_enabled(struct xe_device *xe);
int xe_eudebug_enable(struct xe_device *xe, bool enable);
void xe_eudebug_init(struct xe_device *xe);

#else

static inline void xe_eudebug_init_early(struct xe_device *xe) { }
static inline bool xe_eudebug_is_enabled(struct xe_device *xe) { return false; }
static inline void xe_eudebug_init(struct xe_device *xe) { }

#endif /* CONFIG_DRM_XE_EUDEBUG */

#endif /* _XE_EUDEBUG_H_ */
