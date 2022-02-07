/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#include "xe_pci.h"

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/vga_switcheroo.h>

#include <drm/drm_drv.h>
#include <drm/i915_pciids.h>
#include <drm/drm_color_mgmt.h>

#include "xe_drv.h"
#include "xe_device.h"
#include "xe_macros.h"

#include "../i915/i915_reg.h"

enum intel_ppgtt_type {
	INTEL_PPGTT_NONE = 0,
	INTEL_PPGTT_ALIASING = 1,
	INTEL_PPGTT_FULL = 2,
};

enum pipe {
	INVALID_PIPE = -1,

	PIPE_A = 0,
	PIPE_B,
	PIPE_C,
	PIPE_D,
	_PIPE_EDP,

	I915_MAX_PIPES = _PIPE_EDP
};

/*
 * FIXME: We should probably switch this to a 0-based scheme to be consistent
 * with how we now name/number DBUF_CTL instances.
 */
enum dbuf_slice {
	DBUF_S1,
	DBUF_S2,
	DBUF_S3,
	DBUF_S4,
	I915_MAX_DBUF_SLICES
};


#define I915_GTT_PAGE_SIZE_4K	BIT_ULL(12)
#define I915_GTT_PAGE_SIZE_64K	BIT_ULL(16)
#define I915_GTT_PAGE_SIZE_2M	BIT_ULL(21)


enum intel_region_id {
	INTEL_REGION_SMEM = 0,
	INTEL_REGION_LMEM,
	INTEL_REGION_STOLEN_SMEM,
	INTEL_REGION_STOLEN_LMEM,
	INTEL_REGION_UNKNOWN, /* Should be last */
};

#define REGION_SMEM     BIT(INTEL_REGION_SMEM)
#define REGION_LMEM     BIT(INTEL_REGION_LMEM)
#define REGION_STOLEN_SMEM   BIT(INTEL_REGION_STOLEN_SMEM)
#define REGION_STOLEN_LMEM   BIT(INTEL_REGION_STOLEN_LMEM)

enum transcoder {
	INVALID_TRANSCODER = -1,
	/*
	 * The following transcoders have a 1:1 transcoder -> pipe mapping,
	 * keep their values fixed: the code assumes that TRANSCODER_A=0, the
	 * rest have consecutive values and match the enum values of the pipes
	 * they map to.
	 */
	TRANSCODER_A = PIPE_A,
	TRANSCODER_B = PIPE_B,
	TRANSCODER_C = PIPE_C,
	TRANSCODER_D = PIPE_D,

	/*
	 * The following transcoders can map to any pipe, their enum value
	 * doesn't need to stay fixed.
	 */
	TRANSCODER_EDP,
	TRANSCODER_DSI_0,
	TRANSCODER_DSI_1,
	TRANSCODER_DSI_A = TRANSCODER_DSI_0,	/* legacy DSI */
	TRANSCODER_DSI_C = TRANSCODER_DSI_1,	/* legacy DSI */

	I915_MAX_TRANSCODERS
};

typedef u32 intel_engine_mask_t;

#define DEV_INFO_FOR_EACH_FLAG(func) \
	func(is_mobile); \
	func(is_lp); \
	func(require_force_probe); \
	func(is_dgfx); \
	/* Keep has_* in alphabetical order */ \
	func(has_64bit_reloc); \
	func(gpu_reset_clobbers_display); \
	func(has_reset_engine); \
	func(has_global_mocs); \
	func(has_gt_uc); \
	func(has_l3_dpf); \
	func(has_llc); \
	func(has_logical_ring_contexts); \
	func(has_logical_ring_elsq); \
	func(has_pooled_eu); \
	func(has_rc6); \
	func(has_rc6p); \
	func(has_rps); \
	func(has_runtime_pm); \
	func(has_snoop); \
	func(has_coherent_ggtt); \
	func(unfenced_needs_alignment); \
	func(hws_needs_physical);

#define DEV_INFO_DISPLAY_FOR_EACH_FLAG(func) \
	/* Keep in alphabetical order */ \
	func(cursor_needs_physical); \
	func(has_cdclk_crawl); \
	func(has_dmc); \
	func(has_ddi); \
	func(has_dp_mst); \
	func(has_dsb); \
	func(has_dsc); \
	func(has_fbc); \
	func(has_fpga_dbg); \
	func(has_gmch); \
	func(has_hdcp); \
	func(has_hotplug); \
	func(has_hti); \
	func(has_ipc); \
	func(has_modular_fia); \
	func(has_overlay); \
	func(has_psr); \
	func(has_psr_hw_tracking); \
	func(overlay_needs_physical); \
	func(supports_tv);

