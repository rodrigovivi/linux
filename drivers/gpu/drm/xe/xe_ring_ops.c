// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_engine_types.h"
#include "xe_gt.h"
#include "xe_lrc.h"
#include "xe_macros.h"
#include "xe_ring_ops.h"
#include "xe_sched_job.h"
#include "xe_vm_types.h"

#include "../i915/i915_reg.h"
#include "../i915/gt/intel_gpu_commands.h"
#include "../i915/gt/intel_gt_regs.h"
#include "../i915/gt/intel_lrc_reg.h"

#define PIPE_CONTROL_RENDER_ONLY_FLAGS (\
		PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH | \
		PIPE_CONTROL_DEPTH_CACHE_FLUSH | \
		PIPE_CONTROL_TILE_CACHE_FLUSH | \
		PIPE_CONTROL_DEPTH_STALL | \
		PIPE_CONTROL_STALL_AT_SCOREBOARD | \
		PIPE_CONTROL_VF_CACHE_INVALIDATE)


static void invalidate_tlb(struct xe_sched_job *job, u32 *dw, u32 *pi)
{
	u32 i = *pi;

	if (job->engine->class != XE_ENGINE_CLASS_RENDER) {
		dw[i++] = MI_ARB_CHECK | BIT(8) | BIT(0);

		dw[i] = MI_FLUSH_DW + 1;
		if (job->engine->class == XE_ENGINE_CLASS_VIDEO_DECODE)
			dw[i] |= MI_INVALIDATE_BSD;
		dw[i++] |= MI_INVALIDATE_TLB | MI_FLUSH_DW_OP_STOREDW |
			MI_FLUSH_DW_STORE_INDEX;

		dw[i++] = LRC_PPHWSP_SCRATCH_ADDR | MI_FLUSH_DW_USE_GTT;
		dw[i++] = 0;
		dw[i++] = ~0U;

		dw[i++] = MI_ARB_CHECK | BIT(8);
	} else {
		u32 flags = PIPE_CONTROL_CS_STALL |
			PIPE_CONTROL_COMMAND_CACHE_INVALIDATE |
			PIPE_CONTROL_TLB_INVALIDATE |
			PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE |
			PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE |
			PIPE_CONTROL_VF_CACHE_INVALIDATE |
			PIPE_CONTROL_CONST_CACHE_INVALIDATE |
			PIPE_CONTROL_STATE_CACHE_INVALIDATE |
			PIPE_CONTROL_QW_WRITE |
			PIPE_CONTROL_STORE_DATA_INDEX;

		if (job->engine->class == XE_ENGINE_CLASS_COMPUTE)
			flags &= ~PIPE_CONTROL_RENDER_ONLY_FLAGS;

		dw[i++] = MI_ARB_CHECK | BIT(8) | BIT(0);

		dw[i++] = GFX_OP_PIPE_CONTROL(6);
		dw[i++] = flags;
		dw[i++] = LRC_PPHWSP_SCRATCH_ADDR;
		dw[i++] = 0;
		dw[i++] = 0;
		dw[i++] = 0;

		dw[i++] = MI_LOAD_REGISTER_IMM(1);
		dw[i++] = GEN12_GFX_CCS_AUX_NV.reg;
		dw[i++] = AUX_INV;
		dw[i++] = MI_NOOP;

		dw[i++] = MI_ARB_CHECK | BIT(8);
	}

	*pi = i;
}

#define MI_STORE_QWORD_IMM_GEN8_POSTED (MI_INSTR(0x20, 3) | (1 << 21))

