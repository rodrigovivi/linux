/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#include "intel_wakeref.h"

enum i915_drm_suspend_mode {
	I915_DRM_SUSPEND_IDLE,
	I915_DRM_SUSPEND_MEM,
	I915_DRM_SUSPEND_HIBERNATE,
};

static inline void disable_rpm_wakeref_asserts(void *rpm)
{
}

static inline void enable_rpm_wakeref_asserts(void *rpm)
{
}
