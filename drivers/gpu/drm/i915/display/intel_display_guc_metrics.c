// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include "intel_display_guc_metrics.h"
#include "intel_display_guc_metrics_types.h"

#include <drm/drm_modes.h>

#include "i915_drv.h"
#include "intel_de.h"
#include "intel_display_types.h"

/**
 * Display GuC Metrics
 *
 * GuC SLPC has many optimized strategies to best select the running GT
 * frequency.
 * One common strategy is to take display metrics as input through a shared
 * data buffer. The GuC SLPC will then use these metrics for a optimal balance
 * between power savings and performance.
 *
 * This intel_display_guc_metrics, provides a generic interface where xe_guc_pc
 * or i915's intel_guc_slpc could register themselves in order to recieve the
 * metrics from the running intel_display.
 *
 * Since this is a generic interface, it won't take any further action, but only
 * pass the generic display information about refresh_info, flips and vblank.
 * The GuC SLPC component of the registered driver (Xe or i915) will then be
 * responsible for allocating the shared display buffer, for collecting the
 * right timestamp registers of the GT, and for programming the shared buffer
 * as requested by GuC.
 *
 * The Display Shared Data is a block of global GTT memory into which the host
 * continually writes display related information for SLPC to read and use in
 * its algorithms.
 *
 * The programming flow is as follows.
 *
 * The host allocates sufficient memory in the global GTT for the Display
 * Shared Data.
 *
 * The host initializes the Display Shared Data by setting the Version,
 * Number of Pipes, and Number of Planes per Pipe fields in the Global Info.
 * All other fields should start at 0.
 *
 * The host provides the Display Shared Data memory address in the Shared Data
 * while (re-)activating SLPC through the GUC_ACTION_HOST2GUC_PCV2_SLPC_REQUEST
 * Reset event. SLPC will now begin reading the Display Shared Data as part of
 * its periodic processing. It reads the Global Info section and proceeds to the
 * other sections only if a change count has been incremented.
 *
 * On a display connection to a pipe, the host writes the Refresh Info for the
 * given pipe, then increments the Refresh Info Change Count field of the Global
 * Info to alert SLPC to the change. This is also done if an existing display
 * changes its refresh configuration.
 *
 * On a vblank event, the host updates the Vblank Metrics for the given pipe,
 * then increments the Vblank Metrics Change Count field of the Global Info to
 * alert SLPC to the change.
 *
 * On a flip event, the host updates the Flip Metrics for the given plane on the
 * given pipe, then increments the Flip Metrics Change Count field of the Global
 * Info to alert SLPC to the change.
 */

/**
 * intel_display_guc_metrics_init - For device driver registration (i915 or xe)
 * @gfx_device: Back pointer to whatever device is driving display (i915 or xe).
 * @display: Pointer to the intel_display struct that was initialized by gfx_device.
 * @guc_metrics: Struct with the callback function pointers to get notication from display.
 */
void intel_display_guc_metrics_init(void *gfx_device,
				    struct intel_display *display,
				    struct intel_display_guc_metrics *guc_metrics)
{
	guc_metrics->gfx_device = gfx_device;
	display->guc_metrics = guc_metrics;
}

/**
 * intel_display_guc_metrics_refresh_info - Refresh rate information
 * @display: Pointer to the intel_display struct that is in use by the gfx device.
 * @crtc_state: New CRTC state upon a modeset.
 *
 * To be called on a modeset. It gets current refresh interval in micro seconds
 * and pass back to the gfx device if the refresh_info_update callback is registered.
 */
void intel_display_guc_metrics_refresh_info(struct intel_display *display,
					    struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct drm_display_mode *mode = &crtc_state->hw.adjusted_mode;
	struct intel_display_guc_metrics *guc_metrics = display->guc_metrics;
	u32 interval_us;

	if (!guc_metrics)
		return;

	interval_us = crtc_state->hw.active ? DIV_ROUND_UP(1000000,
						drm_mode_vrefresh(mode)) : 0;

	if (guc_metrics->refresh_info_update)
		guc_metrics->refresh_info_update(guc_metrics->gfx_device,
						 crtc->pipe, interval_us,
						 crtc_state->vrr.enable);
}

/**
 * intel_display_guc_metrics_vblank - Vblank information
 * @display: Pointer to the intel_display struct that is in use by the gfx device.
 * @crtc: The Intel CRTC that received the vblank interrupt.
 *
 * To be called when a vblank is passed. It extracts the pipe from the intel_crtc
 * and pass back to the gfx device if the vblank_update callback is registered.
 */
void intel_display_guc_metrics_vblank(struct intel_display *display,
				      struct intel_crtc *crtc)
{
	struct intel_display_guc_metrics *guc_metrics = display->guc_metrics;

	if (!guc_metrics)
		return;

	if (guc_metrics->vblank_update)
		guc_metrics->vblank_update(guc_metrics->gfx_device, crtc->pipe);
}

/**
 * intel_display_guc_metrics_flip - Flip information
 * @display: Pointer to the intel_display struct that is in use by the gfx device.
 * @crtc_state: New CRTC state upon a page flip.
 * @plane: Which plane ID got the page flip.
 * @async_flip: A boolean to indicate if the page flip was async.
 *
 * To be called on a page flip. Then it pass the relevant information
 * to the gfx device if the flip_update callback is registered.
 */
void intel_display_guc_metrics_flip(struct intel_display *display,
				    const struct intel_crtc_state *crtc_state,
				    int plane, bool async_flip)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct intel_display_guc_metrics *guc_metrics = display->guc_metrics;

	if (!guc_metrics)
		return;

	if (guc_metrics->flip_update)
		guc_metrics->flip_update(guc_metrics->gfx_device,
					 crtc->pipe, plane, async_flip);
}
