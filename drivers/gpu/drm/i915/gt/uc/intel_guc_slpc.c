// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include <drm/drm_cache.h>
#include <linux/string_helpers.h>

#include "i915_drv.h"
#include "i915_reg.h"
#include "display/intel_display_guc_metrics.h"
#include "display/intel_display_guc_metrics_types.h"
#include "intel_guc_slpc.h"
#include "intel_guc_print.h"
#include "intel_mchbar_regs.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_regs.h"
#include "gt/intel_rps.h"

static inline struct intel_guc *slpc_to_guc(struct intel_guc_slpc *slpc)
{
	return container_of(slpc, struct intel_guc, slpc);
}

static inline struct intel_gt *slpc_to_gt(struct intel_guc_slpc *slpc)
{
	return guc_to_gt(slpc_to_guc(slpc));
}

static inline struct drm_i915_private *slpc_to_i915(struct intel_guc_slpc *slpc)
{
	return slpc_to_gt(slpc)->i915;
}

static bool __detect_slpc_supported(struct intel_guc *guc)
{
	/* GuC SLPC is unavailable for pre-Gen12 */
	return guc->submission_supported &&
		GRAPHICS_VER(guc_to_i915(guc)) >= 12;
}

static bool __guc_slpc_selected(struct intel_guc *guc)
{
	if (!intel_guc_slpc_is_supported(guc))
		return false;

	return guc->submission_selected;
}

void intel_guc_slpc_init_early(struct intel_guc_slpc *slpc)
{
	struct intel_guc *guc = slpc_to_guc(slpc);

	slpc->supported = __detect_slpc_supported(guc);
	slpc->selected = __guc_slpc_selected(guc);
}

static void slpc_mem_set_param(struct slpc_shared_data *data,
			       u32 id, u32 value)
{
	GEM_BUG_ON(id >= SLPC_MAX_OVERRIDE_PARAMETERS);
	/*
	 * When the flag bit is set, corresponding value will be read
	 * and applied by SLPC.
	 */
	data->override_params.bits[id >> 5] |= (1 << (id % 32));
	data->override_params.values[id] = value;
}

static void slpc_mem_set_enabled(struct slpc_shared_data *data,
				 u8 enable_id, u8 disable_id)
{
	/*
	 * Enabling a param involves setting the enable_id
	 * to 1 and disable_id to 0.
	 */
	slpc_mem_set_param(data, enable_id, 1);
	slpc_mem_set_param(data, disable_id, 0);
}

static void slpc_mem_set_disabled(struct slpc_shared_data *data,
				  u8 enable_id, u8 disable_id)
{
	/*
	 * Disabling a param involves setting the enable_id
	 * to 0 and disable_id to 1.
	 */
	slpc_mem_set_param(data, disable_id, 1);
	slpc_mem_set_param(data, enable_id, 0);
}

static u32 slpc_get_state(struct intel_guc_slpc *slpc)
{
	struct slpc_shared_data *data;

	GEM_BUG_ON(!slpc->vma);

	drm_clflush_virt_range(slpc->vaddr, sizeof(u32));
	data = slpc->vaddr;

	return data->header.global_state;
}

static int guc_action_slpc_set_param_nb(struct intel_guc *guc, u8 id, u32 value)
{
	u32 request[] = {
		GUC_ACTION_HOST2GUC_PC_SLPC_REQUEST,
		SLPC_EVENT(SLPC_EVENT_PARAMETER_SET, 2),
		id,
		value,
	};
	int ret;

	ret = intel_guc_send_nb(guc, request, ARRAY_SIZE(request), 0);

	return ret > 0 ? -EPROTO : ret;
}

static int slpc_set_param_nb(struct intel_guc_slpc *slpc, u8 id, u32 value)
{
	struct intel_guc *guc = slpc_to_guc(slpc);

	GEM_BUG_ON(id >= SLPC_MAX_PARAM);

	return guc_action_slpc_set_param_nb(guc, id, value);
}

static int guc_action_slpc_set_param(struct intel_guc *guc, u8 id, u32 value)
{
	u32 request[] = {
		GUC_ACTION_HOST2GUC_PC_SLPC_REQUEST,
		SLPC_EVENT(SLPC_EVENT_PARAMETER_SET, 2),
		id,
		value,
	};
	int ret;

	ret = intel_guc_send(guc, request, ARRAY_SIZE(request));

	return ret > 0 ? -EPROTO : ret;
}

static bool slpc_is_running(struct intel_guc_slpc *slpc)
{
	return slpc_get_state(slpc) == SLPC_GLOBAL_STATE_RUNNING;
}

static int guc_action_slpc_query(struct intel_guc *guc, u32 offset)
{
	u32 request[] = {
		GUC_ACTION_HOST2GUC_PC_SLPC_REQUEST,
		SLPC_EVENT(SLPC_EVENT_QUERY_TASK_STATE, 2),
		offset,
		0,
	};
	int ret;

	ret = intel_guc_send(guc, request, ARRAY_SIZE(request));

	return ret > 0 ? -EPROTO : ret;
}

