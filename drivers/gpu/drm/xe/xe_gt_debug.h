/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef __XE_GT_DEBUG_
#define __XE_GT_DEBUG_

#include <linux/bits.h>
#include <linux/math.h>

struct xe_eu_attentions;
struct dentry;
struct xe_gt;

#define XE_GT_ATTENTION_TIMEOUT_MS 150

unsigned int xe_gt_eu_att_regs(struct xe_gt *gt);

int xe_gt_eu_threads_needing_attention(struct xe_gt *gt);
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

int xe_gt_eu_attentions_read(struct xe_gt *gt,
			     struct xe_eu_attentions *a,
			     const unsigned int settle_time_ms);

#if IS_ENABLED(CONFIG_DRM_XE_EUDEBUG)
void xe_gt_debug_debugfs_register(struct xe_gt *gt, struct dentry *root);
#else
static inline void
xe_gt_debug_debugfs_register(struct xe_gt *gt, struct dentry *root) { }
#endif

#endif
