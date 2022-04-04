/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#include "xe_vm.h"

#include <drm/ttm/ttm_execbuf_util.h>
#include <drm/ttm/ttm_tt.h>
#include <drm/xe_drm.h>
#include <linux/mm.h>

#include "xe_bo.h"
#include "xe_device.h"
#include "xe_engine_types.h"
#include "xe_gt.h"
#include "xe_migrate.h"
#include "xe_preempt_fence_types.h"
#include "xe_sync.h"

enum xe_cache_level {
	XE_CACHE_NONE,
	XE_CACHE_WT,
	XE_CACHE_WB,
};

#define PTE_READ_ONLY	BIT(0)
#define PTE_LM		BIT(1)

#define PPAT_UNCACHED			(_PAGE_PWT | _PAGE_PCD)
#define PPAT_CACHED_PDE			0 /* WB LLC */
#define PPAT_CACHED			_PAGE_PAT /* WB LLCeLLC */
#define PPAT_DISPLAY_ELLC		_PAGE_PCD /* WT eLLC */

#define GEN8_PTE_SHIFT 12
#define GEN8_PAGE_SIZE (1 << GEN8_PTE_SHIFT)
#define GEN8_PTE_MASK (GEN8_PAGE_SIZE - 1)
#define GEN8_PDE_SHIFT (GEN8_PTE_SHIFT - 3)
#define GEN8_PDES (1 << GEN8_PDE_SHIFT)
#define GEN8_PDE_MASK (GEN8_PDES - 1)

#define GEN12_PPGTT_PTE_LM	BIT_ULL(11)

#define XE_VM_DEBUG 0

#if XE_VM_DEBUG
#define vm_dbg drm_dbg
#else
__printf(2, 3)
static inline void vm_dbg(const struct drm_device *dev,
			  const char *format, ...)
{ /* noop */ }

#endif

static uint64_t gen8_pde_encode(struct xe_bo *bo, uint64_t bo_offset,
				const enum xe_cache_level level)
{
	uint64_t pde;
	bool is_lmem;

	pde = xe_bo_addr(bo, bo_offset, GEN8_PAGE_SIZE, &is_lmem);
	pde |= _PAGE_PRESENT | _PAGE_RW;

	XE_WARN_ON(IS_DGFX(xe_bo_device(bo)) && !is_lmem);

	if (level != XE_CACHE_NONE)
		pde |= PPAT_CACHED_PDE;
	else
		pde |= PPAT_UNCACHED;

	return pde;
}

static uint64_t gen8_pte_encode(struct xe_bo *bo, uint64_t bo_offset,
				enum xe_cache_level level,
				uint32_t flags)
{
	uint64_t pte;
	bool is_lmem;

	pte = xe_bo_addr(bo, bo_offset, GEN8_PAGE_SIZE, &is_lmem);
	pte |= _PAGE_PRESENT | _PAGE_RW;

	if (unlikely(flags & PTE_READ_ONLY))
		pte &= ~_PAGE_RW;

	if (is_lmem)
		pte |= GEN12_PPGTT_PTE_LM;

	switch (level) {
	case XE_CACHE_NONE:
		pte |= PPAT_UNCACHED;
		break;
	case XE_CACHE_WT:
		pte |= PPAT_DISPLAY_ELLC;
		break;
	default:
		pte |= PPAT_CACHED;
		break;
	}

	return pte;
}

struct xe_pt {
	struct xe_bo *bo;
	unsigned int level;
	unsigned int num_live;
};

struct xe_pt_dir {
	struct xe_pt pt;
	struct xe_pt *entries[GEN8_PDES];
};

struct xe_pt_0 {
	struct xe_pt pt;
};

static struct xe_pt_dir *as_xe_pt_dir(struct xe_pt *pt)
{
	return container_of(pt, struct xe_pt_dir, pt);
}

static uint64_t __xe_vm_empty_pte(struct xe_vm *vm, unsigned int level)
{
	if (!vm->scratch_bo)
		return 0;

	if (level == 0)
		return gen8_pte_encode(vm->scratch_bo, 0, XE_CACHE_WB, 0);
	else
		return gen8_pde_encode(vm->scratch_pt[level - 1]->bo, 0,
				       XE_CACHE_WB);
}

static int __xe_pt_kmap(struct xe_pt *pt, struct ttm_bo_kmap_obj *map)
{
	XE_BUG_ON(pt->bo->size % PAGE_SIZE);
	return ttm_bo_kmap(&pt->bo->ttm, 0, pt->bo->size / PAGE_SIZE, map);
}

static void __xe_pt_write(struct ttm_bo_kmap_obj *map,
			  unsigned int idx, uint64_t data)
{
	bool is_iomem;
	uint64_t *map_u64;

	map_u64 = ttm_kmap_obj_virtual(map, &is_iomem);
	if (is_iomem)
		writeq(data, (uint64_t __iomem *)&map_u64[idx]);
	else
		map_u64[idx] = data;
}

static struct xe_pt *xe_pt_create(struct xe_vm *vm, unsigned int level)
{
	struct xe_pt *pt;
	struct xe_bo *bo;
	size_t size;
	int err;

	size = level ? sizeof(struct xe_pt_dir) : sizeof(struct xe_pt_0);
	pt = kzalloc(size, GFP_KERNEL);
	if (!pt)
		return NULL;

	bo = xe_bo_create(vm->xe, vm, SZ_4K, ttm_bo_type_kernel,
			  XE_BO_CREATE_VRAM_IF_DGFX(vm->xe));
	if (IS_ERR(bo)) {
		err = PTR_ERR(bo);
		goto err_kfree;
	}
	pt->bo = bo;
	pt->level = level;

	XE_BUG_ON(level > XE_VM_MAX_LEVEL);

	ttm_bo_pin(&bo->ttm);
	return pt;

err_kfree:
	kfree(pt);
	return ERR_PTR(err);
}

