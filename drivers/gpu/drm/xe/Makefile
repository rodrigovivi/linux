# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the drm device driver.  This driver provides support for the
# Direct Rendering Infrastructure (DRI) in XFree86 4.1.0 and higher.

# Add a set of useful warning flags and enable -Werror for CI to prevent
# trivial mistakes from creeping in. We have to do this piecemeal as we reject
# any patch that isn't warning clean, so turning on -Wall -Wextra (or W=1) we
# need to filter out dubious warnings.  Still it is our interest
# to keep running locally with W=1 C=1 until we are completely clean.
#
# Note the danger in using -Wall -Wextra is that when CI updates gcc we
# will most likely get a sudden build breakage... Hopefully we will fix
# new warnings before CI updates!
subdir-ccflags-y := -Wall -Wextra
subdir-ccflags-y += $(call cc-disable-warning, unused-parameter)
subdir-ccflags-y += $(call cc-disable-warning, type-limits)
subdir-ccflags-y += $(call cc-disable-warning, missing-field-initializers)
subdir-ccflags-y += $(call cc-disable-warning, unused-but-set-variable)
# clang warnings
subdir-ccflags-y += $(call cc-disable-warning, sign-compare)
subdir-ccflags-y += $(call cc-disable-warning, sometimes-uninitialized)
subdir-ccflags-y += $(call cc-disable-warning, initializer-overrides)
subdir-ccflags-y += $(call cc-disable-warning, frame-address)
subdir-ccflags-$(CONFIG_DRM_XE_WERROR) += -Werror

# Fine grained warnings disable
CFLAGS_xe_pci.o = $(call cc-disable-warning, override-init)

subdir-ccflags-y += -I$(srctree)/$(src)

# Please keep these build lists sorted!

# core driver code

xe-y += xe_bb.o \
	xe_bo.o \
	xe_bo_evict.o \
	xe_debugfs.o \
	xe_devcoredump.o \
	xe_device.o \
	xe_dma_buf.o \
	xe_engine.o \
	xe_exec.o \
	xe_execlist.o \
	xe_force_wake.o \
	xe_ggtt.o \
	xe_gt.o \
	xe_gt_clock.o \
	xe_gt_debugfs.o \
	xe_gt_mcr.o \
	xe_gt_pagefault.o \
	xe_gt_sysfs.o \
	xe_gt_tlb_invalidation.o \
	xe_gt_topology.o \
	xe_guc.o \
	xe_guc_ads.o \
	xe_guc_ct.o \
	xe_guc_debugfs.o \
	xe_guc_hwconfig.o \
	xe_guc_log.o \
	xe_guc_pc.o \
	xe_guc_submit.o \
	xe_hw_engine.o \
	xe_hw_fence.o \
	xe_huc.o \
	xe_huc_debugfs.o \
	xe_irq.o \
	xe_lrc.o \
	xe_migrate.o \
	xe_mmio.o \
	xe_mocs.o \
	xe_module.o \
	xe_pat.o \
	xe_pci.o \
	xe_pcode.o \
	xe_pm.o \
	xe_preempt_fence.o \
	xe_pt.o \
	xe_pt_walk.o \
	xe_query.o \
	xe_reg_sr.o \
	xe_reg_whitelist.o \
	xe_rtp.o \
	xe_ring_ops.o \
	xe_sa.o \
	xe_sched_job.o \
	xe_step.o \
	xe_sync.o \
	xe_trace.o \
	xe_ttm_sys_mgr.o \
	xe_ttm_stolen_mgr.o \
	xe_ttm_vram_mgr.o \
	xe_tuning.o \
	xe_uc.o \
	xe_uc_debugfs.o \
	xe_uc_fw.o \
	xe_vm.o \
	xe_vm_madvise.o \
	xe_wait_user_fence.o \
	xe_wa.o \
	xe_wopcm.o

# i915 Display compat #defines and #includes
subdir-ccflags-$(CONFIG_DRM_XE_DISPLAY) += \
	-I$(srctree)/$(src)/display/ext \
	-I$(srctree)/$(src)/compat-i915-headers \
	-I$(srctree)/drivers/gpu/drm/xe/display/ \
	-I$(srctree)/drivers/gpu/drm/i915/display/ \
	-Ddrm_i915_gem_object=xe_bo \
	-Ddrm_i915_private=xe_device

CFLAGS_i915-display/intel_fbdev.o = $(call cc-disable-warning, override-init)

# Rule to build display code shared with i915
$(obj)/i915-display/%.o: $(srctree)/drivers/gpu/drm/i915/display/%.c FORCE
	$(call cmd,force_checksrc)
	$(call if_changed_rule,cc_o_c)

