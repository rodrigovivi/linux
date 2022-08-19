// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_rtp.h"

#include <drm/xe_drm.h>

#include "xe_gt.h"
#include "xe_macros.h"
#include "xe_reg_sr.h"

static bool rule_matches(struct xe_gt *gt,
			 struct xe_hw_engine *hwe,
			 const struct xe_rtp_entry *entry)
{
	const struct xe_device *xe = gt_to_xe(gt);
	const struct xe_rtp_rule *r;
	unsigned int i;
	bool match;

	for (r = entry->rules, i = 0; i < entry->n_rules;
	     r = &entry->rules[++i]) {
		switch (r->match_type) {
		case XE_RTP_MATCH_PLATFORM:
			match = xe->info.platform == r->platform;
			break;
		case XE_RTP_MATCH_SUBPLATFORM:
			match = xe->info.platform == r->platform &&
				xe->info.subplatform == r->subplatform;
			break;
		case XE_RTP_MATCH_VERSION:
			/* TODO: match media/display */
			match = xe->info.graphics_verx100 >= r->ver_start &&
				xe->info.graphics_verx100 < r->ver_end;
			break;
		case XE_RTP_MATCH_STEP:
			/* TODO: match media/display */
			match = xe->info.step.graphics >= r->step_start &&
				xe->info.step.graphics < r->step_end;
			break;
		case XE_RTP_MATCH_ENGINE_CLASS:
			match = hwe->class == r->engine_class;
			break;
		case XE_RTP_MATCH_NOT_ENGINE_CLASS:
			match = hwe->class != r->engine_class;
			break;
		case XE_RTP_MATCH_FUNC:
			match = r->match_func(gt, hwe);
			break;
		default:
			XE_WARN_ON(r->match_type);
		}

		if (!match)
			return false;
	}

	return true;
}

static void rtp_add_sr_entry(const struct xe_rtp_entry *entry,
			     struct xe_gt *gt,
			     u32 mmio_base,
			     struct xe_reg_sr *sr)
{
	i915_reg_t reg = _MMIO(entry->regval.reg.reg + mmio_base);
	struct xe_reg_sr_entry sr_entry = {
		.clr_bits = entry->regval.clr_bits,
		.set_bits = entry->regval.set_bits,
		.read_mask = entry->regval.read_mask,
		.masked_reg = entry->regval.flags & XE_RTP_FLAG_MASKED_REG,
	};

	xe_reg_sr_add(sr, reg, &sr_entry);
}

void xe_rtp_process(const struct xe_rtp_entry *entries, struct xe_reg_sr *sr,
		    struct xe_gt *gt, struct xe_hw_engine *hwe)
{
	const struct xe_rtp_entry *entry;

	for (entry = entries; entry && entry->name; entry++) {
		u32 mmio_base = 0;

		if (entry->regval.flags & XE_RTP_FLAG_FOREACH_ENGINE) {
			struct xe_hw_engine *each_hwe;
			enum xe_hw_engine_id id;

			for_each_hw_engine(each_hwe, gt, id) {
				mmio_base = each_hwe->mmio_base;

				if (rule_matches(gt, each_hwe, entry))
					rtp_add_sr_entry(entry, gt, mmio_base, sr);
			}
		} else if (rule_matches(gt, hwe, entry)) {
			if (entry->regval.flags & XE_RTP_FLAG_ENGINE_BASE)
				mmio_base = hwe->mmio_base;

			rtp_add_sr_entry(entry, gt, mmio_base, sr);
		}
	}
}
