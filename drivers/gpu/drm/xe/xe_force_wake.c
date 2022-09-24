// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <drm/drm_util.h>

#include "xe_force_wake.h"
#include "xe_gt.h"
#include "xe_mmio.h"
#include "../i915/gt/intel_gt_regs.h"

#include <linux/delay.h>
/*
 * FIXME: This header has been deemed evil and we need to kill it. Temporarily
 * including so we can use '__mask_next_bit'.
 */
#include "i915_utils.h"

#define XE_FORCE_WAKE_ACK_TIMEOUT_MS	50

static struct xe_gt *
fw_to_gt(struct xe_force_wake *fw)
{
	return fw->gt;
}

static struct xe_device *
fw_to_xe(struct xe_force_wake *fw)
{
	return gt_to_xe(fw_to_gt(fw));
}

static void domain_init(struct xe_force_wake_domain *domain,
			enum xe_force_wake_domain_id id,
			u32 reg, u32 ack, u32 val, u32 mask)
{
	domain->id = id;
	domain->reg_ctl = reg;
	domain->reg_ack = ack;
	domain->val = val;
	domain->mask = mask;
}

#define FORCEWAKE_ACK_GT_MTL                 _MMIO(0xdfc)

void xe_force_wake_init(struct xe_gt *gt, struct xe_force_wake *fw)
{
	struct xe_device *xe = gt_to_xe(gt);
	int i, j;

	fw->gt = gt;
	spin_lock_init(&fw->lock);

	/* Assuming gen11+ so assert this assumption is correct */
	XE_BUG_ON(GRAPHICS_VER(gt_to_xe(gt)) < 11);

	if (xe->info.platform == XE_METEORLAKE) {
		domain_init(&fw->domains[XE_FW_DOMAIN_ID_GT],
			    XE_FW_DOMAIN_ID_GT,
			    FORCEWAKE_GT_GEN9.reg,
			    FORCEWAKE_ACK_GT_MTL.reg,
			    BIT(0), BIT(16));
	} else {
		domain_init(&fw->domains[XE_FW_DOMAIN_ID_GT],
			    XE_FW_DOMAIN_ID_GT,
			    FORCEWAKE_GT_GEN9.reg,
			    FORCEWAKE_ACK_GT_GEN9.reg,
			    BIT(0), BIT(16));
	}

	if (!xe_gt_is_media_type(gt))
		domain_init(&fw->domains[XE_FW_DOMAIN_ID_RENDER],
			    XE_FW_DOMAIN_ID_RENDER,
			    FORCEWAKE_RENDER_GEN9.reg,
			    FORCEWAKE_ACK_RENDER_GEN9.reg,
			    BIT(0), BIT(16));

	for (i = XE_HW_ENGINE_VCS0, j = 0; i <= XE_HW_ENGINE_VCS7; ++i, ++j) {
		if (!(gt->info.engine_mask & BIT(i)))
			continue;

		domain_init(&fw->domains[XE_FW_DOMAIN_ID_MEDIA_VDBOX0 + j],
			    XE_FW_DOMAIN_ID_MEDIA_VDBOX0 + j,
			    FORCEWAKE_MEDIA_VDBOX_GEN11(j).reg,
			    FORCEWAKE_ACK_MEDIA_VDBOX_GEN11(j).reg,
			    BIT(0), BIT(16));
	}

	for (i = XE_HW_ENGINE_VECS0, j =0; i <= XE_HW_ENGINE_VECS3; ++i, ++j) {
		if (!(gt->info.engine_mask & BIT(i)))
			continue;

		domain_init(&fw->domains[XE_FW_DOMAIN_ID_MEDIA_VEBOX0 + j],
			    XE_FW_DOMAIN_ID_MEDIA_VEBOX0 + j,
			    FORCEWAKE_MEDIA_VEBOX_GEN11(j).reg,
			    FORCEWAKE_ACK_MEDIA_VEBOX_GEN11(j).reg,
			    BIT(0), BIT(16));
	}
}

void xe_force_wake_prune(struct xe_gt *gt, struct xe_force_wake *fw)
{
	int i, j;

	/* Call after fuses have been read, prune domains that are fused off */

	for (i = XE_HW_ENGINE_VCS0, j = 0; i <= XE_HW_ENGINE_VCS7; ++i, ++j)
		if (!(gt->info.engine_mask & BIT(i)))
			fw->domains[XE_FW_DOMAIN_ID_MEDIA_VDBOX0 + j].reg_ctl = 0;

	for (i = XE_HW_ENGINE_VECS0, j =0; i <= XE_HW_ENGINE_VECS3; ++i, ++j)
		if (!(gt->info.engine_mask & BIT(i)))
			fw->domains[XE_FW_DOMAIN_ID_MEDIA_VEBOX0 + j].reg_ctl = 0;
}

static void domain_wake(struct xe_gt *gt, struct xe_force_wake_domain *domain)
{
	xe_mmio_write32_nofw(gt, domain->reg_ctl, domain->mask | domain->val);
}

static int domain_wake_wait(struct xe_gt *gt,
			    struct xe_force_wake_domain *domain)
{
	return xe_mmio_wait32_nofw(gt, domain->reg_ack, domain->val, domain->val,
				   XE_FORCE_WAKE_ACK_TIMEOUT_MS);
}

