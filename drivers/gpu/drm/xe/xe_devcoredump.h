/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef _XE_DEVCOREDUMP_H_
#define _XE_DEVCOREDUMP_H_

struct xe_device;
struct xe_engine;

#ifdef CONFIG_DEV_COREDUMP
void xe_devcoredump(struct xe_engine *e);
void xe_devcoredump_fini(struct xe_device *xe);
#else
static inline void xe_devcoredump(struct xe_engine *e)
{
}
static inline void xe_devcoredump_fini(struct xe_device *xe)
{
}
#endif

#endif
