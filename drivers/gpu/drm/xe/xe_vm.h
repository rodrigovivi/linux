/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#ifndef _XE_VM_H_
#define _XE_VM_H_

#include <xe_vm_types.h>

struct drm_device;
struct drm_file;

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

static inline void xe_vm_lock(struct xe_vm *vm, struct ww_acquire_ctx *ctx)
{
	dma_resv_lock(&vm->resv, ctx);
}

static inline void xe_vm_unlock(struct xe_vm *vm)
{
	dma_resv_unlock(&vm->resv);
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

struct dma_fence *xe_vm_bind_vma(struct xe_vma *vma, struct xe_sync_entry *syncs, u32 num_syncs);
struct dma_fence *xe_vm_unbind_vma(struct xe_vma *vma, struct xe_sync_entry *syncs, u32 num_syncs, bool evict);

static inline bool xe_vm_has_preempt_fences(struct xe_vm *vm)
{
	return vm->preempt.enabled;
}

extern struct ttm_device_funcs xe_ttm_funcs;

#endif /* _XE_VM_H_ */
