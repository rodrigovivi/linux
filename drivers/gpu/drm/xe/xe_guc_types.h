/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GUC_TYPES_H_
#define _XE_GUC_TYPES_H_

#include "xe_uc_fw_types.h"

/**
 * struct xe_guc - Graphic micro controller
 */
struct xe_guc {
	/** @fw: Generic uC firmware management */
	struct xe_uc_fw fw;
};

#endif	/* _XE_GUC_TYPES_H_ */
