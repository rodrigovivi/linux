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

#include <linux/sizes.h>
#include <drm/drm_managed.h>
#include <drm/ttm/ttm_tt.h>

#include "../i915/gt/intel_gpu_commands.h"

struct xe_migrate {
	struct drm_mm_node copy_node;
	struct xe_engine *eng;
	struct xe_gt *gt;
	struct mutex job_mutex;
};

#define CHUNK_SZ SZ_8M

static void xe_migrate_fini(struct drm_device *dev, void *arg)
{
	struct xe_migrate *m = arg;

	mutex_destroy(&m->job_mutex);
	xe_engine_put(m->eng);
	xe_ggtt_remove_node(m->gt->mem.ggtt, &m->copy_node);
}

struct xe_migrate *xe_migrate_init(struct xe_gt *gt)
{
	struct xe_hw_engine *hwe, *hwe0 = NULL;
	struct xe_device *xe = gt_to_xe(gt);
	enum xe_hw_engine_id id;
	struct xe_migrate *m;
	u32 logical_mask = 0;
	int err;

	m = drmm_kzalloc(&xe->drm, sizeof(*m), GFP_KERNEL);
	if (!m)
		return ERR_PTR(-ENOMEM);

	for_each_hw_engine (hwe, gt, id) {
		if (hwe->class == XE_ENGINE_CLASS_COPY) {
			logical_mask |= BIT(hwe->logical_instance);
			if (!hwe0)
				hwe0 = hwe;
		}
	}

	if (!logical_mask)
		return ERR_PTR(-ENODEV);

	m->gt = gt;

	err = xe_ggtt_insert_special_node(gt->mem.ggtt, &m->copy_node, 2 * CHUNK_SZ, 2 * CHUNK_SZ);
	if (err)
		return ERR_PTR(err);

	m->eng = xe_engine_create(xe, NULL, logical_mask,
				  1, hwe0, ENGINE_FLAG_KERNEL);
	if (IS_ERR(m->eng)) {
		xe_ggtt_remove_node(gt->mem.ggtt, &m->copy_node);
		return ERR_CAST(m->eng);
	}
	mutex_init(&m->job_mutex);
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

#define MAX_GGTT_UPDATE_SIZE (2 * DIV_ROUND_UP(CHUNK_SZ >> 12, 0xff) + (CHUNK_SZ >> 11))
static void emit_pte(struct xe_ggtt *ggtt, struct xe_bb *bb, u64 ggtt_ofs,
		     struct ttm_resource *res, struct xe_res_cursor *cur,
		     u32 ofs, u32 size, struct ttm_tt *ttm)
{
	u32 ptes = size >> 12;
	bool lmem = res->mem_type == TTM_PL_VRAM;

	while (ptes) {
		u32 chunk = min(0xffU, ptes);

		bb->cs[bb->len++] = MI_UPDATE_GTT | (chunk * 2);
		bb->cs[bb->len++] = ggtt_ofs;

		ofs += chunk << 12;
		ggtt_ofs += chunk << 12;
		ptes -= chunk;

		while (chunk--) {
			u64 addr;

			if (lmem) {
				addr = cur->start;
				addr |= 3;
			} else {
				u32 ofs = cur->start >> PAGE_SHIFT;

				addr = ttm->dma_address[ofs];
				addr |= 1;
			}

			bb->cs[bb->len++] = lower_32_bits(addr);
			bb->cs[bb->len++] = upper_32_bits(addr);

			xe_res_next(cur, 4096);
		}
	}
}

static void emit_flush(struct xe_bb *bb)
{
	bb->cs[bb->len++] = (MI_FLUSH_DW | MI_INVALIDATE_TLB | MI_FLUSH_DW_OP_STOREDW | MI_FLUSH_DW_STORE_INDEX) + 1;
	bb->cs[bb->len++] = LRC_PPHWSP_SCRATCH_ADDR | MI_FLUSH_DW_USE_GTT; /* lower_32_bits(addr) */
	bb->cs[bb->len++] = 0; /* upper_32_bits(addr) */
	bb->cs[bb->len++] = 0; /* value */
}

static void emit_copy(struct xe_gt *gt, struct xe_bb *bb,
		      u64 src_ofs, u64 dst_ofs, unsigned int size)
{
	bb->cs[bb->len++] = GEN9_XY_FAST_COPY_BLT_CMD | (10 - 2);
	bb->cs[bb->len++] = BLT_DEPTH_32 | PAGE_SIZE;
	bb->cs[bb->len++] = 0;
	bb->cs[bb->len++] = size >> PAGE_SHIFT << 16 | PAGE_SIZE / 4;
	bb->cs[bb->len++] = dst_ofs; /* dst offset */
	bb->cs[bb->len++] = dst_ofs >> 32ULL;
	bb->cs[bb->len++] = 0;
	bb->cs[bb->len++] = PAGE_SIZE;
	bb->cs[bb->len++] = src_ofs; /* src offset */
	bb->cs[bb->len++] = src_ofs >> 32ULL;
}

struct dma_fence *xe_migrate_copy(struct xe_migrate *m,
				  struct xe_bo *bo,
				  struct ttm_resource *src,
				  struct ttm_resource *dst)
{
	struct xe_gt *gt = m->gt;
	struct dma_fence *fence = NULL;
	u32 size = bo->size;
	u32 ofs = 0;
	u64 ggtt_copy_ofs = m->copy_node.start;
	struct xe_res_cursor src_it, dst_it;
	struct ttm_tt *ttm = bo->ttm.ttm;

