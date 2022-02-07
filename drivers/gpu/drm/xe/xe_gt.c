// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <linux/minmax.h>

#include <drm/drm_managed.h>

#include "xe_bo.h"
#include "xe_force_wake.h"
#include "xe_ggtt.h"
#include "xe_gt.h"
#include "xe_mmio.h"
#include "xe_ttm_gtt_mgr.h"
#include "xe_ttm_vram_mgr.h"
#include "xe_uc.h"

#include "../i915/i915_reg.h"

int xe_gt_alloc(struct xe_gt *gt)
{
	struct drm_device *drm = &gt_to_xe(gt)->drm;

	gt->mmio.fw = drmm_kzalloc(drm, sizeof(*gt->mmio.fw), GFP_KERNEL);
	if (!gt->mmio.fw)
		return -ENOMEM;

	gt->mem.ggtt = drmm_kzalloc(drm, sizeof(*gt->mem.ggtt), GFP_KERNEL);
	if (!gt->mem.ggtt)
		return -ENOMEM;

	gt->mem.vram_mgr = drmm_kzalloc(drm, sizeof(*gt->mem.vram_mgr),
					GFP_KERNEL);
	if (!gt->mem.vram_mgr)
		return -ENOMEM;

	gt->mem.gtt_mgr = drmm_kzalloc(drm, sizeof(*gt->mem.gtt_mgr),
				       GFP_KERNEL);
	if (!gt->mem.gtt_mgr)
		return -ENOMEM;

	return 0;
}

#define CHV_PPAT_SNOOP			REG_BIT(6)
#define GEN8_PPAT_AGE(x)		((x)<<4)
#define GEN8_PPAT_LLCeLLC		(3<<2)
#define GEN8_PPAT_LLCELLC		(2<<2)
#define GEN8_PPAT_LLC			(1<<2)
#define GEN8_PPAT_WB			(3<<0)
#define GEN8_PPAT_WT			(2<<0)
#define GEN8_PPAT_WC			(1<<0)
#define GEN8_PPAT_UC			(0<<0)
#define GEN8_PPAT_ELLC_OVERRIDE		(0<<2)
#define GEN8_PPAT(i, x)			((u64)(x) << ((i) * 8))

static void tgl_setup_private_ppat(struct xe_gt *gt)
{
	/* TGL doesn't support LLC or AGE settings */
	xe_mmio_write32(gt, GEN12_PAT_INDEX(0).reg, GEN8_PPAT_WB);
	xe_mmio_write32(gt, GEN12_PAT_INDEX(1).reg, GEN8_PPAT_WC);
	xe_mmio_write32(gt, GEN12_PAT_INDEX(2).reg, GEN8_PPAT_WT);
	xe_mmio_write32(gt, GEN12_PAT_INDEX(3).reg, GEN8_PPAT_UC);
	xe_mmio_write32(gt, GEN12_PAT_INDEX(4).reg, GEN8_PPAT_WB);
	xe_mmio_write32(gt, GEN12_PAT_INDEX(5).reg, GEN8_PPAT_WB);
	xe_mmio_write32(gt, GEN12_PAT_INDEX(6).reg, GEN8_PPAT_WB);
	xe_mmio_write32(gt, GEN12_PAT_INDEX(7).reg, GEN8_PPAT_WB);
}

static int gt_ttm_mgr_init(struct xe_gt *gt)
{
	int err;
	struct sysinfo si;
	uint64_t gtt_size;

	si_meminfo(&si);
	gtt_size = (uint64_t)si.totalram * si.mem_unit * 3/4;

	if (gt->mem.vram.size) {
		err = xe_ttm_vram_mgr_init(gt, gt->mem.vram_mgr);
		if (err)
			return err;
#ifdef CONFIG_64BIT
		gt->mem.vram.mapping = ioremap_wc(gt->mem.vram.io_start,
						  gt->mem.vram.size);
#endif
		gtt_size = min(max((XE_DEFAULT_GTT_SIZE_MB << 20),
				   gt->mem.vram.size),
			       gtt_size);
	}

	err = xe_ttm_gtt_mgr_init(gt, gt->mem.gtt_mgr, gtt_size);
	if (err)
		goto err_vram_mgr;

	return 0;

err_vram_mgr:
	if (gt->mem.vram.size)
		xe_ttm_vram_mgr_fini(gt->mem.vram_mgr);

	return err;
}

int xe_gt_init(struct xe_gt *gt)
{
	int err;
	int i;

	xe_force_wake_init(gt, gt->mmio.fw);
	err = xe_force_wake_get(gt->mmio.fw, XE_FORCEWAKE_ALL);
	if (err)
		return err;

	tgl_setup_private_ppat(gt);

	err = gt_ttm_mgr_init(gt);
	if (err)
		goto err_force_wake;

	err = xe_ggtt_init(gt, gt->mem.ggtt);
	if (err)
		goto err_ttm_mgr;

	/* Allow driver to load if uC init fails (likely missing firmware) */
	err = xe_uc_init(&gt->uc);
	XE_WARN_ON(err);

	for (i = 0; i < ARRAY_SIZE(gt->hw_engines); i++) {
		err = xe_hw_engine_init(gt, &gt->hw_engines[i], i);
		if (err)
			goto err_hw_engines;
	}
	err = xe_uc_init_hw(&gt->uc);
	if (err)
		goto err_hw_engines;

	err = xe_force_wake_put(gt->mmio.fw, XE_FORCEWAKE_ALL);
	XE_WARN_ON(err);

	return 0;

err_hw_engines:
	for (i = 0; i < ARRAY_SIZE(gt->hw_engines); i++) {
		if (xe_hw_engine_is_valid(&gt->hw_engines[i]))
			xe_hw_engine_finish(&gt->hw_engines[i]);
	}
	xe_uc_fini(&gt->uc);
	xe_ggtt_finish(gt->mem.ggtt);
err_ttm_mgr:
	if (gt->mem.vram.mapping)
		iounmap(gt->mem.vram.mapping);
	if (gt->mem.vram.size)
		xe_ttm_vram_mgr_fini(gt->mem.vram_mgr);
	xe_ttm_gtt_mgr_fini(gt->mem.gtt_mgr);
err_force_wake:
	xe_force_wake_put(gt->mmio.fw, XE_FORCEWAKE_ALL);

	return err;
}

void xe_gt_fini(struct xe_gt *gt)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(gt->hw_engines); i++) {
		if (xe_hw_engine_is_valid(&gt->hw_engines[i]))
			xe_hw_engine_finish(&gt->hw_engines[i]);
	}
	xe_uc_fini(&gt->uc);
	xe_ggtt_finish(gt->mem.ggtt);
	if (gt->mem.vram.mapping)
		iounmap(gt->mem.vram.mapping);
	if (gt->mem.vram.size)
		xe_ttm_vram_mgr_fini(gt->mem.vram_mgr);
	xe_ttm_gtt_mgr_fini(gt->mem.gtt_mgr);
}

struct xe_hw_engine *xe_gt_hw_engine(struct xe_gt *gt,
				     enum xe_engine_class class,
				     uint16_t instance)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(gt->hw_engines); i++) {
		if (xe_hw_engine_is_valid(&gt->hw_engines[i]) &&
		    gt->hw_engines[i].class == class &&
		    gt->hw_engines[i].instance == instance)
			return &gt->hw_engines[i];
	}

	return NULL;
}
