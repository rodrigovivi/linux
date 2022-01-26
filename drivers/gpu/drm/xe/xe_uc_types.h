/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_UC_TYPES_H_
#define _XE_UC_TYPES_H_

#include "xe_guc_types.h"
#include "xe_huc_types.h"

/**
 * struct xe_uc - XE micro controllers
 */
struct xe_uc {
	/** @guc: Graphics micro controller */
	struct xe_guc guc;
	/** @huc: HuC */
	struct xe_huc huc;
};

#endif	/* _XE_UC_TYPES_H_ */