static int slpc_query_task_state(struct intel_guc_slpc *slpc)
{
	struct intel_guc *guc = slpc_to_guc(slpc);
	u32 offset = intel_guc_ggtt_offset(guc, slpc->vma);
	int ret;

	ret = guc_action_slpc_query(guc, offset);
	if (unlikely(ret))
		guc_probe_error(guc, "Failed to query task state: %pe\n", ERR_PTR(ret));

	drm_clflush_virt_range(slpc->vaddr, SLPC_PAGE_SIZE_BYTES);

	return ret;
}

static int slpc_set_param(struct intel_guc_slpc *slpc, u8 id, u32 value)
{
	struct intel_guc *guc = slpc_to_guc(slpc);
	int ret;

	GEM_BUG_ON(id >= SLPC_MAX_PARAM);

	ret = guc_action_slpc_set_param(guc, id, value);
	if (ret)
		guc_probe_error(guc, "Failed to set param %d to %u: %pe\n",
				id, value, ERR_PTR(ret));

	return ret;
}

static int slpc_force_min_freq(struct intel_guc_slpc *slpc, u32 freq)
{
	struct intel_guc *guc = slpc_to_guc(slpc);
	struct drm_i915_private *i915 = slpc_to_i915(slpc);
	intel_wakeref_t wakeref;
	int ret = 0;

	lockdep_assert_held(&slpc->lock);

	if (!intel_guc_is_ready(guc))
		return -ENODEV;

	/*
	 * This function is a little different as compared to
	 * intel_guc_slpc_set_min_freq(). Softlimit will not be updated
	 * here since this is used to temporarily change min freq,
	 * for example, during a waitboost. Caller is responsible for
	 * checking bounds.
	 */

	with_intel_runtime_pm(&i915->runtime_pm, wakeref) {
		/* Non-blocking request will avoid stalls */
		ret = slpc_set_param_nb(slpc,
					SLPC_PARAM_GLOBAL_MIN_GT_UNSLICE_FREQ_MHZ,
					freq);
		if (ret)
			guc_notice(guc, "Failed to send set_param for min freq(%d): %pe\n",
				   freq, ERR_PTR(ret));
	}

	return ret;
}

static void slpc_boost_work(struct work_struct *work)
{
	struct intel_guc_slpc *slpc = container_of(work, typeof(*slpc), boost_work);
	int err;

	/*
	 * Raise min freq to boost. It's possible that
	 * this is greater than current max. But it will
	 * certainly be limited by RP0. An error setting
	 * the min param is not fatal.
	 */
	mutex_lock(&slpc->lock);
	if (atomic_read(&slpc->num_waiters)) {
		err = slpc_force_min_freq(slpc, slpc->boost_freq);
		if (!err)
			slpc->num_boosts++;
	}
	mutex_unlock(&slpc->lock);
}

static void slpc_display_data_init(struct slpc_display_data *data, int version,
				   int num_pipes, int num_planes_per_pipe)
{
	data->global_info.version = version;
	data->global_info.num_pipes = num_pipes;
	data->global_info.num_planes_per_pipe = num_planes_per_pipe;
}

static void slpc_refresh_info(struct slpc_display_data *data, int pipe,
			      u32 refresh_interval, bool vrr_enabled)
{
	data->refresh_info[pipe].refresh_interval = refresh_interval;
	data->refresh_info[pipe].is_variable = vrr_enabled;
	data->global_info.refresh_count += 1;
}

static void slpc_vblank(struct slpc_display_data *data, int pipe,
			u32 timestamp)
{
	u32 vblank;

	vblank = data->vblank_metrics[pipe];

	vblank = REG_FIELD_GET(SLPC_VBLANK_COUNT, vblank);
	vblank = REG_FIELD_PREP(SLPC_VBLANK_COUNT, vblank + 1);
	vblank |= REG_FIELD_PREP(SLPC_VBLANK_LAST, timestamp);

	data->vblank_metrics[pipe] = vblank;
	data->global_info.vblank_count += 1;
}

