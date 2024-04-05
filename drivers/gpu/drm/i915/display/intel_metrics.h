/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __INTEL_METRICS_H__
#define __INTEL_METRICS_H__

#include <linux/types.h>

struct drm_printer;
struct intel_crtc;
struct intel_crtc_state;
struct intel_display;

void intel_metrics_refresh_info(struct intel_display *display,
				struct intel_crtc_state *crtc_state);
void intel_metrics_vblank(struct intel_display *display,
			  struct intel_crtc *intel_crtc);
void intel_metrics_flip(struct intel_display *display,
			const struct intel_crtc_state *crtc_state,
			int plane, bool async_flip);
void intel_metrics_init(struct intel_display *display);
void intel_metrics_fini(struct intel_display *display);
void intel_metrics_show(struct intel_display *display, struct drm_printer *p);

#endif