static int xe_pt_populate_empty(struct xe_vm *vm, struct xe_pt *pt)
{
	struct ttm_bo_kmap_obj map;
	uint64_t empty;
	int err, i;

	err = __xe_pt_kmap(pt, &map);
	if (err)
		return err;

	empty = __xe_vm_empty_pte(vm, pt->level);
	for (i = 0; i < GEN8_PDES; i++)
		__xe_pt_write(&map, i, empty);

	ttm_bo_kunmap(&map);
	return 0;
}

static u64 xe_pt_shift(unsigned int level)
{
	return GEN8_PTE_SHIFT + GEN8_PDE_SHIFT * level;
}

static unsigned int xe_pt_idx(uint64_t addr, unsigned int level)
{
	return (addr >> xe_pt_shift(level)) & GEN8_PDE_MASK;
}

static uint64_t xe_pt_next_start(uint64_t start, unsigned int level)
{
	uint64_t pt_range = 1ull << xe_pt_shift(level);

	return ALIGN_DOWN(start + pt_range, pt_range);
}

static uint64_t xe_pt_prev_end(uint64_t end, unsigned int level)
{
	uint64_t pt_range = 1ull << xe_pt_shift(level);

	return ALIGN_DOWN(end - 1, pt_range);
}

static int xe_pt_populate_for_vma(struct xe_vma *vma, struct xe_pt *pt,
				  u64 start, u64 end)
{
	u32 start_ofs = xe_pt_idx(start, pt->level);
	u32 last_ofs = xe_pt_idx(end - 1, pt->level);
	struct ttm_bo_kmap_obj map;
	struct xe_vm *vm = vma->vm;
	bool init = !pt->num_live;
	u32 i;
	int err;

	if (pt->level) {
		struct xe_pt_dir *pt_dir = as_xe_pt_dir(pt);
		u64 cur = start;

		for (i = start_ofs; i <= last_ofs; i++) {
			u64 next_start = xe_pt_next_start(cur, pt->level);

			if (!pt_dir->entries[i]) {
				struct xe_pt *pte =
					xe_pt_create(vm, pt->level - 1);

				if (IS_ERR(pte))
					return PTR_ERR(pte);

				pt_dir->entries[i] = pte;
				pt_dir->pt.num_live++;
			}

			err = xe_pt_populate_for_vma(vma, pt_dir->entries[i],
						     cur, min(next_start, end));
			if (err)
				return err;

			cur = next_start;
		}
	} else {
		/* newly added entries only, evict didn't decrease num_live */
		if (!vma->evicted)
			pt->num_live += last_ofs + 1 - start_ofs;
	}

	/* any pte entries now exist, fill in now */
	err = __xe_pt_kmap(pt, &map);
	if (err)
		return err;

	if (init) {
		u64 empty = __xe_vm_empty_pte(vma->vm, pt->level);

		for (i = 0; i < start_ofs; i++)
			__xe_pt_write(&map, i, empty);
		for (i = last_ofs + 1; i < GEN8_PDES; i++)
			__xe_pt_write(&map, i, empty);
	}

	if (pt->level) {
		struct xe_pt_dir *pt_dir = as_xe_pt_dir(pt);

		for (i = start_ofs; i <= last_ofs; i++)
			__xe_pt_write(&map, i, gen8_pde_encode(pt_dir->entries[i]->bo, 0, XE_CACHE_WB));
	} else {
		u64 bo_offset = vma->bo_offset + (start - vma->start);

		for (i = start_ofs; i <= last_ofs; i++, bo_offset += GEN8_PAGE_SIZE)
			__xe_pt_write(&map, i, gen8_pte_encode(vma->bo, bo_offset, XE_CACHE_WB, 0));
	}

	ttm_bo_kunmap(&map);
	return 0;
}

static void xe_pt_destroy(struct xe_pt *pt)
{
	int i;

	XE_BUG_ON(!list_empty(&pt->bo->vmas));
	ttm_bo_unpin(&pt->bo->ttm);
	xe_bo_put(pt->bo);

	if (pt->level > 0 && pt->num_live) {
		struct xe_pt_dir *pt_dir = as_xe_pt_dir(pt);

		for (i = 0; i < GEN8_PDES; i++) {
			if (pt_dir->entries[i])
				xe_pt_destroy(pt_dir->entries[i]);
		}
	}
	kfree(pt);
}

static struct xe_vma *xe_vma_create(struct xe_vm *vm,
				    struct xe_bo *bo, uint64_t bo_offset,
				    uint64_t start, uint64_t end)
{
	struct xe_vma *vma;

	XE_BUG_ON(start >= end);
	XE_BUG_ON(end >= vm->size);

	vma = kzalloc(sizeof(*vma), GFP_KERNEL);
	if (!vma)
		return NULL;

	vma->vm = vm;
	vma->start = start;
	vma->end = end;

	xe_bo_assert_held(bo);

	vma->bo = xe_bo_get(bo);
	vma->bo_offset = bo_offset;
	list_add_tail(&vma->bo_link, &bo->vmas);

	return vma;
}

static void xe_vma_destroy(struct xe_vma *vma)
{
	list_del(&vma->bo_link);
	xe_bo_put(vma->bo);
	kfree(vma);
}

static struct xe_vma *to_xe_vma(const struct rb_node *node)
{
	BUILD_BUG_ON(offsetof(struct xe_vma, vm_node) != 0);
	return (struct xe_vma *)node;
}

static int xe_vma_cmp(const struct xe_vma *a, const struct xe_vma *b)
{
	if (a->end < b->start) {
		return -1;
	} else if (b->end < a->start) {
		return 1;
	} else {
		return 0;
	}
}

static bool xe_vma_less_cb(struct rb_node *a, const struct rb_node *b)
{
	return xe_vma_cmp(to_xe_vma(a), to_xe_vma(b)) < 0;
}

