/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GT_PAGEFAULT_H_
#define _XE_GT_PAGEFAULT_H_

#include <linux/types.h>

struct xe_gt;
struct xe_guc;

void xe_gt_pagefault_init(struct xe_gt *gt);
void xe_gt_pagefault_reset(struct xe_gt *gt);
int xe_gt_tlb_invalidate(struct xe_gt *gt);
int xe_gt_tlb_invalidate_wait(struct xe_gt *gt, int seqno);
int xe_guc_pagefault_handler(struct xe_guc *guc, u32 *msg, u32 len);

#endif	/* _XE_GT_PAGEFAULT_ */
