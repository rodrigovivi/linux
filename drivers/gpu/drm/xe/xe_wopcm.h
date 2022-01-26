/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_WOPCM_H_
#define _XE_WOPCM_H_

#include "xe_wopcm_types.h"

/**
 * xe_wopcm_guc_base()
 * @wopcm:	xe_wopcm structure
 *
 * Returns the base of the WOPCM shadowed region.
 *
 * Returns:
 * 0 if GuC is not present or not in use.
 * Otherwise, the GuC WOPCM base.
 */
static inline u32 xe_wopcm_guc_base(struct xe_wopcm *wopcm)
{
	return wopcm->guc.base;
}

/**
 * xe_wopcm_guc_size()
 * @wopcm:	xe_wopcm structure
 *
 * Returns size of the WOPCM shadowed region.
 *
 * Returns:
 * 0 if GuC is not present or not in use.
 * Otherwise, the GuC WOPCM size.
 */
static inline u32 xe_wopcm_guc_size(struct xe_wopcm *wopcm)
{
	return wopcm->guc.size;
}

void xe_wopcm_init(struct xe_wopcm *wopcm);

#endif	/* _XE_WOPCM_H_ */
