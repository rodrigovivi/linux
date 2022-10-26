// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_gt.h"
#include "xe_gt_mcr.h"
#include "xe_gt_topology.h"
#include "xe_gt_types.h"
#include "xe_mmio.h"

#include "../i915/gt/intel_gt_regs.h"

/**
 * DOC: GT Multicast/Replicated (MCR) Register Support
 *
 * Some GT registers are designed as "multicast" or "replicated" registers:
 * multiple instances of the same register share a single MMIO offset.  MCR
 * registers are generally used when the hardware needs to potentially track
 * independent values of a register per hardware unit (e.g., per-subslice,
 * per-L3bank, etc.).  The specific types of replication that exist vary
 * per-platform.
 *
 * MMIO accesses to MCR registers are controlled according to the settings
 * programmed in the platform's MCR_SELECTOR register(s).  MMIO writes to MCR
 * registers can be done in either a (i.e., a single write updates all
 * instances of the register to the same value) or unicast (a write updates only
 * one specific instance).  Reads of MCR registers always operate in a unicast
 * manner regardless of how the multicast/unicast bit is set in MCR_SELECTOR.
 * Selection of a specific MCR instance for unicast operations is referred to
 * as "steering."
 *
 * If MCR register operations are steered toward a hardware unit that is
 * fused off or currently powered down due to power gating, the MMIO operation
 * is "terminated" by the hardware.  Terminated read operations will return a
 * value of zero and terminated unicast write operations will be silently
 * ignored.
 */

enum {
	MCR_OP_READ,
	MCR_OP_WRITE
};

static const struct {
	const char *name;
	void (*init)(struct xe_gt *);
} xe_steering_types[] = {
	/* TODO */
};

void xe_gt_mcr_init(struct xe_gt *gt)
{
	spin_lock_init(&gt->mcr_lock);

	/* TODO: Setup MCR register ranges for each platform */

	/* Select non-terminated steering target for each type */
	for (int i = 0; i < NUM_STEERING_TYPES; i++)
		if (gt->steering[i].ranges && xe_steering_types[i].init)
			xe_steering_types[i].init(gt);
}

/*
 * xe_gt_mcr_get_nonterminated_steering - find group/instance values that
 *    will steer a register to a non-terminated instance
 * @gt: GT structure
 * @reg: register for which the steering is required
 * @group: return variable for group steering
 * @instance: return variable for instance steering
 *
 * This function returns a group/instance pair that is guaranteed to work for
 * read steering of the given register. Note that a value will be returned even
 * if the register is not replicated and therefore does not actually require
 * steering.
 */
static void xe_gt_mcr_get_nonterminated_steering(struct xe_gt *gt,
						 i915_mcr_reg_t reg,
						 u8 *group, u8 *instance)
{
	for (int type = 0; type < NUM_STEERING_TYPES; type++) {
		if (!gt->steering[type].ranges)
			continue;

		for (int i = 0; gt->steering[type].ranges[i].end > 0; i++) {
			if (xe_mmio_in_range(&gt->steering[type].ranges[i], reg.reg)) {
				*group = gt->steering[type].group_target;
				*instance = gt->steering[type].instance_target;
			}
		}
	}

	/*
	 * All MCR registers should be part of one of the steering ranges
	 * we're tracking.
	 */
	drm_WARN(&gt_to_xe(gt)->drm, true,
		 "Did not find MCR register %#x in any MCR steering table\n",
		 reg.reg);
	*group = 0;
	*instance = 0;
}

#define STEER_SEMAPHORE		0xFD0

/*
 * Obtain exclusive access to MCR steering.  On MTL and beyond we also need
 * to synchronize with external clients (e.g., firmware), so a semaphore
 * register will also need to be taken.
 */
static void mcr_lock(struct xe_gt *gt)
{
	struct xe_device *xe = gt_to_xe(gt);
	int ret;

	spin_lock(&gt->mcr_lock);

	/*
	 * Starting with MTL we also need to grab a semaphore register
	 * to synchronize with external agents (e.g., firmware) that now
	 * shares the same steering control register.
	 */
	if (GRAPHICS_VERx100(xe) >= 1270)
		ret = wait_for_us(xe_mmio_read32(gt, STEER_SEMAPHORE) == 0x1, 10);

	drm_WARN_ON_ONCE(&xe->drm, ret == -ETIMEDOUT);
}

static void mcr_unlock(struct xe_gt *gt) {
	/* Release hardware semaphore */
	if (GRAPHICS_VERx100(gt_to_xe(gt)) >= 1270)
		xe_mmio_write32(gt, STEER_SEMAPHORE, 0x1);

	spin_unlock(&gt->mcr_lock);
}

/*
 * Access a register with specific MCR steering
 *
 * Caller needs to make sure the relevant forcewake wells are up.
 */
