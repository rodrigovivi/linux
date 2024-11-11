/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023-2025 Intel Corporation
 */

#ifndef _XE_EUDEBUG_TYPES_H_
#define _XE_EUDEBUG_TYPES_H_

#include <linux/completion.h>
#include <linux/kfifo.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/rbtree.h>
#include <linux/rhashtable.h>
#include <linux/sizes.h>
#include <linux/wait.h>
#include <linux/xarray.h>

#include <uapi/drm/xe_drm_eudebug.h>
#include "xe_gt_debug_types.h"

struct xe_device;
struct xe_eudebug;
struct xe_hw_engine;
struct workqueue_struct;
struct xe_exec_queue;
struct xe_lrc;

/**
 * enum xe_eudebug_cap_state - eudebug capability state
 *
 * @XE_EUDEBUG_CAP_NOT_SUPPORTED: eudebug feature support off
 * @XE_EUDEBUG_CAP_DISABLED: eudebug feature supported but disabled
 * @XE_EUDEBUG_CAP_ENABLED: eudebug enabled
 */
enum xe_eudebug_cap_state {
	XE_EUDEBUG_CAP_NOT_SUPPORTED = 0,
	XE_EUDEBUG_CAP_DISABLED,
	XE_EUDEBUG_CAP_ENABLED,
};

#define XE_EUDEBUG_MAX_EVENT_TYPE DRM_XE_EUDEBUG_EVENT_PAGEFAULT

/**
 * struct xe_eudebug_handle - eudebug resource handle
 */
struct xe_eudebug_handle {
	/** @key: key value in rhashtable <key:id> */
	u64 key;

	/** @id: opaque handle id for xarray <id:key> */
	int id;

	/** @rh_head: rhashtable head */
	struct rhash_head rh_head;
};

/**
 * struct xe_eudebug_resource - Resource map for one resource
 */
struct xe_eudebug_resource {
	/** @lock: protects xa and rh consistency */
	struct mutex lock;

	/** @xa: xarrays for <id->key> */
	struct xarray xa;

	/** @rh: rhashtable for <key->id> */
	struct rhashtable rh;
};

#define XE_EUDEBUG_RES_TYPE_VM		0
#define XE_EUDEBUG_RES_TYPE_EXEC_QUEUE  1
#define XE_EUDEBUG_RES_TYPE_LRC		2
#define XE_EUDEBUG_RES_TYPE_COUNT	(XE_EUDEBUG_RES_TYPE_LRC + 1)

/**
 * struct xe_eudebug_eu_control_ops - interface for eu thread
 * state control backend
 */
struct xe_eudebug_eu_control_ops {
	/** @interrupt_all: interrupts workload active on given hwe */
	int (*interrupt_all)(struct xe_eudebug *e, struct xe_exec_queue *q,
			     struct xe_hw_engine *hwe, struct xe_lrc *lrc);

	/** @resume: resumes threads reflected by bitmask active on given hwe */
	int (*resume)(struct xe_eudebug *e, struct xe_exec_queue *q,
		      struct xe_lrc *lrc, u8 *bitmap, unsigned int bitmap_size);

	/** @stopped: returns bitmap reflecting threads which signal attention */
	int (*stopped)(struct xe_eudebug *e, struct xe_exec_queue *q,
		       struct xe_lrc *lrc, u8 *bitmap, unsigned int bitmap_size);
};

/**
 * struct xe_eudebug - Top level struct for eudebug: the connection
 */
struct xe_eudebug {
	/** @ref: kref counter for this struct */
	struct kref ref;

	/** @target: debug target specifics */
	struct {
		/** @target.xef: the target xe_file that we are debugging
		 *  Protected by xe->eudebug.lock.
		 */
		struct xe_file *xef;

		/** @target.pid: pid of target */
		pid_t pid;

		/** @target.err: error code on disconnect */
		int err;

		/** @target.res: resource maps for all types */
		struct xe_eudebug_resource res[XE_EUDEBUG_RES_TYPE_COUNT];
	} target;

	/** @xe: the parent device we are serving */
	struct xe_device *xe;

	/** @session: session number for this connection (for logs) */
	u64 session;

	/** @flags: state flags */
	unsigned long flags;
#define XE_EUDEBUG_READER_ACTIVE 0

	/** @discovery: completion to wait for discovery */
	struct completion discovery;

	/** @discovery_work: worker to discover resources for target_task */
	struct work_struct discovery_work;

	/** @events: kfifo queue of to-be-delivered events */
	struct {
		/** @events.lock: guards access to fifo, pending and staging */
		spinlock_t lock;

#define DRM_XE_EUDEBUG_EVENT_MAX_SIZE SZ_64K
		/** @events.pending: pending event bounce buffer, preallocated */
		struct drm_xe_eudebug_event *pending;

		/**
		 * @events.pending_occupied: @events.pending holds an event
		 * that was popped from the fifo but not yet delivered
		 */
		bool pending_occupied;

