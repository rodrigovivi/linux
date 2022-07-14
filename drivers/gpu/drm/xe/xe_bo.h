/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef _XE_BO_H_
#define _XE_BO_H_

#include "xe_bo_types.h"
#include "xe_macros.h"
#include "xe_vm_types.h"

#define XE_DEFAULT_GTT_SIZE_MB          3072ULL /* 3GB by default */

#define XE_BO_CREATE_USER_BIT		BIT(1)
#define XE_BO_CREATE_SYSTEM_BIT		BIT(2)
#define XE_BO_CREATE_VRAM_BIT		BIT(3)
#define XE_BO_CREATE_VRAM_IF_DGFX(xe) \
	(IS_DGFX(xe) ? XE_BO_CREATE_VRAM_BIT : XE_BO_CREATE_SYSTEM_BIT)
#define XE_BO_CREATE_GGTT_BIT		BIT(4)
#define XE_BO_CREATE_IGNORE_MIN_PAGE_SIZE_BIT BIT(5)
#define XE_BO_CREATE_PINNED_BIT		BIT(6)
/* this one is trigger internally only */
#define XE_BO_INTERNAL_64K		BIT(31)

#if  !defined(CONFIG_X86)
	#define _PAGE_BIT_PRESENT       0       /* is present */
	#define _PAGE_BIT_RW            1       /* writeable */
	#define _PAGE_BIT_PWT           3       /* page write through */
	#define _PAGE_BIT_PCD           4       /* page cache disabled */
	#define _PAGE_BIT_PAT           7       /* on 4KB pages */

	#define _PAGE_PRESENT   (_AT(pteval_t, 1) << _PAGE_BIT_PRESENT)
	#define _PAGE_RW        (_AT(pteval_t, 1) << _PAGE_BIT_RW)
	#define _PAGE_PWT       (_AT(pteval_t, 1) << _PAGE_BIT_PWT)
	#define _PAGE_PCD       (_AT(pteval_t, 1) << _PAGE_BIT_PCD)
	#define _PAGE_PAT       (_AT(pteval_t, 1) << _PAGE_BIT_PAT)
#endif

#define PPAT_UNCACHED                   (_PAGE_PWT | _PAGE_PCD)
#define PPAT_CACHED_PDE                 0 /* WB LLC */
#define PPAT_CACHED                     _PAGE_PAT /* WB LLCeLLC */
#define PPAT_DISPLAY_ELLC               _PAGE_PCD /* WT eLLC */

#define GEN8_PTE_SHIFT			12
#define GEN8_PAGE_SIZE			(1 << GEN8_PTE_SHIFT)
#define GEN8_PTE_MASK			(GEN8_PAGE_SIZE - 1)
#define GEN8_PDE_SHIFT			(GEN8_PTE_SHIFT - 3)
#define GEN8_PDES			(1 << GEN8_PDE_SHIFT)
#define GEN8_PDE_MASK			(GEN8_PDES - 1)

#define GEN8_64K_PTE_SHIFT		16
#define GEN8_64K_PAGE_SIZE		(1 << GEN8_64K_PTE_SHIFT)
#define GEN8_64K_PTE_MASK		(GEN8_64K_PAGE_SIZE - 1)
#define GEN8_64K_PDE_MASK		(GEN8_PDE_MASK >> 4)

#define GEN8_PDE_PS_2M			BIT_ULL(7)
#define GEN8_PDPE_PS_1G			BIT_ULL(7)
#define GEN8_PDE_IPS_64K		BIT_ULL(11)

#define GEN12_GGTT_PTE_LM		BIT_ULL(1)
#define GEN12_PPGTT_PTE_LM		BIT_ULL(11)
#define GEN12_PDE_64K			BIT_ULL(6)

#define GEN8_PAGE_PRESENT		BIT_ULL(0)
#define GEN8_PAGE_RW			BIT_ULL(1)

#define PTE_READ_ONLY			BIT(0)

struct xe_bo *__xe_bo_create_locked(struct xe_device *xe,
				    struct dma_resv *resv, size_t size,
				    enum ttm_bo_type type, uint32_t flags);
struct xe_bo *xe_bo_create_locked(struct xe_device *xe,
				  struct xe_vm *vm, size_t size,
				  enum ttm_bo_type type, uint32_t flags);
struct xe_bo *xe_bo_create(struct xe_device *xe, struct xe_vm *vm, size_t size,
			   enum ttm_bo_type type, uint32_t flags);
struct xe_bo *xe_bo_create_pin_map(struct xe_device *xe, struct xe_vm *vm,
				   size_t size, enum ttm_bo_type type,
				   uint32_t flags);
