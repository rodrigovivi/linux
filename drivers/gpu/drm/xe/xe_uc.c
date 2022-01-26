// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_uc.h"
#include "xe_uc_fw.h"

void xe_uc_fetch_firmwares(struct xe_uc *uc)
{
	uc->guc.fw.type = XE_UC_FW_TYPE_GUC;
	xe_uc_fw_init(&uc->guc.fw);

	uc->huc.fw.type = XE_UC_FW_TYPE_HUC;
	xe_uc_fw_init(&uc->huc.fw);
}

void xe_uc_cleanup_firmwares(struct xe_uc *uc)
{
	xe_uc_fw_fini(&uc->huc.fw);
	xe_uc_fw_fini(&uc->guc.fw);
}
