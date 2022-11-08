// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <drm/ttm/ttm_placement.h>

#include "xe_bo.h"
#include "xe_bo_evict.h"
#include "xe_device.h"
#include "xe_pm.h"
#include "xe_gt.h"
#include "xe_ggtt.h"
#include "xe_irq.h"
#include "xe_pcode.h"

#include <linux/fb.h>
#include "display/intel_display_types.h"
#include "display/intel_dp.h"
#include "display/intel_fbdev.h"
#include "display/intel_hotplug.h"
#include "display/ext/intel_pm.h"

/**
 * DOC: Xe Power Management
 *
 * Xe PM shall be guided by the simplicity.
 * Use the simplest hook options whenever possible.
 * Let's not reinvent the runtime_pm references and hooks.
 * Shall have a clear separation of display and gt underneath this component.
 *
 * What's next:
 *
 * For now s2idle and s3 are only working in integrated devices. The next step
 * is to iterate through all VRAM's BO backing them up into the system memory
 * before allowing the system suspend.
 *
 * Also runtime_pm needs to be here from the beginning.
 *
 * RC6/RPS are also critical PM features. Let's start with GuCRC and GuC SLPC
 * and no wait boost. Frequency optimizations should come on a next stage.
 */

/**
 * xe_pm_suspend - Helper for System suspend, i.e. S0->S3 / S0->S2idle
 * @xe: xe device instance
 *
 * Return: 0 on success
 */
static void intel_suspend_encoders(struct drm_i915_private *dev_priv)
{
	struct drm_device *dev = &dev_priv->drm;
	struct intel_encoder *encoder;

	if (!dev_priv->info.display.pipe_mask)
		return;

	drm_modeset_lock_all(dev);
	for_each_intel_encoder(dev, encoder)
		if (encoder->suspend)
			encoder->suspend(encoder);
	drm_modeset_unlock_all(dev);
}

static void xe_pm_display_suspend(struct xe_device *xe)
{
	/* We do a lot of poking in a lot of registers, make sure they work
	 * properly. */
	intel_power_domains_disable(xe);
	if (xe->info.display.pipe_mask)
		drm_kms_helper_poll_disable(&xe->drm);

	intel_display_suspend(&xe->drm);

	intel_dp_mst_suspend(xe);

	intel_hpd_cancel_work(xe);

	intel_suspend_encoders(xe);

	intel_opregion_suspend(xe, PCI_D3cold);

	intel_fbdev_set_suspend(&xe->drm, FBINFO_STATE_SUSPENDED, true);

	intel_dmc_ucode_suspend(xe);
}

static void xe_pm_display_suspend_late(struct xe_device *xe)
{
	intel_power_domains_suspend(xe, I915_DRM_SUSPEND_MEM);

	intel_display_power_suspend_late(xe);
}

static void xe_pm_display_resume_early(struct xe_device *xe)
{
	intel_display_power_resume_early(xe);

	intel_power_domains_resume(xe);
}

static void xe_pm_display_resume(struct xe_device *xe)
{
	intel_dmc_ucode_resume(xe);

	if (xe->info.display.pipe_mask)
		drm_mode_config_reset(&xe->drm);

	intel_modeset_init_hw(xe);
	intel_init_clock_gating(xe);
	intel_hpd_init(xe);

	/* MST sideband requires HPD interrupts enabled */
	intel_dp_mst_resume(xe);
	intel_display_resume(&xe->drm);

	intel_hpd_poll_disable(xe);
	if (xe->info.display.pipe_mask)
		drm_kms_helper_poll_enable(&xe->drm);

	intel_opregion_resume(xe);

	intel_fbdev_set_suspend(&xe->drm, FBINFO_STATE_RUNNING, false);

	intel_power_domains_enable(xe);
}

int xe_pm_suspend(struct xe_device *xe)
{
	struct xe_gt *gt;
	u8 id;
	int err;

	for_each_gt(gt, xe, id)
		xe_gt_suspend_prepare(gt);

	/* FIXME: Super racey... */
	err = xe_bo_evict_all(xe);
	if (err)
		return err;

	xe_pm_display_suspend(xe);

	for_each_gt(gt, xe, id) {
		err = xe_gt_suspend(gt);
		if (err) {
			xe_pm_display_resume(xe);
			return err;
		}
	}

	xe_irq_suspend(xe);

	xe_pm_display_suspend_late(xe);

	return 0;
}

/**
 * xe_pm_resume - Helper for System resume S3->S0 / S2idle->S0
 * @xe: xe device instance
 *
 * Return: 0 on success
 */
int xe_pm_resume(struct xe_device *xe)
{
	struct xe_gt *gt;
	u8 id;
	int err;

	for_each_gt(gt, xe, id) {
		err = xe_pcode_init(gt);
		if (err)
			return err;
	}

	xe_pm_display_resume_early(xe);

	/*
	 * This only restores pinned memory which is the memory required for the
	 * GT(s) to resume.
	 */
	err = xe_bo_restore_kernel(xe);
	if (err)
		return err;

	xe_irq_resume(xe);

	xe_pm_display_resume(xe);

	for_each_gt(gt, xe, id)
		xe_gt_resume(gt);

	err = xe_bo_restore_user(xe);
	if (err)
		return err;

	return 0;
}