static void slpc_flip(struct slpc_display_data *data, int pipe, int plane,
		      bool async_flip, u32 timestamp)
{
	u32 part1, part2, count;

	part1 = data->flip_metrics[pipe][plane].part1;
	part2 = data->flip_metrics[pipe][plane].part2;

	part1 = REG_FIELD_GET(SLPC_FLIP_P1_TOTAL_COUNT, part1);
	part1 = REG_FIELD_PREP(SLPC_FLIP_P1_TOTAL_COUNT, part1 + 1);
	part1 |= REG_FIELD_PREP(SLPC_FLIP_P1_LAST, timestamp);

	if (async_flip) {
		count = REG_FIELD_GET(SLPC_FLIP_P2_ASYNC_COUNT, part2);
		part2 &= ~SLPC_FLIP_P2_ASYNC_COUNT;
		part2 |= REG_FIELD_PREP(SLPC_FLIP_P2_ASYNC_COUNT, count + 1);
	} else {
		count = REG_FIELD_GET(SLPC_FLIP_P2_VSYNC_COUNT, part2);
		part2 &= ~SLPC_FLIP_P2_VSYNC_COUNT;
		part2 |= REG_FIELD_PREP(SLPC_FLIP_P2_VSYNC_COUNT, count + 1);
	}

	data->flip_metrics[pipe][plane].part1 = part1;
	data->flip_metrics[pipe][plane].part2 = part2;

	data->global_info.flip_count += 1;
}

static void intel_guc_slpc_refresh_info_update(void *gfx_device, int pipe,
					       u32 refresh_interval,
					       bool vrr_enabled)
{
	struct drm_i915_private *i915 = gfx_device;
	struct intel_gt *gt;
	int i;

	if (pipe > SLPC_MAX_PIPES) {
		drm_err(&i915->drm, "GuC PC Max display pipe exceeded\n");
		return;
	}

	for_each_gt(gt, i915, i)
		slpc_refresh_info(gt->uc.guc.slpc.display.vaddr, pipe,
				  refresh_interval, vrr_enabled);
}

#define MCHBAR_BCLK_COUNT	_MMIO(MCHBAR_MIRROR_BASE_SNB + 0x5984)
#define MTL_BCLK_COUNT		_MMIO(0xc28)
#define   TIMESTAMP_MASK	REG_GENMASK(30, 6)

static u32 bclk_read_timestamp(struct intel_gt *gt)
{
	u32 timestamp;

	if (IS_METEORLAKE(gt->i915))
		timestamp = intel_uncore_read_fw(gt->uncore, MTL_BCLK_COUNT);
	else
		timestamp = intel_uncore_read_fw(gt->uncore, MCHBAR_BCLK_COUNT);

	return REG_FIELD_GET(TIMESTAMP_MASK, timestamp);
}

static void intel_guc_slpc_vblank_update(void *gfx_device, int pipe)
{
	struct drm_i915_private *i915 = gfx_device;
	struct intel_gt *gt;
	u32 timestamp;
	int i;

	if (!(i915->params.enable_guc & ENABLE_GUC_SLPC_VBLANK))
		return;

	if (pipe > SLPC_MAX_PIPES) {
		drm_err(&i915->drm, "GuC PC Max display pipe exceeded\n");
		return;
	}

	for_each_gt(gt, i915, i) {
		timestamp = bclk_read_timestamp(gt);
		slpc_vblank(gt->uc.guc.slpc.display.vaddr, pipe, timestamp);
	}
}

static void intel_guc_slpc_flip_update(void *gfx_device, int pipe, int plane,
				       bool async_flip)
{
	struct drm_i915_private *i915 = gfx_device;
	struct intel_gt *gt;
	u32 timestamp;
	int i;

	if (!(i915->params.enable_guc & ENABLE_GUC_SLPC_FLIP))
		return;

	if (pipe > SLPC_MAX_PIPES) {
		drm_err(&i915->drm, "GuC PC Max display pipe exceeded\n");
		return;
	}

	if (plane > SLPC_MAX_PLANES_PER_PIPE) {
		drm_err(&i915->drm, "GuC PC Max display planes exceeded\n");
		return;
	}

	for_each_gt(gt, i915, i) {
		timestamp = bclk_read_timestamp(gt);
		slpc_flip(gt->uc.guc.slpc.display.vaddr, pipe, plane,
			  async_flip, timestamp);
	}
}

static struct intel_display_guc_metrics guc_metrics = {
	.refresh_info_update = intel_guc_slpc_refresh_info_update,
	.vblank_update = intel_guc_slpc_vblank_update,
	.flip_update = intel_guc_slpc_flip_update,
};

