/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#include "xe_vm.h"

#include <drm/ttm/ttm_execbuf_util.h>
#include <drm/ttm/ttm_tt.h>
#include <drm/xe_drm.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/swap.h>

#include "xe_bo.h"
#include "xe_device.h"
#include "xe_engine.h"
#include "xe_gt.h"
#include "xe_migrate.h"
#include "xe_preempt_fence_types.h"
#include "xe_sync.h"
#include "xe_trace.h"

enum xe_cache_level {
	XE_CACHE_NONE,
	XE_CACHE_WT,
	XE_CACHE_WB,
};

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
	pde |= GEN8_PAGE_PRESENT | GEN8_PAGE_RW;

	XE_WARN_ON(IS_DGFX(xe_bo_device(bo)) && !is_lmem);

	if (level != XE_CACHE_NONE)
		pde |= PPAT_CACHED_PDE;
	else
		pde |= PPAT_UNCACHED;

	return pde;
}

static bool vma_is_userptr(struct xe_vma *vma);

static dma_addr_t vma_addr(struct xe_vma *vma, uint64_t offset,
			   size_t page_size, bool *is_lmem)
{
	if (vma_is_userptr(vma)) {
		uint64_t page = offset >> PAGE_SHIFT;

		*is_lmem = false;
		offset &= (PAGE_SIZE - 1);

		return vma->userptr.dma_address[page] + offset;
	} else {
		return xe_bo_addr(vma->bo, offset, page_size, is_lmem);
	}
}

static uint64_t gen8_pte_encode(struct xe_vma *vma, struct xe_bo *bo,
				uint64_t offset, enum xe_cache_level level,
				uint32_t flags)
{
	uint64_t pte;
	bool is_lmem;

	if (vma)
		pte = vma_addr(vma, offset, GEN8_PAGE_SIZE, &is_lmem);
	else
		pte = xe_bo_addr(bo, offset, GEN8_PAGE_SIZE, &is_lmem);
	pte |= GEN8_PAGE_PRESENT | GEN8_PAGE_RW;

	if (unlikely(flags & PTE_READ_ONLY))
		pte &= ~GEN8_PAGE_RW;

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
		return gen8_pte_encode(NULL, vm->scratch_bo, 0, XE_CACHE_WB, 0);
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

static uint64_t __xe_pt_read(struct ttm_bo_kmap_obj *map,
		unsigned int idx)
{
	bool is_iomem;
	uint64_t *map_u64;

	map_u64 = ttm_kmap_obj_virtual(map, &is_iomem);
	if (is_iomem)
		return readq((uint64_t __iomem *)&map_u64[idx]);

	return map_u64[idx];
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
			  XE_BO_CREATE_VRAM_IF_DGFX(vm->xe) |
			  XE_BO_CREATE_IGNORE_MIN_PAGE_SIZE_BIT);
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
	int numpte = GEN8_PDES;
	int flags = 0;

	err = __xe_pt_kmap(pt, &map);
	if (err)
		return err;

	if (vm->flags & VM_FLAGS_64K && pt->level == 1) {
		numpte = 32;
		flags = GEN12_PDE_64K;
	}

	empty = __xe_vm_empty_pte(vm, pt->level) | flags;
	for (i = 0; i < numpte; i++)
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
	u32 page_size = GEN8_PAGE_SIZE;
	u32 numpdes = GEN8_PDES;
	u32 flags = 0;

	if (vma->bo && vma->bo->flags & XE_BO_INTERNAL_64K) {
		page_size = SZ_64K;
		if (pt->level == 1)
			flags = GEN12_PDE_64K;
		else if (pt->level == 0) {
			numpdes = 32;
			start_ofs = start_ofs / 16;
			last_ofs = last_ofs / 16;
		}
	}

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
		u64 empty = __xe_vm_empty_pte(vma->vm, pt->level) | flags;

		for (i = 0; i < start_ofs; i++)
			__xe_pt_write(&map, i, empty);
		for (i = last_ofs + 1; i < numpdes; i++)
			__xe_pt_write(&map, i, empty);
	}

	if (pt->level) {
		struct xe_pt_dir *pt_dir = as_xe_pt_dir(pt);

		for (i = start_ofs; i <= last_ofs; i++)
			__xe_pt_write(&map, i, gen8_pde_encode(pt_dir->entries[i]->bo,
							       0, XE_CACHE_WB) | flags);
	} else {
		u64 bo_offset = vma->bo_offset + (start - vma->start);

		for (i = start_ofs; i <= last_ofs; i++,
		     bo_offset += page_size)
			__xe_pt_write(&map, i, gen8_pte_encode(vma, vma->bo,
							       bo_offset,
							       XE_CACHE_WB,
							       vma->pte_flags));
	}

	ttm_bo_kunmap(&map);
	return 0;
}

static void xe_pt_destroy(struct xe_pt *pt, uint32_t flags)
{
	int i;
	int numpdes = GEN8_PDES;

	XE_BUG_ON(!list_empty(&pt->bo->vmas));
	ttm_bo_unpin(&pt->bo->ttm);
	xe_bo_put(pt->bo);

	if (pt->level == 0 && flags & VM_FLAGS_64K)
		numpdes = 32;

	if (pt->level > 0 && pt->num_live) {
		struct xe_pt_dir *pt_dir = as_xe_pt_dir(pt);

		for (i = 0; i < numpdes; i++) {
			if (pt_dir->entries[i])
				xe_pt_destroy(pt_dir->entries[i], flags);
		}
	}
	kfree(pt);
}

static bool vma_is_userptr(struct xe_vma *vma)
{
	return !vma->bo;
}

static int __vma_userptr_needs_repin(struct xe_vma *vma)
{
	/* TODO: lockdep assert */
	XE_BUG_ON(!vma_is_userptr(vma));

	if (mmu_interval_read_retry(&vma->userptr.notifier,
				    vma->userptr.notifier_seq))
		return -EAGAIN;

	return 0;
}

static int vma_userptr_needs_repin(struct xe_vma *vma)
{
	struct xe_vm *vm = vma->vm;
	int ret;

	read_lock(&vm->userptr.notifier_lock);
	ret = __vma_userptr_needs_repin(vma);
	read_unlock(&vm->userptr.notifier_lock);

	return ret;
}

