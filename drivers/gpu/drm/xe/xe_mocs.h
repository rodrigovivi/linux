/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_MOCS_H_
#define _XE_MOCS_H_

struct xe_engine;
struct xe_gt;

void xe_mocs_init_engine(const struct xe_engine *engine);
void xe_mocs_init(struct xe_gt *gt);

#endif
