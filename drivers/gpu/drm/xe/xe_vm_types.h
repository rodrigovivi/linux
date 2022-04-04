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

	bool evicted;
};

struct xe_device;
struct xe_pt;

#define xe_vm_assert_held(vm) dma_resv_assert_held(&(vm)->resv)
#define XE_VM_MAX_LEVEL 3

struct xe_vm_pgtable_update {
	/** @bo: page table bo to write to */
	struct xe_bo *pt_bo;

	/** @ofs: offset inside this PTE to begin writing to (in qwords) */
	u32 ofs;

	/** @qwords: number of PTE's to write */
	u32 qwords;

	/** @pt: opaque pointer useful for the caller of xe_migrate_update_pgtables */
	struct xe_pt *pt;

	/** @target: Target bo to write */
	struct xe_bo *target;

	/** @target_offset: Target object offset */
	u64 target_offset;

	/** @pt_entries: Newly added pagetable entries */
	struct xe_pt **pt_entries;
};

struct xe_vm {
	struct xe_device *xe;

	struct kref refcount;

	/* engine used for (un)binding vma's */
	struct xe_engine *eng;

	struct dma_resv resv;

	uint64_t size;
	struct rb_root vmas;

	struct xe_pt *pt_root;

	struct xe_bo *scratch_bo;
	struct xe_pt *scratch_pt[XE_VM_MAX_LEVEL];

	/** @preempt: preempt state */
	struct {
		/**
		 * @enabled: Preempt fences are enabled on this VM (used by
		 * compute engine)
		 */
		bool enabled;
		/**
		 * @num_inflight_ops: number pendings ops (e.g. inflight
		 * un/binds) before the pending preempt fences can call resume
		 * on their respective engines and be inserted back into
		 * the shared slots
		 */
		u32 num_inflight_ops;
		/**
		 * @list: list of pending preempt fences, when the number of
		 * number of inflight ops reaches zero, each fence's engine is
		 * resumed and inserted into a shared slot
		 */
		struct list_head pending_fences;
	} preempt;
};

#endif	/* _XE_VM_TYPES_H_ */
