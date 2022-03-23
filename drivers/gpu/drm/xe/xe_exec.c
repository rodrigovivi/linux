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
	struct dma_fence *userptr_fence;
	struct xe_vm *vm;
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
					  &syncs_user[i], true,
					  engine->flags & ENGINE_FLAG_COMPUTE);
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

	vm = engine->vm;
	mutex_lock(&vm->userptr.list_lock);
retry:
	err = xe_vm_userptr_pin(vm);
	if (err)
		goto err_unlock_list;

	err = xe_exec_begin(engine);
	if (err)
		goto err_unlock_list;

	if (xe_vm_is_closed(engine->vm)) {
		drm_warn(&xe->drm, "Trying to schedule after vm is closed\n");
		err = -EIO;
		goto err_engine_end;
	}

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

	if (xe_vm_has_userptr(vm)) {
		err = dma_resv_reserve_shared(&vm->resv, 1);
		if (err)
			goto err_put_job;
	}

	userptr_fence = xe_vm_userptr_bind(vm);
	if (IS_ERR(userptr_fence)) {
		err = PTR_ERR(userptr_fence);
		goto err_put_job;
	}

	/*
	 * We store the userptr fence in the VM so subsequent execs don't get
	 * scheduled before the binding of userptrs is complete.
	 */
	if (userptr_fence) {
		dma_fence_put(vm->userptr.fence);
		vm->userptr.fence = userptr_fence;
	}
	if (vm->userptr.fence) {
		if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT,
			     &vm->userptr.fence->flags)) {
			dma_fence_put(vm->userptr.fence);
			vm->userptr.fence = NULL;
		} else {
			dma_fence_get(vm->userptr.fence);
			err = drm_sched_job_add_dependency(&job->drm,
							   vm->userptr.fence);
			if (err)
				goto err_put_job;
		}
	}

	/*
	 * Point of no return, if we error after this point just set an error on
	 * the job and let the DRM scheduler / backend clean up the job.
	 */

	xe_sched_job_arm(job);

	if (xe_vm_has_userptr(vm) && !xe_vm_has_preempt_fences(vm))
		dma_resv_add_shared_fence(&vm->resv,
					  &job->drm.s_fence->finished);

	err = xe_vm_userptr_needs_repin(vm);
	if (err)
		xe_sched_job_set_error(job, -ECANCELED);

	for (i = 0; i < num_syncs && !err; i++)
		xe_sync_entry_signal(&syncs[i], job,
				     &job->drm.s_fence->finished);

	xe_sched_job_push(job);

err_put_job:
	if (err && err != -EAGAIN)
		xe_sched_job_free(job);
err_engine_end:
	xe_exec_end(engine);
	if (err == -EAGAIN)
		goto retry;
err_unlock_list:
	mutex_unlock(&vm->userptr.list_lock);
err_syncs:
	for (i = 0; i < num_syncs; i++)
		xe_sync_entry_cleanup(&syncs[i]);
	kfree(syncs);
err_engine:
	xe_engine_put(engine);

	return err;
}
