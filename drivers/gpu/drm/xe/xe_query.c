// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <drm/xe_drm.h>

#include "xe_device.h"
#include "xe_gt.h"
#include "xe_macros.h"
#include "xe_query.h"

static const enum xe_engine_class xe_to_user_engine_class[] = {
	[XE_ENGINE_CLASS_RENDER] = DRM_XE_ENGINE_CLASS_RENDER,
	[XE_ENGINE_CLASS_COPY] = DRM_XE_ENGINE_CLASS_COPY,
	[XE_ENGINE_CLASS_VIDEO_DECODE] = DRM_XE_ENGINE_CLASS_VIDEO_DECODE,
	[XE_ENGINE_CLASS_VIDEO_ENHANCE] = DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE,
	[XE_ENGINE_CLASS_COMPUTE] = DRM_XE_ENGINE_CLASS_COMPUTE,
};

size_t calc_hw_engine_info_size(struct xe_device *xe)
{
	struct xe_hw_engine *hwe;
	enum xe_hw_engine_id id;
	int i = 0;

	for_each_hw_engine(hwe, to_gt(xe), id)
		i++;

	return i * sizeof(struct drm_xe_engine_class_instance);
}

static int query_engines(struct xe_device *xe,
			 struct drm_xe_device_query *query)
{
	size_t size = calc_hw_engine_info_size(xe);
	struct drm_xe_engine_class_instance __user *query_ptr =
		u64_to_user_ptr(query->data);
	struct drm_xe_engine_class_instance *hw_engine_info;
	struct xe_hw_engine *hwe;
	enum xe_hw_engine_id id;
	int i = 0;

	if (query->size == 0) {
		query->size = size;
		return 0;
	} else if (XE_IOCTL_ERR(xe, query->size != size)) {
		return -EINVAL;
	}

	hw_engine_info = kmalloc(size, GFP_KERNEL);
	if (XE_IOCTL_ERR(xe, !hw_engine_info))
		return -ENOMEM;

	for_each_hw_engine(hwe, to_gt(xe), id) {
		hw_engine_info[i].engine_class =
			xe_to_user_engine_class[hwe->class];
		hw_engine_info[i].engine_instance = hwe->instance;
		hw_engine_info[i++].gt_id = to_gt(xe)->info.id;
	}

	if (copy_to_user(query_ptr, hw_engine_info, size)) {
		kfree(hw_engine_info);
		return -EFAULT;
	}
	kfree(hw_engine_info);

	return 0;
}

static int (* const xe_query_funcs[])(struct xe_device *xe,
				      struct drm_xe_device_query *query) = {
	query_engines,
};

int xe_query_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct xe_device *xe = to_xe_device(dev);
	struct drm_xe_device_query *query = data;

	if (XE_IOCTL_ERR(xe, query->extensions != 0))
		return -EINVAL;

	if (XE_IOCTL_ERR(xe, query->query > ARRAY_SIZE(xe_query_funcs)))
		return -EINVAL;

	return xe_query_funcs[query->query](xe, query);
}