static int vma_userptr_pin_pages(struct xe_vma *vma)
{
	struct xe_vm *vm = vma->vm;
	struct xe_device *xe = vm->xe;
	const unsigned long num_pages =
		(vma->end - vma->start + 1) >> PAGE_SHIFT;
	struct page **pages;
	bool in_kthread = !current->mm;
	unsigned long notifier_seq;
	int pinned, ret, i;
	bool read_only = vma->pte_flags & PTE_READ_ONLY;

	XE_BUG_ON(!vma_is_userptr(vma));

retry:
	if (vma->userptr.destroyed)
		return 0;

	notifier_seq = mmu_interval_read_begin(&vma->userptr.notifier);
	if (notifier_seq == vma->userptr.notifier_seq)
		return 0;

	pages = kmalloc(sizeof(*pages) * num_pages, GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	if (in_kthread)
		kthread_use_mm(vma->userptr.notifier.mm);

	pinned = ret = 0;
	while (pinned < num_pages) {
		ret = pin_user_pages_fast(vma->userptr.ptr + pinned * PAGE_SIZE,
					  num_pages - pinned,
					  read_only ? 0 : FOLL_WRITE,
					  &pages[pinned]);
		if (ret < 0)
			goto out;

		pinned += ret;
	}

	for (i = 0; i < pinned; ++i) {
		vma->userptr.dma_address[i] = dma_map_page(xe->drm.dev,
							   pages[i], 0,
							   PAGE_SIZE,
							   DMA_BIDIRECTIONAL);
		if (dma_mapping_error(xe->drm.dev,
				      vma->userptr.dma_address[i])) {
			ret = -EFAULT;
			goto out;
		}
	}

	for (i = 0; i < pinned; ++i) {
		if (!read_only && trylock_page(pages[i])) {
			set_page_dirty(pages[i]);
			unlock_page(pages[i]);
		}

		mark_page_accessed(pages[i]);
	}

out:
	if (in_kthread)
		kthread_unuse_mm(vma->userptr.notifier.mm);
	unpin_user_pages(pages, pinned);
	kfree(pages);

	if (!(ret < 0)) {
		vma->userptr.notifier_seq = notifier_seq;
		vma->userptr.dirty = true;
		if (vma_userptr_needs_repin(vma) == -EAGAIN)
			goto retry;
	}

	return ret < 0 ? ret : 0;
}

static void vm_userptr_pending_rebind_incr(struct xe_vm *vm)
{
	XE_BUG_ON(!xe_vm_has_preempt_fences(vm));
	lockdep_assert_held(&vm->userptr.notifier_lock);

	++vm->userptr.pending_rebind;
}

static int vm_userptr_pending_rebind_decr(struct xe_vm *vm)
{
	int val;

	XE_BUG_ON(!xe_vm_has_preempt_fences(vm));

	write_lock(&vm->userptr.notifier_lock);
	val = --vm->userptr.pending_rebind;
	write_unlock(&vm->userptr.notifier_lock);

	return val;
}

static void reinstall_preempt_fences(struct xe_vm *vm)
{
	struct xe_preempt_fence *pfence, *next;

	xe_vm_assert_held(vm);

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

struct dma_fence *
xe_vm_bind_vma(struct xe_vma *vma, struct xe_sync_entry *syncs, u32 num_syncs);

static void vma_rebind_work_func(struct work_struct *w)
{
	struct xe_vma *vma =
		container_of(w, struct xe_vma, userptr.rebind_work);
	struct xe_vm *vm = vma->vm;
	struct dma_fence *fence;
	int ret;

	XE_BUG_ON(!vma_is_userptr(vma));

	trace_xe_vma_userptr_rebind_worker(vma);

retry:
	ret = vma_userptr_pin_pages(vma);
	XE_WARN_ON(ret < 0);

	xe_vm_lock(vm, NULL);

	if (!vma->userptr.destroyed && vma->userptr.dirty) {
		ret = dma_resv_reserve_shared(&vm->resv, 1);
		if (!ret) {
			fence = xe_vm_bind_vma(vma, NULL, 0);
			if (!IS_ERR(fence)) {
				dma_fence_put(fence);
				ret = vma_userptr_needs_repin(vma);
				if (ret == -EAGAIN) {
					xe_vm_unlock(vm);
					goto retry;
				}
				XE_WARN_ON(ret);
			} else {
				XE_WARN_ON("xe_vm_bind_vma failed");
			}
		} else {
			XE_WARN_ON(ret);
		}
	}

	if (!vm_userptr_pending_rebind_decr(vm) &&
	    !vm->preempt.num_inflight_ops)
		reinstall_preempt_fences(vm);

	xe_vm_unlock(vm);
}

static void vma_destroy_work_func(struct work_struct *w)
{
	struct xe_vma *vma =
		container_of(w, struct xe_vma, userptr.destroy_work);
	struct xe_vm *vm = vma->vm;

	XE_BUG_ON(!vma_is_userptr(vma));
	XE_BUG_ON(!vma->userptr.destroyed);

	if (!list_empty(&vma->userptr_link)) {
		mutex_lock(&vm->userptr.list_lock);
		list_del(&vma->bo_link);
		mutex_unlock(&vm->userptr.list_lock);
	}

	kfree(vma->userptr.dma_address);
	mmu_interval_notifier_remove(&vma->userptr.notifier);
	flush_work(&vma->userptr.rebind_work);
	xe_vm_put(vm);
	kfree(vma);
}

static bool vma_userptr_invalidate(struct mmu_interval_notifier *mni,
				   const struct mmu_notifier_range *range,
				   unsigned long cur_seq)
{
	struct xe_vma *vma = container_of(mni, struct xe_vma, userptr.notifier);
	struct xe_vm *vm = vma->vm;
	long err;

	XE_BUG_ON(!vma_is_userptr(vma));
	trace_xe_vma_userptr_invalidate(vma);

	if (!mmu_notifier_range_blockable(range))
		return false;

	write_lock(&vm->userptr.notifier_lock);
	mmu_interval_set_seq(mni, cur_seq);

	/*
	 * Process exiting, userptr being destroyed, or VMA hasn't gone through
	 * initial bind, regardless nothing to do
	 */
	if (current->flags & PF_EXITING || vma->userptr.destroyed ||
	    !vma->userptr.initial_bind) {
		write_unlock(&vm->userptr.notifier_lock);
		return true;
	}

	if (xe_vm_has_preempt_fences(vm))
		vm_userptr_pending_rebind_incr(vm);
	write_unlock(&vm->userptr.notifier_lock);

	err = dma_resv_wait_timeout(&vm->resv, true, false,
				    MAX_SCHEDULE_TIMEOUT);
	XE_WARN_ON(err <= 0);

	/* If this VM has preemption fences installed, rebind the VMA */
	if (xe_vm_has_preempt_fences(vm))
		if (!queue_work(system_unbound_wq, &vma->userptr.rebind_work))
			vm_userptr_pending_rebind_decr(vm);

	return true;
}

static const struct mmu_interval_notifier_ops vma_userptr_notifier_ops = {
	.invalidate = vma_userptr_invalidate,
};

int xe_vm_userptr_pin(struct xe_vm *vm)
{
	struct xe_vma *vma;
	int err = 0;

	lockdep_assert_held(&vm->userptr.list_lock);
	if (!xe_vm_has_userptr(vm) || xe_vm_has_preempt_fences(vm))
		return 0;

	list_for_each_entry(vma, &vm->userptr.list, userptr_link) {
		err = vma_userptr_pin_pages(vma);
		if (err < 0)
			return err;
	}

	return 0;
}

int xe_vm_userptr_needs_repin(struct xe_vm *vm)
{
	struct xe_vma *vma;
	int err = 0;

	lockdep_assert_held(&vm->userptr.list_lock);
	if (!xe_vm_has_userptr(vm) || xe_vm_has_preempt_fences(vm))
		return 0;

	read_lock(&vm->userptr.notifier_lock);
	list_for_each_entry(vma, &vm->userptr.list, userptr_link) {
		err = __vma_userptr_needs_repin(vma);
		if (err)
			goto out_unlock;
	}

out_unlock:
	read_unlock(&vm->userptr.notifier_lock);
	return err;
}

struct dma_fence *xe_vm_userptr_bind(struct xe_vm *vm)
{
	struct dma_fence *fence = NULL;
	struct xe_vma *vma;

	lockdep_assert_held(&vm->userptr.list_lock);
	if (!xe_vm_has_userptr(vm) || xe_vm_has_preempt_fences(vm))
		return NULL;

	list_for_each_entry(vma, &vm->userptr.list, userptr_link) {
		if (vma->userptr.dirty) {
			dma_fence_put(fence);
			trace_xe_vma_userptr_rebind_exec(vma);
			fence = xe_vm_bind_vma(vma, NULL, 0);
		}
		if (IS_ERR(fence))
			return fence;
	}

	return fence;
}

static struct xe_vma *xe_vma_create(struct xe_vm *vm,
				    struct xe_bo *bo,
				    uint64_t bo_offset_or_userptr,
				    uint64_t start, uint64_t end,
				    bool read_only)
{
	struct xe_vma *vma;

	XE_BUG_ON(start >= end);
	XE_BUG_ON(end >= vm->size);

	vma = kzalloc(sizeof(*vma), GFP_KERNEL);
	if (!vma) {
		vma = ERR_PTR(-ENOMEM);
		return vma;
	}

	vma->vm = vm;
	vma->start = start;
	vma->end = end;
	if (read_only)
		vma->pte_flags = PTE_READ_ONLY;

	if (bo) {
		xe_bo_assert_held(bo);
		vma->bo_offset = bo_offset_or_userptr;
		vma->bo = xe_bo_get(bo);
		list_add_tail(&vma->bo_link, &bo->vmas);
	} else /* userptr */ {
		u64 size = end - start + 1;
		int err;

		vma->userptr.ptr = bo_offset_or_userptr;
		INIT_LIST_HEAD(&vma->userptr_link);

		vma->userptr.dma_address =
			kmalloc(sizeof(*vma->userptr.dma_address) *
				(size >> PAGE_SHIFT), GFP_KERNEL);
		if (!vma->userptr.dma_address) {
			kfree(vma);
			vma = ERR_PTR(-ENOMEM);
			return vma;
		}

		err = mmu_interval_notifier_insert(&vma->userptr.notifier,
						   current->mm,
						   vma->userptr.ptr, size,
						   &vma_userptr_notifier_ops);
		if (err) {
			kfree(vma->userptr.dma_address);
			kfree(vma);
			vma = ERR_PTR(err);
			return vma;
		}

		INIT_WORK(&vma->userptr.rebind_work, vma_rebind_work_func);
		vma->userptr.notifier_seq = LONG_MAX;
		xe_vm_get(vm);
	}

	return vma;
}

static void xe_vma_destroy(struct xe_vma *vma)
{

	if (vma_is_userptr(vma)) {
		/*
		 * Needs to be done outside VM lock as flushing
		 * userptr.rebind_work requires VM lock
		 */
		vma->userptr.destroyed = true;
		INIT_WORK(&vma->userptr.destroy_work, vma_destroy_work_func);
		queue_work(system_unbound_wq, &vma->userptr.destroy_work);
	} else {
		list_del(&vma->bo_link);
		xe_bo_put(vma->bo);
		kfree(vma);
	}
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

static void async_op_work_func(struct work_struct *w);

struct xe_vm *xe_vm_create(struct xe_device *xe, uint32_t flags)
{
	struct xe_vm *vm;
	struct xe_vma *vma;
	int err, i = 0;
	struct xe_engine *eng;

	vm = kzalloc(sizeof(*vm), GFP_KERNEL);
	if (!vm)
		return ERR_PTR(-ENOMEM);

	vm->xe = xe;
	kref_init(&vm->refcount);
	dma_resv_init(&vm->resv);

	vm->size = 1ull << 48;

	vm->vmas = RB_ROOT;

	INIT_LIST_HEAD(&vm->userptr.list);
	mutex_init(&vm->userptr.list_lock);
	rwlock_init(&vm->userptr.notifier_lock);

	INIT_LIST_HEAD(&vm->async_ops.pending);
	INIT_WORK(&vm->async_ops.work, async_op_work_func);
	spin_lock_init(&vm->async_ops.lock);

	xe_vm_lock(vm, NULL);

	if (IS_DGFX(xe) && xe->info.vram_flags & XE_VRAM_FLAGS_NEED64K)
		vm->flags |= VM_FLAGS_64K;

	vm->pt_root = xe_pt_create(vm, 3);
	if (IS_ERR(vm->pt_root)) {
		err = PTR_ERR(vm->pt_root);
		goto err_unlock;
	}

	if (flags & DRM_XE_VM_CREATE_SCRATCH_PAGE) {
		vm->scratch_bo = xe_bo_create(xe, vm, SZ_4K,
					      ttm_bo_type_kernel,
					      XE_BO_CREATE_VRAM_IF_DGFX(xe) |
					      XE_BO_CREATE_IGNORE_MIN_PAGE_SIZE_BIT);
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
				xe_pt_destroy(vm->scratch_pt[i], vm->flags);
				goto err_scratch_pt;
			}
		}
	}

	if (flags & DRM_XE_VM_CREATE_COMPUTE_MODE)
		vm->flags |= VM_FLAG_COMPUTE_MODE;

	if (flags & DRM_XE_VM_CREATE_ASYNC_BIND_OPS)
		vm->flags |= VM_FLAG_ASYNC_BIND_OPS;

	/* Fill pt_root after allocating scratch tables */
	err = xe_pt_populate_empty(vm, vm->pt_root);
	if (err)
		goto err_scratch_pt;

	eng = xe_engine_create_class(xe, NULL, XE_ENGINE_CLASS_COPY, ENGINE_FLAG_VM);
	if (IS_ERR(eng)) {
		err = PTR_ERR(eng);
		goto err_scratch_pt;
	}
	vm->eng = eng;

	xe_vm_unlock(vm);
	return vm;

err_scratch_pt:
	while (i)
		xe_pt_destroy(vm->scratch_pt[--i], vm->flags);
	xe_bo_put(vm->scratch_bo);
err_destroy_root:
	xe_pt_destroy(vm->pt_root, vm->flags);
err_unlock:
	xe_vm_unlock(vm);
	kfree(vma);
	dma_resv_fini(&vm->resv);
	kfree(vm);
	return ERR_PTR(err);
}

static void flush_async_ops(struct xe_vm *vm)
{
	vm->async_ops.flush = true;
	queue_work(system_unbound_wq, &vm->async_ops.work);
	flush_work(&vm->async_ops.work);
}

void xe_vm_close_and_put(struct xe_vm *vm)
{
	struct rb_root contested = RB_ROOT;

	vm->size = 0;
	smp_mb();
	flush_async_ops(vm);

	if (vm->eng) {
		xe_engine_kill(vm->eng);
		xe_engine_put(vm->eng);
		vm->eng = NULL;
	}

	xe_vm_lock(vm, NULL);
	while (vm->vmas.rb_node) {
		struct xe_vma *vma = to_xe_vma(vm->vmas.rb_node);

		rb_erase(&vma->vm_node, &vm->vmas);

		/* easy case, remove from VMA? */
		if (vma_is_userptr(vma) || vma->bo->vm) {
			xe_vma_destroy(vma);
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
			xe_pt_destroy(vm->scratch_pt[i], vm->flags);
	}
	xe_pt_destroy(vm->pt_root, vm->flags);
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
			struct xe_bo *bo = vma->bo;

			rb_erase(&vma->vm_node, &contested);

			xe_bo_lock_no_vm(bo, NULL);
			xe_vma_destroy(vma);
			xe_bo_unlock_no_vm(bo);
		}
	}

	xe_vm_put(vm);
}

