// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_wa.h"

#include "xe_platform_types.h"
#include "xe_gt_types.h"
#include "xe_rtp.h"

#include "../i915/gt/intel_gt_regs.h"

/** DOC: Hardware tuning
 *
 * Hardware tuning are register programming recommendations, usually for
 * performance. They are part of the programming guide for a given platform.
 * In general, its programming is very similar to the Hardware workarounds,
 * however, they are not part of the workaround database and they won't
 * have any locator number associated with it.
 */

static const struct xe_rtp_entry gt_tunings[] = {
	{ XE_RTP_NAME("Tuning: 32B Access Enable"),
	  XE_RTP_RULES(PLATFORM(DG2)),
	  XE_RTP_SET(XEHP_SQCM, EN_32B_ACCESS)
	},
	{}
};

/**
 * xe_tuning_process_gt - process GT tuning
 * @gt: Xe GT instance
 *
 * Process Intel GT tuning register programming.
 */
void xe_tuning_process_gt(struct xe_gt *gt)
{
	xe_rtp_process(gt_tunings, &gt->reg_sr, gt, NULL);
}
