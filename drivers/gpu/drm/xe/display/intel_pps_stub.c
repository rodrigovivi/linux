#include "intel_pps.h"
#include "intel_display.h"

intel_wakeref_t intel_pps_lock(struct intel_dp *intel_dp)
{
        return 0;
}

intel_wakeref_t intel_pps_unlock(struct intel_dp *intel_dp, intel_wakeref_t wakeref)
{
        return wakeref;
}

void intel_pps_backlight_on(struct intel_dp *intel_dp) {}
void intel_pps_backlight_off(struct intel_dp *intel_dp) {}
void intel_pps_backlight_power(struct intel_connector *connector, bool enable) {}

bool intel_pps_vdd_on_unlocked(struct intel_dp *intel_dp) { return false; }
void intel_pps_vdd_off_unlocked(struct intel_dp *intel_dp, bool sync) {}
void intel_pps_on_unlocked(struct intel_dp *intel_dp) {}
void intel_pps_off_unlocked(struct intel_dp *intel_dp) {}
void intel_pps_check_power_unlocked(struct intel_dp *intel_dp) {}

void intel_pps_vdd_on(struct intel_dp *intel_dp) {}
void intel_pps_on(struct intel_dp *intel_dp) {}
void intel_pps_off(struct intel_dp *intel_dp) {}
void intel_pps_vdd_off_sync(struct intel_dp *intel_dp) {}
bool intel_pps_have_panel_power_or_vdd(struct intel_dp *intel_dp) { return false; }
void intel_pps_wait_power_cycle(struct intel_dp *intel_dp) {}

bool intel_pps_init(struct intel_dp *intel_dp) { return false; }
void intel_pps_init_late(struct intel_dp *intel_dp) {}
void intel_pps_encoder_reset(struct intel_dp *intel_dp) {}
void intel_pps_reset_all(struct drm_i915_private *i915) {}

void vlv_pps_init(struct intel_encoder *encoder,
		  const struct intel_crtc_state *crtc_state) {}

void intel_pps_unlock_regs_wa(struct drm_i915_private *i915) {}
void intel_pps_setup(struct drm_i915_private *i915) {}

void assert_pps_unlocked(struct drm_i915_private *i915, enum pipe pipe) {}

