/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_PREEMPT_FENCE_H_
#define _XE_PREEMPT_FENCE_H_

#include "xe_preempt_fence_types.h"

struct dma_fence *
xe_preempt_fence_create(struct xe_engine *e,
			u64 context, u32 seqno);

static inline struct xe_preempt_fence *
to_preempt_fence(struct dma_fence *fence)
{
	return container_of(fence, struct xe_preempt_fence, base);
}

#endif	/* _XE_PREEMPT_FENCE_H_ */
