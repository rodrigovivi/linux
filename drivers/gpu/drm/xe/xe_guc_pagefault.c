// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <drm/ttm/ttm_execbuf_util.h>

#include "xe_bo.h"
#include "xe_gt.h"
#include "xe_guc.h"
#include "xe_guc_ct.h"
#include "xe_guc_pagefault.h"
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

static struct xe_device *
guc_to_xe(struct xe_guc *guc)
{
	return gt_to_xe(guc_to_gt(guc));
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

	/* FIXME: Not handling G2H credits */
	return xe_guc_ct_send_g2h_handler(&guc->ct, action, ARRAY_SIZE(action));
}

static int handle_pagefault(struct xe_guc *guc, struct pagefault *pf)
{
	struct xe_device *xe = guc_to_xe(guc);
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
	ret = send_tlb_invalidate(guc);

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

	return xe_guc_ct_send_g2h_handler(&guc->ct, action, ARRAY_SIZE(action));
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

static void get_pagefault(const u32 *msg, struct pagefault *pf)
{
	const struct xe_guc_pagefault_desc *desc;

	desc = (const struct xe_guc_pagefault_desc *)msg;
	pf->fault_level = FIELD_GET(PFD_FAULT_LEVEL, desc->dw0);
	pf->engine_class = FIELD_GET(PFD_ENG_CLASS, desc->dw0);
	pf->engine_instance = FIELD_GET(PFD_ENG_INSTANCE, desc->dw0);
	pf->pdata = FIELD_GET(PFD_PDATA_HI, desc->dw1) << PFD_PDATA_HI_SHIFT;
	pf->pdata |= FIELD_GET(PFD_PDATA_LO, desc->dw0);
	pf->asid = FIELD_GET(PFD_ASID, desc->dw1);
	pf->vfid = FIELD_GET(PFD_VFID, desc->dw2);
	pf->access_type = FIELD_GET(PFD_ACCESS_TYPE, desc->dw2);
	pf->fault_type = FIELD_GET(PFD_FAULT_TYPE, desc->dw2);
	pf->page_addr = (u64)(FIELD_GET(PFD_VIRTUAL_ADDR_HI, desc->dw3)) <<
		PFD_VIRTUAL_ADDR_HI_SHIFT;
	pf->page_addr |= FIELD_GET(PFD_VIRTUAL_ADDR_LO, desc->dw2) <<
		PFD_VIRTUAL_ADDR_LO_SHIFT;
}

int xe_guc_pagefault_handler(struct xe_guc *guc, u32 *msg, u32 len)
{
	struct xe_device *xe = guc_to_xe(guc);
	struct xe_guc_pagefault_reply reply = {};
	struct pagefault pf = {};
	int ret;

	if (unlikely(len != 4))
		return -EPROTO;

	get_pagefault(msg, &pf);

	ret = handle_pagefault(guc, &pf);
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

	return send_pagefault_reply(guc, &reply);
}
