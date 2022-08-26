// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <drm/drm_debugfs.h>

#include "xe_bo.h"
#include "xe_device.h"
#include "xe_debugfs.h"
#include "xe_gt_debugfs.h"

#ifdef CONFIG_DRM_XE_DEBUG
#include "xe_bo_evict.h"
#include "xe_migrate.h"
#include "xe_vm.h"
#endif

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
	drm_printf(&p, "media_verx100 %d\n", xe->info.media_verx100);
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

#ifdef CONFIG_DRM_XE_DEBUG
static int evict_selftest(struct seq_file *m, void *data)
{
	struct xe_device *xe = node_to_xe(m->private);
	struct drm_printer p = drm_seq_file_printer(m);
	struct xe_bo *bo, *external;
	unsigned bo_flags = XE_BO_CREATE_USER_BIT |
		XE_BO_CREATE_VRAM0_BIT | XE_BO_INTERNAL_TEST;
	struct xe_vm *vm = xe_migrate_get_vm(xe->gt[0].migrate);
	struct ww_acquire_ctx ww;
	int err, i;

	if (!IS_DGFX(xe))
		return 0;

	for (i = 0; i < 2; ++i) {
		xe_vm_lock(vm, &ww, 0, false);
		bo = xe_bo_create(xe, NULL, vm, 0x10000, ttm_bo_type_device,
				  bo_flags);
		xe_vm_unlock(vm, &ww);
		if (IS_ERR(bo)) {
			drm_printf(&p, "bo create err=%ld\n", PTR_ERR(bo));
			break;
		}

		external = xe_bo_create(xe, NULL, NULL, 0x10000,
					ttm_bo_type_device, bo_flags);
		if (IS_ERR(external)) {
			drm_printf(&p, "external bo create err=%ld\n",
				   PTR_ERR(external));
			goto cleanup_bo;
		}

		xe_bo_lock(external, &ww, 0, false);
		err = xe_bo_pin_external(external);
		xe_bo_unlock(external, &ww);
		if (err) {
			drm_printf(&p, "external bo pin err=%d\n", err);
			goto cleanup_external;
		}

		err = xe_bo_evict_all(xe);
		if (err) {
			drm_printf(&p, "evict err=%d\n", err);
			goto cleanup_all;
		}

		err = xe_bo_restore_kernel(xe);
		if (err) {
			drm_printf(&p, "restore kernel err=%d\n", err);
			goto cleanup_all;
		}

		err = xe_bo_restore_user(xe);
		if (err) {
			drm_printf(&p, "restore user err=%d\n", err);
			goto cleanup_all;
		}

		if (!xe_bo_is_vram(external)) {
			drm_printf(&p, "external bo not is vram\n");
			err = -EPROTO;
			goto cleanup_all;
		}

		if (xe_bo_is_vram(bo)) {
			drm_printf(&p, "bo is vram\n");
			err = -EPROTO;
			goto cleanup_all;
		}

		if (i) {
			down_read(&vm->lock);
			xe_vm_lock(vm, &ww, 0, false);
			err = xe_bo_validate(bo, bo->vm);
			xe_vm_unlock(vm, &ww);
			up_read(&vm->lock);
			if (err) {
				drm_printf(&p, "bo valid err=%d\n", err);
				goto cleanup_all;
			}
			xe_bo_lock(external, &ww, 0, false);
			err = xe_bo_validate(external, NULL);
			xe_bo_unlock(external, &ww);
			if (err) {
				drm_printf(&p, "external bo valid err=%d\n",
					   err);
				goto cleanup_all;
			}
		}

		xe_bo_lock(external, &ww, 0, false);
		xe_bo_unpin_external(external);
		xe_bo_unlock(external, &ww);

		xe_bo_put(external);
		xe_bo_put(bo);
		continue;

cleanup_all:
		xe_bo_lock(external, &ww, 0, false);
		xe_bo_unpin_external(external);
		xe_bo_unlock(external, &ww);
cleanup_external:
		xe_bo_put(external);
cleanup_bo:
		xe_bo_put(bo);
		break;
	}

	xe_vm_put(vm);
	if (!err)
		drm_printf(&p, "evict selftest passed\n");

	return 0;
}
#endif

static const struct drm_info_list debugfs_list[] = {
	{"info", info, 0},
#ifdef CONFIG_DRM_XE_DEBUG
	{"evict_selftest", evict_selftest, 0},
#endif
};

void xe_debugfs_register(struct xe_device *xe)
{
	struct ttm_device *bdev = &xe->ttm;
	struct drm_minor *minor = xe->drm.primary;
	struct dentry *root = minor->debugfs_root;
	struct ttm_resource_manager *man;
	struct xe_gt *gt;
	u32 mem_type;
	u8 id;

	drm_debugfs_create_files(debugfs_list,
				 ARRAY_SIZE(debugfs_list),
				 root, minor);

	for (mem_type = XE_PL_VRAM0; mem_type <= XE_PL_VRAM1; ++mem_type) {
		man = ttm_manager_type(bdev, mem_type);

		if (man) {
			char name[16];

			sprintf(name, "vram%d_mm", mem_type - XE_PL_VRAM0);
			ttm_resource_manager_create_debugfs(man, root, name);
		}
	}

	man = ttm_manager_type(bdev, XE_PL_TT);
	ttm_resource_manager_create_debugfs(man, root, "gtt_mm");

	for_each_gt(gt, xe, id)
		xe_gt_debugfs_register(gt);
}
