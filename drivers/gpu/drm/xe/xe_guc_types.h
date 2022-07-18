/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GUC_TYPES_H_
#define _XE_GUC_TYPES_H_

#include <linux/idr.h>
#include <linux/xarray.h>

#include "xe_guc_ads_types.h"
#include "xe_guc_ct_types.h"
#include "xe_guc_fwif.h"
#include "xe_guc_log_types.h"
#include "xe_uc_fw_types.h"

/**
 * struct xe_guc - Graphic micro controller
 */
struct xe_guc {
	/** @fw: Generic uC firmware management */
	struct xe_uc_fw fw;
	/** @log: GuC log */
	struct xe_guc_log log;
	/** @ads: GuC ads */
	struct xe_guc_ads ads;
	/** @log: GuC ct */
	struct xe_guc_ct ct;
	/** @submission_state: GuC submission state */
	struct {
		/** @engine_lookup: Lookup an xe_engine from guc_id */
		struct xarray engine_lookup;
		/** @guc_ids: used to allocate new guc_ids, single-lrc */
		struct ida guc_ids;
		/** @guc_ids_bitmap: used to allocate new guc_ids, multi-lrc */
		unsigned long *guc_ids_bitmap;
		/** @stopped: submissions are stopped */
		atomic_t stopped;
		/** @lock: protects submission state */
		struct mutex lock;
		/** @suspend: suspend fence state */
		struct {
			/** @lock: suspend fences lock */
			spinlock_t lock;
			/** @context: suspend fences context */
			u64 context;
			/** @seqno: suspend fences seqno */
			u32 seqno;
		} suspend;
	} submission_state;

	/** @params: Control params for fw initialization */
	u32 params[GUC_CTL_MAX_DWORDS];
};

#endif	/* _XE_GUC_TYPES_H_ */