static int xe_vma_cmp_vma_cb(const void *key, const struct rb_node *node)
{
	struct xe_vma *cmp = to_xe_vma(node);
	const struct xe_vma *own = key;

	if (own->start > cmp->end)
		return 1;

	if (own->end < cmp->start)
		return -1;

	return 0;
}

static struct xe_vma *
xe_vm_find_overlapping_vma(struct xe_vm *vm, const struct xe_vma *vma)
{
	struct rb_node *node;

	XE_BUG_ON(vma->end >= vm->size);

	node = rb_find(vma, &vm->vmas, xe_vma_cmp_vma_cb);

	return node ? to_xe_vma(node) : NULL;
}

static void xe_vm_insert_vma(struct xe_vm *vm, struct xe_vma *vma)
{
	XE_BUG_ON(vma->vm != vm);

	rb_add(&vma->vm_node, &vm->vmas, xe_vma_less_cb);
}

static void xe_vm_remove_vma(struct xe_vm *vm, struct xe_vma *vma)
{
	XE_BUG_ON(vma->vm != vm);

	rb_erase(&vma->vm_node, &vm->vmas);
}

struct xe_vm *xe_vm_create(struct xe_device *xe, uint32_t flags)
{
	struct xe_vm *vm;
	struct xe_vma *vma;
	int err, i = 0;

	vm = kzalloc(sizeof(*vm), GFP_KERNEL);
	if (!vm)
		return ERR_PTR(-ENOMEM);

	vm->xe = xe;
	kref_init(&vm->refcount);
	dma_resv_init(&vm->resv);

	vm->size = 1ull << 48;

	vm->vmas = RB_ROOT;

	xe_vm_lock(vm, NULL);

	vm->pt_root = xe_pt_create(vm, 3);
	if (IS_ERR(vm->pt_root)) {
		err = PTR_ERR(vm->pt_root);
		goto err_unlock;
	}

	if (flags & DRM_XE_VM_CREATE_SCRATCH_PAGE) {
		vm->scratch_bo = xe_bo_create(xe, vm, SZ_4K,
					      ttm_bo_type_kernel,
					      XE_BO_CREATE_VRAM_IF_DGFX(xe));
		if (IS_ERR(vm->scratch_bo)) {
			err = PTR_ERR(vm->scratch_bo);
			goto err_destroy_root;
		}

		for (i = 0; i < vm->pt_root->level; i++) {
			vm->scratch_pt[i] = xe_pt_create(vm, i);
			if (IS_ERR(vm->scratch_pt[i])) {
				err = PTR_ERR(vm->scratch_pt[i]);
				goto err_scratch_pt;
			}
			err = xe_pt_populate_empty(vm, vm->scratch_pt[i]);
			if (err) {
				xe_pt_destroy(vm->scratch_pt[i]);
				goto err_scratch_pt;
			}
		}
	}

	/* Fill pt_root after allocating scratch tables */
	err = xe_pt_populate_empty(vm, vm->pt_root);
	if (err)
		goto err_destroy_root;

	xe_vm_unlock(vm);
	return vm;

err_scratch_pt:
	while (i)
		xe_pt_destroy(vm->scratch_pt[--i]);
	xe_bo_put(vm->scratch_bo);
err_destroy_root:
	xe_pt_destroy(vm->pt_root);
err_unlock:
	xe_vm_unlock(vm);
	kfree(vma);
	dma_resv_fini(&vm->resv);
	kfree(vm);
	return ERR_PTR(err);
}

void xe_vm_close_and_put(struct xe_vm *vm)
{
	struct rb_root contested = RB_ROOT;

	xe_vm_lock(vm, NULL);
	while (vm->vmas.rb_node) {
		struct xe_vma *vma = to_xe_vma(vm->vmas.rb_node);

		rb_erase(&vma->vm_node, &vm->vmas);

		/* easy case, remove from VMA? */
		if (vma->bo->vm) {
			list_del(&vma->bo_link);
			xe_bo_put(vma->bo);
			kfree(vma);
			continue;
		}

		rb_add(&vma->vm_node, &contested, xe_vma_less_cb);
	}

	/*
	 * All vm operations will add shared fences to resv.
	 * The only exception is eviction for a shared object,
	 * but even so, the unbind when evicted would still
	 * install a fence to resv. Hence it's safe to
	 * destroy the pagetables immediately.
	 */
	if (vm->scratch_bo) {
		u32 i;

		xe_bo_put(vm->scratch_bo);
		for (i = 0; i < vm->pt_root->level; i++)
			xe_pt_destroy(vm->scratch_pt[i]);
	}
	xe_pt_destroy(vm->pt_root);
	vm->size = 0;
	vm->pt_root = NULL;

	xe_vm_unlock(vm);
	if (contested.rb_node) {

		/*
		 * VM is now dead, cannot re-add nodes to vm->vmas if it's NULL
		 * Since we hold a refcount to the bo, we can remove and free
		 * the members safely without locking.
		 */
		while (contested.rb_node) {
			struct xe_vma *vma = to_xe_vma(contested.rb_node);

			rb_erase(&vma->vm_node, &contested);

			xe_bo_lock_no_vm(vma->bo, NULL);
			list_del(&vma->bo_link);
			xe_bo_unlock_no_vm(vma->bo);

			xe_bo_put(vma->bo);
			kfree(vma);
		}
	}

	xe_vm_put(vm);
}

void xe_vm_free(struct kref *ref)
{
	struct xe_vm *vm = container_of(ref, struct xe_vm, refcount);

	/* xe_vm_close_and_put was not called? */
	XE_WARN_ON(vm->pt_root);

	dma_resv_fini(&vm->resv);
	kfree(vm);
}