static void domain_sleep(struct xe_gt *gt, struct xe_force_wake_domain *domain)
{
	xe_mmio_write32_nofw(gt, domain->reg_ctl, domain->mask);
}

static int domain_sleep_wait(struct xe_gt *gt,
			     struct xe_force_wake_domain *domain)
{
	return xe_mmio_wait32_nofw(gt, domain->reg_ack, 0, domain->val,
				   XE_FORCE_WAKE_ACK_TIMEOUT_MS);
}

#define for_each_fw_domain_masked(domain__, mask__, fw__, tmp__) \
	for (tmp__ = (mask__); tmp__ ;) \
		for_each_if((domain__ = ((fw__)->domains + \
					 __mask_next_bit(tmp__))) && \
					 domain__->reg_ctl)

#define for_each_fw_domain(domain__, fw__, tmp__) \
	for_each_fw_domain_masked(domain__, XE_FORCEWAKE_ALL, fw__, tmp__)

static const char *domain_name[] = {
	[XE_FW_DOMAIN_ID_GT] = "GT",
	[XE_FW_DOMAIN_ID_RENDER] = "Render",
	[XE_FW_DOMAIN_ID_MEDIA] = "Media",
	[XE_FW_DOMAIN_ID_MEDIA_VDBOX0] = "VDBOX0",
	[XE_FW_DOMAIN_ID_MEDIA_VDBOX1] = "VDBOX1",
	[XE_FW_DOMAIN_ID_MEDIA_VDBOX2] = "VDBOX2",
	[XE_FW_DOMAIN_ID_MEDIA_VDBOX3] = "VDBOX3",
	[XE_FW_DOMAIN_ID_MEDIA_VDBOX4] = "VDBOX4",
	[XE_FW_DOMAIN_ID_MEDIA_VDBOX5] = "VDBOX5",
	[XE_FW_DOMAIN_ID_MEDIA_VDBOX6] = "VDBOX6",
	[XE_FW_DOMAIN_ID_MEDIA_VDBOX7] = "VDBOX7",
	[XE_FW_DOMAIN_ID_MEDIA_VEBOX0] = "VEBOX0",
	[XE_FW_DOMAIN_ID_MEDIA_VEBOX1] = "VEBOX1",
	[XE_FW_DOMAIN_ID_MEDIA_VEBOX2] = "VEBOX2",
	[XE_FW_DOMAIN_ID_MEDIA_VEBOX3] = "VEBOX3",
	[XE_FW_DOMAIN_ID_GSC] = "GSC",
};

void xe_force_wake_print(struct xe_force_wake *fw, struct drm_printer *p)
{
	struct xe_force_wake_domain *domain;
	enum xe_force_wake_domains tmp;

	for_each_fw_domain(domain, fw, tmp) {
		drm_printf(p, "domain:%s\n", domain_name[domain->id]);
		drm_printf(p, "\tawake:%s\n",
			   str_yes_no(fw->awake_domains & BIT(domain->id)));
		drm_printf(p, "\trefs:%d\n", domain->ref);
	}
}

int xe_force_wake_get(struct xe_force_wake *fw,
		      enum xe_force_wake_domains domains)
{
	struct xe_device *xe = fw_to_xe(fw);
	struct xe_gt *gt = fw_to_gt(fw);
	struct xe_force_wake_domain *domain;
	enum xe_force_wake_domains tmp, woken = 0;
	int ret, ret2 = 0;

	spin_lock_irq(&fw->lock);
	for_each_fw_domain_masked(domain, domains, fw, tmp) {
		if (!domain->ref++) {
			woken |= BIT(domain->id);
			domain_wake(gt, domain);
		}
	}
	for_each_fw_domain_masked(domain, woken, fw, tmp) {
		ret = domain_wake_wait(gt, domain);
		ret2 |= ret;
		if (ret)
			drm_notice(&xe->drm, "Force wake domain (%d) failed to ack wake, ret=%d\n",
				   domain->id, ret);
	}
	fw->awake_domains |= woken;
	spin_unlock_irq(&fw->lock);

	return ret2;
}

int xe_force_wake_put(struct xe_force_wake *fw,
		      enum xe_force_wake_domains domains)
{
	struct xe_device *xe = fw_to_xe(fw);
	struct xe_gt *gt = fw_to_gt(fw);
	struct xe_force_wake_domain *domain;
	enum xe_force_wake_domains tmp, sleep = 0;
	int ret, ret2 = 0;

	spin_lock_irq(&fw->lock);
	for_each_fw_domain_masked(domain, domains, fw, tmp) {
		if (!--domain->ref) {
			sleep |= BIT(domain->id);
			domain_sleep(gt, domain);
		}
	}
	for_each_fw_domain_masked(domain, sleep, fw, tmp) {
		ret = domain_sleep_wait(gt, domain);
		ret2 |= ret;
		if (ret)
			drm_notice(&xe->drm, "Force wake domain (%d) failed to ack sleep, ret=%d\n",
				   domain->id, ret);
	}
	fw->awake_domains &= ~sleep;
	spin_unlock_irq(&fw->lock);

	return ret2;
}
