/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023-2025 Intel Corporation
 */

#ifndef _PRELIM_XE_EUDEBUG_TYPES_H_
#define _PRELIM_XE_EUDEBUG_TYPES_H_

#include <uapi/drm/xe_drm_prelim.h>

#include <linux/completion.h>
#include <linux/kfifo.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/rbtree.h>
#include <linux/rhashtable.h>
#include <linux/wait.h>
#include <linux/xarray.h>

#include <uapi/drm/xe_drm.h>

#include "prelim/xe_gt_debug.h"

struct xe_device;
struct task_struct;
struct prelim_xe_eudebug;
struct prelim_xe_eudebug_event;
struct xe_hw_engine;
struct workqueue_struct;
struct xe_exec_queue;
struct xe_lrc;
struct xe_vm;
struct xe_gt;

#define PRELIM_XE_EUDEBUG_EVENT_QUEUE_SIZE 64

/**
 * struct prelim_xe_eudebug_handle - eudebug resource handle
 */
struct prelim_xe_eudebug_handle {
	/** @key: key value in rhashtable <key:id> */
	u64 key;

	/** @id: opaque handle id for xarray <id:key> */
	int id;

	/** @rh_head: rhashtable head */
	struct rhash_head rh_head;
};

/**
 * struct prelim_xe_eudebug_resource - Resource map for one resource
 */
struct prelim_xe_eudebug_resource {
	/** @xa: xarrays for <id->key> */
	struct xarray xa;

	/** @rh: rhashtable for <key->id> */
	struct rhashtable rh;
};

#define PRELIM_XE_EUDEBUG_RES_TYPE_CLIENT	0
#define PRELIM_XE_EUDEBUG_RES_TYPE_VM		1
#define PRELIM_XE_EUDEBUG_RES_TYPE_EXEC_QUEUE	2
#define PRELIM_XE_EUDEBUG_RES_TYPE_LRC		3
#define PRELIM_XE_EUDEBUG_RES_TYPE_METADATA	4
#define PRELIM_XE_EUDEBUG_RES_TYPE_COUNT	(PRELIM_XE_EUDEBUG_RES_TYPE_METADATA + 1)

/**
 * struct prelim_xe_eudebug_resources - eudebug resources for all types
 */
struct prelim_xe_eudebug_resources {
	/** @lock: guards access into rt */
	struct mutex lock;

	/** @rt: resource maps for all types */
	struct prelim_xe_eudebug_resource rt[PRELIM_XE_EUDEBUG_RES_TYPE_COUNT];
};

/**
 * struct prelim_xe_eudebug_eu_control_ops - interface for eu thread
 * state control backend
 */
struct prelim_xe_eudebug_eu_control_ops {
	/** @interrupt_all: interrupts workload active on given hwe */
	int (*interrupt_all)(struct prelim_xe_eudebug *e, struct xe_exec_queue *q,
			     struct xe_lrc *lrc);

	/** @resume: resumes threads reflected by bitmask active on given hwe */
	int (*resume)(struct prelim_xe_eudebug *e, struct xe_exec_queue *q,
		      struct xe_lrc *lrc, u8 *bitmap, unsigned int bitmap_size);

	/** @stopped: returns bitmap reflecting threads which signal attention */
	int (*stopped)(struct prelim_xe_eudebug *e, struct xe_exec_queue *q,
		       struct xe_lrc *lrc, u8 *bitmap, unsigned int bitmap_size);

	/** @unlock: unlocks threads reflected by bitmask active on given hwe */
	int (*unlock)(struct prelim_xe_eudebug *e, struct xe_exec_queue *q,
		      struct xe_lrc *lrc, u8 *bitmap, unsigned int bitmap_size);

};

/**
 * struct prelim_xe_eudebug - Top level struct for eudebug: the connection
 */
struct prelim_xe_eudebug {
	/** @ref: kref counter for this struct */
	struct kref ref;

	/** @rcu: rcu_head for rcu destruction */
	struct rcu_head rcu;

	/** @connection_link: our link into the xe_device:prelim_eudebug.list */
	struct list_head connection_link;

	/** @connection: eu debug connection status */
	struct {
		/** @connection.status: connected = 1, disconnected = error */
#define PRELIM_XE_EUDEBUG_STATUS_CONNECTED 1
		int status;

