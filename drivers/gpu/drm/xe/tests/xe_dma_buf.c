// SPDX-License-Identifier: GPL-2.0 AND MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <kunit/test.h>

#include "xe_pci.h"

static void check_residency(struct kunit *test, struct xe_bo *exported,
			    struct xe_bo *imported, struct dma_buf *dmabuf,
			    u32 mem_type)
{
	static struct ttm_operation_ctx ctx = {.interruptible = true};
	struct dma_buf_test_params *params = test->priv;
	int ret;

	xe_bo_assert_held(exported);
	xe_bo_assert_held(imported);

	if (!xe_bo_is_mem_type(exported, mem_type)) {
		KUNIT_FAIL(test, "Exported bo was not in expected memory type.\n");
		return;
	}

	if (xe_bo_is_pinned(exported))
		return;

	/*
	 * Evict exporter. Note that the gem object dma_buf member isn't
	 * set from xe_gem_prime_export(), and it's needed for the move_notify()
	 * functionality, so hack that up here. Evicting the exported bo will
	 * evict also the imported bo through the move_notify() functionality if
	 * importer is on a different device. If they're on the same device,
	 * the exporter and the importer should be the same bo.
	 */
	swap(exported->ttm.base.dma_buf, dmabuf);
	ret = ttm_bo_evict(&exported->ttm, &ctx);
	swap(exported->ttm.base.dma_buf, dmabuf);
	if (ret) {
		if (ret != -EINTR && ret != -ERESTARTSYS)
			KUNIT_FAIL(test, "Evicting exporter failed with err=%d.\n",
				   ret);
		return;
	}

	/* Verify that also importer has been evicted to SYSTEM */
	if (!xe_bo_is_mem_type(imported, XE_PL_SYSTEM)) {
		KUNIT_FAIL(test, "Importer wasn't properly evicted.\n");
		return;
	}

	/* Re-validate the importer. This should move also exporter in. */
	ret = xe_bo_validate(imported, NULL);
	if (ret) {
		if (ret != -EINTR && ret != -ERESTARTSYS)
			KUNIT_FAIL(test, "Validating importer failed with err=%d.\n",
				   ret);
		return;
	}

	/*
	 * If on different devices, the exporter is kept in system  if
	 * possible, saving a migration step as the transfer is just
	 * likely as fast from system memory.
	 */
	if (params->force_different_devices &&
	    params->mem_mask & XE_BO_CREATE_SYSTEM_BIT)
		KUNIT_EXPECT_TRUE(test, xe_bo_is_mem_type(exported, XE_PL_TT));
	else
		KUNIT_EXPECT_TRUE(test, xe_bo_is_mem_type(exported, mem_type));

	if (params->force_different_devices)
		KUNIT_EXPECT_TRUE(test, xe_bo_is_mem_type(imported, XE_PL_TT));
	else
		KUNIT_EXPECT_TRUE(test, exported == imported);
}

static void xe_test_dmabuf_import_same_driver(struct xe_device *xe)
{
	struct kunit *test = xe_cur_kunit();
	struct dma_buf_test_params *params = test->priv;
	struct drm_gem_object *import;
	struct dma_buf *dmabuf;
	struct xe_bo *bo;

	/* No VRAM on this device? */
	if (!ttm_manager_type(&xe->ttm, XE_PL_VRAM0) &&
	    (params->mem_mask & XE_BO_CREATE_VRAM0_BIT))
		return;

	kunit_info(test, "running %s\n", __func__);
	bo = xe_bo_create(xe, NULL, NULL, PAGE_SIZE, ttm_bo_type_device,
			  XE_BO_CREATE_USER_BIT | params->mem_mask);
	if (IS_ERR(bo)) {
		KUNIT_FAIL(test, "xe_bo_create() failed with err=%ld\n",
			   PTR_ERR(bo));
		return;
	}

	dmabuf = xe_gem_prime_export(&bo->ttm.base, 0);
	if (IS_ERR(dmabuf)) {
		KUNIT_FAIL(test, "xe_gem_prime_export() failed with err=%ld\n",
			   PTR_ERR(dmabuf));
		goto out;
	}

	/*
	 * We expect an import of an VRAM-only object to fail with
	 * -EOPNOTSUPP because it can't be migrated to SMEM.
	 */
	import = xe_gem_prime_import(&xe->drm, dmabuf);
	if (!IS_ERR(import)) {
		struct xe_bo *import_bo = gem_to_xe_bo(import);

		if (params->attach_ops &&
		    !params->attach_ops->allow_peer2peer &&
		    params->force_different_devices &&
		    !(params->mem_mask & XE_BO_CREATE_SYSTEM_BIT)) {
			KUNIT_FAIL(test,
				   "xe_gem_prime_import() succeeded when it shouldn't have\n");
		} else {
			int err;

			xe_bo_lock_no_vm(import_bo, NULL);
			err = xe_bo_validate(import_bo, NULL);
			if (err && err != -EINTR && err != -ERESTARTSYS)
				KUNIT_FAIL(test,
					   "xe_bo_validate() failed with err=%d\n", err);

			else if ((!params->force_different_devices ||
				  (params->attach_ops &&
				   params->attach_ops->allow_peer2peer)) &&
				 (params->mem_mask & XE_BO_CREATE_VRAM0_BIT))
				check_residency(test, bo, import_bo, dmabuf,
						XE_PL_VRAM0);
			else
				check_residency(test, bo, import_bo, dmabuf,
						XE_PL_TT);

			xe_bo_unlock_no_vm(import_bo);
		}
		drm_gem_object_put(import);
	} else if (PTR_ERR(import) != -EOPNOTSUPP) {
		KUNIT_FAIL(test,
			   "xe_gem_prime_import failed with the wrong err=%ld\n",
			   PTR_ERR(import));
	} else if (params->attach_ops && params->attach_ops->allow_peer2peer) {
		KUNIT_FAIL(test, "dynamic p2p attachment failed with err=%ld\n",
			   PTR_ERR(import));
	}
	dma_buf_put(dmabuf);
out:
	drm_gem_object_put(&bo->ttm.base);
}

