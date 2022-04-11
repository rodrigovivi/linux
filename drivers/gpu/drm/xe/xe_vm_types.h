/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_VM_TYPES_H_
#define _XE_VM_TYPES_H_

#include <linux/dma-resv.h>
#include <linux/kref.h>
#include <linux/mmu_notifier.h>

struct xe_bo;
struct xe_vm;

struct xe_vma {
	struct rb_node vm_node;
	/** @vm: VM which this VMA belongs to */
	struct xe_vm *vm;

	/**
	 * @start: start address of this VMA within its address domain, end -
	 * start + 1 == VMA size
	 */
	uint64_t start;
	/** @end: end address of this VMA within its address domain */
	uint64_t end;
	/** @pte_flags: pte flags for this VMA */
	uint32_t pte_flags;

	/** @bo: BO if not a userptr, must be NULL is userptr */
	struct xe_bo *bo;
	/** @bo_offset: offset into BO if not a userptr, unused for userptr */
	uint64_t bo_offset;

	union {
		/** @bo_link: link into BO if not a userptr */
		struct list_head bo_link;
		/** @userptr_link: link into VM if userptr */
		struct list_head userptr_link;
	};

	/** @userptr: user pointer state */
	struct {
		/** @ptr: user pointer */
		uintptr_t ptr;
		/**
		 * @notifier: MMU notifier for user pointer (invalidation call back)
		 */
		struct mmu_interval_notifier notifier;
		/**
		 * @dma_address: DMA address for each of page of this user pointer
		 */
		dma_addr_t *dma_address;
		/** @rebind_work: worker to rebind this VMA / BO */
		struct work_struct rebind_work;
		/** @destroy_work: worker to destroy this BO */
		struct work_struct destroy_work;
		/** @notifier_seq: notifier sequence number */
		unsigned long notifier_seq;
		/** @dirty: user pointer dirty (needs new VM bind) */
		bool dirty;
		/** @destroyed: user pointer is destroyed */
		bool destroyed;
		/** @initial_bind: user pointer has been bound at least once */
		bool initial_bind;
	} userptr;

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

	/** @target_vma: Target vma to write */
	struct xe_vma *target_vma;

	/** @target_offset: Target object offset */
	u64 target_offset;

	/** @pt_entries: Newly added pagetable entries */
	struct xe_pt **pt_entries;
};

#define XE_VM_FLAGS_64K			BIT(0)

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
	/** @flags: Flags */
	uint32_t flags;

	/** @userptr: user pointer state */
	struct {
		/** @list: list of VMAs which are user pointers */
		struct list_head list;
		/** @list_lock: protects list of user pointers */
		struct mutex list_lock;
		/**
		 * @notifier_lock: protects notifier + pending_rebind
		 */
		rwlock_t notifier_lock;
		/**
		 * @pending_rebind: number of pending userptr rebinds, used when
		 * preempt fences are installed on this VM
		 */
		u32 pending_rebind;
		/** @fence: userptr fence for a rebind from execbuf */
		struct dma_fence *fence;
	} userptr;

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
