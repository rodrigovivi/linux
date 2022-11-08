/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef _INTEL_PCH_DISPLAY_H_
#define _INTEL_PCH_DISPLAY_H_

#include <linux/types.h>

enum pipe;
struct drm_i915_private;
struct intel_atomic_state;
struct intel_crtc;
struct intel_crtc_state;
struct intel_link_m_n;

#ifdef I915
bool intel_has_pch_trancoder(struct drm_i915_private *i915,
			     enum pipe pch_transcoder);
enum pipe intel_crtc_pch_transcoder(struct intel_crtc *crtc);

void ilk_pch_pre_enable(struct intel_atomic_state *state,
			struct intel_crtc *crtc);
void ilk_pch_enable(struct intel_atomic_state *state,
		    struct intel_crtc *crtc);
void ilk_pch_disable(struct intel_atomic_state *state,
		     struct intel_crtc *crtc);
void ilk_pch_post_disable(struct intel_atomic_state *state,
			  struct intel_crtc *crtc);
void ilk_pch_get_config(struct intel_crtc_state *crtc_state);

void lpt_pch_enable(struct intel_atomic_state *state,
		    struct intel_crtc *crtc);
void lpt_pch_disable(struct intel_atomic_state *state,
		     struct intel_crtc *crtc);
void lpt_pch_get_config(struct intel_crtc_state *crtc_state);

void intel_pch_transcoder_get_m1_n1(struct intel_crtc *crtc,
				    struct intel_link_m_n *m_n);
void intel_pch_transcoder_get_m2_n2(struct intel_crtc *crtc,
				    struct intel_link_m_n *m_n);

void intel_pch_sanitize(struct drm_i915_private *i915);
#else
#define intel_has_pch_trancoder(xe, pipe) (xe && pipe && 0)
#define intel_crtc_pch_transcoder(crtc) ((crtc)->pipe)
#define ilk_pch_pre_enable(state, crtc) do { } while (0)
#define ilk_pch_enable(state, crtc) do { } while (0)
#define ilk_pch_disable(state, crtc) do { } while (0)
#define ilk_pch_post_disable(state, crtc) do { } while (0)
#define ilk_pch_get_config(crtc) do { } while (0)
#define lpt_pch_enable(state, crtc) do { } while (0)
#define lpt_pch_disable(state, crtc) do { } while (0)
#define lpt_pch_get_config(crtc) do { } while (0)
#define intel_pch_transcoder_get_m1_n1(crtc, m_n) memset((m_n), 0, sizeof(*m_n))
#define intel_pch_transcoder_get_m2_n2(crtc, m_n) memset((m_n), 0, sizeof(*m_n))
#define intel_pch_sanitize(xe) do { } while (0)
#endif

#endif
