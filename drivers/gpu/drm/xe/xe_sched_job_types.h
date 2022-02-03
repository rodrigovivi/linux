/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_SCHED_JOB_TYPES_H_
#define _XE_SCHED_JOB_TYPES_H_

#include <drm/gpu_scheduler.h>

struct xe_engine;

struct xe_sched_job {
	struct drm_sched_job drm;

	struct xe_engine *engine;

	struct dma_fence *fence;

	uint64_t user_batch_addr;
};

#endif	/* _XE_SCHED_JOB_TYPES_H_ */
