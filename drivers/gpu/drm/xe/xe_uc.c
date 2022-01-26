// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_guc.h"
#include "xe_huc.h"
#include "xe_uc.h"
#include "xe_wopcm.h"

int xe_uc_init(struct xe_uc *uc)
{
	int ret;

	ret = xe_guc_init(&uc->guc);
	if (ret)
		return ret;

	ret = xe_huc_init(&uc->huc);
	if (ret)
		goto err_guc;

	ret = xe_wopcm_init(&uc->wopcm);
	if (ret)
		goto err_huc;

	return 0;

err_huc:
	xe_huc_fini(&uc->huc);
err_guc:
	xe_guc_fini(&uc->guc);
	return ret;
}

void xe_uc_fini(struct xe_uc *uc)
{
	xe_huc_fini(&uc->huc);
	xe_guc_fini(&uc->guc);
}
