/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2020 Intel Corporation
 */

#ifndef __XE_MIGRATE__
#define __XE_MIGRATE__

#include <drm/drm_mm.h>

struct dma_fence;
struct ttm_resource;

struct xe_bo;
struct xe_gt;
struct xe_engine;
struct xe_migrate;

struct xe_migrate *xe_migrate_init(struct xe_gt *gt);

struct dma_fence *xe_migrate_copy(struct xe_migrate *m,
				  struct xe_bo *bo,
				  struct ttm_resource *src,
				  struct ttm_resource *dst);

struct dma_fence *xe_migrate_clear(struct xe_migrate *m,
				   struct xe_bo *bo,
				   u32 value);

#endif
