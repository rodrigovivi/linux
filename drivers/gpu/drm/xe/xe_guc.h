/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GUC_H_
#define _XE_GUC_H_

#include "xe_guc_types.h"

int xe_guc_init(struct xe_guc *guc);
void xe_guc_fini(struct xe_guc *guc);

#endif	/* _XE_GUC_H_ */
