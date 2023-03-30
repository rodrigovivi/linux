/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef _XE_DISPLAY_H_
#define _XE_DISPLAY_H_

#include "xe_device.h"

struct drm_driver;

#if IS_ENABLED(CONFIG_DRM_XE_DISPLAY)

int xe_display_driver_probe_defer(struct pci_dev *pdev);
void xe_display_driver_set_hooks(struct drm_driver *driver);

int xe_display_create(struct xe_device *xe);

void xe_display_info_init(struct xe_device *xe);

int xe_display_init_nommio(struct xe_device *xe);
void xe_display_fini_nommio(struct drm_device *dev, void *dummy);

int xe_display_init_noirq(struct xe_device *xe);
void xe_display_fini_noirq(struct drm_device *dev, void *dummy);

int xe_display_init_noaccel(struct xe_device *xe);
void xe_display_fini_noaccel(struct drm_device *dev, void *dummy);

int xe_display_init(struct xe_device *xe);
void xe_display_unlink(struct xe_device *xe);

void xe_display_register(struct xe_device *xe);
void xe_display_unregister(struct xe_device *xe);
void xe_display_modset_driver_remove(struct xe_device *xe);

void xe_display_irq_handler(struct xe_device *xe, u32 master_ctl);
void xe_display_irq_enable(struct xe_device *xe, u32 gu_misc_iir);

void xe_display_irq_reset(struct xe_device *xe);
void xe_display_irq_postinstall(struct xe_device *xe, struct xe_gt *gt);

void xe_display_pm_suspend(struct xe_device *xe);
void xe_display_pm_suspend_late(struct xe_device *xe);
void xe_display_pm_resume_early(struct xe_device *xe);
void xe_display_pm_resume(struct xe_device *xe);

#else

static inline int xe_display_driver_probe_defer(struct pci_dev *pdev) { return 0; }

static inline void xe_display_driver_set_hooks(struct drm_driver *driver) { }

static inline int
xe_display_create(struct xe_device *xe) { return 0; }

static inline void xe_display_info_init(struct xe_device *xe) { }

static inline int
xe_display_enable(struct pci_dev *pdev, struct drm_driver *driver) { return 0; }

static inline int
xe_display_init_nommio(struct xe_device *xe) { return 0; }
static inline void xe_display_fini_nommio(struct drm_device *dev, void *dummy) {}

static inline int xe_display_init_noirq(struct xe_device *xe) { return 0; }

static inline void
xe_display_fini_noirq(struct drm_device *dev, void *dummy) {}

static inline int xe_display_init_noaccel(struct xe_device *xe) { return 0; }
static inline void xe_display_fini_noaccel(struct drm_device *dev, void *dummy) {}

static inline int xe_display_init(struct xe_device *xe) { return 0; }
static inline void xe_display_unlink(struct xe_device *xe) {}

static inline void xe_display_register(struct xe_device *xe) {}
static inline void xe_display_unregister(struct xe_device *xe) {}
static inline void xe_display_modset_driver_remove(struct xe_device *xe) {}

static inline void xe_display_irq_handler(struct xe_device *xe, u32 master_ctl) {}
static inline void xe_display_irq_enable(struct xe_device *xe, u32 gu_misc_iir) {}
static inline void xe_display_irq_reset(struct xe_device *xe) {}
static inline void xe_display_irq_postinstall(struct xe_device *xe, struct xe_gt *gt) {}

static inline void xe_display_pm_suspend(struct xe_device *xe) {}
static inline void xe_display_pm_suspend_late(struct xe_device *xe) {}
static inline void xe_display_pm_resume_early(struct xe_device *xe) {}
static inline void xe_display_pm_resume(struct xe_device *xe) {}

#endif /* CONFIG_DRM_XE_DISPLAY */
#endif /* _XE_DISPLAY_H_ */
