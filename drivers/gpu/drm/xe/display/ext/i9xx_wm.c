// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include "i9xx_wm.h"

bool ilk_disable_lp_wm(struct drm_i915_private *i915)
{
	return false;
}

void ilk_wm_sanitize(struct drm_i915_private *i915)
{
}

bool intel_set_memory_cxsr(struct drm_i915_private *i915, bool enable)
{
	return true;
}

void i9xx_wm_init(struct drm_i915_private *i915)
{
}
