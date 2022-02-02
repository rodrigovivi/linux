/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GUC_FWIF_H
#define _XE_GUC_FWIF_H

#define GUC_RENDER_ENGINE		0
#define GUC_VIDEO_ENGINE		1
#define GUC_BLITTER_ENGINE		2
#define GUC_VIDEOENHANCE_ENGINE		3
#define GUC_VIDEO_ENGINE2		4
#define GUC_MAX_ENGINES_NUM		(GUC_VIDEO_ENGINE2 + 1)

#define GUC_RENDER_CLASS		0
#define GUC_VIDEO_CLASS			1
#define GUC_VIDEOENHANCE_CLASS		2
#define GUC_BLITTER_CLASS		3
#define GUC_RESERVED_CLASS		4
#define GUC_LAST_ENGINE_CLASS		GUC_RESERVED_CLASS
#define GUC_MAX_ENGINE_CLASSES		16
#define GUC_MAX_INSTANCES_PER_CLASS	32

/* GUC_CTL_* - Parameters for loading the GuC */
#define GUC_CTL_LOG_PARAMS		0
#define   GUC_LOG_VALID			BIT(0)
#define   GUC_LOG_NOTIFY_ON_HALF_FULL	BIT(1)
#define   GUC_LOG_CAPTURE_ALLOC_UNITS	BIT(2)
#define   GUC_LOG_LOG_ALLOC_UNITS	BIT(3)
#define   GUC_LOG_CRASH_SHIFT		4
#define   GUC_LOG_CRASH_MASK		(0x3 << GUC_LOG_CRASH_SHIFT)
#define   GUC_LOG_DEBUG_SHIFT		6
#define   GUC_LOG_DEBUG_MASK	        (0xF << GUC_LOG_DEBUG_SHIFT)
#define   GUC_LOG_CAPTURE_SHIFT		10
#define   GUC_LOG_CAPTURE_MASK	        (0x3 << GUC_LOG_CAPTURE_SHIFT)
#define   GUC_LOG_BUF_ADDR_SHIFT	12

#define GUC_CTL_WA			1
#define   GUC_WA_POLLCS                 BIT(18)

#define GUC_CTL_FEATURE			2
#define   GUC_CTL_ENABLE_SLPC		BIT(2)
#define   GUC_CTL_DISABLE_SCHEDULER	BIT(14)

#define GUC_CTL_DEBUG			3
#define   GUC_LOG_VERBOSITY_SHIFT	0
#define   GUC_LOG_VERBOSITY_LOW		(0 << GUC_LOG_VERBOSITY_SHIFT)
#define   GUC_LOG_VERBOSITY_MED		(1 << GUC_LOG_VERBOSITY_SHIFT)
#define   GUC_LOG_VERBOSITY_HIGH	(2 << GUC_LOG_VERBOSITY_SHIFT)
#define   GUC_LOG_VERBOSITY_ULTRA	(3 << GUC_LOG_VERBOSITY_SHIFT)
#define	  GUC_LOG_VERBOSITY_MIN		0
#define	  GUC_LOG_VERBOSITY_MAX		3
#define	  GUC_LOG_VERBOSITY_MASK	0x0000000f
#define	  GUC_LOG_DESTINATION_MASK	(3 << 4)
#define   GUC_LOG_DISABLED		(1 << 6)
#define   GUC_PROFILE_ENABLED		(1 << 7)

#define GUC_CTL_ADS			4
#define   GUC_ADS_ADDR_SHIFT		1
#define   GUC_ADS_ADDR_MASK		(0xFFFFF << GUC_ADS_ADDR_SHIFT)

#define GUC_CTL_DEVID			5

#define GUC_CTL_MAX_DWORDS		14

struct guc_policies {
	u32 submission_queue_depth[GUC_MAX_ENGINE_CLASSES];
	/* In micro seconds. How much time to allow before DPC processing is
	 * called back via interrupt (to prevent DPC queue drain starving).
	 * Typically 1000s of micro seconds (example only, not granularity). */
	u32 dpc_promote_time;

	/* Must be set to take these new values. */
	u32 is_valid;

	/* Max number of WIs to process per call. A large value may keep CS
	 * idle. */
	u32 max_num_work_items;

	u32 global_flags;
	u32 reserved[4];
} __packed;

/* GuC MMIO reg state struct */
struct guc_mmio_reg {
	u32 offset;
	u32 value;
	u32 flags;
	u32 mask;
#define GUC_REGSET_MASKED		BIT(0)
#define GUC_REGSET_MASKED_WITH_VALUE	BIT(2)
#define GUC_REGSET_RESTORE_ONLY		BIT(3)
} __packed;

/* GuC register sets */
struct guc_mmio_reg_set {
	u32 address;
	u16 count;
	u16 reserved;
} __packed;

/* Generic GT SysInfo data types */
#define GUC_GENERIC_GT_SYSINFO_SLICE_ENABLED		0
#define GUC_GENERIC_GT_SYSINFO_VDBOX_SFC_SUPPORT_MASK	1
#define GUC_GENERIC_GT_SYSINFO_DOORBELL_COUNT_PER_SQIDI	2
#define GUC_GENERIC_GT_SYSINFO_MAX			16

/* HW info */
struct guc_gt_system_info {
	u8 mapping_table[GUC_MAX_ENGINE_CLASSES][GUC_MAX_INSTANCES_PER_CLASS];
	u32 engine_enabled_masks[GUC_MAX_ENGINE_CLASSES];
	u32 generic_gt_sysinfo[GUC_GENERIC_GT_SYSINFO_MAX];
} __packed;

enum {
	GUC_CAPTURE_LIST_INDEX_PF = 0,
	GUC_CAPTURE_LIST_INDEX_VF = 1,
	GUC_CAPTURE_LIST_INDEX_MAX = 2,
};

/* GuC Additional Data Struct */
struct guc_ads {
	struct guc_mmio_reg_set reg_state_list[GUC_MAX_ENGINE_CLASSES][GUC_MAX_INSTANCES_PER_CLASS];
	u32 reserved0;
	u32 scheduler_policies;
	u32 gt_system_info;
	u32 reserved1;
	u32 control_data;
	u32 golden_context_lrca[GUC_MAX_ENGINE_CLASSES];
	u32 eng_state_size[GUC_MAX_ENGINE_CLASSES];
	u32 private_data;
	u32 reserved2;
	u32 capture_instance[GUC_CAPTURE_LIST_INDEX_MAX][GUC_MAX_ENGINE_CLASSES];
	u32 capture_class[GUC_CAPTURE_LIST_INDEX_MAX][GUC_MAX_ENGINE_CLASSES];
	u32 capture_global[GUC_CAPTURE_LIST_INDEX_MAX];
	u32 reserved[14];
} __packed;

/* Engine usage stats */
struct guc_engine_usage_record {
	u32 current_context_index;
	u32 last_switch_in_stamp;
	u32 reserved0;
	u32 total_runtime;
	u32 reserved1[4];
} __packed;

struct guc_engine_usage {
	struct guc_engine_usage_record engines[GUC_MAX_ENGINE_CLASSES][GUC_MAX_INSTANCES_PER_CLASS];
} __packed;

#endif