static void __emit_job_gen12(struct xe_sched_job *job, struct xe_lrc *lrc,
			     u64 batch_addr, u32 seqno)
{
	uint32_t dw[MAX_JOB_SIZE_DW], i = 0;
	u32 ppgtt_flag = job->engine->vm ? BIT(8) : 0;

#if 1
	// TODO: Find a way to make flushing conditional?
	// if (job->engine->vm && (last_vm_unbind_scheduled || !last_vm_unbind_completed))
	invalidate_tlb(job, dw, &i);
#endif

	dw[i++] = MI_STORE_DATA_IMM | BIT(22) /* GGTT */ | 2;
	dw[i++] = xe_lrc_start_seqno_ggtt_addr(lrc);
	dw[i++] = 0;
	dw[i++] = seqno;

	dw[i++] = MI_BATCH_BUFFER_START_GEN8 | ppgtt_flag;
	dw[i++] = lower_32_bits(batch_addr);
	dw[i++] = upper_32_bits(batch_addr);

	if (job->user_fence.used) {
		dw[i++] = MI_STORE_QWORD_IMM_GEN8_POSTED;
		dw[i++] = lower_32_bits(job->user_fence.addr);
		dw[i++] = upper_32_bits(job->user_fence.addr);
		dw[i++] = lower_32_bits(job->user_fence.value);
		dw[i++] = upper_32_bits(job->user_fence.value);
	}

	dw[i++] = MI_STORE_DATA_IMM | BIT(22) /* GGTT */ | 2;
	dw[i++] = xe_lrc_seqno_ggtt_addr(lrc);
	dw[i++] = 0;
	dw[i++] = seqno;

	dw[i++] = MI_USER_INTERRUPT;
	dw[i++] = MI_ARB_ON_OFF | MI_ARB_ENABLE;
	dw[i++] = MI_ARB_CHECK;

	XE_BUG_ON(i > MAX_JOB_SIZE_DW);

	xe_lrc_write_ring(lrc, dw, i * sizeof(*dw));
}

static void emit_migration_job_gen12(struct xe_sched_job *job,
				     struct xe_lrc *lrc, u32 seqno)
{
	u32 dw[MAX_JOB_SIZE_DW], i = 0;
	u64 batch_addr;

	dw[i++] = MI_STORE_DATA_IMM | BIT(22) /* GGTT */ | 2;
	dw[i++] = xe_lrc_start_seqno_ggtt_addr(lrc);
	dw[i++] = 0;
	dw[i++] = seqno;

	batch_addr = job->batch_addr[0];
	dw[i++] = MI_BATCH_BUFFER_START_GEN8 | BIT(8);
	dw[i++] = lower_32_bits(batch_addr);
	dw[i++] = upper_32_bits(batch_addr);

	invalidate_tlb(job, dw, &i);

	batch_addr = job->batch_addr[1];
	dw[i++] = MI_BATCH_BUFFER_START_GEN8 | BIT(8);
	dw[i++] = lower_32_bits(batch_addr);
	dw[i++] = upper_32_bits(batch_addr);

	dw[i++] = (MI_FLUSH_DW | MI_INVALIDATE_TLB | MI_FLUSH_DW_OP_STOREDW) + 1;
	dw[i++] = xe_lrc_seqno_ggtt_addr(lrc) | MI_FLUSH_DW_USE_GTT;
	dw[i++] = 0;
	dw[i++] = seqno; /* value */

	dw[i++] = MI_USER_INTERRUPT;
	dw[i++] = MI_ARB_ON_OFF | MI_ARB_ENABLE;
	dw[i++] = MI_ARB_CHECK;

	XE_BUG_ON(i > MAX_JOB_SIZE_DW);

	xe_lrc_write_ring(lrc, dw, i * sizeof(*dw));
}

static void emit_job_gen12(struct xe_sched_job *job)
{
	int i;

	if (job->engine->vm && job->engine->vm->flags & XE_VM_FLAG_MIGRATION) {
		emit_migration_job_gen12(job, job->engine->lrc, xe_sched_job_seqno(job));
		return;
	}

	/* FIXME: Not doing parallel handshake for now */
	for (i = 0; i < job->engine->width; ++i)
		__emit_job_gen12(job, job->engine->lrc + i, job->batch_addr[i],
				 xe_sched_job_seqno(job));
}

static const struct xe_ring_ops ring_ops_gen12 = {
	.emit_job = emit_job_gen12,
};

const struct xe_ring_ops *
xe_ring_ops_get(struct xe_gt *gt, enum xe_engine_class class)
{
	return &ring_ops_gen12;
}