int intel_guc_slpc_init(struct intel_guc_slpc *slpc)
{
	struct intel_guc *guc = slpc_to_guc(slpc);
	struct drm_i915_private *i915 = guc_to_i915(guc);
	u32 size;
	int err;

	GEM_BUG_ON(slpc->vma);

	size = PAGE_ALIGN(sizeof(struct slpc_shared_data));
	err = intel_guc_allocate_and_map_vma(guc, size, &slpc->vma, (void **)&slpc->vaddr);
	if (unlikely(err)) {
		guc_probe_error(guc, "Failed to allocate SLPC struct: %pe\n", ERR_PTR(err));
		return err;
	}

	size = PAGE_ALIGN(sizeof(struct slpc_display_data));
	err = intel_guc_allocate_and_map_vma(guc, size, &slpc->display.vma,
					     (void **)&slpc->display.vaddr);
	if (unlikely(err)) {
		guc_probe_error(guc, "Failed to allocate SLPC's display struct: %pe\n",
				ERR_PTR(err));
		return err;
	}

	slpc->max_freq_softlimit = 0;
	slpc->min_freq_softlimit = 0;
	slpc->ignore_eff_freq = false;
	slpc->min_is_rpmax = false;

	slpc->boost_freq = 0;
	atomic_set(&slpc->num_waiters, 0);
	slpc->num_boosts = 0;
	slpc->media_ratio_mode = SLPC_MEDIA_RATIO_MODE_DYNAMIC_CONTROL;

	if (i915->params.enable_guc & ENABLE_GUC_SLPC_VBLANK ||
	    i915->params.enable_guc & ENABLE_GUC_SLPC_FLIP)
		intel_display_guc_metrics_init(i915, &i915->display, &guc_metrics);

	mutex_init(&slpc->lock);
	INIT_WORK(&slpc->boost_work, slpc_boost_work);

	return err;
}

static const char *slpc_global_state_to_string(enum slpc_global_state state)
{
	switch (state) {
	case SLPC_GLOBAL_STATE_NOT_RUNNING:
		return "not running";
	case SLPC_GLOBAL_STATE_INITIALIZING:
		return "initializing";
	case SLPC_GLOBAL_STATE_RESETTING:
		return "resetting";
	case SLPC_GLOBAL_STATE_RUNNING:
		return "running";
	case SLPC_GLOBAL_STATE_SHUTTING_DOWN:
		return "shutting down";
	case SLPC_GLOBAL_STATE_ERROR:
		return "error";
	default:
		return "unknown";
	}
}

static const char *slpc_get_state_string(struct intel_guc_slpc *slpc)
{
	return slpc_global_state_to_string(slpc_get_state(slpc));
}

static int guc_action_slpc_reset(struct intel_guc *guc, u32 offset)
{
	u32 request[] = {
		GUC_ACTION_HOST2GUC_PC_SLPC_REQUEST,
		SLPC_EVENT(SLPC_EVENT_RESET, 2),
		offset,
		0,
	};
	int ret;

	ret = intel_guc_send(guc, request, ARRAY_SIZE(request));

	return ret > 0 ? -EPROTO : ret;
}

static int slpc_reset(struct intel_guc_slpc *slpc)
{
	struct intel_guc *guc = slpc_to_guc(slpc);
	u32 offset = intel_guc_ggtt_offset(guc, slpc->vma);
	int ret;

	ret = guc_action_slpc_reset(guc, offset);

	if (unlikely(ret < 0)) {
		guc_probe_error(guc, "SLPC reset action failed: %pe\n", ERR_PTR(ret));
		return ret;
	}

	if (!ret) {
		if (wait_for(slpc_is_running(slpc), SLPC_RESET_TIMEOUT_MS)) {
			guc_probe_error(guc, "SLPC not enabled! State = %s\n",
					slpc_get_state_string(slpc));
			return -EIO;
		}
	}

	return 0;
}

static u32 slpc_decode_min_freq(struct intel_guc_slpc *slpc)
{
	struct slpc_shared_data *data = slpc->vaddr;

	GEM_BUG_ON(!slpc->vma);

	return	DIV_ROUND_CLOSEST(REG_FIELD_GET(SLPC_MIN_UNSLICE_FREQ_MASK,
				  data->task_state_data.freq) *
				  GT_FREQUENCY_MULTIPLIER, GEN9_FREQ_SCALER);
}

static u32 slpc_decode_max_freq(struct intel_guc_slpc *slpc)
{
	struct slpc_shared_data *data = slpc->vaddr;

	GEM_BUG_ON(!slpc->vma);

	return	DIV_ROUND_CLOSEST(REG_FIELD_GET(SLPC_MAX_UNSLICE_FREQ_MASK,
				  data->task_state_data.freq) *
				  GT_FREQUENCY_MULTIPLIER, GEN9_FREQ_SCALER);
}

static void slpc_shared_display_data_reset(struct intel_guc_slpc *slpc)
{
	struct slpc_shared_data *data = slpc->vaddr;
	struct slpc_display_data *display_data = slpc->display.vaddr;

	memset(display_data, 0, sizeof(struct slpc_display_data));

	slpc_display_data_init(slpc->display.vaddr, 1, SLPC_MAX_PIPES,
			       SLPC_MAX_PLANES_PER_PIPE);
	data->header.display_data_addr = intel_guc_ggtt_offset(slpc_to_guc(slpc),
							       slpc->display.vma);
}

