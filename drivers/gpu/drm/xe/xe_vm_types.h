/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_VM_TYPES_H_
#define _XE_VM_TYPES_H_

#include <linux/dma-resv.h>
#include <linux/kref.h>
#include <linux/mmu_notifier.h>

struct xe_bo;
struct xe_vm;

enum xe_cache_level {
	XE_CACHE_NONE,
	XE_CACHE_WT,
	XE_CACHE_WB,
};

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
};

struct xe_device;

struct xe_pt {
	struct xe_bo *bo;
	unsigned int level;
	unsigned int num_live;
	bool rebind;
};

#define xe_vm_assert_held(vm) dma_resv_assert_held(&(vm)->resv)
#define XE_VM_MAX_LEVEL 4

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

	/** @flags: Target flags */
	u32 flags;
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

	/** @flags: flags for this VM, statically setup a creation time */
#define XE_VM_FLAGS_64K			BIT(0)
#define XE_VM_FLAG_COMPUTE_MODE		BIT(1)
#define XE_VM_FLAG_ASYNC_BIND_OPS	BIT(2)
#define XE_VM_FLAG_MIGRATION		BIT(3)
#define XE_VM_FLAG_SCRATCH_PAGE		BIT(4)
	unsigned long flags;

	/**
	 * @lock: outer most lock, protects objects of anything attached to this
	 * VM
	 */
	struct rw_semaphore lock;

	/** @evict_list: list of VMAs that have been evicted */
	struct list_head evict_list;

	/** @rebind_fence: rebind fence from execbuf */
	struct dma_fence *rebind_fence;

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
		/** @pause: pause all pending async VM ops */
		bool pause;
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
};

#endif	/* _XE_VM_TYPES_H_ */
