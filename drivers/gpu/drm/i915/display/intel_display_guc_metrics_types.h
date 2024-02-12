/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __INTEL_DISPLAY_GUC_METRICS_TYPES_H__
#define __INTEL_DISPLAY_GUC_METRICS_TYPES_H__

/**
 * struct intel_display_guc_metrics - Intel Display GuC Metrics main struct
 *
 * The graphics device can register with intel_display to get information
 * about display events that will then be used with GuC SLPC.
 */
struct intel_display_guc_metrics {
	/**
	 * @gfx_device: A pointer to the private device,
	 * either to struct drm_i915_private or to struct xe_device.
	 */
	void *gfx_device;

	/** @refresh_info_update: Callback for getting refresh information on modeset */
	void (*refresh_info_update)(void *gfx_device, int pipe,
				    u32 refresh_interval, bool vrr_enabled);
	/** @vblank_update: Callback for getting vblank information updates */
	void (*vblank_update)(void *gfx_device, int pipe);
	/** @flip_update: Callback for getting page flip information updates */
	void (*flip_update)(void *gfx_device, int pipe, int plane,
			    bool async_flip);
};

#endif
