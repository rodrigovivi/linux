/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#include "xe_bb.h"
#include "xe_sa.h"
#include "xe_device.h"
#include "xe_engine_types.h"
#include "xe_hw_fence.h"
#include "xe_sched_job.h"
#include "xe_vm_types.h"

#include "../i915/gt/intel_gpu_commands.h"

struct xe_bb *xe_bb_new(struct xe_gt *gt, u32 dwords)
{
	struct xe_bb *bb = kmalloc(sizeof(*bb), GFP_KERNEL);
	int err;

	if (!bb)
		return ERR_PTR(-ENOMEM);

	bb->bo = xe_sa_bo_new(&gt->kernel_bb_pool, 4 * dwords + 4);
	if (IS_ERR(bb->bo)) {
		err = PTR_ERR(bb->bo);
		goto err;
	}

	bb->cs = xe_sa_bo_cpu_addr(bb->bo);
	bb->len = 0;

	return bb;
err:
	kfree(bb);
	return ERR_PTR(err);
}

static struct xe_sched_job *
__xe_bb_create_job(struct xe_engine *kernel_eng, struct xe_bb *bb, u64 *addr)
{
	u32 size = bb->bo->eoffset - bb->bo->soffset;

	BUG_ON(bb->len >= size/4 - 1);

	bb->cs[bb->len++] = MI_BATCH_BUFFER_END;

	xe_sa_bo_flush_write(bb->bo);

	return xe_sched_job_create(kernel_eng, addr);
}

struct xe_sched_job *xe_bb_create_migration_job(struct xe_engine *kernel_eng, struct xe_bb *bb,
						u64 batch_base_ofs, u32 second_idx)
{
	u64 addr[2] = {
		batch_base_ofs + bb->bo->soffset,
		batch_base_ofs + bb->bo->soffset + 4 * second_idx,
	};

	BUG_ON(second_idx > bb->len);
	BUG_ON(!(kernel_eng->vm->flags & XE_VM_FLAG_MIGRATION));

	return __xe_bb_create_job(kernel_eng, bb, addr);
}

struct xe_sched_job *xe_bb_create_job(struct xe_engine *kernel_eng, struct xe_bb *bb)
{
	u64 addr = xe_sa_bo_gpu_addr(bb->bo);

	BUG_ON(kernel_eng->vm && kernel_eng->vm->flags & XE_VM_FLAG_MIGRATION);
	return __xe_bb_create_job(kernel_eng, bb, &addr);
}

void xe_bb_free(struct xe_bb *bb, struct dma_fence *fence)
{
	if (!bb)
		return;

	xe_sa_bo_free(bb->bo, fence, -1);
	kfree(bb);
}