static void slpc_shared_data_reset(struct intel_guc_slpc *slpc)
{
	struct slpc_shared_data *data = slpc->vaddr;

	memset(data, 0, sizeof(struct slpc_shared_data));
	data->header.size = sizeof(struct slpc_shared_data);

	slpc_shared_display_data_reset(slpc);

	/* Enable only GTPERF task, disable others */
	slpc_mem_set_enabled(data, SLPC_PARAM_TASK_ENABLE_GTPERF,
			     SLPC_PARAM_TASK_DISABLE_GTPERF);

	slpc_mem_set_disabled(data, SLPC_PARAM_TASK_ENABLE_BALANCER,
			      SLPC_PARAM_TASK_DISABLE_BALANCER);

	slpc_mem_set_disabled(data, SLPC_PARAM_TASK_ENABLE_DCC,
			      SLPC_PARAM_TASK_DISABLE_DCC);
}

/**
 * intel_guc_slpc_set_max_freq() - Set max frequency limit for SLPC.
 * @slpc: pointer to intel_guc_slpc.
 * @val: frequency (MHz)
 *
 * This function will invoke GuC SLPC action to update the max frequency
 * limit for unslice.
 *
 * Return: 0 on success, non-zero error code on failure.
 */
int intel_guc_slpc_set_max_freq(struct intel_guc_slpc *slpc, u32 val)
{
	struct drm_i915_private *i915 = slpc_to_i915(slpc);
	intel_wakeref_t wakeref;
	int ret;

	if (val < slpc->min_freq ||
	    val > slpc->rp0_freq ||
	    val < slpc->min_freq_softlimit)
		return -EINVAL;

	with_intel_runtime_pm(&i915->runtime_pm, wakeref) {
		ret = slpc_set_param(slpc,
				     SLPC_PARAM_GLOBAL_MAX_GT_UNSLICE_FREQ_MHZ,
				     val);

		/* Return standardized err code for sysfs calls */
		if (ret)
			ret = -EIO;
	}

	if (!ret)
		slpc->max_freq_softlimit = val;

	return ret;
}

/**
 * intel_guc_slpc_get_max_freq() - Get max frequency limit for SLPC.
 * @slpc: pointer to intel_guc_slpc.
 * @val: pointer to val which will hold max frequency (MHz)
 *
 * This function will invoke GuC SLPC action to read the max frequency
 * limit for unslice.
 *
 * Return: 0 on success, non-zero error code on failure.
 */
int intel_guc_slpc_get_max_freq(struct intel_guc_slpc *slpc, u32 *val)
{
	struct drm_i915_private *i915 = slpc_to_i915(slpc);
	intel_wakeref_t wakeref;
	int ret = 0;

	with_intel_runtime_pm(&i915->runtime_pm, wakeref) {
		/* Force GuC to update task data */
		ret = slpc_query_task_state(slpc);

		if (!ret)
			*val = slpc_decode_max_freq(slpc);
	}

	return ret;
}

int intel_guc_slpc_set_ignore_eff_freq(struct intel_guc_slpc *slpc, bool val)
{
	struct drm_i915_private *i915 = slpc_to_i915(slpc);
	intel_wakeref_t wakeref;
	int ret;

	mutex_lock(&slpc->lock);
	wakeref = intel_runtime_pm_get(&i915->runtime_pm);

	ret = slpc_set_param(slpc,
			     SLPC_PARAM_IGNORE_EFFICIENT_FREQUENCY,
			     val);
	if (ret) {
		guc_probe_error(slpc_to_guc(slpc), "Failed to set efficient freq(%d): %pe\n",
				val, ERR_PTR(ret));
	} else {
		slpc->ignore_eff_freq = val;

		/* Set min to RPn when we disable efficient freq */
		if (val)
			ret = slpc_set_param(slpc,
					     SLPC_PARAM_GLOBAL_MIN_GT_UNSLICE_FREQ_MHZ,
					     slpc->min_freq);
	}

	intel_runtime_pm_put(&i915->runtime_pm, wakeref);
	mutex_unlock(&slpc->lock);
	return ret;
}

/**
 * intel_guc_slpc_set_min_freq() - Set min frequency limit for SLPC.
 * @slpc: pointer to intel_guc_slpc.
 * @val: frequency (MHz)
 *
 * This function will invoke GuC SLPC action to update the min unslice
 * frequency.
 *
 * Return: 0 on success, non-zero error code on failure.
 */
int intel_guc_slpc_set_min_freq(struct intel_guc_slpc *slpc, u32 val)
{
	struct drm_i915_private *i915 = slpc_to_i915(slpc);
	intel_wakeref_t wakeref;
	int ret;

	if (val < slpc->min_freq ||
	    val > slpc->rp0_freq ||
	    val > slpc->max_freq_softlimit)
		return -EINVAL;

	/* Need a lock now since waitboost can be modifying min as well */
	mutex_lock(&slpc->lock);
	wakeref = intel_runtime_pm_get(&i915->runtime_pm);

	ret = slpc_set_param(slpc,
			     SLPC_PARAM_GLOBAL_MIN_GT_UNSLICE_FREQ_MHZ,
			     val);

	if (!ret)
		slpc->min_freq_softlimit = val;

	intel_runtime_pm_put(&i915->runtime_pm, wakeref);
	mutex_unlock(&slpc->lock);

	/* Return standardized err code for sysfs calls */
	if (ret)
		ret = -EIO;

	return ret;
}

