/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_UC_H_
#define _XE_UC_H_

#include "xe_uc_types.h"

int xe_uc_init(struct xe_uc *uc);
void xe_uc_fini(struct xe_uc *uc);

#endif	/* _XE_UC_H_ */
