/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#include "xe_engine.h"

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/xe_drm.h>

#include "xe_device.h"
#include "xe_gt.h"
#include "xe_lrc.h"
#include "xe_macros.h"
#include "xe_preempt_fence.h"
#include "xe_trace.h"
#include "xe_vm.h"

static struct xe_engine *__xe_engine_create(struct xe_device *xe,
					    struct xe_vm *vm,
					    u32 logical_mask,
					    u16 width, struct xe_hw_engine *hwe,
					    u32 flags)
{
	struct xe_engine *e;
	struct xe_gt *gt = to_gt(xe);
	int err;
	int i;

	e = kzalloc(sizeof(*e) + sizeof(struct xe_lrc) * width, GFP_KERNEL);
	if (!e)
		return ERR_PTR(-ENOMEM);

	kref_init(&e->refcount);
	e->flags = flags;
	e->hwe = hwe;
	e->gt = gt;
	if (vm)
		e->vm = xe_vm_get(vm);
	e->class = hwe->class;
	e->width = width;
	e->logical_mask = logical_mask;
	e->fence_irq = &gt->fence_irq[hwe->class];
	e->ring_ops = gt->ring_ops[hwe->class];
	e->ops = gt->engine_ops;
	INIT_LIST_HEAD(&e->persitent.link);

	/* FIXME: Wire up to configurable default value */
	e->sched_props.timeslice_us = 1 * 1000;
	e->sched_props.preempt_timeout_us = 640 * 1000;

	if (xe_engine_is_parallel(e)) {
		e->parallel.composite_fence_ctx = dma_fence_context_alloc(1);
		e->parallel.composite_fence_seqno = 1;
	}

	for (i = 0; i < width; ++i) {
		err = xe_lrc_init(e->lrc + i, hwe, vm, SZ_16K);
		if (err)
			goto err_lrc;
	}

	err = e->ops->init(e);
	if (err)
		goto err_lrc;

	return e;

err_lrc:
	for (i = i - 1; i >= 0; --i)
		xe_lrc_finish(e->lrc + i);
	kfree(e);
	return ERR_PTR(err);
}

struct xe_engine *xe_engine_create(struct xe_device *xe, struct xe_vm *vm,
				   u32 logical_mask, u16 width,
				   struct xe_hw_engine *hwe, u32 flags)
{
	struct xe_engine *e;

	if (vm)
		xe_vm_lock(vm, NULL);
	e = __xe_engine_create(xe, vm, logical_mask, width, hwe, flags);
	if (vm)
		xe_vm_unlock(vm);

	return e;
}

void xe_engine_destroy(struct kref *ref)
{
	struct xe_engine *e = container_of(ref, struct xe_engine, refcount);

	e->ops->fini(e);
}

void xe_engine_fini(struct xe_engine *e)
{
	int i;

	for (i = 0; i < e->width; ++i)
		xe_lrc_finish(e->lrc + i);
	if (e->vm)
		xe_vm_put(e->vm);

	kfree(e);
}

struct xe_engine *xe_engine_lookup(struct xe_file *xef, u32 id)
{
	struct xe_engine *e;

	mutex_lock(&xef->engine.lock);
	e = xa_load(&xef->engine.xa, id);
	mutex_unlock(&xef->engine.lock);

	if (e)
		xe_engine_get(e);

	return e;
}

static int engine_set_priority(struct xe_device *xe, struct xe_engine *e,
			       u64 value, bool create)
{
	if (XE_IOCTL_ERR(xe, value > DRM_SCHED_PRIORITY_HIGH))
		return -EINVAL;

	if (XE_IOCTL_ERR(xe, value == DRM_SCHED_PRIORITY_HIGH &&
			 !capable(CAP_SYS_NICE)))
		return -EPERM;

	return e->ops->set_priority(e, value);
}

static int engine_set_timeslice(struct xe_device *xe, struct xe_engine *e,
				u64 value, bool create)
{
	if (!capable(CAP_SYS_NICE))
		return -EPERM;

	return e->ops->set_timeslice(e, value);
}

static int engine_set_preemption_timeout(struct xe_device *xe,
					 struct xe_engine *e, u64 value,
					 bool create)
{
	if (!capable(CAP_SYS_NICE))
		return -EPERM;

	return e->ops->set_preempt_timeout(e, value);
}

static const struct xe_preempt_fence_ops preempt_fence_ops;

