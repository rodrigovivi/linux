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
struct xe_sync_entry;
struct xe_pt;
struct xe_vm;
struct xe_vm_pgtable_update;

struct xe_migrate *xe_migrate_init(struct xe_gt *gt);

struct dma_fence *xe_migrate_copy(struct xe_migrate *m,
				  struct xe_bo *bo,
				  struct ttm_resource *src,
				  struct ttm_resource *dst);

struct dma_fence *xe_migrate_clear(struct xe_migrate *m,
				   struct xe_bo *bo,
				   u32 value);

typedef void (*xe_migrate_populatefn_t)(void *pos, u32 ofs, u32 num_qwords,
					struct xe_vm_pgtable_update *update,
					void *arg);

struct xe_vm *xe_migrate_get_vm(struct xe_migrate *m);

struct dma_fence *
xe_migrate_update_pgtables(struct xe_migrate *m,
			   struct xe_vm *vm,
			   struct xe_bo *bo,
			   struct xe_engine *eng,
			   struct xe_vm_pgtable_update *updates,
			   u32 num_updates,
			   struct xe_sync_entry *syncs, u32 num_syncs,
			   xe_migrate_populatefn_t populatefn, void *arg);

#endif
