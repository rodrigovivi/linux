/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GUC_TYPES_H_
#define _XE_GUC_TYPES_H_

#include "xe_guc_ads_types.h"
#include "xe_guc_ct_types.h"
#include "xe_guc_fwif.h"
#include "xe_guc_log_types.h"
#include "xe_uc_fw_types.h"

/**
 * struct xe_guc - Graphic micro controller
 */
struct xe_guc {
	/** @fw: Generic uC firmware management */
	struct xe_uc_fw fw;
	/** @log: GuC log */
	struct xe_guc_log log;
	/** @ads: GuC ads */
	struct xe_guc_ads ads;
	/** @log: GuC ct */
	struct xe_guc_ct ct;

	/** @params: Control params for fw initialization */
	u32 params[GUC_CTL_MAX_DWORDS];
};

#endif	/* _XE_GUC_TYPES_H_ */
