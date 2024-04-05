// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include "intel_metrics.h"

#include <drm/drm_modes.h>
#include <drm/drm_print.h>

#include "i915_drv.h"
#include "intel_de.h"
#include "intel_display_types.h"

/**
 * Display Metrics
 *
 * Provide some display activity overview such as active refresh rates,
 * vblank activity and page flip activities.
 * For now it is informative debug only, but later it will be expanded
 * to be used for GT frequency selection by GuC SLPC.
 */

/*
 * An event using an work queue is used to avoid any disturbance in the
 * critical path that could cause performance impacts.
 */
struct display_event {
	struct work_struct work;
	struct drm_i915_private *i915;
	struct intel_display *display;
	bool is_vblank;
	int pipe;
	int plane;
	bool async_flip;
};

/*
 * Although we could simply save this inside our crtc structs, we are
 * already mimicking the GuC SLPC defition of the display data, for future
 * usage.
 */
#define MAX_PIPES		8
#define MAX_PLANES_PER_PIPE	8

struct display_global_info {
	u32 version:8;
	u32 num_pipes:4;
	u32 num_planes_per_pipe:4;
	u32 reserved_1:16;
	u32 refresh_count:16;
	u32 vblank_count:16;
	u32 flip_count:16;
	u32 reserved_2:16;
	u32 reserved_3[13];
} __packed;

struct display_refresh_info {
	u32 refresh_interval:16;
	u32 is_variable:1;
	u32 reserved:15;
} __packed;

/*
 * When used with GuC SLPC, the host must update each 32-bit part with a single
 * atomic write so that SLPC will read the contained bit fields together.
 * The host must update the two parts in order - total flip count and timestamp
 * first, vsync and async flip counts second.
 * Hence, these items are not defined with individual bitfields.
 */
#define FLIP_P1_LAST		REG_GENMASK(31, 7)
#define FLIP_P1_TOTAL_COUNT	REG_GENMASK(6, 0)
#define FLIP_P2_ASYNC_COUNT	REG_GENMASK(31, 16)
#define FLIP_P2_VSYNC_COUNT	REG_GENMASK(15, 0)

struct display_flip_metrics {
	u32 part1;
	u32 part2;
} __packed;

/*
 * When used with GuC SLPC, the host must update each 32-bit part with a single
 * atomic write, so that SLPC will read the count and timestamp together.
 * Hence, this item is not defined with individual bitfields.
 */
#define VBLANK_LAST	REG_GENMASK(31, 7)
#define VBLANK_COUNT	REG_GENMASK(6, 0)

struct intel_display_metrics {
	struct display_global_info global_info;
	struct display_refresh_info refresh_info[MAX_PIPES];
	u32 vblank_metrics[MAX_PIPES];
	struct display_flip_metrics
	flip_metrics[MAX_PIPES][MAX_PLANES_PER_PIPE];
} __packed;

/**
 * intel_metrics_refresh_info - Refresh rate information
 * @display: Pointer to the intel_display struct that is in use by the gfx device.
 * @crtc_state: New CRTC state upon a modeset.
 *
 * To be called on a modeset. It then saves the current refresh interval in
 * micro seconds.
 */
void intel_metrics_refresh_info(struct intel_display *display,
				struct intel_crtc_state *crtc_state)
{
	struct intel_display_metrics *metrics = display->metrics;
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct drm_display_mode *mode = &crtc_state->hw.adjusted_mode;
	u32 interval_us;

	if (!display->metrics)
		return;

	interval_us = crtc_state->hw.active ? DIV_ROUND_UP(1000000,
						drm_mode_vrefresh(mode)) : 0;

	metrics->refresh_info[crtc->pipe].refresh_interval = interval_us;
	metrics->refresh_info[crtc->pipe].is_variable = crtc_state->uapi.vrr_enabled;
	metrics->global_info.refresh_count += 1;
}

static void metrics_update_vblank(struct intel_display_metrics *metrics, int pipe,
				  u32 timestamp)
{
	u32 vblank;

	vblank = metrics->vblank_metrics[pipe];

	vblank = REG_FIELD_GET(VBLANK_COUNT, vblank);
	vblank = REG_FIELD_PREP(VBLANK_COUNT, vblank + 1);
	vblank |= REG_FIELD_PREP(VBLANK_LAST, timestamp);

	/* Write everything at once in preparation for the GuC SLPC requirement */
	metrics->vblank_metrics[pipe] = vblank;
	metrics->global_info.vblank_count += 1;
}

