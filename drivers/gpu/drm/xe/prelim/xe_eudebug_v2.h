/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef _PRELIM_XE_EUDEBUG_V2_H_
#define _PRELIM_XE_EUDEBUG_V2_H_

struct drm_device;
struct drm_file;
struct xe_device;

#if IS_ENABLED(CONFIG_PRELIM_DRM_XE_EUDEBUG)

int prelim_v2_xe_eudebug_connect_ioctl(struct drm_device *dev, void *data,
				       struct drm_file *file);
void prelim_v2_xe_eudebug_init(struct xe_device *xe);

#else

static inline void prelim_v2_xe_eudebug_init(struct xe_device *xe) { }

#endif /* CONFIG_PRELIM_DRM_XE_EUDEBUG */

#endif /* _PRELIM_XE_EUDEBUG_V2_H_ */
