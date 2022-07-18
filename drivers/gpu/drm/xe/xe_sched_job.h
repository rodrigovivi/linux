/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef _XE_SCHED_JOB_H_
#define _XE_SCHED_JOB_H_

#include "xe_sched_job_types.h"

#define XE_SCHED_HANG_LIMIT 1
#define XE_SCHED_JOB_TIMEOUT LONG_MAX

int xe_sched_job_module_init(void);
void xe_sched_job_module_exit(void);

struct xe_sched_job *xe_sched_job_create(struct xe_engine *e,
					 uint64_t *batch_addr);
void xe_sched_job_free(struct xe_sched_job *job);

void xe_sched_job_set_error(struct xe_sched_job *job, int error);
static inline bool xe_sched_job_is_error(struct xe_sched_job *job)
{
	return job->fence->error < 0;
}

bool xe_sched_job_started(struct xe_sched_job *job);
bool xe_sched_job_completed(struct xe_sched_job *job);

void xe_sched_job_arm(struct xe_sched_job *job);
void xe_sched_job_push(struct xe_sched_job *job);

static inline struct xe_sched_job *
to_xe_sched_job(struct drm_sched_job *drm)
{
	return container_of(drm, struct xe_sched_job, drm);
}

static inline u32 xe_sched_job_seqno(struct xe_sched_job *job)
{
	return job->fence->seqno;
}

#endif /* _XE_SCHED_JOB_H_ */
