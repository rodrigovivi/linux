/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#ifndef _XE_SCHED_JOB_H_
#define _XE_SCHED_JOB_H_

#include "xe_sched_job_types.h"

#define XE_SCHED_HANG_LIMIT 1
#define XE_SCHED_JOB_TIMEOUT LONG_MAX

struct xe_sched_job *xe_sched_job_create(struct xe_engine *e,
					 uint64_t user_batch_addr);
void xe_sched_job_destroy(struct xe_sched_job *job);
void xe_drm_sched_job_free(struct drm_sched_job *drm_job);

static inline struct xe_sched_job *
to_xe_sched_job(struct drm_sched_job *drm)
{
	return container_of(drm, struct xe_sched_job, drm);
}

#endif /* _XE_SCHED_JOB_H_ */
