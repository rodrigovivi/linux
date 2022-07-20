// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 */
#include "xe_migrate.h"

#include "xe_bb.h"
#include "xe_bo.h"
#include "xe_engine.h"
#include "xe_ggtt.h"
#include "xe_gt.h"
#include "xe_hw_engine.h"
#include "xe_lrc.h"
#include "xe_res_cursor.h"
#include "xe_sched_job.h"
#include "xe_sync.h"
#include "xe_vm.h"

#include <linux/sizes.h>
#include <drm/drm_managed.h>
#include <drm/ttm/ttm_tt.h>

#include "../i915/gt/intel_gpu_commands.h"

struct xe_migrate {
	struct xe_engine *eng;
	struct xe_gt *gt;
	struct mutex job_mutex;
	struct xe_bo *pt_bo;
	u64 batch_base_ofs;
	struct dma_fence *fence;

	struct drm_suballoc_manager vm_update_sa;
};

#define NUM_KERNEL_PDE 17
#define NUM_PT_SLOTS 48

static void xe_migrate_sanity_test(struct xe_migrate *m);

static void xe_migrate_fini(struct drm_device *dev, void *arg)
{
	struct xe_migrate *m = arg;
	struct ww_acquire_ctx ww;

	xe_vm_lock(m->eng->vm, &ww, 0, false);
	xe_bo_unpin(m->pt_bo);
	xe_vm_unlock(m->eng->vm, &ww);

	dma_fence_put(m->fence);
	xe_bo_put(m->pt_bo);
	drm_suballoc_manager_fini(&m->vm_update_sa);
	mutex_destroy(&m->job_mutex);
	xe_vm_close_and_put(m->eng->vm);
	xe_engine_put(m->eng);
}

static u32 xe_migrate_pagesize(struct xe_migrate *m)
{
	if (m->eng->vm->flags & XE_VM_FLAGS_64K)
		return SZ_64K;
	else
		return GEN8_PAGE_SIZE;
}

static u64 xe_pt_shift(unsigned int level)
{
	return GEN8_PTE_SHIFT + GEN8_PDE_SHIFT * level;
}

static u64 xe_migrate_vm_addr(u64 slot, u32 level)
{
	XE_BUG_ON(slot >= NUM_PT_SLOTS);

	/* First slot is reserved for mapping of PT bo and bb, start from 1 */
	return (slot + 1ULL) << xe_pt_shift(level + 1);
}

static u64 xe_migrate_vram_ofs(u64 addr)
{
	return addr + (256ULL << xe_pt_shift(2));
}

static int xe_migrate_prepare_vm(struct xe_migrate *m, struct xe_vm *vm)
{
	u32 num_entries = NUM_PT_SLOTS, num_level = vm->pt_root->level;
	struct ttm_bo_kmap_obj map;
	u32 map_ofs, level, i;
	struct xe_bo *bo, *batch = m->gt->kernel_bb_pool.bo;
	struct xe_device *xe = gt_to_xe(m->gt);
	u64 entry;
	int err;

	/* Can't bump NUM_PT_SLOTS too high */
	BUILD_BUG_ON(NUM_PT_SLOTS > SZ_2M/GEN8_PAGE_SIZE);
	/* Must be a multiple of 64K to support all platforms */
	BUILD_BUG_ON(NUM_PT_SLOTS * GEN8_PAGE_SIZE % SZ_64K);
	/* And one slot reserved for the 4KiB page table updates */
	BUILD_BUG_ON(!(NUM_KERNEL_PDE & 1));

	/* Need to be sure everything fits in the first PT, or create more */
	XE_BUG_ON(m->batch_base_ofs + batch->size >= SZ_2M);

	bo = xe_bo_create_pin_map(vm->xe, vm, num_entries * GEN8_PAGE_SIZE,
				  ttm_bo_type_kernel,
				  XE_BO_CREATE_VRAM_IF_DGFX(vm->xe) |
				  XE_BO_CREATE_PINNED_BIT);
	if (IS_ERR(bo))
		return PTR_ERR(bo);

	/* Write top-level entry first */
	err = ttm_bo_kmap(&vm->pt_root->bo->ttm, 0, vm->pt_root->bo->size / PAGE_SIZE, &map);
	if (err) {
		xe_bo_unpin(bo);
		xe_bo_put(bo);
		return err;
	}

	entry = gen8_pde_encode(bo, bo->size - GEN8_PAGE_SIZE, XE_CACHE_WB);
	__xe_pt_write(&map, 0, entry);

#if 0
	/* XXX: allow for 1 GiB pages? */
	for (i = 0; i < num_entries - num_level; i++) {
		entry = gen8_pde_encode(bo, i * GEN8_PAGE_SIZE, XE_CACHE_WB);

		__xe_pt_write(&map, i + 1, entry);
	}
#endif

	ttm_bo_kunmap(&map);

	map_ofs = (num_entries - num_level) * GEN8_PAGE_SIZE;

	/* Map the entire BO in our level 0 pt */
	for (i = 0, level = 0; i < num_entries; level++) {
		entry = gen8_pte_encode(NULL, bo, i * GEN8_PAGE_SIZE, XE_CACHE_WB, 0, 0);

		iosys_map_wr(&bo->vmap, map_ofs + level * 8, u64, entry);

		if (vm->flags & XE_VM_FLAGS_64K)
			i += 16;
		else
			i += 1;
	}

	if (!IS_DGFX(xe)) {
		/* Write out batch too */
		m->batch_base_ofs = NUM_PT_SLOTS * GEN8_PAGE_SIZE;
		for (i = 0; i < batch->size; i += vm->flags & XE_VM_FLAGS_64K ? SZ_64K : SZ_4K) {
			entry = gen8_pte_encode(NULL, batch, i, XE_CACHE_WB, 0, 0);

			iosys_map_wr(&bo->vmap, map_ofs + level * 8, u64, entry);
			level++;
		}
	} else {
		bool is_lmem;
		m->batch_base_ofs = xe_migrate_vram_ofs(xe_bo_addr(batch, 0, GEN8_PAGE_SIZE, &is_lmem));
	}

	for (level = 1; level < num_level; level++) {
		u32 flags = 0;
		if (vm->flags & XE_VM_FLAGS_64K && level == 1)
			flags = GEN12_PDE_64K;

		entry = gen8_pde_encode(bo, map_ofs + (level - 1) * GEN8_PAGE_SIZE, XE_CACHE_WB);
		iosys_map_wr(&bo->vmap, map_ofs + GEN8_PAGE_SIZE * level, u64, entry | flags);

		/* Write PDE's that point to our BO. */
		for (i = 0; i < num_entries - num_level; i++) {
			entry = gen8_pde_encode(bo, i * GEN8_PAGE_SIZE, XE_CACHE_WB);

			/*
			 * HACK: Is it allowed to make level 0 pagetables
			 * 4KiB instead of 64KiB? If not, we should map the
			 * 64 KiB around each pagetable being updated.
			 */
			if (i == NUM_KERNEL_PDE - 1)
				flags = 0;

			iosys_map_wr(&bo->vmap, map_ofs + GEN8_PAGE_SIZE * level + (i + 1) * 8, u64, entry | flags);
		}
	}

	/* Identity map the entire vram at 256GiB offset */
	if (IS_DGFX(xe)) {
		u64 pos, ofs, flags;

		level = 2;
		ofs = map_ofs + GEN8_PAGE_SIZE * level + 256 * 8;
		flags = GEN8_PAGE_RW | GEN8_PAGE_PRESENT | PPAT_CACHED |
			GEN12_PPGTT_PTE_LM | GEN8_PDPE_PS_1G;

		/*
		 * Use 1GB pages, it shouldn't matter the physical amount of
		 * vram is less, when we don't access it.
		 */
		for (pos = 0; pos < m->gt->mem.vram.size; pos += SZ_1G, ofs += 8)
			iosys_map_wr(&bo->vmap, ofs, u64, pos | flags);

	}

	xe_bo_vunmap(bo);

	/*
	 * Example layout created above, with root level = 3:
	 * [PT0...PT7]: kernel PT's for copy/clear; 64 or 4KiB PTE's
	 * [PT8]: Kernel PT for VM_BIND, 4 KiB PTE's
	 * [PT9...PT28]: Userspace PT's for VM_BIND, 4 KiB PTE's
	 * [PT29 = PDE 0] [PT30 = PDE 1] [PT31 = PDE 2]
	 *
	 * This makes the lowest part of the VM point to the pagetables.
	 * Hence the lowest 2M in the vm should point to itself, with a few writes
	 * and flushes, other parts of the VM can be used either for copying and
	 * clearing.
	 *
	 * For performance, the kernel reserves PDE's, so about 20 are left
	 * for async VM updates.
	 *
	 * To make it easier to work, each scratch PT is put in slot (1 + PT #)
	 * everywhere, this allows lockless updates to scratch pages by using
	 * the different addresses in VM.
	 */
	drm_suballoc_manager_init(&m->vm_update_sa, map_ofs / GEN8_PAGE_SIZE - NUM_KERNEL_PDE, 0);

	m->pt_bo = bo;
	return 0;
}

