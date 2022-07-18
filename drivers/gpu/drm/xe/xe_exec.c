// SPDX-License-Identifier: MIT
/*
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

/**
 * DOC: Execbuf (User GPU command submission)
 *
 * Execs have historically been rather complicated in DRM drivers (at least in
 * the i915) because a few things:
 *
 * - Passing in a list BO which are read / written to creating implicit syncs
 * - Binding at exec time
 * - Flow controlling the ring at exec time
 *
 * In XE we avoid all of this complication by not allowing a BO list to be
 * passed into an exec, using the dma-buf implicit sync uAPI, have binds as
 * seperate operations, and using the DRM scheduler to flow control the ring.
 * Let's deep dive on each of these.
 *
 * We can get away from a BO list by forcing the user to use in / out fences on
 * every exec rather than the kernel tracking dependencies of BO (e.g. if the
 * user knows an exec writes to a BO and reads from the BO in the next exec, it
 * is the user's responsibility to pass in / out fence between the two execs).
 *
 * Implicit dependencies for external BOs are handled by using the dma-buf
 * implicit dependency uAPI (TODO: add link). To make this work each exec must
 * install the job's fence into the DMA_RESV_USAGE_WRITE slot of every external
 * BO mapped in the VM.
 *
 * We do not allow a user to trigger a bind at exec time rather we have a VM
 * bind IOCTL which uses the same in / out fence interface as exec. In that
 * sense, a VM bind is basically the same operation as an exec from the user
 * perspective. e.g. If an exec depends on a VM bind use the in / out fence
 * interface (struct drm_xe_sync) to synchronize like syncing between two
 * dependent execs.
 *
 * Although a user cannot trigger a bind, we still have to rebind userptrs in
 * the VM that have been invalidated since the last exec, likewise we also have
 * to rebind BOs that have been evicted by the kernel. We schedule these rebinds
 * behind any pending kernel operations on any external BOs in VM or any BOs
 * private to the VM. This is accomplished by the rebinds waiting on BOs
 * DMA_RESV_USAGE_KERNEL slot (kernel ops) and kernel ops waiting on all BOs
 * slots (inflight execs are in the DMA_RESV_USAGE_BOOKING for private BOs and
 * in DMA_RESV_USAGE_WRITE for external BOs). All of this applies to non-compute
 * VMs only as for compute mode we use preempt fences + a rebind worker.
 *
 * There is no need to flow control the ring in the exec as we write the ring at
 * submission time and set the DRM scheduler max job limit SIZE_OF_RING /
 * MAX_JOB_SIZE. The DRM scheduler will then hold all jobs until space in the
 * ring is available.
 *
 * All of this results in a rather simple exec implementation.
 *
 * Flow
 * ~~~~
 *
 * Parse input arguments
 * Wait for any async VM bind passed as in-fences to start
 * <----------------------------------------------------------------------|
 * Lock VM lists in read mode                                             |
 * Pin userptrs (also finds userptr invalidated since last exec)          |
 * Lock exec (VM dma-resv lock, external BOs dma-resv locks)              |
 * Validate BOs that have been evicted                                    |
 * Create job                                                             |
 * Rebind invalidated userptrs + evicted BOs (non-compute-mode)           |
 * Add rebind fence dependency to job                                     |
 * Add job VM dma-resv bookkeeing slot (non-compute mode)                 |
 * Add job to external BOs dma-resv write slots (non-compute mode)        |
 * Check if any userptrs invalidated since pin ------ Drop locks ---------|
 * Install in / out fences for job
 * Submit job
 * Unlock
 */

static int xe_exec_begin(struct xe_engine *e, struct ww_acquire_ctx *ww,
			 struct ttm_validate_buffer *tv_vm,
			 struct list_head *objs)
{
	struct xe_vm *vm = e->vm;
	int err;
	int i;

	lockdep_assert_held(&vm->lock);

	if (!xe_vm_in_compute_mode(e->vm)) {
		struct xe_vma *vma;
		LIST_HEAD(dups);

		INIT_LIST_HEAD(objs);
		for (i = 0; i < vm->extobj.entries; ++i) {
			struct xe_bo *bo = vm->extobj.bos[i];

			XE_BUG_ON(bo->extobj_tv.num_shared != 1);
			XE_BUG_ON(&bo->ttm != bo->extobj_tv.bo);

			list_add_tail(&bo->extobj_tv.head, objs);
		}
		tv_vm->num_shared = 1;
		tv_vm->bo = xe_vm_ttm_bo(vm);;
		list_add_tail(&tv_vm->head, objs);
		err = ttm_eu_reserve_buffers(ww, objs, true, &dups);
		if (err)
			return err;

		/*
		 * Validate and BOs that have been evicted (i.e. make sure the
		 * BOs have valid placements possibly moving an evicted BO back
		 * to a location where the GPU can access it).
		 *
		 * This list can grow during the loop as xe_bo_validate can
		 * trigger an eviction within this VM. This is safe as newly
		 * evicted VMAs are added at the end of the list and the loop
		 * checks for newly added entries each iteration.
		 */
		list_for_each_entry(vma, &vm->evict_list, evict_link) {
			err = xe_bo_validate(vma->bo, vm);
			if (err) {
				ttm_eu_backoff_reservation(ww, objs);
				return err;
			}
		}
	}

