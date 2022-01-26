// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_guc.h"
#include "xe_uc_fw.h"

int xe_guc_init(struct xe_guc *guc)
{
	int ret;

	guc->fw.type = XE_UC_FW_TYPE_GUC;
	ret = xe_uc_fw_init(&guc->fw);
	if (ret)
		return ret;

	return 0;
}

void xe_guc_fini(struct xe_guc *guc)
{
	xe_uc_fw_fini(&guc->fw);
}
