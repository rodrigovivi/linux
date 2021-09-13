/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#ifndef _XE_HW_FENCE_H_
#define _XE_HW_FENCE_H_

#include <linux/dma-buf-map.h>
#include <linux/dma-fence.h>

struct xe_hw_fence_irq {
	spinlock_t lock;
	struct list_head pending;
};

void xe_hw_fence_irq_init(struct xe_hw_fence_irq *irq);
void xe_hw_fence_irq_finish(struct xe_hw_fence_irq *irq);
void xe_hw_fence_irq_run(struct xe_hw_fence_irq *irq);


struct xe_hw_fence_ctx {
	struct xe_hw_engine *hwe;
	uint64_t dma_fence_ctx;
	uint32_t next_seqno;
};

void xe_hw_fence_ctx_init(struct xe_hw_fence_ctx *ctx,
			  struct xe_hw_engine *hwe);
void xe_hw_fence_ctx_finish(struct xe_hw_fence_ctx *ctx);


struct xe_hw_fence {
	struct dma_fence dma;

	struct xe_hw_fence_ctx *ctx;

	struct dma_buf_map seqno_map;

	/** irq_link: Link in struct xe_hw_fence_irq.pending */
	struct list_head irq_link;
};

struct xe_hw_fence *xe_hw_fence_create(struct xe_hw_fence_irq *irq,
				       struct xe_hw_fence_ctx *ctx,
				       struct dma_buf_map seqno_map);

#endif /* _XE_FENCE_H_ */
