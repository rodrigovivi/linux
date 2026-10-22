/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef __XE_GT_DEBUG_
#define __XE_GT_DEBUG_

#include <linux/types.h>

struct dentry;
struct xe_gt;

#define XE_GT_EU_ATT_ROWS 2u

unsigned int xe_gt_eu_att_regs(struct xe_gt *gt);

int xe_gt_foreach_dss_group_instance(struct xe_gt *gt,
				     int (*fn)(struct xe_gt *gt,
					       void *data,
					       u16 group,
					       u16 instance,
					       bool present),
				     void *data);

int xe_gt_eu_attention_bitmap_size(struct xe_gt *gt);
int xe_gt_eu_attention_bitmap(struct xe_gt *gt, u8 *bits,
			      unsigned int bitmap_size);

#if IS_ENABLED(CONFIG_DRM_XE_EUDEBUG)
void xe_gt_debug_debugfs_register(struct xe_gt *gt, struct dentry *root);
#else
static inline void
xe_gt_debug_debugfs_register(struct xe_gt *gt, struct dentry *root) { }
#endif

#endif
