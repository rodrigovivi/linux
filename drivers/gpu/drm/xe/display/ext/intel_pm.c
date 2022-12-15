/*
 * Copyright © 2012 Intel Corporation
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
 * Authors:
 *    Eugeni Dodonov <eugeni.dodonov@intel.com>
 *
 */

#include "display/intel_de.h"
#include "display/intel_display_trace.h"
#include "display/skl_watermark.h"

#include "../i915_drv.h"
#include "../i915/intel_mchbar_regs.h"
#include "intel_pm.h"

bool intel_set_memory_cxsr(struct drm_i915_private *dev_priv, bool enable)
{
	return false;
}

int ilk_wm_max_level(const struct drm_i915_private *dev_priv)
{
	/* how many WM levels are we expecting */
	if (HAS_HW_SAGV_WM(dev_priv))
		return 5;
	else if (DISPLAY_VER(dev_priv) >= 9)
		return 7;
	else if (IS_HASWELL(dev_priv) || IS_BROADWELL(dev_priv))
		return 4;
	else if (DISPLAY_VER(dev_priv) >= 6)
		return 3;
	else
		return 2;
}

void intel_print_wm_latency(struct drm_i915_private *dev_priv,
			    const char *name, const u16 wm[])
{
	int level, max_level = ilk_wm_max_level(dev_priv);

	for (level = 0; level <= max_level; level++) {
		unsigned int latency = wm[level];

		if (latency == 0) {
			drm_dbg_kms(&dev_priv->drm,
				    "%s WM%d latency not provided\n",
				    name, level);
			continue;
		}

		/*
		 * - latencies are in us on gen9.
		 * - before then, WM1+ latency values are in 0.5us units
		 */
		if (DISPLAY_VER(dev_priv) >= 9)
			latency *= 10;
		else if (level > 0)
			latency *= 5;

		drm_dbg_kms(&dev_priv->drm,
			    "%s WM%d latency %u (%u.%u usec)\n", name, level,
			    wm[level], latency / 10, latency % 10);
	}
}

static void gen12lp_init_clock_gating(struct drm_i915_private *dev_priv)
{
	/* Wa_1409120013 */
	if (DISPLAY_VER(dev_priv) == 12)
		intel_de_write(dev_priv, ILK_DPFC_CHICKEN(INTEL_FBC_A),
				   DPFC_CHICKEN_COMP_DUMMY_PIXEL);

	/* Wa_1409825376:tgl (pre-prod)*/
	if (IS_TGL_DISPLAY_STEP(dev_priv, STEP_A0, STEP_C0))
		intel_de_write(dev_priv, GEN9_CLKGATE_DIS_3, intel_de_read(dev_priv, GEN9_CLKGATE_DIS_3) |
			   TGL_VRH_GATING_DIS);

	/* Wa_14013723622:tgl,rkl,dg1,adl-s */
	if (DISPLAY_VER(dev_priv) == 12)
		intel_de_rmw(dev_priv, CLKREQ_POLICY,
				 CLKREQ_POLICY_MEM_UP_OVRD, 0);
}

static void adlp_init_clock_gating(struct drm_i915_private *dev_priv)
{
	gen12lp_init_clock_gating(dev_priv);

	/* Wa_22011091694:adlp */
	intel_de_rmw(dev_priv, GEN9_CLKGATE_DIS_5, 0, DPCE_GATING_DIS);

	/* Bspec/49189 Initialize Sequence */
	intel_de_rmw(dev_priv, GEN8_CHICKEN_DCPR_1, DDI_CLOCK_REG_ACCESS, 0);
}

static void dg1_init_clock_gating(struct drm_i915_private *dev_priv)
{
	gen12lp_init_clock_gating(dev_priv);

	/* Wa_1409836686:dg1[a0] */
	if (IS_DG1_GRAPHICS_STEP(dev_priv, STEP_A0, STEP_B0))
		intel_de_write(dev_priv, GEN9_CLKGATE_DIS_3, intel_de_read(dev_priv, GEN9_CLKGATE_DIS_3) |
			   DPT_GATING_DIS);
}

static void xehpsdv_init_clock_gating(struct drm_i915_private *dev_priv)
{
	/* Wa_22010146351:xehpsdv */
	if (IS_XEHPSDV_GRAPHICS_STEP(dev_priv, STEP_A0, STEP_B0))
		intel_de_rmw(dev_priv, XEHP_CLOCK_GATE_DIS, 0, SGR_DIS);
}

static void dg2_init_clock_gating(struct drm_i915_private *i915)
{
	/* Wa_22010954014:dg2 */
	intel_de_rmw(i915, XEHP_CLOCK_GATE_DIS, 0,
			 SGSI_SIDECLK_DIS);

	/*
	 * Wa_14010733611:dg2_g10
	 * Wa_22010146351:dg2_g10
	 */
	if (IS_DG2_GRAPHICS_STEP(i915, G10, STEP_A0, STEP_B0))
		intel_de_rmw(i915, XEHP_CLOCK_GATE_DIS, 0,
				 SGR_DIS | SGGI_DIS);
}

static void pvc_init_clock_gating(struct drm_i915_private *dev_priv)
{
	/* Wa_14012385139:pvc */
	if (IS_PVC_BD_STEP(dev_priv, STEP_A0, STEP_B0))
		intel_de_rmw(dev_priv, XEHP_CLOCK_GATE_DIS, 0, SGR_DIS);

	/* Wa_22010954014:pvc */
	if (IS_PVC_BD_STEP(dev_priv, STEP_A0, STEP_B0))
		intel_de_rmw(dev_priv, XEHP_CLOCK_GATE_DIS, 0, SGSI_SIDECLK_DIS);
}

void intel_init_clock_gating(struct drm_i915_private *dev_priv)
{
	if (IS_PONTEVECCHIO(dev_priv))
		pvc_init_clock_gating(dev_priv);
	else if (IS_DG2(dev_priv))
		dg2_init_clock_gating(dev_priv);
	else if (IS_XEHPSDV(dev_priv))
		xehpsdv_init_clock_gating(dev_priv);
	else if (IS_ALDERLAKE_P(dev_priv))
		adlp_init_clock_gating(dev_priv);
	else if (IS_DG1(dev_priv))
		dg1_init_clock_gating(dev_priv);
	else if (GRAPHICS_VER(dev_priv) == 12)
		gen12lp_init_clock_gating(dev_priv);
	else {
		MISSING_CASE(INTEL_DEVID(dev_priv));
	}
}

/* Set up chip specific power management-related functions */
void intel_init_pm(struct drm_i915_private *dev_priv)
{
	skl_wm_init(dev_priv);
}

bool ilk_disable_lp_wm(struct drm_i915_private *dev_priv)
{
	return false;
}

bool intel_wm_plane_visible(const struct intel_crtc_state *crtc_state,
			    const struct intel_plane_state *plane_state)
{
	struct intel_plane *plane = to_intel_plane(plane_state->uapi.plane);

	/* FIXME check the 'enable' instead */
	if (!crtc_state->hw.active)
		return false;

	/*
	 * Treat cursor with fb as always visible since cursor updates
	 * can happen faster than the vrefresh rate, and the current
	 * watermark code doesn't handle that correctly. Cursor updates
	 * which set/clear the fb or change the cursor size are going
	 * to get throttled by intel_legacy_cursor_update() to work
	 * around this problem with the watermark code.
	 */
	if (plane->id == PLANE_CURSOR)
		return plane_state->hw.fb != NULL;
	else
		return plane_state->uapi.visible;
}
