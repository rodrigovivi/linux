/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GUC_H_
#define _XE_GUC_H_

#include "xe_guc_types.h"

struct drm_printer;

int xe_guc_init(struct xe_guc *guc);
int xe_guc_reset(struct xe_guc *guc);
int xe_guc_upload(struct xe_guc *guc);
int xe_guc_enable_communication(struct xe_guc *guc);
void xe_guc_notify(struct xe_guc *guc);
void xe_guc_wb(struct xe_guc *guc);
int xe_guc_auth_huc(struct xe_guc *guc, u32 rsa_addr);
int xe_guc_send_mmio(struct xe_guc *guc, const u32 *request, u32 len);
int xe_guc_self_cfg32(struct xe_guc *guc, u16 key, u32 val);
int xe_guc_self_cfg64(struct xe_guc *guc, u16 key, u64 val);
void xe_guc_irq_handler(struct xe_guc *guc, const u16 iir);
void xe_guc_print_info(struct xe_guc *guc, struct drm_printer *p);

static inline void
xe_guc_sanitize(struct xe_guc *guc)
{
	// TODO - Reset GuC SW state
}

#endif	/* _XE_GUC_H_ */
