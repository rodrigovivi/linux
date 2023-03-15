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
#include "xe_step_types.h"

#if IS_ENABLED(CONFIG_DRM_XE_DISPLAY)
#include "ext/intel_device_info.h"
#include "ext/intel_pch.h"
#include "intel_display_core.h"
#endif

#define XE_BO_INVALID_OFFSET	LONG_MAX

#define GRAPHICS_VER(xe) ((xe)->info.graphics_verx100 / 100)
#define MEDIA_VER(xe) ((xe)->info.media_verx100 / 100)
#define GRAPHICS_VERx100(xe) ((xe)->info.graphics_verx100)
#define MEDIA_VERx100(xe) ((xe)->info.media_verx100)
#define IS_DGFX(xe) ((xe)->info.is_dgfx)

#define XE_VRAM_FLAGS_NEED64K		BIT(0)

#define XE_GT0		0
#define XE_GT1		1
#define XE_MAX_GT	(XE_GT1 + 1)

#define XE_MAX_ASID	(BIT(20))

#define IS_PLATFORM_STEP(_xe, _platform, min_step, max_step)	\
	((_xe)->info.platform == (_platform) &&			\
	 (_xe)->info.step.graphics >= (min_step) &&		\
	 (_xe)->info.step.graphics < (max_step))
#define IS_SUBPLATFORM_STEP(_xe, _platform, sub, min_step, max_step)	\
	((_xe)->info.platform == (_platform) &&				\
	 (_xe)->info.subplatform == (sub) &&				\
	 (_xe)->info.step.graphics >= (min_step) &&			\
	 (_xe)->info.step.graphics < (max_step))

/**
 * struct xe_device - Top level struct of XE device
 */
struct xe_device {
	/** @drm: drm device */
	struct drm_device drm;

	/** @info: device info */
	struct intel_device_info {
		/** @graphics_verx100: graphics IP version */
		u32 graphics_verx100;
		/** @media_verx100: media IP version */
		u32 media_verx100;
		/** @mem_region_mask: mask of valid memory regions */
		u32 mem_region_mask;
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
		/** @step: stepping information for each IP */
		struct xe_step_info step;
		/** @dma_mask_size: DMA address bits */
		u8 dma_mask_size;
		/** @vram_flags: Vram flags */
		u8 vram_flags;
		/** @tile_count: Number of tiles */
		u8 tile_count;
		/** @vm_max_level: Max VM level */
		u8 vm_max_level;
		/** @supports_usm: Supports unified shared memory */
		bool supports_usm;
		/** @has_asid: Has address space ID */
		bool has_asid;
		/** @enable_guc: GuC submission enabled */
		bool enable_guc;
		/** @has_flat_ccs: Whether flat CCS metadata is used */
		bool has_flat_ccs;
		/** @has_4tile: Whether tile-4 tiling is supported */
		bool has_4tile;
		/** @has_range_tlb_invalidation: Has range based TLB invalidations */
		bool has_range_tlb_invalidation;
		/** @enable_display: display enabled */
		bool enable_display;

#if IS_ENABLED(CONFIG_DRM_XE_DISPLAY)
		struct xe_device_display_info {
			u8 ver;

			u8 pipe_mask;
			u8 cpu_transcoder_mask;
			u8 fbc_mask;
			u8 abox_mask;

			struct {
				u16 size; /* in blocks */
				u8 slice_mask;
			} dbuf;

#define DEV_INFO_DISPLAY_FOR_EACH_FLAG(func) \
			/* Keep in alphabetical order */ \
			func(has_cdclk_crawl); \
			func(has_cdclk_squash); \
			func(has_dmc); \
			func(has_dp_mst); \
			func(has_dsb); \
			func(has_dsc); \
			func(has_fpga_dbg); \
			func(has_hdcp); \
			func(has_hti); \
			func(has_ipc); \
			func(has_modular_fia); \
			func(has_psr); \
			func(has_psr_hw_tracking);

#define DEFINE_FLAG(name) u8 name:1
			DEV_INFO_DISPLAY_FOR_EACH_FLAG(DEFINE_FLAG);
#undef DEFINE_FLAG

			/* Register offsets for the various display pipes and transcoders */
			u32 pipe_offsets[I915_MAX_TRANSCODERS];
			u32 trans_offsets[I915_MAX_TRANSCODERS];
			u32 cursor_offsets[I915_MAX_PIPES];

			struct {
				u32 degamma_lut_size;
				u32 gamma_lut_size;
				u32 degamma_lut_tests;
				u32 gamma_lut_tests;
			} color;

			/* Populated by intel_device_runtime_init() */
			u8 num_sprites[I915_MAX_PIPES];
			u8 num_scalers[I915_MAX_PIPES];
			u32 rawclk_freq;
		} display;
#endif
	} info;

