// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <drm/drm_util.h>

#include "xe_force_wake_types.h"
#include "xe_mmio.h"
#include "../i915/i915_reg.h"

#define XE_FORCE_WAKE_ACK_TIMEOUT_MS	50

static struct xe_device *
fw_to_xe(struct xe_force_wake *fw)
{
	return container_of(fw, struct xe_device, fw);
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

void xe_force_wake_init(struct xe_force_wake *fw)
{
	mutex_init(&fw->lock);

	domain_init(&fw->domains[XE_FW_DOMAIN_ID_GT],
		    XE_FW_DOMAIN_ID_GT,
		    FORCEWAKE_GT_GEN9.reg,
		    FORCEWAKE_ACK_GT_GEN9.reg,
		    BIT(0), BIT(16));

	// FIXME - Setup all other FW domains
}

static void domain_wake(struct xe_device *xe,
			struct xe_force_wake_domain *domain)
{
	xe_mmio_write32(xe, domain->reg_ctl, domain->mask | domain->val);
}

static int domain_wake_wait(struct xe_device *xe,
			    struct xe_force_wake_domain *domain)
{
	return xe_mmio_wait32(xe, domain->reg_ack, domain->val, domain->val,
			      XE_FORCE_WAKE_ACK_TIMEOUT_MS);
}

static void domain_sleep(struct xe_device *xe,
			 struct xe_force_wake_domain *domain)
{
	xe_mmio_write32(xe, domain->reg_ctl, domain->mask);
}

static int domain_sleep_wait(struct xe_device *xe,
			     struct xe_force_wake_domain *domain)
{
	return xe_mmio_wait32(xe, domain->reg_ack, 0, domain->val,
			      XE_FORCE_WAKE_ACK_TIMEOUT_MS);
}

#define for_each_fw_domain_masked(domain__, mask__, fw__, tmp__) \
	for (tmp__ = (mask__); tmp__ ;) \
		for_each_if((domain__ = ((fw__)->domains + __mask_next_bit(tmp__))) && \
			    domain__->reg_ctl)

int xe_force_wake_get(struct xe_force_wake *fw,
		      enum xe_force_wake_domains domains)
{
	struct xe_device *xe = fw_to_xe(fw);
	struct xe_force_wake_domain *domain;
	enum xe_force_wake_domains tmp, woken = 0;
	int ret, ret2 = 0;

	mutex_lock(&fw->lock);
	for_each_fw_domain_masked(domain, domains, fw, tmp) {
		if (!domain->ref++) {
			woken |= BIT(domain->id);
			domain_wake(xe, domain);
		}
	}
	for_each_fw_domain_masked(domain, woken, fw, tmp) {
		ret = domain_wake_wait(xe, domain);
		ret2 |= ret;
		if (ret)
			drm_notice(&xe->drm, "Force wake domain (%d) failed to ack wake, ret=%d\n",
				   domain->id, ret);
	}
	fw->awake_domains |= woken;
	mutex_unlock(&fw->lock);

	return ret2;
}

int xe_force_wake_put(struct xe_force_wake *fw,
		      enum xe_force_wake_domains domains)
{
	struct xe_device *xe = fw_to_xe(fw);
	struct xe_force_wake_domain *domain;
	enum xe_force_wake_domains tmp, sleep = 0;
	int ret, ret2 = 0;

	mutex_lock(&fw->lock);
	for_each_fw_domain_masked(domain, domains, fw, tmp) {
		if (!--domain->ref) {
			sleep |= BIT(domain->id);
			domain_sleep(xe, domain);
		}
	}
	for_each_fw_domain_masked(domain, sleep, fw, tmp) {
		ret = domain_sleep_wait(xe, domain);
		ret2 |= ret;
		if (ret)
			drm_notice(&xe->drm, "Force wake domain (%d) failed to ack sleep, ret=%d\n",
				   domain->id, ret);
	}
	fw->awake_domains &= ~sleep;
	mutex_unlock(&fw->lock);

	return ret2;
}