	xe_res_first(src, 0, bo->size, &src_it);
	xe_res_first(dst, 0, bo->size, &dst_it);

	while (size) {
		u32 copy = min_t(u32, CHUNK_SZ, size);
		u32 batch_size = 15 + 2 * MAX_GGTT_UPDATE_SIZE;
		struct xe_sched_job *job;
		struct xe_bb *bb;
		int err;

		dma_fence_put(fence);

		bb = xe_bb_new(gt, batch_size);
		if (IS_ERR(bb))
			return ERR_CAST(bb);

		emit_arb_clear(bb);
		emit_pte(gt->mem.ggtt, bb, ggtt_copy_ofs, src, &src_it, ofs, copy, ttm);
		emit_pte(gt->mem.ggtt, bb, ggtt_copy_ofs + CHUNK_SZ, dst, &dst_it, ofs, copy, ttm);
		emit_flush(bb);
		emit_copy(gt, bb, ggtt_copy_ofs, ggtt_copy_ofs + CHUNK_SZ, copy);

		mutex_lock(&m->job_mutex);

		job = xe_bb_create_job(m->eng, bb);
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

		mutex_unlock(&m->job_mutex);

		xe_bb_free(bb, fence);
		ofs += copy;
		size -= copy;
		continue;

err_job:
		xe_sched_job_free(job);
err:
		mutex_unlock(&m->job_mutex);
		xe_bb_free(bb, NULL);
		return ERR_PTR(err);
	}