struct xe_migrate *xe_migrate_init(struct xe_gt *gt)
{
	struct xe_device *xe = gt_to_xe(gt);
	struct xe_migrate *m;
	struct xe_vm *vm;
	struct ww_acquire_ctx ww;
	int err;

	m = drmm_kzalloc(&xe->drm, sizeof(*m), GFP_KERNEL);
	if (!m)
		return ERR_PTR(-ENOMEM);

	m->gt = gt;

	/* Special layout, prepared below.. */
	vm = xe_vm_create(xe, XE_VM_FLAG_MIGRATION);
	if (IS_ERR(vm))
		return ERR_CAST(vm);

	xe_vm_lock(vm, &ww, 0, false);
	err = xe_migrate_prepare_vm(m, vm);
	xe_vm_unlock(vm, &ww);
	if (err) {
		xe_vm_close_and_put(vm);
		return ERR_PTR(err);
	}

	m->eng = xe_engine_create_class(xe, vm, XE_ENGINE_CLASS_COPY, ENGINE_FLAG_KERNEL);
	if (IS_ERR(m->eng)) {
		xe_vm_close_and_put(vm);
		return ERR_CAST(m->eng);
	}

	mutex_init(&m->job_mutex);

	if (IS_ENABLED(CONFIG_DRM_XE_DEBUG)) {
		xe_vm_lock(vm, &ww, 0, false);
		xe_migrate_sanity_test(m);
		xe_vm_unlock(vm, &ww);
	}

	err = drmm_add_action_or_reset(&xe->drm, xe_migrate_fini, m);
	if (err)
		return ERR_PTR(err);

	return m;
}

static void emit_arb_clear(struct xe_bb *bb)
{
	/* 1 dword */
	bb->cs[bb->len++] = MI_ARB_ON_OFF | MI_ARB_DISABLE;
}

static void xe_migrate_res_sizes(struct ttm_resource *res,
				 struct xe_res_cursor *cur,
				 u64 *L0, u64 *L1)
{
	if (res->mem_type != TTM_PL_VRAM ||
	    (cur->start & (SZ_2M - 1))) {
		*L1 = 0;
		*L0 = cur->remaining;
	} else {
		*L1 = cur->remaining & ~(SZ_2M - 1);
		*L0 = cur->remaining - *L1;
	}
}

static u32 pte_update_size(struct xe_migrate *m,
			   struct ttm_resource *res,
			   u64 *L0, u64 *L0_ofs, u32 *L0_pt,
			   u64 *L1, u64 *L1_ofs, u32 *L1_pt,
			   u32 cmd_size, u32 pt_ofs, u32 avail_pts)
{
	u32 cmds = 0, L0_size = xe_migrate_pagesize(m), used_pts;

	*L1_pt = pt_ofs;
	if (*L1) {
		u64 size = min(*L1, (u64)SZ_1G * avail_pts);

		*L1 = size;
		*L1_ofs = xe_migrate_vm_addr(pt_ofs, 1);

		used_pts = DIV_ROUND_UP(*L1, SZ_1G);
		avail_pts -= used_pts;
		pt_ofs += used_pts;

		/* MI_STORE_DATA_IMM */
		cmds += 3 * DIV_ROUND_UP(*L1 / SZ_2M, 0x1ff);
		/* Actual PDE qwords */
		cmds += *L1 / SZ_2M * 2;

		cmds += cmd_size * used_pts; /* Clears or copies 1 GiB at a time.. */
	}

	*L0_pt = pt_ofs;
	if (*L0) {
		/* Clip L0 to available size */
		u64 size = min(*L0, (u64)avail_pts * SZ_2M);

		*L0 = size;
		*L0_ofs = xe_migrate_vm_addr(pt_ofs, 0);

		/* MI_STORE_DATA_IMM */
		cmds += 3 * DIV_ROUND_UP(size / L0_size, 0x1ff);

		/* PDE qwords */
		cmds += size / xe_migrate_pagesize(m) * 2;

		/* Each command clears 256 MiB at a time */
		cmds += cmd_size * DIV_ROUND_UP(*L0, SZ_256M);
	}

	return cmds;
}

