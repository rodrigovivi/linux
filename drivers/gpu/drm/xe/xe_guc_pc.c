// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <drm/drm_managed.h>
#include "xe_bo.h"
#include "xe_device.h"
#include "xe_gt.h"
#include "xe_gt_types.h"
#include "xe_gt_sysfs.h"
#include "xe_guc_ct.h"
#include "xe_mmio.h"
#include "i915_reg_defs.h"
#include "../i915/i915_reg.h"
#include "../i915/intel_mchbar_regs.h"

/**
 * DOC: GuC Power Conservation (PC)
 *
 * GuC Power Conservation (PC) supports multiple features for the most
 * efficient and performing use of the GT when GuC submission is enabled,
 * including frequency management, Render-C states management, and various
 * algorithms for power balancing.
 *
 * Single Loop Power Conservation (SLPC) is the name given to the suite of
 * connected power conservation features in the GuC firmware. The firmware
 * exposes a programming interface to the host for the control of SLPC.
 *
 * Xe driver enables SLPC with all of its defaults features and frequency
 * selection, which varies per platform.
 *
 * Currently Xe driver is not providing any API for frequency tuning. This
 * shall be implemented soon.
 *
 * Render-C states managements under GuCRC is currently disabled by default
 * in all platforms and Xe is not yet enabling it.
 */

#define GEN12_RPSTAT1			_MMIO(0x1381b4)
#define   GEN12_CAGF_MASK		REG_GENMASK(19, 11)

#define GT_FREQUENCY_MULTIPLIER	50
#define GEN9_FREQ_SCALER	3

static struct xe_guc *
pc_to_guc(struct xe_guc_pc *pc)
{
	return container_of(pc, struct xe_guc, pc);
}

static struct xe_device *
pc_to_xe(struct xe_guc_pc *pc)
{
	struct xe_guc *guc = pc_to_guc(pc);
	struct xe_gt *gt = container_of(guc, struct xe_gt, uc.guc);

	return gt_to_xe(gt);
}

static struct xe_gt *
pc_to_gt(struct xe_guc_pc *pc)
{
	return container_of(pc, struct xe_gt, uc.guc.pc);
}

static struct xe_guc_pc *
dev_to_pc(struct device *dev)
{
	return &kobj_to_gt(&dev->kobj)->uc.guc.pc;
}

static struct iosys_map *
pc_to_maps(struct xe_guc_pc *pc)
{
	return &pc->bo->vmap;
}

#define slpc_shared_data_read(pc_, field_) \
	iosys_map_rd_field(pc_to_maps(pc_), 0, struct slpc_shared_data, field_)

#define SLPC_EVENT(id, count) \
	(FIELD_PREP(HOST2GUC_PC_SLPC_REQUEST_MSG_1_EVENT_ID, id) | \
	 FIELD_PREP(HOST2GUC_PC_SLPC_REQUEST_MSG_1_EVENT_ARGC, count))

static int pc_action_reset(struct xe_guc_pc *pc)
{
	struct  xe_guc_ct *ct = &pc_to_guc(pc)->ct;
	int ret;
	u32 action[] = {
		GUC_ACTION_HOST2GUC_PC_SLPC_REQUEST,
		SLPC_EVENT(SLPC_EVENT_RESET, 2),
		xe_bo_ggtt_addr(pc->bo),
		0,
	};

	ret = xe_guc_ct_send(ct, action, ARRAY_SIZE(action), 0, 0);
	if (ret)
		drm_err(&pc_to_xe(pc)->drm, "GuC PC reset: %pe", ERR_PTR(ret));

	return ret;
}

static int pc_action_query_task_state(struct xe_guc_pc *pc)
{
	struct xe_guc_ct *ct = &pc_to_guc(pc)->ct;
	int ret;
	u32 action[] = {
		GUC_ACTION_HOST2GUC_PC_SLPC_REQUEST,
		SLPC_EVENT(SLPC_EVENT_QUERY_TASK_STATE, 2),
		xe_bo_ggtt_addr(pc->bo),
		0,
	};

	/* Blocking here to ensure the results are ready before reading them */
	ret = xe_guc_ct_send_block(ct, action, ARRAY_SIZE(action));
	if (ret)
		drm_err(&pc_to_xe(pc)->drm,
			"GuC PC query task state failed: %pe", ERR_PTR(ret));

	return ret;
}

