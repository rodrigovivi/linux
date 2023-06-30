// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include "i915_drv.h"
#include "i915_irq.h"
#include "i915_reg.h"
#include "intel_display_irq.h"
#include "intel_display_types.h"
#include "intel_hotplug.h"
#include "intel_hotplug_irq.h"
#include "intel_uncore.h"

void gen3_irq_reset(struct intel_uncore *uncore, i915_reg_t imr,
		    i915_reg_t iir, i915_reg_t ier)
{
	intel_uncore_write(uncore, imr, 0xffffffff);
	intel_uncore_posting_read(uncore, imr);

	intel_uncore_write(uncore, ier, 0);

	/* IIR can theoretically queue up two events. Be paranoid. */
	intel_uncore_write(uncore, iir, 0xffffffff);
	intel_uncore_posting_read(uncore, iir);
	intel_uncore_write(uncore, iir, 0xffffffff);
	intel_uncore_posting_read(uncore, iir);
}

/*
 * We should clear IMR at preinstall/uninstall, and just check at postinstall.
 */
void gen3_assert_iir_is_zero(struct intel_uncore *uncore, i915_reg_t reg)
{
	struct xe_device *xe = container_of(uncore, struct xe_device, uncore);
	u32 val = intel_uncore_read(uncore, reg);

	if (val == 0)
		return;

	drm_WARN(&xe->drm, 1,
		 "Interrupt register 0x%x is not zero: 0x%08x\n",
		 i915_mmio_reg_offset(reg), val);
	intel_uncore_write(uncore, reg, 0xffffffff);
	intel_uncore_posting_read(uncore, reg);
	intel_uncore_write(uncore, reg, 0xffffffff);
	intel_uncore_posting_read(uncore, reg);
}

void gen3_irq_init(struct intel_uncore *uncore,
		   i915_reg_t imr, u32 imr_val,
		   i915_reg_t ier, u32 ier_val,
		   i915_reg_t iir)
{
	gen3_assert_iir_is_zero(uncore, iir);

	intel_uncore_write(uncore, ier, ier_val);
	intel_uncore_write(uncore, imr, imr_val);
	intel_uncore_posting_read(uncore, imr);
}

/**
 * DOC: interrupt handling
 *
 * These functions provide the basic support for enabling and disabling the
 * interrupt handling support. There's a lot more functionality in i915_irq.c
 * and related files, but that will be described in separate chapters.
 */

void gen11_display_irq_postinstall(struct drm_i915_private *dev_priv)
{
	if (!HAS_DISPLAY(dev_priv))
		return;

	if (INTEL_PCH_TYPE(dev_priv) >= PCH_ICP)
		icp_irq_postinstall(dev_priv);

	gen11_de_irq_postinstall(dev_priv);
}

void intel_display_irq_init(struct drm_i915_private *dev_priv)
{
	struct drm_device *dev = &dev_priv->drm;

	if (!HAS_DISPLAY(dev_priv))
		return;

	dev->vblank_disable_immediate = true;

	/* Most platforms treat the display irq block as an always-on
	 * power domain. vlv/chv can disable it at runtime and need
	 * special care to avoid writing any of the display block registers
	 * outside of the power domain. We defer setting up the display irqs
	 * in this case to the runtime pm.
	 */
	dev_priv->display_irqs_enabled = true;
	if (IS_VALLEYVIEW(dev_priv) || IS_CHERRYVIEW(dev_priv))
		dev_priv->display_irqs_enabled = false;

	intel_hotplug_irq_init(dev_priv);
}

void intel_display_irq_uninstall(struct drm_i915_private *dev_priv)
{
	intel_hpd_cancel_work(dev_priv);
}

bool intel_irqs_enabled(struct xe_device *xe)
{
	return xe->irq.enabled;
}

void intel_synchronize_irq(struct xe_device *xe)
{
	synchronize_irq(to_pci_dev(xe->drm.dev)->irq);
}
