/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/*
 * DRM RAS Generic Netlink UAPI.
 *
 * Local, verbatim copy of the upstream include/uapi/drm/drm_ras.h so the
 * standalone backport modules can be built out-of-tree against linux-5.15.y
 * without touching the kernel's uapi headers. The family name, version,
 * commands, attributes and multicast group MUST stay byte-for-byte identical
 * to upstream so the same userspace tooling (e.g. tools/net/drm_ras/drm_ras.py
 * and rasdaemon) works unchanged.
 *
 * Upstream source of truth: Documentation/netlink/specs/drm_ras.yaml
 */

#ifndef _UAPI_LINUX_DRM_RAS_H
#define _UAPI_LINUX_DRM_RAS_H

#define DRM_RAS_FAMILY_NAME	"drm-ras"
#define DRM_RAS_FAMILY_VERSION	1

/*
 * Type of the node. Currently, only error-counter nodes are supported, which
 * expose reliability counters for a hardware/software component.
 */
enum drm_ras_node_type {
	DRM_RAS_NODE_TYPE_ERROR_COUNTER = 1,
};

enum {
	DRM_RAS_A_NODE_ATTRS_NODE_ID = 1,
	DRM_RAS_A_NODE_ATTRS_DEVICE_NAME,
	DRM_RAS_A_NODE_ATTRS_NODE_NAME,
	DRM_RAS_A_NODE_ATTRS_NODE_TYPE,

	__DRM_RAS_A_NODE_ATTRS_MAX,
	DRM_RAS_A_NODE_ATTRS_MAX = (__DRM_RAS_A_NODE_ATTRS_MAX - 1)
};

enum {
	DRM_RAS_A_ERROR_COUNTER_ATTRS_NODE_ID = 1,
	DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_ID,
	DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_NAME,
	DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_VALUE,

	__DRM_RAS_A_ERROR_COUNTER_ATTRS_MAX,
	DRM_RAS_A_ERROR_COUNTER_ATTRS_MAX = (__DRM_RAS_A_ERROR_COUNTER_ATTRS_MAX - 1)
};

enum {
	DRM_RAS_A_ERROR_EVENT_ATTRS_DEVICE_NAME = 1,
	DRM_RAS_A_ERROR_EVENT_ATTRS_NODE_ID,
	DRM_RAS_A_ERROR_EVENT_ATTRS_NODE_NAME,
	DRM_RAS_A_ERROR_EVENT_ATTRS_ERROR_ID,
	DRM_RAS_A_ERROR_EVENT_ATTRS_ERROR_NAME,
	DRM_RAS_A_ERROR_EVENT_ATTRS_ERROR_VALUE,

	__DRM_RAS_A_ERROR_EVENT_ATTRS_MAX,
	DRM_RAS_A_ERROR_EVENT_ATTRS_MAX = (__DRM_RAS_A_ERROR_EVENT_ATTRS_MAX - 1)
};

enum {
	DRM_RAS_CMD_LIST_NODES = 1,
	DRM_RAS_CMD_GET_ERROR_COUNTER,
	DRM_RAS_CMD_CLEAR_ERROR_COUNTER,
	DRM_RAS_CMD_ERROR_EVENT,

	__DRM_RAS_CMD_MAX,
	DRM_RAS_CMD_MAX = (__DRM_RAS_CMD_MAX - 1)
};

#define DRM_RAS_MCGRP_ERROR_REPORT	"error-report"

#endif /* _UAPI_LINUX_DRM_RAS_H */
