// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include <net/genetlink.h>
#include <uapi/drm/drm_netlink.h>

#include "xe_device.h"

static int xe_genl_list_errors(struct drm_device *drm, struct sk_buff *msg, struct genl_info *info)
{
	return 0;
}

static int xe_genl_read_error(struct drm_device *drm, struct sk_buff *msg, struct genl_info *info)
{
	return 0;
}

/* driver callbacks to DRM netlink commands*/
const struct driver_genl_ops xe_genl_ops[] = {
	[DRM_RAS_CMD_QUERY] =		{ .doit = xe_genl_list_errors },
	[DRM_RAS_CMD_READ_ONE] =	{ .doit = xe_genl_read_error },
	[DRM_RAS_CMD_READ_ALL] =	{ .doit = xe_genl_list_errors, },
};
