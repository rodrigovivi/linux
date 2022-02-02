// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_device.h"
#include "xe_guc.h"
#include "xe_guc_ads.h"
#include "xe_guc_log.h"
#include "xe_guc_reg.h"
#include "xe_uc_fw.h"
#include "xe_mmio.h"
#include "xe_force_wake.h"

static struct xe_device *
guc_to_xe(struct xe_guc *guc)
{
	return container_of(guc, struct xe_device, uc.guc);
}

int xe_guc_init(struct xe_guc *guc)
{
	struct xe_device *xe = guc_to_xe(guc);
	int ret;

	guc->fw.type = XE_UC_FW_TYPE_GUC;
	ret = xe_uc_fw_init(&guc->fw);
	if (ret)
		goto out;

	ret = xe_guc_log_init(&guc->log);
	if (ret)
		goto err_fw;

	ret = xe_guc_ads_init(&guc->ads);
	if (ret)
		goto err_log;

	xe_uc_fw_change_status(&guc->fw, XE_UC_FIRMWARE_LOADABLE);

	return 0;

err_log:
	xe_guc_log_fini(&guc->log);
err_fw:
	xe_uc_fw_fini(&guc->fw);
out:
	drm_err(&xe->drm, "GuC init failed with %d", ret);
	return ret;
}

int xe_guc_reset(struct xe_guc *guc)
{
	struct xe_device *xe = guc_to_xe(guc);
	u32 guc_status;
	int ret;
	bool cookie;

	cookie = dma_fence_begin_signalling();
	xe_force_wake_assert_held(&xe->fw, XE_FW_GT);

	xe_mmio_write32(xe, GEN6_GDRST.reg, GEN11_GRDOM_GUC);

	ret = xe_mmio_wait32(xe, GEN6_GDRST.reg, 0, GEN11_GRDOM_GUC, 5);
	if (ret) {
		drm_err(&xe->drm, "GuC reset timed out, GEN6_GDRST=0x%8x\n",
			xe_mmio_read32(xe, GEN6_GDRST.reg));
		goto err_out;
	}

	guc_status = xe_mmio_read32(xe, GUC_STATUS.reg);
	if (!(guc_status & GS_MIA_IN_RESET)) {
		drm_err(&xe->drm,
			"GuC status: 0x%x, MIA core expected to be in reset\n",
			guc_status);
		ret = -EIO;
		goto err_out;
	}

	dma_fence_end_signalling(cookie);
	return 0;

err_out:
	dma_fence_end_signalling(cookie);

	return ret;
}

void xe_guc_fini(struct xe_guc *guc)
{
	if (!xe_uc_fw_is_loadable(&guc->fw))
		return;

	xe_guc_ads_fini(&guc->ads);
	xe_guc_log_fini(&guc->log);
	xe_uc_fw_fini(&guc->fw);
}
