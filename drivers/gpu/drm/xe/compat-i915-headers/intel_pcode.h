/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef __INTEL_PCODE_H__
#define __INTEL_PCODE_H__

#include "intel_uncore.h"
#include "xe_pcode.h"

static inline int
snb_pcode_write_timeout(struct fake_uncore *uncore, u32 mbox, u32 val,
			int fast_timeout_us, int slow_timeout_ms)
{
	return xe_pcode_write_timeout(__fake_uncore_to_gt(uncore), mbox, val,
				      slow_timeout_ms ?: 1);
}

static inline int
snb_pcode_write(struct fake_uncore *uncore, u32 mbox, u32 val)
{

	return xe_pcode_write(__fake_uncore_to_gt(uncore), mbox, val);
}

static inline int
snb_pcode_read(struct fake_uncore *uncore, u32 mbox, u32 *val, u32 *val1)
{
	return xe_pcode_read(__fake_uncore_to_gt(uncore), mbox, val, val1);
}

static inline int
skl_pcode_request(struct fake_uncore *uncore, u32 mbox,
		  u32 request, u32 reply_mask, u32 reply,
		  int timeout_base_ms)
{
	return xe_pcode_request(__fake_uncore_to_gt(uncore), mbox, request, reply_mask, reply,
				timeout_base_ms);
}

#endif /* __INTEL_PCODE_H__ */
