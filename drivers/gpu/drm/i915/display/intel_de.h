/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_DE_H__
#define __INTEL_DE_H__

#include "i915_drv.h"
#include "i915_trace.h"
#include "intel_uncore.h"
#include "intel_pcode.h"

static inline u32
intel_de_read(struct drm_i915_private *i915, i915_reg_t reg)
{
	return intel_uncore_read(&i915->uncore, reg);
}

static inline u8
intel_de_read8(struct drm_i915_private *i915, i915_reg_t reg)
{
	return intel_uncore_read8(&i915->uncore, reg);
}

static inline u64
intel_de_read64_2x32(struct drm_i915_private *i915,
		     i915_reg_t lower_reg, i915_reg_t upper_reg)
{
	return intel_uncore_read64_2x32(&i915->uncore, lower_reg, upper_reg);
}

static inline void
intel_de_posting_read(struct drm_i915_private *i915, i915_reg_t reg)
{
	intel_uncore_posting_read(&i915->uncore, reg);
}

static inline void
intel_de_write(struct drm_i915_private *i915, i915_reg_t reg, u32 val)
{
	intel_uncore_write(&i915->uncore, reg, val);
}

static inline u32
intel_de_rmw(struct drm_i915_private *i915, i915_reg_t reg, u32 clear, u32 set)
{
	return intel_uncore_rmw(&i915->uncore, reg, clear, set);
}

static inline int
intel_de_wait_for_register(struct drm_i915_private *i915, i915_reg_t reg,
			   u32 mask, u32 value, unsigned int timeout)
{
	return intel_wait_for_register(&i915->uncore, reg, mask, value, timeout);
}

static inline int
intel_de_wait_for_register_fw(struct drm_i915_private *i915, i915_reg_t reg,
			      u32 mask, u32 value, unsigned int timeout)
{
	return intel_wait_for_register_fw(&i915->uncore, reg, mask, value, timeout);
}

static inline int
__intel_de_wait_for_register(struct drm_i915_private *i915, i915_reg_t reg,
			     u32 mask, u32 value,
			     unsigned int fast_timeout_us,
			     unsigned int slow_timeout_ms, u32 *out_value)
{
	return __intel_wait_for_register(&i915->uncore, reg, mask, value,
					 fast_timeout_us, slow_timeout_ms, out_value);
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
	u32 val;

	val = intel_uncore_read_fw(&i915->uncore, reg);
	trace_i915_reg_rw(false, reg, val, sizeof(val), true);

	return val;
}

static inline void
intel_de_write_fw(struct drm_i915_private *i915, i915_reg_t reg, u32 val)
{
	trace_i915_reg_rw(true, reg, val, sizeof(val), true);
	intel_uncore_write_fw(&i915->uncore, reg, val);
}

static inline u32
intel_de_read_notrace(struct drm_i915_private *i915, i915_reg_t reg)
{
	return intel_uncore_read_notrace(&i915->uncore, reg);
}

static inline void
intel_de_write_notrace(struct drm_i915_private *i915, i915_reg_t reg, u32 val)
{
	intel_uncore_write_notrace(&i915->uncore, reg, val);
}

static inline void
intel_de_write_samevalue(struct drm_i915_private *i915, i915_reg_t reg)
{
	spin_lock_irq(&i915->uncore.lock);
	intel_de_write_fw(i915, reg, intel_de_read_fw(i915, reg));
	spin_unlock_irq(&i915->uncore.lock);
}

static inline int
intel_de_pcode_write_timeout(struct drm_i915_private *i915, u32 mbox, u32 val,
			    int fast_timeout_us, int slow_timeout_ms)
{
	return snb_pcode_write_timeout(&i915->uncore, mbox, val,
				       fast_timeout_us, slow_timeout_ms);
}

static inline int
intel_de_pcode_write(struct drm_i915_private *i915, u32 mbox, u32 val)
{

	return snb_pcode_write(&i915->uncore, mbox, val);
}

static inline int
intel_de_pcode_read(struct drm_i915_private *i915, u32 mbox, u32 *val, u32 *val1)
{
	return snb_pcode_read(&i915->uncore, mbox, val, val1);
}

static inline int intel_de_pcode_request(struct drm_i915_private *i915, u32 mbox,
					 u32 request, u32 reply_mask, u32 reply,
					 int timeout_base_ms)
{
	return skl_pcode_request(&i915->uncore, mbox, request, reply_mask, reply,
				 timeout_base_ms);
}

#endif /* __INTEL_DE_H__ */
