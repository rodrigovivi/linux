/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#ifndef _XE_VM_H_
#define _XE_VM_H_

#include "xe_macros.h"
#include "xe_vm_types.h"

struct drm_device;
struct drm_file;
struct ttm_bo_kmap_obj;

struct ttm_buffer_object;

struct xe_engine;
struct xe_file;
struct xe_sync_entry;

void __xe_vma_unbind(struct xe_vma *vma);

struct xe_vm *xe_vm_create(struct xe_device *xe, uint32_t flags);
void xe_vm_free(struct kref *ref);

struct xe_vm *xe_vm_lookup(struct xe_file *xef, u32 id);

static inline struct xe_vm *xe_vm_get(struct xe_vm *vm)
{
	kref_get(&vm->refcount);
	return vm;
}

static inline void xe_vm_put(struct xe_vm *vm)
{
	kref_put(&vm->refcount, xe_vm_free);
}

int xe_vm_lock(struct xe_vm *vm, struct ww_acquire_ctx *ww,
	       int num_resv, bool intr);

void xe_vm_unlock(struct xe_vm *vm, struct ww_acquire_ctx *ww);

static inline bool xe_vm_is_closed(struct xe_vm *vm)
{
	/* Only guaranteed not to change when vm->resv is held */
	return !vm->size;
}

#define xe_vm_assert_held(vm) dma_resv_assert_held(&(vm)->resv)

uint64_t xe_vm_pdp4_descriptor(struct xe_vm *vm);

int xe_vm_create_ioctl(struct drm_device *dev, void *data,
		       struct drm_file *file);
int xe_vm_destroy_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file);
int xe_vm_bind_ioctl(struct drm_device *dev, void *data,
		     struct drm_file *file);

void xe_vm_close_and_put(struct xe_vm *vm);

static inline bool xe_vm_in_compute_mode(struct xe_vm *vm)
{
	return vm->flags & XE_VM_FLAG_COMPUTE_MODE;
}
int xe_vm_add_compute_engine(struct xe_vm *vm, struct xe_engine *e);

int xe_vm_userptr_pin(struct xe_vm *vm, bool rebind_worker);
int xe_vm_userptr_needs_repin(struct xe_vm *vm, bool rebind_worker);
struct dma_fence *xe_vm_rebind(struct xe_vm *vm, bool rebind_worker);
static inline bool xe_vm_has_userptr(struct xe_vm *vm)
{
	lockdep_assert_held(&vm->lock);

	return !list_empty(&vm->userptr.list);
}

int xe_vm_async_fence_wait_start(struct dma_fence *fence);

void __xe_pt_write(struct ttm_bo_kmap_obj *map, unsigned int idx, uint64_t data);
u64 gen8_pde_encode(struct xe_bo *bo, u64 bo_offset,
		    const enum xe_cache_level level);
u64 gen8_pte_encode(struct xe_vma *vma, struct xe_bo *bo,
		    u64 offset, enum xe_cache_level cache,
		    u32 flags, u32 pt_level);

extern struct ttm_device_funcs xe_ttm_funcs;

void xe_vm_dump_pgtt(struct xe_vm *vm);

struct ttm_buffer_object *xe_vm_ttm_bo(struct xe_vm *vm);

#endif /* _XE_VM_H_ */