static u32 rw_with_mcr_steering(struct xe_gt *gt, i915_mcr_reg_t reg, u8 rw_flag,
				int group, int instance, u32 value)
{
	u32 steer_reg, steer_val, val = 0;

	lockdep_assert_held(&gt->mcr_lock);

	if (GRAPHICS_VERx100(gt_to_xe(gt)) >= 1270) {
		steer_reg = MTL_MCR_SELECTOR.reg;
		steer_val = REG_FIELD_PREP(MTL_MCR_GROUPID, group) |
			REG_FIELD_PREP(MTL_MCR_INSTANCEID, instance);
	} else {
		steer_reg = GEN8_MCR_SELECTOR.reg;
		steer_val = REG_FIELD_PREP(GEN11_MCR_SLICE_MASK, group) |
			REG_FIELD_PREP(GEN11_MCR_SUBSLICE_MASK, instance);
	}

	/*
	 * Always leave the hardware in multicast mode when doing reads
	 * (see comment about Wa_22013088509 below) and only change it
	 * to unicast mode when doing writes of a specific instance.
	 *
	 * No need to save old steering reg value.
	 */
	if (rw_flag == MCR_OP_READ)
		steer_val |= GEN11_MCR_MULTICAST;

	xe_mmio_write32(gt, steer_reg, steer_val);

	if (rw_flag == MCR_OP_READ)
		val = xe_mmio_read32(gt, reg.reg);
	else
		xe_mmio_write32(gt, reg.reg, value);

	/*
	 * If we turned off the multicast bit (during a write) we're required
	 * to turn it back on before finishing.  The group and instance values
	 * don't matter since they'll be re-programmed on the next MCR
	 * operation.
	 */
	if (rw_flag == MCR_OP_WRITE)
		xe_mmio_write32(gt, steer_reg, GEN11_MCR_MULTICAST);

	return val;
}

/**
 * xe_gt_mcr_unicast_read_any - reads a non-terminated instance of an MCR register
 * @gt: GT structure
 * @reg: register to read
 *
 * Reads a GT MCR register.  The read will be steered to a non-terminated
 * instance (i.e., one that isn't fused off or powered down by power gating).
 * This function assumes the caller is already holding any necessary forcewake
 * domains.
 *
 * Returns the value from a non-terminated instance of @reg.
 */
u32 xe_gt_mcr_unicast_read_any(struct xe_gt *gt, i915_mcr_reg_t reg)
{
	u8 group, instance;
	u32 val;

	xe_gt_mcr_get_nonterminated_steering(gt, reg, &group, &instance);

	mcr_lock(gt);
	val = rw_with_mcr_steering(gt, reg, MCR_OP_READ, group, instance, 0);
	mcr_unlock(gt);

	return val;
}

/**
 * xe_gt_mcr_unicast_read - read a specific instance of an MCR register
 * @gt: GT structure
 * @reg: the MCR register to read
 * @group: the MCR group
 * @instance: the MCR instance
 *
 * Returns the value read from an MCR register after steering toward a specific
 * group/instance.
 */
u32 xe_gt_mcr_unicast_read(struct xe_gt *gt,
			   i915_mcr_reg_t reg,
			   int group, int instance)
{
	u32 val;

	mcr_lock(gt);
	val = rw_with_mcr_steering(gt, reg, MCR_OP_READ, group, instance, 0);
	mcr_unlock(gt);

	return val;
}

/**
 * xe_gt_mcr_unicast_write - write a specific instance of an MCR register
 * @gt: GT structure
 * @reg: the MCR register to write
 * @value: value to write
 * @group: the MCR group
 * @instance: the MCR instance
 *
 * Write an MCR register in unicast mode after steering toward a specific
 * group/instance.
 */
void xe_gt_mcr_unicast_write(struct xe_gt *gt, i915_mcr_reg_t reg, u32 value,
			     int group, int instance)
{
	mcr_lock(gt);
	rw_with_mcr_steering(gt, reg, MCR_OP_WRITE, group, instance, value);
	mcr_unlock(gt);
}

/**
 * xe_gt_mcr_multicast_write - write a value to all instances of an MCR register
 * @gt: GT structure
 * @reg: the MCR register to write
 * @value: value to write
 *
 * Write an MCR register in multicast mode to update all instances.
 */
void xe_gt_mcr_multicast_write(struct xe_gt *gt, i915_mcr_reg_t reg, u32 value)
{
	/*
	 * Synchronize with any unicast operations.  Once we have exclusive
	 * access, the MULTICAST bit should already be set, so there's no need
	 * to touch the steering register.
	 */
	mcr_lock(gt);
	xe_mmio_write32(gt, reg.reg, value);
	mcr_unlock(gt);
}

