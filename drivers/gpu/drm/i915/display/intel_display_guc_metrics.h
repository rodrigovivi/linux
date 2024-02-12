/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __INTEL_DISPLAY_GUC_METRICS_H__
#define __INTEL_DISPLAY_GUC_METRICS_H__

#include <linux/types.h>

struct intel_crtc;
struct intel_crtc_state;
struct intel_display;
struct intel_display_guc_metrics;

void intel_display_guc_metrics_init(void *gfx_device,
				    struct intel_display *display,
				    struct intel_display_guc_metrics *guc_metrics);
void intel_display_guc_metrics_refresh_info(struct intel_display *display,
					    struct intel_crtc_state *crtc_state);
void intel_display_guc_metrics_vblank(struct intel_display *display,
				      struct intel_crtc *intel_crtc);
void intel_display_guc_metrics_flip(struct intel_display *display,
				    const struct intel_crtc_state *crtc_state,
				    int plane, bool async_flip);
#endif