static void emit_pte(struct xe_migrate *m,
		     struct xe_bb *bb, u32 at_pt, u32 pagesize,
		     struct ttm_resource *res,
		     struct xe_res_cursor *cur,
		     u32 size, struct ttm_tt *ttm)
{
	u32 ptes;
	bool lmem = res->mem_type == TTM_PL_VRAM;
	u32 ofs = at_pt * GEN8_PAGE_SIZE;

	if (!pagesize)
		pagesize = xe_migrate_pagesize(m);

	ptes = size / pagesize;

	while (ptes) {
		u32 chunk = min(0x1ffU, ptes);

		if (pagesize == SZ_64K)
			chunk = min(32U, ptes);

		bb->cs[bb->len++] = MI_STORE_DATA_IMM | BIT(21) |
			(chunk * 2 + 1);
		bb->cs[bb->len++] = ofs;
		bb->cs[bb->len++] = 0;

		if (pagesize == SZ_64K)
			ofs += SZ_4K;
		else
			ofs += chunk * 8;
		ptes -= chunk;

		while (chunk--) {
			u64 addr;

			if (lmem) {
				addr = cur->start | GEN12_PPGTT_PTE_LM;
			} else {
				unsigned long page = cur->start >> PAGE_SHIFT;
				unsigned long offset = cur->start & (PAGE_SIZE - 1);

				addr = ttm->dma_address[page] + offset;
			}
			addr |= PPAT_CACHED | GEN8_PAGE_PRESENT | GEN8_PAGE_RW;
			if (pagesize == SZ_2M)
				addr |= GEN8_PDE_PS_2M;

			bb->cs[bb->len++] = lower_32_bits(addr);
			bb->cs[bb->len++] = upper_32_bits(addr);

			xe_res_next(cur, pagesize);
		}
	}
}

static void emit_copy(struct xe_gt *gt, struct xe_bb *bb,
		      u64 src_ofs, u64 dst_ofs, unsigned int size,
		      unsigned pitch)
{
	XE_BUG_ON(size / pitch > S16_MAX);
	XE_BUG_ON(pitch / 4 > S16_MAX);
	XE_BUG_ON(pitch > U16_MAX);

	bb->cs[bb->len++] = GEN9_XY_FAST_COPY_BLT_CMD | (10 - 2);
	bb->cs[bb->len++] = BLT_DEPTH_32 | pitch;
	bb->cs[bb->len++] = 0;
	bb->cs[bb->len++] = (size / pitch) << 16 | pitch / 4;
	bb->cs[bb->len++] = lower_32_bits(dst_ofs);
	bb->cs[bb->len++] = upper_32_bits(dst_ofs);
	bb->cs[bb->len++] = 0;
	bb->cs[bb->len++] = pitch;
	bb->cs[bb->len++] = lower_32_bits(src_ofs);
	bb->cs[bb->len++] = upper_32_bits(src_ofs);
}

static void partition(u64 *lmem_L1, u64 *lmem_L0, u64 *sysmem,
		      u32 *lmem_pts, u32 *sysmem_pts)
{
	XE_BUG_ON(*lmem_L0 >= SZ_2M);

	*lmem_L0 = 0;
	*lmem_pts = 1;
	*sysmem_pts = min_t(u32, *lmem_L1 / SZ_2M, NUM_KERNEL_PDE - 2);
	*sysmem = *lmem_L1 = *sysmem_pts * SZ_2M;
}

struct dma_fence *xe_migrate_copy(struct xe_migrate *m,
				  struct xe_bo *bo,
				  struct ttm_resource *src,
				  struct ttm_resource *dst)
{
	struct xe_gt *gt = m->gt;
	struct xe_device *xe = gt_to_xe(gt);
	struct dma_fence *fence = NULL;
	u64 size = bo->size;
	struct xe_res_cursor src_it, dst_it;
	struct ttm_tt *ttm = bo->ttm.ttm;
	u64 src_L0_ofs, src_L1_ofs, dst_L0_ofs, dst_L1_ofs;
	u32 src_L0_pt, src_L1_pt, dst_L0_pt, dst_L1_pt;
	u64 src_L0, src_L1, dst_L0, dst_L1;
	int pass = 0;
	int err;

	err = dma_resv_reserve_fences(bo->ttm.base.resv, 1);
	if (err)
		return ERR_PTR(err);

	xe_res_first(src, 0, bo->size, &src_it);
	xe_res_first(dst, 0, bo->size, &dst_it);

