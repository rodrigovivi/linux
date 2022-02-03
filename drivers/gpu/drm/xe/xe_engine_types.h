/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_ENGINE_TYPES_H_
#define _XE_ENGINE_TYPES_H_

#include <linux/kref.h>

#include "xe_lrc_types.h"

struct xe_hw_engine;
struct xe_vm;
struct xe_execlist;
struct drm_sched_entity;

struct xe_engine {
	struct xe_hw_engine *hwe;

	struct kref refcount;

	struct xe_vm *vm;

	struct xe_execlist *execlist;

	struct drm_sched_entity *entity;

	struct xe_lrc lrc;
};

#endif	/* _XE_ENGINE_TYPES_H_ */