struct intel_device_info {
	u8 graphics_ver;
	u8 graphics_rel;
	u8 media_ver;
	u8 media_rel;

	intel_engine_mask_t platform_engine_mask; /* Engines supported by the HW */

	enum xe_platform platform;

	unsigned int dma_mask_size; /* available DMA address bits */

	enum intel_ppgtt_type ppgtt_type;
	unsigned int ppgtt_size; /* log2, e.g. 31/32/48 bits */

	unsigned int page_sizes; /* page sizes supported by the HW */

	u32 memory_regions; /* regions supported by the HW */

	u32 display_mmio_offset;

	u8 gt; /* GT number, 0 if undefined */

	u8 pipe_mask;
	u8 cpu_transcoder_mask;

	u8 abox_mask;

#define DEFINE_FLAG(name) u8 name:1
	DEV_INFO_FOR_EACH_FLAG(DEFINE_FLAG);
#undef DEFINE_FLAG

	struct {
		u8 ver;

#define DEFINE_FLAG(name) u8 name:1
		DEV_INFO_DISPLAY_FOR_EACH_FLAG(DEFINE_FLAG);
#undef DEFINE_FLAG
	} display;

	struct {
		u16 size; /* in blocks */
		u8 slice_mask;
	} dbuf;

	/* Register offsets for the various display pipes and transcoders */
	int pipe_offsets[I915_MAX_TRANSCODERS];
	int trans_offsets[I915_MAX_TRANSCODERS];
	int cursor_offsets[I915_MAX_PIPES];

	struct color_luts {
		u32 degamma_lut_size;
		u32 gamma_lut_size;
		u32 degamma_lut_tests;
		u32 gamma_lut_tests;
	} color;
};

#define PLATFORM(x) .platform = (x)
#define GEN(x) \
	.graphics_ver = (x), \
	.media_ver = (x), \
	.display.ver = (x)

#define IVB_PIPE_OFFSETS \
	.pipe_offsets = { \
		[TRANSCODER_A] = PIPE_A_OFFSET,	\
		[TRANSCODER_B] = PIPE_B_OFFSET, \
		[TRANSCODER_C] = PIPE_C_OFFSET, \
	}, \
	.trans_offsets = { \
		[TRANSCODER_A] = TRANSCODER_A_OFFSET, \
		[TRANSCODER_B] = TRANSCODER_B_OFFSET, \
		[TRANSCODER_C] = TRANSCODER_C_OFFSET, \
	}

#define HSW_PIPE_OFFSETS \
	.pipe_offsets = { \
		[TRANSCODER_A] = PIPE_A_OFFSET,	\
		[TRANSCODER_B] = PIPE_B_OFFSET, \
		[TRANSCODER_C] = PIPE_C_OFFSET, \
		[TRANSCODER_EDP] = PIPE_EDP_OFFSET, \
	}, \
	.trans_offsets = { \
		[TRANSCODER_A] = TRANSCODER_A_OFFSET, \
		[TRANSCODER_B] = TRANSCODER_B_OFFSET, \
		[TRANSCODER_C] = TRANSCODER_C_OFFSET, \
		[TRANSCODER_EDP] = TRANSCODER_EDP_OFFSET, \
	}

#define IVB_CURSOR_OFFSETS \
	.cursor_offsets = { \
		[PIPE_A] = CURSOR_A_OFFSET, \
		[PIPE_B] = IVB_CURSOR_B_OFFSET, \
		[PIPE_C] = IVB_CURSOR_C_OFFSET, \
	}

#define TGL_CURSOR_OFFSETS \
	.cursor_offsets = { \
		[PIPE_A] = CURSOR_A_OFFSET, \
		[PIPE_B] = IVB_CURSOR_B_OFFSET, \
		[PIPE_C] = IVB_CURSOR_C_OFFSET, \
		[PIPE_D] = TGL_CURSOR_D_OFFSET, \
	}