	while (size) {
		u32 batch_size = 8 + 512;	/* FIXME: 512 is hack to fix
						   eviction bug, issue #52 */
		struct xe_sched_job *job;
		struct xe_bb *bb;
		u32 num_src_pts;
		u32 num_dst_pts;
		u32 update_idx;

		dma_fence_put(fence);

		xe_migrate_res_sizes(src, &src_it, &src_L0, &src_L1);
		xe_migrate_res_sizes(dst, &dst_it, &dst_L0, &dst_L1);

		drm_dbg(&xe->drm, "Pass %u, sizes: %llu / %llu & %llu %llu\n",
			pass++, src_L1, src_L0, dst_L1, dst_L0);

		/* Only copying to and from lmem, not both sides lmem */
		XE_BUG_ON(src_L1 && dst_L1);

		num_src_pts = DIV_ROUND_UP(src_L1, SZ_1G) +
			DIV_ROUND_UP(src_L0, SZ_2M);
		num_dst_pts = DIV_ROUND_UP(dst_L1, SZ_1G) +
			DIV_ROUND_UP(dst_L0, SZ_2M);

		if (num_src_pts + num_dst_pts > NUM_KERNEL_PDE - 1) {
			/* Copy the biggest chunk we can */
			if (src_L1)
				partition(&src_L1, &src_L0, &dst_L0,
					  &num_src_pts, &num_dst_pts);
			else
				partition(&dst_L1, &dst_L0, &src_L0,
					  &num_dst_pts, &num_src_pts);
		}

		batch_size += pte_update_size(m, src, &src_L0, &src_L0_ofs,
					      &src_L0_pt, &src_L1, &src_L1_ofs,
					      &src_L1_pt, 0, 0, num_src_pts);

		batch_size += pte_update_size(m, dst, &dst_L0, &dst_L0_ofs,
					      &dst_L0_pt, &dst_L1, &dst_L1_ofs,
					      &dst_L1_pt, 0, num_src_pts,
					      num_dst_pts);

		/* Add copy commands size here */
		batch_size += 10 *
			(1 + (src_L1 && src_L0) + (dst_L1 && dst_L0));

		XE_BUG_ON(src_L0 + src_L1 != dst_L0 + dst_L1);

		bb = xe_bb_new(gt, batch_size);
		if (IS_ERR(bb))
			return ERR_CAST(bb);

		emit_arb_clear(bb);
		if (src_L1)
			emit_pte(m, bb, src_L1_pt, SZ_2M, src, &src_it, src_L1,
				 ttm);
		if (src_L0)
			emit_pte(m, bb, src_L0_pt, 0, src, &src_it, src_L0,
				 ttm);
		if (dst_L1)
			emit_pte(m, bb, dst_L1_pt, SZ_2M, dst, &dst_it, dst_L1,
				 ttm);
		if (dst_L0)
			emit_pte(m, bb, dst_L0_pt, 0, dst, &dst_it, dst_L0,
				 ttm);

		bb->cs[bb->len++] = MI_BATCH_BUFFER_END;
		update_idx = bb->len;

		if (src_L1) {
			emit_copy(gt, bb, src_L1_ofs, dst_L0_ofs, src_L1,
				  SZ_32K);
			if (src_L0)
				emit_copy(gt, bb, src_L0_ofs,
					  dst_L0_ofs + src_L1, src_L0, SZ_4K);
		} else if (dst_L1) {
			emit_copy(gt, bb, src_L0_ofs, dst_L1_ofs, dst_L1,
				  SZ_32K);
			if (dst_L0)
				emit_copy(gt, bb, src_L0_ofs + dst_L1,
					  dst_L0_ofs, dst_L0, SZ_4K);
		} else {
			emit_copy(gt, bb, src_L0_ofs, dst_L0_ofs, src_L0,
				  SZ_4K);
		}

		mutex_lock(&m->job_mutex);

		job = xe_bb_create_migration_job(m->eng, bb, m->batch_base_ofs,
						 update_idx);
		if (IS_ERR(job)) {
			err = PTR_ERR(job);
			goto err;
		}

		if (!fence) {
			err = drm_sched_job_add_dependencies_resv(&job->drm,
								  bo->ttm.base.resv,
								  DMA_RESV_USAGE_PREEMPT_FENCE);
			if (err)
				goto err_job;
		}

		xe_sched_job_arm(job);
		fence = dma_fence_get(&job->drm.s_fence->finished);
		xe_sched_job_push(job);

		dma_fence_put(m->fence);
		m->fence = dma_fence_get(fence);

		mutex_unlock(&m->job_mutex);

		xe_bb_free(bb, fence);
		size -= src_L1 + src_L0;
		continue;

err_job:
		xe_sched_job_free(job);
err:
		mutex_unlock(&m->job_mutex);
		xe_bb_free(bb, NULL);
		return ERR_PTR(err);
	}

	dma_resv_add_fence(bo->ttm.base.resv, fence,
			   DMA_RESV_USAGE_KERNEL);

	return fence;
}

static int emit_clear(struct xe_bb *bb, u64 src_ofs, u32 size, u32 pitch, u32 value)
{
	BUG_ON(size / pitch > S16_MAX);
	BUG_ON(pitch / 4 > S16_MAX);

	bb->cs[bb->len++] = XY_COLOR_BLT_CMD | BLT_WRITE_RGBA | (7 - 2);
	bb->cs[bb->len++] = BLT_DEPTH_32 | BLT_ROP_COLOR_COPY | pitch;
	bb->cs[bb->len++] = 0;
	bb->cs[bb->len++] = (size / pitch) << 16 | pitch / 4;
	bb->cs[bb->len++] = lower_32_bits(src_ofs);
	bb->cs[bb->len++] = upper_32_bits(src_ofs);
	bb->cs[bb->len++] = value;

	return 0;
}

struct dma_fence *xe_migrate_clear(struct xe_migrate *m,
				   struct xe_bo *bo,
				   u32 value)
{
	struct xe_gt *gt = m->gt;
	struct xe_device *xe = gt_to_xe(gt);
	struct dma_fence *fence = NULL;
	u64 size = bo->size;
	struct xe_res_cursor src_it;
	struct ttm_resource *src = bo->ttm.resource;
	int err;
	int pass = 0;

	err = dma_resv_reserve_fences(bo->ttm.base.resv, 1);
	if (err)
		return ERR_PTR(err);

	xe_res_first(src, 0, bo->size, &src_it);

	while (size) {
		u64 clear_L0_ofs, clear_L1_ofs;
		u32 clear_L0_pt, clear_L1_pt;
		u64 clear_L0, clear_L1;
		struct xe_sched_job *job;
		struct xe_bb *bb;
		u32 batch_size = 8, update_idx;

		/* Obtain max we can clear through L0 and L1 */
		xe_migrate_res_sizes(src, &src_it, &clear_L0, &clear_L1);
		drm_dbg(&xe->drm, "Pass %u, sizes: %llu / %llu\n", pass++, clear_L1, clear_L0);

		/* And calculate final sizes and batch size.. */
		batch_size +=
			pte_update_size(m, src,
					&clear_L0, &clear_L0_ofs, &clear_L0_pt,
					&clear_L1, &clear_L1_ofs, &clear_L1_pt,
					7, 0, NUM_KERNEL_PDE - 1);

		dma_fence_put(fence);

		if (WARN_ON_ONCE(!clear_L0 && !clear_L1))
			break;

		bb = xe_bb_new(gt, batch_size);
		if (IS_ERR(bb))
			return ERR_CAST(bb);
		size -= clear_L0 + clear_L1;

		/* TODO: Add dependencies here */
		if (clear_L1)
			emit_pte(m, bb, clear_L1_pt, SZ_2M, src, &src_it,
				 clear_L1, bo->ttm.ttm);
		if (clear_L0)
			emit_pte(m, bb, clear_L0_pt, 0, src, &src_it, clear_L0,
				 bo->ttm.ttm);
		bb->cs[bb->len++] = MI_BATCH_BUFFER_END;
		update_idx = bb->len;

		while (clear_L1) {
			u64 chunk = min_t(u64, clear_L1, SZ_1G);

			emit_clear(bb, clear_L1_ofs, chunk, SZ_16K, value);
			clear_L1 -= chunk;
			clear_L1_ofs += chunk;
		}

		while (clear_L0) {
			u32 chunk = min_t(u32, clear_L0, SZ_256M);

			emit_clear(bb, clear_L0_ofs, chunk, SZ_4K, value);
			clear_L0 -= chunk;
			clear_L0_ofs += chunk;
		}

		mutex_lock(&m->job_mutex);
		job = xe_bb_create_migration_job(m->eng, bb, m->batch_base_ofs,
						 update_idx);
		if (IS_ERR(job)) {
			err = PTR_ERR(job);
			goto err;
		}

		if (!fence) {
			err = drm_sched_job_add_implicit_dependencies(&job->drm, &bo->ttm.base, true);
			if (err)
				goto err_job;
		}

		xe_sched_job_arm(job);
		fence = dma_fence_get(&job->drm.s_fence->finished);
		xe_sched_job_push(job);

		dma_fence_put(m->fence);
		m->fence = dma_fence_get(fence);

		mutex_unlock(&m->job_mutex);

		xe_bb_free(bb, fence);
		continue;

err_job:
		xe_sched_job_free(job);
		mutex_unlock(&m->job_mutex);
err:
		xe_bb_free(bb, NULL);
		return ERR_PTR(err);
	}

	dma_resv_add_fence(bo->ttm.base.resv, fence,
			   DMA_RESV_USAGE_KERNEL);

	return fence;
}

