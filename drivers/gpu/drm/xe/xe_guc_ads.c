// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_bo.h"
#include "xe_gt.h"
#include "xe_guc_ads.h"
#include "xe_guc_reg.h"
#include "xe_hw_engine.h"
#include "xe_mmio.h"

static struct xe_guc *
ads_to_guc(struct xe_guc_ads *ads)
{
	return container_of(ads, struct xe_guc, ads);
}

static struct xe_gt *
ads_to_gt(struct xe_guc_ads *ads)
{
	return container_of(ads, struct xe_gt, uc.guc.ads);
}

static struct xe_device *
ads_to_xe(struct xe_guc_ads *ads)
{
	return gt_to_xe(ads_to_gt(ads));
}

static struct dma_buf_map *
ads_to_map(struct xe_guc_ads *ads)
{
	return &ads->bo->vmap;
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

#define ads_blob_read(ads_, field_)					\
	dma_buf_map_read_field(ads_to_map(ads_), struct __guc_ads_blob,	\
			       field_)

#define ads_blob_write(ads_, field_, val_)				\
	dma_buf_map_write_field(ads_to_map(ads_), struct __guc_ads_blob,\
				field_, val_)

#define info_map_write(map_, field_, val_) \
	dma_buf_map_write_field(map_, struct guc_gt_system_info, field_, val_)

#define info_map_read(map_, field_) \
	dma_buf_map_read_field(map_, struct guc_gt_system_info, field_)

static size_t guc_ads_regset_size(struct xe_guc_ads *ads)
{
	/* FIXME: Allocate a proper regset list */
	return PAGE_ALIGN(PAGE_SIZE);
}

static size_t guc_ads_golden_ctxt_size(struct xe_guc_ads *ads)
{
	/* FIXME: Allocate a proper golden context size */
	return PAGE_ALIGN(PAGE_SIZE * 4);
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

static void guc_policies_init(struct xe_guc_ads *ads)
{
	ads_blob_write(ads, policies.dpc_promote_time,
		       GLOBAL_POLICY_DEFAULT_DPC_PROMOTE_TIME_US);
	ads_blob_write(ads, policies.max_num_work_items,
		       GLOBAL_POLICY_MAX_NUM_WI);
	ads_blob_write(ads, policies.global_flags, 0);
	ads_blob_write(ads, policies.is_valid, 1);
}

static void fill_engine_enable_masks(struct xe_gt *gt,
				     struct dma_buf_map *info_map)
{
	info_map_write(info_map, engine_enabled_masks[GUC_RENDER_CLASS], 1);
	info_map_write(info_map, engine_enabled_masks[GUC_BLITTER_CLASS], 1);
	info_map_write(info_map, engine_enabled_masks[GUC_VIDEO_CLASS],
		       VDBOX_MASK(gt));
	info_map_write(info_map, engine_enabled_masks[GUC_VIDEOENHANCE_CLASS],
		       VEBOX_MASK(gt));
}

#define LR_HW_CONTEXT_SIZE (80 * sizeof(u32))
#define LRC_SKIP_SIZE (PAGE_SIZE + LR_HW_CONTEXT_SIZE)
static void guc_prep_golden_context(struct xe_guc_ads *ads)
{
	struct dma_buf_map info_map = DMA_BUF_MAP_INIT_OFFSET(ads_to_map(ads),
			offsetof(struct __guc_ads_blob, system_info));
	u8 guc_class;

	/* FIXME: Setting up dummy golden contexts */
	for (guc_class = 0; guc_class <= GUC_MAX_ENGINE_CLASSES;
	     ++guc_class) {
		if (!info_map_read(&info_map, engine_enabled_masks[guc_class]))
			continue;

		ads_blob_write(ads, ads.eng_state_size[guc_class],
			       guc_ads_golden_ctxt_size(ads) - LRC_SKIP_SIZE);
		ads_blob_write(ads, ads.golden_context_lrca[guc_class],
			       xe_bo_ggtt_addr(ads->bo) +
			       guc_ads_golden_ctxt_offset(ads));
	}
}

static u8 engine_class_to_guc_class(enum xe_engine_class class)
{
	switch (class) {
	case XE_ENGINE_CLASS_RENDER:
		return GUC_RENDER_CLASS;
	case XE_ENGINE_CLASS_VIDEO_DECODE:
		return GUC_VIDEO_CLASS;
	case XE_ENGINE_CLASS_VIDEO_ENHANCE:
		return GUC_VIDEOENHANCE_CLASS;
	case XE_ENGINE_CLASS_COPY:
		return GUC_BLITTER_CLASS;
	case XE_ENGINE_CLASS_OTHER:
	case XE_ENGINE_CLASS_COMPUTE:
	default:
		XE_WARN_ON(class);
		return -1;
	}
}

static void guc_mapping_table_init(struct xe_gt *gt,
				   struct dma_buf_map *info_map)
{
	unsigned int i, j;

	/* Table must be set to invalid values for entries not used */
	for (i = 0; i < GUC_MAX_ENGINE_CLASSES; ++i)
		for (j = 0; j < GUC_MAX_INSTANCES_PER_CLASS; ++j)
			info_map_write(info_map, mapping_table[i][j],
				       GUC_MAX_INSTANCES_PER_CLASS);

	/* FIXME: Setting table up with 1 to 1 to get GuC to load */
	for (i = 0; i < ARRAY_SIZE(gt->hw_engines); i++) {
		struct xe_hw_engine *hwe = &gt->hw_engines[i];
		u8 guc_class;

		if (!xe_hw_engine_is_valid(hwe))
			continue;

		guc_class = engine_class_to_guc_class(hwe->class);
		info_map_write(info_map,
			       mapping_table[guc_class][hwe->instance],
			       hwe->instance);
	}
}

static void guc_capture_list_init(struct xe_guc_ads *ads)
{
	int i, j;
	u32 addr = xe_bo_ggtt_addr(ads->bo) + guc_ads_capture_offset(ads);

	/* FIXME: Populate a proper capture list */
	for (i = 0; i < GUC_CAPTURE_LIST_INDEX_MAX; i++) {
		for (j = 0; j < GUC_MAX_ENGINE_CLASSES; j++) {
			ads_blob_write(ads, ads.capture_instance[i][j], addr);
			ads_blob_write(ads, ads.capture_class[i][j], addr);
		}

		ads_blob_write(ads, ads.capture_global[i], addr);
	}
}

static void guc_mmio_reg_state_init(struct xe_guc_ads *ads)
{
	int i, j;
	u32 addr = xe_bo_ggtt_addr(ads->bo) + guc_ads_regset_offset(ads);

	/* FIXME: Populate a proper reg state list */
	for (i = 0; i < GUC_MAX_ENGINE_CLASSES; ++i) {
		for (j = 0; j < GUC_MAX_INSTANCES_PER_CLASS; ++j) {
			ads_blob_write(ads, ads.reg_state_list[i][j].address,
				       addr);
			ads_blob_write(ads, ads.reg_state_list[i][j].count, 0);
		}
	}
}

static void guc_ads_private_data_reset(struct xe_guc_ads *ads)
{
	struct dma_buf_map map =
		DMA_BUF_MAP_INIT_OFFSET(ads_to_map(ads),
					guc_ads_private_data_offset(ads));
	u32 size;

	size = guc_ads_private_data_size(ads);
	if (!size)
		return;

	dma_buf_map_memset(&map, 0, size);
}

void xe_guc_ads_populate(struct xe_guc_ads *ads)
{
	struct xe_device *xe = ads_to_xe(ads);
	struct xe_gt *gt = ads_to_gt(ads);
	struct dma_buf_map info_map = DMA_BUF_MAP_INIT_OFFSET(ads_to_map(ads),
			offsetof(struct __guc_ads_blob, system_info));
	u32 base = xe_bo_ggtt_addr(ads->bo);

	XE_BUG_ON(!ads->bo);

	guc_policies_init(ads);
	fill_engine_enable_masks(gt, &info_map);
	guc_prep_golden_context(ads);
	guc_mapping_table_init(gt, &info_map);
	guc_capture_list_init(ads);
	guc_mmio_reg_state_init(ads);

	if (GRAPHICS_VER(xe) >= 12 && !IS_DGFX(xe)) {
		u32 distdbreg =
			xe_mmio_read32(gt, GEN12_DIST_DBS_POPULATED.reg);

		ads_blob_write(ads,
			       system_info.generic_gt_sysinfo[GUC_GENERIC_GT_SYSINFO_DOORBELL_COUNT_PER_SQIDI],
			       ((distdbreg >> GEN12_DOORBELLS_PER_SQIDI_SHIFT)
				& GEN12_DOORBELLS_PER_SQIDI) + 1);
	}

	ads_blob_write(ads, ads.scheduler_policies, base +
		       offsetof(struct __guc_ads_blob, policies));
	ads_blob_write(ads, ads.gt_system_info, base +
		       offsetof(struct __guc_ads_blob, system_info));
	ads_blob_write(ads, ads.private_data, base +
		       guc_ads_private_data_offset(ads));

	guc_ads_private_data_reset(ads);
}

void xe_guc_ads_fini(struct xe_guc_ads *ads)
{
	xe_bo_unpin_map_no_vm(ads->bo);
}
