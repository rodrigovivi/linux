/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_TTM_VRAM_MGR_TYPES_H_
#define _XE_TTM_VRAM_MGR_TYPES_H_

#include <drm/drm_buddy.h>
#include <drm/ttm/ttm_device.h>

struct xe_gt;

struct xe_ttm_vram_mgr {
	struct xe_gt *gt;
	struct ttm_resource_manager manager;
	struct drm_buddy mm;
	u64 default_page_size;
	struct mutex lock;
};

struct xe_ttm_vram_mgr_resource {
	struct ttm_resource base;
	struct list_head blocks;
	unsigned long flags;
};

#endif