static struct dma_fence *
xe_migrate_update_pgtables_cpu(struct xe_migrate *m,
			       struct xe_vm *vm,
			       struct xe_bo *bo,
			       struct xe_engine *eng,
			       struct xe_vm_pgtable_update *updates, u32 num_updates,
			       struct xe_sync_entry *syncs, u32 num_syncs,
			       xe_migrate_populatefn_t populatefn,
			       void *arg)
{
	int err = 0;
	u32 i, j;
	struct ttm_bo_kmap_obj maps[9];

	BUG_ON(num_updates > ARRAY_SIZE(maps));

	for (i = 0; i < num_syncs; i++) {
		err = xe_sync_entry_wait(&syncs[i]);
		if (err)
			return ERR_PTR(err);
	}

	if (bo) {
		long wait;

		wait = dma_resv_wait_timeout(bo->ttm.base.resv,
					     DMA_RESV_USAGE_KERNEL,
					     true, MAX_SCHEDULE_TIMEOUT);
		if (wait <= 0)
			return ERR_PTR(-ETIME);
	}

	for (i = 0; i < num_updates; i++) {
		err = ttm_bo_kmap(&updates[i].pt_bo->ttm, 0,
				  updates[i].pt_bo->size / GEN8_PAGE_SIZE, &maps[i]);
		if (err)
			goto unmap;
	}

	for (i = 0; i < num_updates; i++) {
		bool is_iomem;
		struct xe_vm_pgtable_update *update = &updates[i];
		u64 *map_u64 = ttm_kmap_obj_virtual(&maps[i], &is_iomem);

		if (is_iomem) {
			u64 val[192];

			BUG_ON(update->qwords > ARRAY_SIZE(val));

			populatefn(&val, update->ofs, update->qwords, update, arg);
			for (j = 0; j < update->qwords; j++)
				writeq(val[j], (u64 __iomem *)&map_u64[j + update->ofs]);
		} else {
			populatefn(&map_u64[update->ofs], update->ofs,
				   update->qwords, update, arg);
		}
	}

unmap:
	while (i-- > 0)
		ttm_bo_kunmap(&maps[i]);

	if (err)
		return ERR_PTR(err);

	return dma_fence_get_stub();
}


static void write_pgtable(struct xe_bb *bb, u64 ppgtt_ofs,
			  struct xe_vm_pgtable_update *update,
			  xe_migrate_populatefn_t populatefn, void *arg)
{
	u32 chunk;
	u32 ofs = update->ofs, size = update->qwords;

	/*
	 * If we have 512 entries (max), we would populate it ourselves,
	 * and update the PDE above it to the new pointer.
	 * The only time this can only happen if we have to update the top
	 * PDE. This requires a BO that is almost vm->size big.
	 *
	 * This shouldn't be possible in practice.. might change when 16K
	 * pages are used. Hence the BUG_ON.
	 */
	XE_BUG_ON(update->qwords > 0x1ff);
	if (!ppgtt_ofs) {
		bool is_lmem;

		ppgtt_ofs = xe_migrate_vram_ofs(xe_bo_addr(update->pt_bo, 0, GEN8_PAGE_SIZE, &is_lmem));
		XE_BUG_ON(!is_lmem);
	}

	do {
		u64 addr = ppgtt_ofs + ofs * 8;
		chunk = min(update->qwords, 0x1ffU);

		/* Ensure populatefn can do memset64 by aligning bb->cs */
		if (!(bb->len & 1))
			bb->cs[bb->len++] = MI_NOOP;

		bb->cs[bb->len++] = MI_STORE_DATA_IMM | BIT(21) | (chunk * 2 + 1);
		bb->cs[bb->len++] = lower_32_bits(addr);
		bb->cs[bb->len++] = upper_32_bits(addr);
		populatefn(bb->cs + bb->len, ofs, chunk, update, arg);

		bb->len += chunk * 2;
		ofs += chunk;
		size -= chunk;
	} while (size);
}

struct xe_vm *xe_migrate_get_vm(struct xe_migrate *m)
{
	return xe_vm_get(m->eng->vm);
}

