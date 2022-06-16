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

static int xe_exec_begin(struct xe_engine *e, struct ww_acquire_ctx *ww,
			 struct ttm_validate_buffer *tv_vm,
			 struct list_head *objs)
{
	struct xe_vm *vm = e->vm;
	struct xe_vma *vma;
	LIST_HEAD(dups);
	int err;
	int i;

	lockdep_assert_held(&vm->lock);

	if (!xe_vm_in_compute_mode(e->vm)) {
		INIT_LIST_HEAD(objs);
		list_for_each_entry(vma, &vm->external_vma_list,
				    external_vma_link) {
			XE_BUG_ON(vma->external_vma_tv.num_shared != 1);
			vma->external_vma_tv.bo = &vma->bo->ttm;
			list_add_tail(&vma->external_vma_tv.head, objs);
		}
		tv_vm->num_shared = 1;
		tv_vm->bo = xe_vm_ttm_bo(vm);;
		list_add_tail(&tv_vm->head, objs);
		err = ttm_eu_reserve_buffers(ww, objs, true, &dups);
		if (err)
			return err;
	} else {
		xe_vm_lock(e->vm, NULL);
	}

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

static void xe_exec_end(struct xe_engine *e, struct ww_acquire_ctx *ww,
			struct list_head *objs)
{
	if (!xe_vm_in_compute_mode(e->vm))
		ttm_eu_backoff_reservation(ww, objs);
	else
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
	struct ww_acquire_ctx ww;
	struct list_head objs;
	struct ttm_validate_buffer tv_vm;
	int err = 0;

	if (XE_IOCTL_ERR(xe, args->extensions))
		return -EINVAL;

	engine = xe_engine_lookup(xef, args->engine_id);
	if (XE_IOCTL_ERR(xe, !engine))
		return -ENOENT;

	if (XE_IOCTL_ERR(xe, engine->flags & ENGINE_FLAG_VM))
		return -EINVAL;

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
					  engine->flags &
					  ENGINE_FLAG_COMPUTE_MODE);
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
retry:
	err = down_read_interruptible(&vm->lock);
	if (err)
		goto err_syncs;

	err = xe_vm_userptr_pin(vm);
	if (err)
		goto err_unlock_list;

	err = xe_exec_begin(engine, &ww, &tv_vm, &objs);
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

	if (xe_vm_has_userptr(vm) && !xe_vm_in_compute_mode(vm))
		dma_resv_add_fence(&vm->resv,
				   &job->drm.s_fence->finished,
				   DMA_RESV_USAGE_BOOKKEEP);

	err = xe_vm_userptr_needs_repin(vm);

	/*
	 * Make implicit sync work across drivers, assuming all external BOs are
	 * written as we don't pass in a read / write list.
	 */
	if (!xe_vm_in_compute_mode(vm) && !err) {
		struct ttm_validate_buffer *entry;
		struct ttm_buffer_object *ttm_vm = xe_vm_ttm_bo(vm);;

		list_for_each_entry(entry, &objs, head) {
			struct ttm_buffer_object *ttm = entry->bo;

			if (ttm == ttm_vm)
				continue;

			dma_resv_add_fence(ttm->base.resv,
					   &job->drm.s_fence->finished,
					   DMA_RESV_USAGE_WRITE);
		}
	}

	for (i = 0; i < num_syncs && !err; i++)
		err = xe_sync_entry_add_deps(&syncs[i], job);

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
	xe_exec_end(engine, &ww, &objs);
err_unlock_list:
	up_read(&vm->lock);
	if (err == -EAGAIN)
		goto retry;
err_syncs:
	for (i = 0; i < num_syncs; i++)
		xe_sync_entry_cleanup(&syncs[i]);
	kfree(syncs);
err_engine:
	xe_engine_put(engine);

	return err;
}
