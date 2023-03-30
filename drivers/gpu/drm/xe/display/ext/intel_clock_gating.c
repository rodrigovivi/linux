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

#include "intel_de.h"
#include "intel_display_trace.h"

#include "i915_drv.h"
#include "i915_reg.h"
#include "intel_clock_gating.h"
#include "intel_mchbar_regs.h"

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

void intel_clock_gating_init(struct drm_i915_private *dev_priv)
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
