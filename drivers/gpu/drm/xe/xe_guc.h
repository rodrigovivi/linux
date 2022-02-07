/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GUC_H_
#define _XE_GUC_H_

#include "xe_guc_types.h"

struct drm_printer;

int xe_guc_init(struct xe_guc *guc);
int xe_guc_reset(struct xe_guc *guc);
int xe_guc_upload(struct xe_guc *guc);
void xe_guc_print_info(struct xe_guc *guc, struct drm_printer *p);

static inline void
xe_guc_sanitize(struct xe_guc *guc)
{
	// TODO - Reset GuC SW state
}

#endif	/* _XE_GUC_H_ */
