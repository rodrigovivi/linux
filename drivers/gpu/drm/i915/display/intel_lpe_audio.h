/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_LPE_AUDIO_H__
#define __INTEL_LPE_AUDIO_H__

#include <linux/types.h>

enum port;
enum transcoder;
struct drm_i915_private;

#ifdef I915
int  intel_lpe_audio_init(struct drm_i915_private *dev_priv);
void intel_lpe_audio_teardown(struct drm_i915_private *dev_priv);
void intel_lpe_audio_irq_handler(struct drm_i915_private *dev_priv);
void intel_lpe_audio_notify(struct drm_i915_private *dev_priv,
			    enum transcoder cpu_transcoder, enum port port,
			    const void *eld, int ls_clock, bool dp_output);
#else
#define intel_lpe_audio_init(xe) (-ENODEV)
#define intel_lpe_audio_teardown(xe) BUG_ON(1)
#define intel_lpe_audio_irq_handler(xe) do { } while (0)
#define intel_lpe_audio_notify(xe, a, b, c, d, e) do { } while (0)

#endif

#endif /* __INTEL_LPE_AUDIO_H__ */