static void preempt_complete(struct xe_engine *e)
{
	struct dma_fence *pfence;
	struct xe_vm *vm = e->vm;
	int err = 0;
	bool enable_signaling = false;

	pfence = xe_preempt_fence_create(e, &preempt_fence_ops,
					 e->compute.context,
					 ++e->compute.seqno);
	if (IS_ERR(pfence)) {
		XE_WARN_ON("Preempt fence allocation failed");
		return;
	}

	xe_vm_lock(vm, NULL);
	if (!vm->preempt.num_inflight_ops) {
		err = dma_resv_reserve_shared(&vm->resv, 1);
		XE_WARN_ON(err);
		if (!err) {
			e->ops->resume(e);
			dma_resv_add_shared_fence(&vm->resv, pfence);
		}
	} else {
		dma_fence_get(pfence);
		list_add_tail(&to_preempt_fence(pfence)->link,
			      &vm->preempt.pending_fences);
	}
	if (e->compute.pfence) {
		dma_fence_put(e->compute.pfence);
		e->compute.pfence = pfence;
	} else {
		enable_signaling = true;
	}
	if (enable_signaling || err)
		dma_fence_enable_sw_signaling(pfence);
	xe_vm_unlock(vm);
}

static const struct xe_preempt_fence_ops preempt_fence_ops = {
	.preempt_complete = preempt_complete,
};

static int engine_set_compute(struct xe_device *xe, struct xe_engine *e,
			      u64 value, bool create)
{
	if (XE_IOCTL_ERR(xe, !create))
		return -EINVAL;

	if (XE_IOCTL_ERR(xe, e->flags & ENGINE_FLAG_COMPUTE))
		return -EINVAL;

	if (value) {
		struct xe_vm *vm = e->vm;
		struct dma_fence *pfence;
		int err;

		if (XE_IOCTL_ERR(xe, e->width != 1))
			return -EINVAL;

		if (XE_IOCTL_ERR(xe, !is_power_of_2(e->logical_mask)))
			return -EINVAL;

		e->compute.context = dma_fence_context_alloc(1);
		spin_lock_init(&e->compute.lock);
		pfence = xe_preempt_fence_create(e, &preempt_fence_ops,
						 e->compute.context,
						 ++e->compute.seqno);
		if (XE_IOCTL_ERR(xe, IS_ERR(pfence)))
			return PTR_ERR(pfence);

		xe_vm_lock(vm, NULL);
		if (!vm->preempt.enabled) {
			vm->preempt.enabled = true;
			INIT_LIST_HEAD(&vm->preempt.pending_fences);
		}
		err = dma_resv_reserve_shared(&vm->resv, 1);
		if (XE_IOCTL_ERR(xe, err)) {
			dma_fence_put(pfence);
			xe_vm_unlock(vm);
			return err;
		}

		e->compute.pfence = pfence;
		dma_resv_add_shared_fence(&vm->resv, pfence);
		xe_vm_unlock(vm);

		e->flags |= ENGINE_FLAG_COMPUTE;
		e->flags &= ~ENGINE_FLAG_PERSISTENT;
	}

	return 0;
}

static int engine_set_persistence(struct xe_device *xe, struct xe_engine *e,
				  u64 value, bool create)
{
	if (XE_IOCTL_ERR(xe, !create))
		return -EINVAL;

	if (XE_IOCTL_ERR(xe, e->flags & ENGINE_FLAG_COMPUTE))
		return -EINVAL;

	if (value)
		e->flags |= ENGINE_FLAG_PERSISTENT;
	else
		e->flags &= ~ENGINE_FLAG_PERSISTENT;

	return 0;
}

static int engine_set_job_timeout(struct xe_device *xe, struct xe_engine *e,
				  u64 value, bool create)
{
	if (XE_IOCTL_ERR(xe, !create))
		return -EINVAL;

	if (!capable(CAP_SYS_NICE))
		return -EPERM;

	return e->ops->set_job_timeout(e, value);
}

typedef int (*xe_engine_set_property_fn)(struct xe_device *xe,
					 struct xe_engine *e,
					 u64 value, bool create);

static const xe_engine_set_property_fn engine_set_property_funcs[] = {
	[XE_ENGINE_PROPERTY_PRIORITY] = engine_set_priority,
	[XE_ENGINE_PROPERTY_TIMESLICE] = engine_set_timeslice,
	[XE_ENGINE_PROPERTY_PREEMPTION_TIMEOUT] = engine_set_preemption_timeout,
	[XE_ENGINE_PROPERTY_COMPUTE] = engine_set_compute,
	[XE_ENGINE_PROPERTY_PERSISTENCE] = engine_set_persistence,
	[XE_ENGINE_PROPERTY_JOB_TIMEOUT] = engine_set_job_timeout,
};

