/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#ifndef _XE_HW_FENCE_H_
#define _XE_HW_FENCE_H_

#include "xe_hw_fence_types.h"

void xe_hw_fence_irq_init(struct xe_hw_fence_irq *irq);
void xe_hw_fence_irq_finish(struct xe_hw_fence_irq *irq);
void xe_hw_fence_irq_run(struct xe_hw_fence_irq *irq);

void xe_hw_fence_ctx_init(struct xe_hw_fence_ctx *ctx,
			  struct xe_hw_engine *hwe);
void xe_hw_fence_ctx_finish(struct xe_hw_fence_ctx *ctx);

struct xe_hw_fence *xe_hw_fence_create(struct xe_hw_fence_irq *irq,
				       struct xe_hw_fence_ctx *ctx,
				       struct dma_buf_map seqno_map);

#endif /* _XE_HW_FENCE_H_ */
