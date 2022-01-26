/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GUC_LOG_TYPES_H_
#define _XE_GUC_LOG_TYPES_H_

struct xe_bo;

/**
 * struct xe_guc_log - GuC log
 */
struct xe_guc_log {
	/** @bo: XE BO for GuC log */
	struct xe_bo *bo;
};

#endif	/* _XE_GUC_LOG_TYPES_H_ */
