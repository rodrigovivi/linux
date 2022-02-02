/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#include "xe_hw_fence.h"

#include <linux/device.h>
#include <linux/slab.h>

#include "xe_device.h"
#include "xe_hw_engine.h"
#include "xe_macros.h"
#include "xe_bo.h"

void xe_hw_fence_irq_init(struct xe_hw_fence_irq *irq)
{
	spin_lock_init(&irq->lock);
	INIT_LIST_HEAD(&irq->pending);
}

void xe_hw_fence_irq_finish(struct xe_hw_fence_irq *irq)
{
	struct xe_hw_fence *fence, *next;
	unsigned long flags;
	int err;
	bool tmp;

	if (XE_WARN_ON(!list_empty(&irq->pending))) {
		tmp = dma_fence_begin_signalling();
		spin_lock_irqsave(&irq->lock, flags);
		list_for_each_entry_safe(fence, next, &irq->pending, irq_link) {
			list_del_init(&fence->irq_link);
			err = dma_fence_signal_locked(&fence->dma);
			XE_WARN_ON(err);
		}
		spin_unlock_irqrestore(&irq->lock, flags);
		dma_fence_end_signalling(tmp);
	}
}

void xe_hw_fence_irq_run(struct xe_hw_fence_irq *irq)
{
	struct xe_hw_fence *fence, *next;
	bool tmp;

	tmp = dma_fence_begin_signalling();
	spin_lock(&irq->lock);
	list_for_each_entry_safe(fence, next, &irq->pending, irq_link) {
		if (dma_fence_is_signaled_locked(&fence->dma))
			list_del_init(&fence->irq_link);
	}
	spin_unlock(&irq->lock);
	dma_fence_end_signalling(tmp);
}

void xe_hw_fence_ctx_init(struct xe_hw_fence_ctx *ctx,
			  struct xe_hw_engine *hwe)
{
	ctx->hwe = hwe;
	ctx->dma_fence_ctx = dma_fence_context_alloc(1);
	ctx->next_seqno = 1;
}

void xe_hw_fence_ctx_finish(struct xe_hw_fence_ctx *ctx)
{
}

static struct xe_hw_fence *to_xe_hw_fence(struct dma_fence *fence);

static struct xe_hw_fence_irq *xe_hw_fence_irq(struct xe_hw_fence *fence)
{
	return container_of(fence->dma.lock, struct xe_hw_fence_irq, lock);
}

static const char *xe_hw_fence_get_driver_name(struct dma_fence *dma_fence)
{
	struct xe_hw_fence *fence = to_xe_hw_fence(dma_fence);

	return dev_name(fence->ctx->hwe->xe->drm.dev);
}

static const char *xe_hw_fence_get_timeline_name(struct dma_fence *dma_fence)
{
	struct xe_hw_fence *fence = to_xe_hw_fence(dma_fence);

	/* TODO: This is supposed to be a timeline name, not the HW engine */
	return fence->ctx->hwe->name;
}

static bool xe_hw_fence_enable_signaling(struct dma_fence *dma_fence)
{
	struct xe_hw_fence *fence = to_xe_hw_fence(dma_fence);
	struct xe_hw_fence_irq *irq = xe_hw_fence_irq(fence);

	list_add(&fence->irq_link, &irq->pending);

	return true;
}

static bool xe_hw_fence_signaled(struct dma_fence *dma_fence)
{
	struct xe_hw_fence *fence = to_xe_hw_fence(dma_fence);
	uint32_t seqno = dbm_read32(fence->seqno_map);

	return (int32_t)fence->dma.seqno <= (int32_t)seqno;
}

static void xe_hw_fence_release(struct dma_fence *dma_fence)
{
	struct xe_hw_fence *fence = to_xe_hw_fence(dma_fence);
	unsigned long flags;

	if (!list_empty(&fence->irq_link)) {
		spin_lock_irqsave(fence->dma.lock, flags);
		list_del(&fence->irq_link);
		spin_unlock_irqrestore(fence->dma.lock, flags);
	}

	kfree_rcu(fence, dma.rcu);
}

static const struct dma_fence_ops xe_hw_fence_ops = {
	.get_driver_name = xe_hw_fence_get_driver_name,
	.get_timeline_name = xe_hw_fence_get_timeline_name,
	.enable_signaling = xe_hw_fence_enable_signaling,
	.signaled = xe_hw_fence_signaled,
	.release = xe_hw_fence_release,
};

static struct xe_hw_fence *to_xe_hw_fence(struct dma_fence *fence)
{
	if (XE_WARN_ON(fence->ops != &xe_hw_fence_ops))
		return NULL;

	return container_of(fence, struct xe_hw_fence, dma);
}

struct xe_hw_fence *xe_hw_fence_create(struct xe_hw_fence_irq *irq,
				       struct xe_hw_fence_ctx *ctx,
				       struct dma_buf_map seqno_map)
{
	struct xe_hw_fence *fence;

	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (!fence)
		return ERR_PTR(-ENOMEM);


	dma_fence_init(&fence->dma, &xe_hw_fence_ops, &irq->lock,
		       ctx->dma_fence_ctx, ctx->next_seqno++);

	fence->ctx = ctx;
	fence->seqno_map = seqno_map;
	INIT_LIST_HEAD(&fence->irq_link);

	return fence;
}
