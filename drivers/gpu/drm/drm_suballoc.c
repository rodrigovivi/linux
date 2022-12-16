// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <drm/drm_suballoc.h>

/**
 * DOC:
 * This suballocator intends to be a wrapper around a range allocator
 * that is aware also of deferred range freeing with fences. Currently
 * we hard-code the drm_mm as the range allocator.
 * The approach, while rather simple, suffers from three performance
 * issues that can all be fixed if needed at the tradeoff of more and / or
 * more complex code:
 *
 * 1) It's cpu-hungry, the drm_mm allocator is overkill. Either code a
 * much simpler range allocator, or let the caller decide by providing
 * ops that wrap any range allocator. Also could avoid waking up unless
 * there is a reasonable chance of enough space in the range manager.
 *
 * 2) We unnecessarily install the fence callbacks too early, forcing
 * enable_signaling() too early causing extra driver effort. This is likely
 * not an issue if used with the drm_scheduler since it calls
 * enable_signaling() early anyway.
 *
 * 3) Long processing in irq (disabled) context. We've mostly worked around
 * that already by using the idle_list. If that workaround is deemed to
 * complex for little gain, we can remove it and use spin_lock_irq()
 * throughout the manager. If we want to shorten processing in irq context
 * even further, we can skip the spin_trylock in __drm_suballoc_free() and
 * avoid freeing allocations from irq context altogeher. However drm_mm
 * should be quite fast at freeing ranges.
 *
 * 4) Shrinker that starts processing the list items in 2) and 3) to play
 * better with the system.
 */

static void drm_suballoc_process_idle(struct drm_suballoc_manager *sa_manager);

/**
 * drm_suballoc_manager_init() - Initialise the drm_suballoc_manager
 * @sa_manager: pointer to the sa_manager
 * @size: number of bytes we want to suballocate
 * @align: alignment for each suballocated chunk
 *
 * Prepares the suballocation manager for suballocations.
 */
void drm_suballoc_manager_init(struct drm_suballoc_manager *sa_manager,
			       u64 size, u64 align)
{
	spin_lock_init(&sa_manager->lock);
	spin_lock_init(&sa_manager->idle_list_lock);
	mutex_init(&sa_manager->alloc_mutex);
	drm_mm_init(&sa_manager->mm, 0, size);
	init_waitqueue_head(&sa_manager->wq);
	sa_manager->range_size = size;
	sa_manager->alignment = align;
	INIT_LIST_HEAD(&sa_manager->idle_list);
}
EXPORT_SYMBOL(drm_suballoc_manager_init);

/**
 * drm_suballoc_manager_fini() - Destroy the drm_suballoc_manager
 * @sa_manager: pointer to the sa_manager
 *
 * Cleans up the suballocation manager after use. All fences added
 * with drm_suballoc_free() must be signaled, or we cannot clean up
 * the entire manager.
 */
void drm_suballoc_manager_fini(struct drm_suballoc_manager *sa_manager)
{
	drm_suballoc_process_idle(sa_manager);
	drm_mm_takedown(&sa_manager->mm);
	mutex_destroy(&sa_manager->alloc_mutex);
}
EXPORT_SYMBOL(drm_suballoc_manager_fini);

static void __drm_suballoc_free(struct drm_suballoc *sa)
{
	struct drm_suballoc_manager *sa_manager = sa->manager;
	struct dma_fence *fence;

	/*
	 * In order to avoid protecting the potentially lengthy drm_mm manager
	 * *allocation* processing with an irq-disabling lock,
	 * defer touching the drm_mm for freeing until we're in task context,
	 * with no irqs disabled, or happen to succeed in taking the manager
	 * lock.
	 */
	if (!in_task() || irqs_disabled()) {
		unsigned long irqflags;

		if (spin_trylock(&sa_manager->lock))
			goto locked;

		spin_lock_irqsave(&sa_manager->idle_list_lock, irqflags);
		list_add_tail(&sa->idle_link, &sa_manager->idle_list);
		spin_unlock_irqrestore(&sa_manager->idle_list_lock, irqflags);
		wake_up(&sa_manager->wq);
		return;
	}

	spin_lock(&sa_manager->lock);
locked:
	drm_mm_remove_node(&sa->node);

	fence = sa->fence;
	sa->fence = NULL;
	spin_unlock(&sa_manager->lock);
	/* Maybe only wake if first mm hole is sufficiently large? */
	wake_up(&sa_manager->wq);
	dma_fence_put(fence);
	kfree(sa);
}

/* Free all deferred idle allocations */
static void drm_suballoc_process_idle(struct drm_suballoc_manager *sa_manager)
{
	/*
	 * prepare_to_wait() / wake_up() semantics ensure that any list
	 * addition that was done before wake_up() is visible when
	 * this code is called from the wait loop.
	 */
	if (!list_empty_careful(&sa_manager->idle_list)) {
		struct drm_suballoc *sa, *next;
		unsigned long irqflags;
		LIST_HEAD(list);

		spin_lock_irqsave(&sa_manager->idle_list_lock, irqflags);
		list_splice_init(&sa_manager->idle_list, &list);
		spin_unlock_irqrestore(&sa_manager->idle_list_lock, irqflags);

		list_for_each_entry_safe(sa, next, &list, idle_link)
			__drm_suballoc_free(sa);
	}
}

