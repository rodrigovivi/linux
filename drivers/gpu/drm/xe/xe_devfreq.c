// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include "xe_devfreq.h"

#include <linux/devfreq.h>
#include <linux/device.h>
#include <linux/pm_opp.h>

#include "xe_gt_printk.h"

#ifdef CONFIG_PM_DEVFREQ

/**
 * DOC: Xe devfreq
 *
 * Devices overview:
 * Xe uses devfreq infrastructure for exposing and controling GT frequencies in
 * a standardized way.
 * devfreq exposes ... under /sys/class/devfreq/
 * which is linked with our card device directly.
 * TODO: Add information on how to use this.
 */

static int xe_devfreq_get_target(struct devfreq *devfreq, unsigned long *freq)
{
	printk(KERN_ERR "KERNEL-DEBUG: %s %d\n", __FUNCTION__, __LINE__);
	*freq = 1000;
	return 0;
}

static struct devfreq_active_data active_data = {
	.governor_extra_flags = DEVFREQ_GOV_FLAG_IRQ_DRIVEN,
	.get_target_freq = xe_devfreq_get_target,
};

static int xe_devfreq_target(struct device *dev, unsigned long *freq, u32 flags)
{
	printk(KERN_ERR "KERNEL-DEBUG: %s %d\n", __FUNCTION__, __LINE__);
	return 0;
}

static int xe_devfreq_get_dev_status(struct device *dev,
				     struct devfreq_dev_status *status)
{
	printk(KERN_ERR "KERNEL-DEBUG: %s %d\n", __FUNCTION__, __LINE__);
	return 0;
}

static struct devfreq_dev_profile xe_devfreq_profile = {
	.timer = DEVFREQ_TIMER_DELAYED,
	.polling_ms = 50,
	.target = xe_devfreq_target,
	.get_dev_status = xe_devfreq_get_dev_status,
};

void xe_devfreq_init(struct xe_gt *gt)
{
	struct xe_device *xe = gt_to_xe(gt);
	struct devfreq *devfreq;

	dev_pm_opp_add(xe->drm.dev, 100, 0);
	dev_pm_opp_add(xe->drm.dev, 500, 0);
	dev_pm_opp_add(xe->drm.dev, 10000, 0);

	xe_devfreq_profile.name = kmalloc(4, GFP_KERNEL);
	sprintf(xe_devfreq_profile.name, "gt%d", gt->info.id);

	devfreq = devm_devfreq_add_device(xe->drm.dev, &xe_devfreq_profile,
					  DEVFREQ_GOV_ACTIVE,
					  &active_data);
	if (IS_ERR(devfreq))
		xe_gt_err(gt, "Failed to init devfreq\n");
}

void xe_devfreq_fini(struct xe_gt *gt)
{
	kfree(xe_devfreq_profile.name);
}
#endif
