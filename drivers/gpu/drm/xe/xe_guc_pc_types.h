/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GUC_PC_TYPES_H_
#define _XE_GUC_PC_TYPES_H_

/**
 * struct xe_guc_pc - GuC Power Conservation (PC)
 */
struct xe_guc_pc {
	/** @bo: GGTT buffer object that is shared with GuC PC */
	struct xe_bo *bo;
};

#endif	/* _XE_GUC_PC_TYPES_H_ */
