/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_VM_TYPES_H_
#define _XE_VM_TYPES_H_

#include <linux/dma-resv.h>
#include <linux/kref.h>
#include <linux/mmu_notifier.h>

#include "xe_device_types.h"
#include "xe_pt_types.h"

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
	u64 start;
	/** @end: end address of this VMA within its address domain */
	u64 end;
	/** @pte_flags: pte flags for this VMA */
	u32 pte_flags;

	/** @bo: BO if not a userptr, must be NULL is userptr */
	struct xe_bo *bo;
	/** @bo_offset: offset into BO if not a userptr, unused for userptr */
	u64 bo_offset;

	/** @gt_mask: GT mask of where to create binding for this VMA */
	u64 gt_mask;

	/** @gt_present: GT mask of binding are present for this VMA */
	u64 gt_present;

	/** @destroyed: VMA is destroyed */
	bool destroyed;

	/**
	 * @first_munmap_rebind: VMA is first in a sequence of ops that triggers
	 * a rebind (munmap style VM unbinds). This indicates the operation
	 * using this VMA must wait on all dma-resv slots (wait for pending jobs
	 * / trigger preempt fences).
	 */
	bool first_munmap_rebind;

	/**
	 * @last_munmap_rebind: VMA is first in a sequence of ops that triggers
	 * a rebind (munmap style VM unbinds). This indicates the operation
	 * using this VMA must install itself into kernel dma-resv slot (blocks
	 * future jobs) and kick the rebind work in compute mode.
	 */
	bool last_munmap_rebind;

	/** @use_atomic_access_pte_bit: Set atomic access bit in PTE */
	bool use_atomic_access_pte_bit;

	union {
		/** @bo_link: link into BO if not a userptr */
		struct list_head bo_link;
		/** @userptr_link: link into VM if userptr */
		struct list_head userptr_link;
	};

	/** @evict_link: link into VM if this VMA has been evicted */
	struct list_head evict_link;

	/**
	 * @unbind_link: link or list head if an unbind of multiple VMAs, in
	 * single unbind op, is being done.
	 */
	struct list_head unbind_link;

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
		/** @destroy_work: worker to destroy this BO */
		struct work_struct destroy_work;
		/** @notifier_seq: notifier sequence number */
		unsigned long notifier_seq;
		/** @dirty: user pointer dirty (needs new VM bind) */
		bool dirty;
		/** @initial_bind: user pointer has been bound at least once */
		bool initial_bind;
	} userptr;

	/** @usm: unified shared memory state */
	struct xe_vma_usm {
		/** @gt_invalidated: VMA has been invalidated */
		u64 gt_invalidated;
		/** @gt: state for each GT this VMA is mapped in */
		struct {
			/** @num_leaves: the number of leaf pages */
			int num_leaves;
			/**
			 * @leaves: leaves info, used for invalidating VMAs
			 * without a lock in eviction / userptr invalidation
			 * code. Needed as we can't take the required locks to
			 * access / change the stored page table structure in
			 * these paths.
			 */
			struct {
				/**
				 * @bo: buffer object for leaf of page table
				 * structure
				 */
				struct xe_bo *bo;
				/** @start_ofs: start offset in leaf BO */
				u32 start_ofs;
				/** @len: length of memory to zero in leaf BO */
				u32 len;
#define MAX_LEAVES	(XE_VM_MAX_LEVEL * 2 + 1)
			} leaves[MAX_LEAVES];
		} gt[XE_MAX_GT];
	} usm;
};

struct xe_device;

#define xe_vm_assert_held(vm) dma_resv_assert_held(&(vm)->resv)

struct xe_vm {
	struct xe_device *xe;

	struct kref refcount;

	/* engine used for (un)binding vma's */
	struct xe_engine *eng[XE_MAX_GT];

	struct dma_resv resv;

	u64 size;
	struct rb_root vmas;

	struct xe_pt *pt_root[XE_MAX_GT];
	struct xe_bo *scratch_bo[XE_MAX_GT];
	struct xe_pt *scratch_pt[XE_MAX_GT][XE_VM_MAX_LEVEL];

