/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_TTM_VRAM_MGR_TYPES_H_
#define _XE_TTM_VRAM_MGR_TYPES_H_

#include <drm/drm_mm.h>
#include <drm/ttm/ttm_device.h>

struct xe_gt;

struct xe_ttm_vram_mgr {
	struct xe_gt *gt;
	struct ttm_resource_manager manager;
	struct drm_mm mm;
	spinlock_t lock;
	atomic64_t usage;
};

#endif	/* _XE_TTM_VRAM_MGR_TYPES_H_ */