struct xe_vm *xe_vm_lookup(struct xe_file *xef, u32 id)
{
	struct xe_vm *vm;

	mutex_lock(&xef->vm.lock);
	vm = xa_load(&xef->vm.xa, id);
	mutex_unlock(&xef->vm.lock);

	if (vm)
		xe_vm_get(vm);

	return vm;
}

uint64_t xe_vm_pdp4_descriptor(struct xe_vm *vm)
{
	return gen8_pde_encode(vm->pt_root->bo, 0, XE_CACHE_WB);
}

static inline void
xe_vm_printk(const char *prefix, struct xe_vm *vm)
{
	struct rb_node *node;

	for (node = rb_first(&vm->vmas); node; node = rb_next(node)) {
		struct xe_vma *vma = to_xe_vma(node);

		printk("%s [0x%08x %08x, 0x%08x %08x]: BO(%p) + 0x%llx\n",
		       prefix,
		       upper_32_bits(vma->start),
		       lower_32_bits(vma->start),
		       upper_32_bits(vma->end),
		       lower_32_bits(vma->end),
		       vma->bo, vma->bo_offset);
	}
}

static void
xe_migrate_clear_pgtable_callback(void *ptr, u32 qword_ofs, u32 num_qwords,
				  struct xe_vm_pgtable_update *update,
				  void *arg)
{
	struct xe_vma *vma = arg;
	u64 empty = __xe_vm_empty_pte(vma->vm, update->pt->level);

	memset64(ptr, empty, num_qwords);
}

static void
xe_pt_commit_unbind(struct xe_vma *vma,
		    struct xe_vm_pgtable_update *entries, u32 num_entries)
{
	while (num_entries--) {
		struct xe_vm_pgtable_update *entry = &entries[num_entries];
		struct xe_pt *pt = entry->pt;

		pt->num_live -= entries->qwords;
		if (pt->level) {
			struct xe_pt_dir *pt_dir = as_xe_pt_dir(pt);
			u32 i;

			for (i = entry->ofs; i < entry->ofs + entry->qwords; i++) {
				if (pt_dir->entries[i])
					xe_pt_destroy(pt_dir->entries[i]);

				pt_dir->entries[i] = NULL;
			}
		}
	}
}

static inline bool xe_pt_partial_entry(u64 start, u64 end, u32 level)
{
	u64 pte_size = 1ULL << xe_pt_shift(level);

	XE_BUG_ON(end < start);
	XE_BUG_ON(end - start > pte_size);

	return start + pte_size != end;
}

static void
__xe_pt_prepare_unbind(struct xe_vma *vma, struct xe_pt *pt,
		       u32 *removed_parent_pte,
		       u64 start, u64 end,
		       u32 *num_entries, struct xe_vm_pgtable_update *entries,
		       bool evict)
{
	u32 my_removed_pte = 0;
	struct xe_vm_pgtable_update *entry;
	u32 start_ofs = xe_pt_idx(start, pt->level);
	u32 last_ofs = xe_pt_idx(end - 1, pt->level);
	u32 num_live;

	/*
	 * When evicting, we don't hold vma->resv, so we can not make any
	 * assumptions about pt->num_live, as others may be mapped into it.
	 *
	 * We only know that the object lock protects against altering page tables
	 * that it's mapped into, so we can do a read-only walk for all PT's that
	 * the object is bound to.
	 */
	if (!evict)
		num_live = pt->num_live;
	else
		num_live = GEN8_PDES;

	if (!pt->level) {
		my_removed_pte = last_ofs - start_ofs + 1;

		BUG_ON(!my_removed_pte);
	} else {
		struct xe_pt_dir *pt_dir = as_xe_pt_dir(pt);

		if (pt_dir->entries[start_ofs]) {
			u64 pte_end = min(xe_pt_next_start(start, pt->level), end);

			__xe_pt_prepare_unbind(vma, pt_dir->entries[start_ofs],
					       &my_removed_pte, start, pte_end,
					       num_entries, entries, evict);

			/* first entry kept? */
			if (!my_removed_pte)
				start_ofs++;
		} else {
			my_removed_pte++;
		}

		if (start_ofs < last_ofs) {
			/* middle part */
			my_removed_pte += last_ofs - start_ofs - 1;

			/* last */
			if (pt_dir->entries[last_ofs]) {
				u64 end_start = xe_pt_prev_end(end, pt->level);

				__xe_pt_prepare_unbind(vma,
						       pt_dir->entries[last_ofs],
						       &my_removed_pte,
						       end_start, end,
						       num_entries, entries,
						       evict);
			} else {
				my_removed_pte++;
			}
		}

		/* No changes to this entry, fast return.. */
		if (!my_removed_pte)
			return;
	}

	/* Don't try to delete the root.. */
	if (removed_parent_pte && num_live == my_removed_pte) {
		*removed_parent_pte += 1;
		return;
	}

	entry = &entries[(*num_entries)++];
	entry->pt_bo = pt->bo;
	entry->ofs = start_ofs;
	entry->qwords = my_removed_pte;
	entry->pt = pt;
	entry->target = vma->bo;
	entry->target_offset = vma->bo_offset + (start - vma->start);
}

static void
xe_pt_prepare_unbind(struct xe_vma *vma,
		     struct xe_vm_pgtable_update *entries,
		     u32 *num_entries, bool evict)
{
	*num_entries = 0;
	__xe_pt_prepare_unbind(vma, vma->vm->pt_root, NULL,
			       vma->start, vma->end + 1,
			       num_entries, entries, evict);
	XE_BUG_ON(!*num_entries);
}

struct dma_fence *xe_vm_unbind_vma(struct xe_vma *vma, struct xe_sync_entry *syncs, u32 num_syncs, bool evict)
{
	struct xe_vm_pgtable_update entries[XE_VM_MAX_LEVEL * 2 + 1];
	struct xe_vm *vm = vma->vm;
	struct xe_gt *gt = to_gt(vm->xe);
	u32 num_entries;
	struct dma_fence *fence = NULL;

