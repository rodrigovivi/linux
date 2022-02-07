/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#ifndef _XE_MMIO_H_
#define _XE_MMIO_H_

#include <linux/delay.h>

#include "xe_gt_types.h"

/*
 * FIXME: This header has been deemed evil and we need to kill it. Temporarily
 * including so we can use 'wait_for' and unblock initial development. A follow
 * should replace 'wait_for' with a sane version and drop including this header.
 */
#include "i915_utils.h"

struct drm_device;
struct drm_file;
struct xe_device;

int xe_mmio_init(struct xe_device *xe);
void xe_mmio_finish(struct xe_device *xe);

static inline void xe_mmio_write32(struct xe_gt *gt,
				   uint32_t reg, uint32_t val)
{
	writel(val, gt->mmio.regs + reg);
}

static inline uint32_t xe_mmio_read32(struct xe_gt *gt, uint32_t reg)
{
	return readl(gt->mmio.regs + reg);
}

static inline uint64_t xe_mmio_read64(struct xe_gt *gt, uint32_t reg)
{
	return readq(gt->mmio.regs + reg);
}

static inline int xe_mmio_write32_and_verify(struct xe_gt *gt,
					     uint32_t reg, uint32_t val,
					     uint32_t mask, uint32_t eval)
{
	uint32_t reg_val;

	xe_mmio_write32(gt, reg, val);
	reg_val = xe_mmio_read32(gt, reg);

	return (reg_val & mask) != eval ? -EINVAL : 0;
}

static inline int xe_mmio_wait32(struct xe_gt *gt,
				 uint32_t reg, uint32_t val,
				 uint32_t mask, uint32_t timeout_ms)
{
	return wait_for((xe_mmio_read32(gt, reg) & mask) == val,
			timeout_ms);
}

int xe_mmio_ioctl(struct drm_device *dev, void *data,
		  struct drm_file *file);

#endif /* _XE_MMIO_H_ */
