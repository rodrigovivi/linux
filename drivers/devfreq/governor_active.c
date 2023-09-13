// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright © 2023 Intel Corporation
 */

#include <linux/slab.h>
#include <linux/device.h>
#include <linux/devfreq.h>
#include <linux/pm.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include "governor.h"

static int devfreq_active_get_target_freq(struct devfreq *devfreq,
					  unsigned long *freq)
{
	struct devfreq_dev_status *stat;
	struct devfreq_active_data *data = devfreq->data;
//	struct tegra_devfreq *tegra;
//	struct tegra_devfreq_device *dev;
//	unsigned long target_freq = 0;
	//unsigned int i;
	int err;

	printk(KERN_ERR "KERNEL-DEBUG: %s %d\n", __FUNCTION__, __LINE__);


	err = devfreq_update_stats(devfreq);
	if (err)
		return err;

	stat = &devfreq->last_status;

printk(KERN_ERR "KERNEL-DEBUG: %s %d\n", __FUNCTION__, __LINE__);

	if (data->get_target_freq)
		data->get_target_freq(devfreq, freq);
printk(KERN_ERR "KERNEL-DEBUG: %s %d\n", __FUNCTION__, __LINE__);

//	tegra = stat->private_data;

//	for (i = 0; i < ARRAY_SIZE(tegra->devices); i++) {
//		dev = &tegra->devices[i];

//		actmon_update_target(tegra, dev);

//		target_freq = max(target_freq, dev->target_freq);
//	}

//	*freq = target_freq;

	return 0;
}

static int devfreq_active_event_handler(struct devfreq *devfreq,
					unsigned int event, void *data)
{
//	struct tegra_devfreq *tegra = dev_get_drvdata(devfreq->dev.parent);
	unsigned int *new_delay = data;
	struct devfreq_active_data *gov_data = devfreq->data;
	int ret = 0;

	devfreq->governor->flags |= gov_data->governor_extra_flags;
	printk(KERN_ERR "KERNEL-DEBUG: %s %d\n", __FUNCTION__, __LINE__);


	/*
	 * Couple devfreq-device with the governor early because it is
	 * needed at the moment of governor's start (used by ISR).
	 */
//	tegra->devfreq = devfreq;

	switch (event) {
	case DEVFREQ_GOV_START:
		printk(KERN_ERR "KERNEL-DEBUG: %s %d\n", __FUNCTION__, __LINE__);

		devfreq_monitor_start(devfreq);
//		ret = tegra_actmon_start(tegra);
		break;

	case DEVFREQ_GOV_STOP:
printk(KERN_ERR "KERNEL-DEBUG: %s %d\n", __FUNCTION__, __LINE__);

//		tegra_actmon_stop(tegra);
		devfreq_monitor_stop(devfreq);
		break;

	case DEVFREQ_GOV_UPDATE_INTERVAL:
printk(KERN_ERR "KERNEL-DEBUG: %s %d\n", __FUNCTION__, __LINE__);

		/*
		 * ACTMON hardware supports up to 256 milliseconds for the
		 * sampling period.
		 */
		if (*new_delay > 256) {
			ret = -EINVAL;
			break;
		}

//		tegra_actmon_pause(tegra);
		devfreq_update_interval(devfreq, new_delay);
//		ret = tegra_actmon_resume(tegra);
		break;

	case DEVFREQ_GOV_SUSPEND:
printk(KERN_ERR "KERNEL-DEBUG: %s %d\n", __FUNCTION__, __LINE__);

//		tegra_actmon_stop(tegra);
		devfreq_monitor_suspend(devfreq);
		break;

	case DEVFREQ_GOV_RESUME:
printk(KERN_ERR "KERNEL-DEBUG: %s %d\n", __FUNCTION__, __LINE__);

		devfreq_monitor_resume(devfreq);
//		ret = tegra_actmon_start(tegra);
		break;
	}
printk(KERN_ERR "KERNEL-DEBUG: %s %d %d\n", __FUNCTION__, __LINE__, event);

	return ret;
}

static struct devfreq_governor devfreq_active = {
	.name = DEVFREQ_GOV_ACTIVE,
	.attrs = DEVFREQ_GOV_ATTR_POLLING_INTERVAL,
	.flags = DEVFREQ_GOV_FLAG_IMMUTABLE,
//		| DEVFREQ_GOV_FLAG_IRQ_DRIVEN,
	.get_target_freq = devfreq_active_get_target_freq,
	.event_handler = devfreq_active_event_handler,
};

static int __init devfreq_active_init(void)
{
	return devfreq_add_governor(&devfreq_active);
}
subsys_initcall(devfreq_active_init);

static void __exit devfreq_active_exit(void)
{
	int ret;

	ret = devfreq_remove_governor(&devfreq_active);
	if (ret)
		pr_err("%s: failed remove governor %d\n", __func__, ret);

	return;
}
module_exit(devfreq_active_exit);

MODULE_DESCRIPTION("DEVFREQ Active governor");
MODULE_LICENSE("GPL v2");
