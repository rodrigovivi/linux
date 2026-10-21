/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef _XE_DEBUG_DATA_H_
#define _XE_DEBUG_DATA_H_

#include <linux/types.h>

struct drm_gpuva_ops;
struct xe_vm;
struct xe_vma_op;

const char *xe_debug_data_pseudo_path_to_string(u64 pseudopath);
int xe_debug_data_check_extension(struct xe_vm *vm, u32 operation, u64 extension);
int xe_debug_data_process_extension(struct xe_vm *vm, struct drm_gpuva_ops *ops, u32 operation,
				    u64 extension);
int xe_debug_data_op_unwind(struct xe_vm *vm, struct xe_vma_op *vma_op);
void xe_debug_data_destroy(struct xe_vm *vm);

#endif /* _XE_DEBUG_DATA_H_ */
