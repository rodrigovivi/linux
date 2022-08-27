// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <linux/circ_buf.h>

#include <drm/ttm/ttm_execbuf_util.h>

#include "xe_bo.h"
#include "xe_gt.h"
#include "xe_guc.h"
#include "xe_guc_ct.h"
#include "xe_gt_pagefault.h"
#include "xe_trace.h"
#include "xe_vm.h"

struct pagefault {
	u64 page_addr;
	u32 asid;
	u16 pdata;
	u8 vfid;
	u8 access_type;
	u8 fault_type;
	u8 fault_level;
	u8 engine_class;
	u8 engine_instance;
	u8 fault_unsuccessful;
};

static struct xe_gt *
guc_to_gt(struct xe_guc *guc)
{
	return container_of(guc, struct xe_gt, uc.guc);
}

static int send_tlb_invalidate(struct xe_guc *guc)
{
	u32 action[] = {
		XE_GUC_ACTION_TLB_INVALIDATION,
		0,
		XE_GUC_TLB_INVAL_FULL << XE_GUC_TLB_INVAL_TYPE_SHIFT |
		XE_GUC_TLB_INVAL_MODE_HEAVY << XE_GUC_TLB_INVAL_MODE_SHIFT |
		XE_GUC_TLB_INVAL_FLUSH_CACHE,
	};

	return xe_guc_ct_send(&guc->ct, action, ARRAY_SIZE(action),
			      G2H_LEN_DW_TLB_INVALIDATE, 1);
}

static int handle_pagefault(struct xe_gt *gt, struct pagefault *pf)
{
	struct xe_device *xe = gt_to_xe(gt);
	struct xe_vm *vm;
	struct xe_vma *vma, lookup;
	LIST_HEAD(objs);
	LIST_HEAD(dups);
	struct ttm_validate_buffer tv_bo, tv_vm;
	struct ww_acquire_ctx ww;
	struct dma_fence *fence;
	int ret = 0;

	/* ASID to VM */
	mutex_lock(&xe->usm.lock);
	vm = xa_load(&xe->usm.asid_to_vm, pf->asid);
	if (vm)
		xe_vm_get(vm);
	mutex_unlock(&xe->usm.lock);
	if (!vm)
		return -EINVAL;

	down_read(&vm->lock);

	/* Lookup VMA */
	lookup.start = pf->page_addr;
	lookup.end = lookup.start + SZ_4K - 1;
	vma = xe_vm_find_overlapping_vma(vm, &lookup);
	if (!vma) {
		ret = -EINVAL;
		goto unlock_vm;
	}
	trace_xe_vma_pagefault(vma);

	/* TODO: Check for Already bound */
	XE_WARN_ON(!vma->bo);	/* TODO: userptr */

	/* Lock VM and BOs dma-resv */
	tv_vm.num_shared = xe->info.tile_count;
	tv_vm.bo = xe_vm_ttm_bo(vm);
	list_add(&tv_vm.head, &objs);
	if (vma->bo) {
		tv_bo.bo = &vma->bo->ttm;
		tv_bo.num_shared = xe->info.tile_count;
		list_add(&tv_bo.head, &objs);
	}
	ret = ttm_eu_reserve_buffers(&ww, &objs, false, &dups);
	if (ret)
		goto unlock_vm;

	/* Create backing store if needed */
	if (vma->bo) {
		ret = xe_bo_validate(vma->bo, vm);
		if (ret)
			goto unlock_dma_resv;
	}

	/*
	 * Bind VMA
	 *
	 * XXX: For multi-GT we will bind to both GTs, fixup to only bind to the
	 * GT which took the fault.
	 */
	fence = xe_vm_bind_vma(vma, NULL, NULL, 0);
	if (IS_ERR(fence)) {
		ret = PTR_ERR(fence);
		goto unlock_dma_resv;
	}
	dma_fence_wait(fence, false);

	/* FIXME: Doing a full TLB invalidation for now */
	ret = send_tlb_invalidate(&gt->uc.guc);

unlock_dma_resv:
	ttm_eu_backoff_reservation(&ww, &objs);
unlock_vm:
	up_read(&vm->lock);
	xe_vm_put(vm);

	return ret;
}

static int send_pagefault_reply(struct xe_guc *guc,
				struct xe_guc_pagefault_reply *reply)
{
	u32 action[] = {
		XE_GUC_ACTION_PAGE_FAULT_RES_DESC,
		reply->dw0,
		reply->dw1,
	};

	return xe_guc_ct_send(&guc->ct, action, ARRAY_SIZE(action), 0, 0);
}

static void print_pagefault(struct xe_device *xe, struct pagefault *pf)
{
	drm_warn(&xe->drm, "\n\tASID: %d\n"
		 "\tVFID: %d\n"
		 "\tPDATA: 0x%04x\n"
		 "\tFaulted Address: 0x%08x%08x\n"
		 "\tFaultType: %d\n"
		 "\tAccessType: %d\n"
		 "\tFaultLevel: %d\n"
		 "\tEngineClass: %d\n"
		 "\tEngineInstance: %d\n",
		 pf->asid, pf->vfid, pf->pdata, upper_32_bits(pf->page_addr),
		 lower_32_bits(pf->page_addr),
		 pf->fault_type, pf->access_type, pf->fault_level,
		 pf->engine_class, pf->engine_instance);
}

#define PF_MSG_LEN_DW	4

