/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_RTP_TYPES_
#define _XE_RTP_TYPES_

#include <linux/types.h>

#include "i915_reg_defs.h"

struct xe_hw_engine;
struct xe_gt;

struct xe_rtp_regval {
	i915_reg_t	reg;
	/*
	 * TODO: maybe we need a union here with a func pointer for cases
	 * that are too specific to be generalized
	 */
	u32		clr_bits;
	u32		set_bits;
	/* Mask for bits to consider when reading value back */
#define XE_RTP_NOCHECK		.read_mask = 0
	u32		read_mask;
#define XE_RTP_FLAG_FOREACH_ENGINE	BIT(0)
#define XE_RTP_FLAG_MASKED_REG		BIT(1)
#define XE_RTP_FLAG_ENGINE_BASE		BIT(2)
	u8		flags;
};

enum {
	XE_RTP_MATCH_PLATFORM,
	XE_RTP_MATCH_SUBPLATFORM,
	XE_RTP_MATCH_VERSION,
	XE_RTP_MATCH_STEP,
	XE_RTP_MATCH_ENGINE_CLASS,
	XE_RTP_MATCH_NOT_ENGINE_CLASS,
	XE_RTP_MATCH_FUNC,
};

struct xe_rtp_rule {
	u8 match_type;

	/* match filters */
	union {
		/* MATCH_PLATFORM / MATCH_SUBPLATFORM */
		struct {
			u8 platform;
			u8 subplatform;
		};
		/* MATCH_VERSION */
		struct {
			u32 ver_start;
			u32 ver_end;
		};
		/* MATCH_STEP */
		struct {
			u8 step_start;
			u8 step_end;
		};
		/* MATCH_ENGINE_CLASS / MATCH_NOT_ENGINE_CLASS */
		struct {
			u8 engine_class;
		};
		/* MATCH_FUNC */
		bool (*match_func)(const struct xe_gt *gt,
				   const struct xe_hw_engine *hwe);
	};
};

/*
 * Single table entry with all the registers and rules to process
 */
struct xe_rtp_entry {
	const char *name;
	const struct xe_rtp_regval regval;
	const struct xe_rtp_rule *rules;
	unsigned int n_rules;
};

#endif