#define IVB_COLORS \
	.color = { .degamma_lut_size = 1024, .gamma_lut_size = 1024 }
#define GLK_COLORS \
	.color = { .degamma_lut_size = 33, .gamma_lut_size = 1024, \
		   .degamma_lut_tests = DRM_COLOR_LUT_NON_DECREASING | \
					DRM_COLOR_LUT_EQUAL_CHANNELS, \
	}

/* Keep in gen based order, and chronological order within a gen */

#define GEN_DEFAULT_PAGE_SIZES \
	.page_sizes = I915_GTT_PAGE_SIZE_4K

#define GEN_DEFAULT_REGIONS \
	.memory_regions = REGION_SMEM | REGION_STOLEN_SMEM

#define GEN7_FEATURES  \
	GEN(7), \
	.pipe_mask = BIT(PIPE_A) | BIT(PIPE_B) | BIT(PIPE_C), \
	.cpu_transcoder_mask = BIT(TRANSCODER_A) | BIT(TRANSCODER_B) | BIT(TRANSCODER_C), \
	.display.has_hotplug = 1, \
	.display.has_fbc = 1, \
	.platform_engine_mask = BIT(XE_HW_ENGINE_RCS0) | \
	BIT(XE_HW_ENGINE_VCS0) | BIT(XE_HW_ENGINE_BCS0), \
	.has_coherent_ggtt = true, \
	.has_llc = 1, \
	.has_rc6 = 1, \
	.has_rc6p = 1, \
	.has_reset_engine = true, \
	.has_rps = true, \
	.dma_mask_size = 40, \
	.ppgtt_type = INTEL_PPGTT_ALIASING, \
	.ppgtt_size = 31, \
	IVB_PIPE_OFFSETS, \
	IVB_CURSOR_OFFSETS, \
	IVB_COLORS, \
	GEN_DEFAULT_PAGE_SIZES, \
	GEN_DEFAULT_REGIONS

#define IVB_D_PLATFORM \
	GEN7_FEATURES, \
	PLATFORM(XE_IVYBRIDGE), \
	.has_l3_dpf = 1

#define G75_FEATURES  \
	GEN7_FEATURES, \
	.platform_engine_mask = BIT(XE_HW_ENGINE_RCS0) | \
	BIT(XE_HW_ENGINE_VCS0) | BIT(XE_HW_ENGINE_BCS0) | \
	BIT(XE_HW_ENGINE_VECS0), \
	.cpu_transcoder_mask = BIT(TRANSCODER_A) | BIT(TRANSCODER_B) | \
		BIT(TRANSCODER_C) | BIT(TRANSCODER_EDP), \
	.display.has_ddi = 1, \
	.display.has_fpga_dbg = 1, \
	.display.has_psr = 1, \
	.display.has_psr_hw_tracking = 1, \
	.display.has_dp_mst = 1, \
	.has_rc6p = 0 /* RC6p removed-by HSW */, \
	HSW_PIPE_OFFSETS, \
	.has_runtime_pm = 1

#define GEN8_FEATURES \
	G75_FEATURES, \
	GEN(8), \
	.has_logical_ring_contexts = 1, \
	.dma_mask_size = 39, \
	.ppgtt_type = INTEL_PPGTT_FULL, \
	.ppgtt_size = 48, \
	.has_64bit_reloc = 1

#define GEN9_DEFAULT_PAGE_SIZES \
	.page_sizes = I915_GTT_PAGE_SIZE_4K | \
		      I915_GTT_PAGE_SIZE_64K

#define GEN9_FEATURES \
	GEN8_FEATURES, \
	GEN(9), \
	GEN9_DEFAULT_PAGE_SIZES, \
	.display.has_dmc = 1, \
	.has_gt_uc = 1, \
	.display.has_hdcp = 1, \
	.display.has_ipc = 1, \
	.dbuf.size = 896 - 4, /* 4 blocks for bypass path allocation */ \
	.dbuf.slice_mask = BIT(DBUF_S1)

#define GEN10_FEATURES \
	GEN9_FEATURES, \
	GEN(10), \
	.dbuf.size = 1024 - 4, /* 4 blocks for bypass path allocation */ \
	.display.has_dsc = 1, \
	.has_coherent_ggtt = false, \
	GLK_COLORS

