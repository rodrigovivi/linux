/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GUC_ENGINE_TYPES_H_
#define _XE_GUC_ENGINE_TYPES_H_

#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include <drm/gpu_scheduler.h>

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
	/** @fini_async: do final fini async from this worker */
	struct work_struct fini_async;
	/** @state: GuC specific state for this xe_engine */
	u32 state;
	/** @wqi_head: work queue item tail */
	u32 wqi_head;
	/** @wqi_tail: work queue item tail */
	u32 wqi_tail;
	/** @id: GuC id for this xe_engine */
	u16 id;
	/** @reset: Engine reset */
	bool reset;
	/** @killed: Engine killed */
	bool killed;
};

#endif	/* _XE_GUC_ENGINE_TYPES_H_ */
