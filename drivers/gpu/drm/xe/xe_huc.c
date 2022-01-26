// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_huc.h"
#include "xe_uc_fw.h"

int xe_huc_init(struct xe_huc *huc)
{
	int ret;

	huc->fw.type = XE_UC_FW_TYPE_HUC;
	ret = xe_uc_fw_init(&huc->fw);
	if (ret)
		return ret;

	return 0;
}

void xe_huc_fini(struct xe_huc *huc)
{
	xe_uc_fw_fini(&huc->fw);
}
