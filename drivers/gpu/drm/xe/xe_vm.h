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

int xe_vm_userptr_pin(struct xe_vm *vm);
int xe_vm_userptr_needs_repin(struct xe_vm *vm);
struct dma_fence *xe_vm_userptr_bind(struct xe_vm *vm);
static inline bool xe_vm_has_userptr(struct xe_vm *vm)
{
	lockdep_assert_held(&vm->userptr.list_lock);

	return !list_empty(&vm->userptr.list);
}

static inline int xe_vm_userptr_pending_rebind_read(struct xe_vm *vm)
{
	int val;

	XE_BUG_ON(!xe_vm_has_preempt_fences(vm));

	read_lock(&vm->userptr.notifier_lock);
	val = vm->userptr.pending_rebind;
	read_unlock(&vm->userptr.notifier_lock);

	return val;
}

extern struct ttm_device_funcs xe_ttm_funcs;

void xe_vm_dump_pgtt(struct xe_vm *vm);

#endif /* _XE_VM_H_ */
