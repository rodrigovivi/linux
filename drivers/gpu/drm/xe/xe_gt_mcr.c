// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_gt_types.h"
#include "xe_mmio.h"

#include "../i915/gt/intel_gt_regs.h"

/**
 * DOC: Xe GT Multicast Register
 *
 */

/* FIXME: This should come from the guc hwconfig or some other common place */
#define GEN_DSS_PER_GSLICE      4
#define GEN_DSS_PER_MSLICE      8

/**
 * xe_gt_mcr_init: Init the default MCR
 * gt: gt instance
 *
 * It is particularly needed for the workarounds.
 * It depends on the gt's fuse information.
 */
void xe_gt_mcr_init(struct xe_gt *gt)
{
	unsigned long *tmp_map = bitmap_alloc(gt->fuse.size, GFP_KERNEL);
	int i;

	bitmap_zero(tmp_map, gt->fuse.size);

	for_each_set_bit(i, gt->fuse.mslice_map, GEN12_MAX_MSLICES) {
		set_bit(i * 2, tmp_map);
		set_bit((i * 2) + 1, tmp_map);
	}

	if (bitmap_intersects(tmp_map, gt->fuse.gslice_map, gt->fuse.size))
		bitmap_or(tmp_map, tmp_map, gt->fuse.gslice_map, gt->fuse.size);

	if(bitmap_intersects(tmp_map, gt->fuse.mslice_map, gt->fuse.size))
		bitmap_or(tmp_map, tmp_map, gt->fuse.mslice_map, gt->fuse.size);

	gt->mcr.group_id = find_first_bit(tmp_map, gt->fuse.size);
	gt->mcr.instance_id = find_next_bit(gt->fuse.dss_map, gt->fuse.size,
					    gt->mcr.group_id *
					    GEN_DSS_PER_GSLICE) %
			      GEN_DSS_PER_GSLICE;

	xe_mmio_rmw32(gt, GEN8_MCR_SELECTOR.reg,
		      GEN11_MCR_SLICE_MASK | GEN11_MCR_SUBSLICE_MASK,
		      GEN11_MCR_SLICE(gt->mcr.group_id) |
		      GEN11_MCR_SUBSLICE(gt->mcr.instance_id));

	bitmap_free(tmp_map);
}
