/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_HUC_TYPES_H_
#define _XE_HUC_TYPES_H_

#include "xe_uc_fw_types.h"

/**
 * struct xe_huc - HuC
 */
struct xe_huc {
	/** @fw: Generic uC firmware management */
	struct xe_uc_fw fw;

	/** @status: HuC register status info */
	struct {
		/** @reg: status register address */
		u32 reg;
		/** @mask: mask of status field */
		u32 mask;
		/** @value: expected status value */
		u32 value;
	} status;
};

#endif	/* _XE_HUC_TYPES_H_ */
