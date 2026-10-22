/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023-2025 Intel Corporation
 */

#ifndef _XE_EUDEBUG_HW_H_
#define _XE_EUDEBUG_HW_H_

#include <linux/types.h>

struct xe_hw_engine;

void xe_eudebug_init_hw_engine(struct xe_hw_engine *hwe, bool enable);

#endif /* _XE_EUDEBUG_HW_H_ */