	/** @irq: device interrupt state */
	struct {
		/** @lock: lock for processing irq's on this device */
		spinlock_t lock;

		/** @enabled: interrupts enabled on this device */
		bool enabled;
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

	/** @mem: memory info for device */
	struct {
		/** @vram: VRAM info for device */
		struct {
			/** @io_start: start address of VRAM */
			resource_size_t io_start;
			/** @size: size of VRAM */
			resource_size_t size;
			/** @mapping: pointer to VRAM mappable space */
			void *__iomem mapping;
		} vram;
	} mem;

	/** @usm: unified memory state */
	struct {
		/** @asid: convert a ASID to VM */
		struct xarray asid_to_vm;
		/** @next_asid: next ASID, used to cyclical alloc asids */
		u32 next_asid;
		/** @num_vm_in_fault_mode: number of VM in fault mode */
		u32 num_vm_in_fault_mode;
		/** @num_vm_in_non_fault_mode: number of VM in non-fault mode */
		u32 num_vm_in_non_fault_mode;
		/** @lock: protects UM state */
		struct mutex lock;
	} usm;

	/** @persistent_engines: engines that are closed but still running */
	struct {
		/** @lock: protects persistent engines */
		struct mutex lock;
		/** @list: list of persistent engines */
		struct list_head list;
	} persistent_engines;

	/** @pinned: pinned BO state */
	struct {
		/** @lock: protected pinned BO list state */
		spinlock_t lock;
		/** @evicted: pinned kernel BO that are present */
		struct list_head kernel_bo_present;
		/** @evicted: pinned BO that have been evicted */
		struct list_head evicted;
		/** @external_vram: pinned external BO in vram*/
		struct list_head external_vram;
	} pinned;

	/** @ufence_wq: user fence wait queue */
	wait_queue_head_t ufence_wq;

	/** @ordered_wq: used to serialize compute mode resume */
	struct workqueue_struct *ordered_wq;

	/** @gt: graphics tile */
	struct xe_gt gt[XE_MAX_GT];

	/**
	 * @mem_access: keep track of memory access in the device, possibly
	 * triggering additional actions when they occur.
	 */
	struct {
		/** @lock: protect the ref count */
		struct mutex lock;
		/** @ref: ref count of memory accesses */
		s32 ref;
		/** @hold_rpm: need to put rpm ref back at the end */
		bool hold_rpm;
	} mem_access;

	/** @d3cold_allowed: Indicates if d3cold is a valid device state */
	bool d3cold_allowed;

	/* private: */

#if IS_ENABLED(CONFIG_DRM_XE_DISPLAY)
	/*
	 * Any fields below this point are the ones used by display.
	 * They are temporarily added here so xe_device can be desguised as
	 * drm_i915_private during build. After cleanup these should go away,
	 * migrating to the right sub-structs
	 */
	struct intel_display display;
	enum intel_pch pch_type;
	u16 pch_id;

	struct dram_info {
		bool wm_lv_0_adjust_needed;
		u8 num_channels;
		bool symmetric_memory;
		enum intel_dram_type {
			INTEL_DRAM_UNKNOWN,
			INTEL_DRAM_DDR3,
			INTEL_DRAM_DDR4,
			INTEL_DRAM_LPDDR3,
			INTEL_DRAM_LPDDR4,
			INTEL_DRAM_DDR5,
			INTEL_DRAM_LPDDR5,
		} type;
		u8 num_qgv_points;
		u8 num_psf_gv_points;
	} dram_info;

	/* To shut up runtime pm macros.. */
	struct xe_runtime_pm {} runtime_pm;

	/* For pcode */
	struct mutex sb_lock;

	/* Should be in struct intel_display */
	u32 skl_preferred_vco_freq, max_dotclk_freq, hti_state;
	u8 snps_phy_failed_calibration;
	struct drm_atomic_state *modeset_restore_state;
	struct list_head global_obj_list;

	u32 de_irq_mask[I915_MAX_PIPES];
	bool display_irqs_enabled;
	u32 enabled_irq_mask;

	struct {
		/* Backlight: XXX: needs to be set to -1 */
		s32 invert_brightness;
		s32 vbt_sdvo_panel_type;
		u32 edp_vswing;

		/* PM support, needs to be -1 as well */
		s32 disable_power_well;
		s32 enable_dc;

		const char *dmc_firmware_path;
		s32 enable_dpcd_backlight;
		s32 enable_dp_mst;
		s32 enable_fbc;
		s32 enable_psr;
		bool psr_safest_params;
		s32 enable_psr2_sel_fetch;

		s32 panel_use_ssc;
		const char *vbt_firmware;
		u32 lvds_channel_mode;
	} params;
#endif
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

#endif