static int engine_user_ext_set_property(struct xe_device *xe,
					struct xe_engine *e,
					u64 extension,
					bool create)
{
	uint64_t __user *address = u64_to_user_ptr(extension);
	struct drm_xe_ext_engine_set_property ext;
	int err;

	err = __copy_from_user(&ext, address, sizeof(ext));
	if (XE_IOCTL_ERR(xe, err))
		return -EFAULT;

	if (XE_IOCTL_ERR(xe, ext.property >=
			 ARRAY_SIZE(engine_set_property_funcs)))
		return -EINVAL;

	return engine_set_property_funcs[ext.property](xe, e, ext.value,
						       create);
}

typedef int (*xe_engine_user_extension_fn)(struct xe_device *xe,
					   struct xe_engine *e,
					   u64 extension,
					   bool create);

static const xe_engine_set_property_fn engine_user_extension_funcs[] = {
	[XE_ENGINE_EXTENSION_SET_PROPERTY] = engine_user_ext_set_property,
};

#define MAX_USER_EXTENSIONS	16
static int engine_user_extensions(struct xe_device *xe, struct xe_engine *e,
				  u64 extensions, int ext_number, bool create)
{
	uint64_t __user *address = u64_to_user_ptr(extensions);
	struct xe_user_extension ext;
	int err;

	if (XE_IOCTL_ERR(xe, ext_number >= MAX_USER_EXTENSIONS))
		return -E2BIG;

	err = __copy_from_user(&ext, address, sizeof(ext));
	if (XE_IOCTL_ERR(xe, err))
		return -EFAULT;

	if (XE_IOCTL_ERR(xe, ext.name >=
			 ARRAY_SIZE(engine_user_extension_funcs)))
		return -EINVAL;

	err = engine_user_extension_funcs[ext.name](xe, e, extensions, create);
	if (XE_IOCTL_ERR(xe, err))
		return err;

	if (ext.next_extension)
		return engine_user_extensions(xe, e, ext.next_extension,
					      ++ext_number, create);

	return 0;
}

static const enum xe_engine_class user_to_xe_engine_class[] = {
	[DRM_XE_ENGINE_CLASS_RENDER] = XE_ENGINE_CLASS_RENDER,
	[DRM_XE_ENGINE_CLASS_COPY] = XE_ENGINE_CLASS_COPY,
	[DRM_XE_ENGINE_CLASS_VIDEO_DECODE] = XE_ENGINE_CLASS_VIDEO_DECODE,
	[DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE] = XE_ENGINE_CLASS_VIDEO_ENHANCE,
	[DRM_XE_ENGINE_CLASS_COMPUTE] = XE_ENGINE_CLASS_COMPUTE,
};

static struct xe_hw_engine *
find_hw_engine(struct xe_device *xe,
	       struct drm_xe_engine_class_instance eci)
{
	if (eci.engine_class > ARRAY_SIZE(user_to_xe_engine_class))
		return NULL;

	if (eci.gt_id != 0)
		return NULL;

	return xe_gt_hw_engine(to_gt(xe),
			       user_to_xe_engine_class[eci.engine_class],
			       eci.engine_instance, true);
}

static u32 calc_validate_logical_mask(struct xe_device *xe,
				      struct drm_xe_engine_class_instance *eci,
				      u16 width, u16 num_placements)
{
	int len = width * num_placements;
	int i, j, n;
	u16 class;
	u32 return_mask = 0, prev_mask;

	if (XE_IOCTL_ERR(xe, !xe_gt_guc_submission_enabled(to_gt(xe)) &&
			 len > 1))
		return 0;

	for (i = 0; i < width; ++i) {
		u32 current_mask = 0;

		for (j = 0; j < num_placements; ++j) {
			n = i * num_placements + j;

			if (XE_IOCTL_ERR(xe, !find_hw_engine(xe, eci[n])))
				return 0;

			if (n && XE_IOCTL_ERR(xe, eci[n].engine_class != class))
				return 0;
			else
				class = eci[n].engine_class;

			if (width == 1 || !j)
				return_mask |= BIT(eci[n].engine_instance);
			current_mask |= BIT(eci[n].engine_instance);
		}

		/* Parallel submissions must be logically contiguous */
		if (i && XE_IOCTL_ERR(xe, current_mask != prev_mask << 1))
			return 0;

		prev_mask = current_mask;
	}

	return return_mask;
}

