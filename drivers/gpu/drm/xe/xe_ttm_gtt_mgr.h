/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_TTGM_GTT_MGR_H_
#define _XE_TTGM_GTT_MGR_H_

#include "xe_ttm_gtt_mgr_types.h"

struct xe_device;

int xe_ttm_gtt_mgr_init(struct xe_device *xe, uint64_t gtt_size);
void xe_ttm_gtt_mgr_fini(struct xe_device *xe);

#endif	/* _XE_TTGM_GTT_MGR_H_ */
