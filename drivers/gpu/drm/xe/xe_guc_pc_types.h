/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GUC_PC_TYPES_H_
#define _XE_GUC_PC_TYPES_H_

#include <linux/types.h>
#include <linux/mutex.h>

/**
 * struct xe_guc_pc - GuC Power Conservation (PC)
 */
struct xe_guc_pc {
	/** @bo: GGTT buffer object that is shared with GuC PC */
	struct xe_bo *bo;
	/** @rp0_freq: HW RP0 frequency - The Maximum one */
	u32 rp0_freq;
	/** @rpe_freq: HW RPe frequency - The Efficient one */
	u32 rpe_freq;
	/** @rpn_freq: HW RPN frequency - The Minimum one */
	u32 rpn_freq;
	/** @min_req_freq: Stash the minimum requested freq */
	u32 min_req_freq;
	/** @max_req_freq: Stash the maximum requested freq */
	u32 max_req_freq;
	/** @lock: Let's protect the min and max requested freq from races */
	struct mutex lock;
};

#endif	/* _XE_GUC_PC_TYPES_H_ */