int xe_engine_create_ioctl(struct drm_device *dev, void *data,
			   struct drm_file *file)
{
	struct xe_device *xe = to_xe_device(dev);
	struct xe_file *xef = to_xe_file(file);
	struct drm_xe_engine_create *args = data;
	struct drm_xe_engine_class_instance eci[XE_HW_ENGINE_MAX_INSTANCE];
	struct drm_xe_engine_class_instance __user *user_eci =
		u64_to_user_ptr(args->instances);
	struct xe_hw_engine *hwe;
	struct xe_vm *vm;
	struct xe_engine *e;
	u32 logical_mask;
	u32 id;
	int len;
	int err;

	if (XE_IOCTL_ERR(xe, args->flags))
		return -EINVAL;

	len = args->width * args->num_placements;
	if (XE_IOCTL_ERR(xe, !len || len > XE_HW_ENGINE_MAX_INSTANCE))
		return -EINVAL;

	err = __copy_from_user(eci, user_eci,
			       sizeof(struct drm_xe_engine_class_instance) *
			       len);
	if (XE_IOCTL_ERR(xe, err))
		return -EFAULT;

	logical_mask = calc_validate_logical_mask(xe, eci, args->width,
						  args->num_placements);
	if (XE_IOCTL_ERR(xe, !logical_mask))
		return -EINVAL;

	hwe = find_hw_engine(xe, eci[0]);
	if (XE_IOCTL_ERR(xe, !hwe))
		return -EINVAL;

	vm = xe_vm_lookup(xef, args->vm_id);
	if (XE_IOCTL_ERR(xe, !vm))
		return -ENOENT;

	e = xe_engine_create(xe, vm, logical_mask, args->width, hwe,
			     ENGINE_FLAG_PERSISTENT);
	xe_vm_put(vm);
	if (IS_ERR(e))
		return PTR_ERR(e);

	if (args->extensions) {
		err = engine_user_extensions(xe, e, args->extensions, 0, true);
		if (XE_IOCTL_ERR(xe, err))
			goto put_engine;
	}

	e->persitent.xef = xef;

	mutex_lock(&xef->engine.lock);
	err = xa_alloc(&xef->engine.xa, &id, e, xa_limit_32b, GFP_KERNEL);
	mutex_unlock(&xef->engine.lock);
	if (err)
		goto put_engine;

	args->engine_id = id;

	return 0;

put_engine:
	xe_engine_kill(e);
	xe_engine_put(e);
	return err;
}

void xe_engine_kill(struct xe_engine *e)
{
	e->ops->kill(e);

	if (!(e->flags & ENGINE_FLAG_COMPUTE))
	      return;

	xe_vm_lock(e->vm, NULL);
	if (e->compute.pfence) {
		if (!list_empty(&to_preempt_fence(e->compute.pfence)->link)) {
			dma_fence_put(e->compute.pfence);
			list_del(&to_preempt_fence(e->compute.pfence)->link);
		}
		dma_fence_enable_sw_signaling(e->compute.pfence);
		e->compute.pfence = NULL;
	}
	xe_vm_unlock(e->vm);
}

int xe_engine_destroy_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file)
{
	struct xe_device *xe = to_xe_device(dev);
	struct xe_file *xef = to_xe_file(file);
	struct drm_xe_engine_destroy *args = data;
	struct xe_engine *e;

	if (XE_IOCTL_ERR(xe, args->pad))
		return -EINVAL;

	mutex_lock(&xef->engine.lock);
	e = xa_erase(&xef->engine.xa, args->engine_id);
	mutex_unlock(&xef->engine.lock);
	if (XE_IOCTL_ERR(xe, !e))
		return -ENOENT;

	if (!(e->flags & ENGINE_FLAG_PERSISTENT))
		xe_engine_kill(e);
	else
		xe_device_add_persitent_engines(xe, e);

	trace_xe_engine_close(e);
	xe_engine_put(e);

	return 0;
}

int xe_engine_set_property_ioctl(struct drm_device *dev, void *data,
				 struct drm_file *file)
{
	struct xe_device *xe = to_xe_device(dev);
	struct xe_file *xef = to_xe_file(file);
	struct drm_xe_engine_set_property *args = data;
	struct xe_engine *e;
	int ret;

	e = xe_engine_lookup(xef, args->engine_id);
	if (XE_IOCTL_ERR(xe, !e))
		return -ENOENT;

	if (XE_IOCTL_ERR(xe, args->property >=
			 ARRAY_SIZE(engine_set_property_funcs))) {
		ret = -EINVAL;
		goto out;
	}

	ret = engine_set_property_funcs[args->property](xe, e, args->value,
							false);
	if (XE_IOCTL_ERR(xe, ret))
		goto out;

	if (args->extensions)
		ret = engine_user_extensions(xe, e, args->extensions, 0,
					     false);
out:
	xe_engine_put(e);

	return ret;
}