struct dma_fence *
xe_migrate_update_pgtables(struct xe_migrate *m,
			   struct xe_vm *vm,
			   struct xe_bo *bo,
			   struct xe_engine *eng,
			   struct xe_vm_pgtable_update *updates,
			   u32 num_updates,
			   struct xe_sync_entry *syncs, u32 num_syncs,
			   xe_migrate_populatefn_t populatefn, void *arg)
{
	struct xe_gt *gt = m->gt;
	struct xe_device *xe = gt_to_xe(gt);
	struct xe_sched_job *job;
	struct dma_fence *fence;
	struct drm_suballoc *sa_bo = NULL;
	struct xe_vma *vma = arg;
	struct xe_bb *bb;
	u32 i, batch_size, ppgtt_ofs, update_idx;
	u64 addr;
	int err = 0;

	if (gt_to_xe(gt)->info.platform == XE_DG2) {
		fence = xe_migrate_update_pgtables_cpu(m, vm, bo, eng, updates,
						       num_updates, syncs,
						       num_syncs, populatefn,
						       arg);
		if (IS_ERR(fence))
			return fence;

		for (i = 0; i < num_syncs; ++i)
			xe_sync_entry_signal(&syncs[i], NULL, fence);

		return fence;
	}

	/* fixed + PTE entries */
	if (IS_DGFX(xe))
		batch_size = 2;
	else
		batch_size = 6 + num_updates * 2;

	for (i = 0; i < num_updates; i++) {
		u32 num_cmds = DIV_ROUND_UP(updates[i].qwords, 0x1ff);

		/* align noop + MI_STORE_DATA_IMM cmd prefix */
		batch_size += 4 * num_cmds + updates[i].qwords * 2;
	}

	/*
	 * XXX: Create temp bo to copy from, if batch_size becomes too big?
	 *
	 * Worst case: Sum(2 * (each lower level page size) + (top level page size))
	 * Should be reasonably bound..
	 */
	XE_BUG_ON(batch_size >= SZ_128K);

	bb = xe_bb_new(gt, batch_size);
	if (IS_ERR(bb))
		return ERR_CAST(bb);

	/* For sysmem PTE's, need to map them in our hole.. */
	if (!IS_DGFX(xe)) {
		ppgtt_ofs = NUM_KERNEL_PDE - 1;
		if (eng) {
			sa_bo = drm_suballoc_new(&m->vm_update_sa, num_updates);
			if (IS_ERR(sa_bo)) {
				err = PTR_ERR(sa_bo);
				goto err;
			}

			ppgtt_ofs = NUM_KERNEL_PDE + sa_bo->soffset;
		}
		emit_arb_clear(bb);

		/* Map our PT's to gtt */
		bb->cs[bb->len++] = MI_STORE_DATA_IMM | BIT(21) | (num_updates * 2 + 1);
		bb->cs[bb->len++] = ppgtt_ofs * GEN8_PAGE_SIZE;
		bb->cs[bb->len++] = 0; /* upper_32_bits */

		for (i = 0; i < num_updates; i++) {
			struct xe_bo *bo = updates[i].pt_bo;

			BUG_ON(bo->size != SZ_4K);

			addr = gen8_pte_encode(NULL, bo, 0, XE_CACHE_WB, 0, 0);
			bb->cs[bb->len++] = lower_32_bits(addr);
			bb->cs[bb->len++] = upper_32_bits(addr);
		}

		bb->cs[bb->len++] = MI_BATCH_BUFFER_END;
		update_idx = bb->len;

		addr = xe_migrate_vm_addr(ppgtt_ofs, 0);
		for (i = 0; i < num_updates; i++)
			write_pgtable(bb, addr + i * GEN8_PAGE_SIZE, &updates[i], populatefn, arg);
	} else {
		/* phys pages, no preamble required */
		bb->cs[bb->len++] = MI_BATCH_BUFFER_END;
		update_idx = bb->len;

		emit_arb_clear(bb);
		for (i = 0; i < num_updates; i++)
			write_pgtable(bb, 0, &updates[i], populatefn, arg);
	}

	if (!eng)
		mutex_lock(&m->job_mutex);

	job = xe_bb_create_migration_job(eng ?: m->eng, bb, m->batch_base_ofs, update_idx);
	if (IS_ERR(job)) {
		err = PTR_ERR(job);
		goto err_bb;
	}

	/* Wait on BO move */
	if (bo) {
		err = drm_sched_job_add_dependencies_resv(&job->drm,
							  bo->ttm.base.resv,
							  DMA_RESV_USAGE_KERNEL);
		if (err)
			goto err_job;
	}

	/*
	 * Munmap style VM unbind, need to wait for all jobs to be complete /
	 * trigger preempts before moving forward
	 */
	if (vma->first_munmap_rebind) {
		err = drm_sched_job_add_dependencies_resv(&job->drm, &vm->resv,
							  DMA_RESV_USAGE_PREEMPT_FENCE);
		if (err)
			goto err_job;
	}

	for (i = 0; !err && i < num_syncs; i++)
		err = xe_sync_entry_add_deps(&syncs[i], job);

	if (err)
		goto err_job;

	xe_sched_job_arm(job);
	fence = dma_fence_get(&job->drm.s_fence->finished);
	xe_sched_job_push(job);

	if (!eng)
		mutex_unlock(&m->job_mutex);

	for (i = 0; i < num_syncs; i++)
		xe_sync_entry_signal(&syncs[i], job, fence);

	xe_bb_free(bb, fence);
	drm_suballoc_free(sa_bo, fence, -1);

	return fence;

err_job:
	xe_sched_job_free(job);
err_bb:
	if (!eng)
		mutex_unlock(&m->job_mutex);
	xe_bb_free(bb, NULL);
err:
	drm_suballoc_free(sa_bo, NULL, 0);
	return ERR_PTR(err);
}

void xe_migrate_wait(struct xe_migrate *m)
{
	if (m->fence)
		dma_fence_wait(m->fence, false);
}

static bool sanity_fence_failed(struct xe_device *xe, struct dma_fence *fence, const char *str)
{
	long ret;

	if (IS_ERR(fence)) {
		drm_err(&xe->drm, "Failed to create fence for %s: %li\n", str, PTR_ERR(fence));
		return true;
	}
	if (!fence)
		return true;

	ret = dma_fence_wait_timeout(fence, false, 5 * HZ);
	if (ret <= 0) {
		drm_err(&xe->drm, "Fence timed out for %s: %li\n", str, ret);
		return true;
	}

	return false;
}

static int run_sanity_job(struct xe_migrate *m, struct xe_device *xe, struct xe_bb *bb, u32 second_idx, const char *str)
{
	struct xe_sched_job *job = xe_bb_create_migration_job(m->eng, bb, m->batch_base_ofs, second_idx);
	struct dma_fence *fence;

	if (IS_ERR(job)) {
		drm_err(&xe->drm, "Failed to allocate fake pt: %li\n", PTR_ERR(job));
		return PTR_ERR(job);
	}

	xe_sched_job_arm(job);
	fence = dma_fence_get(&job->drm.s_fence->finished);
	xe_sched_job_push(job);

	if (sanity_fence_failed(xe, fence, str))
		return -ETIMEDOUT;

	dma_fence_put(fence);
	drm_dbg(&xe->drm, "%s: Job completed\n", str);
	return 0;
}

static void
sanity_populate_cb(void *dst, u32 qword_ofs, u32 num_qwords,
		   struct xe_vm_pgtable_update *update,
		   void *arg)
{
	int i;
	u64 *ptr = dst;

	for (i = 0; i < num_qwords; i++)
		ptr[i] = (qword_ofs + i - update->ofs) * 0x1111111111111111ULL;
}

#define check(retval, expected, str) \
	do { if (retval != expected) { \
		drm_err(&xe->drm, "Sanity check failed: "str" expected %llx, got %llx\n", \
			(u64)expected, (u64)retval); \
	} } while (0)

