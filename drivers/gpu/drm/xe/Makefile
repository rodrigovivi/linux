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
# making these call cc-disable-warning breaks when trying to build xe.mod.o
# by calling make M=drivers/gpu/drm/xe. This doesn't happen in upstream tree,
# so it was somehow fixed by the changes in the build system. Move it back to
# $(call cc-disable-warning, ...) after rebase.
subdir-ccflags-y += -Wno-unused-parameter
subdir-ccflags-y += -Wno-type-limits
#subdir-ccflags-y += $(call cc-disable-warning, unused-parameter)
#subdir-ccflags-y += $(call cc-disable-warning, type-limits)
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
	xe_pci.o \
	xe_pcode.o \
	xe_pm.o \
	xe_preempt_fence.o \
	xe_pt.o \
	xe_query.o \
	xe_reg_sr.o \
	xe_rtp.o \
	xe_ring_ops.o \
	xe_sa.o \
	xe_sched_job.o \
	xe_step.o \
	xe_sync.o \
	xe_trace.o \
	xe_ttm_gtt_mgr.o \
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

# i915 compat #defines and #includes
subdir-ccflags-$(CONFIG_DRM_XE_DISPLAY) += -I$(srctree)/$(src)/display/ext \
	-Ddrm_i915_gem_object=xe_bo \
	-Ddrm_i915_private=xe_device

CFLAGS_display/intel_fbdev.o = $(call cc-disable-warning, override-init)

# Display..
xe-$(CONFIG_DRM_XE_DISPLAY) += \
	display/icl_dsi.o \
	display/intel_acpi.o \
	display/intel_atomic.o \
	display/intel_atomic_plane.o \
	display/intel_audio.o \
	display/intel_backlight.o \
	display/intel_bios.o \
	display/intel_bw.o \
	display/intel_cdclk.o \
	display/intel_color.o \
	display/intel_combo_phy.o \
	display/intel_connector.o \
	display/intel_crtc_state_dump.o \
	display/intel_crtc.o \
	display/intel_cursor.o \
	display/intel_ddi_buf_trans.o \
	display/intel_ddi.o \
	display/intel_display.o \
	display/intel_display_debugfs.o \
	display/intel_display_power.o \
	display/intel_display_power_map.o \
	display/intel_display_power_well.o \
	display/intel_display_trace.o \
	display/intel_dmc.o \
	display/intel_dp_aux_backlight.o \
	display/intel_dp_aux.o \
	display/intel_dp.o \
	display/intel_dp_hdcp.o \
	display/intel_dp_link_training.o \
	display/intel_dpll.o \
	display/intel_dpll_mgr.o \
	display/intel_dp_mst.o \
	display/intel_drrs.o \
	display/intel_dsb.o \
	display/intel_dsi.o \
	display/intel_dsi_dcs_backlight.o \
	display/intel_dsi_vbt.o \
	display/intel_fb.o \
	display/intel_fbc.o \
	display/intel_fbdev.o \
	display/intel_fdi.o \
	display/intel_fifo_underrun.o \
	display/intel_frontbuffer.o \
	display/intel_global_state.o \
	display/intel_gmbus.o \
	display/intel_hdcp.o \
	display/intel_hdmi.o \
	display/intel_hotplug.o \
	display/intel_lspcon.o \
	display/intel_lvds.o \
	display/intel_modeset_setup.o \
	display/intel_modeset_verify.o \
	display/intel_opregion.o \
	display/intel_panel.o \
	display/intel_pipe_crc.o \
	display/intel_pps.o \
	display/intel_psr.o \
	display/intel_qp_tables.o \
	display/intel_quirks.o \
	display/intel_snps_phy.o \
	display/intel_sprite.o \
	display/intel_tc.o \
	display/intel_vdsc.o \
	display/intel_vga.o \
	display/intel_vrr.o \
	display/xe_fb_pin.o \
	display/xe_plane_initial.o \
	display/skl_scaler.o \
	display/skl_universal_plane.o \
	display/skl_watermark.o \
	display/ext/i915_irq.o \
	display/ext/intel_device_info.o \
	display/ext/intel_dram.o \
	display/ext/intel_pch.o \
	display/ext/intel_pm.o

obj-$(CONFIG_DRM_XE) += xe.o
obj-$(CONFIG_DRM_XE_KUNIT_TEST) += tests/

ifneq ($(CONFIG_DRM_XE_DISPLAY),y)
	skipdisplay:=-not -path display/\*
endif

# header test
always-$(CONFIG_DRM_XE_WERROR) += \
	$(patsubst %.h,%.hdrtest, $(shell cd $(srctree)/$(src) && find * -name '*.h' $(skipdisplay)))

quiet_cmd_hdrtest = HDRTEST $(patsubst %.hdrtest,%.h,$@)
      cmd_hdrtest = $(CC) -DHDRTEST $(filter-out $(CFLAGS_GCOV), $(c_flags)) $(cflags-display) -S -o /dev/null -x c /dev/null -include $<; touch $@

$(obj)/%.hdrtest: $(src)/%.h FORCE
	$(call if_changed_dep,hdrtest)
