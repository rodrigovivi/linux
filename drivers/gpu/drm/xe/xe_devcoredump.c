// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include "xe_devcoredump.h"
#include "xe_devcoredump_types.h"

#include <linux/devcoredump.h>
#include <generated/utsrelease.h>

#include "xe_engine.h"
#include "xe_gt.h"

/**
 * DOC: Xe device coredump
 *
 * Devices overview:
 * Xe uses dev_coredump infrastructure for exposing the crash errors in a
 * standardized way.
 * devcoredump exposes a temporary device under /sys/class/devcoredump/
 * which is linked with our card device directly.
 * The core dump can be accessed either from
 * /sys/class/drm/card<n>/device/devcoredump/ or from
 * /sys/class/devcoredump/devcd<m> where
 * /sys/class/devcoredump/devcd<m>/failing_device is a link to
 * /sys/class/drm/card<n>/device/.
 *
 * Snapshot at hang:
 * The 'data' file is printed with a drm_printer pointer at devcoredump read
 * time. For this reason, we need to take snapshots from when the hang has
 * happened, and not only when the user is reading the file. Otherwise the
 * information is outdated since the resets might have happened in between.
 *
 * 'First' failure snapshot:
 * In general, the first hang is the most critical one since the following hangs
 * can be a consequence of the initial hang. For this reason we only take the
 * snapshot of the 'first' failure and ignore subsequent calls of this function,
 * at least while the coredump device is alive. Dev_coredump has a delayed work
 * queue that will eventually delete the device and free all the dump
 * information. At this time we also clear the faulty_engine and allow the next
 * hang capture.
 */

static ssize_t xe_devcoredump_read(char *buffer, loff_t offset,
				   size_t count, void *data, size_t datalen)
{
	struct xe_devcoredump *coredump = data;
	struct xe_devcoredump_snapshot *ss;
	struct drm_printer p;
	struct drm_print_iterator iter;
	struct timespec64 ts;

	iter.data = buffer;
	iter.offset = 0;
	iter.start = offset;
	iter.remain = count;

	mutex_lock(&coredump->lock);

	ss = &coredump->snapshot;
	p = drm_coredump_printer(&iter);

	drm_printf(&p, "**** Xe Device Coredump ****\n");
	drm_printf(&p, "kernel: " UTS_RELEASE "\n");
	drm_printf(&p, "module: " KBUILD_MODNAME "\n");

	ts = ktime_to_timespec64(ss->snapshot_time);
	drm_printf(&p, "Snapshot time: %lld.%09ld\n", ts.tv_sec, ts.tv_nsec);
	ts = ktime_to_timespec64(ss->boot_time);
	drm_printf(&p, "Boot time: %lld.%09ld\n", ts.tv_sec, ts.tv_nsec);
	ts = ktime_to_timespec64(ktime_sub(ss->snapshot_time, ss->boot_time));
	drm_printf(&p, "Uptime: %lld.%09ld\n", ts.tv_sec, ts.tv_nsec);

	mutex_unlock(&coredump->lock);

	return count - iter.remain;
}

static void xe_devcoredump_free(void *data)
{
	struct xe_devcoredump *coredump = data;
	struct xe_device *xe = container_of(coredump, struct xe_device,
					    devcoredump);
	mutex_lock(&coredump->lock);

	coredump->faulty_engine = NULL;
	drm_info(&xe->drm, "Xe device coredump has been deleted.\n");

	mutex_unlock(&coredump->lock);
}

static void devcoredump_snapshot(struct xe_devcoredump *coredump)
{
	struct xe_devcoredump_snapshot *ss = &coredump->snapshot;

	lockdep_assert_held(&coredump->lock);
	ss->snapshot_time = ktime_get_real();
	ss->boot_time = ktime_get_boottime();
}

/**
 * xe_devcoredump - Take the required snapshots and initialize coredump device.
 * @e: The faulty xe_engine, where the issue was detected.
 *
 * This function should be called at the crash time. It is skipped if we still
 * have the core dump device available with the information of the 'first'
 * snapshot.
 */
void xe_devcoredump(struct xe_engine *e)
{
	struct xe_device *xe = gt_to_xe(e->gt);
	struct xe_devcoredump *coredump = &xe->devcoredump;
	bool cookie;

	cookie = dma_fence_begin_signalling();
	mutex_lock(&coredump->lock);

	if (coredump->faulty_engine) {
		drm_dbg(&xe->drm, "Multiple hangs are occuring, but only the first snapshot was taken\n");
		mutex_unlock(&coredump->lock);
		return;
	}
	coredump->faulty_engine = e;
	devcoredump_snapshot(coredump);

	mutex_unlock(&coredump->lock);
	dma_fence_end_signalling(cookie);

	drm_info(&xe->drm, "Xe device coredump has been created\n");
	drm_info(&xe->drm, "Check your /sys/class/drm/card%d/device/devcoredump/data\n",
		 xe->drm.primary->index);

	dev_coredumpm(xe->drm.dev, THIS_MODULE, coredump, 0, GFP_KERNEL,
		      xe_devcoredump_read, xe_devcoredump_free);
}

/**
 * xe_devcoredump_init - Initialize xe_devcoredump.
 * @xe: Xe device.
 *
 * This function should be called at the probe so the mutex lock can be
 * initialized.
 */
void xe_devcoredump_init(struct xe_device *xe)
{
	struct xe_devcoredump *coredump = &xe->devcoredump;

	mutex_init(&coredump->lock);
}
