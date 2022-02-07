/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#ifndef _XE_DEVICE_H_
#define _XE_DEVICE_H_

#include "xe_device_types.h"

static inline struct xe_device *to_xe_device(const struct drm_device *dev)
{
	return container_of(dev, struct xe_device, drm);
}

static inline struct xe_device *pdev_to_xe_device(struct pci_dev *pdev)
{
	return pci_get_drvdata(pdev);
}

static inline struct xe_device *ttm_to_xe_device(struct ttm_device *ttm)
{
	return container_of(ttm, struct xe_device, ttm);
}

struct xe_device *xe_device_create(struct pci_dev *pdev,
				   const struct pci_device_id *ent);
int xe_device_probe(struct xe_device *xe);
void xe_device_remove(struct xe_device *xe);
void xe_device_shutdown(struct xe_device *xe);

static inline struct xe_file *to_xe_file(const struct drm_file *file)
{
	return file->driver_priv;
}

/*
 * FIXME: Placeholder until multi-gt lands. Once that lands, kill this function.
 */
static inline struct xe_gt *to_gt(struct xe_device *xe)
{
	return &xe->gt;
}

#endif /* _XE_DEVICE_H_ */
