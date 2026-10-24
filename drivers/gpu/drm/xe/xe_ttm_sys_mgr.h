/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef _XE_TTM_SYS_MGR_H_
#define _XE_TTM_SYS_MGR_H_

#include <linux/atomic.h>
#include <linux/types.h>

#include <drm/ttm/ttm_pool.h>

#include "xe_device_types.h"

struct xe_device;

/*
 * The SYSMEM_NUMA regions take the remaining bits of the 32-bit gem_create
 * placement, after the SYSMEM (0) and VRAM (1..XE_MAX_TILES_PER_DEVICE) ones.
 */
#define XE_SYSMEM_NUMA_FIRST_INSTANCE	(1 + XE_MAX_TILES_PER_DEVICE)
#define XE_SYSMEM_NUMA_MAX		(32 - XE_SYSMEM_NUMA_FIRST_INSTANCE)

/**
 * struct xe_sysmem_numa - System memory of a CPU NUMA node
 */
struct xe_sysmem_numa {
	/** @pool: page pool allocating from @node */
	struct ttm_pool pool;
	/** @size: memory size of @node, in bytes */
	u64 size;
	/** @used: size of the TT resources of the BOs placed here, in bytes */
	atomic64_t used;
	/** @node: CPU NUMA node */
	int node;
	/** @distance: NUMA distance to the device, 0 if unknown */
	u32 distance;
};

int xe_ttm_sys_mgr_init(struct xe_device *xe);

#endif
