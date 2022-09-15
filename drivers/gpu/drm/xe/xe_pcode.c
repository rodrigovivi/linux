// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_pcode_api.h"
#include "xe_pcode.h"

#include "xe_gt.h"
#include "xe_mmio.h"

#include <linux/errno.h>

/**
 * DOC: Xe PCODE
 *
 * Xe PCODE is the component responsible for interfacing with the PCODE
 * firmware.
 * It shall provide a very simple ABI to other Xe components, but be the
 * single and consolidated place that will communicate with PCODE. All read
 * and write operations to PCODE will be internal and private to this component.
 *
 * What's next:
 * - PCODE dgfx init wait and timeout
 * - PCODE hw metrics
 * - PCODE for display operations
 */

static int pcode_mailbox_status(struct xe_gt *gt)
{
	u32 err;
	static const struct pcode_err_decode err_decode[] = {
		[PCODE_ILLEGAL_CMD] = {-ENXIO, "Illegal Command"},
		[PCODE_TIMEOUT] = {-ETIMEDOUT, "Timed out"},
		[PCODE_ILLEGAL_DATA] = {-EINVAL, "Illegal Data"},
		[PCODE_ILLEGAL_SUBCOMMAND] = {-ENXIO, "Illegal Subcommand"},
		[PCODE_LOCKED] = {-EBUSY, "PCODE Locked"},
		[PCODE_GT_RATIO_OUT_OF_RANGE] = {-EOVERFLOW,
			"GT ratio out of range"},
		[PCODE_REJECTED] = {-EACCES, "PCODE Rejected"},
		[PCODE_ERROR_MASK] = {-EPROTO, "Unknown"},
	};

	err = xe_mmio_read32(gt, PCODE_MAILBOX.reg) & PCODE_ERROR_MASK;
	if (err) {
		drm_err(&gt_to_xe(gt)->drm, "PCODE Mailbox failed: %d %s", err,
			err_decode[err].str ?: "Unknown");
		return err_decode[err].errno ?: -EPROTO;
	}

	return 0;
}

static bool pcode_mailbox_done(struct xe_gt *gt)
{
	return (xe_mmio_read32(gt, PCODE_MAILBOX.reg) & PCODE_READY) == 0;
}

static int pcode_mailbox_rw(struct xe_gt *gt, u32 mbox, u32 *data0, u32 *data1,
			    unsigned int timeout, bool return_data)
{
	if (!pcode_mailbox_done(gt))
		return -EAGAIN;

	xe_mmio_write32(gt, PCODE_DATA0.reg, *data0);
	xe_mmio_write32(gt, PCODE_DATA1.reg, data1 ? *data1 : 0);
	xe_mmio_write32(gt, PCODE_MAILBOX.reg, PCODE_READY | mbox);

	wait_for(pcode_mailbox_done(gt), timeout);

	if (return_data) {
		*data0 = xe_mmio_read32(gt, PCODE_DATA0.reg);
		if (data1)
			*data1 = xe_mmio_read32(gt, PCODE_DATA1.reg);
	}

	return pcode_mailbox_status(gt);
}

static int pcode_mailbox_write(struct xe_gt *gt, u32 mbox, u32 data)
{
	return pcode_mailbox_rw(gt, mbox, &data, NULL, 500, false);
}

/**
 * xe_pcode_init_min_freq_table - Initialize PCODE's QOS frequency table
 * @gt: gt instance
 * @min_gt_freq: Minimal (RPn) GT frequency in units of 50MHz.
 * @max_gt_freq: Maximal (RP0) GT frequency in units of 50MHz.
 *
 * This function initialize PCODE's QOS frequency table for a proper minimal
 * frequency/power steering decision, depending on the current requested GT
 * frequency. For older platforms this was a more complete table including
 * the IA freq. However for the latest platforms this table become a simple
 * 1-1 Ring vs GT frequency. Even though, without setting it, PCODE might
 * not take the right decisions for some memory frequencies and affect latency.
 *
 * It returns 0 on success, and -ERROR number on failure, -EINVAL if max
 * frequency is higher then the minimal, and other errors directly translated
 * from the PCODE Error returs:
 * - -ENXIO: "Illegal Command"
 * - -ETIMEDOUT: "Timed out"
 * - -EINVAL: "Illegal Data"
 * - -ENXIO, "Illegal Subcommand"
 * - -EBUSY: "PCODE Locked"
 * - -EOVERFLOW, "GT ratio out of range"
 * - -EACCES, "PCODE Rejected"
 * - -EPROTO, "Unknown"
 */
int xe_pcode_init_min_freq_table(struct xe_gt *gt, u32 min_gt_freq,
				 u32 max_gt_freq)
{
	int ret;
	u32 freq;

	if (IS_DGFX(gt_to_xe(gt)))
		return 0;

	if (max_gt_freq <= min_gt_freq)
		return -EINVAL;

	for (freq = min_gt_freq; freq <= max_gt_freq; freq++) {
		ret = pcode_mailbox_write(gt, PCODE_WRITE_MIN_FREQ_TABLE,
					  freq << PCODE_FREQ_RING_RATIO_SHIFT |
					  freq);
		if (ret)
			return ret;
	}
	return 0;
}
