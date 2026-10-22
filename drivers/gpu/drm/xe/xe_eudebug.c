// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023-2025 Intel Corporation
 */

#include <linux/debugfs.h>
#include <linux/workqueue.h>

#include <drm/drm_managed.h>

#include "xe_device.h"
#include "xe_eudebug.h"
#include "xe_eudebug_hw.h"
#include "xe_eudebug_types.h"
#include "xe_gt.h"
#include "xe_oa.h"
#include "xe_pm.h"
#include "xe_sriov.h"
#include "xe_sriov_pf.h"

/**
 * DOC: EU debug mode
 *
 * In EU debug mode the render and compute engines run with the thread
 * debug controls in TD_CTL enabled, together with the workarounds that
 * debugging needs. A breakpoint or an exception then halts the EU thread
 * that hit it and raises attention, instead of being ignored.
 *
 * The mode is off by default and is toggled per device. The new state is
 * applied through a GT reset. While no debugger interface is merged, the
 * only way to toggle it is the eudebug file in the device debugfs
 * directory.
 */

bool xe_eudebug_is_enabled(struct xe_device *xe)
{
	return READ_ONCE(xe->eudebug.cap_state) == XE_EUDEBUG_CAP_ENABLED;
}

struct eudebug_hw_update {
	struct work_struct work;
	struct xe_gt *gt;
	bool enable;
};

static void eudebug_hw_update_work(struct work_struct *w)
{
	struct eudebug_hw_update *u = container_of(w, typeof(*u), work);
	struct xe_gt *gt = u->gt;
	int i;

	for (i = 0; i < ARRAY_SIZE(gt->hw_engines); i++) {
		if (!(gt->info.engine_mask & BIT(i)))
			continue;

		xe_eudebug_init_hw_engine(&gt->hw_engines[i], u->enable);
	}

	/*
	 * If a reset is already pending this does not queue another one,
	 * but that reset is queued behind us and applies the new state.
	 */
	xe_gt_reset_async(gt);
}

/*
 * The new state only reaches the hardware through a reset applying
 * hwe->reg_sr. Update the lists from gt->ordered_wq, which is where
 * gt_reset_worker() runs, so that no reset can be walking them meanwhile,
 * and so that a reset already in flight, which would make
 * xe_gt_reset_async() a no-op, has either finished before the update or
 * runs after it.
 */
static void eudebug_hw_update(struct xe_gt *gt, bool enable)
{
	struct eudebug_hw_update u = {
		.gt = gt,
		.enable = enable,
	};

	INIT_WORK_ONSTACK(&u.work, eudebug_hw_update_work);
	queue_work(gt->ordered_wq, &u.work);
	flush_work(&u.work);
	destroy_work_on_stack(&u.work);

	flush_work(&gt->reset.worker);
}

/**
 * xe_eudebug_enable - Enable or disable EU debug mode
 * @xe: the &xe_device
 * @enable: the new state
 *
 * EU debug mode and SR-IOV are exclusive: on a PF, enabling it locks
 * down VF enabling until it is disabled again. It is also exclusive with
 * OA streams.
 *
 * Return: 0 on success, -EPERM if the device does not support EU debug,
 * -EBUSY if an OA stream is open, or a negative error code if VFs are
 * enabled.
 */