	/** @flags: flags for this VM, statically setup a creation time */
#define XE_VM_FLAGS_64K			BIT(0)
#define XE_VM_FLAG_COMPUTE_MODE		BIT(1)
#define XE_VM_FLAG_ASYNC_BIND_OPS	BIT(2)
#define XE_VM_FLAG_MIGRATION		BIT(3)
#define XE_VM_FLAG_SCRATCH_PAGE		BIT(4)
#define XE_VM_FLAG_FAULT_MODE		BIT(5)
#define XE_VM_FLAG_GT_ID(flags)		(((flags) >> 6) & 0x3)
#define XE_VM_FLAG_SET_GT_ID(gt)	((gt)->info.id << 6)
	unsigned long flags;

	/** @composite_fence_ctx: context composite fence */
	u64 composite_fence_ctx;
	/** @composite_fence_seqno: seqno for composite fence */
	u32 composite_fence_seqno;

	/**
	 * @lock: outer most lock, protects objects of anything attached to this
	 * VM
	 */
	struct rw_semaphore lock;

	/** @evict_list: list of VMAs that have been evicted */
	struct list_head evict_list;

	/** @rebind_fence: rebind fence from execbuf */
	struct dma_fence *rebind_fence;

	/**
	 * @destroy_work: worker to destroy VM, needed as a dma_fence signaling
	 * from an irq context can be last put and the destroy needs to be able
	 * to sleep.
	 */
	struct work_struct destroy_work;

	/** @extobj: bookkeeping for external objects */
	struct {
		/** @enties: number of external BOs attached this VM */
		u32 entries;
		/** @bos: external BOs attached to this VM */
		struct xe_bo **bos;
	} extobj;

	/** @async_ops: async VM operations (bind / unbinds) */
	struct {
		/** @list: list of pending async VM ops */
		struct list_head pending;
		/** @work: worker to execute async VM ops */
		struct work_struct work;
		/** @lock: protects list of pending async VM ops and fences */
		spinlock_t lock;
		/** @error_capture: error capture state */
		struct {
			/** @mm: user MM */
			struct mm_struct *mm;
			/**
			 * @addr: user pointer to copy error capture state too
			 */
			u64 addr;
			/** @wq: user fence wait queue for VM errors */
			wait_queue_head_t wq;
		} error_capture;
		/** @fence: fence state */
		struct {
			/** @context: context of async fence */
			u64 context;
			/** @seqno: seqno of async fence */
			u32 seqno;
		} fence;
		/** @error: error state for async VM ops */
		int error;
		/**
		 * @munmap_rebind_inflight: an munmap style VM bind is in the
		 * middle of a set of ops which requires a rebind at the end.
		 */
		bool munmap_rebind_inflight;
	} async_ops;

	/** @userptr: user pointer state */
	struct {
		/** @list: list of VMAs which are user pointers */
		struct list_head list;
		/**
		 * @notifier_lock: protects notifier
		 */
		rwlock_t notifier_lock;
	} userptr;

	/** @preempt: preempt state */
	struct {
		/**
		 * @min_run_period_ms: The minimum run period before preempting
		 * an engine again
		 */
		s64 min_run_period_ms;
		/** @engines: list of engines attached to this VM */
		struct list_head engines;
		/** @num_engines: number user engines attached to this VM */
		int num_engines;
		/**
		 * @rebind_work: worker to rebind invalidated userptrs / evicted
		 * BOs
		 */
		struct work_struct rebind_work;
		/**
		 * @resume_wq: resume wait queue which delays the resume until
		 * new preempt fences are installed
		 */
		wait_queue_head_t resume_wq;
		/**
		 * @resume_go: tells resume waiter if they are safe to resume
		 */
		int resume_go;
	} preempt;

	/** @um: unified memory state */
	struct {
		/** @asid: address space ID, unique to each VM */
		u32 asid;
		/**
		 * @last_fault_vma: Last fault VMA, used for fast lookup when we
		 * get a flood of faults to the same VMA
		 */
		struct xe_vma *last_fault_vma;
	} usm;

	/** @error_capture_flag: allow to track errors once */
#define XE_ERROR_CAPTURE_FLAG_DUMP_VMA		BIT(0)
	u32  error_capture_flag;
};

#endif
