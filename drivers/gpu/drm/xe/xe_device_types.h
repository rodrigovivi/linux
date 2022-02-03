/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_DEVICE_TYPES_H_
#define _XE_DEVICE_TYPES_H_

#include <linux/pci.h>

#include <drm/drm_device.h>
#include <drm/drm_file.h>

#include "xe_ggtt_types.h"
#include "xe_force_wake_types.h"
#include "xe_hw_engine_types.h"
#include "xe_ttm_vram_mgr_types.h"
#include "xe_ttm_gtt_mgr_types.h"
#include "xe_uc_types.h"
#include "xe_platform_types.h"

#define XE_BO_INVALID_OFFSET	LONG_MAX

#define GRAPHICS_VER(xe) ((xe)->info.graphics_verx10 / 10)
#define GRAPHICS_VERx10(xe) ((xe)->info.graphics_verx10)
#define IS_DGFX(xe) ((xe)->info.is_dgfx)

#define ENGINE_INSTANCES_MASK(xe, first, count) ({		\
	unsigned int first__ = (first);					\
	unsigned int count__ = (count);					\
	((xe)->info.engine_mask &					\
	 GENMASK(first__ + count__ - 1, first__)) >> first__;		\
})
#define VDBOX_MASK(xe) \
	ENGINE_INSTANCES_MASK(xe, XE_HW_ENGINE_VCS0, \
			      (XE_HW_ENGINE_VCS7 - XE_HW_ENGINE_VCS0 + 1))
#define VEBOX_MASK(xe) \
	ENGINE_INSTANCES_MASK(xe, XE_HW_ENGINE_VECS0, \
			      (XE_HW_ENGINE_VECS3 - XE_HW_ENGINE_VECS0 + 1))

struct xe_device {
	struct drm_device drm;

	struct {
		uint8_t graphics_verx10;
		bool is_dgfx;
		enum xe_platform platform;
		u64 engine_mask;
		u16 devid;
		u8 revid;
	} info;

	struct ttm_device ttm;
	struct xe_ttm_vram_mgr vram_mgr;
	struct xe_ttm_gtt_mgr gtt_mgr;

	bool irq_enabled;
	spinlock_t gt_irq_lock;

	struct {
		size_t size;
		void *regs;
	} mmio;

	struct {
		resource_size_t io_start;
		resource_size_t size;
		void *__iomem mapping;
	} vram;

	struct xe_force_wake fw;

	struct xe_uc uc;

	struct xe_ggtt ggtt;

	struct xe_hw_engine hw_engines[XE_NUM_HW_ENGINES];
};

struct xe_file {
	struct drm_file *drm;

	struct xarray vm_xa;
	struct mutex vm_lock;

	struct xarray engine_xa;
	struct mutex engine_lock;
};

#endif	/* _XE_DEVICE_TYPES_H_ */