#define GEN11_DEFAULT_PAGE_SIZES \
	.page_sizes = I915_GTT_PAGE_SIZE_4K | \
		      I915_GTT_PAGE_SIZE_64K | \
		      I915_GTT_PAGE_SIZE_2M

#define GEN11_FEATURES \
	GEN10_FEATURES, \
	GEN11_DEFAULT_PAGE_SIZES, \
	.abox_mask = BIT(0), \
	.cpu_transcoder_mask = BIT(TRANSCODER_A) | BIT(TRANSCODER_B) | \
		BIT(TRANSCODER_C) | BIT(TRANSCODER_EDP) | \
		BIT(TRANSCODER_DSI_0) | BIT(TRANSCODER_DSI_1), \
	.pipe_offsets = { \
		[TRANSCODER_A] = PIPE_A_OFFSET, \
		[TRANSCODER_B] = PIPE_B_OFFSET, \
		[TRANSCODER_C] = PIPE_C_OFFSET, \
		[TRANSCODER_EDP] = PIPE_EDP_OFFSET, \
		[TRANSCODER_DSI_0] = PIPE_DSI0_OFFSET, \
		[TRANSCODER_DSI_1] = PIPE_DSI1_OFFSET, \
	}, \
	.trans_offsets = { \
		[TRANSCODER_A] = TRANSCODER_A_OFFSET, \
		[TRANSCODER_B] = TRANSCODER_B_OFFSET, \
		[TRANSCODER_C] = TRANSCODER_C_OFFSET, \
		[TRANSCODER_EDP] = TRANSCODER_EDP_OFFSET, \
		[TRANSCODER_DSI_0] = TRANSCODER_DSI0_OFFSET, \
		[TRANSCODER_DSI_1] = TRANSCODER_DSI1_OFFSET, \
	}, \
	GEN(11), \
	.dbuf.size = 2048, \
	.dbuf.slice_mask = BIT(DBUF_S1) | BIT(DBUF_S2), \
	.has_logical_ring_elsq = 1, \
	.color = { .degamma_lut_size = 33, .gamma_lut_size = 262145 }

#define GEN12_FEATURES \
	GEN11_FEATURES, \
	GEN(12), \
	.abox_mask = GENMASK(2, 1), \
	.pipe_mask = BIT(PIPE_A) | BIT(PIPE_B) | BIT(PIPE_C) | BIT(PIPE_D), \
	.cpu_transcoder_mask = BIT(TRANSCODER_A) | BIT(TRANSCODER_B) | \
		BIT(TRANSCODER_C) | BIT(TRANSCODER_D) | \
		BIT(TRANSCODER_DSI_0) | BIT(TRANSCODER_DSI_1), \
	.pipe_offsets = { \
		[TRANSCODER_A] = PIPE_A_OFFSET, \
		[TRANSCODER_B] = PIPE_B_OFFSET, \
		[TRANSCODER_C] = PIPE_C_OFFSET, \
		[TRANSCODER_D] = PIPE_D_OFFSET, \
		[TRANSCODER_DSI_0] = PIPE_DSI0_OFFSET, \
		[TRANSCODER_DSI_1] = PIPE_DSI1_OFFSET, \
	}, \
	.trans_offsets = { \
		[TRANSCODER_A] = TRANSCODER_A_OFFSET, \
		[TRANSCODER_B] = TRANSCODER_B_OFFSET, \
		[TRANSCODER_C] = TRANSCODER_C_OFFSET, \
		[TRANSCODER_D] = TRANSCODER_D_OFFSET, \
		[TRANSCODER_DSI_0] = TRANSCODER_DSI0_OFFSET, \
		[TRANSCODER_DSI_1] = TRANSCODER_DSI1_OFFSET, \
	}, \
	TGL_CURSOR_OFFSETS, \
	.has_global_mocs = 1, \
	.display.has_dsb = 1

static const struct intel_device_info tgl_info = {
	GEN12_FEATURES,
	PLATFORM(XE_TIGERLAKE),
	.display.has_modular_fia = 1,
	.platform_engine_mask =
		BIT(XE_HW_ENGINE_RCS0) | BIT(XE_HW_ENGINE_BCS0) |
		BIT(XE_HW_ENGINE_VECS0) | BIT(XE_HW_ENGINE_VCS0) |
		BIT(XE_HW_ENGINE_VCS2),
};

