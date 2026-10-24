// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021-2022 Intel Corporation
 * Copyright (C) 2021-2022 Red Hat
 */

#include "xe_ttm_sys_mgr.h"

#include <linux/nodemask.h>
#include <linux/sort.h>

#include <drm/drm_managed.h>
#include <drm/drm_print.h>

#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_range_manager.h>
#include <drm/ttm/ttm_tt.h>

#include "xe_bo.h"

struct xe_ttm_sys_node {
	struct ttm_buffer_object *tbo;
	struct xe_sysmem_numa *sysmem_numa;
	struct ttm_range_mgr_node base;
};

static inline struct xe_ttm_sys_node *
to_xe_ttm_sys_node(struct ttm_resource *res)
{
	return container_of(res, struct xe_ttm_sys_node, base.base);
}

static int xe_ttm_sys_mgr_new(struct ttm_resource_manager *man,
			      struct ttm_buffer_object *tbo,
			      const struct ttm_place *place,
			      struct ttm_resource **res)
{
	struct xe_ttm_sys_node *node;
	int r;

	node = kzalloc_flex(*node, base.mm_nodes, 1);
	if (!node)
		return -ENOMEM;

	node->tbo = tbo;
	ttm_resource_init(tbo, place, &node->base.base);

	if (!(place->flags & TTM_PL_FLAG_TEMPORARY) &&
	    ttm_resource_manager_usage(man) > (man->size << PAGE_SHIFT)) {
		r = -ENOSPC;
		goto err_fini;
	}

	node->base.mm_nodes[0].start = 0;
	node->base.mm_nodes[0].size = PFN_UP(node->base.base.size);
	node->base.base.start = XE_BO_INVALID_OFFSET;

	if (xe_bo_is_xe_bo(tbo))
		node->sysmem_numa = ttm_to_xe_bo(tbo)->sysmem_numa;
	if (node->sysmem_numa)
		atomic64_add(node->base.base.size, &node->sysmem_numa->used);

	*res = &node->base.base;

	return 0;

err_fini:
	ttm_resource_fini(man, &node->base.base);
	kfree(node);
	return r;
}

static void xe_ttm_sys_mgr_del(struct ttm_resource_manager *man,
			       struct ttm_resource *res)
{
	struct xe_ttm_sys_node *node = to_xe_ttm_sys_node(res);

	if (node->sysmem_numa)
		atomic64_sub(res->size, &node->sysmem_numa->used);

	ttm_resource_fini(man, res);
	kfree(node);
}

static void xe_ttm_sys_mgr_debug(struct ttm_resource_manager *man,
				 struct drm_printer *printer)
{
	/*
	 * This function is called by debugfs entry and would require
	 * pm_runtime_{get,put} wrappers around any operation.
	 */
}

static const struct ttm_resource_manager_func xe_ttm_sys_mgr_func = {
	.alloc = xe_ttm_sys_mgr_new,
	.free = xe_ttm_sys_mgr_del,
	.debug = xe_ttm_sys_mgr_debug
};

static void xe_ttm_sys_mgr_fini(struct drm_device *drm, void *arg)
{
	struct xe_device *xe = (struct xe_device *)arg;
	struct ttm_resource_manager *man = &xe->mem.sys_mgr;
	int err;

	ttm_resource_manager_set_used(man, false);

	err = ttm_resource_manager_evict_all(&xe->ttm, man);
	if (err)
		return;

	ttm_resource_manager_cleanup(man);
	ttm_set_driver_manager(&xe->ttm, XE_PL_TT, NULL);
}

static void xe_sysmem_numa_fini(struct drm_device *drm, void *arg)
{
	struct xe_device *xe = arg;
	u32 i;

	/* Let the BOs pending destruction give their pages back first */
	flush_workqueue(xe->ttm.wq);

	for (i = 0; i < xe->mem.num_sysmem_numa; i++)
		ttm_pool_fini(&xe->mem.sysmem_numa[i].pool);
}

static int xe_sysmem_numa_cmp(const void *a, const void *b)
{
	const struct xe_sysmem_numa *x = a, *y = b;

	if (x->distance != y->distance)
		return x->distance < y->distance ? -1 : 1;

	return x->node - y->node;
}

/*
 * With multiple CPU NUMA nodes, expose the system memory of each node as a
 * SYSMEM_NUMA region, nearest to the device first. They are all managed by
 * the single TT manager, only the page pool used to populate the BOs differs.
 */
static int xe_sysmem_numa_init(struct xe_device *xe)
{
	/* Movable-only nodes, like most CXL memory, can't back TTM pages */
	u32 max = num_node_state(N_HIGH_MEMORY), count = 0, i;
	int dev_node = dev_to_node(xe->drm.dev);
	struct xe_sysmem_numa *regions;
	int nid;

	if (max < 2)
		return 0;

	regions = drmm_kcalloc(&xe->drm, max, sizeof(*regions), GFP_KERNEL);
	if (!regions)
		return -ENOMEM;

	for_each_node_state(nid, N_HIGH_MEMORY) {
		if (count == max)
			break;

		regions[count].node = nid;
		regions[count].size = (u64)node_present_pages(nid) << PAGE_SHIFT;
		if (dev_node != NUMA_NO_NODE)
			regions[count].distance = node_distance(dev_node, nid);
		count++;
	}

	sort(regions, count, sizeof(*regions), xe_sysmem_numa_cmp, NULL);

	if (count > XE_SYSMEM_NUMA_MAX) {
		drm_info(&xe->drm, "Exposing the nearest %u of %u NUMA nodes\n",
			 XE_SYSMEM_NUMA_MAX, count);
		count = XE_SYSMEM_NUMA_MAX;
	}

	for (i = 0; i < count; i++)
		ttm_pool_init(&regions[i].pool, xe->drm.dev, regions[i].node,
			      xe->ttm.pool.alloc_flags);

	xe->mem.sysmem_numa = regions;
	xe->mem.num_sysmem_numa = count;

	return drmm_add_action_or_reset(&xe->drm, xe_sysmem_numa_fini, xe);
}

int xe_ttm_sys_mgr_init(struct xe_device *xe)
{
	struct ttm_resource_manager *man = &xe->mem.sys_mgr;
	struct sysinfo si;
	u64 gtt_size;
	int err;

	err = xe_sysmem_numa_init(xe);
	if (err)
		return err;

	si_meminfo(&si);
	/* Potentially restrict amount of TT memory here. */
	gtt_size = (u64)si.totalram * si.mem_unit;

	man->use_tt = true;
	man->func = &xe_ttm_sys_mgr_func;
	ttm_resource_manager_init(man, &xe->ttm, gtt_size >> PAGE_SHIFT);
	ttm_set_driver_manager(&xe->ttm, XE_PL_TT, man);
	ttm_resource_manager_set_used(man, true);
	return drmm_add_action_or_reset(&xe->drm, xe_ttm_sys_mgr_fini, xe);
}
