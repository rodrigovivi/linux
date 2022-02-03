/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_VM_TYPES_H_
#define _XE_VM_TYPES_H_

#include <linux/dma-resv.h>
#include <linux/kref.h>

struct xe_bo;
struct xe_vm;

struct xe_vma {
	struct rb_node vm_node;
	struct xe_vm *vm;

	uint64_t start;
	uint64_t end;

	struct xe_bo *bo;
	uint64_t bo_offset;
	struct list_head bo_link;
};

struct xe_device;
struct xe_pt;

#define xe_vm_assert_held(vm) dma_resv_assert_held(&(vm)->resv)

struct xe_vm {
	struct xe_device *xe;

	struct kref refcount;

	struct dma_resv resv;

	uint64_t size;
	struct rb_root vmas;

	struct xe_pt *pt_root;

	struct xe_bo *scratch_bo;
	struct xe_pt *scratch_pt[3];
};

#endif	/* _XE_VM_TYPES_H_ */
