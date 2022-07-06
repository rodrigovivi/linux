// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <linux/minmax.h>

#include <drm/drm_managed.h>

#include "xe_bo.h"
#include "xe_engine_types.h"
#include "xe_execlist.h"
#include "xe_force_wake.h"
#include "xe_ggtt.h"
#include "xe_gt.h"
#include "xe_hw_fence.h"
#include "xe_migrate.h"
#include "xe_mmio.h"
#include "xe_ring_ops.h"
#include "xe_sa.h"
#include "xe_ttm_gtt_mgr.h"
#include "xe_ttm_vram_mgr.h"
#include "xe_uc.h"
#include "xe_wopcm.h"

#include "../i915/gt/intel_gt_regs.h"

/* FIXME: Move to common param infrastructure */
static bool enable_guc = true;
module_param_named_unsafe(enable_guc, enable_guc, bool, 0444);
MODULE_PARM_DESC(enable_guc, "Enable GuC submission");

static void gt_params_init(struct xe_gt *gt)
{
	gt->info.enable_guc = enable_guc;
}

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

	gt->ordered_wq = alloc_ordered_workqueue("gt-ordered-wq", 0);

	gt_params_init(gt);

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
		return err;

	return 0;
}

static void gt_fini(struct drm_device *drm, void *arg)
{
	struct xe_gt *gt = arg;
	int i;

	destroy_workqueue(gt->ordered_wq);

	for (i = 0; i < XE_ENGINE_CLASS_MAX; ++i)
		xe_hw_fence_irq_finish(&gt->fence_irq[i]);

	if (gt->mem.vram.mapping)
		iounmap(gt->mem.vram.mapping);
}

static void gt_reset_worker(struct work_struct *w);

int xe_gt_init(struct xe_gt *gt)
{
	int err;
	int i;

	INIT_WORK(&gt->reset.worker, gt_reset_worker);

	for (i = 0; i < XE_ENGINE_CLASS_MAX; ++i) {
		gt->ring_ops[i] = xe_ring_ops_get(gt, i);
		xe_hw_fence_irq_init(&gt->fence_irq[i]);
	}

	xe_force_wake_init(gt, gt->mmio.fw);
	err = xe_force_wake_get(gt->mmio.fw, XE_FORCEWAKE_ALL);
	if (err)
		goto err_hw_fence_irq;

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

	err = xe_execlist_init(gt);
	if (err)
		goto err_ttm_mgr;

	err = xe_hw_engines_init(gt);
	if (err)
		goto err_ttm_mgr;

	err = xe_sa_bo_manager_init(gt, &gt->kernel_bb_pool, SZ_1M, 16);
	if (err)
		goto err_ttm_mgr;

	/* Reserve the last page for prefetcher overflow */
	gt->kernel_bb_pool.base.size -= SZ_4K;

	err = xe_uc_init_hw(&gt->uc);
	if (err)
		goto err_ttm_mgr;

	gt->migrate = xe_migrate_init(gt);
	if (IS_ERR(gt->migrate))
		goto err_ttm_mgr;

	err = xe_force_wake_put(gt->mmio.fw, XE_FORCEWAKE_ALL);
	XE_WARN_ON(err);

	xe_force_wake_prune(gt, gt->mmio.fw);

	err = drmm_add_action_or_reset(&gt_to_xe(gt)->drm, gt_fini, gt);
	if (err)
		return err;

	return 0;

err_ttm_mgr:
	if (gt->mem.vram.mapping)
		iounmap(gt->mem.vram.mapping);
err_force_wake:
	xe_force_wake_put(gt->mmio.fw, XE_FORCEWAKE_ALL);
err_hw_fence_irq:
	for (i = 0; i < XE_ENGINE_CLASS_MAX; ++i)
		xe_hw_fence_irq_finish(&gt->fence_irq[i]);

	return err;
}

int do_gt_reset(struct xe_gt *gt)
{
	struct xe_device *xe = gt_to_xe(gt);
	int err;

	xe_mmio_write32(gt, GEN6_GDRST.reg, GEN11_GRDOM_FULL);
	err = xe_mmio_wait32(gt, GEN6_GDRST.reg, 0, GEN11_GRDOM_FULL, 5);
	if (err)
		drm_err(&xe->drm,
			"GT reset failed to clear GEN11_GRDOM_FULL\n");

	return err;
}

