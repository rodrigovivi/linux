/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#include <linux/slab.h>

#include "xe_engine.h"
#include "xe_preempt_fence.h"

static void preempt_fence_work_func(struct work_struct *w)
{
	bool cookie = dma_fence_begin_signalling();
	struct xe_preempt_fence *pfence =
		container_of(w, typeof(*pfence), preempt_work);
	struct xe_engine *e = pfence->engine;
	struct dma_fence *sfence = pfence->sfence;

	if (IS_ERR(sfence))
		goto err_out;

	dma_fence_wait(sfence, false);
	dma_fence_signal(&pfence->base);
	dma_fence_end_signalling(cookie);

	pfence->ops->preempt_complete(e);
	xe_engine_put(e);

	return;

err_out:
	dma_fence_set_error(&pfence->base, PTR_ERR(sfence));
	dma_fence_signal(&pfence->base);
	dma_fence_end_signalling(cookie);
	dma_fence_put(&pfence->base);
	xe_engine_put(e);
}

static const char *
preempt_fence_get_driver_name(struct dma_fence *fence)
{
	return "xe";
}

static const char *
preempt_fence_get_timeline_name(struct dma_fence *fence)
{
	return "preempt";
}

static bool preempt_fence_enable_signaling(struct dma_fence *fence)
{
	struct xe_preempt_fence *pfence =
		container_of(fence, typeof(*pfence), base);
	struct xe_engine *e = pfence->engine;

	pfence->sfence = e->ops->suspend(e);
	queue_work(system_unbound_wq, &pfence->preempt_work);
	return true;
}

static const struct dma_fence_ops preempt_fence_ops = {
	.get_driver_name = preempt_fence_get_driver_name,
	.get_timeline_name = preempt_fence_get_timeline_name,
	.enable_signaling = preempt_fence_enable_signaling,
};

struct dma_fence *
xe_preempt_fence_create(struct xe_engine *e,
			const struct xe_preempt_fence_ops *ops,
			u64 context, u32 seqno)
{
	struct xe_preempt_fence *pfence;

	pfence = kmalloc(sizeof(*pfence), GFP_KERNEL);
	if (!pfence)
		return ERR_PTR(-ENOMEM);

	pfence->engine = xe_engine_get(e);
	pfence->ops = ops;

	INIT_WORK(&pfence->preempt_work, preempt_fence_work_func);
	dma_fence_init(&pfence->base, &preempt_fence_ops,
		       &e->compute.lock, context, seqno);

	return &pfence->base;
}
