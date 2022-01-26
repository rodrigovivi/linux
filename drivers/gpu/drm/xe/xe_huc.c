// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_device.h"
#include "xe_huc.h"
#include "xe_uc_fw.h"
#include "xe_guc_reg.h"

static struct xe_device *
huc_to_xe(struct xe_huc *huc)
{
	return container_of(huc, struct xe_device, uc.huc);
}

int xe_huc_init(struct xe_huc *huc)
{
	struct xe_device *xe = huc_to_xe(huc);
	int ret;

	huc->fw.type = XE_UC_FW_TYPE_HUC;
	ret = xe_uc_fw_init(&huc->fw);
	if (ret)
		goto out;

	huc->status.reg = GEN11_HUC_KERNEL_LOAD_INFO.reg;
	huc->status.mask = HUC_LOAD_SUCCESSFUL;
	huc->status.value = HUC_LOAD_SUCCESSFUL;

	xe_uc_fw_change_status(&huc->fw, XE_UC_FIRMWARE_LOADABLE);

	return 0;

out:
	drm_err(&xe->drm, "HuC init failed with %d", ret);
	return ret;
}

void xe_huc_fini(struct xe_huc *huc)
{
	if (!xe_uc_fw_is_loadable(&huc->fw))
		return;

	xe_uc_fw_fini(&huc->fw);
}
