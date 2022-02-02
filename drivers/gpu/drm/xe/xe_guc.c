// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_device.h"
#include "xe_bo.h"
#include "xe_guc.h"
#include "xe_guc_ads.h"
#include "xe_guc_log.h"
#include "xe_guc_reg.h"
#include "xe_uc_fw.h"
#include "xe_wopcm.h"
#include "xe_mmio.h"
#include "xe_force_wake.h"

static struct xe_device *
guc_to_xe(struct xe_guc *guc)
{
	return container_of(guc, struct xe_device, uc.guc);
}

/* GuC addresses above GUC_GGTT_TOP also don't map through the GTT */
#define GUC_GGTT_TOP    0xFEE00000
static u32 guc_bo_ggtt_addr(struct xe_guc *guc,
			    struct xe_bo *bo)
{
	u32 addr = xe_bo_ggtt_addr(bo);

	XE_BUG_ON(addr < xe_wopcm_size(guc_to_xe(guc)));
	XE_BUG_ON(range_overflows_t(u32, addr, bo->size, GUC_GGTT_TOP));

	return addr;
}

static u32 guc_ctl_debug_flags(struct xe_guc *guc)
{
	u32 level = xe_guc_log_get_level(&guc->log);
	u32 flags = 0;

	if (!GUC_LOG_LEVEL_IS_VERBOSE(level))
		flags |= GUC_LOG_DISABLED;
	else
		flags |= GUC_LOG_LEVEL_TO_VERBOSITY(level) <<
			 GUC_LOG_VERBOSITY_SHIFT;

	return flags;
}

static u32 guc_ctl_feature_flags(struct xe_guc *guc)
{
	/* FIXME: Just loading the GuC for now, disable submission */
	return GUC_CTL_DISABLE_SCHEDULER;
}

static u32 guc_ctl_log_params_flags(struct xe_guc *guc)
{
	u32 offset = guc_bo_ggtt_addr(guc, guc->log.bo) >> PAGE_SHIFT;
	u32 flags;

	#if (((CRASH_BUFFER_SIZE) % SZ_1M) == 0)
	#define LOG_UNIT SZ_1M
	#define LOG_FLAG GUC_LOG_LOG_ALLOC_UNITS
	#else
	#define LOG_UNIT SZ_4K
	#define LOG_FLAG 0
	#endif

	#if (((CAPTURE_BUFFER_SIZE) % SZ_1M) == 0)
	#define CAPTURE_UNIT SZ_1M
	#define CAPTURE_FLAG GUC_LOG_CAPTURE_ALLOC_UNITS
	#else
	#define CAPTURE_UNIT SZ_4K
	#define CAPTURE_FLAG 0
	#endif

	BUILD_BUG_ON(!CRASH_BUFFER_SIZE);
	BUILD_BUG_ON(!IS_ALIGNED(CRASH_BUFFER_SIZE, LOG_UNIT));
	BUILD_BUG_ON(!DEBUG_BUFFER_SIZE);
	BUILD_BUG_ON(!IS_ALIGNED(DEBUG_BUFFER_SIZE, LOG_UNIT));
	BUILD_BUG_ON(!CAPTURE_BUFFER_SIZE);
	BUILD_BUG_ON(!IS_ALIGNED(CAPTURE_BUFFER_SIZE, CAPTURE_UNIT));

	BUILD_BUG_ON((CRASH_BUFFER_SIZE / LOG_UNIT - 1) >
			(GUC_LOG_CRASH_MASK >> GUC_LOG_CRASH_SHIFT));
	BUILD_BUG_ON((DEBUG_BUFFER_SIZE / LOG_UNIT - 1) >
			(GUC_LOG_DEBUG_MASK >> GUC_LOG_DEBUG_SHIFT));
	BUILD_BUG_ON((CAPTURE_BUFFER_SIZE / CAPTURE_UNIT - 1) >
			(GUC_LOG_CAPTURE_MASK >> GUC_LOG_CAPTURE_SHIFT));

	flags = GUC_LOG_VALID |
		GUC_LOG_NOTIFY_ON_HALF_FULL |
		CAPTURE_FLAG |
		LOG_FLAG |
		((CRASH_BUFFER_SIZE / LOG_UNIT - 1) << GUC_LOG_CRASH_SHIFT) |
		((DEBUG_BUFFER_SIZE / LOG_UNIT - 1) << GUC_LOG_DEBUG_SHIFT) |
		((CAPTURE_BUFFER_SIZE / CAPTURE_UNIT - 1) << GUC_LOG_CAPTURE_SHIFT) |
		(offset << GUC_LOG_BUF_ADDR_SHIFT);

	#undef LOG_UNIT
	#undef LOG_FLAG
	#undef CAPTURE_UNIT
	#undef CAPTURE_FLAG

	return flags;
}