	xe_bo_assert_held(vma->bo);
	if (!evict)
		xe_vm_assert_held(vm);

	XE_WARN_ON(vma->evicted && evict);

	xe_pt_prepare_unbind(vma, entries, &num_entries, evict);
	XE_BUG_ON(num_entries > ARRAY_SIZE(entries));

	/*
	 * Even if we were already evicted and unbind to destroy, we need to
	 * clear again here. The eviction may have updated pagetables at a
	 * lower level, because it needs to be more conservative.
	 */
	fence = xe_migrate_update_pgtables(gt->migrate,
					   vm->preempt.enabled ? vm : NULL,
					   entries, num_entries,
					   syncs, num_syncs,
					   xe_migrate_clear_pgtable_callback, vma);
	if (!IS_ERR(fence)) {
		if (!evict) {
			/* add shared fence now for pagetable delayed destroy */
			dma_resv_add_shared_fence(&vm->resv, fence);

			/* This fence will be installed by caller when doing eviction */
			if (!vma->bo->vm)
				dma_resv_add_shared_fence(vma->bo->ttm.base.resv, fence);
			xe_pt_commit_unbind(vma, entries, num_entries);
		}
		vma->evicted = evict;
	}

	return fence;
}

static void
xe_vm_populate_pgtable(void *data, u32 qword_ofs, u32 num_qwords,
		       struct xe_vm_pgtable_update *update, void *arg)
{
	u64 bo_offset = update->target_offset +
		GEN8_PAGE_SIZE * (qword_ofs - update->ofs);
	struct xe_pt **ptes = update->pt_entries;
	u64 *ptr = data;
	u32 i;

	for (i = 0; i < num_qwords; i++, bo_offset += GEN8_PAGE_SIZE) {
		if (ptes && ptes[i])
			ptr[i] = gen8_pde_encode(ptes[i]->bo, 0, XE_CACHE_WB);
		else
			ptr[i] = gen8_pte_encode(update->target, bo_offset, XE_CACHE_WB, 0);
	}
}

static void xe_pt_abort_bind(struct xe_vma *vma, struct xe_vm_pgtable_update *entries, u32 num_entries)
{
	u32 i, j;

	for (i = 0; i < num_entries; i++) {
		if (!entries[i].pt_entries)
			continue;

		for (j = 0; j < entries[i].qwords; j++)
			xe_pt_destroy(entries[i].pt_entries[j]);
		kfree(entries[i].pt_entries);
	}
}

static void xe_pt_commit_bind(struct xe_vma *vma, struct xe_vm_pgtable_update *entries, u32 num_entries)
{
	u32 i, j;

	for (i = 0; i < num_entries; i++) {
		struct xe_pt *pt = entries[i].pt;
		struct xe_pt_dir *pt_dir;

		if (!vma->evicted)
			pt->num_live += entries[i].qwords;

		if (!pt->level)
			continue;

		pt_dir = as_xe_pt_dir(pt);
		for (j = 0; j < entries[i].qwords; j++) {
			u32 j_ = j + entries[i].ofs;
			struct xe_pt *newpte = entries[i].pt_entries[j];

			if (pt_dir->entries[j_])
				xe_pt_destroy(pt_dir->entries[j_]);

			pt_dir->entries[j_] = newpte;
		}
		kfree(entries[i].pt_entries);
	}
}

static int
__xe_pt_prepare_bind(struct xe_vma *vma, struct xe_pt *pt,
		     u64 start, u64 end,
		     u32 *num_entries, struct xe_vm_pgtable_update *entries)
{
	struct xe_device *xe = vma->vm->xe;
	u32 my_added_pte = 0;
	struct xe_vm_pgtable_update *entry;
	u32 start_ofs = xe_pt_idx(start, pt->level);
	u32 last_ofs = xe_pt_idx(end - 1, pt->level);
	struct xe_pt **pte = NULL;

	XE_BUG_ON(start < vma->start);
	XE_BUG_ON(end > vma->end + 1);

	my_added_pte = last_ofs + 1 - start_ofs;
	BUG_ON(!my_added_pte);

	if (!pt->level) {
		vm_dbg(&xe->drm, "\t%u: Populating entry [%u + %u) [%llx...%llx)\n",
		       pt->level, start_ofs, my_added_pte, start, end);
	} else {
		struct xe_pt_dir *pt_dir = as_xe_pt_dir(pt);
		u32 i;
		u64 start_end = min(xe_pt_next_start(start, pt->level), end);
		u64 end_start = max(start, xe_pt_prev_end(end, pt->level));
		u64 cur = start;
		int err;
		bool partial_begin = false, partial_end = false;

		if (pt_dir->entries[start_ofs])
			partial_begin = xe_pt_partial_entry(start, start_end, pt->level);

		if (pt_dir->entries[last_ofs] && last_ofs > start_ofs)
			partial_end = xe_pt_partial_entry(end_start, end, pt->level);

		my_added_pte -= partial_begin + partial_end;

		vm_dbg(&xe->drm, "\t%u: [%llx...%llx) partial begin/end: %u / %u, %u entries\n",
		       pt->level, start, end, partial_begin, partial_end,
		       my_added_pte);

		/* Prepare partially filled first part.. */
		if (partial_begin) {
			vm_dbg(&xe->drm, "\t%u: Descending to first subentry %u level %u [%llx...%llx)\n",
			       pt->level, start_ofs,
			       pt->level - 1, start, start_end);
			err = __xe_pt_prepare_bind(vma,
						   pt_dir->entries[start_ofs++],
						   start, start_end,
						   num_entries, entries);
			if (err)
				return err;

			start = cur = start_end;
		}

		/* optional middle part, includes begin/end if not partial */
		pte = kmalloc_array(my_added_pte, sizeof(*pte), GFP_KERNEL);
		if (!pte)
			return -ENOMEM;

		for (i = 0; i < my_added_pte; i++) {
			struct xe_pt *entry;
			u64 cur_end =
				min(xe_pt_next_start(cur, pt->level), end);

			vm_dbg(&xe->drm, "\t%u: Populating %u/%u subentry %u level %u [%llx...%llx)\n",
			       pt->level, i + 1, my_added_pte,
			       start_ofs + i, pt->level - 1, cur, cur_end);

			entry = xe_pt_create(vma->vm, pt->level - 1);
			if (IS_ERR(entry)) {
				err = PTR_ERR(entry);
				goto unwind;
			}
			pte[i] = entry;

			err = xe_pt_populate_for_vma(vma, entry, cur, end);
			if (err) {
				xe_pt_destroy(entry);
				goto unwind;
			}

			cur = cur_end;
		}

		/* last? */
		if (partial_end) {
			XE_WARN_ON(cur >= end);
			XE_WARN_ON(cur != end_start);

			vm_dbg(&xe->drm, "\t%u: Descending to last subentry %u level %u [%llx...%llx)\n",
			       pt->level, last_ofs, pt->level - 1, cur, end);

			err = __xe_pt_prepare_bind(vma, pt_dir->entries[last_ofs], cur, end, num_entries, entries);
			if (err)
				goto unwind;
		}

		/* No changes to this entry, fast return, no need to free 0 size ptr.. */
		if (!my_added_pte)
			return 0;
		goto done;

unwind:
		while (i--)
			xe_pt_destroy(pte[i]);

		kfree(pte);
		return err;
	}

done:
	entry = &entries[(*num_entries)++];
	entry->pt_bo = pt->bo;
	entry->ofs = start_ofs;
	entry->qwords = my_added_pte;
	entry->pt = pt;
	entry->target = vma->bo;
	entry->target_offset = vma->bo_offset + (start - vma->start);
	entry->pt_entries = pte;
	return 0;
}

