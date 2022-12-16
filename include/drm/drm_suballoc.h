/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */
#ifndef _DRM_SUBALLOC_H_
#define _DRM_SUBALLOC_H_

#include <drm/drm_mm.h>

#include <linux/dma-fence.h>
#include <linux/types.h>

/**
 * struct drm_suballoc_manager - Wrapper for fenced range allocations
 * @mm: The range manager. Protected by @lock.
 * @range_size: The total size of the range.
 * @alignment: Range alignment.
 * @wq: Wait queue for sleeping allocations on contention.
 * @idle_list: List of idle but not yet freed allocations. Protected by
 * @idle_list_lock.
 * @task: Task waiting for allocation. Protected by @lock.
 */
struct drm_suballoc_manager {
	/** @lock: Manager lock. Protects @mm. */
	spinlock_t lock;
	/**
	 * @idle_list_lock: Lock to protect the idle_list.
	 * Disable irqs when locking.
	 */
	spinlock_t idle_list_lock;
	/** @alloc_mutex: Mutex to protect against stavation. */
	struct mutex alloc_mutex;
	struct drm_mm mm;
	u64 range_size;
	u64 alignment;
	wait_queue_head_t wq;
	struct list_head idle_list;
};

/**
 * struct drm_suballoc: Suballocated range.
 * @node: The drm_mm representation of the range.
 * @fence: dma-fence indicating whether allocation is active or idle.
 * Assigned on call to free the allocation so doesn't need protection.
 * @cb: dma-fence callback structure. Used for callbacks when the fence signals.
 * @manager: The struct drm_suballoc_manager the range belongs to. Immutable.
 * @idle_link: Link for the manager idle_list. Progected by the
 * drm_suballoc_manager::idle_lock.
 */
struct drm_suballoc {
	struct drm_mm_node node;
	struct dma_fence *fence;
	struct dma_fence_cb cb;
	struct drm_suballoc_manager *manager;
	struct list_head idle_link;
};

void drm_suballoc_manager_init(struct drm_suballoc_manager *sa_manager,
			       u64 size, u64 align);

void drm_suballoc_manager_fini(struct drm_suballoc_manager *sa_manager);

struct drm_suballoc *drm_suballoc_new(struct drm_suballoc_manager *sa_manager,
				      u64 size, gfp_t gfp, bool intr);

void drm_suballoc_free(struct drm_suballoc *sa, struct dma_fence *fence);

/**
 * drm_suballoc_soffset - Range start.
 * @sa: The struct drm_suballoc.
 *
 * Return: The start of the allocated range.
 */
static inline u64 drm_suballoc_soffset(struct drm_suballoc *sa)
{
	return sa->node.start;
}

/**
 * drm_suballoc_eoffset - Range end.
 * @sa: The struct drm_suballoc.
 *
 * Return: The end of the allocated range + 1.
 */
static inline u64 drm_suballoc_eoffset(struct drm_suballoc *sa)
{
	return sa->node.start + sa->node.size;
}

/**
 * drm_suballoc_size - Range size.
 * @sa: The struct drm_suballoc.
 *
 * Return: The size of the allocated range.
 */
static inline u64 drm_suballoc_size(struct drm_suballoc *sa)
{
	return sa->node.size;
}

#ifdef CONFIG_DEBUG_FS
void drm_suballoc_dump_debug_info(struct drm_suballoc_manager *sa_manager,
				  struct drm_printer *p, u64 suballoc_base);
#else
static inline void
drm_suballoc_dump_debug_info(struct drm_suballoc_manager *sa_manager,
			     struct drm_printer *p, u64 suballoc_base)
{ }

#endif

#endif /* _DRM_SUBALLOC_H_ */
