// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 *
 * Standalone backport of the xe KMD drm-ras consumer (drivers/gpu/drm/xe/
 * xe_drm_ras.c) to linux-5.15.y.
 *
 * The real xe driver does not exist on 5.15, so the struct xe_device and
 * system-controller hardware read/clear paths are replaced by software
 * counters. Everything that defines the drm-ras contract is reproduced
 * faithfully from xe_drm_ras.c: one node per error severity
 * (correctable-errors / uncorrectable-errors), one counter per error component
 * (core-compute / soc-internal), and the same DRM_XE_RAS_* id/name mapping.
 * It links only against the API exported by drm_ras.ko.
 */

#include <linux/atomic.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "drm_ras.h"
#include "xe_ras_uapi.h"

#define XE_RAS_NAME "xe_ras"

#define for_each_error_severity(i)	\
	for ((i) = 0; (i) < DRM_XE_RAS_ERR_SEV_MAX; (i)++)

static const char * const error_components[] = DRM_XE_RAS_ERROR_COMPONENT_NAMES;
static const char * const error_severity[] = DRM_XE_RAS_ERROR_SEVERITY_NAMES;

/* Software stand-in for the xe hardware counters (mirrors xe_drm_ras_counter). */
struct xe_ras_counter {
	const char *name;
	atomic_t counter;
};

/* Per-severity node plus its component counters. */
struct xe_ras_node {
	struct drm_ras_node node;
	struct xe_ras_counter info[DRM_XE_RAS_ERR_COMP_MAX];
};

struct xe_ras_device {
	struct platform_device *pdev;
	struct xe_ras_node nodes[DRM_XE_RAS_ERR_SEV_MAX];
};

static struct xe_ras_device *xe_ras;

static int query_error_counter(struct xe_ras_node *xn, u32 error_id,
			       const char **name, u32 *val)
{
	if (error_id >= DRM_XE_RAS_ERR_COMP_MAX || !xn->info[error_id].name)
		return -ENOENT;

	*name = xn->info[error_id].name;
	*val = atomic_read(&xn->info[error_id].counter);

	return 0;
}

static int clear_error_counter(struct xe_ras_node *xn, u32 error_id)
{
	if (error_id >= DRM_XE_RAS_ERR_COMP_MAX || !xn->info[error_id].name)
		return -ENOENT;

	atomic_set(&xn->info[error_id].counter, 0);

	return 0;
}

static int xe_query_error_counter(struct drm_ras_node *node, u32 error_id,
				  const char **name, u32 *val)
{
	return query_error_counter(node->priv, error_id, name, val);
}

static int xe_clear_error_counter(struct drm_ras_node *node, u32 error_id)
{
	return clear_error_counter(node->priv, error_id);
}

static void init_counters(struct xe_ras_node *xn)
{
	int i;

	for (i = DRM_XE_RAS_ERR_COMP_CORE_COMPUTE; i < DRM_XE_RAS_ERR_COMP_MAX;
	     i++) {
		if (!error_components[i])
			continue;

		xn->info[i].name = error_components[i];
		atomic_set(&xn->info[i].counter, 0);
	}
}

/*
 * Mirror of xe_drm_ras_event(): bump the software counter for a component and
 * multicast the event. Real xe fetches @value from the system controller.
 */
static void xe_ras_event(u32 severity, u32 component)
{
	struct xe_ras_node *xn;
	u32 value;
	int ret;

	if (severity >= DRM_XE_RAS_ERR_SEV_MAX ||
	    component >= DRM_XE_RAS_ERR_COMP_MAX)
		return;

	xn = &xe_ras->nodes[severity];
	if (!xn->info[component].name)
		return;

	value = atomic_inc_return(&xn->info[component].counter);

	ret = drm_ras_nl_error_event(&xn->node, component,
				     xn->info[component].name, value);
	if (ret)
		pr_warn(XE_RAS_NAME ": error-event failed: %d for %s %s\n", ret,
			error_components[component], error_severity[severity]);
}

static int xe_ras_register(void)
{
	struct device *dev = &xe_ras->pdev->dev;
	int i, ret;

	for_each_error_severity(i) {
		struct xe_ras_node *xn = &xe_ras->nodes[i];
		struct drm_ras_node *node = &xn->node;

		init_counters(xn);

		node->device_name = dev_name(dev);
		node->node_name = error_severity[i];
		node->type = DRM_RAS_NODE_TYPE_ERROR_COUNTER;
		node->error_counter_range.first = DRM_XE_RAS_ERR_COMP_CORE_COMPUTE;
		node->error_counter_range.last = DRM_XE_RAS_ERR_COMP_MAX - 1;
		node->query_error_counter = xe_query_error_counter;
		node->clear_error_counter = xe_clear_error_counter;
		node->priv = xn;

		ret = drm_ras_node_register(node);
		if (ret) {
			pr_err(XE_RAS_NAME ": failed to register %s node: %d\n",
			       error_severity[i], ret);
			goto err_unwind;
		}
	}

	return 0;

err_unwind:
	while (--i >= 0)
		drm_ras_node_unregister(&xe_ras->nodes[i].node);
	return ret;
}

static int __init xe_ras_init(void)
{
	int ret;

	xe_ras = kzalloc(sizeof(*xe_ras), GFP_KERNEL);
	if (!xe_ras)
		return -ENOMEM;

	xe_ras->pdev = platform_device_register_simple(XE_RAS_NAME, -1, NULL, 0);
	if (IS_ERR(xe_ras->pdev)) {
		ret = PTR_ERR(xe_ras->pdev);
		goto err_free;
	}

	ret = xe_ras_register();
	if (ret)
		goto err_pdev;

	/* Emit one event per severity so subscribers see traffic on load. */
	xe_ras_event(DRM_XE_RAS_ERR_SEV_CORRECTABLE,
		     DRM_XE_RAS_ERR_COMP_CORE_COMPUTE);
	xe_ras_event(DRM_XE_RAS_ERR_SEV_UNCORRECTABLE,
		     DRM_XE_RAS_ERR_COMP_SOC_INTERNAL);

	return 0;

err_pdev:
	platform_device_unregister(xe_ras->pdev);
err_free:
	kfree(xe_ras);
	xe_ras = NULL;
	return ret;
}
module_init(xe_ras_init);

static void __exit xe_ras_exit(void)
{
	int i;

	for_each_error_severity(i)
		drm_ras_node_unregister(&xe_ras->nodes[i].node);

	platform_device_unregister(xe_ras->pdev);
	kfree(xe_ras);
	xe_ras = NULL;
}
module_exit(xe_ras_exit);

MODULE_DESCRIPTION("xe drm-ras consumer (standalone 5.15 backport)");
MODULE_AUTHOR("Rodrigo Vivi");
MODULE_LICENSE("Dual MIT/GPL");
