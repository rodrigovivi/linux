/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __I915_IRQ_H__
#define __I915_IRQ_H__

#include <linux/ktime.h>
#include <linux/types.h>

enum pipe;
struct drm_crtc;
struct drm_device;
struct drm_display_mode;
struct drm_i915_private;
struct intel_crtc;
struct intel_encoder;
struct intel_uncore;

void intel_display_irq_init(struct drm_i915_private *dev_priv);
void intel_irq_fini(struct drm_i915_private *dev_priv);
int intel_irq_install(struct drm_i915_private *dev_priv);
void intel_irq_uninstall(struct drm_i915_private *dev_priv);

u32 i915_pipestat_enable_mask(struct drm_i915_private *dev_priv,
			      enum pipe pipe);
void
i915_enable_pipestat(struct drm_i915_private *dev_priv, enum pipe pipe,
		     u32 status_mask);

void
i915_disable_pipestat(struct drm_i915_private *dev_priv, enum pipe pipe,
		      u32 status_mask);

static inline void valleyview_enable_display_irqs(struct drm_i915_private *dev_priv)
{
}
static inline void valleyview_disable_display_irqs(struct drm_i915_private *dev_priv)
{
}

void ilk_update_display_irq(struct drm_i915_private *dev_priv,
			    u32 interrupt_mask, u32 enabled_irq_mask);
void ilk_enable_display_irq(struct drm_i915_private *i915, u32 bits);
void ilk_disable_display_irq(struct drm_i915_private *i915, u32 bits);

void bdw_update_port_irq(struct drm_i915_private *i915, u32 interrupt_mask, u32 enabled_irq_mask);
void bdw_enable_pipe_irq(struct drm_i915_private *i915, enum pipe pipe, u32 bits);
void bdw_disable_pipe_irq(struct drm_i915_private *i915, enum pipe pipe, u32 bits);

void ibx_display_interrupt_update(struct drm_i915_private *i915,
				  u32 interrupt_mask, u32 enabled_irq_mask);
void ibx_enable_display_interrupt(struct drm_i915_private *i915, u32 bits);
void ibx_disable_display_interrupt(struct drm_i915_private *i915, u32 bits);

void gen5_enable_gt_irq(struct drm_i915_private *dev_priv, u32 mask);
void gen5_disable_gt_irq(struct drm_i915_private *dev_priv, u32 mask);
void gen11_reset_rps_interrupts(struct drm_i915_private *dev_priv);
void gen6_reset_rps_interrupts(struct drm_i915_private *dev_priv);
void gen6_enable_rps_interrupts(struct drm_i915_private *dev_priv);
void gen6_disable_rps_interrupts(struct drm_i915_private *dev_priv);
void gen6_rps_reset_ei(struct drm_i915_private *dev_priv);
u32 gen6_sanitize_rps_pm_mask(const struct drm_i915_private *i915, u32 mask);

void intel_runtime_pm_disable_interrupts(struct drm_i915_private *dev_priv);
void intel_runtime_pm_enable_interrupts(struct drm_i915_private *dev_priv);
bool intel_irqs_enabled(struct drm_i915_private *dev_priv);
void intel_synchronize_irq(struct drm_i915_private *i915);
void intel_synchronize_hardirq(struct drm_i915_private *i915);

int intel_get_crtc_scanline(struct intel_crtc *crtc);
void gen8_irq_power_well_post_enable(struct drm_i915_private *dev_priv,
				     u8 pipe_mask);
void gen8_irq_power_well_pre_disable(struct drm_i915_private *dev_priv,
				     u8 pipe_mask);
u32 gen8_de_pipe_underrun_mask(struct drm_i915_private *dev_priv);

bool intel_crtc_get_vblank_timestamp(struct drm_crtc *crtc, int *max_error,
				     ktime_t *vblank_time, bool in_vblank_irq);

u32 i915_get_vblank_counter(struct drm_crtc *crtc);
u32 g4x_get_vblank_counter(struct drm_crtc *crtc);

int i8xx_enable_vblank(struct drm_crtc *crtc);
int i915gm_enable_vblank(struct drm_crtc *crtc);
int i965_enable_vblank(struct drm_crtc *crtc);
int ilk_enable_vblank(struct drm_crtc *crtc);
int bdw_enable_vblank(struct drm_crtc *crtc);
void i8xx_disable_vblank(struct drm_crtc *crtc);
void i915gm_disable_vblank(struct drm_crtc *crtc);
void i965_disable_vblank(struct drm_crtc *crtc);
void ilk_disable_vblank(struct drm_crtc *crtc);
void bdw_disable_vblank(struct drm_crtc *crtc);

void gen11_display_irq_postinstall(struct drm_i915_private *dev_priv);
void gen11_display_irq_reset(struct drm_i915_private *dev_priv);
void gen11_display_irq_handler(struct drm_i915_private *dev_priv);

#endif /* __I915_IRQ_H__ */