static int
xe_pt_prepare_bind(struct xe_vma *vma,
		   struct xe_vm_pgtable_update *entries, u32 *num_entries)
{
	int err;

	vm_dbg(&vma->vm->xe->drm, "Preparing bind, with range [%llx...%llx)\n",
	       vma->start, vma->end);

	*num_entries = 0;
	err = __xe_pt_prepare_bind(vma, vma->vm->pt_root, vma->start, vma->end + 1, num_entries, entries);
	if (!err)
		BUG_ON(!*num_entries);
	else /* abort! */
		xe_pt_abort_bind(vma, entries, *num_entries);

	return err;
}

struct dma_fence *
xe_vm_bind_vma(struct xe_vma *vma, struct xe_sync_entry *syncs, u32 num_syncs)
{
	struct xe_vm_pgtable_update entries[XE_VM_MAX_LEVEL * 2 + 1];
	struct xe_vm *vm = vma->vm;
	struct xe_gt *gt = to_gt(vm->xe);
	u32 num_entries, i;
	struct dma_fence *fence;
	int err;

	xe_bo_assert_held(vma->bo);
	xe_vm_assert_held(vm);

	err = xe_pt_prepare_bind(vma, entries, &num_entries);
	if (err)
		goto err;
	XE_BUG_ON(num_entries > ARRAY_SIZE(entries));

	vm_dbg(&vm->xe->drm, "%u entries to update\n", num_entries);
	for (i = 0; i < num_entries; i++) {
		struct xe_vm_pgtable_update *entry = &entries[i];

		u64 start = vma->start + entry->target_offset - vma->bo_offset;
		u64 len = (u64)entry->qwords << xe_pt_shift(entry->pt->level);
		u64 end;

		start = xe_pt_prev_end(start + 1, entry->pt->level);
		end = start + len;

		vm_dbg(&vm->xe->drm, "\t%u: Update level %u at (%u + %u) [%llx...%llx)\n",
		       i, entry->pt->level, entry->ofs, entry->qwords,
		       start, end);
	}

	fence = xe_migrate_update_pgtables(gt->migrate,
					   vm->preempt.enabled ? vm : NULL,
					   entries, num_entries,
					   syncs, num_syncs,
					   xe_vm_populate_pgtable, vma);
	if (!IS_ERR(fence)) {
		/* add shared fence now for pagetable delayed destroy */
		dma_resv_add_shared_fence(&vm->resv, fence);

		if (!vma->bo->vm)
			dma_resv_add_shared_fence(vma->bo->ttm.base.resv, fence);
		xe_pt_commit_bind(vma, entries, num_entries);

		/* This vma is live (again?) now */
		vma->evicted = false;
	} else {
		xe_pt_abort_bind(vma, entries, num_entries);
	}

	return fence;

err:
	return ERR_PTR(err);
}

struct preempt_op {
	struct xe_vm *vm;
	struct dma_fence_cb cb;
	struct work_struct worker;
};

static void preempt_op_worker(struct work_struct *w)
{
	struct preempt_op *op = container_of(w, struct preempt_op, worker);
	struct xe_vm *vm = op->vm;
	struct xe_preempt_fence *pfence, *next;

	XE_BUG_ON(!vm->preempt.enabled);

	xe_vm_lock(vm, NULL);
	if (!--vm->preempt.num_inflight_ops) {
		list_for_each_entry_safe(pfence, next,
					 &vm->preempt.pending_fences, link) {
			struct xe_engine *e = pfence->engine;
			int err;

			err = dma_resv_reserve_shared(&vm->resv, 1);
			XE_WARN_ON(err);
			if (!err) {
				e->ops->resume(e);
				dma_resv_add_shared_fence(&vm->resv,
							  &pfence->base);
			}
			list_del_init(&pfence->link);
			dma_fence_put(&pfence->base);
		}
	}
	xe_vm_unlock(vm);

	xe_vm_put(vm);
	kfree(op);
}

