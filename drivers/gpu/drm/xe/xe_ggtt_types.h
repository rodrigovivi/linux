/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GGTT_TYPES_H_
#define _XE_GGTT_TYPES_H_

#include <drm/drm_mm.h>

struct xe_bo;
struct xe_gt;

struct xe_ggtt {
	struct xe_gt *gt;

	uint64_t size;

	struct xe_bo *scratch;

	struct mutex lock;

	uint64_t __iomem *gsm;

	struct drm_mm mm;

	struct {
		spinlock_t lock;
		struct list_head list;
	} bos;
};

#endif	/* _XE_GGTT_TYPES_H_ */
