/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef _XE_TTM_SYS_MGR_H_
#define _XE_TTM_SYS_MGR_H_

#include <linux/minmax.h>
#include <linux/stddef.h>
#include <linux/types.h>

#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_pool.h>
#include <drm/ttm/ttm_resource.h>

#include <uapi/drm/xe_drm.h>

#include "xe_bo.h"
#include "xe_device_types.h"

struct xe_device;

/*
 * The SYSMEM_NUMA regions take the gem_create placement bits after the SYSMEM
 * (0) and VRAM (1..XE_MAX_TILES_PER_DEVICE) ones, and the TTM memory types
 * from XE_PL_SYSMEM_NUMA0 up to XE_PL_STOLEN, whichever runs out first.
 */
#define XE_SYSMEM_NUMA_FIRST_INSTANCE	(1 + XE_MAX_TILES_PER_DEVICE)
#define XE_SYSMEM_NUMA_MAX \
	MIN_T(u32, sizeof_field(struct drm_xe_gem_create, placement) * \
	      BITS_PER_BYTE - XE_SYSMEM_NUMA_FIRST_INSTANCE, \
	      XE_PL_STOLEN - XE_PL_SYSMEM_NUMA0)

/**
 * struct xe_sysmem_numa - System memory of a CPU NUMA node
 */
struct xe_sysmem_numa {
	/** @man: TTM manager of the BOs placed here */
	struct ttm_resource_manager man;
	/** @pool: page pool allocating from @node */
	struct ttm_pool pool;
	/** @evict_places: where to evict the BOs placed here from VRAM */
	struct ttm_place evict_places[2];
	/** @size: memory size of @node, in bytes */
	u64 size;
	/** @node: CPU NUMA node */
	int node;
	/** @distance: NUMA distance to the device, 0 if unknown */
	u32 distance;
	/** @mem_type: TTM memory type of the BOs placed here */
	u32 mem_type;
};

int xe_ttm_sys_mgr_init(struct xe_device *xe);

#endif
