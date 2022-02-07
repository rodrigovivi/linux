// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <drm/drm_debugfs.h>

#include "xe_device.h"
#include "xe_debugfs.h"
#include "xe_gt_debugfs.h"

static struct xe_device *node_to_xe(struct drm_info_node *node)
{
	return to_xe_device(node->minor->dev);
}

static int info(struct seq_file *m, void *data)
{
	struct xe_device *xe = node_to_xe(m->private);
	struct drm_printer p = drm_seq_file_printer(m);

	drm_printf(&p, "graphics_verx10 %d\n", xe->info.graphics_verx10);
	drm_printf(&p, "is_dgfx %s\n", xe->info.is_dgfx ? "yes" : "no");
	drm_printf(&p, "platform %d\n", xe->info.platform);
	drm_printf(&p, "devid 0x%x\n", xe->info.devid);
	drm_printf(&p, "revid %d\n", xe->info.revid);

	return 0;
}

static const struct drm_info_list debugfs_list[] = {
	{"info", info, 0},
};

void xe_debugfs_register(struct xe_device *xe)
{
	struct drm_minor *minor = xe->drm.primary;

	drm_debugfs_create_files(debugfs_list,
				 ARRAY_SIZE(debugfs_list),
				 minor->debugfs_root, minor);

	xe_gt_debugfs_register(to_gt(xe));
}
