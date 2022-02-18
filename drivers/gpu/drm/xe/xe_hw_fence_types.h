/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_HW_FENCE_TYPES_H_
#define _XE_HW_FENCE_TYPES_H_

#include <linux/dma-buf-map.h>
#include <linux/dma-fence.h>
#include <linux/irq_work.h>
#include <linux/list.h>
#include <linux/spinlock.h>

struct xe_gt;

struct xe_hw_fence_irq {
	spinlock_t lock;
	struct irq_work work;
	struct list_head pending;
};

#define MAX_FENCE_NAME_LEN	16

struct xe_hw_fence_ctx {
	struct xe_gt *gt;
	struct xe_hw_fence_irq *irq;
	uint64_t dma_fence_ctx;
	uint32_t next_seqno;
	char name[MAX_FENCE_NAME_LEN];
};

struct xe_hw_fence {
	struct dma_fence dma;

	struct xe_hw_fence_ctx *ctx;

	struct dma_buf_map seqno_map;

	/** irq_link: Link in struct xe_hw_fence_irq.pending */
	struct list_head irq_link;
};

#endif /* _XE_HW_FENCE_TYPES_H_ */
