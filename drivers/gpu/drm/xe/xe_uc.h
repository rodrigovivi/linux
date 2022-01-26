/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_UC_H_
#define _XE_UC_H_

#include "xe_uc_types.h"

void xe_uc_fetch_firmwares(struct xe_uc *uc);
void xe_uc_cleanup_firmwares(struct xe_uc *uc);

#endif	/* _XE_UC_H_ */
