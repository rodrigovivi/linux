/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 *
 * Kernel-facing DRM RAS API for the standalone linux-5.15.y backport.
 * Adapted from include/drm/drm_ras.h: the CONFIG_DRM_RAS stubs are dropped
 * (the core always lives in drm_ras.ko here) and the UAPI is pulled from the
 * bundled local copy instead of <uapi/drm/drm_ras.h>.
 */

#ifndef __DRM_RAS_H__
#define __DRM_RAS_H__

#include <linux/types.h>

#include "drm_ras_uapi.h"

/**
 * struct drm_ras_node - A DRM RAS Node
 */
struct drm_ras_node {
	/** @id: Unique identifier for the node. Dynamically assigned. */
	u32 id;
	/**
	 * @device_name: Human-readable name of the device. Given by the driver.
	 */
	const char *device_name;
	/** @node_name: Human-readable name of the node. Given by the driver. */
	const char *node_name;
	/** @type: Type of the node (enum drm_ras_node_type). */
	enum drm_ras_node_type type;

	/* Error-Counter Related Callback and Variables */

	/** @error_counter_range: Range of valid Error IDs for this node. */
	struct {
		/** @first: First valid Error ID. */
		u32 first;
		/** @last: Last valid Error ID. Mandatory entry. */
		u32 last;
	} error_counter_range;

	/**
	 * @query_error_counter:
	 *
	 * Mandatory callback used by drm-ras to query a specific error counter.
	 * Called with error_id from `error_counter_range.first` to
	 * `error_counter_range.last`.
	 *
	 * Returns: 0 on success,
	 *          -ENOENT when error_id is not supported (drm-ras silently
	 *                  skips this entry; used for non-contiguous ranges),
	 *          other negative values terminate the netlink query.
	 */
	int (*query_error_counter)(struct drm_ras_node *node, u32 error_id,
				   const char **name, u32 *val);

	/**
	 * @clear_error_counter:
	 *
	 * Optional callback used by drm-ras to clear a specific error counter.
	 *
	 * Returns: 0 on success, negative error code on failure.
	 */
	int (*clear_error_counter)(struct drm_ras_node *node, u32 error_id);

	/** @priv: Driver private data */
	void *priv;
};

int drm_ras_node_register(struct drm_ras_node *node);
void drm_ras_node_unregister(struct drm_ras_node *node);
int drm_ras_nl_error_event(struct drm_ras_node *node, u32 error_id,
			   const char *error_name, u32 value);

#endif
