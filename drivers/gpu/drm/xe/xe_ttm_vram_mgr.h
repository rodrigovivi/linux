/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_TTM_VRAM_MGR_H_
#define _XE_TTM_VRAM_MGR_H_

#include "xe_ttm_vram_mgr_types.h"

struct xe_device;

int xe_ttm_vram_mgr_init(struct xe_device *xe);
void xe_ttm_vram_mgr_fini(struct xe_device *xe);

#endif	/* _XE_TTM_VRAM_MGR_H_ */
