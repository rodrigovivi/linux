// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_device.h"
#include "xe_guc.h"
#include "xe_huc.h"
#include "xe_uc.h"
#include "xe_wopcm.h"

static struct xe_device *
uc_to_xe(struct xe_uc *uc)
{
	return container_of(uc, struct xe_device, uc);
}

/* Should be called once at driver load only */
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

static int uc_reset(struct xe_uc *uc)
{
	struct xe_device *xe = uc_to_xe(uc);
	int ret;

	ret = xe_guc_reset(&uc->guc);
	if (ret) {
		drm_err(&xe->drm, "Failed to reset GuC, ret = %d\n", ret);
		return ret;
	}

	return 0;
}

static int uc_sanitize(struct xe_uc *uc)
{
	xe_huc_sanitize(&uc->huc);
	xe_guc_sanitize(&uc->guc);

	return uc_reset(uc);
}

/*
 * Should be called during driver load, after every GT reset, and after every
 * suspend to reload / auth the firmwares.
 */
int xe_uc_init_hw(struct xe_uc *uc)
{
	int ret;

	ret = uc_sanitize(uc);
	if (ret)
		return ret;

	ret = xe_huc_upload(&uc->huc);
	if (ret)
		return ret;

	return 0;
}

void xe_uc_fini(struct xe_uc *uc)
{
	xe_huc_fini(&uc->huc);
	xe_guc_fini(&uc->guc);
}