static u32 decode_freq(u32 raw)
{
	return DIV_ROUND_CLOSEST(raw * GT_FREQUENCY_MULTIPLIER,
				 GEN9_FREQ_SCALER);
}

static ssize_t freq_act_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct kobject *kobj = &dev->kobj;
	struct xe_gt *gt = kobj_to_gt(kobj);
	u32 freq;

	freq = xe_mmio_read32(gt, GEN12_RPSTAT1.reg);
	freq = REG_FIELD_GET(GEN12_CAGF_MASK, freq);
	return sysfs_emit(buf, "%d\n", decode_freq(freq));
}
static DEVICE_ATTR_RO(freq_act);

static ssize_t freq_min_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct xe_guc_pc *pc = dev_to_pc(dev);
	u32 freq;
	ssize_t ret;

	ret = pc_action_query_task_state(pc);
	if (ret)
		return ret;

	freq = FIELD_GET(SLPC_MIN_UNSLICE_FREQ_MASK,
			 slpc_shared_data_read(pc, task_state_data.freq));

	return sysfs_emit(buf, "%d\n", decode_freq(freq));
}
static DEVICE_ATTR_RO(freq_min);

static ssize_t freq_max_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct xe_guc_pc *pc = dev_to_pc(dev);
	u32 freq;
	ssize_t ret;

	ret = pc_action_query_task_state(pc);
	if (ret)
		return ret;

	freq = FIELD_GET(SLPC_MAX_UNSLICE_FREQ_MASK,
			 slpc_shared_data_read(pc, task_state_data.freq));

	return sysfs_emit(buf, "%d\n", decode_freq(freq));
}
static DEVICE_ATTR_RO(freq_max);

static const struct attribute *pc_attrs[] = {
	&dev_attr_freq_act.attr,
	&dev_attr_freq_min.attr,
	&dev_attr_freq_max.attr,
	NULL
};

static void pc_fini(struct drm_device *drm, void *arg)
{
	struct xe_guc_pc *pc = arg;

	sysfs_remove_files(pc_to_gt(pc)->sysfs, pc_attrs);
	xe_bo_unpin_map_no_vm(pc->bo);
}

/**
 * xe_guc_pc_init - Initialize GuC's Power Conservation component
 * @pc: Xe_GuC_PC instance
 */
int xe_guc_pc_init(struct xe_guc_pc *pc)
{
	struct xe_gt *gt = pc_to_gt(pc);
	struct xe_device *xe = gt_to_xe(gt);
	struct xe_bo *bo;
	struct slpc_shared_data *data;
	u32 size = PAGE_ALIGN(sizeof(struct slpc_shared_data));
	int err;

	data = kzalloc(size, GFP_KERNEL);
	data->header.size = size;

	bo = xe_bo_create_from_data(xe, gt, data, size,
				    ttm_bo_type_kernel,
				    XE_BO_CREATE_VRAM_IF_DGFX(gt) |
				    XE_BO_CREATE_GGTT_BIT);
	kfree(data);
	if (IS_ERR(bo))
		return PTR_ERR(bo);

	pc->bo = bo;

	err = sysfs_create_files(gt->sysfs, pc_attrs);
	if (err)
		return err;

	err = drmm_add_action_or_reset(&xe->drm, pc_fini, pc);
	if (err)
		return err;

	return 0;
}

/**
 * xe_guc_pc_start - Start GuC's Power Conservation component
 * @pc: Xe_GuC_PC instance
 */
int xe_guc_pc_start(struct xe_guc_pc *pc)
{
	XE_WARN_ON(!xe_device_guc_submission_enabled(pc_to_xe(pc)));

	return pc_action_reset(pc);
}