static u32 guc_ctl_ads_flags(struct xe_guc *guc)
{
	u32 ads = guc_bo_ggtt_addr(guc, guc->ads.bo) >> PAGE_SHIFT;
	u32 flags = ads << GUC_ADS_ADDR_SHIFT;

	return flags;
}

static u32 guc_ctl_wa_flags(struct xe_guc *guc)
{
	struct xe_device *xe = guc_to_xe(guc);
	u32 flags = 0;

	/* Wa_22012773006:gen11,gen12 < XeHP */
	if (GRAPHICS_VER(xe) >= 11 &&
	    GRAPHICS_VERx10(xe) < 125)
		flags |= GUC_WA_POLLCS;

	return flags;
}

static u32 guc_ctl_devid(struct xe_guc *guc)
{
	struct xe_device *xe = guc_to_xe(guc);

	return (((u32)xe->info.devid) << 16) | xe->info.revid;
}

static void guc_init_params(struct xe_guc *guc)
{
	struct xe_device *xe = guc_to_xe(guc);
	u32 *params = guc->params;
	int i;

	BUILD_BUG_ON(sizeof(guc->params) != GUC_CTL_MAX_DWORDS * sizeof(u32));
	BUILD_BUG_ON(SOFT_SCRATCH_COUNT != GUC_CTL_MAX_DWORDS + 2);

	params[GUC_CTL_LOG_PARAMS] = guc_ctl_log_params_flags(guc);
	params[GUC_CTL_FEATURE] = guc_ctl_feature_flags(guc);
	params[GUC_CTL_DEBUG] = guc_ctl_debug_flags(guc);
	params[GUC_CTL_ADS] = guc_ctl_ads_flags(guc);
	params[GUC_CTL_WA] = guc_ctl_wa_flags(guc);
	params[GUC_CTL_DEVID] = guc_ctl_devid(guc);

	for (i = 0; i < GUC_CTL_MAX_DWORDS; i++)
		drm_dbg(&xe->drm, "GuC param[%2d] = 0x%08x\n", i, params[i]);
}

int xe_guc_init(struct xe_guc *guc)
{
	struct xe_device *xe = guc_to_xe(guc);
	int ret;

	guc->fw.type = XE_UC_FW_TYPE_GUC;
	ret = xe_uc_fw_init(&guc->fw);
	if (ret)
		goto out;

	ret = xe_guc_log_init(&guc->log);
	if (ret)
		goto err_fw;

	ret = xe_guc_ads_init(&guc->ads);
	if (ret)
		goto err_log;

	guc_init_params(guc);

	xe_uc_fw_change_status(&guc->fw, XE_UC_FIRMWARE_LOADABLE);

	return 0;

err_log:
	xe_guc_log_fini(&guc->log);
err_fw:
	xe_uc_fw_fini(&guc->fw);
out:
	drm_err(&xe->drm, "GuC init failed with %d", ret);
	return ret;
}

int xe_guc_reset(struct xe_guc *guc)
{
	struct xe_device *xe = guc_to_xe(guc);
	u32 guc_status;
	int ret;
	bool cookie;

	cookie = dma_fence_begin_signalling();
	xe_force_wake_assert_held(&xe->fw, XE_FW_GT);

	xe_mmio_write32(xe, GEN6_GDRST.reg, GEN11_GRDOM_GUC);

	ret = xe_mmio_wait32(xe, GEN6_GDRST.reg, 0, GEN11_GRDOM_GUC, 5);
	if (ret) {
		drm_err(&xe->drm, "GuC reset timed out, GEN6_GDRST=0x%8x\n",
			xe_mmio_read32(xe, GEN6_GDRST.reg));
		goto err_out;
	}

	guc_status = xe_mmio_read32(xe, GUC_STATUS.reg);
	if (!(guc_status & GS_MIA_IN_RESET)) {
		drm_err(&xe->drm,
			"GuC status: 0x%x, MIA core expected to be in reset\n",
			guc_status);
		ret = -EIO;
		goto err_out;
	}

	dma_fence_end_signalling(cookie);
	return 0;

err_out:
	dma_fence_end_signalling(cookie);

	return ret;
}

void xe_guc_fini(struct xe_guc *guc)
{
	if (!xe_uc_fw_is_loadable(&guc->fw))
		return;

	xe_guc_ads_fini(&guc->ads);
	xe_guc_log_fini(&guc->log);
	xe_uc_fw_fini(&guc->fw);
}
