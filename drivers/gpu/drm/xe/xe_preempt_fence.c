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
	struct ww_acquire_ctx ww;

	if (pfence->error)
		dma_fence_set_error(&pfence->base, pfence->error);
	else
		e->ops->suspend_wait(e);

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

	pfence->error = e->ops->suspend(e);
	queue_work(system_unbound_wq, &pfence->preempt_work);
	return true;
}

static const struct dma_fence_ops preempt_fence_ops = {
	.get_driver_name = preempt_fence_get_driver_name,
	.get_timeline_name = preempt_fence_get_timeline_name,
	.enable_signaling = preempt_fence_enable_signaling,
};

/**
 * xe_preempt_fence_alloc() - Allocate a preempt fence with minimal
 * initialization
 *
 * Allocate a preempt fence, and initialize its list head. The preserve the
 * possibility to keep struct xe_preempt_fence opaque, the function returns a
 * struct list_head that can be used for subsequent calls into the
 * xe_preempt_fence api. If the preempt_fence allocated has been armed with
 * xe_preempt_fence_arm(), it must be freed using dma_fence_put(). If not,
 * it must be freed using xe_preempt_fence_free().
 *
 * Return: A struct list_head pointer used for calling into
 * xe_preempt_fence_arm() or xe_preempt_fence_free().
 * The list head pointed to has been initialized. An error pointer on error.
 */
struct list_head *xe_preempt_fence_alloc(void)
{
	struct xe_preempt_fence *pfence;

	pfence = kmalloc(sizeof(*pfence), GFP_KERNEL);
	if (!pfence)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&pfence->link);
	INIT_WORK(&pfence->preempt_work, preempt_fence_work_func);

	return &pfence->link;
}

/**
 * xe_preempt_fence_free() - Free a preempt fence allocated using
 * xe_preempt_fence_alloc().
 * @link: struct list_head pointer obtained from xe_preempt_fence_alloc();
 *
 * Free a preempt fence that has not yet been armed.
 */
void xe_preempt_fence_free(struct list_head *link)
{
	list_del(link);
	kfree(container_of(link, struct xe_preempt_fence, link));
}

/**
 * xe_preempt_fence_arm() - Arm a preempt fence allocated using
 * xe_preempt_fence_alloc().
 * @link: The struct list_head pointer returned from xe_preempt_fence_alloc().
 * @e: The struct xe_engine used for arming.
 * @context: The dma-fence context used for arming.
 * @seqno: The dma-fence seqno used for arming.
 *
 * Inserts the preempt fence into @context's timeline, takes @link off any
 * list, and registers the struct xe_engine as the xe_engine to be preempted.
 *
 * Return: A pointer to a struct dma_fence embedded into the preempt fence.
 * This function doesn't error.
 */
struct dma_fence *
xe_preempt_fence_arm(struct list_head *link, struct xe_engine *e,
		     u64 context, u32 seqno)
{
	struct xe_preempt_fence *pfence =
		container_of(link, typeof(*pfence), link);

	list_del_init(link);
	pfence->engine = xe_engine_get(e);
	dma_fence_init(&pfence->base, &preempt_fence_ops,
		      &e->compute.lock, context, seqno);

	return &pfence->base;
}

/**
 * xe_preempt_fence_create() - Helper to create and arm a preempt fence.
 * @e: The struct xe_engine used for arming.
 * @context: The dma-fence context used for arming.
 * @seqno: The dma-fence seqno used for arming.
 *
 * Allocates and inserts the preempt fence into @context's timeline,
 * and registers @e as the struct xe_engine to be preempted.
 *
 * Return: A pointer to the resulting struct dma_fence on success. An error
 * pointer on error. In particular if allocation fails it returns
 * ERR_PTR(-ENOMEM);
 */
struct dma_fence *
xe_preempt_fence_create(struct xe_engine *e,
			u64 context, u32 seqno)
{
	struct list_head *link;

	link = xe_preempt_fence_alloc();
	if (IS_ERR(link))
		return ERR_CAST(link);

	return xe_preempt_fence_arm(link, e, context, seqno);
}