# Display code specific to xe
xe-$(CONFIG_DRM_XE_DISPLAY) += \
	xe_display.o \
	display/xe_fb_pin.o \
	display/xe_hdcp_gsc.o \
	display/xe_plane_initial.o \
	display/xe_display_rps.o \
	display/ext/i915_irq.o \
	display/ext/intel_clock_gating.o \
	display/ext/intel_device_info.o \
	display/ext/intel_dram.o \
	display/ext/intel_pch.o

# Display code shared with i915
xe-$(CONFIG_DRM_XE_DISPLAY) += \
	i915-display/icl_dsi.o \
	i915-display/intel_atomic.o \
	i915-display/intel_atomic_plane.o \
	i915-display/intel_audio.o \
	i915-display/intel_backlight.o \
	i915-display/intel_bios.o \
	i915-display/intel_bw.o \
	i915-display/intel_cdclk.o \
	i915-display/intel_color.o \
	i915-display/intel_combo_phy.o \
	i915-display/intel_connector.o \
	i915-display/intel_crtc.o \
	i915-display/intel_crtc_state_dump.o \
	i915-display/intel_cursor.o \
	i915-display/intel_cx0_phy.o \
	i915-display/intel_ddi.o \
	i915-display/intel_ddi_buf_trans.o \
	i915-display/intel_display.o \
	i915-display/intel_display_driver.o \
	i915-display/intel_display_debugfs.o \
	i915-display/intel_display_power.o \
	i915-display/intel_display_power_map.o \
	i915-display/intel_display_power_well.o \
	i915-display/intel_display_trace.o \
	i915-display/intel_dkl_phy.o \
	i915-display/intel_dmc.o \
	i915-display/intel_dp.o \
	i915-display/intel_dp_aux.o \
	i915-display/intel_dp_aux_backlight.o \
	i915-display/intel_dp_hdcp.o \
	i915-display/intel_dp_link_training.o \
	i915-display/intel_dp_mst.o \
	i915-display/intel_dpll.o \
	i915-display/intel_dpll_mgr.o \
	i915-display/intel_dpt.o \
	i915-display/intel_drrs.o \
	i915-display/intel_dsb.o \
	i915-display/intel_dsi.o \
	i915-display/intel_dsi_dcs_backlight.o \
	i915-display/intel_dsi_vbt.o \
	i915-display/intel_fb.o \
	i915-display/intel_fbc.o \
	i915-display/intel_fdi.o \
	i915-display/intel_fifo_underrun.o \
	i915-display/intel_frontbuffer.o \
	i915-display/intel_global_state.o \
	i915-display/intel_gmbus.o \
	i915-display/intel_hdcp.o \
	i915-display/intel_hdmi.o \
	i915-display/intel_hotplug.o \
	i915-display/intel_hotplug_irq.o \
	i915-display/intel_hti.o \
	i915-display/intel_lspcon.o \
	i915-display/intel_modeset_setup.o \
	i915-display/intel_modeset_verify.o \
	i915-display/intel_panel.o \
	i915-display/intel_pipe_crc.o \
	i915-display/intel_pps.o \
	i915-display/intel_psr.o \
	i915-display/intel_qp_tables.o \
	i915-display/intel_quirks.o \
	i915-display/intel_snps_phy.o \
	i915-display/intel_tc.o \
	i915-display/intel_vblank.o \
	i915-display/intel_vdsc.o \
	i915-display/intel_vga.o \
	i915-display/intel_vrr.o \
	i915-display/intel_wm.o \
	i915-display/skl_scaler.o \
	i915-display/skl_universal_plane.o \
	i915-display/skl_watermark.o

ifeq ($(CONFIG_ACPI),y)
	xe-$(CONFIG_DRM_XE_DISPLAY) += \
		i915-display/intel_acpi.o \
		i915-display/intel_opregion.o
endif

ifeq ($(CONFIG_DRM_FBDEV_EMULATION),y)
	xe-$(CONFIG_DRM_XE_DISPLAY) += i915-display/intel_fbdev.o
endif

obj-$(CONFIG_DRM_XE) += xe.o
obj-$(CONFIG_DRM_XE_KUNIT_TEST) += tests/

# header test
hdrtest_find_args := -not -path xe_rtp_helpers.h
ifneq ($(CONFIG_DRM_XE_DISPLAY),y)
	hdrtest_find_args += -not -path display/\* -not -path compat-i915-headers/\* -not -path xe_display.h
endif

always-$(CONFIG_DRM_XE_WERROR) += \
	$(patsubst %.h,%.hdrtest, $(shell cd $(srctree)/$(src) && find * -name '*.h' $(hdrtest_find_args)))

quiet_cmd_hdrtest = HDRTEST $(patsubst %.hdrtest,%.h,$@)
      cmd_hdrtest = $(CC) -DHDRTEST $(filter-out $(CFLAGS_GCOV), $(c_flags)) $(cflags-display) -S -o /dev/null -x c /dev/null -include $<; touch $@

$(obj)/%.hdrtest: $(src)/%.h FORCE
	$(call if_changed_dep,hdrtest)
