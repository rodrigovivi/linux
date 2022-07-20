/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GUC_ENGINE_TYPES_H_
#define _XE_GUC_ENGINE_TYPES_H_

#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include <drm/gpu_scheduler.h>

struct dma_fence;
struct xe_engine;

/**
 * struct xe_guc_engine - GuC specific state for an xe_engine
 */
struct xe_guc_engine {
	/** @engine: Backpointer to parent xe_engine */
	struct xe_engine *engine;
	/** @sched: GPU scheduler for this xe_engine */
	struct drm_gpu_scheduler sched;
	/** @entity: Scheduler entity for this xe_engine */
	struct drm_sched_entity entity;
	/**
	 * @static_msgs: Static messages for this xe_engine, used when a message
	 * needs to sent through the GPU scheduler but memory allocations are
	 * not allowed.
	 */
#define MAX_STATIC_MSG_TYPE	3
	struct drm_sched_msg static_msgs[MAX_STATIC_MSG_TYPE];
	/** @fini_async: do final fini async from this worker */
	struct work_struct fini_async;
	/** @static_fence: static fence used for suspend */
	struct dma_fence static_fence;
	/**
	 * @suspend_fence: suspend fence, only non-NULL when suspend in flight
	 */
	struct dma_fence *suspend_fence;
	/** @resume_time: time of last resume */
	u64 resume_time;
	/** @state: GuC specific state for this xe_engine */
	atomic_t state;
	/** @wqi_head: work queue item tail */
	u32 wqi_head;
	/** @wqi_tail: work queue item tail */
	u32 wqi_tail;
	/** @id: GuC id for this xe_engine */
	u16 id;
};

#endif	/* _XE_GUC_ENGINE_TYPES_H_ */
