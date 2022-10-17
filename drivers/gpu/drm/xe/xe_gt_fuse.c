// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_gt.h"
#include "xe_gt_types.h"
#include "xe_mmio.h"

#include "../i915/gt/intel_gt_regs.h"

/**
 * DOC: Xe GT Fuse information (Slice, Sub-Slice, and EU)
 *
 */

/* FIXME: This should come from the guc hwconfig or some other common place */
#define I915_MAX_SS_FUSE_REGS 2
#define GEN_DSS_PER_GSLICE      4
#define GEN_DSS_PER_CSLICE      8
#define GEN_DSS_PER_MSLICE      8

static void load_bitmap_from_reg(struct xe_gt *gt, unsigned long *bitmap,
				 u32 reg, u32 mask)
{
	u32 fuse_val[1];

	fuse_val[0] = xe_mmio_read32(gt, reg) & mask;
	bitmap_from_arr32(bitmap, fuse_val, 32);
}

static void load_bitmap_from_regs(struct xe_gt *gt, unsigned long *bitmap,
				  int numregs, ...)
{
        va_list argp;
        u32 fuse_val[I915_MAX_SS_FUSE_REGS] = {};
        int i;

        if (WARN_ON(numregs > I915_MAX_SS_FUSE_REGS))
                numregs = I915_MAX_SS_FUSE_REGS;

        va_start(argp, numregs);
        for (i = 0; i < numregs; i++)
                fuse_val[i] = xe_mmio_read32(gt, va_arg(argp, u32));
        va_end(argp);

        bitmap_from_arr32(bitmap, fuse_val, numregs * 32);
}

static void sectionmap_from_bitmap(unsigned long *dest, unsigned long *src,
				   unsigned long size, int section_size)
{
	unsigned long *section_mask = bitmap_alloc(size, GFP_KERNEL);
	unsigned long *tmp = bitmap_alloc(size, GFP_KERNEL);
	int i;

	bitmap_fill(section_mask, section_size);
	bitmap_copy(tmp, src, size);
	bitmap_zero(dest, size);

	for (i = 0; !bitmap_empty(tmp, size); i++) {
		if (bitmap_intersects(tmp, section_mask, section_size))
			set_bit(i, dest);

		bitmap_shift_right(tmp, tmp, section_size, size);
	}

	bitmap_free(section_mask);
	bitmap_free(tmp);
}

/**
 * xe_gt_fuse_init - Init the Fuse information
 * @gt: gt instance
 */
void xe_gt_fuse_init(struct xe_gt *gt)
{
	struct xe_device *xe = gt_to_xe(gt);
	unsigned long *tmp_map;

	/* FIXME: This size should come from hwconfig or somewhere global */
	gt->fuse.size = 64;
	gt->fuse.dss_map = bitmap_alloc(gt->fuse.size, GFP_KERNEL);
	gt->fuse.mslice_map = bitmap_alloc(gt->fuse.size, GFP_KERNEL);
	gt->fuse.gslice_map = bitmap_alloc(gt->fuse.size, GFP_KERNEL);

	tmp_map = bitmap_alloc(gt->fuse.size, GFP_KERNEL);

	bitmap_zero(gt->fuse.dss_map, gt->fuse.size);
	bitmap_zero(gt->fuse.mslice_map, gt->fuse.size);
	bitmap_zero(gt->fuse.gslice_map, gt->fuse.size);
	bitmap_zero(tmp_map, gt->fuse.size);

	load_bitmap_from_regs(gt, gt->fuse.dss_map, 1,
			      GEN12_GT_GEOMETRY_DSS_ENABLE.reg);
	if (GRAPHICS_VERx100(xe) >= 1250) {
		load_bitmap_from_regs(gt, tmp_map,
				      xe->info.platform == XE_DG2? 1 : 2,
				      GEN12_GT_COMPUTE_DSS_ENABLE.reg,
				      XEHPC_GT_COMPUTE_DSS_ENABLE_EXT.reg);
	}
	bitmap_or(gt->fuse.dss_map, gt->fuse.dss_map, tmp_map,
		  gt->fuse.size);

	if (xe->info.platform == XE_DG2) {
		sectionmap_from_bitmap(gt->fuse.mslice_map, gt->fuse.dss_map,
				       gt->fuse.size, GEN_DSS_PER_MSLICE);
	}

	bitmap_zero(tmp_map, gt->fuse.size);
	load_bitmap_from_reg(gt, tmp_map,
			     GEN10_MIRROR_FUSE3.reg,
			     GEN12_MEML3_EN_MASK);
	bitmap_or(gt->fuse.mslice_map, gt->fuse.mslice_map,
		  tmp_map, gt->fuse.size);

	sectionmap_from_bitmap(gt->fuse.gslice_map, gt->fuse.dss_map,
			       gt->fuse.size, GEN_DSS_PER_GSLICE);

	bitmap_free(tmp_map);
}

/**
 * xe_gt_fuse_fini - Finalize the Fuse information
 * @gt: gt instance
 */
void xe_gt_fuse_fini(struct xe_gt *gt)
{
	bitmap_free(gt->fuse.dss_map);
	bitmap_free(gt->fuse.mslice_map);
	bitmap_free(gt->fuse.gslice_map);
}