int xe_eudebug_enable(struct xe_device *xe, bool enable)
{
	struct xe_gt *gt;
	int ret;
	u8 id;

	guard(mutex)(&xe->eudebug.lock);

	if (xe->eudebug.cap_state == XE_EUDEBUG_CAP_NOT_SUPPORTED)
		return -EPERM;

	if (enable == xe_eudebug_is_enabled(xe))
		return 0;

	/*
	 * eudebug and OA are mutually exclusive: both own EU state, see
	 * xe_oa_stream_open_ioctl_locked(). Publish the new state before
	 * looking for streams, and take it back if one is found. An OA open
	 * racing this is then either seen here, because it took the gt_lock
	 * first and published its stream, or sees eudebug enabled under that
	 * same lock and is refused.
	 */
	if (enable) {
		WRITE_ONCE(xe->eudebug.cap_state, XE_EUDEBUG_CAP_ENABLED);
		if (xe_oa_stream_active(xe)) {
			WRITE_ONCE(xe->eudebug.cap_state, XE_EUDEBUG_CAP_DISABLED);
			drm_dbg(&xe->drm, "Can't enable eudebug: OA stream is open\n");
			return -EBUSY;
		}
	}

	if (enable && IS_SRIOV_PF(xe)) {
		ret = xe_sriov_pf_lockdown(xe);
		if (ret) {
			/* Take back what the check above published */
			WRITE_ONCE(xe->eudebug.cap_state, XE_EUDEBUG_CAP_DISABLED);
			drm_dbg(&xe->drm, "Can't enable eudebug: VFs are enabled\n");
			return ret;
		}
	}

	xe_pm_runtime_get(xe);
	for_each_gt(gt, xe, id) {
		/* No engine there to program, so no reason to reset it */
		if (xe_gt_is_media_type(gt))
			continue;

		eudebug_hw_update(gt, enable);
	}
	xe_pm_runtime_put(xe);

	WRITE_ONCE(xe->eudebug.cap_state, enable ?
		   XE_EUDEBUG_CAP_ENABLED : XE_EUDEBUG_CAP_DISABLED);

	if (!enable && IS_SRIOV_PF(xe))
		xe_sriov_pf_end_lockdown(xe);

	return 0;
}

static ssize_t eudebug_show(struct file *f, char __user *ubuf,
			    size_t size, loff_t *pos)
{
	struct xe_device *xe = file_inode(f)->i_private;
	char buf[8];
	int len;

	len = scnprintf(buf, sizeof(buf), "%u\n", xe_eudebug_is_enabled(xe));

	return simple_read_from_buffer(ubuf, size, pos, buf, len);
}

static ssize_t eudebug_set(struct file *f, const char __user *ubuf,
			   size_t size, loff_t *pos)
{
	struct xe_device *xe = file_inode(f)->i_private;
	bool enable;
	int ret;

	ret = kstrtobool_from_user(ubuf, size, &enable);
	if (ret)
		return ret;

	ret = xe_eudebug_enable(xe, enable);
	if (ret)
		return ret;

	return size;
}

static const struct file_operations eudebug_fops = {
	.owner = THIS_MODULE,
	.read = eudebug_show,
	.write = eudebug_set,
};

/**
 * xe_eudebug_init_early - Initialize EU debug mode state
 * @xe: the &xe_device
 */
void xe_eudebug_init_early(struct xe_device *xe)
{
	struct drm_device *dev = &xe->drm;
	int err;

	WRITE_ONCE(xe->eudebug.cap_state, XE_EUDEBUG_CAP_NOT_SUPPORTED);

	err = drmm_mutex_init(dev, &xe->eudebug.lock);
	if (err) {
		drm_warn(&xe->drm, "eudebug disabled, early init fail: %d\n", err);
	} else if (IS_SRIOV_VF(xe)) {
		drm_dbg(&xe->drm, "eudebug not available in SR-IOV VF mode\n");
	} else if (GRAPHICS_VER(xe) >= 35) {
		/* debug register layouts are only known up to Xe3 */
		drm_dbg(&xe->drm, "eudebug not supported on graphics version %u\n",
			GRAPHICS_VER(xe));
	} else {
		WRITE_ONCE(xe->eudebug.cap_state, XE_EUDEBUG_CAP_DISABLED);
	}
}

/**
 * xe_eudebug_init - Initialize EU debug mode
 * @xe: the &xe_device
 */
void xe_eudebug_init(struct xe_device *xe)
{
	/* early init failed */
	if (xe->eudebug.cap_state == XE_EUDEBUG_CAP_NOT_SUPPORTED)
		return;

	/* Last: once the file exists it can enable us */
	debugfs_create_file("eudebug", 0600, xe->drm.primary->debugfs_root,
			    xe, &eudebug_fops);
}

#if IS_ENABLED(CONFIG_DRM_XE_KUNIT_TEST)
#include "tests/xe_eudebug_test.c"
#endif
