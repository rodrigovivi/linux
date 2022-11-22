/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_PREEMPT_FENCE_H_
#define _XE_PREEMPT_FENCE_H_

#include "xe_preempt_fence_types.h"

struct list_head;

struct dma_fence *
xe_preempt_fence_create(struct xe_engine *e,
			u64 context, u32 seqno);

struct list_head *xe_preempt_fence_alloc(void);

void xe_preempt_fence_free(struct list_head *link);

struct dma_fence *
xe_preempt_fence_arm(struct list_head *link, struct xe_engine *e,
		     u64 context, u32 seqno);

static inline struct xe_preempt_fence *
to_preempt_fence(struct dma_fence *fence)
{
	return container_of(fence, struct xe_preempt_fence, base);
}

#endif
