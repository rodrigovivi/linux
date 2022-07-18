// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <linux/slab.h>

#include "xe_engine.h"
#include "xe_preempt_fence.h"
#include "xe_vm.h"

static void preempt_fence_work_func(struct work_struct *w)
{
	bool cookie = dma_fence_begin_signalling();
	struct xe_preempt_fence *pfence =
		container_of(w, typeof(*pfence), preempt_work);
	struct xe_engine *e = pfence->engine;
	struct dma_fence *sfence = pfence->sfence;
	struct ww_acquire_ctx ww;

	if (IS_ERR(sfence))
		dma_fence_set_error(&pfence->base, PTR_ERR(sfence));
	else
		dma_fence_wait(sfence, false);

	dma_fence_signal(&pfence->base);
	dma_fence_end_signalling(cookie);

	xe_vm_lock(e->vm, &ww, 0, false);
	/*
	 * Possible race a new preempt fence could be installed before we grab
	 * this lock, guard against that by checking
	 * DMA_FENCE_FLAG_SIGNALED_BIT.
	 */
	if (e->compute.pfence &&
	    test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &e->compute.pfence->flags)) {
		e->compute.pfence = NULL;
		dma_fence_put(e->compute.pfence);
	}
	xe_vm_unlock(e->vm, &ww);

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
			u64 context, u32 seqno)
{
	struct xe_preempt_fence *pfence;

	pfence = kmalloc(sizeof(*pfence), GFP_KERNEL);
	if (!pfence)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&pfence->link);
	pfence->engine = xe_engine_get(e);

	INIT_WORK(&pfence->preempt_work, preempt_fence_work_func);
	dma_fence_init(&pfence->base, &preempt_fence_ops,
		       &e->compute.lock, context, seqno);

	return &pfence->base;
}