static void test_copy(struct xe_migrate *m, struct xe_bo *bo)
{
	struct xe_device *xe = gt_to_xe(m->gt);
	u64 retval, expected = 0xc0c0c0c0c0c0c0c0ULL;
	bool big = bo->size >= SZ_2M;
	struct dma_fence *fence;
	const char *str = big ? "Copying big bo" : "Copying small bo";
	int err;

	struct xe_bo *sysmem = xe_bo_create_pin_map(xe, m->eng->vm, bo->size,
						    ttm_bo_type_kernel,
						    XE_BO_CREATE_SYSTEM_BIT |
						    XE_BO_CREATE_PINNED_BIT |
						    XE_BO_INTERNAL_TEST);
	if (IS_ERR(sysmem)) {
		drm_err(&xe->drm, "Failed to allocate sysmem bo for %s: %li\n", str, PTR_ERR(sysmem));
		return;
	}

	err = xe_bo_populate(sysmem);
	if (err)
		goto free_sysmem;

	iosys_map_memset(&sysmem->vmap, 0, 0xd0, sysmem->size);
	fence = xe_migrate_clear(m, sysmem, 0xc0c0c0c0);
	if (!sanity_fence_failed(xe, fence, big ? "Clearing sysmem big bo" :
				 "Clearing sysmem small bo")) {
                retval = iosys_map_rd(&sysmem->vmap, 0, u64);
                check(retval, expected, "sysmem first offset should be cleared");
                retval = iosys_map_rd(&sysmem->vmap, sysmem->size - 8, u64);
                check(retval, expected, "sysmem last offset should be cleared");
	}
	dma_fence_put(fence);

	/* Try to copy 0xc0 from sysmem to lmem with 2MB or 64KiB/4KiB pages */
	iosys_map_memset(&sysmem->vmap, 0, 0xc0, sysmem->size);
	iosys_map_memset(&bo->vmap, 0, 0xd0, bo->size);

	fence = xe_migrate_copy(m, sysmem, sysmem->ttm.resource, bo->ttm.resource);
	if (!sanity_fence_failed(xe, fence, big ? "Copying big bo sysmem -> vram" : "Copying small bo sysmem -> vram")) {
		retval = iosys_map_rd(&bo->vmap, 0, u64);
		check(retval, expected, "sysmem -> vram bo first offset should be copied");
		retval = iosys_map_rd(&bo->vmap, bo->size - 8, u64);
		check(retval, expected, "sysmem -> vram bo offset should be copied");
	}
	dma_fence_put(fence);

	/* And other way around.. slightly hacky.. */
	iosys_map_memset(&sysmem->vmap, 0, 0xd0, sysmem->size);
	iosys_map_memset(&bo->vmap, 0, 0xc0, bo->size);

	fence = xe_migrate_copy(m, sysmem, bo->ttm.resource, sysmem->ttm.resource);
	if (!sanity_fence_failed(xe, fence, big ? "Copying big bo vram -> sysmem" : "Copying small bo vram -> sysmem")) {
		retval = iosys_map_rd(&sysmem->vmap, 0, u64);
		check(retval, expected, "vram -> sysmem bo first offset should be copied");
		retval = iosys_map_rd(&sysmem->vmap, bo->size - 8, u64);
		check(retval, expected, "vram -> sysmem bo last offset should be copied");
	}
	dma_fence_put(fence);

free_sysmem:
	xe_bo_unpin(sysmem);
	xe_bo_put(sysmem);
}

static void test_addressing_2mb(struct xe_migrate *m)
{
	struct xe_bb *bb = xe_bb_new(m->gt, 1024);
	struct xe_device *xe = gt_to_xe(m->gt);
	u32 size = (NUM_KERNEL_PDE - 1) * SZ_2M;
	struct xe_res_cursor src_it;
	u64 expected, retval;
	struct xe_bo *bo;
	int i, err;
	u32 update_idx;

	if (IS_ERR(bb)) {
		drm_err(&xe->drm, "Failed to create a batchbuffer for testing 2mb: %li\n", PTR_ERR(bb));
		return;
	}

	bo = xe_bo_create_pin_map(xe, m->eng->vm, size,
				  ttm_bo_type_kernel,
				  XE_BO_CREATE_VRAM_BIT |
				  XE_BO_CREATE_PINNED_BIT);

	if (IS_ERR(bo)) {
		drm_err(&xe->drm, "Failed to create a fake bo for testing pagetables: %li\n", PTR_ERR(bo));
		goto err_bb;
	}

	err = xe_bo_populate(bo);
	if (err)
		goto err_bo;

	iosys_map_memset(&bo->vmap, 0, 0xcc, bo->size);

	/* write our pagetables, one at a time.. */
	xe_res_first(bo->ttm.resource, 0, bo->size, &src_it);
	for (i = 0; i < NUM_KERNEL_PDE - 1; i++)
		emit_pte(m, bb, i, SZ_2M, bo->ttm.resource, &src_it, SZ_2M, bo->ttm.ttm);

	bb->cs[bb->len++] = MI_BATCH_BUFFER_END;
	update_idx = bb->len;

	for (i = 0; i < NUM_KERNEL_PDE - 1; i++)
		emit_clear(bb, xe_migrate_vm_addr(i, 1), SZ_2M, SZ_16K, 0xff124800U | i);

	run_sanity_job(m, xe, bb, update_idx, "Testing that 2 MB pages job work as intended");

	for (i = 0; i < NUM_KERNEL_PDE - 1; i++) {
		u32 addrs[] = { 0, 4096, SZ_2M - 8 };
		u32 j;

		expected = 0xff124800U | i;
		expected |= expected << 32ULL;

		for (j = 0; j < ARRAY_SIZE(addrs); j += 8) {
			retval = iosys_map_rd(&bo->vmap, i * SZ_2M + addrs[j], u64);
			if (retval != expected) {
				drm_err(&xe->drm, "Sanity check failed at 2 mb page %u offset %u expected %llx, got %llx\n",
					i, addrs[j], expected, retval);
				break;
			}
		}
	}

err_bo:
	xe_bo_unpin(bo);
	xe_bo_put(bo);
err_bb:
	xe_bb_free(bb, NULL);
}

static void test_pt_update(struct xe_migrate *m, struct xe_bo *pt)
{
	struct xe_device *xe = gt_to_xe(m->gt);
	struct dma_fence *fence;
	u64 retval, expected;
	int i;

	struct xe_vm_pgtable_update update = {
		.ofs = 1,
		.qwords = 0x10,
		.pt_bo = pt,
	};

	/* Test xe_migrate_update_pgtables() updates the pagetable as expected */
	expected = 0xf0f0f0f0f0f0f0f0ULL;
	iosys_map_memset(&pt->vmap, 0, (u8)expected, pt->size);

	fence = xe_migrate_update_pgtables(m, NULL, NULL, m->eng, &update, 1,
					   NULL, 0, sanity_populate_cb,
					   NULL);
	if (sanity_fence_failed(xe, fence, "Migration pagetable update"))
		return;

	dma_fence_put(fence);
	retval = iosys_map_rd(&pt->vmap, 0, u64);
	check(retval, expected, "PTE[0] must stay untouched");

	for (i = 0; i < update.qwords; i++) {
		retval = iosys_map_rd(&pt->vmap, (update.ofs + i) * 8, u64);
		check(retval, i * 0x1111111111111111ULL, "PTE update");
	}

	retval = iosys_map_rd(&pt->vmap, 8 * (update.ofs + update.qwords), u64);
	check(retval, expected, "PTE[0x11] must stay untouched");
}