	return 0;
}

static void xe_exec_end(struct xe_engine *e, struct ww_acquire_ctx *ww,
			struct list_head *objs)
{
	if (!xe_vm_in_compute_mode(e->vm))
		ttm_eu_backoff_reservation(ww, objs);
}

int xe_exec_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct xe_device *xe = to_xe_device(dev);
	struct xe_file *xef = to_xe_file(file);
	struct drm_xe_exec *args = data;
	struct drm_xe_sync __user *syncs_user = u64_to_user_ptr(args->syncs);
	uint64_t __user *addresses_user = u64_to_user_ptr(args->address);
	struct xe_engine *engine;
	struct xe_sync_entry *syncs = NULL;
	uint64_t addresses[XE_HW_ENGINE_MAX_INSTANCE];
	uint32_t i, num_syncs = 0;
	struct xe_sched_job *job;
	struct dma_fence *rebind_fence;
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

	if (args->num_syncs) {
		syncs = kcalloc(args->num_syncs, sizeof(*syncs), GFP_KERNEL);
		if (!syncs) {
			err = -ENOMEM;
			goto err_engine;
		}
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

	/*
	 * We can't install a job into the VM dma-resv shared slot before an
	 * async VM bind passed in as a fence without the risk of deadlocking as
	 * the bind can trigger an eviction which in turn depends on anything in
	 * the VM dma-resv shared slots. Not an ideal solution, but we wait for
	 * all dependent async VM binds to start (install correct fences into
	 * dma-resv slots) before moving forward.
	 */
	if (!xe_vm_in_compute_mode(vm) &&
	    vm->flags & XE_VM_FLAG_ASYNC_BIND_OPS) {
		for (i = 0; i < args->num_syncs; i++) {
			struct dma_fence *fence = syncs[i].fence;
			if (fence) {
				err = xe_vm_async_fence_wait_start(fence);
				if (err)
					goto err_syncs;
			}
		}
	}

retry:
	err = down_read_interruptible(&vm->lock);
	if (err)
		goto err_syncs;

	err = xe_vm_userptr_pin(vm, false);
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

	/*
	 * Rebind any invalidated userptr or evicted BOs in the VM, non-compute
	 * VM mode only.
	 */
	rebind_fence = xe_vm_rebind(vm, false);
	if (IS_ERR(rebind_fence)) {
		err = PTR_ERR(rebind_fence);
		goto err_put_job;
	}

	/*
	 * We store the rebind_fence in the VM so subsequent execs don't get
	 * scheduled before the rebinds of userptrs / evicted BOs is complete.
	 */
	if (rebind_fence) {
		dma_fence_put(vm->rebind_fence);
		vm->rebind_fence = rebind_fence;
	}
	if (vm->rebind_fence) {
		if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT,
			     &vm->rebind_fence->flags)) {
			dma_fence_put(vm->rebind_fence);
			vm->rebind_fence = NULL;
		} else {
			dma_fence_get(vm->rebind_fence);
			err = drm_sched_job_add_dependency(&job->drm,
							   vm->rebind_fence);
			if (err)
				goto err_put_job;
		}
	}

	/*
	 * Point of no return, if we error after this point just set an error on
	 * the job and let the DRM scheduler / backend clean up the job.
	 */

	xe_sched_job_arm(job);

	if (!xe_vm_in_compute_mode(vm)) {
		/* Block userptr invalidations / BO eviction */
		dma_resv_add_fence(&vm->resv,
				   &job->drm.s_fence->finished,
				   DMA_RESV_USAGE_BOOKKEEP);

		/*
		 * Make implicit sync work across drivers, assuming all external
		 * BOs are written as we don't pass in a read / write list.
		 */
		for (i = 0; i < vm->extobj.entries; ++i) {
			struct xe_bo *bo = vm->extobj.bos[i];

			dma_resv_add_fence(bo->ttm.base.resv,
					   &job->drm.s_fence->finished,
					   DMA_RESV_USAGE_WRITE);
		}
	}

	err = xe_vm_userptr_needs_repin(vm, false);

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
