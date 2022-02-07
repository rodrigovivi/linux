// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_bo.h"
#include "xe_gt.h"
#include "xe_guc_log.h"

static struct xe_gt *
log_to_gt(struct xe_guc_log *log)
{
	return container_of(log, struct xe_gt, uc.guc.log);
}

static struct xe_device *
log_to_xe(struct xe_guc_log *log)
{
	return gt_to_xe(log_to_gt(log));
}

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

void xe_guc_log_dump(struct xe_guc_log *log, struct drm_printer *p)
{
	struct dma_buf_map map;
	size_t size;
	int i, j;

	XE_BUG_ON(!log->bo);

	map = log->bo->vmap;
	size = log->bo->size;

#define DW_PER_PRINT		4
	XE_BUG_ON(size % (DW_PER_PRINT * sizeof(u32)));
	for (i = 0; i < size / sizeof(u32); i += DW_PER_PRINT) {
		u32 read[DW_PER_PRINT];

		for (j = 0; j < DW_PER_PRINT; ++j) {
			read[j] = dbm_read32(map);
			dma_buf_map_incr(&map, sizeof(u32));
		}

		drm_printf(p, "0x%08x 0x%08x 0x%08x 0x%08x\n",
			   *(read + 0), *(read + 1),
			   *(read + 2), *(read + 3));
	}
#undef DW_PER_PRINT
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

	dma_buf_map_memset(&bo->vmap, 0, guc_log_size());
	log->bo = bo;
	log->level = 5;	/* FIXME: Connect to modparam / debugfs */

	return 0;
}

void xe_guc_log_fini(struct xe_guc_log *log)
{
	xe_bo_unpin_map_no_vm(log->bo);
}
