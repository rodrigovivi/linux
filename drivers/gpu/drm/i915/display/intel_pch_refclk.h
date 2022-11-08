/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef _INTEL_PCH_REFCLK_H_
#define _INTEL_PCH_REFCLK_H_

#include <linux/types.h>

struct drm_i915_private;
struct intel_crtc_state;

#ifdef I915
void lpt_program_iclkip(const struct intel_crtc_state *crtc_state);
void lpt_disable_iclkip(struct drm_i915_private *dev_priv);
int lpt_get_iclkip(struct drm_i915_private *dev_priv);
int lpt_iclkip(const struct intel_crtc_state *crtc_state);

void intel_init_pch_refclk(struct drm_i915_private *dev_priv);
void lpt_disable_clkout_dp(struct drm_i915_private *dev_priv);
#else
#define lpt_program_iclkip(cstate) do { } while (0)
#define lpt_disable_iclkip(xe) do { } while (0)
#define lpt_get_iclkip(xe) (WARN_ON(-ENODEV))
#define intel_init_pch_refclk(xe) do { } while (0)
#define lpt_disable_clkout_dp(xe) do { } while (0)
#endif

#endif