/**
 * intel_guc_slpc_get_min_freq() - Get min frequency limit for SLPC.
 * @slpc: pointer to intel_guc_slpc.
 * @val: pointer to val which will hold min frequency (MHz)
 *
 * This function will invoke GuC SLPC action to read the min frequency
 * limit for unslice.
 *
 * Return: 0 on success, non-zero error code on failure.
 */
int intel_guc_slpc_get_min_freq(struct intel_guc_slpc *slpc, u32 *val)
{
	struct drm_i915_private *i915 = slpc_to_i915(slpc);
	intel_wakeref_t wakeref;
	int ret = 0;

	with_intel_runtime_pm(&i915->runtime_pm, wakeref) {
		/* Force GuC to update task data */
		ret = slpc_query_task_state(slpc);

		if (!ret)
			*val = slpc_decode_min_freq(slpc);
	}

	return ret;
}

int intel_guc_slpc_set_strategy(struct intel_guc_slpc *slpc, u32 val)
{
	struct drm_i915_private *i915 = slpc_to_i915(slpc);
	intel_wakeref_t wakeref;
	int ret = 0;

	with_intel_runtime_pm(&i915->runtime_pm, wakeref)
		ret = slpc_set_param(slpc,
				     SLPC_PARAM_STRATEGIES,
				     val);

	return ret;
}

int intel_guc_slpc_set_media_ratio_mode(struct intel_guc_slpc *slpc, u32 val)
{
	struct drm_i915_private *i915 = slpc_to_i915(slpc);
	intel_wakeref_t wakeref;
	int ret = 0;

	if (!HAS_MEDIA_RATIO_MODE(i915))
		return -ENODEV;

	with_intel_runtime_pm(&i915->runtime_pm, wakeref)
		ret = slpc_set_param(slpc,
				     SLPC_PARAM_MEDIA_FF_RATIO_MODE,
				     val);
	return ret;
}

void intel_guc_pm_intrmsk_enable(struct intel_gt *gt)
{
	u32 pm_intrmsk_mbz = 0;

	/*
	 * Allow GuC to receive ARAT timer expiry event.
	 * This interrupt register is setup by RPS code
	 * when host based Turbo is enabled.
	 */
	pm_intrmsk_mbz |= ARAT_EXPIRED_INTRMSK;

	intel_uncore_rmw(gt->uncore,
			 GEN6_PMINTRMSK, pm_intrmsk_mbz, 0);
}

static int slpc_set_softlimits(struct intel_guc_slpc *slpc)
{
	int ret = 0;

	/*
	 * Softlimits are initially equivalent to platform limits
	 * unless they have deviated from defaults, in which case,
	 * we retain the values and set min/max accordingly.
	 */
	if (!slpc->max_freq_softlimit) {
		slpc->max_freq_softlimit = slpc->rp0_freq;
		slpc_to_gt(slpc)->defaults.max_freq = slpc->max_freq_softlimit;
	} else if (slpc->max_freq_softlimit != slpc->rp0_freq) {
		ret = intel_guc_slpc_set_max_freq(slpc,
						  slpc->max_freq_softlimit);
	}

	if (unlikely(ret))
		return ret;

	if (!slpc->min_freq_softlimit) {
		/* Min softlimit is initialized to RPn */
		slpc->min_freq_softlimit = slpc->min_freq;
		slpc_to_gt(slpc)->defaults.min_freq = slpc->min_freq_softlimit;
	} else {
		return intel_guc_slpc_set_min_freq(slpc,
						   slpc->min_freq_softlimit);
	}

	return 0;
}

static bool is_slpc_min_freq_rpmax(struct intel_guc_slpc *slpc)
{
	int slpc_min_freq;
	int ret;

	ret = intel_guc_slpc_get_min_freq(slpc, &slpc_min_freq);
	if (ret) {
		guc_err(slpc_to_guc(slpc), "Failed to get min freq: %pe\n", ERR_PTR(ret));
		return false;
	}

	if (slpc_min_freq == SLPC_MAX_FREQ_MHZ)
		return true;
	else
		return false;
}

static void update_server_min_softlimit(struct intel_guc_slpc *slpc)
{
	/* For server parts, SLPC min will be at RPMax.
	 * Use min softlimit to clamp it to RP0 instead.
	 */
	if (!slpc->min_freq_softlimit &&
	    is_slpc_min_freq_rpmax(slpc)) {
		slpc->min_is_rpmax = true;
		slpc->min_freq_softlimit = slpc->rp0_freq;
		(slpc_to_gt(slpc))->defaults.min_freq = slpc->min_freq_softlimit;
	}
}