		/** @connection.lock: guards access to status */
		spinlock_t lock;
	} connection;

	/** @xe: the parent device we are serving */
	struct xe_device *xe;

	/** @target_task: the task that we are debugging */
	struct task_struct *target_task;

	/** @res: the resource maps we track for target_task */
	struct prelim_xe_eudebug_resources *res;

	/** @session: session number for this connection (for logs) */
	u64 session;

	/** @discovery: completion to wait for discovery */
	struct completion discovery;

	/** @discovery_work: worker to discover resources for target_task */
	struct work_struct discovery_work;

	/** @eu_lock: guards operations on eus (eu thread control and attention) */
	struct mutex eu_lock;

	/** @events: kfifo queue of to-be-delivered events */
	struct {
		/** @events.lock: guards access to fifo */
		spinlock_t lock;

		/** @events.fifo: queue of events pending */
		DECLARE_KFIFO(fifo,
			      struct prelim_xe_eudebug_event *,
			      PRELIM_XE_EUDEBUG_EVENT_QUEUE_SIZE);

		/** @events.write_done: waitqueue for signalling write to fifo */
		wait_queue_head_t write_done;

		/** @events.read_done: waitqueue for signalling read from fifo */
		wait_queue_head_t read_done;

		/** @events.seqno: seqno counter to stamp events for fifo */
		atomic_long_t seqno;
	} events;

	/** @acks: user fences tracked by this debugger */
	struct {
		/** @acks.lock: guards access to tree */
		spinlock_t lock;

		struct rb_root tree;
	} acks;

	/** @ops: operations for eu_control */
	struct prelim_xe_eudebug_eu_control_ops *ops;

	/** @pf_lock: guards access to pagefaults list*/
	struct mutex pf_lock;
	/** @pagefaults: xe_eudebug_pagefault list for pagefault event queuing */
	struct list_head pagefaults;
	/**
	 * @pf_fence: fence on operations of eus (eu thread control and attention)
	 * when page faults are being handled, protected by @eu_lock.
	 */
	struct dma_fence *pf_fence;
};

/**
 * enum prelim_xe_eudebug_state - eudebug capability state
 *
 * @PRELIM_XE_EUDEBUG_NOT_SUPPORTED: eudebug feature support off
 * @PRELIM_XE_EUDEBUG_SUPPORTED: eudebug feature supported but disabled
 * @PRELIM_XE_EUDEBUG_ENABLED: eudebug enabled
 */
enum prelim_xe_eudebug_state {
	PRELIM_XE_EUDEBUG_NOT_SUPPORTED = 0,
	PRELIM_XE_EUDEBUG_SUPPORTED,
	PRELIM_XE_EUDEBUG_ENABLED,
};

/**
 * struct prelim_xe_eudebug_event - Internal base event struct for eudebug
 */
struct prelim_xe_eudebug_event {
	/** @len: length of this event, including payload */
	u32 len;

	/** @type: message type */
	u16 type;

	/** @flags: message flags */
	u16 flags;

	/** @seqno: sequence number for ordering */
	u64 seqno;

	/** @reserved: reserved field MBZ */
	u64 reserved;

	/** @data: payload bytes */
	u8 data[];
};

struct prelim_xe_eudebug_event_envelope {
	struct list_head link;
	struct prelim_xe_eudebug_event *event;
};

/**
 * struct prelim_xe_eudebug_event_open - Internal event for client open/close
 */
struct prelim_xe_eudebug_event_open {
	/** @base: base event */
	struct prelim_xe_eudebug_event base;

	/** @client_handle: opaque handle for client */
	u64 client_handle;
};

/**
 * struct prelim_xe_eudebug_event_vm - Internal event for vm open/close
 */
struct prelim_xe_eudebug_event_vm {
	/** @base: base event */
	struct prelim_xe_eudebug_event base;

	/** @client_handle: client containing the vm open/close */
	u64 client_handle;

	/** @vm_handle: vm handle it's open/close */
	u64 vm_handle;
};

/**
 * struct prelim_xe_eudebug_event_exec_queue - Internal event for
 * exec_queue create/destroy
 */
struct prelim_xe_eudebug_event_exec_queue {
	/** @base: base event */
	struct prelim_xe_eudebug_event base;

	/** @client_handle: client for the engine create/destroy */
	u64 client_handle;

