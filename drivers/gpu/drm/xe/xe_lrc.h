/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */
#ifndef _XE_LRC_H_
#define _XE_LRC_H_

#include "xe_lrc_types.h"

struct xe_vm;

int xe_lrc_init(struct xe_lrc *lrc, struct xe_hw_engine *hwe,
		struct xe_vm *vm, uint32_t ring_size);
void xe_lrc_finish(struct xe_lrc *lrc);

uint32_t xe_lrc_ring_head(struct xe_lrc *lrc);
uint32_t xe_lrc_ring_space(struct xe_lrc *lrc);
void xe_lrc_write_ring(struct xe_lrc *lrc, const void *data, size_t size);

uint32_t xe_lrc_ggtt_addr(struct xe_lrc *lrc);
uint32_t *xe_lrc_regs(struct xe_lrc *lrc);

uint32_t xe_lrc_read_ctx_reg(struct xe_lrc *lrc, int reg_nr);
void xe_lrc_write_ctx_reg(struct xe_lrc *lrc, int reg_nr, uint32_t val);

uint64_t xe_lrc_descriptor(struct xe_lrc *lrc);

uint32_t xe_lrc_seqno_ggtt_addr(struct xe_lrc *lrc);
struct dma_fence *xe_lrc_create_seqno_fence(struct xe_lrc *lrc);

#endif /* _XE_LRC_H_ */
