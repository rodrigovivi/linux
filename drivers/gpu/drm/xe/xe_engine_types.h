/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_ENGINE_TYPES_H_
#define _XE_ENGINE_TYPES_H_

#include <linux/kref.h>

#include "xe_lrc_types.h"

struct drm_sched_entity;
struct xe_execlist_engine;
struct xe_gt;
struct xe_guc_engine;
struct xe_hw_engine;
struct xe_vm;

struct xe_engine {
	struct xe_gt *gt;

	struct xe_hw_engine *hwe;

	struct kref refcount;

	struct xe_vm *vm;

#define ENGINE_FLAG_BANNED	BIT(0)
#define ENGINE_FLAG_KERNEL	BIT(1)
	unsigned long flags;

	union {
		struct xe_execlist_engine *execlist;
		struct xe_guc_engine *guc;
	};

	struct drm_sched_entity *entity;

	struct xe_lrc lrc;
};

/**
 * struct xe_engine_ops - Submission backend engine operations
 */
struct xe_engine_ops {
	/** @init: Initialize engine for submission backend */
	int (*init)(struct xe_engine *e);
	/** @init: Fini engine for submission backend */
	void (*fini)(struct xe_engine *e);
};

#endif	/* _XE_ENGINE_TYPES_H_ */
