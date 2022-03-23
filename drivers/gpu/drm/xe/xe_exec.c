/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/xe_drm.h>

#include "xe_bo.h"
#include "xe_device.h"
#include "xe_engine.h"
#include "xe_exec.h"
#include "xe_macros.h"
#include "xe_sched_job.h"
#include "xe_sync.h"
#include "xe_vm.h"

static int xe_exec_begin(struct xe_engine *e)
{
	int err;
	int i;

	xe_vm_lock(e->vm, NULL);

	for (i = 0; i < e->width; ++i) {
		err = xe_bo_vmap(e->lrc[i].bo);
		if (err)
			goto err_unlock_vm;
	}

	return 0;

err_unlock_vm:
	xe_vm_unlock(e->vm);
	return err;
}

static void xe_exec_end(struct xe_engine *e)
{
	xe_vm_unlock(e->vm);
}

int xe_exec_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct xe_device *xe = to_xe_device(dev);
	struct xe_file *xef = to_xe_file(file);
	struct drm_xe_exec *args = data;
	struct drm_xe_sync __user *syncs_user = u64_to_user_ptr(args->syncs);
	uint64_t __user *addresses_user = u64_to_user_ptr(args->address);
	struct xe_engine *engine;
	struct xe_sync_entry *syncs;
	uint64_t addresses[XE_HW_ENGINE_MAX_INSTANCE];
	uint32_t i, num_syncs = 0;
	struct xe_sched_job *job;
	int err = 0;

	if (XE_IOCTL_ERR(xe, args->extensions))
		return -EINVAL;

	engine = xe_engine_lookup(xef, args->engine_id);
	if (XE_IOCTL_ERR(xe, !engine))
		return -ENOENT;

	if (XE_IOCTL_ERR(xe, engine->width != args->num_batch_buffer))
		return -EINVAL;

	if (XE_IOCTL_ERR(xe, engine->flags & ENGINE_FLAG_BANNED)) {
		err = -ECANCELED;
		goto err_engine;
	}

	syncs = kcalloc(args->num_syncs, sizeof(*syncs), GFP_KERNEL);
	if (!syncs) {
		err = -ENOMEM;
		goto err_engine;
	}

	for (i = 0; i < args->num_syncs; i++) {
		err = xe_sync_entry_parse(xe, xef, &syncs[num_syncs++],
					  &syncs_user[i]);
		if (err)
			goto err_syncs;
	}

	if (xe_engine_is_parallel(engine)) {
		err = __copy_from_user(addresses, addresses_user, sizeof(uint64_t) *
				       engine->width);
		if (err) {
			err = -EFAULT;
			goto err_syncs;
		}
	}

	err = xe_exec_begin(engine);
	if (err)
		goto err_syncs;

	job = xe_sched_job_create(engine, xe_engine_is_parallel(engine) ?
				  addresses : &args->address);
	if (IS_ERR(job)) {
		err = PTR_ERR(job);
		goto err_engine_end;
	}

	for (i = 0; i < num_syncs; i++) {
		err = xe_sync_entry_add_deps(&syncs[i], job);
		if (err)
			goto err_put_job;
	}

	xe_sched_job_arm(job);

	for (i = 0; i < num_syncs; i++)
		xe_sync_entry_signal(&syncs[i], &job->drm.s_fence->finished);

	xe_sched_job_push(job);

err_put_job:
	if (err)
		xe_sched_job_free(job);
err_engine_end:
	xe_exec_end(engine);
err_syncs:
	for (i = 0; i < num_syncs; i++)
		xe_sync_entry_cleanup(&syncs[i]);
	kfree(syncs);
err_engine:
	xe_engine_put(engine);

	return err;
}
