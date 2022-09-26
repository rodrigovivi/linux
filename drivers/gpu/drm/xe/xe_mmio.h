/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef _XE_MMIO_H_
#define _XE_MMIO_H_

#include "xe_gt_types.h"

struct drm_device;
struct drm_file;
struct xe_device;

int xe_mmio_init(struct xe_device *xe);
void xe_mmio_write32(struct xe_gt *gt, u32 reg, u32 val);
u32 xe_mmio_read32(struct xe_gt *gt, u32 reg);
void xe_mmio_rmw32(struct xe_gt *gt, u32 reg, u32 mask, u32 val);
void xe_mmio_write64(struct xe_gt *gt, u32 reg, u64 val);
u64 xe_mmio_read64(struct xe_gt *gt, u32 reg);
int xe_mmio_write32_and_verify(struct xe_gt *gt,
			       u32 reg, u32 val,
			       u32 mask, u32 eval);
int xe_mmio_wait32(struct xe_gt *gt,
		   u32 reg, u32 val,
		   u32 mask, u32 timeout_ms);
int xe_mmio_ioctl(struct drm_device *dev, void *data,
		  struct drm_file *file);

#endif
