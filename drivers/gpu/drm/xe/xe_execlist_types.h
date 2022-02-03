/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_EXECLIST_TYPES_H_
#define _XE_EXECLIST_TYPES_H_

#include <linux/list.h>
#include <linux/spinlock.h>

#include <drm/gpu_scheduler.h>

struct xe_hw_engine;
struct xe_execlist;

struct xe_execlist_port {
	struct xe_hw_engine *hwe;

	spinlock_t lock;

	struct list_head active[DRM_SCHED_PRIORITY_COUNT];

	uint32_t last_ctx_id;

	struct xe_execlist *running_exl;

	struct timer_list irq_fail;
};

struct xe_execlist {
	struct xe_engine *engine;

	struct drm_gpu_scheduler sched;

	struct drm_sched_entity entity;

	struct xe_execlist_port *port;

	bool has_run;

	enum drm_sched_priority active_priority;
	struct list_head active_link;
};

#endif	/* _XE_EXECLIST_TYPES_H_ */