	/** @vm_handle: vm handle for the engine create/destroy */
	u64 vm_handle;

	/** @exec_queue_handle: engine handle */
	u64 exec_queue_handle;

	/** @engine_class: engine class */
	u32 engine_class;

	/** @width: submission width (number BB per exec) for this exec queue */
	u32 width;

	/** @lrc_handle: handles for each logical ring context created with this exec queue */
	u64 lrc_handle[] __counted_by(width);
};

/**
 * struct prelim_xe_eudebug_event_eu_attention - Internal event for EU attention
 */
struct prelim_xe_eudebug_event_eu_attention {
	/** @base: base event */
	struct prelim_xe_eudebug_event base;

	/** @client_handle: client for the attention */
	u64 client_handle;

	/** @exec_queue_handle: handle of exec_queue which raised attention */
	u64 exec_queue_handle;

	/** @lrc_handle: lrc handle of the workload which raised attention */
	u64 lrc_handle;

	/** @flags: eu attention event flags, currently MBZ */
	u32 flags;

	/** @bitmask_size: size of the bitmask, specific to device */
	u32 bitmask_size;

	/**
	 * @bitmask: reflects threads currently signalling attention,
	 * starting from natural hardware order of DSS=0, eu=0
	 */
	u8 bitmask[] __counted_by(bitmask_size);
};

/**
 * struct prelim_xe_eudebug_event_vm_bind - Internal event for vm bind/unbind operation
 */
struct prelim_xe_eudebug_event_vm_bind {
	/** @base: base event */
	struct prelim_xe_eudebug_event base;

	/** @client_handle: client for the VM BIND */
	u64 client_handle;
	/** @vm_handle: vm handle */
	u64 vm_handle;

	/** @flags: flags for bind */
	u32 flags;
	/** @num_binds: number of binds user should receive */
	u32 num_binds;
};

/**
 * struct prelim_xe_eudebug_event_vm_bind_op - Internal event for vm bind/unbind instance operation
 */
struct prelim_xe_eudebug_event_vm_bind_op {
	/** @base: base event */
	struct prelim_xe_eudebug_event base;

	/** @vm_bind_ref_seqno: seqno of the struct prelim_xe_eudebug_event_vm_bind */
	u64 vm_bind_ref_seqno;
	/** @num_extensions: number of extensions for this bind */
	u64 num_extensions;

	/** @addr: address of the bind */
	u64 addr; /* Zero for unmap all ? */
	/** @range: range of the bind */
	u64 range; /* Zero for unmap all ? */
};

/**
 * struct prelim_xe_eudebug_event_vm_bind_ufence - Internal event for vm bind ufence
 */
struct prelim_xe_eudebug_event_vm_bind_ufence {
	/** @base: base event */
	struct prelim_xe_eudebug_event base;
	/** @vm_bind_ref_seqno: seqno of parent struct prelim_xe_eudebug_event_vm_bind */
	u64 vm_bind_ref_seqno;
};

/**
 * struct prelim_xe_eudebug_event_metadata - Internal event for metadata
 */
struct prelim_xe_eudebug_event_metadata {
	/** @base: base event */
	struct prelim_xe_eudebug_event base;

	/** @client_handle: client for the attention */
	u64 client_handle;

	/** @metadata_handle: debug metadata handle it's created/destroyed */
	u64 metadata_handle;

	/** @type: metadata type, refer to xe_drm.h for options */
	u64 type;

	/** @len: size of metadata paylad */
	u64 len;
};

/**
 * struct prelim_xe_eudebug_event_vm_bind_op_metadata - Internal event for vm bind metadata
 */
struct prelim_xe_eudebug_event_vm_bind_op_metadata {
	/** @base: base event */
	struct prelim_xe_eudebug_event base;
	/** @vm_bind_op_ref_seqno: seqno of parent struct prelim_xe_eudebug_event_vm_bind_op */
	u64 vm_bind_op_ref_seqno;

	/** @metadata_handle: metadata handle */
	u64 metadata_handle;
	/** @metadata_cookie: metadata cookie */
	u64 metadata_cookie;
};

/**
 * struct prelim_xe_eudebug_event_pagefault - Internal event for EU Pagefault
 */
struct prelim_xe_eudebug_event_pagefault {
	/** @base: base event */
	struct prelim_xe_eudebug_event base;