static void
drm_suballoc_fence_signaled(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct drm_suballoc *sa = container_of(cb, typeof(*sa), cb);

	__drm_suballoc_free(sa);
}

static int drm_suballoc_tryalloc(struct drm_suballoc *sa, u64 size)
{
	struct drm_suballoc_manager *sa_manager = sa->manager;
	int err;

	drm_suballoc_process_idle(sa_manager);
	spin_lock(&sa_manager->lock);
	err = drm_mm_insert_node_generic(&sa_manager->mm, &sa->node, size,
					 sa_manager->alignment, 0,
					 DRM_MM_INSERT_EVICT);
	spin_unlock(&sa_manager->lock);
	return err;
}

/**
 * drm_suballoc_new() - Make a suballocation.
 * @sa_manager: pointer to the sa_manager
 * @size: number of bytes we want to suballocate.
 * @gfp: Allocation context.
 * @intr: Whether to sleep interruptibly if sleeping.
 *
 * Try to make a suballocation of size @size, which will be rounded
 * up to the alignment specified in specified in drm_suballoc_manager_init().
 *
 * Returns a new suballocated bo, or an ERR_PTR.
 */
struct drm_suballoc*
drm_suballoc_new(struct drm_suballoc_manager *sa_manager, u64 size,
		 gfp_t gfp, bool intr)
{
	struct drm_suballoc *sa;
	DEFINE_WAIT(wait);
	int err = 0;

	if (size > sa_manager->range_size)
		return ERR_PTR(-ENOSPC);

	sa = kzalloc(sizeof(*sa), gfp);
	if (!sa)
		return ERR_PTR(-ENOMEM);

	/* Avoid starvation using the alloc_mutex */
	if (intr)
		err = mutex_lock_interruptible(&sa_manager->alloc_mutex);
	else
		mutex_lock(&sa_manager->alloc_mutex);
	if (err) {
		kfree(sa);
		return ERR_PTR(err);
	}

	sa->manager = sa_manager;
	err = drm_suballoc_tryalloc(sa, size);
	if (err != -ENOSPC)
		goto out;

	for (;;) {
		prepare_to_wait(&sa_manager->wq, &wait,
				intr ? TASK_INTERRUPTIBLE :
				TASK_UNINTERRUPTIBLE);

		err = drm_suballoc_tryalloc(sa, size);
		if (err != -ENOSPC)
			break;

		if (intr && signal_pending(current)) {
			err = -ERESTARTSYS;
			break;
		}

		io_schedule();
	}
	finish_wait(&sa_manager->wq, &wait);

out:
	mutex_unlock(&sa_manager->alloc_mutex);
	if (!sa->node.size) {
		kfree(sa);
		WARN_ON(!err);
		sa = ERR_PTR(err);
	}

	return sa;
}
EXPORT_SYMBOL(drm_suballoc_new);

/**
 * drm_suballoc_free() - Free a suballocation
 * @suballoc: pointer to the suballocation
 * @fence: fence that signals when suballocation is idle
 * @queue: the index to which queue the suballocation will be placed on the free list.
 *
 * Free the suballocation. The suballocation can be re-used after @fence
 * signals.
 */
void
drm_suballoc_free(struct drm_suballoc *sa, struct dma_fence *fence)
{
	if (!sa)
		return;

	if (!fence || dma_fence_is_signaled(fence)) {
		__drm_suballoc_free(sa);
		return;
	}

	sa->fence = dma_fence_get(fence);
	if (dma_fence_add_callback(fence, &sa->cb, drm_suballoc_fence_signaled))
		__drm_suballoc_free(sa);
}
EXPORT_SYMBOL(drm_suballoc_free);

#ifdef CONFIG_DEBUG_FS

/**
 * drm_suballoc_dump_debug_info() - Dump the suballocator state
 * @sa_manager: The suballoc manager.
 * @p: Pointer to a drm printer for output.
 * @suballoc_base: Constant to add to the suballocated offsets on printout.
 *
 * This function dumps the suballocator state. Note that the caller has
 * to explicitly order frees and calls to this function in order for the
 * freed node to show up as protected by a fence.
 */
void drm_suballoc_dump_debug_info(struct drm_suballoc_manager *sa_manager,
				  struct drm_printer *p, u64 suballoc_base)
{
	const struct drm_mm_node *entry;

	spin_lock(&sa_manager->lock);
	drm_mm_for_each_node(entry, &sa_manager->mm) {
		struct drm_suballoc *sa =
			container_of(entry, typeof(*sa), node);

		drm_printf(p, " ");
		drm_printf(p, "[0x%010llx 0x%010llx] size %8lld",
			   (unsigned long long)suballoc_base + entry->start,
			   (unsigned long long)suballoc_base + entry->start +
			   entry->size, (unsigned long long)entry->size);

		if (sa->fence)
			drm_printf(p, " protected by 0x%016llx on context %llu",
				   (unsigned long long)sa->fence->seqno,
				   (unsigned long long)sa->fence->context);

		drm_printf(p, "\n");
	}
	spin_unlock(&sa_manager->lock);
}
EXPORT_SYMBOL(drm_suballoc_dump_debug_info);
#endif

MODULE_AUTHOR("Intel Corporation");
MODULE_DESCRIPTION("Simple range suballocator helper");
MODULE_LICENSE("GPL and additional rights");