static int slpc_use_fused_rp0(struct intel_guc_slpc *slpc)
{
	/* Force SLPC to used platform rp0 */
	return slpc_set_param(slpc,
			      SLPC_PARAM_GLOBAL_MAX_GT_UNSLICE_FREQ_MHZ,
			      slpc->rp0_freq);
}

static void slpc_get_rp_values(struct intel_guc_slpc *slpc)
{
	struct intel_rps *rps = &slpc_to_gt(slpc)->rps;
	struct intel_rps_freq_caps caps;

	gen6_rps_get_freq_caps(rps, &caps);
	slpc->rp0_freq = intel_gpu_freq(rps, caps.rp0_freq);
	slpc->rp1_freq = intel_gpu_freq(rps, caps.rp1_freq);
	slpc->min_freq = intel_gpu_freq(rps, caps.min_freq);

	if (!slpc->boost_freq)
		slpc->boost_freq = slpc->rp0_freq;
}

/*
 * intel_guc_slpc_enable() - Start SLPC
 * @slpc: pointer to intel_guc_slpc.
 *
 * SLPC is enabled by setting up the shared data structure and
 * sending reset event to GuC SLPC. Initial data is setup in
 * intel_guc_slpc_init. Here we send the reset event. We do
 * not currently need a slpc_disable since this is taken care
 * of automatically when a reset/suspend occurs and the GuC
 * CTB is destroyed.
 *
 * Return: 0 on success, non-zero error code on failure.
 */
int intel_guc_slpc_enable(struct intel_guc_slpc *slpc)
{
	struct intel_guc *guc = slpc_to_guc(slpc);
	int ret;

	GEM_BUG_ON(!slpc->vma);

	slpc_shared_data_reset(slpc);

	ret = slpc_reset(slpc);
	if (unlikely(ret < 0)) {
		guc_probe_error(guc, "SLPC Reset event returned: %pe\n", ERR_PTR(ret));
		return ret;
	}

	ret = slpc_query_task_state(slpc);
	if (unlikely(ret < 0))
		return ret;

	intel_guc_pm_intrmsk_enable(slpc_to_gt(slpc));

	slpc_get_rp_values(slpc);

	/* Handle the case where min=max=RPmax */
	update_server_min_softlimit(slpc);

	/* Set SLPC max limit to RP0 */
	ret = slpc_use_fused_rp0(slpc);
	if (unlikely(ret)) {
		guc_probe_error(guc, "Failed to set SLPC max to RP0: %pe\n", ERR_PTR(ret));
		return ret;
	}

	/* Set cached value of ignore efficient freq */
	intel_guc_slpc_set_ignore_eff_freq(slpc, slpc->ignore_eff_freq);

	/* Revert SLPC min/max to softlimits if necessary */
	ret = slpc_set_softlimits(slpc);
	if (unlikely(ret)) {
		guc_probe_error(guc, "Failed to set SLPC softlimits: %pe\n", ERR_PTR(ret));
		return ret;
	}

	/* Set cached media freq ratio mode */
	intel_guc_slpc_set_media_ratio_mode(slpc, slpc->media_ratio_mode);

	/* Enable SLPC Optimized Strategy for compute */
	intel_guc_slpc_set_strategy(slpc, SLPC_OPTIMIZED_STRATEGY_COMPUTE);

	slpc_set_param_nb(slpc, SLPC_PARAM_STRATEGIES,
			  SLPC_OPTIMIZED_STRATEGIES_VSYNC_FLIP |
			  SLPC_OPTIMIZED_STRATEGIES_ASYNC_FLIP);

	return 0;
}

int intel_guc_slpc_set_boost_freq(struct intel_guc_slpc *slpc, u32 val)
{
	int ret = 0;

	if (val < slpc->min_freq || val > slpc->rp0_freq)
		return -EINVAL;

	mutex_lock(&slpc->lock);

	if (slpc->boost_freq != val) {
		/* Apply only if there are active waiters */
		if (atomic_read(&slpc->num_waiters)) {
			ret = slpc_force_min_freq(slpc, val);
			if (ret) {
				ret = -EIO;
				goto done;
			}
		}

		slpc->boost_freq = val;
	}

done:
	mutex_unlock(&slpc->lock);
	return ret;
}

void intel_guc_slpc_dec_waiters(struct intel_guc_slpc *slpc)
{
	/*
	 * Return min back to the softlimit.
	 * This is called during request retire,
	 * so we don't need to fail that if the
	 * set_param fails.
	 */
	mutex_lock(&slpc->lock);
	if (atomic_dec_and_test(&slpc->num_waiters))
		slpc_force_min_freq(slpc, slpc->min_freq_softlimit);
	mutex_unlock(&slpc->lock);
}

static void slpc_print_display_metrics(struct drm_printer *p,
				       struct slpc_display_data *data)
{
	int pipe, plane;
	u32 val;