static void metrics_update_flip(struct intel_display_metrics *metrics, int pipe,
				int plane, bool async_flip, u32 timestamp)
{
	u32 part1, part2, count;

	part1 = metrics->flip_metrics[pipe][plane].part1;
	part2 = metrics->flip_metrics[pipe][plane].part2;

	part1 = REG_FIELD_GET(FLIP_P1_TOTAL_COUNT, part1);
	part1 = REG_FIELD_PREP(FLIP_P1_TOTAL_COUNT, part1 + 1);
	part1 |= REG_FIELD_PREP(FLIP_P1_LAST, timestamp);

	if (async_flip) {
		count = REG_FIELD_GET(FLIP_P2_ASYNC_COUNT, part2);
		part2 &= ~FLIP_P2_ASYNC_COUNT;
		part2 |= REG_FIELD_PREP(FLIP_P2_ASYNC_COUNT, count + 1);
	} else {
		count = REG_FIELD_GET(FLIP_P2_VSYNC_COUNT, part2);
		part2 &= ~FLIP_P2_VSYNC_COUNT;
		part2 |= REG_FIELD_PREP(FLIP_P2_VSYNC_COUNT, count + 1);
	}

	/*
	 * Write everything in this way and this order in preparation for the
	 * GuC SLPC requirement
	 */
	metrics->flip_metrics[pipe][plane].part1 = part1;
	metrics->flip_metrics[pipe][plane].part2 = part2;

	metrics->global_info.flip_count += 1;
}

/*
 * Let's use the same register GuC SLPC uses for timestamp.
 * It uses a register that is outside GT domain so GuC doesn't need
 * to wake the GT for reading during SLPC loop.
 * This is a single register regarding the GT, so we can read directly
 * from here, regarding the GT GuC is in.
 */
#define MCHBAR_MIRROR_BASE_SNB	0x140000
#define MCHBAR_BCLK_COUNT	_MMIO(MCHBAR_MIRROR_BASE_SNB + 0x5984)
#define MTL_BCLK_COUNT		_MMIO(0xc28)
#define   TIMESTAMP_MASK	REG_GENMASK(30, 6)

static u32 bclk_read_timestamp(struct drm_i915_private *i915)
{
	u32 timestamp;

	if (DISPLAY_VER(i915) >= 14)
		timestamp = intel_de_read(i915, MTL_BCLK_COUNT);
	else
		timestamp = intel_de_read(i915, MCHBAR_BCLK_COUNT);

	return REG_FIELD_GET(TIMESTAMP_MASK, timestamp);
}

static void display_event_work(struct work_struct *work)
{
	struct display_event *event = container_of(work, struct display_event, work);
	struct intel_display *display = event->display;
	u32 timestamp;

	timestamp = bclk_read_timestamp(event->i915);

	if (event->is_vblank) {
		metrics_update_vblank(display->metrics, event->pipe, timestamp);
	} else {
		metrics_update_flip(display->metrics, event->pipe, event->plane,
				    event->async_flip, timestamp);
	}

	kfree(event);
}

void intel_metrics_init(struct intel_display *display)
{
	struct intel_display_metrics *metrics;

	metrics = kzalloc(sizeof(*metrics), GFP_KERNEL);
	if (!metrics)
		return;

	metrics->global_info.version = 1;
	metrics->global_info.num_pipes = MAX_PIPES;
	metrics->global_info.num_planes_per_pipe = MAX_PLANES_PER_PIPE;

	display->metrics = metrics;
}

void intel_metrics_fini(struct intel_display *display)
{
	if (!display->metrics)
		return;

	kfree(display->metrics);
}

/**
 * intel_metrics_vblank - Vblank information
 * @display: Pointer to the intel_display struct that is in use by the gfx device.
 * @crtc: The Intel CRTC that received the vblank interrupt.
 *
 * To be called when a vblank is passed.
 */
void intel_metrics_vblank(struct intel_display *display,
			  struct intel_crtc *crtc)
{
	struct display_event *event;

	if (!display->metrics)
		return;