	return fence;
}

static int emit_clear(struct xe_bb *bb, u64 src_ofs, u32 size, u32 value)
{
	BUG_ON(size >> PAGE_SHIFT > S16_MAX);

	bb->cs[bb->len++] = XY_COLOR_BLT_CMD | BLT_WRITE_RGBA | (7 - 2);
	bb->cs[bb->len++] = BLT_DEPTH_32 | BLT_ROP_COLOR_COPY | PAGE_SIZE;
	bb->cs[bb->len++] = 0;
	bb->cs[bb->len++] = size >> PAGE_SHIFT << 16 | PAGE_SIZE / 4;
	bb->cs[bb->len++] = src_ofs; /* offset */
	bb->cs[bb->len++] = src_ofs >> 32ULL;
	bb->cs[bb->len++] = value;

	return 0;
}

struct dma_fence *xe_migrate_clear(struct xe_migrate *m,
				   struct xe_bo *bo,
				   u32 value)
{
	struct xe_gt *gt = m->gt;
	struct dma_fence *fence = NULL;
	u32 size = bo->size;
	u32 ofs = 0;
	u64 ggtt_copy_ofs = m->copy_node.start;
	struct xe_res_cursor src_it;
	struct ttm_resource *src = bo->ttm.resource;

	xe_res_first(src, 0, bo->size, &src_it);

	while (size) {
		u32 clear = min_t(u32, CHUNK_SZ, size);
		struct xe_sched_job *job;
		struct xe_bb *bb;
		int err;

		dma_fence_put(fence);

		bb = xe_bb_new(gt, 12 + MAX_GGTT_UPDATE_SIZE);
		if (IS_ERR(bb))
			return ERR_CAST(bb);

		/* TODO: Add dependencies here */
		emit_arb_clear(bb);
		emit_pte(gt->mem.ggtt, bb, ggtt_copy_ofs, src, &src_it, ofs, clear, bo->ttm.ttm);
		emit_flush(bb);
		emit_clear(bb, ggtt_copy_ofs, clear, value);

		mutex_lock(&m->job_mutex);
		job = xe_bb_create_job(m->eng, bb);
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
		mutex_unlock(&m->job_mutex);

		xe_bb_free(bb, fence);

		ofs += clear;
		size -= clear;
		continue;

err_job:
		xe_sched_job_free(job);
		mutex_unlock(&m->job_mutex);
err:
		xe_bb_free(bb, NULL);
		return ERR_PTR(err);
	}

	return fence;
}

static void write_pgtable(struct xe_bb *bb, u64 ggtt_ofs,
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
	do {
		chunk = min(update->qwords, 0x1ffU);

		/* Ensure populatefn can do memset64 by aligning bb->cs */
		if (!(bb->len & 1))
			bb->cs[bb->len++] = MI_NOOP;

		bb->cs[bb->len++] = MI_STORE_DATA_IMM | BIT(22) | BIT(21) | (chunk * 2 + 1);
		bb->cs[bb->len++] = lower_32_bits(ggtt_ofs + ofs * 8);
		bb->cs[bb->len++] = upper_32_bits(ggtt_ofs + ofs * 8);
		populatefn(bb->cs + bb->len, ofs, chunk, update, arg);

		bb->len += chunk * 2;
		ofs += chunk;
		size -= chunk;
	} while (size);
}

struct dma_fence *
xe_migrate_update_pgtables(struct xe_migrate *m,
			   struct xe_vm *vm,
			   struct xe_vm_pgtable_update *updates,
			   u32 num_updates,
			   struct xe_sync_entry *syncs, u32 num_syncs,
			   xe_migrate_populatefn_t populatefn, void *arg)
{
	struct xe_gt *gt = m->gt;
	struct xe_sched_job *job;
	struct dma_fence *fence;
	struct xe_bb *bb;
	u32 i, batch_size;
	int err = 0;

	/* fixed + PTE entries */
	batch_size = 7;

	for (i = 0; i < num_updates; i++) {
		u32 num_cmds = DIV_ROUND_UP(updates[i].qwords, 0x1ff);

		batch_size += 2;
		/* align noops + MI_STORE_DATA_IMM cmd prefix */
		batch_size += 2 + 4 * num_cmds + updates[i].qwords * 2;
	}

	/*
	 * XXX: Create temp bo to copy from, if batch_size becomes too big?
	 * 1GiB bo would need upwards of ~512KiB of updates. Or just allow huge
	 * pages..
	 */
	XE_BUG_ON(batch_size >= SZ_128K);

	bb = xe_bb_new(gt, batch_size);
	if (IS_ERR(bb))
		return ERR_CAST(bb);

	emit_arb_clear(bb);

	/* Map our PT's to gtt */
	bb->cs[bb->len++] = MI_UPDATE_GTT | (num_updates * 2);
	bb->cs[bb->len++] = m->copy_node.start;

	for (i = 0; i < num_updates; i++) {
		struct xe_bo *bo = updates[i].pt_bo;
		u64 addr;

		BUG_ON(bo->size != SZ_4K);

		if (bo->ttm.resource->mem_type == TTM_PL_VRAM) {
			struct xe_res_cursor src_it;

			xe_res_first(bo->ttm.resource, 0, bo->size, &src_it);
			addr = src_it.start | 3;
		} else {
			addr = bo->ttm.ttm->dma_address[0] | 1;
		}

		bb->cs[bb->len++] = lower_32_bits(addr);
		bb->cs[bb->len++] = upper_32_bits(addr);
	}

	emit_flush(bb);

	for (i = 0; i < num_updates; i++) {
		u64 ggtt_ofs;

		ggtt_ofs = m->copy_node.start + SZ_4K * i;

		write_pgtable(bb, ggtt_ofs, &updates[i], populatefn, arg);
	}

	mutex_lock(&m->job_mutex);
	job = xe_bb_create_job(m->eng, bb);
	if (IS_ERR(job)) {
		err = PTR_ERR(job);
		goto err;
	}

	if (vm) {
		err = drm_sched_job_add_implicit_dependencies_resv(&job->drm,
								   &vm->resv,
								   true);
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
	mutex_unlock(&m->job_mutex);

	for (i = 0; i < num_syncs; i++)
		xe_sync_entry_signal(&syncs[i], job, fence);

	xe_bb_free(bb, fence);

	return fence;

err_job:
	xe_sched_job_free(job);
err:
	mutex_unlock(&m->job_mutex);
	xe_bb_free(bb, NULL);
	return ERR_PTR(err);
}
