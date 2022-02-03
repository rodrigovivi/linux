/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GGTT_TYPES_H_
#define _XE_GGTT_TYPES_H_

#include <drm/drm_mm.h>

struct xe_bo;
struct xe_device;

struct xe_ggtt {
	struct xe_device *xe;

	uint64_t size;

	struct xe_bo *scratch;

	struct mutex lock;

	uint64_t __iomem *gsm;

	struct drm_mm mm;
};

#endif	/* _XE_GGTT_TYPES_H_ */
