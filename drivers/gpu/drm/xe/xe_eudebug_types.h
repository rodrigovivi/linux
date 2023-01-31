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

#include "xe_eudebug_proto.h"

struct xe_device;
struct workqueue_struct;

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

#define XE_EUDEBUG_MAX_EVENT_TYPE DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE

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
#define XE_EUDEBUG_RES_TYPE_COUNT	(XE_EUDEBUG_RES_TYPE_VM + 1)

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
};

#endif /* _XE_EUDEBUG_TYPES_H_ */
