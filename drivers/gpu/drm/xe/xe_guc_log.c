// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_device.h"
#include "xe_bo.h"
#include "xe_guc_log.h"

static size_t guc_log_size(void)
{
	/*
	 *  GuC Log buffer Layout
	 *
	 *  +===============================+ 00B
	 *  |    Crash dump state header    |
	 *  +-------------------------------+ 32B
	 *  |      Debug state header       |
	 *  +-------------------------------+ 64B
	 *  |     Capture state header      |
	 *  +-------------------------------+ 96B
	 *  |                               |
	 *  +===============================+ PAGE_SIZE (4KB)
	 *  |        Crash Dump logs        |
	 *  +===============================+ + CRASH_SIZE
	 *  |          Debug logs           |
	 *  +===============================+ + DEBUG_SIZE
	 *  |         Capture logs          |
	 *  +===============================+ + CAPTURE_SIZE
	 */
	return PAGE_SIZE + CRASH_BUFFER_SIZE + DEBUG_BUFFER_SIZE +
		CAPTURE_BUFFER_SIZE;
}

static struct xe_device *
log_to_xe(struct xe_guc_log *log)
{
	return container_of(log, struct xe_device, uc.guc.log);
}

int xe_guc_log_init(struct xe_guc_log *log)
{
	struct xe_device *xe = log_to_xe(log);
	struct xe_bo *bo;

	bo = xe_bo_create_pin_map(xe, NULL, guc_log_size(),
				  ttm_bo_type_kernel,
				  XE_BO_CREATE_VRAM_IF_DGFX(xe) |
				  XE_BO_CREATE_GGTT_BIT);
	if (IS_ERR(bo))
		return PTR_ERR(bo);

	log->bo = bo;
	log->level = 5;	/* FIXME: Connect to modparam / debugfs */

	return 0;
}

void xe_guc_log_fini(struct xe_guc_log *log)
{
	xe_bo_unpin_map_no_vm(log->bo);
}
