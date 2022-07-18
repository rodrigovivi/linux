/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_PLATFORM_INFO_TYPES_H_
#define _XE_PLATFORM_INFO_TYPES_H_

/* Keep in gen based order, and chronological order within a gen */
enum xe_platform {
	XE_PLATFORM_UNINITIALIZED = 0,
	/* gen12 */
	XE_TIGERLAKE,
	XE_DG1,
	XE_DG2,
	XE_PVC,
	XE_ALDERLAKE_S,
};

#endif	/* _XE_PLATFORM_INFO_TYPES_H_ */
