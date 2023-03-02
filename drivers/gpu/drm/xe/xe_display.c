// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#if IS_ENABLED(CONFIG_DRM_XE_DISPLAY)

#include "xe_display.h"
#include "regs/xe_regs.h"

#include <linux/fb.h>

#include <drm/drm_managed.h>
#include <drm/xe_drm.h>

#include "ext/i915_irq.h"
#include "ext/intel_dram.h"
#include "intel_acpi.h"
#include "intel_audio.h"
#include "intel_bw.h"
#include "intel_clock_gating.h"
#include "intel_display.h"
#include "intel_display_driver.h"
#include "intel_display_types.h"
#include "intel_dmc.h"
#include "intel_dp.h"
#include "intel_fbdev.h"
#include "intel_hdcp.h"
#include "intel_hotplug.h"
#include "intel_opregion.h"
#include "xe_module.h"

/* Xe device functions */

static void xe_display_last_close(struct drm_device *dev)
{
	intel_fbdev_restore_mode(to_xe_device(dev));
}

/**
 * xe_display_set_driver_hooks - set driver flags and hooks for display
 * @pdev: PCI device
 * @driver: DRM device driver
 *
 * Set features and function hooks in @driver that are needed for driving the
 * display IP, when that is enabled.
 *
 * Returns: 0 on success
 */
int xe_display_set_driver_hooks(struct pci_dev *pdev, struct drm_driver *driver)
{
	if (!enable_display)
		return 0;

	/* Detect if we need to wait for other drivers early on */
	if (intel_display_driver_probe_defer(pdev))
		return -EPROBE_DEFER;

	driver->driver_features |= DRIVER_MODESET | DRIVER_ATOMIC;
	driver->lastclose = xe_display_last_close;

	return 0;
}

static void display_destroy(struct drm_device *dev, void *dummy)
{
	struct xe_device *xe = to_xe_device(dev);

	destroy_workqueue(xe->display.hotplug.dp_wq);
}

/**
 * xe_display_create - create display struct
 * @xe: XE device instance
 *
 * Initialize all fields used by the display part.
 *
 * TODO: once everything can be inside a single struct, make the struct opaque
 * to the rest of xe and return it to be xe->display.
 *
 * Returns: 0 on success
 */
int xe_display_create(struct xe_device *xe)
{
	int err;

	/* Initialize display parts here.. */
	spin_lock_init(&xe->display.fb_tracking.lock);

	xe->display.hotplug.dp_wq = alloc_ordered_workqueue("xe-dp", 0);

	drmm_mutex_init(&xe->drm, &xe->sb_lock);
	drmm_mutex_init(&xe->drm, &xe->display.backlight.lock);
	drmm_mutex_init(&xe->drm, &xe->display.audio.mutex);
	drmm_mutex_init(&xe->drm, &xe->display.wm.wm_mutex);
	drmm_mutex_init(&xe->drm, &xe->display.pps.mutex);
	drmm_mutex_init(&xe->drm, &xe->display.hdcp.comp_mutex);
	xe->enabled_irq_mask = ~0;

	xe->params.invert_brightness = -1;
	xe->params.vbt_sdvo_panel_type = -1;
	xe->params.disable_power_well = -1;
	xe->params.enable_dc = -1;
	xe->params.enable_dpcd_backlight = -1;
	xe->params.enable_dp_mst = -1;
	xe->params.enable_dpt = true;
	xe->params.enable_fbc = -1;
	xe->params.enable_psr = -1;
	xe->params.enable_psr2_sel_fetch = -1;
	xe->params.enable_sagv = true;
	xe->params.panel_use_ssc = -1;

	err = drmm_add_action_or_reset(&xe->drm, display_destroy, NULL);
	if (err)
		return err;

	return 0;
}

void xe_display_fini_nommio(struct drm_device *dev, void *dummy)
{
	struct xe_device *xe = to_xe_device(dev);

	if (!xe->info.enable_display)
		return;

	intel_power_domains_cleanup(xe);
}

int xe_display_init_nommio(struct xe_device *xe)
{
	int err;

	if (!xe->info.enable_display)
		return 0;

	/* Fake uncore lock */
	spin_lock_init(&xe->uncore.lock);

	/* This must be called before any calls to HAS_PCH_* */
	intel_detect_pch(xe);
	intel_display_irq_init(xe);

	err = intel_power_domains_init(xe);
	if (err)
		return err;

	intel_init_display_hooks(xe);

	return drmm_add_action_or_reset(&xe->drm, xe_display_fini_nommio, xe);
}

void xe_display_fini_noirq(struct drm_device *dev, void *dummy)
{
	struct xe_device *xe = to_xe_device(dev);

	if (!xe->info.enable_display)
		return;

	intel_display_driver_remove_noirq(xe);
	intel_power_domains_driver_remove(xe);
}