	/** @client_handle: client for the Pagefault */
	u64 client_handle;

	/** @exec_queue_handle: handle of exec_queue which raised Pagefault */
	u64 exec_queue_handle;

	/** @lrc_handle: lrc handle of the workload which raised Pagefault */
	u64 lrc_handle;

	/** @flags: eu Pagefault event flags, currently MBZ */
	u32 flags;

	/**
	 * @bitmask_size: sum of size before/after/resolved att bits.
	 * It has three times the size of xe_eudebug_event_eu_attention.bitmask_size.
	 */
	u32 bitmask_size;

	/** @pagefault_address: The ppgtt address where the Pagefault occurred */
	u64 pagefault_address;

	/**
	 * @bitmask: Bitmask of thread attentions starting from natural,
	 * hardware order of DSS=0, eu=0, 8 attention bits per eu.
	 * The order of the bitmask array is before, after, resolved.
	 */
	u8 bitmask[];
};

/**
 * struct prelim_xe_eudebug_pagefault - eudebug structure for queuing pagefault
 */
struct prelim_xe_eudebug_pagefault {
	/** @list: link into the xe_eudebug.pagefaults */
	struct list_head list;
	/** @q: exec_queue which raised pagefault */
	struct xe_exec_queue *q;
	/** @lrc_idx: lrc index of the workload which raised pagefault */
	int lrc_idx;
	/** @vm: vm involved in a pagefault **/
	struct xe_vm *vm;
	/** @gt: gt involved in a pagefault **/
	struct xe_gt *gt;

	/** @fault: pagefault raw partial data passed from guc*/
	struct {
		/** @fault.addr: ppgtt address where the pagefault occurred */
		u64 addr;
		/** @fault.type: type of fault */
		int type;
		/** @fault.level: level of fault */
		int level;
		/** @fault.access: access type */
		int access;
	} fault;

	/** @attentions: struct holding aggregated attensions */
	struct {
		/** @attentions.before: state of attention bits before page fault WA processing */
		struct prelim_xe_eu_attentions before;
		/**
		 * @attentions.after: status of attention bits during page fault WA processing.
		 * It includes eu threads where attention bits are turned on for
		 * reasons other than page fault WA (breakpoint, interrupt, etc.).
		 */
		struct prelim_xe_eu_attentions after;
		/**
		 * @attentions.resolved: state of the attention bits after page fault WA.
		 * It includes the eu thread that caused the page fault.
		 * To determine the eu thread that caused the page fault,
		 * do XOR attentions.after and attentions.resolved.
		 */
		struct prelim_xe_eu_attentions resolved;
	} attentions;

	/**
	 * @deferred_resolved: to update attentions.resolved again when attention
	 * bits are ready if the eu thread fails to turn on attention bits within
	 * a certain time after page fault WA processing.
	 */
	bool deferred_resolved;
};

/**
 * struct prelim_xe_eudebug_event_sync_host - Internal event for sync host.
 */
struct prelim_xe_eudebug_event_sync_host {
	/** @base: base event */
	struct prelim_xe_eudebug_event base;

	/** @client_handle: client handle which workload stopped */
	u64 client_handle;

	/** @exec_queue_handle: handle of exec_queue which eus halted */
	u64 exec_queue_handle;

	/** @lrc_handle: lrc handle of the workload which eus halted */
	u64 lrc_handle;
};

/**
 * struct prelim_xe_eudebug_event_exec_queue_placements - Internal event for exec queue placement.
 */
struct prelim_xe_eudebug_event_exec_queue_placements {
	/** @base: base event */
	struct prelim_xe_eudebug_event base;

	/** @client_handle: client for the engine create/destroy */
	u64 client_handle;

	/** @vm_handle: vm handle for the engine create/destroy */
	u64 vm_handle;

	/** @exec_queue_handle: engine handle */
	u64 exec_queue_handle;

	/** @lrc_handle: lrc handle of the workload which caused this event */
	u64 lrc_handle;

	/** @num_placements: all possible placements for given lrc */
	u32 num_placements;

	/** @pad: padding */
	u32 pad;

	/**
	 * @instances: num_placements sized array containing
	 * struct drm_xe_engine_class_instance
	 */
	u64 instances[] __counted_by(num_placements);
};

#endif /* _PRELIM_XE_EUDEBUG_TYPES_H_ */
