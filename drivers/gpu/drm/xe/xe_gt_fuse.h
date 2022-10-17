/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GT_FUSE_H_
#define _XE_GT_FUSE_H_

struct xe_gt;

void xe_gt_fuse_init(struct xe_gt *gt);
void xe_gt_fuse_fini(struct xe_gt *gt);

#endif