#define DGFX_FEATURES \
	.memory_regions = REGION_SMEM | REGION_LMEM | REGION_STOLEN_LMEM, \
	.has_llc = 0, \
	.has_snoop = 1, \
	.is_dgfx = 1

static const struct intel_device_info dg1_info __maybe_unused = {
	GEN12_FEATURES,
	DGFX_FEATURES,
	.graphics_rel = 10,
	PLATFORM(XE_DG1),
	.pipe_mask = BIT(PIPE_A) | BIT(PIPE_B) | BIT(PIPE_C) | BIT(PIPE_D),
	.require_force_probe = 1,
	.platform_engine_mask =
		BIT(XE_HW_ENGINE_RCS0) | BIT(XE_HW_ENGINE_BCS0) |
		BIT(XE_HW_ENGINE_VECS0) | BIT(XE_HW_ENGINE_VCS0) |
		BIT(XE_HW_ENGINE_VCS2),
	/* Wa_16011227922 */
	.ppgtt_size = 47,
};

#undef GEN
#undef PLATFORM

/*
 * Make sure any device matches here are from most specific to most
 * general.  For example, since the Quanta match is based on the subsystem
 * and subvendor IDs, we need it to come before the more general IVB
 * PCI ID matches, otherwise we'll use the wrong info struct above.
 */
static const struct pci_device_id pciidlist[] = {
	INTEL_TGL_12_GT2_IDS(&tgl_info),
	INTEL_DG1_IDS(&dg1_info),
	{0, 0, 0}
};
MODULE_DEVICE_TABLE(pci, pciidlist);

static void xe_pci_remove(struct pci_dev *pdev)
{
	struct xe_device *xe;

	xe = pci_get_drvdata(pdev);
	if (!xe) /* driver load aborted, nothing to cleanup */
		return;

	xe_device_remove(xe);
	pci_set_drvdata(pdev, NULL);
}

static int xe_pci_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	const struct intel_device_info *devinfo = (void *)ent->driver_data;
	struct xe_device *xe;
	int err;

	/* Only bind to function 0 of the device. Early generations
	 * used function 1 as a placeholder for multi-head. This causes
	 * us confusion instead, especially on the systems where both
	 * functions have the same PCI-ID!
	 */
	if (PCI_FUNC(pdev->devfn))
		return -ENODEV;

	/*
	 * apple-gmux is needed on dual GPU MacBook Pro
	 * to probe the panel if we're the inactive GPU.
	 */
	if (vga_switcheroo_client_probe_defer(pdev))
		return -EPROBE_DEFER;

	xe = xe_device_create(pdev, ent);
	if (IS_ERR(xe))
		return PTR_ERR(xe);

	/* TODO: Once we sort out includes, embed the intel_device_info
	 * directly in xe_device
	 */
	XE_BUG_ON(devinfo->graphics_rel % 10);
	xe->info.graphics_verx10 = devinfo->graphics_ver * 10 +
				   devinfo->graphics_rel / 10;
	xe->info.is_dgfx = devinfo->is_dgfx;
	xe->info.platform = devinfo->platform;
	to_gt(xe)->info.engine_mask = devinfo->platform_engine_mask;

	pci_set_drvdata(pdev, xe);
	err = pci_enable_device(pdev);
	if (err) {
		drm_dev_put(&xe->drm);
		return err;
	}

	pci_set_master(pdev);

	if (pci_enable_msi(pdev) < 0)
		drm_dbg(&xe->drm, "can't enable MSI");

	err = xe_device_probe(xe);
	if (err) {
		pci_disable_device(pdev);
		return err;
	}

	return 0;
}

static void xe_pci_shutdown(struct pci_dev *pdev)
{
	xe_device_shutdown(pdev_to_xe_device(pdev));
}

static struct pci_driver i915_pci_driver = {
	.name = DRIVER_NAME,
	.id_table = pciidlist,
	.probe = xe_pci_probe,
	.remove = xe_pci_remove,
	.shutdown = xe_pci_shutdown,
//	.driver.pm = &xe_pm_ops,
};

int i915_register_pci_driver(void)
{
	return pci_register_driver(&i915_pci_driver);
}

void i915_unregister_pci_driver(void)
{
	pci_unregister_driver(&i915_pci_driver);
}
