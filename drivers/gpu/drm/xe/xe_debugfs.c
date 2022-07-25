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
	struct xe_gt *gt;
	u8 id;

	drm_printf(&p, "graphics_verx100 %d\n", xe->info.graphics_verx100);
	drm_printf(&p, "is_dgfx %s\n", xe->info.is_dgfx ? "yes" : "no");
	drm_printf(&p, "platform %d\n", xe->info.platform);
	drm_printf(&p, "devid 0x%x\n", xe->info.devid);
	drm_printf(&p, "revid %d\n", xe->info.revid);
	drm_printf(&p, "tile_count %d\n", xe->info.tile_count);
	drm_printf(&p, "vm_max_level %d\n", xe->info.vm_max_level);
	drm_printf(&p, "enable_guc %s\n", xe->info.enable_guc ? "yes" : "no");
	for_each_gt(gt, xe, id)
		drm_printf(&p, "gt%d force wake %d\n", id,
			   xe_force_wake_ref(gt_to_fw(gt), XE_FW_GT));

	return 0;
}

static const struct drm_info_list debugfs_list[] = {
	{"info", info, 0},
};

void xe_debugfs_register(struct xe_device *xe)
{
	struct drm_minor *minor = xe->drm.primary;
	struct xe_gt *gt;
	u8 id;

	drm_debugfs_create_files(debugfs_list,
				 ARRAY_SIZE(debugfs_list),
				 minor->debugfs_root, minor);

	for_each_gt(gt, xe, id)
		xe_gt_debugfs_register(gt);
}