int xe_display_init_noirq(struct xe_device *xe)
{
	int err;

	if (!xe->info.enable_display)
		return 0;

	intel_display_driver_early_probe(xe);

	/* Early display init.. */
	intel_opregion_setup(xe);

	/*
	 * Fill the dram structure to get the system dram info. This will be
	 * used for memory latency calculation.
	 */
	intel_dram_detect(xe);

	intel_bw_init_hw(xe);

	intel_device_info_runtime_init(xe);

	err = intel_display_driver_probe_noirq(xe);
	if (err)
		return err;

	return drmm_add_action_or_reset(&xe->drm, xe_display_fini_noirq, NULL);
}

void xe_display_fini_noaccel(struct drm_device *dev, void *dummy)
{
	struct xe_device *xe = to_xe_device(dev);

	if (!xe->info.enable_display)
		return;

	intel_display_driver_remove_nogem(xe);
}

int xe_display_init_noaccel(struct xe_device *xe)
{
	int err;

	if (!xe->info.enable_display)
		return 0;

	err = intel_display_driver_probe_nogem(xe);
	if (err)
		return err;

	return drmm_add_action_or_reset(&xe->drm, xe_display_fini_noaccel, NULL);
}

int xe_display_init(struct xe_device *xe)
{
	if (!xe->info.enable_display)
		return 0;

	return intel_display_driver_probe(xe);
}

void xe_display_unlink(struct xe_device *xe)
{
	if (!xe->info.enable_display)
		return;

	/* poll work can call into fbdev, hence clean that up afterwards */
	intel_hpd_poll_fini(xe);
	intel_fbdev_fini(xe);

	intel_hdcp_component_fini(xe);
	intel_audio_deinit(xe);
}

void xe_display_register(struct xe_device *xe)
{
	if (!xe->info.enable_display)
		return;

	intel_display_driver_register(xe);
	intel_register_dsm_handler();
	intel_power_domains_enable(xe);
}

void xe_display_unregister(struct xe_device *xe)
{
	if (!xe->info.enable_display)
		return;

	intel_unregister_dsm_handler();
	intel_power_domains_disable(xe);
	intel_display_driver_unregister(xe);
}

void xe_display_modset_driver_remove(struct xe_device *xe)
{
	if (!xe->info.enable_display)
		return;

	intel_display_driver_remove(xe);
}

/* IRQ-related functions */

void xe_display_irq_handler(struct xe_device *xe, u32 master_ctl)
{
	if (!xe->info.enable_display)
		return;

	if (master_ctl & DISPLAY_IRQ)
		gen11_display_irq_handler(xe);
}

void xe_display_irq_enable(struct xe_device *xe, u32 gu_misc_iir)
{
	if (!xe->info.enable_display)
		return;

	if (gu_misc_iir & GU_MISC_GSE)
		intel_opregion_asle_intr(xe);
}

void xe_display_irq_reset(struct xe_device *xe)
{
	if (!xe->info.enable_display)
		return;

	gen11_display_irq_reset(xe);
}

void xe_display_irq_postinstall(struct xe_device *xe, struct xe_gt *gt)
{
	if (!xe->info.enable_display)
		return;

	if (gt->info.id == XE_GT0)
		gen11_display_irq_postinstall(xe);
}

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

void xe_display_pm_suspend(struct xe_device *xe)
{
	if (!xe->info.enable_display)
		return;

	/*
	 * We do a lot of poking in a lot of registers, make sure they work
	 * properly.
	 */
	intel_power_domains_disable(xe);
	if (xe->info.display.pipe_mask)
		drm_kms_helper_poll_disable(&xe->drm);

	intel_display_driver_suspend(xe);

	intel_dp_mst_suspend(xe);

	intel_hpd_cancel_work(xe);

	intel_suspend_encoders(xe);

	intel_opregion_suspend(xe, PCI_D3cold);

	intel_fbdev_set_suspend(&xe->drm, FBINFO_STATE_SUSPENDED, true);

	intel_dmc_suspend(xe);
}

void xe_display_pm_suspend_late(struct xe_device *xe)
{
	if (!xe->info.enable_display)
		return;

	intel_power_domains_suspend(xe, I915_DRM_SUSPEND_MEM);

	intel_display_power_suspend_late(xe);
}

void xe_display_pm_resume_early(struct xe_device *xe)
{
	if (!xe->info.enable_display)
		return;

	intel_display_power_resume_early(xe);

	intel_power_domains_resume(xe);
}

void xe_display_pm_resume(struct xe_device *xe)
{
	if (!xe->info.enable_display)
		return;

	intel_dmc_resume(xe);

	if (xe->info.display.pipe_mask)
		drm_mode_config_reset(&xe->drm);

	intel_display_driver_init_hw(xe);
	intel_clock_gating_init(xe);
	intel_hpd_init(xe);

	/* MST sideband requires HPD interrupts enabled */
	intel_dp_mst_resume(xe);
	intel_display_driver_resume(xe);

	intel_hpd_poll_disable(xe);
	if (xe->info.display.pipe_mask)
		drm_kms_helper_poll_enable(&xe->drm);

	intel_opregion_resume(xe);

	intel_fbdev_set_suspend(&xe->drm, FBINFO_STATE_RUNNING, false);

	intel_power_domains_enable(xe);
}

#endif
