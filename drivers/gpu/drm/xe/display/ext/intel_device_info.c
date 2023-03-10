/*
 * Copyright © 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include "intel_cdclk.h"
#include "intel_de.h"
#include "intel_display.h"
#include "intel_device_info.h"
#include "i915_drv.h"
#include <drm/drm_drv.h>

/**
 * intel_device_info_runtime_init - initialize runtime info
 * @dev_priv: the i915 device
 *
 * Determine various intel_device_info fields at runtime.
 *
 * Use it when either:
 *   - it's judged too laborious to fill n static structures with the limit
 *     when a simple if statement does the job,
 *   - run-time checks (eg read fuse/strap registers) are needed.
 *
 * This function needs to be called:
 *   - after the MMIO has been setup as we are reading registers,
 *   - after the PCH has been detected,
 *   - before the first usage of the fields it can tweak.
 */
void intel_device_info_runtime_init(struct drm_i915_private *dev_priv)
{
	struct intel_runtime_info *runtime = RUNTIME_INFO(dev_priv);
	enum pipe pipe;

	/* Wa_14011765242: adl-s A0,A1 */
	if (IS_ADLS_DISPLAY_STEP(dev_priv, STEP_A0, STEP_A2)) {
		for_each_pipe(dev_priv, pipe)
			runtime->num_scalers[pipe] = 0;
	} else if (DISPLAY_VER(dev_priv) >= 11) {
		for_each_pipe(dev_priv, pipe)
			runtime->num_scalers[pipe] = 2;
	} else if (DISPLAY_VER(dev_priv) >= 9) {
		runtime->num_scalers[PIPE_A] = 2;
		runtime->num_scalers[PIPE_B] = 2;
		runtime->num_scalers[PIPE_C] = 1;
	}

	if (DISPLAY_VER(dev_priv) >= 13 || HAS_D12_PLANE_MINIMIZATION(dev_priv)) {
		for_each_pipe(dev_priv, pipe)
			runtime->num_sprites[pipe] = 4;
	} else if (DISPLAY_VER(dev_priv) >= 11) {
		for_each_pipe(dev_priv, pipe)
			runtime->num_sprites[pipe] = 6;
	}

	if (HAS_DISPLAY(dev_priv) && DISPLAY_VER(dev_priv) >= 9) {
		u32 dfsm = intel_de_read(dev_priv, SKL_DFSM);

		if (dfsm & SKL_DFSM_PIPE_A_DISABLE) {
			runtime->pipe_mask &= ~BIT(PIPE_A);
			runtime->cpu_transcoder_mask &= ~BIT(TRANSCODER_A);
			runtime->fbc_mask &= ~BIT(INTEL_FBC_A);
		}
		if (dfsm & SKL_DFSM_PIPE_B_DISABLE) {
			runtime->pipe_mask &= ~BIT(PIPE_B);
			runtime->cpu_transcoder_mask &= ~BIT(TRANSCODER_B);
		}
		if (dfsm & SKL_DFSM_PIPE_C_DISABLE) {
			runtime->pipe_mask &= ~BIT(PIPE_C);
			runtime->cpu_transcoder_mask &= ~BIT(TRANSCODER_C);
		}

		if (DISPLAY_VER(dev_priv) >= 12 &&
		    (dfsm & TGL_DFSM_PIPE_D_DISABLE)) {
			runtime->pipe_mask &= ~BIT(PIPE_D);
			runtime->cpu_transcoder_mask &= ~BIT(TRANSCODER_D);
		}

		if (dfsm & SKL_DFSM_DISPLAY_HDCP_DISABLE)
			runtime->has_hdcp = 0;

		if (dfsm & SKL_DFSM_DISPLAY_PM_DISABLE)
			runtime->fbc_mask = 0;

		if (DISPLAY_VER(dev_priv) >= 11 && (dfsm & ICL_DFSM_DMC_DISABLE))
			runtime->has_dmc = 0;

		if (DISPLAY_VER(dev_priv) >= 10 &&
		    (dfsm & GLK_DFSM_DISPLAY_DSC_DISABLE))
			runtime->has_dsc = 0;
	}

	runtime->rawclk_freq = intel_read_rawclk(dev_priv);
	drm_dbg(&dev_priv->drm, "rawclk rate: %d kHz\n", runtime->rawclk_freq);
}

bool i915_vtd_active(struct drm_i915_private *i915)
{
	if (device_iommu_mapped(i915->drm.dev))
		return true;

	/* Running as a guest, we assume the host is enforcing VT'd */
	return i915_run_as_guest();
}

/* i915 specific, just put here for shutting it up */
int __i915_inject_probe_error(struct drm_i915_private *i915, int err,
							  const char *func, int line)
{
	return 0;
}
