/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_LRC_TYPES_H_
#define _XE_LRC_TYPES_H_

#include "xe_hw_fence_types.h"

struct xe_bo;

/**
 * struct xe_lrc - Logical ring context (LRC) and submission ring object
 */
struct xe_lrc {
	/**
	 * @bo: buffer object (memory) for logical ring context, per process HW
	 * status page, and submission ring.
	 */
	struct xe_bo *bo;

	/** @flags: LRC flags */
	uint32_t flags;
#define XE_LRC_PINNED BIT(1)

	/** @ring: submission ring state */
	struct {
		/** @size: size of submission ring */
		uint32_t size;
		/** @tail: tail of submission ring */
		uint32_t tail;
		/** @old_tail: shadow of tail */
		uint32_t old_tail;
	} ring;

	/** @desc: LRC descriptor */
	uint64_t desc;

	/** @fence_ctx: context for hw fence */
	struct xe_hw_fence_ctx fence_ctx;
};

#endif	/* _XE_LRC_TYPES_H_ */
