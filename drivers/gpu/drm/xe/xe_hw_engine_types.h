/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_HW_ENGINE_TYPES_H_
#define _XE_HW_ENGINE_TYPES_H_

#include "xe_force_wake_types.h"
#include "xe_lrc_types.h"

/* See "Engine ID Definition" struct in the Icelake PRM */
enum xe_engine_class {
	XE_ENGINE_CLASS_RENDER = 0,
	XE_ENGINE_CLASS_VIDEO_DECODE = 1,
	XE_ENGINE_CLASS_VIDEO_ENHANCE = 2,
	XE_ENGINE_CLASS_COPY = 3,
	XE_ENGINE_CLASS_OTHER = 4,
	XE_ENGINE_CLASS_COMPUTE = 5,
};

enum xe_hw_engine_id {
	XE_HW_ENGINE_RCS0,
	XE_HW_ENGINE_BCS0,
	XE_HW_ENGINE_VCS0,
	XE_HW_ENGINE_VCS1,
	XE_HW_ENGINE_VCS2,
	XE_HW_ENGINE_VCS3,
	XE_HW_ENGINE_VCS4,
	XE_HW_ENGINE_VCS5,
	XE_HW_ENGINE_VCS6,
	XE_HW_ENGINE_VCS7,
	XE_HW_ENGINE_VECS0,
	XE_HW_ENGINE_VECS1,
	XE_HW_ENGINE_VECS2,
	XE_HW_ENGINE_VECS3,
	XE_NUM_HW_ENGINES,
};

struct xe_bo;
struct xe_execlist_port;
struct xe_gt;

struct xe_hw_engine {
	struct xe_gt *gt;

	const char *name;
	enum xe_engine_class class;
	uint16_t instance;
	uint32_t mmio_base;
	enum xe_force_wake_domains domain;

	struct xe_bo *hwsp;

	struct xe_lrc kernel_lrc;

	struct xe_execlist_port *exl_port;

	struct xe_hw_fence_irq fence_irq;

	void (*irq_handler)(struct xe_hw_engine *, uint16_t);
};

#endif /* _XE_HW_ENGINE_TYPES_H_ */
