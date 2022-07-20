// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include "xe_pci.h"

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/vga_switcheroo.h>

#include <drm/drm_drv.h>
#include <drm/drm_color_mgmt.h>
#include <drm/xe_pciids.h>

#include "xe_drv.h"
#include "xe_device.h"
#include "xe_macros.h"
#include "xe_pm.h"
#include "xe_step.h"

#include "../i915/i915_reg.h"

typedef u32 intel_engine_mask_t;

#define DEV_INFO_FOR_EACH_FLAG(func) \
	func(require_force_probe); \
	func(is_dgfx); \
	/* Keep has_* in alphabetical order */ \


struct xe_subplatform_desc {
	enum xe_subplatform subplatform;
	const char *name;
	const u16 *pciidlist;
};

struct xe_device_desc {
	u8 graphics_ver;
	u8 graphics_rel;
	u8 media_ver;
	u8 media_rel;

	intel_engine_mask_t platform_engine_mask; /* Engines supported by the HW */

	enum xe_platform platform;
	const char *platform_name;
	const struct xe_subplatform_desc *subplatforms;

	u8 dma_mask_size; /* available DMA address bits */

	u8 gt; /* GT number, 0 if undefined */

#define DEFINE_FLAG(name) u8 name:1
	DEV_INFO_FOR_EACH_FLAG(DEFINE_FLAG);
#undef DEFINE_FLAG

	u8 vram_flags;
	bool has_tiles;
	u8 vm_max_level;
};

#define PLATFORM(x)		\
	.platform = (x),	\
	.platform_name = #x

#define NOP(x)	x

/* Keep in gen based order, and chronological order within a gen */

#define GEN12_FEATURES \
	.graphics_ver = 12, \
	.media_ver = 12, \
	.has_tiles = false, \
	.vm_max_level = 3, \
	.vram_flags = 0

static const struct xe_device_desc tgl_desc = {
	GEN12_FEATURES,
	PLATFORM(XE_TIGERLAKE),
	.platform_engine_mask =
		BIT(XE_HW_ENGINE_RCS0) | BIT(XE_HW_ENGINE_BCS0) |
		BIT(XE_HW_ENGINE_VECS0) | BIT(XE_HW_ENGINE_VCS0) |
		BIT(XE_HW_ENGINE_VCS2),
};

static const struct xe_device_desc adl_s_desc = {
	GEN12_FEATURES,
	PLATFORM(XE_ALDERLAKE_S),
	.platform_engine_mask =
		BIT(XE_HW_ENGINE_RCS0) | BIT(XE_HW_ENGINE_BCS0) |
		BIT(XE_HW_ENGINE_VECS0) | BIT(XE_HW_ENGINE_VCS0) |
		BIT(XE_HW_ENGINE_VCS2),
};

#define DGFX_FEATURES \
	.is_dgfx = 1

static const struct xe_device_desc dg1_desc = {
	GEN12_FEATURES,
	DGFX_FEATURES,
	.graphics_rel = 10,
	PLATFORM(XE_DG1),
	.require_force_probe = true,
	.platform_engine_mask =
		BIT(XE_HW_ENGINE_RCS0) | BIT(XE_HW_ENGINE_BCS0) |
		BIT(XE_HW_ENGINE_VECS0) | BIT(XE_HW_ENGINE_VCS0) |
		BIT(XE_HW_ENGINE_VCS2),
};

#define XE_HP_FEATURES \
	.graphics_ver = 12, \
	.graphics_rel = 50, \
	.dma_mask_size = 46, \
	.has_tiles = false, \
	.vm_max_level = 3

#define XE_HPM_FEATURES \
	.media_ver = 12, \
	.media_rel = 50

static const u16 dg2_g10_ids[] = { XE_DG2_G10_IDS(NOP), 0 };
static const u16 dg2_g11_ids[] = { XE_DG2_G11_IDS(NOP), 0 };
static const u16 dg2_g12_ids[] = { XE_DG2_G12_IDS(NOP), 0 };

static const struct xe_device_desc ats_m_desc = {
	XE_HP_FEATURES,
	XE_HPM_FEATURES,
	DGFX_FEATURES,
	.graphics_rel = 55,
	.media_rel = 55,
	PLATFORM(XE_DG2),
	.subplatforms = (const struct xe_subplatform_desc[]) {
		{ XE_SUBPLATFORM_DG2_G10, "G10", dg2_g10_ids },
		{ XE_SUBPLATFORM_DG2_G11, "G11", dg2_g11_ids },
		{ XE_SUBPLATFORM_DG2_G12, "G12", dg2_g12_ids },
		{ }
	},
	.platform_engine_mask =
		BIT(XE_HW_ENGINE_RCS0) | BIT(XE_HW_ENGINE_BCS0) |
		BIT(XE_HW_ENGINE_VECS0) | BIT(XE_HW_ENGINE_VECS1) |
		BIT(XE_HW_ENGINE_VCS0) | BIT(XE_HW_ENGINE_VCS2) |
		BIT(XE_HW_ENGINE_CCS0) | BIT(XE_HW_ENGINE_CCS1) |
		BIT(XE_HW_ENGINE_CCS2) | BIT(XE_HW_ENGINE_CCS3),
	.require_force_probe = true,
	.vram_flags = XE_VRAM_FLAGS_NEED64K,
};

