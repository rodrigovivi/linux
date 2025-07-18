/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef _XE_DEBUG_DATA_TYPES_H_
#define _XE_DEBUG_DATA_TYPES_H_

#include <linux/limits.h>
#include <linux/list.h>
#include <linux/types.h>

struct xe_debug_data {
	struct list_head link;
	u64 addr;
	u64 range;
	u64 flags;
	u64 offset;
	union {
		u64 pseudopath;
		char pathname[PATH_MAX];
	};
};

#endif /* _XE_DEBUG_DATA_TYPES_H_ */
