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
#include "xe_preempt_fence.h"
#include "xe_res_cursor.h"
#include "xe_sync.h"
#include "xe_trace.h"

#define TEST_VM_ASYNC_OPS_ERROR


#if IS_ENABLED(CONFIG_DRM_XE_DEBUG_VM)
#define vm_dbg drm_dbg
#else
__printf(2, 3)
static inline void vm_dbg(const struct drm_device *dev,
			  const char *format, ...)
{ /* noop */ }

#endif

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

u64 gen8_pde_encode(struct xe_bo *bo, u64 bo_offset,
		    const enum xe_cache_level level)
{
	u64 pde;
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

u64 gen8_pte_encode(struct xe_vma *vma, struct xe_bo *bo,
		    u64 offset, enum xe_cache_level cache,
		    u32 flags, u32 pt_level)
{
	u64 pte;
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

	switch (cache) {
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

	if (pt_level == 1)
		pte |= GEN8_PDE_PS_2M;
	else if (pt_level == 2)
		pte |= GEN8_PDPE_PS_1G;

	/* XXX: Does hw support 1 GiB pages? */
	XE_BUG_ON(pt_level > 2);

	return pte;
}

static uint64_t __xe_vm_empty_pte(struct xe_vm *vm, unsigned int level)
{
	if (!vm->scratch_bo)
		return 0;

	if (level == 0)
		return gen8_pte_encode(NULL, vm->scratch_bo, 0, XE_CACHE_WB, 0, level);
	else
		return gen8_pde_encode(vm->scratch_pt[level - 1]->bo, 0,
				       XE_CACHE_WB);
}

static int __xe_pt_kmap(struct xe_pt *pt, struct ttm_bo_kmap_obj *map)
{
	XE_BUG_ON(pt->bo->size % PAGE_SIZE);
	return ttm_bo_kmap(&pt->bo->ttm, 0, pt->bo->size / PAGE_SIZE, map);
}

void __xe_pt_write(struct ttm_bo_kmap_obj *map, unsigned int idx, uint64_t data)
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

	size = level ? sizeof(struct xe_pt_dir) : sizeof(struct xe_pt);
	pt = kzalloc(size, GFP_KERNEL);
	if (!pt)
		return ERR_PTR(-ENOMEM);

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

	xe_bo_pin(bo);
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

	if (vm->flags & XE_VM_FLAGS_64K && pt->level == 1) {
		numpte = 32;
		if (vm->scratch_bo)
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

static bool xe_pte_hugepage_possible(struct xe_vma *vma, u32 level, u64 start, u64 end)
{
	u64 pagesize = 1ull << xe_pt_shift(level);
	u64 bo_ofs = vma->bo_offset + (start - vma->start);
	struct xe_res_cursor cur;

	XE_BUG_ON(!level);
	XE_BUG_ON(end - start > pagesize);

	if (level > 2)
		return false;

	if (start + pagesize != end)
		return false;

	if (vma->bo->ttm.resource->mem_type != TTM_PL_VRAM)
		return false;

	xe_res_first(vma->bo->ttm.resource, bo_ofs, pagesize, &cur);
	if (cur.size < pagesize)
		return false;

	if (cur.start & (pagesize - 1))
		return false;

	return true;
}

static int xe_pt_populate_for_vma(struct xe_vma *vma, struct xe_pt *pt,
				  u64 start, u64 end, bool rebind)
{
	u32 start_ofs = xe_pt_idx(start, pt->level);
	u32 last_ofs = xe_pt_idx(end - 1, pt->level);
	struct ttm_bo_kmap_obj map;
	struct xe_vm *vm = vma->vm;
	struct xe_pt_dir *pt_dir = NULL;
	bool init = !pt->num_live;
	u32 i;
	int err;
	u32 page_size = 1 << xe_pt_shift(pt->level);
	u32 numpdes = GEN8_PDES;
	u32 flags = 0;
	u64 bo_offset = vma->bo_offset + (start - vma->start);

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

	vm_dbg(&vm->xe->drm, "\t\t%u: %d..%d F:0x%x\n", pt->level,
	       start_ofs, last_ofs, flags);

	if (pt->level) {
		u64 cur = start;

		pt_dir = as_xe_pt_dir(pt);

		for (i = start_ofs; i <= last_ofs; i++) {
			u64 next_start = xe_pt_next_start(cur, pt->level);
			struct xe_pt *pte = NULL;
			u64 cur_end = min(next_start, end);

			XE_WARN_ON(pt_dir->entries[i]);

			if (!xe_pte_hugepage_possible(vma, pt->level, cur,
						      cur_end))
				pte = xe_pt_create(vm, pt->level - 1);

			if (IS_ERR(pte))
				return PTR_ERR(pte);

			if (pte) {
				err = xe_pt_populate_for_vma(vma, pte,
							     cur, cur_end,
							     rebind);
				if (err)
					return err;
			}

			pt_dir->entries[i] = pte;
			if (!rebind)
				pt_dir->pt.num_live++;

			cur = next_start;
		}
	} else {
		/* newly added entries only, evict didn't decrease num_live */
		if (!rebind)
			pt->num_live += last_ofs + 1 - start_ofs;
	}

	/* any pte entries now exist, fill in now */
	err = __xe_pt_kmap(pt, &map);
	if (err)
		return err;

	if (init) {
		u32 init_flags = (vma->vm->scratch_bo) ? flags : 0;
		u64 empty = __xe_vm_empty_pte(vma->vm, pt->level) | init_flags;

		for (i = 0; i < start_ofs; i++)
			__xe_pt_write(&map, i, empty);
		for (i = last_ofs + 1; i < numpdes; i++)
			__xe_pt_write(&map, i, empty);
	}

	for (i = start_ofs; i <= last_ofs; i++, bo_offset += page_size) {
		u64 entry;

		if (pt_dir && pt_dir->entries[i])
			entry = gen8_pde_encode(pt_dir->entries[i]->bo,
						0, XE_CACHE_WB) | flags;
		else
			entry = gen8_pte_encode(vma, vma->bo, bo_offset,
						XE_CACHE_WB, vma->pte_flags, pt->level);

		__xe_pt_write(&map, i, entry);
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

	if (pt->level == 0 && flags & XE_VM_FLAGS_64K)
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
	if (vma->destroyed)
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
		trace_xe_vma_userptr_pin_set_dirty(vma);
		if (vma_userptr_needs_repin(vma) == -EAGAIN)
			goto retry;
	}

	return ret < 0 ? ret : 0;
}

static int alloc_preempt_fences(struct xe_vm *vm)
{
	struct xe_engine *e;
	bool wait = false;
	long timeout;

	lockdep_assert_held(&vm->lock);
	xe_vm_assert_held(vm);

	/*
	 * We test for a corner case where the rebind worker is queue'd twice
	 * in a row but the first run of the worker fixes all the page tables.
	 * If any of pfences are NULL or is signaling is enabled on pfence we
	 * know that their are page tables which need fixing.
	 */
	list_for_each_entry(e, &vm->preempt.engines, compute.link) {
		if (!e->compute.pfence || (e->compute.pfence &&
		    test_bit(DMA_FENCE_FLAG_ENABLE_SIGNAL_BIT,
			     &e->compute.pfence->flags))) {
			wait = true;
			break;
		}
	}

	if (!wait)
		return 1;	/* nothing to do */

	list_for_each_entry(e, &vm->preempt.engines, compute.link) {
		struct dma_fence *pfence;

		if (e->compute.pfence) {
			timeout = dma_fence_wait(e->compute.pfence, false);
			if (timeout < 0)
				return -ETIME;
			dma_fence_put(e->compute.pfence);
			e->compute.pfence = NULL;
		}

		pfence = xe_preempt_fence_create(e, e->compute.context,
						 ++e->compute.seqno);
		XE_WARN_ON(!pfence);
		if (!pfence)
			return -ENOMEM;

		e->compute.pfence = pfence;
	}

	return 0;
}

static int add_preempt_fences(struct xe_vm *vm, struct xe_bo *bo)
{
	struct xe_engine *e;
	struct ww_acquire_ctx ww;
	int err;

	err = xe_bo_lock(bo, &ww, vm->preempt.num_engines, true);
	if (err)
		return err;

	list_for_each_entry(e, &vm->preempt.engines, compute.link)
		if (e->compute.pfence) {
			dma_resv_add_fence(bo->ttm.base.resv,
					   e->compute.pfence,
					   DMA_RESV_USAGE_PREEMPT_FENCE);
		}

	xe_bo_unlock(bo, &ww);
	return 0;
}

static void reinstall_preempt_fences(struct xe_vm *vm)
{
	struct xe_engine *e;
	int i;

	lockdep_assert_held(&vm->lock);
	xe_vm_assert_held(vm);

	list_for_each_entry(e, &vm->preempt.engines, compute.link) {
		e->ops->resume(e);

		dma_resv_add_fence(&vm->resv, e->compute.pfence,
				   DMA_RESV_USAGE_PREEMPT_FENCE);

		for (i = 0; i < vm->extobj.entries; ++i) {
			struct xe_bo *bo = vm->extobj.bos[i];

			dma_resv_add_fence(bo->ttm.base.resv, e->compute.pfence,
					   DMA_RESV_USAGE_PREEMPT_FENCE);
		}
	}
}

int xe_vm_add_compute_engine(struct xe_vm *vm, struct xe_engine *e)
{
	struct ttm_validate_buffer tv_vm;
	struct ttm_validate_buffer *tv_bos = NULL;
	struct ww_acquire_ctx ww;
	struct list_head objs, dups;
	struct dma_fence *pfence;
	int i;
	int err;

	XE_BUG_ON(!xe_vm_in_compute_mode(vm));

	down_read(&vm->lock);

	tv_bos = kmalloc(sizeof(*tv_bos) * vm->extobj.entries,
			 GFP_KERNEL);
	if (!tv_bos) {
		err = -ENOMEM;
		goto out_unlock;
	}

	INIT_LIST_HEAD(&objs);
	INIT_LIST_HEAD(&dups);

	for (i = 0; i < vm->extobj.entries; ++i) {
		struct xe_bo *bo = vm->extobj.bos[i];

		tv_bos[i].num_shared = 1;
		tv_bos[i].bo = &bo->ttm;

		list_add_tail(&tv_bos[i].head, &objs);
	}
	tv_vm.num_shared = 1;
	tv_vm.bo = xe_vm_ttm_bo(vm);;
	list_add_tail(&tv_vm.head, &objs);

	err = ttm_eu_reserve_buffers(&ww, &objs, true, &dups);
	if (err)
		goto out_unlock_outer;

	pfence = xe_preempt_fence_create(e, e->compute.context,
					 ++e->compute.seqno);
	if (!pfence) {
		err = -ENOMEM;
		goto out_unlock;
	}

	list_add(&e->compute.link, &vm->preempt.engines);
	++vm->preempt.num_engines;
	e->compute.pfence = pfence;

	dma_resv_add_fence(&vm->resv, pfence,
			   DMA_RESV_USAGE_PREEMPT_FENCE);

	for (i = 0; i < vm->extobj.entries; ++i) {
		struct xe_bo *bo = vm->extobj.bos[i];

		dma_resv_add_fence(bo->ttm.base.resv, pfence,
				   DMA_RESV_USAGE_PREEMPT_FENCE);
	}

out_unlock:
	ttm_eu_backoff_reservation(&ww, &objs);
out_unlock_outer:
	up_read(&vm->lock);
	kfree(tv_bos);

	return err;
}

static void preempt_rebind_work_func(struct work_struct *w)
{
	struct xe_vm *vm = container_of(w, struct xe_vm, preempt.rebind_work);
	struct xe_vma *vma;
	struct ttm_validate_buffer tv_vm;
	struct ttm_validate_buffer *tv_bos = NULL;
	struct ww_acquire_ctx ww;
	struct list_head objs, dups;
	struct dma_fence *rebind_fence;
	int i, err;
	long wait;

	XE_BUG_ON(!xe_vm_in_compute_mode(vm));
	trace_xe_vm_rebind_worker_enter(vm);

retry:
	if (xe_vm_is_closed(vm)) {
		trace_xe_vm_rebind_worker_exit(vm);
		return;
	}

	down_read(&vm->lock);

	err = xe_vm_userptr_pin(vm, true);
	if (err)
		goto out_unlock_outer;

	if (!tv_bos) {
		tv_bos = kmalloc(sizeof(*tv_bos) * vm->extobj.entries,
				 GFP_KERNEL);
		if (!tv_bos)
			goto out_unlock_outer;
	}

	INIT_LIST_HEAD(&objs);
	INIT_LIST_HEAD(&dups);

	for (i = 0; i < vm->extobj.entries; ++i) {
		struct xe_bo *bo = vm->extobj.bos[i];

		tv_bos[i].num_shared = vm->preempt.num_engines;
		tv_bos[i].bo = &bo->ttm;

		list_add_tail(&tv_bos[i].head, &objs);
	}
	tv_vm.num_shared = vm->preempt.num_engines;
	tv_vm.bo = xe_vm_ttm_bo(vm);;
	list_add_tail(&tv_vm.head, &objs);

	err = ttm_eu_reserve_buffers(&ww, &objs, false, &dups);
	if (err)
		goto out_unlock_outer;

	err = alloc_preempt_fences(vm);
	if (err)
		goto out_unlock;
	vm->preempt.resume_go = 0;

	list_for_each_entry(vma, &vm->evict_list, evict_link) {
		err = xe_bo_validate(vma->bo, vm);
		if (err)
			goto out_unlock;
	}

	rebind_fence = xe_vm_rebind(vm, true);
	if (IS_ERR(rebind_fence)) {
		err = PTR_ERR(rebind_fence);
		goto out_unlock;
	}

	if (rebind_fence) {
		dma_fence_wait(rebind_fence, false);
		dma_fence_put(rebind_fence);
	}

	reinstall_preempt_fences(vm);
	err = xe_vm_userptr_needs_repin(vm, true);

	vm->preempt.resume_go = err == -EAGAIN ? -1 : 1;
	smp_mb();
	wake_up_all(&vm->preempt.resume_wq);

out_unlock:
	ttm_eu_backoff_reservation(&ww, &objs);
out_unlock_outer:
	up_read(&vm->lock);

	if (err == -EAGAIN) {
		wait = dma_resv_wait_timeout(&vm->resv,
					     DMA_RESV_USAGE_PREEMPT_FENCE,
					     false, MAX_SCHEDULE_TIMEOUT);
		if (wait <= 0) {
			err = -ETIME;
			goto free;
		}
		trace_xe_vm_rebind_worker_retry(vm);
		goto retry;
	}

free:
	kfree(tv_bos);
	XE_WARN_ON(err < 0);	/* TODO: Kill VM or put in error state */
	trace_xe_vm_rebind_worker_exit(vm);
}

struct async_op_fence;
static int __xe_vm_bind(struct xe_vm *vm, struct xe_vma *vma,
			struct xe_engine *e, struct xe_sync_entry *syncs,
			u32 num_syncs, struct async_op_fence *afence,
			bool rebind);

static void vma_destroy_work_func(struct work_struct *w)
{
	struct xe_vma *vma =
		container_of(w, struct xe_vma, userptr.destroy_work);
	struct xe_vm *vm = vma->vm;

	XE_BUG_ON(!vma_is_userptr(vma));

	if (!list_empty(&vma->userptr_link)) {
		down_write(&vm->lock);
		list_del(&vma->bo_link);
		up_write(&vm->lock);
	}

	kfree(vma->userptr.dma_address);
	mmu_interval_notifier_remove(&vma->userptr.notifier);
	xe_vm_put(vm);
	kfree(vma);
}

static bool vma_userptr_invalidate(struct mmu_interval_notifier *mni,
				   const struct mmu_notifier_range *range,
				   unsigned long cur_seq)
{
	struct xe_vma *vma = container_of(mni, struct xe_vma, userptr.notifier);
	struct xe_vm *vm = vma->vm;
	struct dma_resv_iter cursor;
	struct dma_fence *fence;
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
	if (current->flags & PF_EXITING || vma->destroyed ||
	    !vma->userptr.initial_bind) {
		write_unlock(&vm->userptr.notifier_lock);
		return true;
	}

	write_unlock(&vm->userptr.notifier_lock);

	/* Preempt fences turn into schedule disables, pipeline these */
	dma_resv_iter_begin(&cursor, &vm->resv, DMA_RESV_USAGE_PREEMPT_FENCE);
	dma_resv_for_each_fence_unlocked(&cursor, fence)
		dma_fence_enable_sw_signaling(fence);
	dma_resv_iter_end(&cursor);

	err = dma_resv_wait_timeout(&vm->resv, DMA_RESV_USAGE_PREEMPT_FENCE,
				    false, MAX_SCHEDULE_TIMEOUT);
	XE_WARN_ON(err <= 0);

	trace_xe_vma_userptr_invalidate_complete(vma);

	/* If this VM in compute mode, rebind the VMA */
	if (xe_vm_in_compute_mode(vm))
		queue_work(to_gt(vm->xe)->ordered_wq, &vm->preempt.rebind_work);

	return true;
}

static const struct mmu_interval_notifier_ops vma_userptr_notifier_ops = {
	.invalidate = vma_userptr_invalidate,
};

int xe_vm_userptr_pin(struct xe_vm *vm, bool rebind_worker)
{
	struct xe_vma *vma;
	int err = 0;

	lockdep_assert_held(&vm->lock);
	if (!xe_vm_has_userptr(vm) ||
	    (xe_vm_in_compute_mode(vm) && !rebind_worker))
		return 0;

	list_for_each_entry(vma, &vm->userptr.list, userptr_link) {
		err = vma_userptr_pin_pages(vma);
		if (err < 0)
			return err;
	}

	return 0;
}

int xe_vm_userptr_needs_repin(struct xe_vm *vm, bool rebind_worker)
{
	struct xe_vma *vma;
	int err = 0;

	lockdep_assert_held(&vm->lock);
	if (!xe_vm_has_userptr(vm) ||
	    (xe_vm_in_compute_mode(vm) && !rebind_worker))
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

static struct dma_fence *
xe_vm_bind_vma(struct xe_vma *vma, struct xe_engine *e,
	       struct xe_sync_entry *syncs, u32 num_syncs,
	       bool rebind);

struct dma_fence *xe_vm_rebind(struct xe_vm *vm, bool rebind_worker)
{
	struct dma_fence *fence = NULL;
	struct xe_vma *vma, *next;

	lockdep_assert_held(&vm->lock);
	if (xe_vm_in_compute_mode(vm) && !rebind_worker)
		return NULL;

	xe_vm_assert_held(vm);
	list_for_each_entry(vma, &vm->userptr.list, userptr_link) {
		if (vma->userptr.dirty && vma->userptr.initial_bind) {
			dma_fence_put(fence);
			if (rebind_worker)
				trace_xe_vma_userptr_rebind_worker(vma);
			else
				trace_xe_vma_userptr_rebind_exec(vma);
			fence = xe_vm_bind_vma(vma, NULL, NULL, 0, true);
		}
		if (IS_ERR(fence))
			return fence;
	}

	list_for_each_entry_safe(vma, next, &vm->evict_list, evict_link) {
		list_del_init(&vma->evict_link);
		if (vma->userptr.initial_bind) {
			dma_fence_put(fence);
			if (rebind_worker)
				trace_xe_vma_rebind_worker(vma);
			else
				trace_xe_vma_rebind_exec(vma);
			fence = xe_vm_bind_vma(vma, NULL, NULL, 0, true);
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

	INIT_LIST_HEAD(&vma->evict_link);

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

		vma->userptr.notifier_seq = LONG_MAX;
		xe_vm_get(vm);
	}

	return vma;
}

static void xe_vma_destroy(struct xe_vma *vma)
{
	lockdep_assert_held(&vma->vm->lock);

	if (!list_empty(&vma->evict_link))
		list_del(&vma->evict_link);

	if (vma_is_userptr(vma)) {
		/* FIXME: Probably don't need a worker here anymore */
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
	lockdep_assert_held(&vm->lock);

	node = rb_find(vma, &vm->vmas, xe_vma_cmp_vma_cb);

	return node ? to_xe_vma(node) : NULL;
}

static void xe_vm_insert_vma(struct xe_vm *vm, struct xe_vma *vma)
{
	XE_BUG_ON(vma->vm != vm);
	lockdep_assert_held(&vm->lock);

	rb_add(&vma->vm_node, &vm->vmas, xe_vma_less_cb);
}

static void xe_vm_remove_vma(struct xe_vm *vm, struct xe_vma *vma)
{
	XE_BUG_ON(vma->vm != vm);
	lockdep_assert_held(&vm->lock);

	rb_erase(&vma->vm_node, &vm->vmas);
}

static void async_op_work_func(struct work_struct *w);

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

	vm->size = 1ull << xe_pt_shift(xe->info.vm_max_level + 1);

	vm->vmas = RB_ROOT;
	vm->flags = flags;

	init_rwsem(&vm->lock);

	INIT_LIST_HEAD(&vm->evict_list);

	INIT_LIST_HEAD(&vm->userptr.list);
	rwlock_init(&vm->userptr.notifier_lock);

	INIT_LIST_HEAD(&vm->async_ops.pending);
	INIT_WORK(&vm->async_ops.work, async_op_work_func);
	spin_lock_init(&vm->async_ops.lock);

	INIT_LIST_HEAD(&vm->preempt.engines);
	init_waitqueue_head(&vm->preempt.resume_wq);
	vm->preempt.min_run_period_ms = 10;	/* FIXME: Wire up to uAPI */

	err = dma_resv_lock_interruptible(&vm->resv, NULL);
	if (err)
		goto err_put;

	if (IS_DGFX(xe) && xe->info.vram_flags & XE_VRAM_FLAGS_NEED64K)
		vm->flags |= XE_VM_FLAGS_64K;

	vm->pt_root = xe_pt_create(vm, xe->info.vm_max_level);
	if (IS_ERR(vm->pt_root)) {
		err = PTR_ERR(vm->pt_root);
		goto err_unlock;
	}

	if (flags & XE_VM_FLAG_SCRATCH_PAGE) {
		vm->scratch_bo = xe_bo_create(xe, vm, SZ_4K,
					      ttm_bo_type_kernel,
					      XE_BO_CREATE_VRAM_IF_DGFX(xe) |
					      XE_BO_CREATE_IGNORE_MIN_PAGE_SIZE_BIT);
		if (IS_ERR(vm->scratch_bo)) {
			err = PTR_ERR(vm->scratch_bo);
			goto err_destroy_root;
		}
		xe_bo_pin(vm->scratch_bo);

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

	if (flags & DRM_XE_VM_CREATE_COMPUTE_MODE) {
		INIT_WORK(&vm->preempt.rebind_work, preempt_rebind_work_func);
		vm->flags |= XE_VM_FLAG_COMPUTE_MODE;
	}

	if (flags & DRM_XE_VM_CREATE_ASYNC_BIND_OPS) {
		vm->async_ops.fence.context = dma_fence_context_alloc(1);
		vm->flags |= XE_VM_FLAG_ASYNC_BIND_OPS;
	}

	/* Fill pt_root after allocating scratch tables */
	err = xe_pt_populate_empty(vm, vm->pt_root);
	if (err)
		goto err_scratch_pt;

	dma_resv_unlock(&vm->resv);

	/* Kernel migration VM shouldn't have a circular loop.. */
	if (!(flags & XE_VM_FLAG_MIGRATION)) {
		struct xe_vm *migrate_vm =
			xe_migrate_get_vm(to_gt(xe)->migrate);
		struct xe_engine *eng;

		eng = xe_engine_create_class(xe, migrate_vm,
					     XE_ENGINE_CLASS_COPY,
					     ENGINE_FLAG_VM);
		xe_vm_put(migrate_vm);
		if (IS_ERR(eng)) {
			xe_vm_close_and_put(vm);
			return ERR_CAST(eng);
		}
		vm->eng = eng;
	}

	trace_xe_vm_create(vm);

	return vm;

err_scratch_pt:
	while (i)
		xe_pt_destroy(vm->scratch_pt[--i], vm->flags);
	xe_bo_unpin(vm->scratch_bo);
	xe_bo_put(vm->scratch_bo);
err_destroy_root:
	xe_pt_destroy(vm->pt_root, vm->flags);
err_unlock:
	dma_resv_unlock(&vm->resv);
err_put:
	kfree(vma);
	dma_resv_fini(&vm->resv);
	kfree(vm);
	return ERR_PTR(err);
}

static void flush_async_ops(struct xe_vm *vm)
{
	queue_work(system_unbound_wq, &vm->async_ops.work);
	flush_work(&vm->async_ops.work);
}

static void vm_async_op_error_capture(struct xe_vm *vm, int err,
				      u32 op, u64 addr, u64 size)
{
	struct drm_xe_vm_bind_op_error_capture capture;
	uint64_t __user *address =
		u64_to_user_ptr(vm->async_ops.error_capture.addr);
	bool in_kthread = !current->mm;

	capture.error = err;
	capture.op = op;
	capture.addr = addr;
	capture.size = size;

	if (in_kthread)
		kthread_use_mm(vm->async_ops.error_capture.mm);

	if (copy_to_user(address, &capture, sizeof(capture)))
		XE_WARN_ON("Copy to user failed");

	if (in_kthread)
		kthread_unuse_mm(vm->async_ops.error_capture.mm);

	wake_up_all(&vm->async_ops.error_capture.wq);
}

void xe_vm_close_and_put(struct xe_vm *vm)
{
	struct rb_root contested = RB_ROOT;
	struct ww_acquire_ctx ww;

	XE_BUG_ON(vm->preempt.num_engines);

	vm->size = 0;
	smp_mb();
	flush_async_ops(vm);
	if (xe_vm_in_compute_mode(vm))
		flush_work(&vm->preempt.rebind_work);

	if (vm->eng) {
		xe_engine_kill(vm->eng);
		xe_engine_put(vm->eng);
		vm->eng = NULL;
	}

	down_write(&vm->lock);
	xe_vm_lock(vm, &ww, 0, false);
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

		xe_bo_unpin(vm->scratch_bo);
		xe_bo_put(vm->scratch_bo);
		for (i = 0; i < vm->pt_root->level; i++)
			xe_pt_destroy(vm->scratch_pt[i], vm->flags);
	}
	xe_vm_unlock(vm, &ww);

	if (contested.rb_node) {

		/*
		 * VM is now dead, cannot re-add nodes to vm->vmas if it's NULL
		 * Since we hold a refcount to the bo, we can remove and free
		 * the members safely without locking.
		 */
		while (contested.rb_node) {
			struct xe_vma *vma = to_xe_vma(contested.rb_node);

			rb_erase(&vma->vm_node, &contested);
			xe_vma_destroy(vma);
		}
	}

	if (vm->async_ops.error_capture.addr)
		vm_async_op_error_capture(vm, -ENODEV, XE_VM_BIND_OP_CLOSE,
					  0, 0);

	kfree(vm->extobj.bos);
	vm->extobj.bos = NULL;
	up_write(&vm->lock);

	xe_vm_put(vm);
}

void xe_vm_free(struct kref *ref)
{
	struct xe_vm *vm = container_of(ref, struct xe_vm, refcount);
	struct ww_acquire_ctx ww;

	/* xe_vm_close_and_put was not called? */
	XE_WARN_ON(vm->size);

	/*
	 * XXX: We delay destroying the PT root until the VM if freed as PT root
	 * is needed for xe_vm_lock to work. If we remove that dependency this
	 * can be moved to xe_vm_close_and_put.
	 */
	xe_vm_lock(vm, &ww, 0, false);
	xe_pt_destroy(vm->pt_root, vm->flags);
	vm->pt_root = NULL;
	xe_vm_unlock(vm, &ww);

	trace_xe_vm_free(vm);
	dma_fence_put(vm->rebind_fence);
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

		pt->num_live -= entry->qwords;
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
		       u32 *num_entries, struct xe_vm_pgtable_update *entries)
{
	u32 my_removed_pte = 0;
	struct xe_vm_pgtable_update *entry;
	u32 start_ofs, last_ofs;
	u32 num_live;

	if (!pt) {
		/* hugepage entry, skipped */
		(*removed_parent_pte)++;
		return;
	}

	start_ofs = xe_pt_idx(start, pt->level);
	last_ofs = xe_pt_idx(end - 1, pt->level);

	num_live = pt->num_live;

	if (!pt->level) {
		my_removed_pte = last_ofs - start_ofs + 1;
		if (vma->bo && vma->bo->flags & XE_BO_INTERNAL_64K) {
			start_ofs = start_ofs / 16;
			last_ofs = last_ofs / 16;
			my_removed_pte = last_ofs - start_ofs + 1;
		}
		vm_dbg(&vma->vm->xe->drm,
		       "\t%u: De-Populating entry [%u..%u +%u) [%llx...%llx)\n",
			pt->level, start_ofs, last_ofs, my_removed_pte, start, end);
		BUG_ON(!my_removed_pte);
	} else {
		struct xe_pt_dir *pt_dir = as_xe_pt_dir(pt);

		u64 start_end = min(xe_pt_next_start(start, pt->level), end);
		u64 end_start = max(start, xe_pt_prev_end(end, pt->level));
		u64 cur = start;
		bool partial_begin = false, partial_end = false;
		u32 my_rm_pte = last_ofs + 1 - start_ofs;
		u32 i;
		u32 first_ofs = start_ofs;

		if (pt_dir->entries[start_ofs])
			partial_begin = xe_pt_partial_entry(start, start_end, pt->level);

		if (pt_dir->entries[last_ofs] && last_ofs > start_ofs)
			partial_end = xe_pt_partial_entry(end_start, end, pt->level);
		vm_dbg(&vma->vm->xe->drm,
		       "\t%u: [%llx...%llx) partial begin/end: %u / %u, %u entries\n",
		       pt->level, start, end, partial_begin, partial_end, my_rm_pte);
		my_rm_pte -= partial_begin + partial_end;
		if (partial_begin) {
			u32 rem = 0;

			vm_dbg(&vma->vm->xe->drm,
			       "\t%u: Descending to first subentry %u level %u [%llx...%llx)\n",
			       pt->level, start_ofs,
			       pt->level - 1, start, start_end);
			__xe_pt_prepare_unbind(vma,
					       pt_dir->entries[start_ofs++],
					       &rem,
					       start, start_end,
					       num_entries, entries);
			start = cur = start_end;
			if (rem)
				my_removed_pte++;
		}
		for (i = 0; i < my_rm_pte; i++) {
			u32 rem = 0;
			u64 cur_end = min(xe_pt_next_start(cur, pt->level), end);

			vm_dbg(&vma->vm->xe->drm, "\t%llx...%llx / %llx",
			       xe_pt_next_start(cur, pt->level), end, cur_end);
			__xe_pt_prepare_unbind(vma, pt_dir->entries[start_ofs++],
					       &rem, cur, cur_end, num_entries,
					       entries);
			if (rem) {
				if (!my_removed_pte)
					first_ofs = start_ofs;
				my_removed_pte++;
			}
			cur = cur_end;
		}
		if (partial_end) {
			u32 rem = 0;

			XE_WARN_ON(cur >= end);
			XE_WARN_ON(cur != end_start);

			vm_dbg(&vma->vm->xe->drm,
			       "\t%u: Descending to last subentry %u level %u [%llx...%llx)\n",
			       pt->level, last_ofs, pt->level - 1, cur, end);

			__xe_pt_prepare_unbind(vma, pt_dir->entries[last_ofs],
					       &rem, cur, end, num_entries,
					       entries);
			if (rem) {
				if (!my_removed_pte)
					first_ofs = last_ofs;
				my_removed_pte++;
			}
		}

		/* No changes to this entry, fast return.. */
		if (!my_removed_pte)
			return;

		start_ofs = first_ofs;
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
	entry->flags = 0;

	vm_dbg(&vma->vm->xe->drm, "REMOVE %d L:%d o:%d q:%d t:0x%llx (%llx,%llx,%llx) f:0x%x\n",
	       (*num_entries)-1, pt->level, entry->ofs, entry->qwords, entry->target_offset,
	       vma->bo_offset, start, vma->start, entry->flags);
}

static void
xe_pt_prepare_unbind(struct xe_vma *vma,
		     struct xe_vm_pgtable_update *entries,
		     u32 *num_entries)
{
	*num_entries = 0;
	__xe_pt_prepare_unbind(vma, vma->vm->pt_root, NULL,
			       vma->start, vma->end + 1,
			       num_entries, entries);
	XE_BUG_ON(!*num_entries);
}

static struct dma_fence *
xe_vm_unbind_vma(struct xe_vma *vma, struct xe_engine *e,
		 struct xe_sync_entry *syncs, u32 num_syncs)
{
	struct xe_vm_pgtable_update entries[XE_VM_MAX_LEVEL * 2 + 1];
	struct xe_vm *vm = vma->vm;
	struct xe_gt *gt = to_gt(vm->xe);
	u32 num_entries;
	struct dma_fence *fence = NULL;
	u32 i;

	xe_bo_assert_held(vma->bo);
	xe_vm_assert_held(vm);
	trace_xe_vma_unbind(vma);

	xe_pt_prepare_unbind(vma, entries, &num_entries);
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


	/*
	 * Even if we were already evicted and unbind to destroy, we need to
	 * clear again here. The eviction may have updated pagetables at a
	 * lower level, because it needs to be more conservative.
	 */
	fence = xe_migrate_update_pgtables(gt->migrate,
					   vm, NULL, e ? e : vm->eng,
					   entries, num_entries,
					   syncs, num_syncs,
					   xe_migrate_clear_pgtable_callback,
					   vma);
	if (!IS_ERR(fence)) {
		/* add shared fence now for pagetable delayed destroy */
		dma_resv_add_fence(&vm->resv, fence,
				   DMA_RESV_USAGE_BOOKKEEP);

		/* This fence will be installed by caller when doing eviction */
		if (!vma_is_userptr(vma) && !vma->bo->vm)
			dma_resv_add_fence(vma->bo->ttm.base.resv, fence,
					   DMA_RESV_USAGE_BOOKKEEP);
		xe_pt_commit_unbind(vma, entries, num_entries);
	}

	return fence;
}

static void
xe_vm_populate_pgtable(void *data, u32 qword_ofs, u32 num_qwords,
		       struct xe_vm_pgtable_update *update, void *arg)
{
	u32 page_size = 1 << xe_pt_shift(update->pt->level);
	u64 bo_offset;
	struct xe_pt **ptes = update->pt_entries;
	u64 *ptr = data;
	u32 i;

	if (update->pt->level == 0 && update->flags & GEN12_PDE_64K)
		page_size = SZ_64K;
	bo_offset = update->target_offset +
		page_size * (qword_ofs - update->ofs);

	for (i = 0; i < num_qwords; i++, bo_offset += page_size) {
		if (ptes && ptes[i])
			ptr[i] = gen8_pde_encode(ptes[i]->bo, 0, XE_CACHE_WB) | update->flags;
		else
			ptr[i] = gen8_pte_encode(update->target_vma,
						 update->target_vma->bo,
						 bo_offset,
						 XE_CACHE_WB,
						 update->target_vma->pte_flags,
						 update->pt->level);
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

static void xe_pt_commit_bind(struct xe_vma *vma,
			      struct xe_vm_pgtable_update *entries,
			      u32 num_entries, bool rebind)
{
	u32 i, j;

	for (i = 0; i < num_entries; i++) {
		struct xe_pt *pt = entries[i].pt;
		struct xe_pt_dir *pt_dir;

		if (!rebind)
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
		     u32 *num_entries, struct xe_vm_pgtable_update *entries,
		     bool rebind)
{
	struct xe_device *xe = vma->vm->xe;
	u32 my_added_pte = 0;
	struct xe_vm_pgtable_update *entry;
	u32 start_ofs = xe_pt_idx(start, pt->level);
	u32 last_ofs = xe_pt_idx(end - 1, pt->level);
	struct xe_pt **pte = NULL;
	u32 flags = 0;

	XE_BUG_ON(start < vma->start);
	XE_BUG_ON(end > vma->end + 1);

	my_added_pte = last_ofs + 1 - start_ofs;
	BUG_ON(!my_added_pte);

	if (!pt->level) {
		if (vma->bo && vma->bo->flags & XE_BO_INTERNAL_64K) {
			start_ofs = start_ofs / 16;
			last_ofs = last_ofs / 16;
			my_added_pte = last_ofs + 1 - start_ofs;
		}
		vm_dbg(&xe->drm, "\t%u: Populating entry [%u..%u +%u) [%llx...%llx)\n",
		       pt->level, start_ofs, last_ofs, my_added_pte, start, end);
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
						   num_entries, entries,
						   rebind);
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

			if (vma->bo && vma->bo->flags & XE_BO_INTERNAL_64K && pt->level == 1)
				flags = GEN12_PDE_64K;

			vm_dbg(&xe->drm, "\t%u: Populating %u/%u subentry %u level %u [%llx...%llx) f: 0x%x\n",
			       pt->level, i + 1, my_added_pte,
			       start_ofs + i, pt->level - 1, cur, cur_end, flags);

			if (xe_pte_hugepage_possible(vma, pt->level, cur, cur_end)) {
				/* We will directly a PTE to object */
				entry = NULL;
			} else {
				entry = xe_pt_create(vma->vm, pt->level - 1);
				if (IS_ERR(entry)) {
					err = PTR_ERR(entry);
					goto unwind;
				}
			}
			pte[i] = entry;

			if (entry) {
				err = xe_pt_populate_for_vma(vma, entry, cur,
							     cur_end, rebind);
				if (err) {
					xe_pt_destroy(entry, vma->vm->flags);
					goto unwind;
				}
			}

			cur = cur_end;
		}

		/* last? */
		if (partial_end) {
			XE_WARN_ON(cur >= end);
			XE_WARN_ON(cur != end_start);

			vm_dbg(&xe->drm, "\t%u: Descending to last subentry %u level %u [%llx...%llx)\n",
			       pt->level, last_ofs, pt->level - 1, cur, end);

			err = __xe_pt_prepare_bind(vma,
						   pt_dir->entries[last_ofs],
						   cur, end, num_entries,
						   entries, rebind);
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
	entry->flags = flags;

	vm_dbg(&xe->drm, "ADD %d L:%d o:%d q:%d t:0x%llx (%llx,%llx,%llx) f:0x%x\n",
	       *num_entries-1, pt->level, entry->ofs, entry->qwords, entry->target_offset,
	       vma->bo_offset, start, vma->start, entry->flags);

	return 0;
}

static int
xe_pt_prepare_bind(struct xe_vma *vma,
		   struct xe_vm_pgtable_update *entries, u32 *num_entries,
		   bool rebind)
{
	int err;

	vm_dbg(&vma->vm->xe->drm, "Preparing bind, with range [%llx...%llx)\n",
	       vma->start, vma->end);

	*num_entries = 0;
	err = __xe_pt_prepare_bind(vma, vma->vm->pt_root, vma->start, vma->end +
				   1, num_entries, entries, rebind);
	if (!err)
		BUG_ON(!*num_entries);
	else /* abort! */
		xe_pt_abort_bind(vma, entries, *num_entries);

	return err;
}

static struct dma_fence *
xe_vm_bind_vma(struct xe_vma *vma, struct xe_engine *e,
	       struct xe_sync_entry *syncs, u32 num_syncs,
	       bool rebind)
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

	err = xe_pt_prepare_bind(vma, entries, &num_entries, rebind);
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
					   vm, vma->bo,
					   e ? e: vm->eng,
					   entries, num_entries,
					   syncs, num_syncs,
					   xe_vm_populate_pgtable, vma);
	if (!IS_ERR(fence)) {
		/* add shared fence now for pagetable delayed destroy */
		dma_resv_add_fence(&vm->resv, fence, DMA_RESV_USAGE_BOOKKEEP);

		if (!vma_is_userptr(vma) && !vma->bo->vm)
			dma_resv_add_fence(vma->bo->ttm.base.resv, fence,
					   DMA_RESV_USAGE_BOOKKEEP);
		xe_pt_commit_bind(vma, entries, num_entries, rebind);

		/* This vma is live (again?) now */
		vma->userptr.dirty = false;
		vma->userptr.initial_bind = true;

		/*
		 * FIXME: workaround for xe_evict.evict-mixed-many-threads-small
		 * failure, likely related to xe_exec_threads.threads-rebind
		 * failure. Details in issue #39
		 */
		if (rebind && !xe_vm_in_compute_mode(vm))
			dma_fence_wait(fence, false);
	} else {
		xe_pt_abort_bind(vma, entries, num_entries);
	}

	return fence;

err:
	return ERR_PTR(err);
}

struct async_op_fence {
	struct dma_fence fence;
	struct dma_fence_cb cb;
	struct xe_vm *vm;
	wait_queue_head_t wq;
	bool started;
};

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

	if (!xe_vm_in_compute_mode(vm)) {
		afence->started = true;
		smp_wmb();
		wake_up_all(&afence->wq);
	}

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

int xe_vm_async_fence_wait_start(struct dma_fence *fence)
{
	if (fence->ops == &async_op_fence_ops) {
		struct async_op_fence *afence =
			container_of(fence, struct async_op_fence, fence);

		XE_BUG_ON(xe_vm_in_compute_mode(afence->vm));

		smp_rmb();
		return wait_event_interruptible(afence->wq, afence->started);
	}

	return 0;
}

static int __xe_vm_bind(struct xe_vm *vm, struct xe_vma *vma,
			struct xe_engine *e, struct xe_sync_entry *syncs,
			u32 num_syncs, struct async_op_fence *afence,
			bool rebind)
{
	struct dma_fence *fence;

	xe_vm_assert_held(vm);

	fence = xe_vm_bind_vma(vma, e, syncs, num_syncs, rebind);
	if (IS_ERR(fence))
		return PTR_ERR(fence);
	if (afence)
		add_async_op_fence_cb(vm, fence, afence);

	dma_fence_put(fence);
	return 0;
}

static int xe_vm_bind(struct xe_vm *vm, struct xe_vma *vma, struct xe_engine *e,
		      struct xe_bo *bo, struct xe_sync_entry *syncs,
		      u32 num_syncs, struct async_op_fence *afence)
{
	int err;

	xe_vm_assert_held(vm);
	xe_bo_assert_held(bo);

	if (bo) {
		err = xe_bo_validate(bo, vm);
		if (err)
			return err;

		err = xe_bo_populate(bo);
		if (err)
			return err;
	}

	return __xe_vm_bind(vm, vma, e, syncs, num_syncs, afence, false);
}

static int xe_vm_bind_userptr(struct xe_vm *vm, struct xe_vma *vma,
			      struct xe_engine *e, struct xe_sync_entry *syncs,
			      u32 num_syncs, struct async_op_fence *afence)
{
	struct ww_acquire_ctx ww;
	int err;

	err = xe_vm_lock(vm, &ww, 1, true);
	if (!err) {
		err = __xe_vm_bind(vm, vma, e, syncs, num_syncs, afence, false);
		xe_vm_unlock(vm, &ww);
	}
	if (err)
		return err;

	/*
	 * Corner case where initial bind no longer valid, kick preempt fences
	 * to fix page tables
	 */
	if (xe_vm_in_compute_mode(vm) &&
	    vma_userptr_needs_repin(vma) == -EAGAIN) {
		struct dma_resv_iter cursor;
		struct dma_fence *fence;

		dma_resv_iter_begin(&cursor, &vm->resv,
				    DMA_RESV_USAGE_PREEMPT_FENCE);
		dma_resv_for_each_fence_unlocked(&cursor, fence)
			dma_fence_enable_sw_signaling(fence);
		dma_resv_iter_end(&cursor);
	}

	return 0;
}

static int xe_vm_unbind(struct xe_vm *vm, struct xe_vma *vma,
			struct xe_engine *e, struct xe_bo *bo,
			struct xe_sync_entry *syncs, u32 num_syncs,
			struct async_op_fence *afence)
{
	struct dma_fence *fence;

	xe_vm_assert_held(vm);
	xe_bo_assert_held(bo);

	fence = xe_vm_unbind_vma(vma, e, syncs, num_syncs);
	if (IS_ERR(fence))
		return PTR_ERR(fence);
	if (afence)
		add_async_op_fence_cb(vm, fence, afence);

	xe_vma_destroy(vma);
	dma_fence_put(fence);
	return 0;
}

static int vm_set_error_capture_address(struct xe_device *xe, struct xe_vm *vm,
					u64 value)
{
	if (XE_IOCTL_ERR(xe, !value))
		return -EINVAL;

	if (XE_IOCTL_ERR(xe, !(vm->flags & XE_VM_FLAG_ASYNC_BIND_OPS)))
		return -ENOTSUPP;

	if (XE_IOCTL_ERR(xe, vm->async_ops.error_capture.addr))
		return -ENOTSUPP;

	vm->async_ops.error_capture.mm = current->mm;
	vm->async_ops.error_capture.addr = value;
	init_waitqueue_head(&vm->async_ops.error_capture.wq);

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
	u32 flags = 0;

	if (XE_IOCTL_ERR(xe, args->flags & ~ALL_DRM_XE_VM_CREATE_FLAGS))
		return -EINVAL;

	if (args->flags & DRM_XE_VM_CREATE_SCRATCH_PAGE)
		flags |= XE_VM_FLAG_SCRATCH_PAGE;
	if (args->flags & DRM_XE_VM_CREATE_COMPUTE_MODE)
		flags |= XE_VM_FLAG_COMPUTE_MODE;
	if (args->flags & DRM_XE_VM_CREATE_ASYNC_BIND_OPS)
		flags |= XE_VM_FLAG_ASYNC_BIND_OPS;

	vm = xe_vm_create(xe, flags);
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

#if IS_ENABLED(CONFIG_DRM_XE_DEBUG_MEM)
	/* Warning: Security issue - never enable by default */
	args->reserved[0] = xe_bo_main_addr(vm->pt_root->bo, GEN8_PAGE_SIZE);
#endif

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

	vm = xe_vm_lookup(xef, args->vm_id);
	if (XE_IOCTL_ERR(xe, !vm))
		return -ENOENT;
	xe_vm_put(vm);

	/* FIXME: Extend this check to non-compute mode VMs */
	if (XE_IOCTL_ERR(xe, vm->preempt.num_engines))
		return -EBUSY;

	mutex_lock(&xef->vm.lock);
	xa_erase(&xef->vm.xa, args->vm_id);
	mutex_unlock(&xef->vm.lock);

	xe_vm_close_and_put(vm);

	return 0;
}

#define VM_BIND_OP(op)	(op & 0xffff)

static int __vm_bind_ioctl(struct xe_vm *vm, struct xe_vma *vma,
			   struct xe_engine *e, struct xe_bo *bo, u64 bo_offset,
			   u64 range, u64 addr, u32 op,
			   struct xe_sync_entry *syncs, u32 num_syncs,
			   struct async_op_fence *afence)
{
	switch (VM_BIND_OP(op)) {
	case XE_VM_BIND_OP_MAP:
		return xe_vm_bind(vm, vma, e, bo, syncs, num_syncs, afence);
	case XE_VM_BIND_OP_UNMAP:
		return xe_vm_unbind(vm, vma, e, bo, syncs, num_syncs, afence);
	case XE_VM_BIND_OP_MAP_USERPTR:
		return xe_vm_bind_userptr(vm, vma, e, syncs, num_syncs, afence);
	default:
		XE_BUG_ON("NOT POSSIBLE");
		return -EINVAL;
	}
}

struct ttm_buffer_object *xe_vm_ttm_bo(struct xe_vm *vm)
{
	return &vm->pt_root->bo->ttm;
}

static void xe_vm_tv_populate(struct xe_vm *vm, struct ttm_validate_buffer *tv)
{
	tv->num_shared = 1;
	tv->bo = xe_vm_ttm_bo(vm);
}

static int vm_bind_ioctl(struct xe_vm *vm, struct xe_vma *vma,
			 struct xe_engine *e, struct xe_bo *bo,
			 struct drm_xe_vm_bind_op *bind_op,
			 struct xe_sync_entry *syncs, u32 num_syncs,
			 struct async_op_fence *fence)
{
	int err;

	lockdep_assert_held(&vm->lock);

	/*
	 * FIXME: workaround for xe_exec_threads.threads-rebind failure, likely
	 * related to xe_evict.evict-mixed-many-threads-small failure. Details
	 * in issue #39
	 */
	if (VM_BIND_OP(bind_op->op) == XE_VM_BIND_OP_UNMAP) {
		int i;

		for (i = 0; i < num_syncs; i++) {
			err = xe_sync_entry_wait(&syncs[i]);
			if (err)
				return err;
		}
	}

	if (!(VM_BIND_OP(bind_op->op) == XE_VM_BIND_OP_MAP_USERPTR)) {
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
			err = __vm_bind_ioctl(vm, vma, e, bo,
					      bind_op->obj_offset,
					      bind_op->range, bind_op->addr,
					      bind_op->op, syncs, num_syncs,
					      fence);
			ttm_eu_backoff_reservation(&ww, &objs);
		}
	} else {
		err = __vm_bind_ioctl(vm, vma, e, NULL, bind_op->userptr,
				      bind_op->range, bind_op->addr,
				      bind_op->op, syncs, num_syncs, fence);
	}

	return err;
}

struct async_op {
	struct xe_vma *vma;
	struct xe_engine *engine;
	struct xe_bo *bo;
	struct drm_xe_vm_bind_op bind_op;
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

		if (vm->async_ops.pause && !xe_vm_is_closed(vm))
			break;

		spin_lock_irq(&vm->async_ops.lock);
		op = list_first_entry_or_null(&vm->async_ops.pending,
					      struct async_op, link);
		if (op)
			list_del_init(&op->link);
		spin_unlock_irq(&vm->async_ops.lock);

		if (!op)
			break;

		if (!xe_vm_is_closed(vm)) {
			down_write(&vm->lock);
#ifdef TEST_VM_ASYNC_OPS_ERROR
#define FORCE_ASYNC_OP_ERROR	BIT(31)
			if (!(op->bind_op.op & FORCE_ASYNC_OP_ERROR)) {
				err = vm_bind_ioctl(vm, op->vma, op->engine,
						    op->bo, &op->bind_op,
						    op->syncs, op->num_syncs,
						    op->fence);
			} else {
				err = -ENOMEM;
				op->bind_op.op &= ~FORCE_ASYNC_OP_ERROR;
			}
#else
			err = vm_bind_ioctl(vm, op->vma, op->engine, op->bo,
					    &op->bind_op, op->syncs,
					    op->num_syncs, op->fence);
#endif
			up_write(&vm->lock);
			if (err) {
				trace_xe_vma_fail(op->vma);
				drm_warn(&vm->xe->drm, "Async VM op(%d) failed with %d",
					 VM_BIND_OP(op->bind_op.op),
					 err);

				spin_lock_irq(&vm->async_ops.lock);
				list_add(&op->link, &vm->async_ops.pending);
				spin_unlock_irq(&vm->async_ops.lock);

				vm->async_ops.pause = true;
				smp_mb();

				if (vm->async_ops.error_capture.addr)
					vm_async_op_error_capture(vm, err,
								  op->bind_op.op,
								  op->bind_op.addr,
								  op->bind_op.range);
				break;
			}
		} else {
			trace_xe_vma_flush(op->vma);

			if (VM_BIND_OP(op->bind_op.op) == XE_VM_BIND_OP_UNMAP) {
				down_write(&vm->lock);
				xe_vma_destroy(op->vma);
				up_write(&vm->lock);
			}

			if (op->fence && !test_bit(DMA_FENCE_FLAG_SIGNALED_BIT,
						   &op->fence->fence.flags)) {
				if (!xe_vm_in_compute_mode(vm)) {
					op->fence->started = true;
					smp_wmb();
					wake_up_all(&op->fence->wq);
				}
				dma_fence_signal(&op->fence->fence);
			}
		}

		while (op->num_syncs--)
			xe_sync_entry_cleanup(&op->syncs[op->num_syncs]);
		kfree(op->syncs);
		if (op->bo)
			drm_gem_object_put(&op->bo->ttm.base);
		if (op->engine)
			xe_engine_put(op->engine);
		xe_vm_put(vm);
		if (op->fence)
			dma_fence_put(&op->fence->fence);
		kfree(op);
	}
}

static int vm_bind_ioctl_async(struct xe_vm *vm, struct xe_vma *vma,
			       struct xe_engine *e, struct xe_bo *bo,
			       struct drm_xe_vm_bind_op *bind_op,
			       struct xe_sync_entry *syncs, u32 num_syncs)
{
	struct async_op *op;
	bool installed = false;
	u64 seqno;
	int i;

	op = kmalloc(sizeof(*op), GFP_KERNEL);
	if (!op) {
		return -ENOMEM;
	}

	if (num_syncs) {
		op->fence = kmalloc(sizeof(*op->fence), GFP_KERNEL);
		if (!op->fence) {
			kfree(op);
			return -ENOMEM;
		}

		seqno = e ? ++e->bind.fence_seqno : ++vm->async_ops.fence.seqno;
		dma_fence_init(&op->fence->fence, &async_op_fence_ops,
			       &vm->async_ops.lock, e ? e->bind.fence_ctx :
			       vm->async_ops.fence.context, seqno);

		if (!xe_vm_in_compute_mode(vm)) {
			op->fence->vm = vm;
			op->fence->started = false;
			init_waitqueue_head(&op->fence->wq);
		}
	} else {
		op->fence = NULL;
	}
	op->vma = vma;
	op->engine = e;
	op->bo = bo;
	op->bind_op = *bind_op;
	op->syncs = syncs;
	op->num_syncs = num_syncs;
	INIT_LIST_HEAD(&op->link);

	for (i = 0; i < num_syncs; i++)
		installed |= xe_sync_entry_signal(&syncs[i], NULL,
						  &op->fence->fence);

	if (!installed && op->fence)
		dma_fence_signal(&op->fence->fence);

	spin_lock_irq(&vm->async_ops.lock);
	list_add_tail(&op->link, &vm->async_ops.pending);
	spin_unlock_irq(&vm->async_ops.lock);

	if (!vm->async_ops.pause)
		queue_work(system_unbound_wq, &vm->async_ops.work);

	return 0;
}

static bool bo_has_vm_references(struct xe_bo *bo, struct xe_vm *vm,
				 struct xe_vma *ignore)
{
	struct xe_vma *vma;

	list_for_each_entry(vma, &bo->vmas, bo_link) {
		if (vma != ignore && vma->vm == vm && !vma->destroyed)
			return true;
	}

	return false;
}

static int vm_insert_extobj(struct xe_vm *vm, struct xe_vma *vma)
{
	struct xe_bo **bos, *bo = vma->bo;

	lockdep_assert_held(&vm->lock);

	if (bo_has_vm_references(bo, vm, vma))
		return 0;

	bos = krealloc(vm->extobj.bos, (vm->extobj.entries + 1) * sizeof(*bos),
		       GFP_KERNEL);
	if (!bos)
		return -ENOMEM;

	vm->extobj.bos = bos;
	vm->extobj.bos[vm->extobj.entries++] = bo;
	return 0;
}

static void vm_remove_extobj(struct xe_vm *vm, struct xe_vma *vma)
{
	struct xe_bo *bo = vma->bo;
	int i;

	lockdep_assert_held(&vm->lock);

	if (bo_has_vm_references(bo, vm, vma))
		return;

	vm->extobj.entries--;
	for (i = 0; i < vm->extobj.entries; i++) {
		if (vm->extobj.bos[i] == bo) {
			swap(vm->extobj.bos[vm->extobj.entries],
			     vm->extobj.bos[i]);
			break;
		}
	}
}

struct xe_vma *vm_bind_ioctl_lookup_vma(struct xe_vm *vm, struct xe_bo *bo,
					u64 bo_offset_or_userptr, u64 addr,
					u64 range, u32 op)
{
	struct xe_device *xe = vm->xe;
	struct ww_acquire_ctx ww;
	struct xe_vma *vma, lookup;
	int err;

	lockdep_assert_held(&vm->lock);

	lookup.start = addr;
	lookup.end = addr + range - 1;

	switch (VM_BIND_OP(op)) {
	case XE_VM_BIND_OP_MAP:
		XE_BUG_ON(!bo);

		vma = xe_vm_find_overlapping_vma(vm, &lookup);
		if (XE_IOCTL_ERR(xe, vma))
			return ERR_PTR(-EBUSY);


		err = xe_bo_lock(bo, &ww, 0, true);
		if (err)
			return ERR_PTR(err);
		vma = xe_vma_create(vm, bo, bo_offset_or_userptr, addr,
				    addr + range - 1,
				    op & XE_VM_BIND_FLAG_READONLY);
		xe_bo_unlock(bo, &ww);
		if (!vma)
			return ERR_PTR(-ENOMEM);

		xe_vm_insert_vma(vm, vma);
		if (!bo->vm) {
			vm_insert_extobj(vm, vma);
			err = add_preempt_fences(vm, bo);
			if (err)
				return ERR_PTR(err);
		}
		break;
	case XE_VM_BIND_OP_UNMAP:
		vma = xe_vm_find_overlapping_vma(vm, &lookup);

		if (XE_IOCTL_ERR(xe, !vma) ||
		    XE_IOCTL_ERR(xe, vma->bo != bo) ||
		    XE_IOCTL_ERR(xe, vma->start != addr) ||
		    XE_IOCTL_ERR(xe, vma->end != addr + range - 1))
			return ERR_PTR(-EINVAL);

		vma->destroyed = true;
		xe_vm_remove_vma(vm, vma);
		if (bo && !bo->vm)
			vm_remove_extobj(vm, vma);
		break;
	case XE_VM_BIND_OP_MAP_USERPTR:
		XE_BUG_ON(bo);

		vma = xe_vma_create(vm, NULL, bo_offset_or_userptr, addr,
				    addr + range - 1,
				    op & XE_VM_BIND_FLAG_READONLY);
		if (!vma)
			return ERR_PTR(-ENOMEM);

		err = vma_userptr_pin_pages(vma);
		if (err || xe_vm_find_overlapping_vma(vm, &lookup)) {
			xe_vma_destroy(vma);
			vma = err ? ERR_PTR(err) : ERR_PTR(-EBUSY);
		} else {
			xe_vm_insert_vma(vm, vma);
			list_add_tail(&vma->userptr_link, &vm->userptr.list);
		}
		break;
	default:
		XE_BUG_ON("NOT POSSIBLE");
		vma = ERR_PTR(-EINVAL);
	}

	return vma;
}

#ifdef TEST_VM_ASYNC_OPS_ERROR
#define SUPPORTED_FLAGS	\
	(FORCE_ASYNC_OP_ERROR | XE_VM_BIND_FLAG_ASYNC | \
	 XE_VM_BIND_FLAG_READONLY | 0xffff)
#else
#define SUPPORTED_FLAGS	\
	(XE_VM_BIND_FLAG_ASYNC | XE_VM_BIND_FLAG_READONLY | 0xffff)
#endif
#define XE_64K_PAGE_MASK 0xffffull

#define MAX_BINDS	512	/* FIXME: Picking random upper limit */

int vm_bind_ioctl_check_args(struct xe_device *xe, struct drm_xe_vm_bind *args,
			     struct drm_xe_vm_bind_op **bind_ops, bool *async)
{
	int err;
	int i;

	if (XE_IOCTL_ERR(xe, args->extensions) ||
	    XE_IOCTL_ERR(xe, !args->num_binds) ||
	    XE_IOCTL_ERR(xe, args->num_binds > MAX_BINDS))
		return -EINVAL;

	if (args->num_binds > 1) {
		uint64_t __user *bind_user =
			u64_to_user_ptr(args->vector_of_binds);

		*bind_ops = kmalloc(sizeof(struct drm_xe_vm_bind_op) *
				    args->num_binds, GFP_KERNEL);
		if (!*bind_ops)
			return -ENOMEM;

		err = __copy_from_user(*bind_ops, bind_user,
				       sizeof(struct drm_xe_vm_bind_op) *
				       args->num_binds);
		if (XE_IOCTL_ERR(xe, err)) {
			err = -EFAULT;
			goto free_bind_ops;
		}
	} else {
		*bind_ops = &args->bind;
	}

	for (i = 0; i < args->num_binds; ++i) {
		u64 range = (*bind_ops)[i].range;
		u64 addr = (*bind_ops)[i].addr;
		u32 op = (*bind_ops)[i].op;
		u32 obj = (*bind_ops)[i].obj;
		u64 obj_offset = (*bind_ops)[i].obj_offset;

		if (i == 0) {
			*async = !!(op & XE_VM_BIND_FLAG_ASYNC);
		} else if (XE_IOCTL_ERR(xe, !*async) ||
			   XE_IOCTL_ERR(xe, !(op & XE_VM_BIND_FLAG_ASYNC)) ||
			   XE_IOCTL_ERR(xe, VM_BIND_OP(op) ==
					XE_VM_BIND_OP_RESTART)) {
			err = -EINVAL;
			goto free_bind_ops;
		}

		if (XE_IOCTL_ERR(xe, VM_BIND_OP(op) >
				 XE_VM_BIND_OP_RESTART) ||
		    XE_IOCTL_ERR(xe, op & ~SUPPORTED_FLAGS) ||
		    XE_IOCTL_ERR(xe, !obj &&
				 VM_BIND_OP(op) == XE_VM_BIND_OP_MAP) ||
		    XE_IOCTL_ERR(xe, obj &&
				 VM_BIND_OP(op) == XE_VM_BIND_OP_MAP_USERPTR)) {
			err = -EINVAL;
			goto free_bind_ops;
		}


		if (XE_IOCTL_ERR(xe, obj_offset & ~PAGE_MASK) ||
		    XE_IOCTL_ERR(xe, addr & ~PAGE_MASK) ||
		    XE_IOCTL_ERR(xe, range & ~PAGE_MASK) ||
		    XE_IOCTL_ERR(xe, !range && VM_BIND_OP(op) !=
				 XE_VM_BIND_OP_RESTART)) {
			err = -EINVAL;
			goto free_bind_ops;
		}
	}

	return 0;

free_bind_ops:
	if (args->num_binds > 1)
		kfree(*bind_ops);
	return err;
}

int xe_vm_bind_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct xe_device *xe = to_xe_device(dev);
	struct xe_file *xef = to_xe_file(file);
	struct drm_xe_vm_bind *args = data;
	struct drm_xe_sync __user *syncs_user;
	struct xe_bo **bos = NULL;
	struct xe_vma **vmas = NULL;
	struct xe_vm *vm;
	struct xe_engine *e = NULL;
	u32 num_syncs;
	struct xe_sync_entry *syncs = NULL;
	struct drm_xe_vm_bind_op *bind_ops;
	bool async;
	int err;
	int i, j = 0;

	err = vm_bind_ioctl_check_args(xe, args, &bind_ops, &async);
	if (err)
		return err;

	vm = xe_vm_lookup(xef, args->vm_id);
	if (XE_IOCTL_ERR(xe, !vm))
		goto free_objs;

	if (XE_IOCTL_ERR(xe, xe_vm_is_closed(vm))) {
		DRM_ERROR("VM closed while we began looking up?\n");
		err = -ENOENT;
		goto put_vm;
	}

	if (args->engine_id) {
		e = xe_engine_lookup(xef, args->engine_id);
		if (XE_IOCTL_ERR(xe, !e)) {
			err = -ENOENT;
			goto put_vm;
		}
		if (XE_IOCTL_ERR(xe, !(e->flags & ENGINE_FLAG_VM))) {
			err = -EINVAL;
			goto put_vm;
		}
	}

	if (VM_BIND_OP(bind_ops[0].op) == XE_VM_BIND_OP_RESTART) {
		if (XE_IOCTL_ERR(xe, !(vm->flags & XE_VM_FLAG_ASYNC_BIND_OPS)))
			err = -ENOTSUPP;
		if (XE_IOCTL_ERR(xe, !err && args->num_syncs))
			err = EINVAL;
		if (XE_IOCTL_ERR(xe, !err && !vm->async_ops.pause))
			err = -EPROTO;

		if (!err) {
			trace_xe_vm_restart(vm);
			vm->async_ops.pause = false;
			queue_work(system_unbound_wq, &vm->async_ops.work);
		}

		if (e)
			xe_engine_put(e);
		xe_vm_put(vm);
		return err;
	}

	if (XE_IOCTL_ERR(xe, !vm->async_ops.pause &&
			 async != !!(vm->flags & XE_VM_FLAG_ASYNC_BIND_OPS))) {
		err = -ENOTSUPP;
		goto put_engine;
	}

	for (i = 0; i < args->num_binds; ++i) {
		u64 range = bind_ops[i].range;
		u64 addr = bind_ops[i].addr;

		if (XE_IOCTL_ERR(xe, !range) ||
		    XE_IOCTL_ERR(xe, range > vm->size) ||
		    XE_IOCTL_ERR(xe, addr > vm->size - range)) {
			err = -EINVAL;
			goto put_engine;
		}
	}

	bos = kzalloc(sizeof(*bos) * args->num_binds, GFP_KERNEL);
	if (!bos) {
		err = -ENOMEM;
		goto put_engine;
	}

	vmas = kzalloc(sizeof(*vmas) * args->num_binds, GFP_KERNEL);
	if (!vmas) {
		err = -ENOMEM;
		goto put_engine;
	}

	for (i = 0; i < args->num_binds; ++i) {
		struct drm_gem_object *gem_obj;
		u64 range = bind_ops[i].range;
		u64 addr = bind_ops[i].addr;
		u32 obj = bind_ops[i].obj;
		u64 obj_offset = bind_ops[i].obj_offset;

		if (!obj)
			continue;

		gem_obj = drm_gem_object_lookup(file, obj);
		if (XE_IOCTL_ERR(xe, !gem_obj)) {
			err = -ENOENT;
			goto put_obj;
		}
		bos[i] = gem_to_xe_bo(gem_obj);

		if (XE_IOCTL_ERR(xe, range > bos[i]->size) ||
		    XE_IOCTL_ERR(xe, obj_offset >
				 bos[i]->size - range)) {
			err = -EINVAL;
			goto put_obj;
		}

		if (bos[i]->flags & XE_BO_INTERNAL_64K) {
			if (XE_IOCTL_ERR(xe, obj_offset &
					 XE_64K_PAGE_MASK) ||
			    XE_IOCTL_ERR(xe, addr & XE_64K_PAGE_MASK) ||
			    XE_IOCTL_ERR(xe, range & XE_64K_PAGE_MASK)) {
				err = -EINVAL;
				goto put_obj;
			}
		}
	}

	if (args->num_syncs) {
		syncs = kcalloc(args->num_syncs, sizeof(*syncs), GFP_KERNEL);
		if (!syncs) {
			err = -ENOMEM;
			goto put_obj;
		}
	}

	syncs_user = u64_to_user_ptr(args->syncs);
	for (num_syncs = 0; num_syncs < args->num_syncs; num_syncs++) {
		err = xe_sync_entry_parse(xe, xef, &syncs[num_syncs],
					  &syncs_user[num_syncs], false, false);
		if (err)
			goto free_syncs;
	}

	err = down_write_killable(&vm->lock);
	if (err)
		goto free_syncs;

	for (i = 0; i < args->num_binds; ++i) {
		u64 range = bind_ops[i].range;
		u64 addr = bind_ops[i].addr;
		u32 op = bind_ops[i].op;
		u64 obj_offset = bind_ops[i].obj_offset;

		vmas[i] = vm_bind_ioctl_lookup_vma(vm, bos[i], obj_offset,
						   addr, range, op);
		if (IS_ERR(vmas[i])) {
			err = PTR_ERR(vmas[i]);
			vmas[i] = NULL;
			goto destroy_vmas;
		}
	}

	for (j = 0; j < args->num_binds; ++j) {
		struct xe_sync_entry *__syncs;
		u32 __num_syncs = 0;
		bool first_or_last = j == 0 || j == args->num_binds - 1;

		if (args->num_binds == 1) {
			__num_syncs = num_syncs;
			__syncs = syncs;
		} else if (first_or_last) {
			bool first = j == 0;

			__syncs = kmalloc(sizeof(*__syncs) * num_syncs,
					  GFP_KERNEL);
			if (!__syncs) {
				err = ENOMEM;
				break;
			}

			/* in-syncs on first bind, out-syncs on last bind */
			for (i = 0; i < num_syncs; ++i) {
				bool signal = syncs[i].flags &
					DRM_XE_SYNC_SIGNAL;

				if ((first && !signal) || (!first && signal))
					__syncs[__num_syncs++] = syncs[i];
			}
		} else {
			__num_syncs = 0;
			__syncs = NULL;
		}

		if (async) {
			bool last = j == args->num_binds - 1;

			/*
			 * Each pass of async worker drops the ref, take a ref
			 * here, 1 set of refs taken above
			 */
			if (!last) {
				if (e)
					xe_engine_get(e);
				xe_vm_get(vm);
			}

			err = vm_bind_ioctl_async(vm, vmas[j], e, bos[j],
						  bind_ops + j, __syncs,
						  __num_syncs);
			if (err && !last) {
				if (e)
					xe_engine_put(e);
				xe_vm_put(vm);
			}
			if (err)
				break;
		} else {
			XE_BUG_ON(j != 0);	/* Not supported */
			err = vm_bind_ioctl(vm, vmas[j], e, bos[j],
					    bind_ops + j, __syncs,
					    __num_syncs, NULL);
			break;	/* Needed so cleanup loops work */
		}
	}

	/* Most of cleanup owned by the async bind worker */
	if (async && !err) {
		up_write(&vm->lock);
		if (args->num_binds > 1)
			kfree(syncs);
		goto free_objs;
	}

destroy_vmas:
	for (i = j; err && i < args->num_binds; ++i) {
		u32 op = bind_ops[i].op;

		if (!vmas[i])
			break;

		switch (VM_BIND_OP(op)) {
		case XE_VM_BIND_OP_MAP:
		case XE_VM_BIND_OP_MAP_USERPTR:
			xe_vma_destroy(vmas[i]);
		}
	}
	up_write(&vm->lock);
free_syncs:
	while (num_syncs--) {
		if (async && j &&
		    !(syncs[num_syncs].flags & DRM_XE_SYNC_SIGNAL))
			continue;	/* Still in async worker */
		xe_sync_entry_cleanup(&syncs[num_syncs]);
	}

	kfree(syncs);
put_obj:
	for (i = j; i < args->num_binds; ++i) {
		if (bos[i])
			drm_gem_object_put(&bos[i]->ttm.base);
	}
put_engine:
	if (e)
		xe_engine_put(e);
put_vm:
	xe_vm_put(vm);
free_objs:
	kfree(bos);
	kfree(vmas);
	if (args->num_binds > 1)
		kfree(bind_ops);
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
	dump_pgtt_lvl(vm, pt, vm->xe->info.vm_max_level, 0);
}

/*
 * XXX: Using the TTM wrappers for now, likely can call into dma-resv code
 * directly to optimize. Also this likely should be an inline function.
 */
int xe_vm_lock(struct xe_vm *vm, struct ww_acquire_ctx *ww,
	       int num_resv, bool intr)
{
	struct ttm_validate_buffer tv_vm;
	LIST_HEAD(objs);
	LIST_HEAD(dups);

	XE_BUG_ON(!ww);

	tv_vm.num_shared = num_resv;
	tv_vm.bo = xe_vm_ttm_bo(vm);;
	list_add_tail(&tv_vm.head, &objs);

	return ttm_eu_reserve_buffers(ww, &objs, intr, &dups);
}

void xe_vm_unlock(struct xe_vm *vm, struct ww_acquire_ctx *ww)
{
	dma_resv_unlock(&vm->resv);
	ww_acquire_fini(ww);
}