	drm_printf(p, "\nSLPC Display Data - Globals:\n");
	drm_printf(p, "\tVersion: %d\n", data->global_info.version);
	drm_printf(p, "\tNum Pipes: %d\n", data->global_info.num_pipes);
	drm_printf(p, "\tNum Planes per Pipe: %d\n",
		   data->global_info.num_planes_per_pipe);
	drm_printf(p, "\tGlobal Refresh Info Count: %d\n",
		   data->global_info.refresh_count);
	drm_printf(p, "\tGlobal Vblank Count: %d\n",
		   data->global_info.vblank_count);
	drm_printf(p, "\tGlobal Flip Count: %d\n",
		   data->global_info.flip_count);

	for (pipe = 0; pipe < SLPC_MAX_PIPES; pipe++) {

		if (!data->refresh_info[pipe].refresh_interval)
			continue;

		drm_printf(p, "\nSLPC Display Data - Refresh Info - Pipe[%d]:\n",
			   pipe);
		drm_printf(p, "\tRefresh Interval: %d\n",
			   data->refresh_info[pipe].refresh_interval);
		drm_printf(p, "\tIS VRR: %d\n",
			   data->refresh_info[pipe].is_variable);

		drm_printf(p, "SLPC Display Data - Vblank Info - Pipe[%d]:\n",
			   pipe);
		val = data->vblank_metrics[pipe];
		drm_printf(p, "\tVBlank Last Timestamp: %x\n",
			   REG_FIELD_GET(SLPC_VBLANK_LAST, val));
		drm_printf(p, "\tVBlank Count: %d\n",
			   REG_FIELD_GET(SLPC_VBLANK_COUNT, val));

		drm_printf(p, "SLPC Display Data - Flip Info - Pipe[%d]:\n", pipe);
		for (plane = 0; plane < SLPC_MAX_PLANES_PER_PIPE; plane++) {

			val = data->flip_metrics[pipe][plane].part1;
			if (!val)
				continue;

			drm_printf(p, "\tFlip Info - Plane[%d]:\n", plane);
			drm_printf(p, "\t\tFlip Last Timestamp: %x\n",
				   REG_FIELD_GET(SLPC_FLIP_P1_LAST, val));
			drm_printf(p, "\t\tFlip Total Count: %d\n",
				   REG_FIELD_GET(SLPC_FLIP_P1_TOTAL_COUNT, val));

			val = data->flip_metrics[pipe][plane].part2;

			drm_printf(p, "\t\tFlip Async Count: %d\n",
				   REG_FIELD_GET(SLPC_FLIP_P2_ASYNC_COUNT, val));
			drm_printf(p, "\t\tFlip Vsync Count: %d\n",
				   REG_FIELD_GET(SLPC_FLIP_P2_VSYNC_COUNT, val));
		}
	}
}

int intel_guc_slpc_print_info(struct intel_guc_slpc *slpc, struct drm_printer *p)
{
	struct drm_i915_private *i915 = slpc_to_i915(slpc);
	struct slpc_shared_data *data = slpc->vaddr;
	struct slpc_task_state_data *slpc_tasks;
	intel_wakeref_t wakeref;
	int ret = 0;

	GEM_BUG_ON(!slpc->vma);

	with_intel_runtime_pm(&i915->runtime_pm, wakeref) {
		ret = slpc_query_task_state(slpc);

		if (!ret) {
			slpc_tasks = &data->task_state_data;

			drm_printf(p, "\tSLPC state: %s\n", slpc_get_state_string(slpc));
			drm_printf(p, "\tGTPERF task active: %s\n",
				   str_yes_no(slpc_tasks->status & SLPC_GTPERF_TASK_ENABLED));
			drm_printf(p, "\tMax freq: %u MHz\n",
				   slpc_decode_max_freq(slpc));
			drm_printf(p, "\tMin freq: %u MHz\n",
				   slpc_decode_min_freq(slpc));

			if (i915->params.enable_guc & ENABLE_GUC_SLPC_VBLANK ||
			    i915->params.enable_guc & ENABLE_GUC_SLPC_FLIP) {
				if (data->header.display_data_addr)
					slpc_print_display_metrics(p,
								   slpc->display.vaddr);
			} else {
				drm_printf(p, "\twaitboosts: %u\n",
					   slpc->num_boosts);
				drm_printf(p, "\tBoosts outstanding: %u\n",
					   atomic_read(&slpc->num_waiters));
			}
		}
	}

	return ret;
}

void intel_guc_slpc_fini(struct intel_guc_slpc *slpc)
{
	if (!slpc->vma)
		return;

	i915_vma_unpin_and_release(&slpc->display.vma, I915_VMA_RELEASE_MAP);
	i915_vma_unpin_and_release(&slpc->vma, I915_VMA_RELEASE_MAP);
}
