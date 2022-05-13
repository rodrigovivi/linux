// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <drm/xe_drm.h>
#include <drm/ttm/ttm_placement.h>
#include <linux/nospec.h>

#include "xe_device.h"
#include "xe_gt.h"
#include "xe_macros.h"
#include "xe_query.h"
#include "xe_ggtt.h"

static const enum xe_engine_class xe_to_user_engine_class[] = {
	[XE_ENGINE_CLASS_RENDER] = DRM_XE_ENGINE_CLASS_RENDER,
	[XE_ENGINE_CLASS_COPY] = DRM_XE_ENGINE_CLASS_COPY,
	[XE_ENGINE_CLASS_VIDEO_DECODE] = DRM_XE_ENGINE_CLASS_VIDEO_DECODE,
	[XE_ENGINE_CLASS_VIDEO_ENHANCE] = DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE,
	[XE_ENGINE_CLASS_COMPUTE] = DRM_XE_ENGINE_CLASS_COMPUTE,
};

static size_t calc_hw_engine_info_size(struct xe_device *xe)
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
		hw_engine_info[i].engine_instance = hwe->logical_instance;
		hw_engine_info[i++].gt_id = to_gt(xe)->info.id;
	}

	if (copy_to_user(query_ptr, hw_engine_info, size)) {
		kfree(hw_engine_info);
		return -EFAULT;
	}
	kfree(hw_engine_info);

	return 0;
}

static size_t calc_memory_usage_size(struct xe_device *xe)
{
	u32 num_managers = 1;

	if (ttm_manager_type(&xe->ttm, TTM_PL_VRAM))
		num_managers++;

	return offsetof(struct drm_xe_query_mem_usage, regions[num_managers]);
}

static int query_memory_usage(struct xe_device *xe,
			      struct drm_xe_device_query *query)
{
	size_t size = calc_memory_usage_size(xe);
	struct drm_xe_query_mem_usage *usage;
	struct drm_xe_query_mem_usage __user *query_ptr =
		u64_to_user_ptr(query->data);
	struct ttm_resource_manager *man;
	int ret;

	if (query->size == 0) {
		query->size = size;
		return 0;
	} else if (XE_IOCTL_ERR(xe, query->size != size)) {
		return -EINVAL;
	}

	usage = kmalloc(size, GFP_KERNEL);
	if (XE_IOCTL_ERR(xe, !usage))
		return -ENOMEM;

	usage->pad = 0;

	man = ttm_manager_type(&xe->ttm, TTM_PL_TT);
	usage->regions[0].mem_class = XE_QUERY_MEM_REGION_CLASS_SYSMEM;
	usage->regions[0].instance = 0;
	usage->regions[0].pad = 0;
	usage->regions[0].total_size = man->size << PAGE_SHIFT;
	usage->regions[0].used = ttm_resource_manager_usage(man);

	man = ttm_manager_type(&xe->ttm, TTM_PL_VRAM);
	if (man) {
		usage->regions[1].mem_class = XE_QUERY_MEM_REGION_CLASS_LMEM;
		usage->regions[1].instance = 0;
		usage->regions[1].pad = 0;
		usage->regions[1].total_size = man->size << PAGE_SHIFT;
		usage->regions[1].used = ttm_resource_manager_usage(man);

		usage->num_regions = 2;
	} else {
		usage->num_regions = 1;
	}

	if (!copy_to_user(query_ptr, usage, size))
		ret = 0;
	else
		ret = -ENOSPC;

	kfree(usage);
	return ret;
}

static int query_config(struct xe_device *xe,
			 struct drm_xe_device_query *query)
{
	u32 num_params = XE_QUERY_CONFIG_NUM_PARAM;
	size_t size =
		sizeof(struct drm_xe_query_config) + num_params * sizeof(u64);
	struct drm_xe_query_config __user *query_ptr =
		u64_to_user_ptr(query->data);
	struct drm_xe_query_config *config;

	if (query->size == 0) {
		query->size = size;
		return 0;
	} else if (XE_IOCTL_ERR(xe, query->size != size)) {
		return -EINVAL;
	}

	config = kzalloc(size, GFP_KERNEL);
	if (XE_IOCTL_ERR(xe, !config))
		return -ENOMEM;

	config->num_params = num_params;
	config->info[XE_QUERY_CONFIG_REV_AND_DEVICE_ID] =
		xe->info.devid | (xe->info.revid << 16);
	if (xe->gt.mem.vram.size)
		config->info[XE_QUERY_CONFIG_FLAGS] = XE_QUERY_CONFIG_FLAGS_HAS_VRAM;
	if (xe->gt.info.enable_guc)
		config->info[XE_QUERY_CONFIG_FLAGS] |= XE_QUERY_CONFIG_FLAGS_USE_GUC;
	config->info[XE_QUERY_CONFIG_MIN_ALIGNEMENT] =
		xe->info.vram_flags & XE_VRAM_FLAGS_NEED64K ? SZ_64K : SZ_4K;
	config->info[XE_QUERY_CONFIG_GTT_SIZE] = xe->gt.mem.ggtt->size;
	config->info[XE_QUERY_CONFIG_TILE_COUNT] = xe->info.tile_count;

	if (copy_to_user(query_ptr, config, size)) {
		kfree(config);
		return -EFAULT;
	}
	kfree(config);

	return 0;
}

static int (* const xe_query_funcs[])(struct xe_device *xe,
				      struct drm_xe_device_query *query) = {
	query_engines,
	query_memory_usage,
	query_config,
};

int xe_query_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct xe_device *xe = to_xe_device(dev);
	struct drm_xe_device_query *query = data;
	u32 idx;

	if (XE_IOCTL_ERR(xe, query->extensions != 0))
		return -EINVAL;

	if (XE_IOCTL_ERR(xe, query->query > ARRAY_SIZE(xe_query_funcs)))
		return -EINVAL;

	idx = array_index_nospec(query->query, ARRAY_SIZE(xe_query_funcs));
	if (XE_IOCTL_ERR(xe, !xe_query_funcs[idx]))
		return -EINVAL;

	return xe_query_funcs[idx](xe, query);
}