		/** @events.staging: write side staging buffer, preallocated */
		struct drm_xe_eudebug_event *staging;

		/** @events.fifo: queue of events pending */
		struct kfifo fifo;

		/** @events.fifo_buf: memory for the fifo */
		void *fifo_buf;
#define XE_EUDEBUG_FIFO_SIZE SZ_16M

		/** @events.write_done: waitqueue for signalling write to fifo */
		wait_queue_head_t write_done;

		/**
		 * @events.seqno: seqno counter to stamp events for fifo
		 *
		 * Stamped when an event is generated, which is not the same
		 * moment it reaches the fifo, so concurrent producers can
		 * take N and N+1 and then queue them the other way round.
		 *
		 * That is deliberate. The number is the client timeline the
		 * debugger reconstructs, not a delivery order. Making the two
		 * agree would mean holding @events.lock across event
		 * generation, serialising the producers and losing the very
		 * ordering the debugger is after.
		 *
		 * See drm_xe_eudebug_event.seqno for what this promises
		 * userspace.
		 */
		atomic64_t seqno;
	} events;

	/** @acks: user fence acks tracked by this debugger */
	struct {
		/** @acks.lock: guards access to tree */
		spinlock_t lock;

		/** @acks.tree: pending acks by seqnos */
		struct rb_root tree;
	} acks;

	/** @ops: operations for eu_control */
	const struct xe_eudebug_eu_control_ops *ops;

	/**
	 * @pf: Aggregates pagefault related fields.
	 */
	struct {
		/** @pf.lock: guards access to pagefaults list*/
		struct mutex lock;

		/** @pf.pagefaults: xe_eudebug_pagefault list for pagefault event queuing */
		struct list_head pagefaults;

		/**
		 * @pf.fence: fence on operations of eus (eu thread control and attention)
		 * when page faults are being handled, protected by @xe.eudebug.lock.
		 */
		struct dma_fence *fence;

		/**
		 * @pf.active: number of pagefaults that are being processed in
		 * parallel by workers and that are after xe_eudebug_pagefault_start()
		 * but before xe_eudebug_pagefault_end() (ack_fault_end() handler).
		 *
		 * Value 0 means that no other worker is currently handling eudebug
		 * related pagefault but there still may be a pagefault queued and waiting.
		 *
		 * Protected by @xe.eudebug.lock.
		 */
		u32 active;

		/**
		 * @pf.first: first pagefault. Only this pagefault will be reported
		 * with event.
		 * Subsequent pagefaults with matching VM will be ACKed without
		 * being handled by eudebug.
		 * Valid until there are no more pagefaults with matching asid.
		 *
		 * This field may be set while @pf.active is 0 if eudebug related
		 * pagefault is queued but not yet processed.
		 * Protected by @xe.eudebug.lock.
		 */
		struct xe_eudebug_pagefault *first;

		/**
		 * @pf.asid: VM id of the first pagefault (@pf.first).
		 */
		u32 asid;
	} pf;
};

/**
 * struct xe_eudebug_pagefault - eudebug structure for queuing pagefault
 */
struct xe_eudebug_pagefault {
	/** @link: link into the xe_eudebug.pagefaults */
	struct list_head link;
	/** @d: eudebug instance. */
	struct xe_eudebug *d;
	/** @q: exec_queue which raised pagefault */
	struct xe_exec_queue *q;
	/** @lrc_idx: lrc index of the workload which raised pagefault */
	int lrc_idx;

	/** @fault: pagefault raw partial data passed from guc */
	struct {
		/** @fault.addr: ppgtt address where the pagefault occurred */
		u64 addr;
		/** @fault.type_level: mixed type & level */
		u8 type_level;
		/** @fault.access_type: access type */
		u8 access_type;
	} fault;

	/** @attentions: attention states in different phases of fault */
	struct {
		/** @attentions.before: state of attention bits before page fault WA processing*/
		struct xe_eu_attentions before;
		/**
		 * @attentions.after: status of attention bits during page fault WA processing.
		 * It includes eu threads where attention bits are turned on for
		 * reasons other than page fault WA (breakpoint, interrupt, etc.).
		 */
		struct xe_eu_attentions after;
		/**
		 * @attentions.resolved: state of the attention bits after page fault WA.
		 * It includes the eu thread that caused the page fault.
		 * To determine the eu thread that caused the page fault,
		 * do XOR attentions.after and attentions.resolved.
		 */
		struct xe_eu_attentions resolved;
	} attentions;

	/**
	 * @private: copied the (struct xe_pagefault *)->producer.private filed.
	 * EU Debugger masks private field in the struct xe_pagefault.
	 * The xe_eudebug_pagefault_get_private() function to extracts original
	 * private field regardless if it was shadowed or not.
	 */
	void *private;

	/**
	 * @null_vma_addr: address to look for temporary vma for handling pagefault.
	 * Shall be removed when pagefault is handled.
	 */
	u64 null_vma_addr;
};

#endif /* _XE_EUDEBUG_TYPES_H_ */
