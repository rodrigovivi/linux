// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_device.h"
#include "xe_bo.h"
#include "xe_guc_ads.h"
#include "xe_guc_types.h"
#include "xe_guc_fwif.h"

static struct xe_device *
ads_to_xe(struct xe_guc_ads *ads)
{
	return container_of(ads, struct xe_device, uc.guc.ads);
}

static struct xe_guc *
ads_to_guc(struct xe_guc_ads *ads)
{
	return container_of(ads, struct xe_guc, ads);
}

/*
 * The Additional Data Struct (ADS) has pointers for different buffers used by
 * the GuC. One single gem object contains the ADS struct itself (guc_ads) and
 * all the extra buffers indirectly linked via the ADS struct's entries.
 *
 * Layout of the ADS blob allocated for the GuC:
 *
 *      +---------------------------------------+ <== base
 *      | guc_ads                               |
 *      +---------------------------------------+
 *      | guc_policies                          |
 *      +---------------------------------------+
 *      | guc_gt_system_info                    |
 *      +---------------------------------------+
 *      | guc_engine_usage                      |
 *      +---------------------------------------+ <== static
 *      | guc_mmio_reg[countA] (engine 0.0)     |
 *      | guc_mmio_reg[countB] (engine 0.1)     |
 *      | guc_mmio_reg[countC] (engine 1.0)     |
 *      |   ...                                 |
 *      +---------------------------------------+ <== dynamic
 *      | padding                               |
 *      +---------------------------------------+ <== 4K aligned
 *      | golden contexts                       |
 *      +---------------------------------------+
 *      | padding                               |
 *      +---------------------------------------+ <== 4K aligned
 *      | capture lists                         |
 *      +---------------------------------------+
 *      | padding                               |
 *      +---------------------------------------+ <== 4K aligned
 *      | private data                          |
 *      +---------------------------------------+
 *      | padding                               |
 *      +---------------------------------------+ <== 4K aligned
 */
struct __guc_ads_blob {
	struct guc_ads ads;
	struct guc_policies policies;
	struct guc_gt_system_info system_info;
	struct guc_engine_usage engine_usage;
	/* From here on, location is dynamic! Refer to above diagram. */
	struct guc_mmio_reg regset[0];
} __packed;

static size_t guc_ads_regset_size(struct xe_guc_ads *ads)
{
	/* FIXME: Allocate a proper regset list */
	return PAGE_ALIGN(PAGE_SIZE);
}

static size_t guc_ads_golden_ctxt_size(struct xe_guc_ads *ads)
{
	/* FIXME: Allocate a proper golden context size */
	return PAGE_ALIGN(PAGE_SIZE);
}

static size_t guc_ads_capture_size(struct xe_guc_ads *ads)
{
	/* FIXME: Allocate a proper capture list */
	return PAGE_ALIGN(PAGE_SIZE);
}

static size_t guc_ads_private_data_size(struct xe_guc_ads *ads)
{
	return PAGE_ALIGN(ads_to_guc(ads)->fw.private_data_size);
}

static size_t guc_ads_regset_offset(struct xe_guc_ads *ads)
{
	return offsetof(struct __guc_ads_blob, regset);
}

static size_t guc_ads_golden_ctxt_offset(struct xe_guc_ads *ads)
{
	size_t offset;

	offset = guc_ads_regset_offset(ads) +
		guc_ads_regset_size(ads);

	return PAGE_ALIGN(offset);
}

static size_t guc_ads_capture_offset(struct xe_guc_ads *ads)
{
	size_t offset;

	offset = guc_ads_golden_ctxt_offset(ads) +
		guc_ads_golden_ctxt_size(ads);

	return PAGE_ALIGN(offset);
}

static size_t guc_ads_private_data_offset(struct xe_guc_ads *ads)
{
	size_t offset;

	offset = guc_ads_capture_offset(ads) +
		guc_ads_capture_size(ads);

	return PAGE_ALIGN(offset);
}

static size_t guc_ads_size(struct xe_guc_ads *ads)
{
	return guc_ads_private_data_offset(ads) +
		guc_ads_private_data_size(ads);
}

int xe_guc_ads_init(struct xe_guc_ads *ads)
{
	struct xe_device *xe = ads_to_xe(ads);
	struct xe_bo *bo;

	bo = xe_bo_create_pin_map(xe, NULL, guc_ads_size(ads),
				  ttm_bo_type_kernel,
				  XE_BO_CREATE_VRAM_IF_DGFX(xe) |
				  XE_BO_CREATE_GGTT_BIT);
	if (IS_ERR(bo))
		return PTR_ERR(bo);

	ads->bo = bo;

	return 0;
}

void xe_guc_ads_fini(struct xe_guc_ads *ads)
{
	xe_bo_unpin_map_no_vm(ads->bo);
}
