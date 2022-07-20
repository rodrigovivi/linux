// SPDX-License-Identifier: MIT
/*
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
#include "xe_migrate.h"
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
	INIT_LIST_HEAD(&e->compute.link);

	/* FIXME: Wire up to configurable default value */
	e->sched_props.timeslice_us = 1 * 1000;
	e->sched_props.preempt_timeout_us = 640 * 1000;

	if (xe_engine_is_parallel(e)) {
		e->parallel.composite_fence_ctx = dma_fence_context_alloc(1);
		e->parallel.composite_fence_seqno = 1;
	}
	if (e->flags & ENGINE_FLAG_VM) {
		e->bind.fence_ctx = dma_fence_context_alloc(1);
		e->bind.fence_seqno = 1;
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
	struct ww_acquire_ctx ww;
	struct xe_engine *e;
	int err;

	if (vm) {
		err = xe_vm_lock(vm, &ww, 0, true);
		if (err)
			return ERR_PTR(err);
	}
	e = __xe_engine_create(xe, vm, logical_mask, width, hwe, flags);
	if (vm)
		xe_vm_unlock(vm, &ww);

	return e;
}

struct xe_engine *xe_engine_create_class(struct xe_device *xe, struct xe_vm *vm,
					 enum xe_engine_class class, u32 flags)
{
	struct xe_hw_engine *hwe, *hwe0 = NULL;
	struct xe_gt *gt = to_gt(xe);
	enum xe_hw_engine_id id;
	u32 logical_mask = 0;

	for_each_hw_engine(hwe, gt, id) {
		if (hwe->class == class) {
			logical_mask |= BIT(hwe->logical_instance);
			if (!hwe0)
				hwe0 = hwe;
		}
	}

	if (!logical_mask)
		return ERR_PTR(-ENODEV);

	return xe_engine_create(xe, vm, logical_mask, 1, hwe0, flags);
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

static int engine_set_compute_mode(struct xe_device *xe, struct xe_engine *e,
				   u64 value, bool create)
{
	if (XE_IOCTL_ERR(xe, !create))
		return -EINVAL;

	if (XE_IOCTL_ERR(xe, e->flags & ENGINE_FLAG_COMPUTE_MODE))
		return -EINVAL;

	if (XE_IOCTL_ERR(xe, e->flags & ENGINE_FLAG_VM))
		return -EINVAL;

	if (value) {
		struct xe_vm *vm = e->vm;
		int err;

		/*
		 * XXX: Installing new preempt fences are tricky if things are
		 * inflight as all the preempt fences should be suspending /
		 * resuming in unison, let's try not to support this. Adding a
		 * warning for now until we get a UMD by in.
		 */
		XE_WARN_ON(!RB_EMPTY_ROOT(&vm->vmas));

		if (XE_IOCTL_ERR(xe, !(e->vm->flags & XE_VM_FLAG_COMPUTE_MODE)))
			return -ENOTSUPP;

		if (XE_IOCTL_ERR(xe, e->width != 1))
			return -EINVAL;

		if (XE_IOCTL_ERR(xe, !is_power_of_2(e->logical_mask)))
			return -EINVAL;

		e->compute.context = dma_fence_context_alloc(1);
		spin_lock_init(&e->compute.lock);

		err = xe_vm_add_compute_engine(vm, e);
		if (XE_IOCTL_ERR(xe, err))
			return err;

		e->flags |= ENGINE_FLAG_COMPUTE_MODE;
		e->flags &= ~ENGINE_FLAG_PERSISTENT;
	}

	return 0;
}

static int engine_set_persistence(struct xe_device *xe, struct xe_engine *e,
				  u64 value, bool create)
{
	if (XE_IOCTL_ERR(xe, !create))
		return -EINVAL;

	if (XE_IOCTL_ERR(xe, e->flags & ENGINE_FLAG_COMPUTE_MODE))
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
	[XE_ENGINE_PROPERTY_COMPUTE_MODE] = engine_set_compute_mode,
	[XE_ENGINE_PROPERTY_PERSISTENCE] = engine_set_persistence,
	[XE_ENGINE_PROPERTY_JOB_TIMEOUT] = engine_set_job_timeout,
};

static int engine_user_ext_set_property(struct xe_device *xe,
					struct xe_engine *e,
					u64 extension,
					bool create)
{
	u64 __user *address = u64_to_user_ptr(extension);
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
	u64 __user *address = u64_to_user_ptr(extensions);
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

static u32 bind_engine_logical_mask(struct xe_device *xe,
				    struct drm_xe_engine_class_instance *eci,
				    u16 width, u16 num_placements)
{
	struct xe_hw_engine *hwe;
	struct xe_gt *gt = to_gt(xe);
	enum xe_hw_engine_id id;
	u32 logical_mask = 0;

	if (XE_IOCTL_ERR(xe, width != 1))
		return 0;
	if (XE_IOCTL_ERR(xe, num_placements != 1))
		return 0;
	if (XE_IOCTL_ERR(xe, eci[0].engine_instance != 0))
		return 0;

	eci[0].engine_class = DRM_XE_ENGINE_CLASS_COPY;

	for_each_hw_engine(hwe, gt, id)
		if (hwe->class ==
		    user_to_xe_engine_class[DRM_XE_ENGINE_CLASS_COPY])
			logical_mask |= BIT(hwe->logical_instance);

	return logical_mask;
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
	struct xe_vm *vm, *migrate_vm = NULL;
	struct xe_engine *e;
	u32 logical_mask;
	u32 id;
	int len;
	int err;
	bool bind_engine;

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

	bind_engine = eci[0].engine_class == DRM_XE_ENGINE_CLASS_VM_BIND;
	if (bind_engine)
		logical_mask = bind_engine_logical_mask(xe, eci, args->width,
							args->num_placements);
	else
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

	if (bind_engine)
		migrate_vm = xe_migrate_get_vm(to_gt(xe)->migrate);

	e = xe_engine_create(xe, migrate_vm ?: vm, logical_mask,
			     args->width, hwe, ENGINE_FLAG_PERSISTENT |
			     (bind_engine ? ENGINE_FLAG_VM : 0));
	if (migrate_vm)
		xe_vm_put(migrate_vm);
	xe_vm_put(vm);
	if (IS_ERR(e))
		return PTR_ERR(e);

	if (args->extensions) {
		err = engine_user_extensions(xe, e, args->extensions, 0, true);
		if (XE_IOCTL_ERR(xe, err))
			goto put_engine;
	}

	if (XE_IOCTL_ERR(xe, e->vm && !!(e->vm->flags & XE_VM_FLAG_COMPUTE_MODE) !=
			 !!(e->flags & ENGINE_FLAG_COMPUTE_MODE))) {
		err = -ENOTSUPP;
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

	if (!(e->flags & ENGINE_FLAG_COMPUTE_MODE))
	      return;

	down_write(&e->vm->lock);
	list_del(&e->compute.link);
	--e->vm->preempt.num_engines;
	if (e->compute.pfence)
		dma_fence_enable_sw_signaling(e->compute.pfence);
	up_write(&e->vm->lock);
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