static void preempt_op_cb(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct preempt_op *op = container_of(cb, struct preempt_op, cb);

	INIT_WORK(&op->worker, preempt_op_worker);
	queue_work(system_unbound_wq, &op->worker);
}

static void add_preempt_op_cb(struct xe_vm *vm, struct dma_fence *fence,
			      struct preempt_op *op)
{
	int ret;

	xe_vm_assert_held(vm);

	op->vm = xe_vm_get(vm);
	ret = dma_fence_add_callback(fence, &op->cb, preempt_op_cb);
	if (!ret) {
		++vm->preempt.num_inflight_ops;
	} else {
		xe_vm_put(vm);
		if (ret != -ENOENT)
			XE_WARN_ON("fence add callback failed");
	}
}

static int xe_vm_bind(struct xe_vm *vm, struct xe_bo *bo,
		      u64 bo_offset, u64 range, u64 addr,
		      struct xe_sync_entry *syncs, u32 num_syncs)
{
	struct xe_vma *vma, *prev;
	struct dma_fence *fence;
	struct preempt_op *op = NULL;
	int err;

	xe_vm_assert_held(vm);
	xe_bo_assert_held(bo);

	err = xe_bo_populate(bo);
	if (err)
		goto err;

	vma = xe_vma_create(vm, bo, bo_offset, addr, addr + range - 1);
	if (!vma) {
		err = -ENOMEM;
		goto err;
	}

	/*
	 * If preempt is enabled (a compute engine uses this VM), on every VM
	 * un/bind we trigger all preempt fences (in shared slots of this VM) by
	 * waiting on the excl slot of this VM. The preempt fences will create
	 * new preempt fences and either resume their engines scheduling +
	 * insert the new fences into the VM shared slots or defers this until
	 * all operations that triggered the fence are complete.
	 *
	 * FIXME: We likely don't have to do this on every un/bind, but doing it
	 * for now to test this code and show how preemption fences + VM un/bind
	 * code interacts.
	 */
	if (vm->preempt.enabled) {
		op = kmalloc(sizeof(*op), GFP_KERNEL);
		if (!op) {
			err = -ENOMEM;
			goto err_destroy;
		}
	}

	prev = xe_vm_find_overlapping_vma(vm, vma);

	/* Already something mapped here? */
	if (prev) {
		printk(KERN_DEBUG "VM reserved [0x%08x %08x, 0x%08x %08x]\n",
		       upper_32_bits(vma->start), lower_32_bits(vma->start),
		       upper_32_bits(vma->end), lower_32_bits(vma->end));
		printk(KERN_DEBUG "Overlapping VM: [0x%08x %08x, 0x%08x %08x]\n",
		       upper_32_bits(prev->start), lower_32_bits(prev->start),
		       upper_32_bits(prev->end), lower_32_bits(prev->end));
		err = -EBUSY;
		goto err_destroy;
	}

	fence = xe_vm_bind_vma(vma, syncs, num_syncs);
	if (IS_ERR(fence)) {
		err = PTR_ERR(fence);
		goto err_free_op;
	}
	if (vm->preempt.enabled)
		add_preempt_op_cb(vm, fence, op);

	xe_vm_insert_vma(vm, vma);
#if 1 // REMOVEME when tests are fixed
	dma_fence_wait(fence, false);
#endif
	dma_fence_put(fence);
	return 0;

err_free_op:
	kfree(op);
err_destroy:
	xe_vma_destroy(vma);
err:
	return err;
}

static int xe_vm_unbind(struct xe_vm *vm, struct xe_bo *bo, u64 range,
			u64 addr, struct xe_sync_entry *syncs, u32 num_syncs)
{
	struct xe_device *xe = to_xe_device(bo->ttm.base.dev);
	struct xe_vma *vma, lookup;
	struct dma_fence *fence;
	struct preempt_op *op = NULL;

	xe_vm_assert_held(vm);
	xe_bo_assert_held(bo);

	lookup.start = addr;
	lookup.end = addr + range - 1;

	vma = xe_vm_find_overlapping_vma(vm, &lookup);

	if (XE_IOCTL_ERR(xe, !vma) ||
	    XE_IOCTL_ERR(xe, vma->bo != bo) ||
	    XE_IOCTL_ERR(xe, vma->start != addr) ||
	    XE_IOCTL_ERR(xe, vma->end != addr + range - 1))
		return -EINVAL;

	if (vm->preempt.enabled) {
		op = kmalloc(sizeof(*op), GFP_KERNEL);
		if (!op)
			return -ENOMEM;
	}

	fence = xe_vm_unbind_vma(vma, syncs, num_syncs, false);
	if (IS_ERR(fence)) {
		kfree(op);
		return PTR_ERR(fence);
	}
	if (vm->preempt.enabled)
		add_preempt_op_cb(vm, fence, op);

	xe_vm_remove_vma(vm, vma);
	xe_vma_destroy(vma);

#if 1 // REMOVEME when tests are fixed
	dma_fence_wait(fence, false);
#endif
	dma_fence_put(fence);
	return 0;
}

#define ALL_DRM_XE_VM_CREATE_FLAGS DRM_XE_VM_CREATE_SCRATCH_PAGE

int xe_vm_create_ioctl(struct drm_device *dev, void *data,
		       struct drm_file *file)
{
	struct xe_device *xe = to_xe_device(dev);
	struct xe_file *xef = to_xe_file(file);
	struct drm_xe_vm_create *args = data;
	struct xe_vm *vm;
	u32 id;
	int err;

	if (XE_IOCTL_ERR(xe, args->extensions))
		return -EINVAL;

	if (XE_IOCTL_ERR(xe, args->flags & ~ALL_DRM_XE_VM_CREATE_FLAGS))
		return -EINVAL;

