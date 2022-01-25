/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#ifndef _XE_DEVICE_H_
#define _XE_DEVICE_H_

#include <linux/pci.h>

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_mm.h>
#include <drm/ttm/ttm_device.h>

#include "xe_ggtt.h"
#include "xe_hw_engine.h"
#include "xe_force_wake_types.h"

#define XE_BO_INVALID_OFFSET	LONG_MAX

#define GRAPHICS_VER(xe) ((xe)->info.graphics_verx10 / 10)
#define GRAPHICS_VERx10(xe) ((xe)->info.graphics_verx10)
#define IS_DGFX(xe) ((xe)->info.is_dgfx)

/* Keep in gen based order, and chronological order within a gen */
enum xe_platform {
	XE_PLATFORM_UNINITIALIZED = 0,
	/* gen2 */
	XE_I830,
	XE_I845G,
	XE_I85X,
	XE_I865G,
	/* gen3 */
	XE_I915G,
	XE_I915GM,
	XE_I945G,
	XE_I945GM,
	XE_G33,
	XE_PINEVIEW,
	/* gen4 */
	XE_I965G,
	XE_I965GM,
	XE_G45,
	XE_GM45,
	/* gen5 */
	XE_IRONLAKE,
	/* gen6 */
	XE_SANDYBRIDGE,
	/* gen7 */
	XE_IVYBRIDGE,
	XE_VALLEYVIEW,
	XE_HASWELL,
	/* gen8 */
	XE_BROADWELL,
	XE_CHERRYVIEW,
	/* gen9 */
	XE_SKYLAKE,
	XE_BROXTON,
	XE_KABYLAKE,
	XE_GEMINILAKE,
	XE_COFFEELAKE,
	XE_COMETLAKE,
	/* gen10 */
	XE_CANNONLAKE,
	/* gen11 */
	XE_ICELAKE,
	XE_ELKHARTLAKE,
	XE_JASPERLAKE,
	/* gen12 */
	XE_TIGERLAKE,
	XE_ROCKETLAKE,
	XE_DG1,
	XE_ALDERLAKE_S,
	XE_ALDERLAKE_P,
	XE_XEHPSDV,
	XE_DG2,
	XE_MAX_PLATFORMS
};

struct xe_ttm_vram_mgr {
	struct ttm_resource_manager manager;
	struct drm_mm mm;
	spinlock_t lock;
	atomic64_t usage;
};

struct xe_ttm_gtt_mgr {
	struct ttm_resource_manager manager;
	atomic64_t used;
};

struct xe_device {
	struct drm_device drm;

	struct {
		uint8_t graphics_verx10;
		bool is_dgfx;
		enum xe_platform platform;
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

static inline struct xe_device *to_xe_device(const struct drm_device *dev)
{
	return container_of(dev, struct xe_device, drm);
}

static inline struct xe_device *pdev_to_xe_device(struct pci_dev *pdev)
{
	return pci_get_drvdata(pdev);
}

static inline struct xe_device *ttm_to_xe_device(struct ttm_device *ttm)
{
	return container_of(ttm, struct xe_device, ttm);
}

struct xe_device *xe_device_create(struct pci_dev *pdev,
				   const struct pci_device_id *ent);
int xe_device_probe(struct xe_device *xe);
void xe_device_remove(struct xe_device *xe);
void xe_device_shutdown(struct xe_device *xe);

struct xe_hw_engine *xe_device_hw_engine(struct xe_device *xe,
					 enum xe_engine_class class,
					 uint16_t instance);

static inline struct xe_file *to_xe_file(const struct drm_file *file)
{
	return file->driver_priv;
}

int xe_irq_install(struct xe_device *xe);
void xe_irq_uninstall(struct xe_device *xe);

/* TTM memory managers */
int xe_ttm_vram_mgr_init(struct xe_device *xe);
void xe_ttm_vram_mgr_fini(struct xe_device *xe);

int xe_ttm_gtt_mgr_init(struct xe_device *xe, uint64_t gtt_size);
void xe_ttm_gtt_mgr_fini(struct xe_device *xe);
#endif /* _XE_DEVICE_H_ */
