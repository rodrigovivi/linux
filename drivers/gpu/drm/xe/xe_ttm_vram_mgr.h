/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_TTM_VRAM_MGR_H_
#define _XE_TTM_VRAM_MGR_H_

#include "xe_ttm_vram_mgr_types.h"

struct xe_gt;

int xe_ttm_vram_mgr_init(struct xe_gt *gt, struct xe_ttm_vram_mgr *mgr);
void xe_ttm_vram_mgr_fini(struct xe_ttm_vram_mgr *mgr);

#endif	/* _XE_TTM_VRAM_MGR_H_ */
