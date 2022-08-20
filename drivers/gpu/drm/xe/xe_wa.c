// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_wa.h"

#include <linux/compiler_types.h>

#include "xe_device_types.h"
#include "xe_hw_engine_types.h"
#include "xe_gt.h"
#include "xe_hw_engine_types.h"
#include "xe_platform_types.h"
#include "xe_rtp.h"
#include "xe_step.h"

#include "../i915/gt/intel_engine_regs.h"
#include "../i915/gt/intel_gt_regs.h"

/* TODO:
 * - whitelist
 * - steering:  we probably want that separate, and xe_wa.c only cares about the
 *   value to be added to the table
 * - apply workarounds with and without guc
 * - move tables to single compilation units? or single elf section?
 */

static bool match_14011060649(const struct xe_gt *gt,
			      const struct xe_hw_engine *hwe)
{
	const struct xe_device *xe = gt_to_xe((struct xe_gt *)gt);

	return MEDIA_VER(xe) == 12 && hwe->instance % 2 == 0;
}

static const struct xe_rtp_entry gt_was[] = {
	{ XE_RTP_NAME("14011060649"),
	  XE_RTP_RULES(ENGINE_CLASS(VIDEO_DECODE),
		       FUNC(match_14011060649)),
	  XE_RTP_SET(VDBOX_CGCTL3F10(0), IECPUNIT_CLKGATE_DIS,
		     XE_RTP_FLAG(FOREACH_ENGINE))
	},
	{ XE_RTP_NAME("16010515920"),
	  XE_RTP_RULES(SUBPLATFORM(DG2, G10),
		       STEP(A0, B0),
		       ENGINE_CLASS(VIDEO_DECODE)),
	  XE_RTP_SET(VDBOX_CGCTL3F18(0), ALNUNIT_CLKGATE_DIS,
		     XE_RTP_FLAG(FOREACH_ENGINE))
	},
	{ XE_RTP_NAME("22010523718"),
	  XE_RTP_RULES(SUBPLATFORM(DG2, G10)),
	  XE_RTP_SET(UNSLICE_UNIT_LEVEL_CLKGATE, CG3DDISCFEG_CLKGATE_DIS)
	},
	{ XE_RTP_NAME("14011006942"),
	  XE_RTP_RULES(SUBPLATFORM(DG2, G10)),
	  XE_RTP_SET(SUBSLICE_UNIT_LEVEL_CLKGATE, DSS_ROUTER_CLKGATE_DIS)
	},
	{ XE_RTP_NAME("14010948348"),
	  XE_RTP_RULES(SUBPLATFORM(DG2, G10), STEP(A0, B0)),
	  XE_RTP_SET(UNSLCGCTL9430, MSQDUNIT_CLKGATE_DIS)
	},
	{ XE_RTP_NAME("14011037102"),
	  XE_RTP_RULES(SUBPLATFORM(DG2, G10), STEP(A0, B0)),
	  XE_RTP_SET(UNSLCGCTL9444, LTCDD_CLKGATE_DIS)
	},
	{ XE_RTP_NAME("14011371254"),
	  XE_RTP_RULES(SUBPLATFORM(DG2, G10), STEP(A0, B0)),
	  XE_RTP_SET(SLICE_UNIT_LEVEL_CLKGATE, NODEDSS_CLKGATE_DIS)
	},
	{ XE_RTP_NAME("14011431319/0"),
	  XE_RTP_RULES(SUBPLATFORM(DG2, G10), STEP(A0, B0)),
	  XE_RTP_SET(UNSLCGCTL9440,
		     GAMTLBOACS_CLKGATE_DIS |
		     GAMTLBVDBOX7_CLKGATE_DIS | GAMTLBVDBOX6_CLKGATE_DIS |
		     GAMTLBVDBOX5_CLKGATE_DIS | GAMTLBVDBOX4_CLKGATE_DIS |
		     GAMTLBVDBOX3_CLKGATE_DIS | GAMTLBVDBOX2_CLKGATE_DIS |
		     GAMTLBVDBOX1_CLKGATE_DIS | GAMTLBVDBOX0_CLKGATE_DIS |
		     GAMTLBKCR_CLKGATE_DIS | GAMTLBGUC_CLKGATE_DIS |
		     GAMTLBBLT_CLKGATE_DIS)
	},
	{ XE_RTP_NAME("14011431319/1"),
	  XE_RTP_RULES(SUBPLATFORM(DG2, G10), STEP(A0, B0)),
	  XE_RTP_SET(UNSLCGCTL9444,
		     GAMTLBGFXA0_CLKGATE_DIS | GAMTLBGFXA1_CLKGATE_DIS |
		     GAMTLBCOMPA0_CLKGATE_DIS | GAMTLBCOMPA1_CLKGATE_DIS |
		     GAMTLBCOMPB0_CLKGATE_DIS | GAMTLBCOMPB1_CLKGATE_DIS |
		     GAMTLBCOMPC0_CLKGATE_DIS | GAMTLBCOMPC1_CLKGATE_DIS |
		     GAMTLBCOMPD0_CLKGATE_DIS | GAMTLBCOMPD1_CLKGATE_DIS |
		     GAMTLBMERT_CLKGATE_DIS |
		     GAMTLBVEBOX3_CLKGATE_DIS | GAMTLBVEBOX2_CLKGATE_DIS |
		     GAMTLBVEBOX1_CLKGATE_DIS | GAMTLBVEBOX0_CLKGATE_DIS)
	},
	{ XE_RTP_NAME("14010569222"),
	  XE_RTP_RULES(SUBPLATFORM(DG2, G10), STEP(A0, B0)),
	  XE_RTP_SET(UNSLICE_UNIT_LEVEL_CLKGATE, GAMEDIA_CLKGATE_DIS)
	},
	{ XE_RTP_NAME("14011028019"),
	  XE_RTP_RULES(SUBPLATFORM(DG2, G10), STEP(A0, B0)),
	  XE_RTP_SET(SSMCGCTL9530, RTFUNIT_CLKGATE_DIS)
	},
	{ XE_RTP_NAME("14014830051"),
	  XE_RTP_RULES(PLATFORM(DG2)),
	  XE_RTP_CLR(SARB_CHICKEN1, COMP_CKN_IN)
	},
	{ XE_RTP_NAME("14015795083"),
	  XE_RTP_RULES(PLATFORM(DG2)),
	  XE_RTP_CLR(GEN7_MISCCPCTL, GEN12_DOP_CLOCK_GATE_RENDER_ENABLE)
	},
	{}
};

static const struct xe_rtp_entry engine_was[] = {
	{ XE_RTP_NAME("14015227452"),
	  XE_RTP_RULES(PLATFORM(DG2), ENGINE_CLASS(RENDER)),
	  XE_RTP_SET(GEN9_ROW_CHICKEN4, XEHP_DIS_BBL_SYSPIPE,
		     XE_RTP_FLAG(MASKED_REG))
	},
	{}
};

void xe_wa_process_gt(struct xe_gt *gt)
{
	xe_rtp_process(gt_was, &gt->reg_sr, gt, NULL);
}

void xe_wa_process_engine(struct xe_hw_engine *hwe)
{
	xe_rtp_process(engine_was, &hwe->reg_sr, hwe->gt, hwe);
}

void xe_wa_process_ctx(struct xe_hw_engine *hwe)
{
	//xe_rtp_process(engine_was, &hwe->reg_sr, gt, hwe);
}
