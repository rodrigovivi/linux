/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GUC_LOG_H_
#define _XE_GUC_LOG_H_

#include "xe_guc_log_types.h"

int xe_guc_log_init(struct xe_guc_log *log);
void xe_guc_log_fini(struct xe_guc_log *log);

#endif	/* _XE_GUC_LOG_H_ */