static const struct dma_buf_attach_ops nop2p_attach_ops = {
	.allow_peer2peer = false,
	.move_notify = xe_dma_buf_move_notify
};

/*
 * We test the implementation with bos of different residency and with
 * importers with different capabilities; some lacking p2p support and some
 * lacking dynamic capabilities (attach_ops == NULL). We also fake
 * different devices avoiding the import shortcut that just reuses the same
 * gem object.
 */
static const struct dma_buf_test_params test_params[] = {
	{.mem_mask = XE_BO_CREATE_VRAM0_BIT,
	 .attach_ops = &xe_dma_buf_attach_ops},
	{.mem_mask = XE_BO_CREATE_VRAM0_BIT,
	 .attach_ops = &xe_dma_buf_attach_ops,
	 .force_different_devices = true},

	{.mem_mask = XE_BO_CREATE_VRAM0_BIT,
	 .attach_ops = &nop2p_attach_ops},
	{.mem_mask = XE_BO_CREATE_VRAM0_BIT,
	 .attach_ops = &nop2p_attach_ops,
	 .force_different_devices = true},

	{.mem_mask = XE_BO_CREATE_VRAM0_BIT},
	{.mem_mask = XE_BO_CREATE_VRAM0_BIT,
	 .force_different_devices = true},

	{.mem_mask = XE_BO_CREATE_SYSTEM_BIT,
	 .attach_ops = &xe_dma_buf_attach_ops},
	{.mem_mask = XE_BO_CREATE_SYSTEM_BIT,
	 .attach_ops = &xe_dma_buf_attach_ops,
	 .force_different_devices = true},

	{.mem_mask = XE_BO_CREATE_SYSTEM_BIT,
	 .attach_ops = &nop2p_attach_ops},
	{.mem_mask = XE_BO_CREATE_SYSTEM_BIT,
	 .attach_ops = &nop2p_attach_ops,
	 .force_different_devices = true},

	{.mem_mask = XE_BO_CREATE_SYSTEM_BIT},
	{.mem_mask = XE_BO_CREATE_SYSTEM_BIT,
	 .force_different_devices = true},

	{.mem_mask = XE_BO_CREATE_SYSTEM_BIT | XE_BO_CREATE_VRAM0_BIT,
	 .attach_ops = &xe_dma_buf_attach_ops},
	{.mem_mask = XE_BO_CREATE_SYSTEM_BIT | XE_BO_CREATE_VRAM0_BIT,
	 .attach_ops = &xe_dma_buf_attach_ops,
	 .force_different_devices = true},

	{.mem_mask = XE_BO_CREATE_SYSTEM_BIT | XE_BO_CREATE_VRAM0_BIT,
	 .attach_ops = &nop2p_attach_ops},
	{.mem_mask = XE_BO_CREATE_SYSTEM_BIT | XE_BO_CREATE_VRAM0_BIT,
	 .attach_ops = &nop2p_attach_ops,
	 .force_different_devices = true},

	{.mem_mask = XE_BO_CREATE_SYSTEM_BIT | XE_BO_CREATE_VRAM0_BIT},
	{.mem_mask = XE_BO_CREATE_SYSTEM_BIT | XE_BO_CREATE_VRAM0_BIT,
	 .force_different_devices = true},

	{}
};

static int dma_buf_run_device(struct xe_device *xe)
{
	const struct dma_buf_test_params *params;
	struct kunit *test = xe_cur_kunit();

	for (params = test_params; params->mem_mask; ++params) {
		test->priv = (void __force *)params;
		xe_test_dmabuf_import_same_driver(xe);
	}

	/* A non-zero return would halt iteration over driver devices */
	return 0;
}

void xe_dma_buf_kunit(struct kunit *test)
{
	xe_call_for_each_device(dma_buf_run_device);
}
EXPORT_SYMBOL(xe_dma_buf_kunit);