	event = kzalloc(sizeof(*event), GFP_ATOMIC);
	if (!event)
		return;

	INIT_WORK(&event->work, display_event_work);
	event->i915 = to_i915(crtc->base.dev);
	event->display = display;
	event->is_vblank = true;
	event->pipe = crtc->pipe;
	queue_work(system_highpri_wq, &event->work);
}

/**
 * intel_metrics_flip - Flip information
 * @display: Pointer to the intel_display struct that is in use by the gfx device.
 * @crtc_state: New CRTC state upon a page flip.
 * @plane: Which plane ID got the page flip.
 * @async_flip: A boolean to indicate if the page flip was async.
 *
 * To be called on a page flip.
 */
void intel_metrics_flip(struct intel_display *display,
			const struct intel_crtc_state *crtc_state,
			int plane, bool async_flip)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct display_event *event;

	if (!display->metrics)
		return;

	event = kzalloc(sizeof(*event), GFP_ATOMIC);
	if (!event)
		return;

	INIT_WORK(&event->work, display_event_work);
	event->i915 = to_i915(crtc->base.dev);
	event->display = display;
	event->pipe = crtc->pipe;
	event->plane = plane;
	event->async_flip = async_flip;
	queue_work(system_highpri_wq, &event->work);
}

void intel_metrics_show(struct intel_display *display, struct drm_printer *p)
{
	struct intel_display_metrics *metrics = display->metrics;
	int pipe, plane;
	u32 val;

	if (!metrics)
		return;

	drm_printf(p, "\nDisplay Metrics - Globals:\n");
	drm_printf(p, "\tVersion: %d\n", metrics->global_info.version);
	drm_printf(p, "\tNum Pipes: %d\n", metrics->global_info.num_pipes);
	drm_printf(p, "\tNum Planes per Pipe: %d\n",
		   metrics->global_info.num_planes_per_pipe);
	drm_printf(p, "\tGlobal Refresh Info Count: %d\n",
		   metrics->global_info.refresh_count);
	drm_printf(p, "\tGlobal Vblank Count: %d\n",
		   metrics->global_info.vblank_count);
	drm_printf(p, "\tGlobal Flip Count: %d\n",
		   metrics->global_info.flip_count);

	for (pipe = 0; pipe < MAX_PIPES; pipe++) {
		if (!metrics->refresh_info[pipe].refresh_interval)
			continue;

		drm_printf(p, "\nDisplay Metrics - Refresh Info - Pipe[%d]:\n",
			   pipe);
		drm_printf(p, "\tRefresh Interval: %d\n",
			   metrics->refresh_info[pipe].refresh_interval);
		drm_printf(p, "\tIS VRR: %d\n",
			   metrics->refresh_info[pipe].is_variable);

		drm_printf(p, "Display Metrics - Vblank Info - Pipe[%d]:\n",
			   pipe);
		val = metrics->vblank_metrics[pipe];
		drm_printf(p, "\tVBlank Last Timestamp: %x\n",
			   REG_FIELD_GET(VBLANK_LAST, val));
		drm_printf(p, "\tVBlank Count: %d\n",
			   REG_FIELD_GET(VBLANK_COUNT, val));

		drm_printf(p, "Display Metrics - Flip Info - Pipe[%d]:\n", pipe);
		for (plane = 0; plane < MAX_PLANES_PER_PIPE; plane++) {
			val = metrics->flip_metrics[pipe][plane].part1;
			if (!val)
				continue;

			drm_printf(p, "\tFlip Info - Plane[%d]:\n", plane);
			drm_printf(p, "\t\tFlip Last Timestamp: %x\n",
				   REG_FIELD_GET(FLIP_P1_LAST, val));
			drm_printf(p, "\t\tFlip Total Count: %d\n",
				   REG_FIELD_GET(FLIP_P1_TOTAL_COUNT, val));

			val = metrics->flip_metrics[pipe][plane].part2;

			drm_printf(p, "\t\tFlip Async Count: %d\n",
				   REG_FIELD_GET(FLIP_P2_ASYNC_COUNT, val));
			drm_printf(p, "\t\tFlip Vsync Count: %d\n",
				   REG_FIELD_GET(FLIP_P2_VSYNC_COUNT, val));
		}
	}
}
