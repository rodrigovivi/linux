/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#include "xe_sched_job.h"

#include <linux/slab.h>

#include "xe_device_types.h"
#include "xe_engine.h"
#include "xe_lrc.h"

struct xe_sched_job *xe_sched_job_create(struct xe_engine *e,
					 uint64_t user_batch_addr)
{
	struct xe_sched_job *job;
	int err;

	xe_engine_assert_held(e);

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return ERR_PTR(-ENOMEM);

	err = drm_sched_job_init(&job->drm, e->entity, NULL);
	if (err)
		goto err_free;

	job->engine = e;

	job->fence = xe_lrc_create_seqno_fence(&e->lrc);
	if (IS_ERR(job->fence)) {
		err = PTR_ERR(job->fence);
		goto err_sched_job;
	}

	job->user_batch_addr = user_batch_addr;

	return job;

err_sched_job:
	drm_sched_job_cleanup(&job->drm);
err_free:
	kfree(job);
	return ERR_PTR(err);
}

void xe_sched_job_destroy(struct xe_sched_job *job)
{
	dma_fence_put(job->fence);
	drm_sched_job_cleanup(&job->drm);
	kfree(job);
}

void xe_drm_sched_job_free(struct drm_sched_job *drm_job)
{
	xe_sched_job_destroy(to_xe_sched_job(drm_job));
}