	vm = xe_vm_create(xe, args->flags);
	if (IS_ERR(vm))
		return PTR_ERR(vm);

	mutex_lock(&xef->vm.lock);
	err = xa_alloc(&xef->vm.xa, &id, vm, xa_limit_32b, GFP_KERNEL);
	mutex_unlock(&xef->vm.lock);
	if (err) {
		xe_vm_close_and_put(vm);
		return err;
	}

	args->vm_id = id;

	return 0;
}

int xe_vm_destroy_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file)
{
	struct xe_device *xe = to_xe_device(dev);
	struct xe_file *xef = to_xe_file(file);
	struct drm_xe_vm_destroy *args = data;
	struct xe_vm *vm;

	if (XE_IOCTL_ERR(xe, args->pad))
		return -EINVAL;

	mutex_lock(&xef->vm.lock);
	vm = xa_erase(&xef->vm.xa, args->vm_id);
	mutex_unlock(&xef->vm.lock);
	if (XE_IOCTL_ERR(xe, !vm))
		return -ENOENT;

	xe_vm_close_and_put(vm);

	return 0;
}

static int __xe_vm_bind_ioctl(struct xe_vm *vm, struct xe_bo *bo, u64 bo_offset,
			      u64 range, u64 addr, u32 op,
			      struct xe_sync_entry *syncs, u32 num_syncs)
{
	struct xe_device *xe = to_xe_device(bo->ttm.base.dev);

	if (XE_IOCTL_ERR(xe, !vm->size)) {
		DRM_ERROR("VM closed while we began looking up?\n");
		return -ENOENT;
	}

	if (XE_IOCTL_ERR(xe, op > XE_VM_BIND_OP_UNMAP))
		return -EINVAL;

	if (XE_IOCTL_ERR(xe, bo_offset & ~PAGE_MASK) ||
	    XE_IOCTL_ERR(xe, addr & ~PAGE_MASK) ||
	    XE_IOCTL_ERR(xe, range & ~PAGE_MASK))
		return -EINVAL;

	/* vm arguments sane? */
	if (XE_IOCTL_ERR(xe, !range) ||
	    XE_IOCTL_ERR(xe, range > vm->size) ||
	    XE_IOCTL_ERR(xe, addr > vm->size - range))
		return -EINVAL;

	/* bo sane? */
	if (XE_IOCTL_ERR(xe, range > bo->size) ||
	    XE_IOCTL_ERR(xe, bo_offset > bo->size - range))
		return -EINVAL;

	switch (op) {
	case XE_VM_BIND_OP_MAP:
		return xe_vm_bind(vm, bo, bo_offset, range, addr, syncs, num_syncs);
	case XE_VM_BIND_OP_UNMAP:
		return xe_vm_unbind(vm, bo, range, addr, syncs, num_syncs);
	default:
		XE_IOCTL_ERR(xe, op > XE_VM_BIND_OP_UNMAP);
		return -EINVAL;
	}
}

static void xe_vm_tv_populate(struct xe_vm *vm, struct ttm_validate_buffer *tv)
{
	tv->num_shared = 1;
	tv->bo = &vm->pt_root->bo->ttm;
}

int xe_vm_bind_ioctl(struct drm_device *dev, void *data,
		     struct drm_file *file)
{
	struct xe_device *xe = to_xe_device(dev);
	struct xe_file *xef = to_xe_file(file);
	struct drm_xe_vm_bind *args = data;
	struct drm_xe_sync __user *syncs_user;
	struct drm_gem_object *gem_obj = NULL;
	LIST_HEAD(objs);
	LIST_HEAD(dups);
	struct ttm_validate_buffer tv_bo, tv_vm;
	struct ww_acquire_ctx ww;
	struct xe_bo *bo;
	struct xe_vm *vm;
	int err = 0;
	u32 num_syncs;
	struct xe_sync_entry *syncs;

	if (XE_IOCTL_ERR(xe, args->extensions) ||
	    XE_IOCTL_ERR(xe, args->op > XE_VM_BIND_OP_UNMAP))
		return -EINVAL;

	vm = xe_vm_lookup(xef, args->vm_id);
	if (XE_IOCTL_ERR(xe, !vm))
		return -ENOENT;

	xe_vm_tv_populate(vm, &tv_vm);
	list_add_tail(&tv_vm.head, &objs);

	gem_obj = drm_gem_object_lookup(file, args->obj);
	if (XE_IOCTL_ERR(xe, !gem_obj)) {
		err = -ENOENT;
		goto put_vm;
	}

	syncs = kcalloc(args->num_syncs, sizeof(*syncs), GFP_KERNEL);
	if (!syncs) {
		err = -ENOMEM;
		goto put_obj;
	}

	syncs_user = u64_to_user_ptr(args->syncs);
	for (num_syncs = 0; num_syncs < args->num_syncs; num_syncs++) {
		err = xe_sync_entry_parse(xe, xef, &syncs[num_syncs], &syncs_user[num_syncs]);
		if (err)
			goto free_syncs;
	}

	bo = gem_to_xe_bo(gem_obj);

	tv_bo.bo = &bo->ttm;
	tv_bo.num_shared = 1;
	list_add(&tv_bo.head, &objs);

	err = ttm_eu_reserve_buffers(&ww, &objs, true, &dups);
	if (!err) {
		err = __xe_vm_bind_ioctl(vm, bo, args->obj_offset,
					args->range, args->addr, args->op,
					syncs, num_syncs);
		ttm_eu_backoff_reservation(&ww, &objs);
	}

free_syncs:
	while (num_syncs--)
		xe_sync_entry_cleanup(&syncs[num_syncs]);

	kfree(syncs);
put_obj:
	drm_gem_object_put(gem_obj);
put_vm:
	xe_vm_put(vm);
	return err;
}
