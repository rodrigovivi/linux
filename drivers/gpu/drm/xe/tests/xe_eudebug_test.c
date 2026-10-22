// SPDX-License-Identifier: GPL-2.0 AND MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <kunit/visibility.h>
#include <linux/string_choices.h>

#include "regs/xe_gt_regs.h"
#include "xe_force_wake.h"
#include "xe_gt_mcr.h"
#include "xe_mmio.h"
#include "tests/xe_kunit_helpers.h"
#include "tests/xe_pci_test.h"
#include "tests/xe_test.h"

static const char *reg_to_str(struct xe_reg reg)
{
	if (reg.raw == TD_CTL.__reg.raw)
		return "TD_CTL";
	else if (reg.raw == ROW_CHICKEN.__reg.raw)
		return "ROW_CHICKEN";
	else if (reg.raw == ROW_CHICKEN2.__reg.raw)
		return "ROW_CHICKEN2";
	else
		return "UNKNOWN REG";
}

static u32 get_reg_mask(struct xe_device *xe, struct xe_reg reg, struct kunit *test)
{
	u32 val = 0;

	if (reg.raw == TD_CTL.__reg.raw) {
		val = TD_CTL_BREAKPOINT_ENABLE |
		      TD_CTL_FORCE_THREAD_BREAKPOINT_ENABLE |
		      TD_CTL_FEH_AND_FEE_ENABLE;
		if (GRAPHICS_VERx100(xe) >= 1250)
			val |= TD_CTL_GLOBAL_DEBUG_ENABLE;
	} else if (reg.raw == ROW_CHICKEN.__reg.raw) {
		val = STALL_DOP_GATING_DISABLE;
	} else if (reg.raw == ROW_CHICKEN2.__reg.raw) {
		val = XEHPC_DISABLE_BTB;
	} else {
		KUNIT_FAIL(test, "Invalid register selection: 0x%x\n", reg.raw);
	}

	return val;
}

static u32 get_reg_expected(struct xe_device *xe, struct xe_reg reg,
			    bool enable_eudebug, struct kunit *test)
{
	return enable_eudebug ? get_reg_mask(xe, reg, test) : 0;
}

static void check_reg(struct xe_gt *gt, bool enable_eudebug,
		      struct xe_reg reg, struct kunit *test)
{
	struct xe_device *xe = gt_to_xe(gt);
	const u32 reg_bits_expected = get_reg_expected(xe, reg, enable_eudebug, test);
	const u32 reg_mask = get_reg_mask(xe, reg, test);
	u32 reg_bits = 0;

	if (reg.mcr)
		reg_bits = xe_gt_mcr_unicast_read_any(gt, (struct xe_reg_mcr){.__reg = reg});
	else
		reg_bits = xe_mmio_read32(&gt->mmio, reg);

	reg_bits &= reg_mask;

	KUNIT_EXPECT_EQ_MSG(test, reg_bits_expected, reg_bits,
			    "Invalid bits set for %s\n", reg_to_str(reg));
}

static void __check_regs(struct xe_gt *gt, bool enable_eudebug, struct kunit *test)
{
	struct xe_device *xe = gt_to_xe(gt);

	if (GRAPHICS_VERx100(xe) >= 1200)
		check_reg(gt, enable_eudebug, TD_CTL.__reg, test);

	if (GRAPHICS_VERx100(xe) >= 1250 && GRAPHICS_VERx100(xe) <= 1274)
		check_reg(gt, enable_eudebug, ROW_CHICKEN.__reg, test);

	if (xe->info.platform == XE_PVC)
		check_reg(gt, enable_eudebug, ROW_CHICKEN2.__reg, test);
}

static void check_regs(struct xe_device *xe, bool enable_eudebug, struct kunit *test)
{
	struct xe_gt *gt;
	unsigned int fw_ref;
	u8 id;

	kunit_printk(KERN_DEBUG, test, "Check regs for eudebug %s\n",
		     str_enabled_disabled(enable_eudebug));

	for_each_gt(gt, xe, id) {
		if (xe_gt_is_media_type(gt))
			continue;

		/* XXX: Figure out per platform proper domain */
		fw_ref = xe_force_wake_get(gt_to_fw(gt), XE_FORCEWAKE_ALL);
		KUNIT_ASSERT_TRUE_MSG(test, fw_ref, "Forcewake failed.\n");

		__check_regs(gt, enable_eudebug, test);

		xe_force_wake_put(gt_to_fw(gt), fw_ref);
	}
}

struct eudebug_state_restore {
	struct xe_device *xe;
	struct kunit *test;
	bool enabled;
};

static void restore_eudebug_state(void *arg)
{
	struct eudebug_state_restore *r = arg;

	KUNIT_EXPECT_EQ_MSG(r->test, 0, xe_eudebug_enable(r->xe, r->enabled),
			    "Failed to restore eudebug to %s\n",
			    str_enabled_disabled(r->enabled));
}

static void toggle_reg(struct kunit *test)
{
	struct xe_device *xe = test->priv;
	const bool enable_eudebug = xe_eudebug_is_enabled(xe);
	struct eudebug_state_restore *restore;
	int ret;

	/* Covers an SR-IOV VF too, xe_eudebug_init() marks one unsupported */
	if (xe->eudebug.cap_state == XE_EUDEBUG_CAP_NOT_SUPPORTED)
		kunit_skip(test, "eudebug not supported\n");

	if (IS_SRIOV_VF(xe))
		kunit_skip(test, "eudebug not available in SR-IOV VF mode\n");

	kunit_printk(KERN_DEBUG, test, "Test eudebug WAs for graphics version: %u\n",
		     GRAPHICS_VERx100(xe));

	check_regs(xe, enable_eudebug, test);

	/* Nothing to restore until the first toggle below */
	restore = kunit_kzalloc(test, sizeof(*restore), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, restore);
	restore->xe = xe;
	restore->test = test;
	restore->enabled = enable_eudebug;
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, restore_eudebug_state,
						  restore));

	ret = xe_eudebug_enable(xe, !enable_eudebug);
	if (ret == -EBUSY)
		kunit_skip(test, "eudebug toggle busy\n");
	KUNIT_ASSERT_EQ(test, 0, ret);

	check_regs(xe, !enable_eudebug, test);

	ret = xe_eudebug_enable(xe, enable_eudebug);
	KUNIT_ASSERT_EQ(test, 0, ret);
	check_regs(xe, enable_eudebug, test);
}

static bool xe_eudebug_has_regs(struct xe_device *xe)
{
	return GRAPHICS_VERx100(xe) >= 1200;
}

static int xe_eudebug_test_init(struct kunit *test)
{
	struct xe_device *xe;
	int ret;

	ret = xe_kunit_helper_xe_device_live_test_init(test);
	if (ret)
		return ret;

	xe = test->priv;
	kunit_info(test, "running on graphics version: %u\n", GRAPHICS_VERx100(xe));

	if (!xe_eudebug_has_regs(xe))
		kunit_skip(test, "hardware not supported: 0x%x\n", GRAPHICS_VERx100(xe));

	return 0;
}

static struct kunit_case xe_eudebug_tests[] = {
	KUNIT_CASE_PARAM(toggle_reg, xe_pci_live_device_gen_param),
	{}
};

VISIBLE_IF_KUNIT
struct kunit_suite xe_eudebug_test_suite = {
	.name = "xe_eudebug",
	.test_cases = xe_eudebug_tests,
	.init = xe_eudebug_test_init,
};
EXPORT_SYMBOL_IF_KUNIT(xe_eudebug_test_suite);