static int gt_reset(struct xe_gt *gt)
{
	struct xe_device *xe = gt_to_xe(gt);
	struct xe_hw_engine *hwe;
	enum xe_hw_engine_id id;
	int err;

	/* We only support GT resets with GuC submission */
	if (!xe_gt_guc_submission_enabled(gt))
		return -ENODEV;

	drm_info(&xe->drm, "GT reset started\n");

	err = xe_force_wake_get(gt->mmio.fw, XE_FORCEWAKE_ALL);
	if (err)
		goto err_unlock;

	err = xe_uc_stop(&gt->uc);
	if (err)
		goto err_out;

	err = do_gt_reset(gt);
	if (err)
		goto err_out;

	tgl_setup_private_ppat(gt);

	err = xe_wopcm_init(&gt->uc.wopcm);
	if (err)
		goto err_out;

	for_each_hw_engine(hwe, gt, id)
		xe_hw_engine_enable_ring(hwe);

	err = xe_uc_init_hw(&gt->uc);
	if (err)
		goto err_out;

	err = xe_uc_start(&gt->uc);
	if (err)
		goto err_out;

	err = xe_force_wake_put(gt->mmio.fw, XE_FORCEWAKE_ALL);
	XE_WARN_ON(err);

	drm_info(&xe->drm, "GT reset done\n");

	return 0;

err_out:
	XE_WARN_ON(xe_force_wake_put(gt->mmio.fw, XE_FORCEWAKE_ALL));
err_unlock:
	drm_err(&xe->drm, "GT reset failed, err=%d\n", err);

	return err;
}

static void gt_reset_worker(struct work_struct *w)
{
	struct xe_gt *gt = container_of(w, typeof(*gt), reset.worker);

	gt_reset(gt);
}

void xe_gt_reset_async(struct xe_gt *gt)
{
	struct xe_device *xe = gt_to_xe(gt);

	drm_info(&xe->drm, "Try GT reset\n");

	/* Don't do a reset while one is already in flight */
	if (xe_uc_reset_prepare(&gt->uc))
		return;

	drm_info(&xe->drm, "Doing GT reset\n");
	queue_work(gt->ordered_wq, &gt->reset.worker);
}

int xe_gt_suspend(struct xe_gt *gt)
{
	struct xe_device *xe = gt_to_xe(gt);
	int err;

	/* For now suspend/resume is only allowed with GuC */
	if (!xe_gt_guc_submission_enabled(gt))
		return -ENODEV;

	err = xe_force_wake_get(gt->mmio.fw, XE_FORCEWAKE_ALL);
	if (err)
		goto err_msg;

	err = xe_uc_suspend(&gt->uc);
	if (err)
		goto err_fw;

	XE_WARN_ON(xe_force_wake_put(gt->mmio.fw, XE_FORCEWAKE_ALL));
	drm_info(&xe->drm, "GT suspended\n");

	return 0;

err_fw:
	XE_WARN_ON(xe_force_wake_put(gt->mmio.fw, XE_FORCEWAKE_ALL));
err_msg:
	drm_err(&xe->drm, "GT suspend failed: %d\n", err);

	return err;
}

int xe_gt_resume(struct xe_gt *gt)
{
	struct xe_device *xe = gt_to_xe(gt);
	int err;

	err = xe_force_wake_get(gt->mmio.fw, XE_FORCEWAKE_ALL);
	if (err)
		goto err_msg;

	err = xe_uc_resume(&gt->uc);
	if (err)
		goto err_fw;

	XE_WARN_ON(xe_force_wake_put(gt->mmio.fw, XE_FORCEWAKE_ALL));
	drm_info(&xe->drm, "GT resumed\n");

	return 0;

err_fw:
	XE_WARN_ON(xe_force_wake_put(gt->mmio.fw, XE_FORCEWAKE_ALL));
err_msg:
	drm_err(&xe->drm, "GT resume failed: %d\n", err);

	return err;
}

struct xe_hw_engine *xe_gt_hw_engine(struct xe_gt *gt,
				     enum xe_engine_class class,
				     uint16_t instance, bool logical)
{
	struct xe_hw_engine *hwe;
	enum xe_hw_engine_id id;

	for_each_hw_engine(hwe, gt, id)
		if (hwe->class == class &&
		    ((!logical && hwe->instance == instance) ||
		    (logical && hwe->logical_instance == instance)))
			return hwe;

	return NULL;
}
