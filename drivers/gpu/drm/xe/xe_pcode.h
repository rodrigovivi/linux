/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_PCODE_H_
#define _XE_PCODE_H_

#include <linux/types.h>
struct xe_gt;

int xe_pcode_probe(struct xe_gt *gt);
int xe_pcode_init(struct xe_gt *gt);
int xe_pcode_init_min_freq_table(struct xe_gt *gt, u32 min_gt_freq,
				 u32 max_gt_freq);

#endif
