// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023-2025 Intel Corporation
 */

#include "xe_eudebug_hw.h"

#include <generated/xe_wa_oob.h>

#include "regs/xe_gt_regs.h"

#include "xe_device.h"
#include "xe_gt.h"
#include "xe_hw_engine_types.h"
#include "xe_reg_sr.h"
#include "xe_rtp.h"
#include "xe_wa.h"

static void add_sr_entry(struct xe_hw_engine *hwe,
			 struct xe_reg_mcr mcr_reg,
			 u32 mask, bool enable)
{
	const struct xe_reg_sr_entry sr_entry = {
		.reg = mcr_reg.__reg,
		.clr_bits = mask,
		.set_bits = enable ? mask : 0,
		.read_mask = mask,
	};

	xe_reg_sr_add(&hwe->reg_sr, &sr_entry, hwe->gt, true);
}

void xe_eudebug_init_hw_engine(struct xe_hw_engine *hwe, bool enable)
{
	struct xe_gt *gt = hwe->gt;
	struct xe_device *xe = gt_to_xe(gt);

	if (!xe_rtp_match_first_render_or_compute(xe, gt, hwe))
		return;

	if (XE_GT_WA(gt, 18022722726))
		add_sr_entry(hwe, ROW_CHICKEN,
			     STALL_DOP_GATING_DISABLE, enable);

	if (XE_GT_WA(gt, 14015474168))
		add_sr_entry(hwe, ROW_CHICKEN2,
			     XEHPC_DISABLE_BTB,
			     enable);

	if (xe->info.graphics_verx100 >= 1200)
		add_sr_entry(hwe, TD_CTL,
			     TD_CTL_BREAKPOINT_ENABLE |
			     TD_CTL_FORCE_THREAD_BREAKPOINT_ENABLE |
			     TD_CTL_FEH_AND_FEE_ENABLE,
			     enable);

	if (xe->info.graphics_verx100 >= 1250)
		add_sr_entry(hwe, TD_CTL,
			     TD_CTL_GLOBAL_DEBUG_ENABLE, enable);
}

