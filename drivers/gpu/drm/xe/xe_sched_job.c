/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#include "xe_sched_job.h"

#include <linux/dma-fence-array.h>
#include <linux/slab.h>

#include "xe_device_types.h"
#include "xe_engine.h"
#include "xe_gt.h"
#include "xe_hw_engine_types.h"
#include "xe_lrc.h"
#include "xe_macros.h"
#include "xe_trace.h"

struct xe_sched_job *xe_sched_job_create(struct xe_engine *e,
					 uint64_t *batch_addr)
{
	struct xe_sched_job *job;
	struct dma_fence **fences;
	int err;
	int i, j;

	xe_engine_assert_held(e);

	job = kzalloc(sizeof(*job) + sizeof(uint64_t) * e->width, GFP_KERNEL);
	if (!job)
		return ERR_PTR(-ENOMEM);

	err = drm_sched_job_init(&job->drm, e->entity, NULL);
	if (err)
		goto err_free;

	job->engine = e;

	if (!xe_engine_is_parallel(e)) {
		job->fence = xe_lrc_create_seqno_fence(e->lrc);
		if (IS_ERR(job->fence)) {
			err = PTR_ERR(job->fence);
			goto err_sched_job;
		}
	} else {
		struct dma_fence_array *cf;

		fences = kmalloc_array(e->width, sizeof(*fences), GFP_KERNEL);
		if (!fences) {
			err = -ENOMEM;
			goto err_sched_job;
		}

		for (j = 0; j < e->width; ++j) {
			fences[j] = xe_lrc_create_seqno_fence(e->lrc + j);
			if (IS_ERR(fences[j])) {
				err = PTR_ERR(fences[j]);
				goto err_fences;
			}
		}

		cf = dma_fence_array_create(e->width, fences,
					    e->parallel.composite_fence_ctx,
					    e->parallel.composite_fence_seqno++,
					    false);
		if (!cf) {
			--e->parallel.composite_fence_seqno;
			err = -ENOMEM;
			goto err_fences;
		}

		/* Move ownership to dma_fence_array created above */
		for (j = 0; j < e->width; ++j) {
			XE_BUG_ON(cf->base.seqno != fences[j]->seqno);
			dma_fence_get(fences[j]);
		}

		job->fence = &cf->base;
	}

	for (i = 0; i < e->width; ++i)
		job->batch_addr[i] = batch_addr[i];

	return job;

err_fences:
	for (j = j - 1; j >= 0; --j) {
		--e->lrc[j].fence_ctx.next_seqno;
		dma_fence_put(fences[j]);
	}
	kfree(fences);
err_sched_job:
	drm_sched_job_cleanup(&job->drm);
err_free:
	kfree(job);
	return ERR_PTR(err);
}

void xe_sched_job_free(struct xe_sched_job *job)
{
	dma_fence_put(job->fence);
	drm_sched_job_cleanup(&job->drm);
	kfree(job);
}

bool xe_sched_job_started(struct xe_sched_job *job)
{
	struct xe_lrc *lrc = job->engine->lrc;

	return xe_lrc_start_seqno(lrc) >= xe_sched_job_seqno(job);
}

bool xe_sched_job_completed(struct xe_sched_job *job)
{
	struct xe_lrc *lrc = job->engine->lrc;

	/*
	 * Can safely check just LRC[0] seqno as that is last seqno written when
	 * parallel handshake is done.
	 */

	return xe_lrc_seqno(lrc) >= xe_sched_job_seqno(job);
}

void xe_sched_job_arm(struct xe_sched_job *job)
{
	drm_sched_job_arm(&job->drm);
}

void xe_sched_job_push(struct xe_sched_job *job)
{
	xe_engine_get(job->engine);
	trace_xe_sched_job_exec(job);
	drm_sched_entity_push_job(&job->drm);
}
