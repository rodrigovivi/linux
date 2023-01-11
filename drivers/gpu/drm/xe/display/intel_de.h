/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_DE_H__
#define __INTEL_DE_H__

#include "i915_drv.h"
#include "xe_mmio.h"
#include "xe_pcode.h"

/* This was included from i915_trace.h -> i915_irq.h -> i915_reg.h, kept for compat */
#include "i915_reg.h"

static inline u32
intel_de_read(struct drm_i915_private *i915, i915_reg_t reg)
{
	return xe_mmio_read32(to_gt(i915), reg.reg);
}

static inline void
intel_de_posting_read(struct drm_i915_private *i915, i915_reg_t reg)
{
	xe_mmio_read32(to_gt(i915), reg.reg);
}

static inline void
intel_de_write(struct drm_i915_private *i915, i915_reg_t reg, u32 val)
{
	xe_mmio_write32(to_gt(i915), reg.reg, val);
}

static inline void
intel_de_rmw(struct drm_i915_private *i915, i915_reg_t reg, u32 clear, u32 set)
{
	xe_mmio_rmw32(to_gt(i915), reg.reg, ~clear, set);
}

static inline int
intel_de_wait_for_register(struct drm_i915_private *i915, i915_reg_t reg,
			   u32 mask, u32 value, unsigned int timeout)
{
	return xe_mmio_wait32(to_gt(i915), reg.reg, value, mask, timeout * USEC_PER_MSEC, NULL,
			      false);
}

static inline int
intel_de_wait_for_register_fw(struct drm_i915_private *i915, i915_reg_t reg,
			      u32 mask, u32 value, unsigned int timeout)
{
	return xe_mmio_wait32(to_gt(i915), reg.reg, value, mask, timeout * USEC_PER_MSEC, NULL,
			      false);
}

static inline int
__intel_de_wait_for_register(struct drm_i915_private *i915, i915_reg_t reg,
			     u32 mask, u32 value,
			     unsigned int fast_timeout_us,
			     unsigned int slow_timeout_ms, u32 *out_value)
{
	return wait_for_atomic(((*out_value = xe_mmio_read32(to_gt(i915), reg.reg)) & mask) == value,
			slow_timeout_ms);
}

static inline int
intel_de_wait_for_set(struct drm_i915_private *i915, i915_reg_t reg,
		      u32 mask, unsigned int timeout)
{
	return intel_de_wait_for_register(i915, reg, mask, mask, timeout);
}

static inline int
intel_de_wait_for_clear(struct drm_i915_private *i915, i915_reg_t reg,
			u32 mask, unsigned int timeout)
{
	return intel_de_wait_for_register(i915, reg, mask, 0, timeout);
}

/*
 * Unlocked mmio-accessors, think carefully before using these.
 *
 * Certain architectures will die if the same cacheline is concurrently accessed
 * by different clients (e.g. on Ivybridge). Access to registers should
 * therefore generally be serialised, by either the dev_priv->uncore.lock or
 * a more localised lock guarding all access to that bank of registers.
 */
static inline u32
intel_de_read_fw(struct drm_i915_private *i915, i915_reg_t reg)
{
	return xe_mmio_read32(to_gt(i915), reg.reg);
}

static inline void
intel_de_write_fw(struct drm_i915_private *i915, i915_reg_t reg, u32 val)
{
	xe_mmio_write32(to_gt(i915), reg.reg, val);
}

static inline void
intel_de_write_samevalue(struct drm_i915_private *i915, i915_reg_t reg)
{
	/*
	 * Not implemented, requires lock on all reads/writes.
	 * only required for really old FBC. Not ever going to be needed.
	 */
	XE_BUG_ON(1);
}

static inline u32
intel_de_read_notrace(struct drm_i915_private *i915, i915_reg_t reg)
{
	return xe_mmio_read32(to_gt(i915), reg.reg);
}

static inline void
intel_de_write_notrace(struct drm_i915_private *i915, i915_reg_t reg, u32 val)
{
	xe_mmio_write32(to_gt(i915), reg.reg, val);
}

static inline int
intel_de_pcode_write_timeout(struct drm_i915_private *i915, u32 mbox, u32 val,
			    int fast_timeout_us, int slow_timeout_ms)
{
	return xe_pcode_write_timeout(to_gt(i915), mbox, val,
				       slow_timeout_ms ?: 1);
}

static inline int
intel_de_pcode_write(struct drm_i915_private *i915, u32 mbox, u32 val)
{

	return xe_pcode_write(to_gt(i915), mbox, val);
}

static inline int
intel_de_pcode_read(struct drm_i915_private *i915, u32 mbox, u32 *val, u32 *val1)
{
	return xe_pcode_read(to_gt(i915), mbox, val, val1);
}

static inline int intel_de_pcode_request(struct drm_i915_private *i915, u32 mbox,
					 u32 request, u32 reply_mask, u32 reply,
					 int timeout_base_ms)
{
	return xe_pcode_request(to_gt(i915), mbox, request, reply_mask, reply,
				timeout_base_ms);
}

#endif /* __INTEL_DE_H__ */