static int get_pagefault(struct pf_queue *pf_queue, struct pagefault *pf)
{
	const struct xe_guc_pagefault_desc *desc;
	int ret = 0;

	spin_lock(&pf_queue->lock);
	if (pf_queue->head != pf_queue->tail) {
		desc = (const struct xe_guc_pagefault_desc *)
			(pf_queue->data + pf_queue->head);

		pf->fault_level = FIELD_GET(PFD_FAULT_LEVEL, desc->dw0);
		pf->engine_class = FIELD_GET(PFD_ENG_CLASS, desc->dw0);
		pf->engine_instance = FIELD_GET(PFD_ENG_INSTANCE, desc->dw0);
		pf->pdata = FIELD_GET(PFD_PDATA_HI, desc->dw1) <<
			PFD_PDATA_HI_SHIFT;
		pf->pdata |= FIELD_GET(PFD_PDATA_LO, desc->dw0);
		pf->asid = FIELD_GET(PFD_ASID, desc->dw1);
		pf->vfid = FIELD_GET(PFD_VFID, desc->dw2);
		pf->access_type = FIELD_GET(PFD_ACCESS_TYPE, desc->dw2);
		pf->fault_type = FIELD_GET(PFD_FAULT_TYPE, desc->dw2);
		pf->page_addr = (u64)(FIELD_GET(PFD_VIRTUAL_ADDR_HI, desc->dw3)) <<
			PFD_VIRTUAL_ADDR_HI_SHIFT;
		pf->page_addr |= FIELD_GET(PFD_VIRTUAL_ADDR_LO, desc->dw2) <<
			PFD_VIRTUAL_ADDR_LO_SHIFT;

		pf_queue->head = (pf_queue->head + PF_MSG_LEN_DW) %
			PF_QUEUE_NUM_DW;
	} else {
		ret = -1;
	}
	spin_unlock(&pf_queue->lock);

	return ret;
}

static bool pf_queue_full(struct pf_queue *pf_queue)
{
	lockdep_assert_held(&pf_queue->lock);

	return CIRC_SPACE(pf_queue->tail, pf_queue->head, PF_QUEUE_NUM_DW) <=
		PF_MSG_LEN_DW;
}

int xe_guc_pagefault_handler(struct xe_guc *guc, u32 *msg, u32 len)
{
	struct xe_gt *gt = guc_to_gt(guc);
	struct pf_queue *pf_queue;
	u32 asid;
	bool full;

	if (unlikely(len != PF_MSG_LEN_DW))
		return -EPROTO;

	asid = FIELD_GET(PFD_ASID, msg[1]);
	pf_queue = &gt->usm.pf_queue[asid % NUM_PF_QUEUE];

	spin_lock(&pf_queue->lock);
	full = pf_queue_full(pf_queue);
	if (!full) {
		memcpy(pf_queue->data + pf_queue->tail, msg, len * sizeof(u32));
		pf_queue->tail = (pf_queue->tail + len) % PF_QUEUE_NUM_DW;
		queue_work(system_unbound_wq, &pf_queue->worker);
	} else {
		XE_WARN_ON("PF Queue full, shouldn't be possible");
	}
	spin_unlock(&pf_queue->lock);

	return full ? -ENOSPC : 0;
}

static void pf_queue_work_func(struct work_struct *w)
{
	struct pf_queue *pf_queue = container_of(w, struct pf_queue, worker);
	struct xe_gt *gt = pf_queue->gt;
	struct xe_device *xe = gt_to_xe(gt);
	struct xe_guc_pagefault_reply reply = {};
	struct pagefault pf = {};
	int ret;

	ret = get_pagefault(pf_queue, &pf);
	if (ret)
		return;

	ret = handle_pagefault(gt, &pf);
	if (unlikely(ret)) {
		print_pagefault(xe, &pf);
		pf.fault_unsuccessful = 1;
		drm_warn(&xe->drm, "Fault response: Unsuccessful %d\n", ret);
	}

	reply.dw0 = FIELD_PREP(PFR_VALID, 1) |
		FIELD_PREP(PFR_SUCCESS, pf.fault_unsuccessful) |
		FIELD_PREP(PFR_REPLY, PFR_ACCESS) |
		FIELD_PREP(PFR_DESC_TYPE, FAULT_RESPONSE_DESC) |
		FIELD_PREP(PFR_ASID, pf.asid);

	reply.dw1 = FIELD_PREP(PFR_VFID, pf.vfid) |
		FIELD_PREP(PFR_ENG_INSTANCE, pf.engine_instance) |
		FIELD_PREP(PFR_ENG_CLASS, pf.engine_class) |
		FIELD_PREP(PFR_PDATA, pf.pdata);

	send_pagefault_reply(&gt->uc.guc, &reply);
}

void xe_gt_pagefault_init(struct xe_gt *gt)
{
	struct xe_device *xe = gt_to_xe(gt);
	int i;

	if (!xe->info.supports_usm)
		return;

	for (i = 0; i < NUM_PF_QUEUE; ++i) {
		gt->usm.pf_queue[i].gt = gt;
		spin_lock_init(&gt->usm.pf_queue[i].lock);
		INIT_WORK(&gt->usm.pf_queue[i].worker, pf_queue_work_func);
	}
}

void xe_gt_pagefault_reset(struct xe_gt *gt)
{
	struct xe_device *xe = gt_to_xe(gt);
	int i;

	if (!xe->info.supports_usm)
		return;

	for (i = 0; i < NUM_PF_QUEUE; ++i) {
		spin_lock(&gt->usm.pf_queue[i].lock);
		gt->usm.pf_queue[i].head = 0;
		gt->usm.pf_queue[i].tail = 0;
		spin_unlock(&gt->usm.pf_queue[i].lock);
	}
}