#define PVC_ENGINES \
	BIT(XE_HW_ENGINE_BCS0) | BIT(XE_HW_ENGINE_BCS1) | \
	BIT(XE_HW_ENGINE_BCS2) | BIT(XE_HW_ENGINE_BCS3) | \
	BIT(XE_HW_ENGINE_BCS4) | BIT(XE_HW_ENGINE_BCS5) | \
	BIT(XE_HW_ENGINE_BCS6) | BIT(XE_HW_ENGINE_BCS7) | \
	BIT(XE_HW_ENGINE_BCS8) | \
	BIT(XE_HW_ENGINE_VCS0) | BIT(XE_HW_ENGINE_VCS1) | \
	BIT(XE_HW_ENGINE_VCS2) | \
	BIT(XE_HW_ENGINE_CCS0) | BIT(XE_HW_ENGINE_CCS1) | \
	BIT(XE_HW_ENGINE_CCS2) | BIT(XE_HW_ENGINE_CCS3)

static const struct xe_device_desc pvc_desc = {
	XE_HP_FEATURES,
	XE_HPM_FEATURES,
	DGFX_FEATURES,
	PLATFORM(XE_PVC),
	.graphics_rel = 60,
	.media_rel = 60,
	.platform_engine_mask = PVC_ENGINES,
	.require_force_probe = true,
	.vram_flags = XE_VRAM_FLAGS_NEED64K,
	.dma_mask_size = 52,
	.has_tiles = true,
	.vm_max_level = 4,
};

#undef PLATFORM

#define INTEL_VGA_DEVICE(id, info) {			\
	PCI_DEVICE(PCI_VENDOR_ID_INTEL, id),		\
	PCI_BASE_CLASS_DISPLAY << 16, 0xff << 16,	\
	(unsigned long) info }

/*
 * Make sure any device matches here are from most specific to most
 * general.  For example, since the Quanta match is based on the subsystem
 * and subvendor IDs, we need it to come before the more general IVB
 * PCI ID matches, otherwise we'll use the wrong info struct above.
 */
static const struct pci_device_id pciidlist[] = {
	XE_TGL_GT2_IDS(INTEL_VGA_DEVICE, &tgl_desc),
	XE_DG1_IDS(INTEL_VGA_DEVICE, &dg1_desc),
	XE_ATS_M_IDS(INTEL_VGA_DEVICE, &ats_m_desc),
	XE_DG2_IDS(INTEL_VGA_DEVICE, &ats_m_desc), /* TODO: switch to proper dg2_desc */
	XE_ADLS_IDS(INTEL_VGA_DEVICE, &adl_s_desc),
	XE_PVC_IDS(INTEL_VGA_DEVICE, &pvc_desc),
	{ }
};
MODULE_DEVICE_TABLE(pci, pciidlist);

#undef INTEL_VGA_DEVICE

static const struct xe_subplatform_desc *
subplatform_get(const struct xe_device *xe, const struct xe_device_desc *desc)
{
	const struct xe_subplatform_desc *sp;
	const u16 *id;

	for (sp = desc->subplatforms; sp->subplatform; sp++)
		for (id = sp->pciidlist; *id; id++)
			if (*id == xe->info.devid)
				return sp;

	return NULL;
}

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
	const struct xe_device_desc *desc = (void *)ent->driver_data;
	const struct xe_subplatform_desc *spd;
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

	xe->info.graphics_verx100 = desc->graphics_ver * 100 +
				   desc->graphics_rel;
	xe->info.is_dgfx = desc->is_dgfx;
	xe->info.platform = desc->platform;
	xe->info.dma_mask_size = desc->dma_mask_size;
	xe->info.vram_flags = desc->vram_flags;
	to_gt(xe)->info.engine_mask = desc->platform_engine_mask;
	xe->info.tile_count = desc->has_tiles ? 1 : 0;
	xe->info.vm_max_level = desc->vm_max_level;

	spd = subplatform_get(xe, desc);
	xe->info.subplatform = spd ? spd->subplatform : XE_SUBPLATFORM_NONE;
	xe->info.step = xe_step_get(xe);

	drm_dbg(&xe->drm, "%s %s %04x:%04x dgfx:%d gfx100:%d dma_m_s:%d tc:%d",
		desc->platform_name, spd ? spd->name : "",
		xe->info.devid, xe->info.revid,
		xe->info.is_dgfx, xe->info.graphics_verx100,
		xe->info.dma_mask_size, xe->info.tile_count);

	drm_dbg(&xe->drm, "Stepping = (G:%s, M:%s, D:%s)\n",
		xe_step_name(xe->info.step.graphics),
		xe_step_name(xe->info.step.media),
		xe_step_name(xe->info.step.display));

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

#ifdef CONFIG_PM_SLEEP
static int xe_pci_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	int err;

	err = xe_pm_suspend(pdev_to_xe_device(pdev));
	if (err)
		return err;

	pci_save_state(pdev);
	pci_disable_device(pdev);

	err = pci_set_power_state(pdev, PCI_D3hot);
	if (err)
		return err;

	return 0;
}

static int xe_pci_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	int err;

	err = pci_set_power_state(pdev, PCI_D0);
	if (err)
		return err;

	pci_restore_state(pdev);

	err = pci_enable_device(pdev);
	if (err)
		return err;

	pci_set_master(pdev);

	err = xe_pm_resume(pdev_to_xe_device(pdev));
	if (err)
		return err;

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(xe_pm_ops, xe_pci_suspend, xe_pci_resume);

static struct pci_driver i915_pci_driver = {
	.name = DRIVER_NAME,
	.id_table = pciidlist,
	.probe = xe_pci_probe,
	.remove = xe_pci_remove,
	.shutdown = xe_pci_shutdown,
	.driver.pm = &xe_pm_ops,
};

int xe_register_pci_driver(void)
{
	return pci_register_driver(&i915_pci_driver);
}

void xe_unregister_pci_driver(void)
{
	pci_unregister_driver(&i915_pci_driver);
}
