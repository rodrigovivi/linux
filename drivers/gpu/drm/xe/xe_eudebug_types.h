/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023-2025 Intel Corporation
 */

#ifndef _XE_EUDEBUG_TYPES_H_
#define _XE_EUDEBUG_TYPES_H_

/**
 * enum xe_eudebug_cap_state - eudebug capability state
 *
 * @XE_EUDEBUG_CAP_NOT_SUPPORTED: eudebug feature support off
 * @XE_EUDEBUG_CAP_DISABLED: eudebug feature supported but disabled
 * @XE_EUDEBUG_CAP_ENABLED: eudebug enabled
 */
enum xe_eudebug_cap_state {
	XE_EUDEBUG_CAP_NOT_SUPPORTED = 0,
	XE_EUDEBUG_CAP_DISABLED,
	XE_EUDEBUG_CAP_ENABLED,
};

#endif /* _XE_EUDEBUG_TYPES_H_ */
