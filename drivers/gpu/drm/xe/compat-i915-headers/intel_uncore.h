/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef __INTEL_UNCORE_H__
#define __INTEL_UNCORE_H__

#include "xe_device.h"
#include "xe_device_types.h"
#include "xe_mmio.h"

static inline struct xe_gt *__fake_uncore_to_gt(struct fake_uncore *uncore)
{
	struct xe_device *xe = container_of(uncore, struct xe_device, uncore);

	return to_gt(xe);
}

static inline u32 intel_uncore_read(struct fake_uncore *uncore, i915_reg_t reg)
{
	return xe_mmio_read32(__fake_uncore_to_gt(uncore), reg.reg);
}

static inline u32 intel_uncore_read8(struct fake_uncore *uncore, i915_reg_t reg)
{
	return xe_mmio_read8(__fake_uncore_to_gt(uncore), reg.reg);
}

static inline u64 intel_uncore_read64_2x32(struct fake_uncore *uncore, i915_reg_t lower_reg, i915_reg_t upper_reg)
{
	u32 upper, lower, old_upper;
	int loop = 0;

	upper = xe_mmio_read32(__fake_uncore_to_gt(uncore), upper_reg.reg);
	do {
		old_upper = upper;
		lower = xe_mmio_read32(__fake_uncore_to_gt(uncore), lower_reg.reg);
		upper = xe_mmio_read32(__fake_uncore_to_gt(uncore), upper_reg.reg);
	} while (upper != old_upper && loop++ < 2);

	return (u64)upper << 32 | lower;
}

static inline void intel_uncore_posting_read(struct fake_uncore *uncore, i915_reg_t reg)
{
	xe_mmio_read32(__fake_uncore_to_gt(uncore), reg.reg);
}

static inline void intel_uncore_write(struct fake_uncore *uncore, i915_reg_t reg, u32 val)
{
	xe_mmio_write32(__fake_uncore_to_gt(uncore), reg.reg, val);
}

static inline u32 intel_uncore_rmw(struct fake_uncore *uncore, i915_reg_t reg, u32 clear, u32 set)
{
	return xe_mmio_rmw32(__fake_uncore_to_gt(uncore), reg.reg, clear, set);
}

static inline int intel_wait_for_register(struct fake_uncore *uncore, i915_reg_t reg, u32 mask, u32 value, unsigned int timeout)
{
	return xe_mmio_wait32(__fake_uncore_to_gt(uncore), reg.reg, value, mask, timeout * USEC_PER_MSEC, NULL, false);
}

static inline int intel_wait_for_register_fw(struct fake_uncore *uncore, i915_reg_t reg, u32 mask, u32 value, unsigned int timeout)
{
	return xe_mmio_wait32(__fake_uncore_to_gt(uncore), reg.reg, value, mask, timeout * USEC_PER_MSEC, NULL, false);
}

static inline int __intel_wait_for_register(struct fake_uncore *uncore, i915_reg_t reg, u32 mask, u32 value,
					    unsigned int fast_timeout_us, unsigned int slow_timeout_ms, u32 *out_value)
{
	return xe_mmio_wait32(__fake_uncore_to_gt(uncore), reg.reg, value, mask,
			      fast_timeout_us + 1000 * slow_timeout_ms,
			      out_value, false);
}

static inline u32 intel_uncore_read_fw(struct fake_uncore *uncore, i915_reg_t reg)
{
	return xe_mmio_read32(__fake_uncore_to_gt(uncore), reg.reg);
}

static inline void intel_uncore_write_fw(struct fake_uncore *uncore, i915_reg_t reg, u32 val)
{
	xe_mmio_write32(__fake_uncore_to_gt(uncore), reg.reg, val);
}

static inline u32 intel_uncore_read_notrace(struct fake_uncore *uncore, i915_reg_t reg)
{
	return xe_mmio_read32(__fake_uncore_to_gt(uncore), reg.reg);
}

static inline void intel_uncore_write_notrace(struct fake_uncore *uncore, i915_reg_t reg, u32 val)
{
	xe_mmio_write32(__fake_uncore_to_gt(uncore), reg.reg, val);
}

#endif /* __INTEL_UNCORE_H__ */