static void xe_migrate_sanity_test(struct xe_migrate *m)
{
	struct xe_device *xe = gt_to_xe(m->gt);
	struct xe_bo *pt, *bo = m->pt_bo, *big, *tiny;
	struct xe_res_cursor src_it;
	struct dma_fence *fence;
	u64 retval, expected;
	struct xe_bb *bb;
	int err;

	err = xe_bo_vmap(bo);
	if (err) {
		drm_err(&xe->drm, "Failed to vmap our pagetables: %li\n", PTR_ERR(bo));
		return;
	}

	big = xe_bo_create_pin_map(xe, m->eng->vm, SZ_4M, ttm_bo_type_kernel,
				      XE_BO_CREATE_VRAM_IF_DGFX(xe) |
				      XE_BO_CREATE_PINNED_BIT);
	if (IS_ERR(big)) {
		drm_err(&xe->drm, "Failed to allocate bo: %li\n", PTR_ERR(big));
		goto vunmap;
	}
	err = xe_bo_populate(big);
	if (err)
		goto free_big;

	pt = xe_bo_create_pin_map(xe, m->eng->vm, GEN8_PAGE_SIZE, ttm_bo_type_kernel,
				  XE_BO_CREATE_VRAM_IF_DGFX(xe) |
				  XE_BO_CREATE_IGNORE_MIN_PAGE_SIZE_BIT |
				  XE_BO_CREATE_PINNED_BIT);
	if (IS_ERR(pt)) {
		drm_err(&xe->drm, "Failed to allocate fake pt: %li\n", PTR_ERR(pt));
		goto free_big;
	}
	err = xe_bo_populate(pt);
	if (err)
		goto free_pt;

	tiny = xe_bo_create_pin_map(xe, m->eng->vm, 2 * xe_migrate_pagesize(m), ttm_bo_type_kernel,
				  XE_BO_CREATE_VRAM_IF_DGFX(xe) |
				  XE_BO_CREATE_PINNED_BIT);
	if (IS_ERR(tiny)) {
		drm_err(&xe->drm, "Failed to allocate fake pt: %li\n", PTR_ERR(pt));
		goto free_pt;
	}
	err = xe_bo_populate(tiny);
	if (err)
		goto free_tiny;

	bb = xe_bb_new(m->gt, 32);
	if (IS_ERR(bb)) {
		drm_err(&xe->drm, "Failed to create batchbuffer: %li\n", PTR_ERR(bb));
		goto free_tiny;
	}

	drm_dbg(&xe->drm, "Starting tests, top level PT addr: %llx, special pagetable base addr: %llx\n",
		xe_bo_main_addr(m->eng->vm->pt_root->bo, GEN8_PAGE_SIZE),
		xe_bo_main_addr(m->pt_bo, GEN8_PAGE_SIZE));

	/* First part of the test, are we updating our pagetable bo with a new entry? */
	iosys_map_wr(&bo->vmap, GEN8_PAGE_SIZE * (NUM_KERNEL_PDE - 1), u64, 0xdeaddeadbeefbeef);
	expected = gen8_pte_encode(NULL, pt, 0, XE_CACHE_WB, 0, 0);

	xe_res_first(pt->ttm.resource, 0, pt->size, &src_it);
	emit_pte(m, bb, NUM_KERNEL_PDE - 1, GEN8_PAGE_SIZE, pt->ttm.resource, &src_it, GEN8_PAGE_SIZE, pt->ttm.ttm);
	run_sanity_job(m, xe, bb, bb->len, "Writing PTE for our fake PT");

	retval = iosys_map_rd(&bo->vmap, GEN8_PAGE_SIZE * (NUM_KERNEL_PDE - 1), u64);
	check(retval, expected, "PTE entry write");

	/* Now try to write data to our newly mapped 'pagetable', see if it succeeds */
	bb->len = 0;
	bb->cs[bb->len++] = MI_BATCH_BUFFER_END;
	iosys_map_wr(&pt->vmap, 0, u32, 0xdeaddead);
	expected = 0x12345678U;

	emit_clear(bb, xe_migrate_vm_addr(NUM_KERNEL_PDE - 1, 0), 4, 4, expected);
	run_sanity_job(m, xe, bb, 1, "Writing to our newly mapped pagetable");

	retval = iosys_map_rd(&pt->vmap, 0, u32);
	check(retval, expected, "Write to PT after adding PTE");

	if (IS_DGFX(xe))
		test_addressing_2mb(m);

	/* Sanity checks passed, try the full ones! */

	/* Clear a small bo */
	iosys_map_memset(&tiny->vmap, 0, 0x22, tiny->size);
	expected = 0x224488ff;
	fence = xe_migrate_clear(m, tiny, expected);
	if (sanity_fence_failed(xe, fence, "Clearing small bo"))
		goto out;

	dma_fence_put(fence);
	retval = iosys_map_rd(&tiny->vmap, 0, u32);
	check(retval, expected, "Command clear small first value");
	retval = iosys_map_rd(&tiny->vmap, tiny->size - 4, u32);
	check(retval, expected, "Command clear small last value");

	if (IS_DGFX(xe))
		test_copy(m, tiny);

	/* Clear a big bo with a fixed value */
	iosys_map_memset(&big->vmap, 0, 0x11, big->size);
	expected = 0x11223344U;
	fence = xe_migrate_clear(m, big, expected);
	if (sanity_fence_failed(xe, fence, "Clearing big bo"))
		goto out;

	dma_fence_put(fence);
	retval = iosys_map_rd(&big->vmap, 0, u32);
	check(retval, expected, "Command clear big first value");
	retval = iosys_map_rd(&big->vmap, big->size - 4, u32);
	check(retval, expected, "Command clear big last value");

	if (IS_DGFX(xe))
		test_copy(m, big);

	test_pt_update(m, pt);

out:
	xe_bb_free(bb, NULL);
free_tiny:
	xe_bo_unpin(tiny);
	xe_bo_put(tiny);
free_pt:
	xe_bo_unpin(pt);
	xe_bo_put(pt);
free_big:
	xe_bo_unpin(big);
	xe_bo_put(big);
vunmap:
	xe_bo_vunmap(m->pt_bo);
}
