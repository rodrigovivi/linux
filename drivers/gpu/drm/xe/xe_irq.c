/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#include <linux/sched/clock.h>

#include <drm/drm_managed.h>

#include "xe_device.h"
#include "xe_drv.h"
#include "xe_guc.h"
#include "xe_gt.h"
#include "xe_hw_engine.h"
#include "xe_mmio.h"

#include "../i915/i915_reg.h"

static void gen3_assert_iir_is_zero(struct xe_device *xe, i915_reg_t reg)
{
	struct xe_gt *gt = to_gt(xe);
	uint32_t val = xe_mmio_read32(gt, reg.reg);

	if (val == 0)
		return;

	drm_WARN(&xe->drm, 1,
		 "Interrupt register 0x%x is not zero: 0x%08x\n",
		 reg.reg, val);
	xe_mmio_write32(gt, reg.reg, 0xffffffff);
	xe_mmio_read32(gt, reg.reg);
	xe_mmio_write32(gt, reg.reg, 0xffffffff);
	xe_mmio_read32(gt, reg.reg);
}

static void gen3_irq_init(struct xe_device *xe,
			  i915_reg_t imr, u32 imr_val,
			  i915_reg_t ier, u32 ier_val,
			  i915_reg_t iir)
{
	struct xe_gt *gt = to_gt(xe);

	gen3_assert_iir_is_zero(xe, iir);

	xe_mmio_write32(gt, ier.reg, ier_val);
	xe_mmio_write32(gt, imr.reg, imr_val);
	xe_mmio_read32(gt, imr.reg);
}
#define GEN3_IRQ_INIT(xe, type, imr_val, ier_val) \
	gen3_irq_init((xe), \
		      type##IMR, imr_val, \
		      type##IER, ier_val, \
		      type##IIR)

static void gen3_irq_reset(struct xe_device *xe, i915_reg_t imr,
			   i915_reg_t iir, i915_reg_t ier)
{
	struct xe_gt *gt = to_gt(xe);

	xe_mmio_write32(gt, imr.reg, 0xffffffff);
	xe_mmio_read32(gt, imr.reg);

	xe_mmio_write32(gt, ier.reg, 0);

	/* IIR can theoretically queue up two events. Be paranoid. */
	xe_mmio_write32(gt, iir.reg, 0xffffffff);
	xe_mmio_read32(gt, iir.reg);
	xe_mmio_write32(gt, iir.reg, 0xffffffff);
	xe_mmio_read32(gt, iir.reg);
}
#define GEN3_IRQ_RESET(xe, type) \
	gen3_irq_reset((xe), type##IMR, type##IIR, type##IER)

static uint32_t gen11_intr_disable(struct xe_device *xe)
{
	struct xe_gt *gt = to_gt(xe);

	xe_mmio_write32(gt, GEN11_GFX_MSTR_IRQ.reg, 0);

	/*
	 * Now with master disabled, get a sample of level indications
	 * for this interrupt. Indications will be cleared on related acks.
	 * New indications can and will light up during processing,
	 * and will generate new interrupt after enabling master.
	 */
	return xe_mmio_read32(gt, GEN11_GFX_MSTR_IRQ.reg);
}

static inline void gen11_intr_enable(struct xe_device *xe, bool stall)
{
	struct xe_gt *gt = to_gt(xe);

	xe_mmio_write32(gt, GEN11_GFX_MSTR_IRQ.reg, GEN11_MASTER_IRQ);
	if (stall)
		xe_mmio_read32(gt, GEN11_GFX_MSTR_IRQ.reg);
}

static void gen11_gt_irq_postinstall(struct xe_device *xe)
{
	struct xe_gt *gt = to_gt(xe);
	uint32_t irqs, dmask, smask;

	irqs = GT_RENDER_USER_INTERRUPT |
	       GT_CS_MASTER_ERROR_INTERRUPT |
	       GT_CONTEXT_SWITCH_INTERRUPT |
	       GT_WAIT_SEMAPHORE_INTERRUPT;

	dmask = irqs << 16 | irqs;
	smask = irqs << 16;

	/* Enable RCS, BCS, VCS and VECS class interrupts. */
	xe_mmio_write32(gt, GEN11_RENDER_COPY_INTR_ENABLE.reg, dmask);
	xe_mmio_write32(gt, GEN11_VCS_VECS_INTR_ENABLE.reg, dmask);

	/* Unmask irqs on RCS, BCS, VCS and VECS engines. */
	xe_mmio_write32(gt, GEN11_RCS0_RSVD_INTR_MASK.reg, ~smask);
	xe_mmio_write32(gt, GEN11_BCS_RSVD_INTR_MASK.reg, ~smask);
	xe_mmio_write32(gt, GEN11_VCS0_VCS1_INTR_MASK.reg, ~dmask);
	xe_mmio_write32(gt, GEN11_VCS2_VCS3_INTR_MASK.reg, ~dmask);
	//if (HAS_ENGINE(gt, VCS4) || HAS_ENGINE(gt, VCS5))
	//	intel_uncore_write(uncore, GEN12_VCS4_VCS5_INTR_MASK, ~dmask);
	//if (HAS_ENGINE(gt, VCS6) || HAS_ENGINE(gt, VCS7))
	//	intel_uncore_write(uncore, GEN12_VCS6_VCS7_INTR_MASK, ~dmask);
	xe_mmio_write32(gt, GEN11_VECS0_VECS1_INTR_MASK.reg, ~dmask);
	//if (HAS_ENGINE(gt, VECS2) || HAS_ENGINE(gt, VECS3))
	//	intel_uncore_write(uncore, GEN12_VECS2_VECS3_INTR_MASK, ~dmask);

	/*
	 * RPS interrupts will get enabled/disabled on demand when RPS itself
	 * is enabled/disabled.
	 */
	/* TODO: gt->pm_ier, gt->pm_imr */
	xe_mmio_write32(gt, GEN11_GPM_WGBOXPERF_INTR_ENABLE.reg, 0);
	xe_mmio_write32(gt, GEN11_GPM_WGBOXPERF_INTR_MASK.reg,  ~0);

	/* Same thing for GuC interrupts */
	xe_mmio_write32(gt, GEN11_GUC_SG_INTR_ENABLE.reg, 0);
	xe_mmio_write32(gt, GEN11_GUC_SG_INTR_MASK.reg,  ~0);
}

static void gen11_irq_postinstall(struct xe_device *xe)
{
	/* TODO: PCH */

	gen11_gt_irq_postinstall(xe);

	/* TODO: Display */

	GEN3_IRQ_INIT(xe, GEN11_GU_MISC_, ~GEN11_GU_MISC_GSE, GEN11_GU_MISC_GSE);

	gen11_intr_enable(xe, true);
}

static uint32_t
gen11_gt_engine_identity(struct xe_device *xe,
			 const unsigned int bank,
			 const unsigned int bit)
{
	struct xe_gt *gt = to_gt(xe);
	uint32_t timeout_ts;
	uint32_t ident;

	lockdep_assert_held(&xe->irq.lock);

	xe_mmio_write32(gt, GEN11_IIR_REG_SELECTOR(bank).reg, BIT(bit));

	/*
	 * NB: Specs do not specify how long to spin wait,
	 * so we do ~100us as an educated guess.
	 */
	timeout_ts = (local_clock() >> 10) + 100;
	do {
		ident = xe_mmio_read32(gt, GEN11_INTR_IDENTITY_REG(bank).reg);
	} while (!(ident & GEN11_INTR_DATA_VALID) &&
		 !time_after32(local_clock() >> 10, timeout_ts));

	if (unlikely(!(ident & GEN11_INTR_DATA_VALID))) {
		drm_err(&xe->drm, "INTR_IDENTITY_REG%u:%u 0x%08x not valid!\n",
			bank, bit, ident);
		return 0;
	}

	xe_mmio_write32(gt, GEN11_INTR_IDENTITY_REG(bank).reg,
			GEN11_INTR_DATA_VALID);

	return ident;
}

static void
gen11_gt_other_irq_handler(struct xe_gt *gt, const u8 instance, const u16 iir)
{
	if (instance == OTHER_GUC_INSTANCE)
		return xe_guc_irq_handler(&gt->uc.guc, iir);
}

static void gen11_gt_irq_handler(struct xe_device *xe, uint32_t master_ctl)
{
	struct xe_gt *gt = to_gt(xe);
	unsigned int bank, bit;
	long unsigned int intr_dw;
	uint32_t identity[32];
	uint16_t instance, intr_vec;
	enum xe_engine_class class;
	struct xe_hw_engine *hwe;

	spin_lock(&xe->irq.lock);

	for (bank = 0; bank < 2; bank++) {
		if (!(master_ctl & GEN11_GT_DW_IRQ(bank)))
			continue;

		intr_dw = xe_mmio_read32(gt, GEN11_GT_INTR_DW(bank).reg);
		for_each_set_bit(bit, &intr_dw, 32)
			identity[bit] = gen11_gt_engine_identity(xe, bank, bit);
		xe_mmio_write32(gt, GEN11_GT_INTR_DW(bank).reg, intr_dw);

		for_each_set_bit(bit, &intr_dw, 32) {
			class = GEN11_INTR_ENGINE_CLASS(identity[bit]);
			instance = GEN11_INTR_ENGINE_INSTANCE(identity[bit]);
			intr_vec = GEN11_INTR_ENGINE_INTR(identity[bit]);

			if (class == XE_ENGINE_CLASS_OTHER) {
				gen11_gt_other_irq_handler(gt, instance,
							   intr_vec);
				continue;
			}

			hwe = xe_gt_hw_engine(gt, class, instance);
			if (!hwe)
				continue;

			xe_hw_engine_handle_irq(hwe, intr_vec);
		}
	}

	spin_unlock(&xe->irq.lock);
}

static irqreturn_t gen11_irq_handler(int irq, void *arg)
{
	struct xe_device *xe = arg;
	uint32_t master_ctl;

	master_ctl = gen11_intr_disable(xe);
	if (!master_ctl) {
		gen11_intr_enable(xe, false);
		return IRQ_NONE;
	}

	gen11_gt_irq_handler(xe, master_ctl);

	gen11_intr_enable(xe, false);

	/* TODO: Handle display interrupts */

	return IRQ_HANDLED;
}

static uint32_t dg1_intr_disable(struct xe_device *xe)
{
	struct xe_gt *gt = to_gt(xe);
	uint32_t val;

	/* First disable interrupts */
	xe_mmio_write32(gt, DG1_MSTR_TILE_INTR.reg, 0);

	/* Get the indication levels and ack the master unit */
	val = xe_mmio_read32(gt, DG1_MSTR_TILE_INTR.reg);
	if (unlikely(!val))
		return 0;

	xe_mmio_write32(gt, DG1_MSTR_TILE_INTR.reg, val);

	return val;
}

static void dg1_intr_enable(struct xe_device *xe, bool stall)
{
	struct xe_gt *gt = to_gt(xe);

	xe_mmio_write32(gt, DG1_MSTR_TILE_INTR.reg, DG1_MSTR_IRQ);
	if (stall)
		xe_mmio_read32(gt, DG1_MSTR_TILE_INTR.reg);
}

static void dg1_irq_postinstall(struct xe_device *xe)
{
	gen11_gt_irq_postinstall(xe);

	GEN3_IRQ_INIT(xe, GEN11_GU_MISC_, ~GEN11_GU_MISC_GSE, GEN11_GU_MISC_GSE);

	/* TODO: Display */

	dg1_intr_enable(xe, true);
}

static irqreturn_t dg1_irq_handler(int irq, void *arg)
{
	struct xe_device *xe = arg;
	struct xe_gt *gt = to_gt(xe);
	uint32_t master_tile_ctl, master_ctl;

	/* TODO: This really shouldn't be copied+pasted */

	master_tile_ctl = dg1_intr_disable(xe);
	if (!master_tile_ctl) {
		dg1_intr_enable(xe, false);
		return IRQ_NONE;
	}

	if (master_tile_ctl & DG1_MSTR_TILE(0)) {
		master_ctl = xe_mmio_read32(gt, GEN11_GFX_MSTR_IRQ.reg);
		xe_mmio_write32(gt, GEN11_GFX_MSTR_IRQ.reg, master_ctl);
	} else {
		drm_err(&xe->drm, "Tile not supported: 0x%08x\n",
			master_tile_ctl);
		dg1_intr_enable(xe, false);
		return IRQ_NONE;
	}

	gen11_gt_irq_handler(xe, master_ctl);

	dg1_intr_enable(xe, false);

	/* TODO: Handle display interrupts */

	return IRQ_HANDLED;
}

void gen11_gt_irq_reset(struct xe_device *xe)
{
	struct xe_gt *gt = to_gt(xe);

	/* Disable RCS, BCS, VCS and VECS class engines. */
	xe_mmio_write32(gt, GEN11_RENDER_COPY_INTR_ENABLE.reg,	 0);
	xe_mmio_write32(gt, GEN11_VCS_VECS_INTR_ENABLE.reg,	 0);

	/* Restore masks irqs on RCS, BCS, VCS and VECS engines. */
	xe_mmio_write32(gt, GEN11_RCS0_RSVD_INTR_MASK.reg,	~0);
	xe_mmio_write32(gt, GEN11_BCS_RSVD_INTR_MASK.reg,	~0);
	xe_mmio_write32(gt, GEN11_VCS0_VCS1_INTR_MASK.reg,	~0);
	xe_mmio_write32(gt, GEN11_VCS2_VCS3_INTR_MASK.reg,	~0);
//	if (HAS_ENGINE(gt, VCS4) || HAS_ENGINE(gt, VCS5))
//		xe_mmio_write32(xe, GEN12_VCS4_VCS5_INTR_MASK.reg,   ~0);
//	if (HAS_ENGINE(gt, VCS6) || HAS_ENGINE(gt, VCS7))
//		xe_mmio_write32(xe, GEN12_VCS6_VCS7_INTR_MASK.reg,   ~0);
	xe_mmio_write32(gt, GEN11_VECS0_VECS1_INTR_MASK.reg,	~0);
//	if (HAS_ENGINE(gt, VECS2) || HAS_ENGINE(gt, VECS3))
//		xe_mmio_write32(xe, GEN12_VECS2_VECS3_INTR_MASK.reg, ~0);

	xe_mmio_write32(gt, GEN11_GPM_WGBOXPERF_INTR_ENABLE.reg, 0);
	xe_mmio_write32(gt, GEN11_GPM_WGBOXPERF_INTR_MASK.reg,  ~0);
	xe_mmio_write32(gt, GEN11_GUC_SG_INTR_ENABLE.reg,	 0);
	xe_mmio_write32(gt, GEN11_GUC_SG_INTR_MASK.reg,		~0);
}

static void gen11_irq_reset(struct xe_device *xe)
{
	gen11_intr_disable(xe);

	gen11_gt_irq_reset(xe);

	/* TODO: Display */

	GEN3_IRQ_RESET(xe, GEN11_GU_MISC_);
	GEN3_IRQ_RESET(xe, GEN8_PCU_);
}

static void dg1_irq_reset(struct xe_device *xe)
{
	dg1_intr_disable(xe);

	gen11_gt_irq_reset(xe);

	/* TODO: Display */

	GEN3_IRQ_RESET(xe, GEN11_GU_MISC_);
	GEN3_IRQ_RESET(xe, GEN8_PCU_);
}

static void xe_irq_reset(struct xe_device *xe)
{
	if (GRAPHICS_VERx10(xe) >= 121) {
		dg1_irq_reset(xe);
	} else if (GRAPHICS_VER(xe) >= 11) {
		gen11_irq_reset(xe);
	} else {
		drm_err(&xe->drm, "No interrupt reset hook");
	}
}

static void xe_irq_postinstall(struct xe_device *xe)
{
	if (GRAPHICS_VERx10(xe) >= 121) {
		dg1_irq_postinstall(xe);
	} else if (GRAPHICS_VER(xe) >= 11) {
		gen11_irq_postinstall(xe);
	} else {
		drm_err(&xe->drm, "No interrupt postinstall hook");
	}
}

static irq_handler_t xe_irq_handler(struct xe_device *xe)
{
	if (GRAPHICS_VERx10(xe) >= 121) {
		return dg1_irq_handler;
	} else if (GRAPHICS_VER(xe) >= 11) {
		return gen11_irq_handler;
	} else {
		return NULL;
	}
}

static void irq_uninstall(struct drm_device *drm, void *arg)
{
	struct xe_device *xe = arg;
	int irq = to_pci_dev(xe->drm.dev)->irq;

	if (!xe->irq.enabled)
		return;

	xe->irq.enabled = false;
	xe_irq_reset(xe);
	free_irq(irq, xe);
}

int xe_irq_install(struct xe_device *xe)
{
	int irq = to_pci_dev(xe->drm.dev)->irq;
	static irq_handler_t irq_handler;
	int err;

	irq_handler = xe_irq_handler(xe);
	if (!irq_handler) {
		drm_err(&xe->drm, "No supported interrupt handler");
		return -EINVAL;
	}

	xe->irq.enabled = true;

	xe_irq_reset(xe);

	err = request_irq(irq, irq_handler,
			  IRQF_SHARED, DRIVER_NAME, xe);
	if (err < 0) {
		xe->irq.enabled = false;
		return err;
	}

	err = drmm_add_action_or_reset(&xe->drm, irq_uninstall, xe);
	if (err)
		return err;

	xe_irq_postinstall(xe);

	return err;
}
