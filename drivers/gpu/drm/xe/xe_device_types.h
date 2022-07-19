/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_DEVICE_TYPES_H_
#define _XE_DEVICE_TYPES_H_

#include <linux/pci.h>

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/ttm/ttm_device.h>

#include "xe_gt_types.h"
#include "xe_platform_types.h"

#define XE_BO_INVALID_OFFSET	LONG_MAX

#define GRAPHICS_VER(xe) ((xe)->info.graphics_verx100 / 100)
#define GRAPHICS_VERx100(xe) ((xe)->info.graphics_verx100)
#define IS_DGFX(xe) ((xe)->info.is_dgfx)

#define XE_VRAM_FLAGS_NEED64K		BIT(0)

/**
 * struct xe_device - Top level struct of XE device
 */
struct xe_device {
	/** @drm: drm device */
	struct drm_device drm;

	/** @info: device info */
	struct {
		/** @graphics_verx100: graphics version */
		u32 graphics_verx100;
		/** @is_dgfx: is discrete device */
		bool is_dgfx;
		/** @platform: XE platform enum */
		enum xe_platform platform;
		/** @subplatform: XE subplatform enum */
		enum xe_subplatform subplatform;
		/** @devid: device ID */
		u16 devid;
		/** @revid: device revision */
		u8 revid;
		/** @dma_mask_size: DMA address bits */
		u8 dma_mask_size;
		/** @vram_flags: Vram flags */
		u8 vram_flags;
		/** @tile_count: Number of tiles */
		u8 tile_count;
		/** @vm_max_level: Max VM level */
		u8 vm_max_level;
	} info;

	/** @irq: device interrupt state */
	struct {
		/** @enabled: interrupts enabled on this device */
		bool enabled;
		/** @lock: lock for processing irq's on this device */
		spinlock_t lock;
	} irq;

	/** @ttm: ttm device */
	struct ttm_device ttm;

	/** @mmio: mmio info for device */
	struct {
		/** @size: size of MMIO space for device */
		size_t size;
		/** @regs: pointer to MMIO space for device */
		void *regs;
	} mmio;

	/** @persitent_engines: engines that are closed but still running */
	struct {
		/** @lock: protects persitent engines */
		struct mutex lock;
		/** @list: list of persitent engines */
		struct list_head list;
	} persitent_engines;

	/** @pinned: pinned BO state */
	struct {
		/** @lock: protected pinned BO list state */
		spinlock_t lock;
		/** @evicted: pinned BO that are present */
		struct list_head present;
		/** @evicted: pinned BO that have been evicted */
		struct list_head evicted;
	} pinned;

	/** @ufence_wq: user fence wait queue */
	wait_queue_head_t ufence_wq;

	/** @gt: graphics tile */
	struct xe_gt gt;
};

/**
 * struct xe_file - file handle for XE driver
 */
struct xe_file {
	/** @drm: base DRM file */
	struct drm_file *drm;

	/** @vm: VM state for file */
	struct {
		/** @xe: xarray to store VMs */
		struct xarray xa;
		/** @lock: protects file VM state */
		struct mutex lock;
	} vm;

	/** @engine: Submission engine state for file */
	struct {
		/** @xe: xarray to store engines */
		struct xarray xa;
		/** @lock: protects file engine state */
		struct mutex lock;
	} engine;
};

#endif	/* _XE_DEVICE_TYPES_H_ */