struct xe_bo *xe_bo_create_from_data(struct xe_device *xe, const void *data,
				     size_t size, enum ttm_bo_type type,
				     uint32_t flags);

void xe_bo_trigger_rebind(struct xe_bo *bo);

static inline struct xe_bo *ttm_to_xe_bo(const struct ttm_buffer_object *bo)
{
	return container_of(bo, struct xe_bo, ttm);
}

static inline struct xe_bo *gem_to_xe_bo(const struct drm_gem_object *obj)
{
	return container_of(obj, struct xe_bo, ttm.base);
}

#define xe_bo_device(bo) ttm_to_xe_device((bo)->ttm.bdev)

static inline struct xe_bo *xe_bo_get(struct xe_bo *bo)
{
	if (bo)
		ttm_bo_get(&bo->ttm);
	return bo;
}

static inline void xe_bo_put(struct xe_bo *bo)
{
	if (bo)
		ttm_bo_put(&bo->ttm);
}

static inline void xe_bo_assert_held(struct xe_bo *bo)
{
	if (bo)
		dma_resv_assert_held((bo)->ttm.base.resv);
}

int xe_bo_lock(struct xe_bo *bo, struct ww_acquire_ctx *ww,
	       int num_resv, bool intr);

void xe_bo_unlock(struct xe_bo *bo, struct ww_acquire_ctx *ww);

static inline void xe_bo_unlock_vm_held(struct xe_bo *bo)
{
	if (bo) {
		XE_BUG_ON(bo->vm && bo->ttm.base.resv != &bo->vm->resv);
		if (bo->vm)
			xe_vm_assert_held(bo->vm);
		else
			dma_resv_unlock(bo->ttm.base.resv);
	}
}

static inline void xe_bo_lock_no_vm(struct xe_bo *bo, struct ww_acquire_ctx *ctx)
{
	if (bo) {
		XE_BUG_ON(bo->vm || bo->ttm.base.resv != &bo->ttm.base._resv);
		dma_resv_lock(bo->ttm.base.resv, ctx);
	}
}

static inline void xe_bo_unlock_no_vm(struct xe_bo *bo)
{
	if (bo) {
		XE_BUG_ON(bo->vm || bo->ttm.base.resv != &bo->ttm.base._resv);
		dma_resv_unlock(bo->ttm.base.resv);
	}
}

int xe_bo_populate(struct xe_bo *bo);
int xe_bo_pin(struct xe_bo *bo);
void xe_bo_unpin(struct xe_bo *bo);
int xe_bo_validate(struct xe_bo *bo, struct xe_vm *vm);

static inline bool xe_bo_is_pinned(struct xe_bo *bo)
{
	return bo->ttm.pin_count;
}

static inline void xe_bo_unpin_map_no_vm(struct xe_bo *bo)
{
	if (likely(bo)) {
		xe_bo_lock_no_vm(bo, NULL);
		xe_bo_unpin(bo);
		xe_bo_unlock_no_vm(bo);

		xe_bo_put(bo);
	}
}

bool xe_bo_is_xe_bo(struct ttm_buffer_object *bo);
dma_addr_t xe_bo_addr(struct xe_bo *bo, uint64_t offset,
		      size_t page_size, bool *is_lmem);

static inline dma_addr_t
xe_bo_main_addr(struct xe_bo *bo, size_t page_size)
{
	bool is_lmem;

	return xe_bo_addr(bo, 0, page_size, &is_lmem);
}

static inline uint32_t
xe_bo_ggtt_addr(struct xe_bo *bo)
{
	XE_BUG_ON(bo->ggtt_node.size != bo->size);
	XE_BUG_ON(bo->ggtt_node.start + bo->ggtt_node.size > (1ull << 32));
	return bo->ggtt_node.start;
}

int xe_bo_vmap(struct xe_bo *bo);
void xe_bo_vunmap(struct xe_bo *bo);

extern struct ttm_device_funcs xe_ttm_funcs;

int xe_gem_create_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file);
int xe_gem_mmap_offset_ioctl(struct drm_device *dev, void *data,
			     struct drm_file *file);

/*
 * FIXME: The below helpers should be in common code. Lucas has a series
 * reworking the iosys-map headers. Let's see how that pans out and follow up
 * on his series if needed.
 */
static inline uint32_t dbm_read32(struct iosys_map map)
{
	if (map.is_iomem)
		return readl(map.vaddr_iomem);
	else
		return READ_ONCE(*(uint32_t *)map.vaddr);
}

static inline void dbm_write32(struct iosys_map map, uint32_t val)
{
	if (map.is_iomem)
		writel(val, map.vaddr_iomem);
	else
		*(uint32_t *)map.vaddr = val;
}

#endif /* _XE_BO_H_ */