void xe_vm_free(struct kref *ref)
{
	struct xe_vm *vm = container_of(ref, struct xe_vm, refcount);

	/* xe_vm_close_and_put was not called? */
	XE_WARN_ON(vm->pt_root);

	dma_fence_put(vm->userptr.fence);
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
					xe_pt_destroy(pt_dir->entries[i],
						      vma->vm->flags);

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
	entry->target_vma = vma;
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
	trace_xe_vma_unbind(vma);

	XE_WARN_ON(vma->evicted && evict);

	xe_pt_prepare_unbind(vma, entries, &num_entries, evict);
	XE_BUG_ON(num_entries > ARRAY_SIZE(entries));

	/*
	 * Even if we were already evicted and unbind to destroy, we need to
	 * clear again here. The eviction may have updated pagetables at a
	 * lower level, because it needs to be more conservative.
	 */
	fence = xe_migrate_update_pgtables(gt->migrate,
					   xe_vm_has_preempt_fences(vm) ?
					   vm : NULL, evict ? NULL : vm->eng,
					   entries, num_entries,
					   syncs, num_syncs,
					   xe_migrate_clear_pgtable_callback, vma);
	if (!IS_ERR(fence)) {
		if (!evict) {
			/* add shared fence now for pagetable delayed destroy */
			dma_resv_add_shared_fence(&vm->resv, fence);

			/* This fence will be installed by caller when doing eviction */
			if (!vma_is_userptr(vma) && !vma->bo->vm)
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
			ptr[i] = gen8_pte_encode(update->target_vma,
						 update->target_vma->bo,
						 bo_offset,
						 XE_CACHE_WB,
						 update->target_vma->pte_flags);
	}
}

static void xe_pt_abort_bind(struct xe_vma *vma, struct xe_vm_pgtable_update *entries, u32 num_entries)
{
	u32 i, j;

	for (i = 0; i < num_entries; i++) {
		if (!entries[i].pt_entries)
			continue;

		for (j = 0; j < entries[i].qwords; j++)
			xe_pt_destroy(entries[i].pt_entries[j], vma->vm->flags);
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
				xe_pt_destroy(pt_dir->entries[j_], vma->vm->flags);

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
				xe_pt_destroy(entry, vma->vm->flags);
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
			xe_pt_destroy(pte[i], vma->vm->flags);

		kfree(pte);
		return err;
	}

done:
	entry = &entries[(*num_entries)++];
	entry->pt_bo = pt->bo;
	entry->ofs = start_ofs;
	entry->qwords = my_added_pte;
	entry->pt = pt;
	entry->target_vma = vma;
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
	trace_xe_vma_bind(vma);

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
					   xe_vm_has_preempt_fences(vm) ?
					   vm : NULL, vm->eng,
					   entries, num_entries,
					   syncs, num_syncs,
					   xe_vm_populate_pgtable, vma);
	if (!IS_ERR(fence)) {
		/* add shared fence now for pagetable delayed destroy */
		dma_resv_add_shared_fence(&vm->resv, fence);

		if (!vma_is_userptr(vma) && !vma->bo->vm)
			dma_resv_add_shared_fence(vma->bo->ttm.base.resv, fence);
		xe_pt_commit_bind(vma, entries, num_entries);

		/* This vma is live (again?) now */
		vma->evicted = false;
		vma->userptr.dirty = false;
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

	XE_BUG_ON(!xe_vm_has_preempt_fences(vm));

	xe_vm_lock(vm, NULL);
	if (!--vm->preempt.num_inflight_ops &&
	    !xe_vm_userptr_pending_rebind_read(vm))
		reinstall_preempt_fences(vm);
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

struct async_op_fence {
	struct dma_fence fence;
	struct dma_fence_cb cb;
	struct xe_vm *vm;
};

static void async_op_fence_cb(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct async_op_fence *afence =
		container_of(cb, struct async_op_fence, cb);

	dma_fence_signal(&afence->fence);
	xe_vm_put(afence->vm);
	dma_fence_put(&afence->fence);
}

static void add_async_op_fence_cb(struct xe_vm *vm,
				  struct dma_fence *fence,
				  struct async_op_fence *afence)
{
	int ret;

	afence->vm = xe_vm_get(vm);
	dma_fence_get(&afence->fence);
	ret = dma_fence_add_callback(fence, &afence->cb, async_op_fence_cb);
	if (ret == -ENOENT)
		dma_fence_signal(&afence->fence);
	if (ret) {
		xe_vm_put(vm);
		dma_fence_put(&afence->fence);
	}
	XE_WARN_ON(ret && ret != -ENOENT);
}

static int __xe_vm_bind(struct xe_vm *vm, struct xe_vma *vma,
			struct xe_sync_entry *syncs, u32 num_syncs,
			struct async_op_fence *afence)
{
	struct dma_fence *fence;
	struct preempt_op *op = NULL;
	int err;

	xe_vm_assert_held(vm);

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
	if (xe_vm_has_preempt_fences(vm)) {
		op = kmalloc(sizeof(*op), GFP_KERNEL);
		if (!op)
			return -ENOMEM;
	}

	fence = xe_vm_bind_vma(vma, syncs, num_syncs);
	if (IS_ERR(fence)) {
		err = PTR_ERR(fence);
		goto err_free_op;
	}
	if (afence)
		add_async_op_fence_cb(vm, fence, afence);
	if (xe_vm_has_preempt_fences(vm))
		add_preempt_op_cb(vm, fence, op);

	dma_fence_put(fence);
	return 0;

err_free_op:
	kfree(op);
	return err;
}

static int xe_vm_bind(struct xe_vm *vm, struct xe_vma *vma, struct xe_bo *bo,
		      struct xe_sync_entry *syncs, u32 num_syncs,
		      struct async_op_fence *afence)
{
	xe_vm_assert_held(vm);
	xe_bo_assert_held(bo);

	return __xe_vm_bind(vm, vma, syncs, num_syncs, afence);
}

static int xe_vm_bind_userptr(struct xe_vm *vm, struct xe_vma *vma,
			      struct xe_sync_entry *syncs, u32 num_syncs,
			      struct async_op_fence *afence)
{
	int err;

	xe_vm_lock(vm, NULL);
	err = dma_resv_reserve_shared(&vm->resv, 1);
	if (!err)
		err = __xe_vm_bind(vm, vma, syncs, num_syncs, afence);
	xe_vm_unlock(vm);
	if (err)
		return err;

	/* Once initial bind done, make userptr available to execs */
	mutex_lock(&vm->userptr.list_lock);
	list_add_tail(&vma->userptr_link, &vm->userptr.list);
	mutex_unlock(&vm->userptr.list_lock);

	/*
	 * Corner case where initial bind no longer valid, kick preempt fences
	 * to fix page tables
	 */
	vma->userptr.initial_bind = true;
	if (xe_vm_has_preempt_fences(vm) &&
	    vma_userptr_needs_repin(vma) == -EAGAIN)
		dma_resv_wait_timeout(&vm->resv, true, false,
				      MAX_SCHEDULE_TIMEOUT);

	return 0;
}

static int xe_vm_unbind(struct xe_vm *vm, struct xe_vma *vma,
			struct xe_bo *bo, struct xe_sync_entry *syncs,
			u32 num_syncs, struct async_op_fence *afence)
{
	struct dma_fence *fence;
	struct preempt_op *op = NULL;

	xe_vm_assert_held(vm);
	xe_bo_assert_held(bo);

	if (xe_vm_has_preempt_fences(vm)) {
		op = kmalloc(sizeof(*op), GFP_KERNEL);
		if (!op)
			return -ENOMEM;
	}

	fence = xe_vm_unbind_vma(vma, syncs, num_syncs, false);
	if (IS_ERR(fence)) {
		kfree(op);
		return PTR_ERR(fence);
	}
	if (afence)
		add_async_op_fence_cb(vm, fence, afence);
	if (xe_vm_has_preempt_fences(vm))
		add_preempt_op_cb(vm, fence, op);

	xe_vma_destroy(vma);
	dma_fence_put(fence);
	return 0;
}

static int vm_set_error_capture_address(struct xe_device *xe, struct xe_vm *vm,
					u64 value)
{
	return 0;
}

typedef int (*xe_vm_set_property_fn)(struct xe_device *xe, struct xe_vm *vm,
				     u64 value);

static const xe_vm_set_property_fn vm_set_property_funcs[] = {
	[XE_VM_PROPERTY_BIND_OP_ERROR_CAPTURE_ADDRESS] =
		vm_set_error_capture_address,
};

static int vm_user_ext_set_property(struct xe_device *xe, struct xe_vm *vm,
				    u64 extension)
{
	uint64_t __user *address = u64_to_user_ptr(extension);
	struct drm_xe_ext_vm_set_property ext;
	int err;

	err = __copy_from_user(&ext, address, sizeof(ext));
	if (XE_IOCTL_ERR(xe, err))
		return -EFAULT;

	if (XE_IOCTL_ERR(xe, ext.property >=
			 ARRAY_SIZE(vm_set_property_funcs)))
		return -EINVAL;

	return vm_set_property_funcs[ext.property](xe, vm, ext.value);
}

typedef int (*xe_vm_user_extension_fn)(struct xe_device *xe, struct xe_vm *vm,
				       u64 extension);

static const xe_vm_set_property_fn vm_user_extension_funcs[] = {
	[XE_VM_EXTENSION_SET_PROPERTY] = vm_user_ext_set_property,
};

#define MAX_USER_EXTENSIONS	16
static int vm_user_extensions(struct xe_device *xe, struct xe_vm *vm,
			      u64 extensions, int ext_number)
{
	uint64_t __user *address = u64_to_user_ptr(extensions);
	struct xe_user_extension ext;
	int err;

	if (XE_IOCTL_ERR(xe, ext_number >= MAX_USER_EXTENSIONS))
		return -E2BIG;

	err = __copy_from_user(&ext, address, sizeof(ext));
	if (XE_IOCTL_ERR(xe, err))
		return -EFAULT;

	if (XE_IOCTL_ERR(xe, ext.name >=
			 ARRAY_SIZE(vm_user_extension_funcs)))
		return -EINVAL;

	err = vm_user_extension_funcs[ext.name](xe, vm, extensions);
	if (XE_IOCTL_ERR(xe, err))
		return err;

	if (ext.next_extension)
		return vm_user_extensions(xe, vm, ext.next_extension,
					  ++ext_number);

	return 0;
}

#define ALL_DRM_XE_VM_CREATE_FLAGS (DRM_XE_VM_CREATE_SCRATCH_PAGE | \
				    DRM_XE_VM_CREATE_COMPUTE_MODE | \
				    DRM_XE_VM_CREATE_ASYNC_BIND_OPS)

int xe_vm_create_ioctl(struct drm_device *dev, void *data,
		       struct drm_file *file)
{
	struct xe_device *xe = to_xe_device(dev);
	struct xe_file *xef = to_xe_file(file);
	struct drm_xe_vm_create *args = data;
	struct xe_vm *vm;
	u32 id;
	int err;

	if (XE_IOCTL_ERR(xe, args->flags & ~ALL_DRM_XE_VM_CREATE_FLAGS))
		return -EINVAL;

	vm = xe_vm_create(xe, args->flags);
	if (IS_ERR(vm))
		return PTR_ERR(vm);

	if (args->extensions) {
		err = vm_user_extensions(xe, vm, args->extensions, 0);
		if (XE_IOCTL_ERR(xe, err)) {
			xe_vm_close_and_put(vm);
			return err;
		}
	}

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

#define VM_BIND_OP(op)	(op & 0xffff)

static int __vm_bind_ioctl(struct xe_vm *vm, struct xe_vma *vma,
			   struct xe_bo *bo, u64 bo_offset,
			   u64 range, u64 addr, u32 op,
			   struct xe_sync_entry *syncs, u32 num_syncs,
			   struct async_op_fence *afence)
{
	switch (VM_BIND_OP(op)) {
	case XE_VM_BIND_OP_MAP:
		return xe_vm_bind(vm, vma, bo, syncs, num_syncs, afence);
	case XE_VM_BIND_OP_UNMAP:
		return xe_vm_unbind(vm, vma, bo, syncs, num_syncs, afence);
	case XE_VM_BIND_OP_MAP_USERPTR:
		return xe_vm_bind_userptr(vm, vma, syncs, num_syncs, afence);
	default:
		XE_BUG_ON("NOT POSSIBLE");
		return -EINVAL;
	}
}

static void xe_vm_tv_populate(struct xe_vm *vm, struct ttm_validate_buffer *tv)
{
	tv->num_shared = 1;
	tv->bo = &vm->pt_root->bo->ttm;
}

static int vm_bind_ioctl(struct xe_vm *vm, struct xe_vma *vma, struct xe_bo *bo,
			 struct drm_xe_vm_bind *args,
			 struct xe_sync_entry *syncs, u32 num_syncs,
			 struct async_op_fence *fence)
{
	int err;

	if (!(VM_BIND_OP(args->op) == XE_VM_BIND_OP_MAP_USERPTR)) {
		LIST_HEAD(objs);
		LIST_HEAD(dups);
		struct ttm_validate_buffer tv_bo, tv_vm;
		struct ww_acquire_ctx ww;

		xe_vm_tv_populate(vm, &tv_vm);
		list_add_tail(&tv_vm.head, &objs);

		if (bo) {
			tv_bo.bo = &bo->ttm;
			tv_bo.num_shared = 1;
			list_add(&tv_bo.head, &objs);
		}

		err = ttm_eu_reserve_buffers(&ww, &objs, true, &dups);
		if (!err) {
			err = __vm_bind_ioctl(vm, vma, bo, args->obj_offset,
					      args->range, args->addr, args->op,
					      syncs, num_syncs, fence);
			ttm_eu_backoff_reservation(&ww, &objs);
		}
	} else {
		err = __vm_bind_ioctl(vm, vma, NULL, args->userptr,
				      args->range, args->addr, args->op,
				      syncs, num_syncs, fence);
	}

	return err;
}

struct async_op {
	struct xe_vma *vma;
	struct xe_bo *bo;
	struct drm_xe_vm_bind args;
	struct xe_sync_entry *syncs;
	u32 num_syncs;
	struct list_head link;
	struct async_op_fence *fence;
};

static void async_op_work_func(struct work_struct *w)
{
	struct xe_vm *vm = container_of(w, struct xe_vm, async_ops.work);

	for (;;) {
		struct async_op *op;
		int err;

		if (vm->async_ops.pause && !vm->async_ops.flush)
			break;

		spin_lock_irq(&vm->async_ops.lock);
		op = list_first_entry_or_null(&vm->async_ops.pending,
					      struct async_op, link);
		if (op)
			list_del_init(&op->link);
		spin_unlock_irq(&vm->async_ops.lock);

		if (!op)
			break;

		if (!vm->async_ops.flush) {
			err = vm_bind_ioctl(vm, op->vma, op->bo, &op->args,
					    op->syncs, op->num_syncs,
					    op->fence);
			if (err) {
				drm_warn(&vm->xe->drm, "Async VM op(%d) failed with %d",
					 VM_BIND_OP(op->args.op), err);

				spin_lock_irq(&vm->async_ops.lock);
				list_add(&op->link, &vm->async_ops.pending);
				spin_unlock_irq(&vm->async_ops.lock);

				/* TODO: Notify user of VM bind error */

				vm->async_ops.pause = true;
				break;
			}
		} else {
			if (VM_BIND_OP(op->args.op) == XE_VM_BIND_OP_UNMAP) {
				if (op->bo && op->bo->vm != vm)
					dma_resv_lock(op->bo->ttm.base.resv,
						      NULL);
				xe_vm_lock(vm, NULL);
				xe_vma_destroy(op->vma);
				xe_vm_unlock(vm);
				if (op->bo && op->bo->vm != vm)
					dma_resv_unlock(op->bo->ttm.base.resv);
			}

			if (!test_bit(DMA_FENCE_FLAG_SIGNALED_BIT,
				      &op->fence->fence.flags))
				dma_fence_signal(&op->fence->fence);
		}

		while (op->num_syncs--)
			xe_sync_entry_cleanup(&op->syncs[op->num_syncs]);
		kfree(op->syncs);
		if (op->bo)
			drm_gem_object_put(&op->bo->ttm.base);
		xe_vm_put(vm);
		dma_fence_put(&op->fence->fence);
		kfree(op);
	}
}

static const char *async_op_fence_get_driver_name(struct dma_fence *dma_fence)
{
	return "xe";
}

static const char *
async_op_fence_get_timeline_name(struct dma_fence *dma_fence)
{
	return "async_op_fence";
}

static const struct dma_fence_ops async_op_fence_ops = {
	.get_driver_name = async_op_fence_get_driver_name,
	.get_timeline_name = async_op_fence_get_timeline_name,
};

static int vm_bind_ioctl_async(struct xe_vm *vm, struct xe_vma *vma,
			       struct xe_bo *bo, struct drm_xe_vm_bind *args,
			       struct xe_sync_entry *syncs, u32 num_syncs)
{
	struct async_op *op;
	bool installed = false;
	int i;

	op = kmalloc(sizeof(*op), GFP_KERNEL);
	if (!op)
		return -ENOMEM;

	op->fence = kmalloc(sizeof(*op->fence), GFP_KERNEL);
	if (!op->fence) {
		kfree(op->fence);
		return -ENOMEM;
	}

	dma_fence_init(&op->fence->fence, &async_op_fence_ops,
		       &vm->async_ops.lock, 0, 0);
	op->vma = vma;
	op->bo = bo;
	op->args = *args;
	op->syncs = syncs;
	op->num_syncs = num_syncs;
	INIT_LIST_HEAD(&op->link);

	for (i = 0; i < num_syncs; i++)
		installed |= xe_sync_entry_signal(&syncs[i], NULL,
						  &op->fence->fence);

	if (!installed)
		dma_fence_signal(&op->fence->fence);

	spin_lock_irq(&vm->async_ops.lock);
	list_add_tail(&op->link, &vm->async_ops.pending);
	spin_unlock_irq(&vm->async_ops.lock);

	if (!vm->async_ops.pause)
		queue_work(system_unbound_wq, &vm->async_ops.work);

	return 0;
}

struct xe_vma *vm_bind_ioctl_lookup_vma(struct xe_vm *vm, struct xe_bo *bo,
					u64 bo_offset_or_userptr, u64 addr,
					u64 range, u32 op)
{
	struct xe_device *xe = vm->xe;
	struct xe_vma *vma, lookup;
	int err;

	lookup.start = addr;
	lookup.end = addr + range - 1;

	if (bo) {
		if (bo->vm != vm)
			dma_resv_lock(bo->ttm.base.resv, NULL);
		xe_vm_lock(vm, NULL);
	}
	switch (VM_BIND_OP(op)) {
	case XE_VM_BIND_OP_MAP:
		vma = xe_vm_find_overlapping_vma(vm, &lookup);
		if (XE_IOCTL_ERR(xe, vma)) {
			vma = ERR_PTR(-EBUSY);
			goto out_unlock;
		}

		err = xe_bo_populate(bo);
		if (err) {
			vma = ERR_PTR(err);
			goto out_unlock;
		}

		vma = xe_vma_create(vm, bo, bo_offset_or_userptr, addr,
				    addr + range - 1,
				    op & XE_VM_BIND_FLAG_READONLY);
		if (!vma) {
			vma = ERR_PTR(-ENOMEM);
			goto out_unlock;
		}

		xe_vm_insert_vma(vm, vma);
		break;
	case XE_VM_BIND_OP_UNMAP:
		if (!bo)
			xe_vm_lock(vm, NULL);

		vma = xe_vm_find_overlapping_vma(vm, &lookup);

		if (XE_IOCTL_ERR(xe, !vma) ||
		    XE_IOCTL_ERR(xe, vma->bo != bo) ||
		    XE_IOCTL_ERR(xe, vma->start != addr) ||
		    XE_IOCTL_ERR(xe, vma->end != addr + range - 1)) {
			vma = ERR_PTR(-EINVAL);
			if (!bo)
				xe_vm_unlock(vm);
			goto out_unlock;
		}

		xe_vm_remove_vma(vm, vma);
		if (!bo)
			xe_vm_unlock(vm);
		break;
	case XE_VM_BIND_OP_MAP_USERPTR:
		XE_BUG_ON(bo);

		vma = xe_vma_create(vm, NULL, bo_offset_or_userptr, addr,
				    addr + range - 1,
				    op & XE_VM_BIND_FLAG_READONLY);
		if (!vma)
			return ERR_PTR(-ENOMEM);

		err = vma_userptr_pin_pages(vma);
		xe_vm_lock(vm, NULL);
		if (err || xe_vm_find_overlapping_vma(vm, &lookup)) {
			xe_vma_destroy(vma);
			vma = err ? ERR_PTR(err) : ERR_PTR(-EBUSY);
		} else {
			xe_vm_insert_vma(vm, vma);
		}
		xe_vm_unlock(vm);
		break;
	default:
		XE_BUG_ON("NOT POSSIBLE");
		vma = ERR_PTR(-EINVAL);
	}

out_unlock:
	if (bo) {
		xe_vm_unlock(vm);
		if (bo->vm != vm)
			dma_resv_unlock(bo->ttm.base.resv);
	}

	return vma;
}

#define SUPPORTED_FLAGS	\
	(XE_VM_BIND_FLAG_ASYNC | XE_VM_BIND_FLAG_READONLY | 0xffff)

int xe_vm_bind_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct xe_device *xe = to_xe_device(dev);
	struct xe_file *xef = to_xe_file(file);
	struct drm_xe_vm_bind *args = data;
	struct drm_xe_sync __user *syncs_user;
	struct drm_gem_object *gem_obj = NULL;
	struct xe_bo *bo = NULL;
	struct xe_vm *vm;
	struct xe_vma *vma;
	int err = 0;
	u32 num_syncs;
	struct xe_sync_entry *syncs;
	u64 range = args->range;
	u64 addr = args->addr;
	u32 op = args->op;
	bool async = (op & XE_VM_BIND_FLAG_ASYNC);

	if (XE_IOCTL_ERR(xe, args->extensions) ||
	    XE_IOCTL_ERR(xe, VM_BIND_OP(op) >
			 XE_VM_BIND_OP_RESTART) ||
	    XE_IOCTL_ERR(xe, op & ~SUPPORTED_FLAGS) ||
	    XE_IOCTL_ERR(xe, !args->obj &&
			 VM_BIND_OP(op) == XE_VM_BIND_OP_MAP) ||
	    XE_IOCTL_ERR(xe, args->obj &&
			 VM_BIND_OP(op) == XE_VM_BIND_OP_MAP_USERPTR))
		return -EINVAL;

	if (XE_IOCTL_ERR(xe, args->obj_offset & ~PAGE_MASK) ||
	    XE_IOCTL_ERR(xe, addr & ~PAGE_MASK) ||
	    XE_IOCTL_ERR(xe, range & ~PAGE_MASK) ||
	    XE_IOCTL_ERR(xe, !range && VM_BIND_OP(op) != XE_VM_BIND_OP_RESTART))
		return -EINVAL;

	vm = xe_vm_lookup(xef, args->vm_id);
	if (XE_IOCTL_ERR(xe, !vm))
		return -ENOENT;

	if (XE_IOCTL_ERR(xe, xe_vm_is_closed(vm))) {
		DRM_ERROR("VM closed while we began looking up?\n");
		err = -ENOENT;
		goto put_vm;
	}

	if (VM_BIND_OP(op) == XE_VM_BIND_OP_RESTART) {
		if (XE_IOCTL_ERR(xe, !(vm->flags & VM_FLAG_ASYNC_BIND_OPS)))
			return -ENOTSUPP;
		if (XE_IOCTL_ERR(xe, args->num_syncs))
			err = EINVAL;
		if (XE_IOCTL_ERR(xe, !vm->async_ops.pause))
			err = -EPROTO;
		if (!err) {
			vm->async_ops.pause = false;
			queue_work(system_unbound_wq, &vm->async_ops.work);
		}
		xe_vm_put(vm);
		return err;
	}

	if (XE_IOCTL_ERR(xe, !vm->async_ops.pause &&
			 async != !!(vm->flags & VM_FLAG_ASYNC_BIND_OPS)))
		return -ENOTSUPP;

	if (XE_IOCTL_ERR(xe, !range) ||
	    XE_IOCTL_ERR(xe, range > vm->size) ||
	    XE_IOCTL_ERR(xe, addr > vm->size - range)) {
		err = -EINVAL;
		goto put_vm;
	}

	if (args->obj) {
		gem_obj = drm_gem_object_lookup(file, args->obj);
		if (XE_IOCTL_ERR(xe, !gem_obj)) {
			err = -ENOENT;
			goto put_vm;
		}
		bo = gem_to_xe_bo(gem_obj);

		if (XE_IOCTL_ERR(xe, range > bo->size) ||
		    XE_IOCTL_ERR(xe, args->obj_offset > bo->size - range)) {
			err = -EINVAL;
			goto put_obj;
		}
	}

	syncs = kcalloc(args->num_syncs, sizeof(*syncs), GFP_KERNEL);
	if (!syncs) {
		err = -ENOMEM;
		goto put_obj;
	}

	syncs_user = u64_to_user_ptr(args->syncs);
	for (num_syncs = 0; num_syncs < args->num_syncs; num_syncs++) {
		err = xe_sync_entry_parse(xe, xef, &syncs[num_syncs],
					  &syncs_user[num_syncs], false, false);
		if (err)
			goto free_syncs;
	}

	vma = vm_bind_ioctl_lookup_vma(vm, bo, args->obj_offset, addr, range,
				       op);
	if (IS_ERR(vma)) {
		err = PTR_ERR(vma);
		goto free_syncs;
	}

	if (async) {
		err = vm_bind_ioctl_async(vm, vma, bo, args, syncs, num_syncs);
		if (!err)
			return 0;
	} else {
		err = vm_bind_ioctl(vm, vma, bo, args, syncs, num_syncs, NULL);
	}

	if (err) {
		switch (VM_BIND_OP(op)) {
		case XE_VM_BIND_OP_MAP:
		case XE_VM_BIND_OP_MAP_USERPTR:
			if (bo && bo->vm != vm)
				dma_resv_lock(bo->ttm.base.resv, NULL);
			xe_vm_lock(vm, NULL);
			xe_vm_remove_vma(vm, vma);
			xe_vma_destroy(vma);
			xe_vm_unlock(vm);
			if (bo && bo->vm != vm)
				dma_resv_unlock(bo->ttm.base.resv);
		}
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

static char *xe_dump_prefix_lvl[5] = { "     ", "    ", "   ", "  ", " "};

static void dump_pgtt_lvl(struct xe_vm *vm, struct xe_pt *pt, int lvl, int tag64k)
{
	struct xe_pt_dir *pt_dir;
	int i;
	int err;
	struct ttm_bo_kmap_obj map;

	if (lvl == 0) {
		err = __xe_pt_kmap(pt, &map);
		if (!err) {
			char *mode = "M4k";
			int numpt = GEN8_PDES;

			if (tag64k) {
				numpt = 32;
				mode = "M64k";
			}
			for (i = 0; i < numpt; i++) {
				uint64_t v = __xe_pt_read(&map, i);

				if (v) {
					drm_info(&vm->xe->drm, "L%d %s index %d <0x%llx> %s\n",
						lvl, xe_dump_prefix_lvl[lvl], i, v, mode);
				}
			}
			ttm_bo_kunmap(&map);
		}
		return;
	}
	pt_dir = as_xe_pt_dir(pt);
	err = __xe_pt_kmap(pt, &map);
	if (!err) {
		for (i = 0; i < GEN8_PDES; i++) {
			if (pt_dir->entries[i]) {
				uint64_t v = 0;
				int is_64k = 0;

				v = __xe_pt_read(&map, i);
				is_64k = v & GEN12_PDE_64K;
				drm_info(&vm->xe->drm, "L%d %s index %d exist <0x%llx> %s\n",
					lvl, xe_dump_prefix_lvl[lvl], i, v, (is_64k)?"64k":"");
				dump_pgtt_lvl(vm, pt_dir->entries[i], lvl-1, is_64k);
			}
		}
		ttm_bo_kunmap(&map);
	}
}

void xe_vm_dump_pgtt(struct xe_vm *vm)
{
	struct xe_pt *pt =  vm->pt_root;
	uint64_t desc = xe_vm_pdp4_descriptor(vm);

	drm_info(&vm->xe->drm, "dump_pgtt desc=0x%llx bo(%p)\n", desc, vm->pt_root->bo);
	dump_pgtt_lvl(vm, pt, 3, 0);
}
