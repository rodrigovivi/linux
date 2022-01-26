// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_device.h"
#include "xe_guc.h"
#include "xe_guc_log.h"
#include "xe_uc_fw.h"

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

	xe_uc_fw_change_status(&guc->fw, XE_UC_FIRMWARE_LOADABLE);

	return 0;

err_fw:
	xe_uc_fw_fini(&guc->fw);
out:
	drm_err(&xe->drm, "GuC init failed with %d", ret);
	return ret;
}

void xe_guc_fini(struct xe_guc *guc)
{
	if (!xe_uc_fw_is_loadable(&guc->fw))
		return;

	xe_guc_log_fini(&guc->log);
	xe_uc_fw_fini(&guc->fw);
}
