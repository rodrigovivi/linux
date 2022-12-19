// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <linux/pm_runtime.h>

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

#if IS_ENABLED(CONFIG_DRM_XE_DISPLAY)
#include "display/intel_display_types.h"
#include "display/intel_dp.h"
#include "display/intel_fbdev.h"
#include "display/intel_hotplug.h"
#include "display/ext/intel_pm.h"
#endif

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

#if IS_ENABLED(CONFIG_DRM_XE_DISPLAY)
static void intel_suspend_encoders(struct xe_device *xe)
{
	struct drm_device *dev = &xe->drm;
	struct intel_encoder *encoder;

	if (!xe->info.display.pipe_mask)
		return;

	drm_modeset_lock_all(dev);
	for_each_intel_encoder(dev, encoder)
		if (encoder->suspend)
			encoder->suspend(encoder);
	drm_modeset_unlock_all(dev);
}
#endif

static void xe_pm_display_suspend(struct xe_device *xe)
{
#if IS_ENABLED(CONFIG_DRM_XE_DISPLAY)
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
#endif
}

static void xe_pm_display_suspend_late(struct xe_device *xe)
{
#if IS_ENABLED(CONFIG_DRM_XE_DISPLAY)
	intel_power_domains_suspend(xe, I915_DRM_SUSPEND_MEM);

	intel_display_power_suspend_late(xe);
#endif
}

static void xe_pm_display_resume_early(struct xe_device *xe)
{
#if IS_ENABLED(CONFIG_DRM_XE_DISPLAY)
	intel_display_power_resume_early(xe);

	intel_power_domains_resume(xe);
#endif
}

static void xe_pm_display_resume(struct xe_device *xe)
{
#if IS_ENABLED(CONFIG_DRM_XE_DISPLAY)
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
#endif
}

/**
 * xe_pm_suspend - Helper for System suspend, i.e. S0->S3 / S0->S2idle
 * @xe: xe device instance
 *
 * Return: 0 on success
 */
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

void xe_pm_runtime_init(struct xe_device *xe)
{
	struct device *dev = xe->drm.dev;

	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_set_active(dev);
	pm_runtime_allow(dev);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
}

int xe_pm_runtime_suspend(struct xe_device *xe)
{
	struct xe_gt *gt;
	u8 id;
	int err;

	if (xe->d3cold_allowed) {
		if (xe_device_mem_access_ongoing(xe))
			return -EBUSY;

		err = xe_bo_evict_all(xe);
		if (err)
			return err;
	}

	for_each_gt(gt, xe, id) {
		err = xe_gt_suspend(gt);
		if (err)
			return err;
	}

	xe_irq_suspend(xe);

	return 0;
}

int xe_pm_runtime_resume(struct xe_device *xe)
{
	struct xe_gt *gt;
	u8 id;
	int err;

	if (xe->d3cold_allowed) {
		for_each_gt(gt, xe, id) {
			err = xe_pcode_init(gt);
			if (err)
				return err;
		}

		/*
		 * This only restores pinned memory which is the memory
		 * required for the GT(s) to resume.
		 */
		err = xe_bo_restore_kernel(xe);
		if (err)
			return err;
	}

	xe_irq_resume(xe);

	for_each_gt(gt, xe, id)
		xe_gt_resume(gt);

	if (xe->d3cold_allowed) {
		err = xe_bo_restore_user(xe);
		if (err)
			return err;
	}

	return 0;
}

int xe_pm_runtime_get(struct xe_device *xe)
{
	return pm_runtime_get_sync(xe->drm.dev);
}

int xe_pm_runtime_put(struct xe_device *xe)
{
	pm_runtime_mark_last_busy(xe->drm.dev);
	return pm_runtime_put_autosuspend(xe->drm.dev);
}

/* Return true if resume operation happened and usage count was increased */
bool xe_pm_runtime_resume_if_suspended(struct xe_device *xe)
{
	/* In case we are suspended we need to immediately wake up */
	if (pm_runtime_suspended(xe->drm.dev))
		return !pm_runtime_resume_and_get(xe->drm.dev);

	return false;
}

int xe_pm_runtime_get_if_active(struct xe_device *xe)
{
	WARN_ON(pm_runtime_suspended(xe->drm.dev));
	return pm_runtime_get_if_active(xe->drm.dev, true);
}
