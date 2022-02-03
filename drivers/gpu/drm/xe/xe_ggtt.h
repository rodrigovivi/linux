/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#ifndef _XE_GGTT_H_
#define _XE_GGTT_H_

#include "xe_ggtt_types.h"

int xe_ggtt_init(struct xe_device *xe, struct xe_ggtt *ggtt);
void xe_ggtt_finish(struct xe_ggtt *ggtt);
void xe_ggtt_printk(struct xe_ggtt *ggtt, const char *prefix);

int xe_ggtt_insert_bo(struct xe_ggtt *ggtt, struct xe_bo *bo);
void xe_ggtt_remove_bo(struct xe_ggtt *ggtt, struct xe_bo *bo);

#endif /* _XE_GGTT_H_ */
