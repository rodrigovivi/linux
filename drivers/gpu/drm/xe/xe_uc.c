// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_guc.h"
#include "xe_uc.h"
#include "xe_uc_fw.h"
#include "xe_wopcm.h"

static void uc_fetch_firmwares(struct xe_uc *uc)
{
	uc->guc.fw.type = XE_UC_FW_TYPE_GUC;
	xe_uc_fw_init(&uc->guc.fw);

	uc->huc.fw.type = XE_UC_FW_TYPE_HUC;
	xe_uc_fw_init(&uc->huc.fw);
}

static void uc_cleanup_firmwares(struct xe_uc *uc)
{
	xe_uc_fw_fini(&uc->huc.fw);
	xe_uc_fw_fini(&uc->guc.fw);
}

int xe_uc_init(struct xe_uc *uc)
{
	int ret;

	uc_fetch_firmwares(uc);
	xe_wopcm_init(&uc->wopcm);

	ret = xe_guc_init(&uc->guc);
	if (ret)
		return ret;

	return 0;
}

void xe_uc_fini(struct xe_uc *uc)
{
	xe_guc_fini(&uc->guc);
	uc_cleanup_firmwares(uc);
}
