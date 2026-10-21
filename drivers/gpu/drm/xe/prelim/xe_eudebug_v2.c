// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <linux/device.h>
#include <linux/kstrtox.h>
#include <linux/sysfs.h>

#include <drm/drm_drv.h>

#include "prelim/xe_eudebug_v2.h"
#include "prelim/xe_eudebug_v2_abi.h"
#include "xe_device.h"
#include "../xe_eudebug.h"	/* the eudebug core, not prelim/xe_eudebug.h */

/**
 * DOC: PRELIM v2 eudebug
 *
 * The eudebug core interface, exposed under PRELIM v2 names and numbers until
 * it lands upstream. The uAPI is ABI identical to the one of the eudebug core,
 * which serves it unchanged, see xe_eudebug_v2_abi.h. Only the values living
 * in shared namespaces (drm ioctl, exec queue property, vm bind op and
 * extension) use PRELIM numbers.
 */

/**
 * prelim_v2_xe_eudebug_connect_ioctl - PRELIM v2 eudebug connect
 * @dev: DRM device
 * @data: ioctl data, struct prelim_v2_drm_xe_eudebug_connect
 * @file: DRM file
 *
 * Return: eudebug file descriptor on success, negative error code on failure.
 */
int prelim_v2_xe_eudebug_connect_ioctl(struct drm_device *dev, void *data,
				       struct drm_file *file)
{
	return xe_eudebug_connect_ioctl(dev, data, file);
}

static ssize_t prelim_v2_enable_eudebug_show(struct device *dev,
					     struct device_attribute *attr, char *buf)
{
	struct xe_device *xe = pdev_to_xe_device(to_pci_dev(dev));

	return sysfs_emit(buf, "%u\n", xe_eudebug_is_enabled(xe));
}

static ssize_t prelim_v2_enable_eudebug_store(struct device *dev,
					      struct device_attribute *attr,
					      const char *buf, size_t count)
{
	struct xe_device *xe = pdev_to_xe_device(to_pci_dev(dev));
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;

	ret = xe_eudebug_enable(xe, enable);
	if (ret)
		return ret;

	return count;
}

static DEVICE_ATTR_RW(prelim_v2_enable_eudebug);

static void prelim_v2_xe_eudebug_sysfs_fini(void *arg)
{
	struct xe_device *xe = arg;

	sysfs_remove_file(&xe->drm.dev->kobj, &dev_attr_prelim_v2_enable_eudebug.attr);
}

/**
 * prelim_v2_xe_eudebug_init - Set up PRELIM v2 eudebug
 * @xe: xe device
 *
 * Expose the prelim_v2_enable_eudebug sysfs attribute, if the eudebug core is
 * available on @xe.
 */
void prelim_v2_xe_eudebug_init(struct xe_device *xe)
{
	int err;

	if (READ_ONCE(xe->eudebug.cap_state) == XE_EUDEBUG_CAP_NOT_SUPPORTED)
		return;

	err = devm_add_action_or_reset(xe->drm.dev, prelim_v2_xe_eudebug_sysfs_fini, xe);
	if (err)
		goto out_err;

	err = sysfs_create_file(&xe->drm.dev->kobj, &dev_attr_prelim_v2_enable_eudebug.attr);
	if (err)
		goto out_err;

	return;

out_err:
	drm_warn(&xe->drm, "PRELIM v2 eudebug disabled, init fail: %d\n", err);
}
