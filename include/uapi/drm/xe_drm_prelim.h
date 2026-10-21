/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef _UAPI_XE_DRM_PRELIM_H_
#define _UAPI_XE_DRM_PRELIM_H_

#include <linux/limits.h>

#include "xe_drm.h"

/**
 * DOC: Xe PRELIM uAPI
 *
 * Reasoning:
 *
 * The DRM community imposes some strict requirements on the uAPI:
 *
 * - https://www.kernel.org/doc/html/latest/gpu/drm-uapi.html#open-source-userspace-requirements
 * - "The open-source userspace must not be a toy/test application, but the real thing."
 * - "The userspace patches must be against the canonical upstream, not some vendor fork."
 *
 * In some very specific cases, there will be a particular need to get a preliminary and
 * non-upstream uAPI merged in some of our internal branches. Either xe-internal, or
 * an RTL/PO branch, or a topic development branch. When this happens, the uAPI cannot
 * take a risk of conflicting IOCTL ranges with other preliminary uAPI or with a possible
 * real user space uAPI that could win the race towards upstream.
 *
 * 'PRELIM' uAPIs are APIs that are not yet merged on upstream. They were designed to be
 * in a different range in a way that the divergence with the upstream and other development
 * branches can be controlled and the conflicts minimized.
 *
 * It is also a mechanism that prevents user space regressions since the prelim modification
 * is always in a two-phase approach, where upstream and prelim can coexist for a period of
 * time while the UMDs adjust to the changes.
 *
 * Rules:
 *
 * - Communication will happen on a specific Teams channel: KMD uAPI Changes
 * - APIs to be considered Preliminary / WIP / Temporary to what will eventually be in upstream.
 *   It's designed to allow kernel and userspace to work together and make sure it works with all
 *   components before committing to support it forever in upstream (we don't want to break Linux's
 *   rule about not breaking userspace).
 * - IOCTL in separate file (this one).
 * - IOCTL, flags, enums, or any number that might conflict should be in separated range
 *   (i.e end of range like IOCTL numbers).
 * - PRELIM prefix mandatory in any define, struct, or non-static functions in PRELIM source files
 *   called by other source files (i.e xe_perf_ioctl -> prelim_xe_perf_ioctl)
 * - Code .c/.h using PRELIM should also be placed in prelim/ sub-directory for easier rebase.
 * - Two-Phase removal:
 *
 *   + When API needs to be modified in non-backwards compatible ways, the current PRELIM API
 *     cannot be removed (yet).
 *   + Either because it must change the behavior or because the final upstream one has landed.
 *   + If a new PRELIM is needed, it needs to be added as a new PRELIM_V<n+1> without removing the
 *     PRELIM_V<n>.
 *   + The previous one can only be removed after all user space components confirmed they are not
 *     using.
 *
 * Other prelim considerations:
 *
 * - Out of tree Sysfs and debugfs need to stay behind a 'prelim' directory.
 * - Out of tree module-parameters need to be identified by a PRELIM prefix.
 *   (xe.prelim_my_awesome_param=not_default)
 * - Internal/downstream declarations must be added here, not to xe_drm.h.
 * - The values in xe_drm_prelim.h must also be kept synchronized with values in xe_drm.h.
 * - PRELIM ioctl's: IOCTL numbers go down from 0x5f
 * - PRELIM reservation: xe-internal is the source of truth of the PRELIM stuff.
 *   Any developer working on a topic branch or RTL centric branch needs to first reserve the
 *   prelim range in this file at xe-internal branch.
 */

/*
 * Replace occurences of PRELIM flags in a bitmask with their non-prelim
 * equivalents from upstream.  E.g.,
 *
 *	FIX_PRELIM_FLAG(mask, FOO)
 *
 * at the top of a function will remove PRELIM_FOO from mask and, if found,
 * replace it with FOO.
 */
#define FIX_PRELIM_FLAG(mask, bitname) \
	if ((mask) & PRELIM_##bitname) (mask) = ((mask) & ~PRELIM_##bitname) | bitname

/*
 * If variable holds a specific prelim uapi value, replace it with the
 * non-prelim equivalent from upstream.
 *
 *	FIX_PRELIM_VALUE(var, FOO)
 *
 * will set var to FOO if var was PRELIM_FOO.
 */
#define FIX_PRELIM_VALUE(var, value) \
	if ((var) == PRELIM_##value) (var) = value

/*
 * Default marker for PRELIM values in shared flags/extensions
 */
#define PRELIM_DRM_XE_VALUE_INDICATOR		(1 << 16)

/*
 * Default mask for PRELIM values created by PRELIM_DRM_XE_VALUE_INDICATOR.
 * The PRELIM value shall be created by:
 * PRELIM_DRM_XE_VALUE_INDICATOR | value
 * and 'value' shall be smaller than MASK value.
 */
#define PRELIM_DRM_XE_VALUE_MASK		0xffff

/*
 * Value of the PRELIM flag/extension without PRELIM INDICATOR
 */
#define PRELIM_DRM_XE_VALUE(val)		((val) & PRELIM_DRM_XE_VALUE_MASK)
/*
 * IOCTL numbers listed below are reserved, they are taken up by other
 * components. Please add an unreserved ioctl number here to reserve that
 * number. xe-internal is the source of truth for the full reservation list.
 */

/*
 * PRELIM v2 eudebug uAPI
 *
 * The eudebug interface as being proposed upstream, under PRELIM names until
 * it lands there. It can be used side by side with other PRELIM eudebug uAPI
 * versions, but only one eudebug interface can be enabled on a device at a
 * time. Enabled through the prelim_v2_enable_eudebug sysfs attribute.
 */

/**
 * DOC: PRELIM_V2_DRM_XE_EUDEBUG_IOCTL_READ_EVENT
 *
 * Receive one event from the connection returned by
 * &PRELIM_V2_DRM_IOCTL_XE_EUDEBUG_CONNECT. The argument is a pointer to a
 * &struct prelim_v2_drm_xe_eudebug_event filled in as described there.
 *
 * A connection starts out blocking, and then waits up to five seconds for
 * an event to arrive; O_NONBLOCK can be set on it with fcntl(). Only one
 * reader at a time is allowed on a connection.
 *
 * Return: 0 on success. Negative error code on failure:
 *
 * - -EMSGSIZE if the pending event is larger than the supplied len. len is
 *   updated with the size needed and the event stays queued.
 * - -ETIMEDOUT if the blocking wait expired with no event.
 * - -EAGAIN if O_NONBLOCK was set and no event was queued.
 * - -EBUSY if another thread is already reading on this connection.
 * - -ENOTCONN if the debug target is gone.
 *
 * Detaching discards whatever is still queued: the events describe resources
 * of a client that no longer exists, and the handles in them can no longer be
 * resolved. A debugger that gets -ENOTCONN, or EPOLLHUP from poll(), should
 * treat the event stream as having ended at an unspecified point rather than
 * assume it saw everything the target did.
 */
#define PRELIM_V2_DRM_XE_EUDEBUG_IOCTL_READ_EVENT		_IOWR('j', 0x40, struct prelim_v2_drm_xe_eudebug_event)
#define PRELIM_V2_DRM_XE_EUDEBUG_IOCTL_ACK_EVENT		_IOW('j', 0x41, struct prelim_v2_drm_xe_eudebug_ack)
#define PRELIM_V2_DRM_XE_EUDEBUG_IOCTL_VM_OPEN		_IOW('j', 0x42, struct prelim_v2_drm_xe_eudebug_vm_open)
#define PRELIM_V2_DRM_XE_EUDEBUG_IOCTL_EU_CONTROL		_IOWR('j', 0x43, struct prelim_v2_drm_xe_eudebug_eu_control)

/**
 * struct prelim_v2_drm_xe_eudebug_event - Base type of event delivered by xe_eudebug.
 *
 * Base event for xe_eudebug interface.
 *
 * For receiving events :c:member:`prelim_v2_drm_xe_eudebug_event.type` has to
 * be PRELIM_V2_DRM_XE_EUDEBUG_EVENT_READ. On return, this is set to the type
 * of event received. :c:member:`prelim_v2_drm_xe_eudebug_event.len` has to be
 * set to maximum size that can be received. On return, len will be set
 * to the event size. If the pending event was larger than this size,
 * -EMSGSIZE is returned instead of 0 and the caller should retry with a larger
 * allocated receive length.
 *
 * :c:member:`prelim_v2_drm_xe_eudebug_event.seqno` can be used to form a timeline
 * as event delivery order does not guarantee event creation
 * order. Must be set to zero.
 *
 * :c:member:`prelim_v2_drm_xe_eudebug_event.flags` will indicate if a resource was
 * created, destroyed, or if its state changed. Must be set to zero.
 *
 * If PRELIM_V2_DRM_XE_EUDEBUG_EVENT_NEED_ACK is set, xe_eudebug
 * will hold the said resource until it is acked by userspace
 * using the acking ioctl with the seqno of the said event.
 *
 * %PRELIM_V2_DRM_XE_EUDEBUG_EVENT_NONE and %PRELIM_V2_DRM_XE_EUDEBUG_EVENT_READ are never
 * delivered: the first is unused and the second is the request type for
 * the read ioctl.
 *
 * :c:member:`prelim_v2_drm_xe_eudebug_event.len` is authoritative for the event that
 * was read, and events may grow fields in later kernels. Step to the next
 * event and skip a type this debugger does not know by @len rather than by
 * the size of the matching struct in the header it was built against.
 */
struct prelim_v2_drm_xe_eudebug_event {
	/** @len: Length */
	__u32 len;

	/** @type: Type */
	__u16 type;
#define PRELIM_V2_DRM_XE_EUDEBUG_EVENT_NONE		0
#define PRELIM_V2_DRM_XE_EUDEBUG_EVENT_READ		1
#define PRELIM_V2_DRM_XE_EUDEBUG_EVENT_VM			2
#define PRELIM_V2_DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE		3
#define PRELIM_V2_DRM_XE_EUDEBUG_EVENT_VM_BIND		4
#define PRELIM_V2_DRM_XE_EUDEBUG_EVENT_VM_BIND_OP_DEBUG_DATA	5
#define PRELIM_V2_DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE	6
#define PRELIM_V2_DRM_XE_EUDEBUG_EVENT_EU_ATTENTION	7
#define PRELIM_V2_DRM_XE_EUDEBUG_EVENT_PAGEFAULT		8

	/** @flags: Flags */
	__u16 flags;
#define PRELIM_V2_DRM_XE_EUDEBUG_EVENT_CREATE		(1 << 0)
#define PRELIM_V2_DRM_XE_EUDEBUG_EVENT_DESTROY		(1 << 1)
#define PRELIM_V2_DRM_XE_EUDEBUG_EVENT_STATE_CHANGE	(1 << 2)
#define PRELIM_V2_DRM_XE_EUDEBUG_EVENT_NEED_ACK		(1 << 3)

	/**
	 * @seqno: Sequence number, for the debugger to construct a timeline.
	 *
	 * Allocated per connection and monotonically increasing, taken at the
	 * point the event is generated rather than when it is queued.
	 *
	 * Event generation is asynchronous: threads in the target produce
	 * events concurrently, so events are not guaranteed to arrive in
	 * seqno order. The numbering may also contain gaps, as a number can
	 * be allocated for an event that is never queued. Neither a lower
	 * seqno arriving after a higher one nor a gap means an event was
	 * lost; if an event cannot be queued the connection is disconnected
	 * instead of silently dropping it.
	 *
	 * Use this to order and correlate the events that have been read,
	 * which is what timeline means here, and to refer back to an earlier
	 * event: %PRELIM_V2_DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE and
	 * &prelim_v2_drm_xe_eudebug_ack.seqno identify the event they belong to this
	 * way. Do not use it as a delivery order guarantee.
	 */
	__u64 seqno;

	/** @reserved: Reserved field, must be zero. */
	__u64 reserved;
};

/**
 * struct prelim_v2_drm_xe_eudebug_event_vm - VM event
 *
 * VM event is delivered when vm is created or destroyed. Only vms created
 * with %DRM_XE_VM_CREATE_FLAG_LR_MODE are reported; other vms of the
 * target are not visible to the debugger.
 */
struct prelim_v2_drm_xe_eudebug_event_vm {
	/** @base: base event */
	struct prelim_v2_drm_xe_eudebug_event base;

	/** @vm_handle: unique handle for vm */
	__u64 vm_handle;
};

/**
 * struct prelim_v2_drm_xe_eudebug_event_exec_queue - Exec Queue resource event
 *
 * Resource creation/destruction event for an Exec Queue
 */
struct prelim_v2_drm_xe_eudebug_event_exec_queue {
	/** @base: base event */
	struct prelim_v2_drm_xe_eudebug_event base;

	/** @vm_handle: the vm handle this exec queue belongs to */
	__u64 vm_handle;

	/** @exec_queue_handle: unique handle for this exec queue */
	__u64 exec_queue_handle;

	/** @engine_class: engine class, a %DRM_XE_ENGINE_CLASS_* value */
	__u32 engine_class;

	/** @width: width of exec queue, how many lrcs (handles) it has */
	__u32 width;

	/** @lrc_handle: array of lrc handles */
	__u64 lrc_handle[];
};

/**
 * struct prelim_v2_drm_xe_eudebug_event_vm_bind - VM Bind Event
 *
 * When the client (debuggee) calls the vm_bind_ioctl with the
 * DRM_XE_VM_BIND_OP_[ADD|REMOVE]_DEBUG_DATA operation, the following event
 * sequence will be created (for the debugger)::
 *
 *  ┌───────────────────────┐
 *  │  EVENT_VM_BIND        ├──────────────────┬─┬┄┐
 *  └───────────────────────┘                  │ │ ┊
 *      ┌──────────────────────────────────┐   │ │ ┊
 *      │ EVENT_VM_BIND_OP_DEBUG_DATA #1   ├───┘ │ ┊
 *      └──────────────────────────────────┘     │ ┊
 *                      ...                      │ ┊
 *      ┌──────────────────────────────────┐     │ ┊
 *      │ EVENT_VM_BIND_OP_DEBUG_DATA #n   ├─────┘ ┊
 *      └──────────────────────────────────┘       ┊
 *                                                 ┊
 *      ┌┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┐       ┊
 *      ┊ EVENT_UFENCE                     ├┄┄┄┄┄┄┄┘
 *      └┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┘
 *
 * All the events below VM_BIND will reference the VM_BIND
 * they associate with, by field .vm_bind_ref_seqno.
 * EVENT_UFENCE will only be included if the client did
 * attach sync of type UFENCE into its vm_bind_ioctl(). It is
 * not sent, even with PRELIM_V2_DRM_XE_EUDEBUG_EVENT_VM_BIND_FLAG_UFENCE
 * set, if the vm_bind_ioctl() fails after the ops were executed
 * and the fence is never signalled.
 *
 * When EVENT_UFENCE is sent by the driver, all the OPs of
 * the original VM_BIND are completed and the [addr,range]
 * contained in them are present and modifiable through the
 * vm accessors. Accessing [addr, range] before related ufence
 * event will lead to undefined results as the actual bind
 * operations are async and the backing storage might not
 * be there on a moment of receiving the event.
 *
 * Client's UFENCE sync will be held by the driver: client's
 * drm_xe_wait_ufence will not complete and the value of the ufence
 * won't appear until ufence is acked by the debugger process calling
 * PRELIM_V2_DRM_XE_EUDEBUG_IOCTL_ACK_EVENT with the event_ufence.base.seqno.
 * This will signal the fence, .value will update and the wait will
 * complete allowing the client to continue.
 *
 */
struct prelim_v2_drm_xe_eudebug_event_vm_bind {
	/** @base: Base event */
	struct prelim_v2_drm_xe_eudebug_event base;

	/** @vm_handle: VM handle for this bind */
	__u64 vm_handle;

	/** @flags: Bind specific flags */
	__u32 flags;
#define PRELIM_V2_DRM_XE_EUDEBUG_EVENT_VM_BIND_FLAG_UFENCE (1 << 0)

	/** @num_bind_ops: How many [ADD|REMOVE]_DEBUG_DATA operations this bind has */
	__u32 num_bind_ops;
};

/**
 * struct prelim_v2_drm_xe_eudebug_event_vm_bind_op_debug_data - VM Bind Op Debug Data Event
 *
 * When the target drm client issues a vm bind, each operation of type
 * %PRELIM_V2_DRM_XE_VM_BIND_OP_ADD_DEBUG_DATA or %PRELIM_V2_DRM_XE_VM_BIND_OP_REMOVE_DEBUG_DATA
 * will generate an event of this type. For reference see
 * :c:type:`prelim_v2_drm_xe_vm_bind_op_ext_debug_data`.
 */
struct prelim_v2_drm_xe_eudebug_event_vm_bind_op_debug_data {
	/** @base: Base event */
	struct prelim_v2_drm_xe_eudebug_event base;

	/** @vm_bind_ref_seqno: Parent :c:member:`prelim_v2_drm_xe_eudebug_event_vm_bind.base.seqno` */
	__u64 vm_bind_ref_seqno;

	/** @addr: Address of the debug data mapping */
	__u64 addr;

	/** @range: Range of the debug data mapping */
	__u64 range;

	/** @flags: Debug data flags */
	__u64 flags;

	/** @offset: Offset into the debug data file */
	__u64 offset;

	/** @reserved: Reserved, must be zero */
	__u64 reserved;

	/**
	 * @pseudopath: Pseudopath when
	 * %PRELIM_V2_DRM_XE_VM_BIND_DEBUG_DATA_FLAG_PSEUDO is set in @flags, zero
	 * otherwise
	 */
	__u64 pseudopath;

	/**
	 * @pathname: Path to the debug data file
	 *
	 * Present only when %PRELIM_V2_DRM_XE_VM_BIND_DEBUG_DATA_FLAG_PSEUDO is not
	 * set in @flags, and covered by
	 * :c:member:`prelim_v2_drm_xe_eudebug_event.len` rather than by a fixed size:
	 * the event is as long as the path it carries. The path is NUL
	 * terminated and the terminator is included in @len, which may then
	 * be padded with further zeroes to keep the event size aligned, so
	 * read it as a string rather than taking the whole tail of the
	 * event as the name.
	 */
	char pathname[];
};

/**
 * struct prelim_v2_drm_xe_eudebug_event_vm_bind_ufence - User Fence Event
 *
 * When target drm client does vm bind with associated user fence,
 * this event will be delivered. This event will have
 * PRELIM_V2_DRM_XE_EUDEBUG_EVENT_NEED_ACK set in :c:member:`prelim_v2_drm_xe_eudebug_event.flags`
 * and upon receiving this event you need to ack it with
 * PRELIM_V2_DRM_XE_EUDEBUG_IOCTL_ACK_EVENT.
 *
 */
struct prelim_v2_drm_xe_eudebug_event_vm_bind_ufence {
	/** @base: Base event */
	struct prelim_v2_drm_xe_eudebug_event base;

	/** @vm_bind_ref_seqno: Parent :c:member:`prelim_v2_drm_xe_eudebug_event_vm_bind.base.seqno` */
	__u64 vm_bind_ref_seqno;
};

/**
 * struct prelim_v2_drm_xe_eudebug_ack - Deliver ack for an event
 *
 * If event base.flags has PRELIM_V2_DRM_XE_EUDEBUG_EVENT_NEED_ACK set,
 * then the associated resource processing is held for client and
 * thus held for the debugger. In order to release the client,
 * ack needs to be delivered with PRELIM_V2_DRM_XE_EUDEBUG_IOCTL_ACK_EVENT.
 *
 * The ioctl returns -EBUSY while discovery is still in progress. Retry.
 */
struct prelim_v2_drm_xe_eudebug_ack {
	/** @type: Type, must be zero */
	__u32 type;

	/** @flags: Flags, must be zero */
	__u32 flags;

	/** @seqno: Seqno of event that is to be acked */
	__u64 seqno;

	/** @reserved: Reserved field, must be zero. */
	__u64 reserved;
};

/**
 * struct prelim_v2_drm_xe_eudebug_vm_open - Open a target vm
 *
 * Open target VM for reading and writing with PRELIM_V2_DRM_XE_EUDEBUG_IOCTL_VM_OPEN.
 *
 * The ioctl returns -EBUSY while discovery is still in progress. Retry.
 *
 * File descriptor is returned which can be used with pread and pwrite
 * to inspect and modify the target VM.
 *
 * Multiple operations can be synced with calling fsync(fd). The
 * fsync will time out after timeout_ns if the VM can't be guaranteed
 * to be in sync. Caller should re-read the state with pread again.
 *
 * pread() and pwrite() take a vm address as the offset and transfer
 * within one mapping, so a transfer that reaches its end is short rather
 * than continuing into the next one. Errors worth telling apart:
 *
 * - -EINVAL if no mapping of this vm covers the offset, or the vm handle
 *   has gone. Retrying does not help until the target binds the range.
 * - -EBUSY if the mapping is there but its backing store is not ready to
 *   be touched yet. Retry.
 * - -EINTR if the wait for that backing store was interrupted.
 * - -ENOTCONN if the debug target is gone.
 * - -ETIMEDOUT from fsync() if the flush did not complete within
 *   @timeout_ns. The vm may be out of sync, so pread() again.
 *
 */
struct prelim_v2_drm_xe_eudebug_vm_open {
	/** @extensions: Reserved for future extensions, must be zero */
	__u64 extensions;

	/** @vm_handle: handle of vm to be accessed */
	__u64 vm_handle;

	/** @flags: flags, must be zero */
	__u64 flags;

	/**
	 * @timeout_ns: Timeout value in nanoseconds
	 *
	 * Applies to each fsync() on the returned fd. The flush waits on
	 * hardware while holding a device wide lock, so the value is capped:
	 * anything above 10 seconds is rejected with -EINVAL. 0 selects the
	 * cap. The value is rounded up to microseconds.
	 */
	__u64 timeout_ns;
};

/**
 * struct prelim_v2_drm_xe_eudebug_eu_control - Control EU states
 *
 * Issue commands to execution units in hardware.
 *
 * With PRELIM_V2_DRM_XE_EUDEBUG_IOCTL_EU_CONTROL debugger can
 * interrupt all threads on execution units, query thread
 * state and resume execution.
 *
 * The ioctl returns -EBUSY while discovery is still in progress. Retry.
 *
 * :c:member:`prelim_v2_drm_xe_eudebug_eu_control.seqno`
 * will be updated to the timeline point when
 * the command was issued.
 *
 * :c:member:`prelim_v2_drm_xe_eudebug_eu_control.cmd` can
 * be following:
 *
 * *PRELIM_V2_DRM_XE_EUDEBUG_EU_CONTROL_CMD_INTERRUPT_ALL*
 *  will instruct hardware to stop all threads on EUs
 *  for exec_queue:lrc. This command takes no bitmask, so
 *  :c:member:`prelim_v2_drm_xe_eudebug_eu_control.bitmask_size`
 *  must be set to zero.
 *
 * *PRELIM_V2_DRM_XE_EUDEBUG_EU_CONTROL_CMD_STOPPED*
 *  returns the bitmask for threads that are
 *  in so called attention state.
 *
 * *PRELIM_V2_DRM_XE_EUDEBUG_EU_CONTROL_CMD_RESUME*
 *  resumes the threads whose bit is set in the bitmask.
 *
 */
struct prelim_v2_drm_xe_eudebug_eu_control {
	/** @cmd: Command for execution units */
#define PRELIM_V2_DRM_XE_EUDEBUG_EU_CONTROL_CMD_INTERRUPT_ALL	0
#define PRELIM_V2_DRM_XE_EUDEBUG_EU_CONTROL_CMD_STOPPED		1
#define PRELIM_V2_DRM_XE_EUDEBUG_EU_CONTROL_CMD_RESUME		2
	__u32 cmd;

	/** @flags: Flags, must be set to zero */
	__u32 flags;

	/** @seqno: Seqno, must be set to zero */
	__u64 seqno;

	/** @exec_queue_handle: Exec queue handle for the command */
	__u64 exec_queue_handle;

	/** @lrc_handle: LRC handle for the command */
	__u64 lrc_handle;

	/** @reserved: Reserved field, must be set to zero */
	__u32 reserved;

	/**
	 * @bitmask_size: Bitmask size in bytes
	 *
	 * Only the leading bytes that fit are used. If this differs from
	 * the size the hardware bitmask needs, it is overwritten with that
	 * size on return, so a short buffer can be grown and the command
	 * retried.
	 */
	__u32 bitmask_size;

	/** @bitmask_ptr: Bitmask pointer, each bit is one thread */
	__u64 bitmask_ptr;
};

/**
 * struct prelim_v2_drm_xe_eudebug_event_eu_attention - EU Attention Event
 *
 * Whenever there is any thread in halted/attentions state, this
 * event will be delivered. The event will be delivered periodically
 * until there are no attentions detected.
 *
 */
struct prelim_v2_drm_xe_eudebug_event_eu_attention {
	/** @base: base event */
	struct prelim_v2_drm_xe_eudebug_event base;

	/** @exec_queue_handle: Exec queue handle for the attentions */
	__u64 exec_queue_handle;

	/** @lrc_handle: LRC handle for the attentions */
	__u64 lrc_handle;

	/**
	 * @flags: Reserved for future use, reads as zero
	 *
	 * Event wide flags are carried in
	 * :c:member:`prelim_v2_drm_xe_eudebug_event_eu_attention.base`.
	 */
	__u32 flags;

	/** @bitmask_size: Bitmask size in bytes for bitmask[] */
	__u32 bitmask_size;

	/**
	 * @bitmask: Attention bits, one per thread
	 *
	 * @bitmask_size is always a multiple of 4, and the array is 8 byte
	 * aligned within the event, so it can be scanned as u32 or u64
	 * words instead of byte at a time. The content is hardware register
	 * state in native endianness.
	 */
	__u8 bitmask[];
};

/**
 * struct prelim_v2_drm_xe_eudebug_event_pagefault - EU Pagefault Event
 *
 * Delivered when a thread of a debuggable exec queue takes a pagefault that
 * the debugger is expected to resolve. The EUs are halted while the fault is
 * handled, and the attention state is captured on the way in and on the way
 * out, so that the threads the fault itself stopped can be told apart from
 * the ones that were already halted.
 */
struct prelim_v2_drm_xe_eudebug_event_pagefault {
	/** @base: base event */
	struct prelim_v2_drm_xe_eudebug_event base;

	/** @exec_queue_handle: Exec queue handle for the faulting threads */
	__u64 exec_queue_handle;

	/** @lrc_handle: LRC handle for the faulting threads */
	__u64 lrc_handle;

	/**
	 * @flags: Reserved for future use, reads as zero
	 *
	 * Event wide flags are carried in
	 * :c:member:`prelim_v2_drm_xe_eudebug_event_pagefault.base`.
	 */
	__u32 flags;

	/** @bitmask_size: Combined size in bytes of the bitmaps in bitmask[] */
	__u32 bitmask_size;

	/** @pagefault_address: The address that faulted */
	__u64 pagefault_address;

	/**
	 * @bitmask: Three attention bitmaps back to back, one bit per thread
	 *
	 * @bitmask_size covers all three, so each one is @bitmask_size / 3
	 * bytes, and they appear in this order:
	 *
	 *  - before: threads already in attention when the fault arrived
	 *  - after: threads that halted in response to the fault being
	 *    taken. A thread still stalled on the fault itself cannot run
	 *    the instruction that would halt it, so it does not appear
	 *    here; if every thread faulted, this is empty
	 *  - resolved: threads in attention once the fault was serviced,
	 *    which is where threads from the second group turn up
	 *
	 * Each has the same layout, alignment and endianness as
	 * :c:member:`prelim_v2_drm_xe_eudebug_event_eu_attention.bitmask`.
	 */
	__u8 bitmask[];
};


#define PRELIM_V2_DRM_XE_EUDEBUG_CONNECT		0x5a
#define PRELIM_V2_DRM_IOCTL_XE_EUDEBUG_CONNECT		DRM_IOW(DRM_COMMAND_BASE + PRELIM_V2_DRM_XE_EUDEBUG_CONNECT, struct prelim_v2_drm_xe_eudebug_connect)

/**
 * struct prelim_v2_drm_xe_eudebug_connect - Input of &PRELIM_V2_DRM_IOCTL_XE_EUDEBUG_CONNECT
 *
 * This structure is used to connect to an eudebug interface of target drm file.
 *
 * On connect, the resources the target already has are first relayed to
 * the debugger as events. This is called discovery.
 */
struct prelim_v2_drm_xe_eudebug_connect {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

	/** @fd: Debug target DRM client fd */
	__u64 fd;

	/** @flags: Flags, MBZ */
	__u64 flags;

	/** @reserved: MBZ */
	__u64 reserved;
};

#define   PRELIM_V2_DRM_XE_EXEC_QUEUE_SET_PROPERTY_EUDEBUG		(PRELIM_DRM_XE_VALUE_INDICATOR | 1)
#define     PRELIM_V2_DRM_XE_EXEC_QUEUE_EUDEBUG_FLAG_ENABLE		(1 << 0)

#define	PRELIM_V2_XE_VM_BIND_OP_EXTENSIONS_DEBUG_DATA (PRELIM_DRM_XE_VALUE_INDICATOR | 1)

#define PRELIM_V2_DRM_XE_VM_BIND_OP_ADD_DEBUG_DATA	(PRELIM_DRM_XE_VALUE_INDICATOR | 0)
#define PRELIM_V2_DRM_XE_VM_BIND_OP_REMOVE_DEBUG_DATA	(PRELIM_DRM_XE_VALUE_INDICATOR | 1)

/**
 * struct prelim_v2_drm_xe_vm_bind_op_ext_debug_data - debug data extension struct for
 * :c:type:`drm_xe_vm_bind_op`
 *
 * The GPU VM can be annotated by issuing a bind operation with the
 * :c:member:`drm_xe_vm_bind_op.op` set to %PRELIM_V2_DRM_XE_VM_BIND_OP_ADD_DEBUG_DATA or
 * %PRELIM_V2_DRM_XE_VM_BIND_OP_REMOVE_DEBUG_DATA. Each such operation has to provide a
 * :c:type:`prelim_v2_drm_xe_vm_bind_op_ext_debug_data` extension, which describes the
 * debug data to add or remove.
 *
 * This extension can either point to a file that contains relevant debug data
 * or annotate the VM range with a pseudopath by setting the
 * %PRELIM_V2_DRM_XE_VM_BIND_DEBUG_DATA_FLAG_PSEUDO flag and providing one of the supported
 * pseudopath values:
 *
 *  - %PRELIM_V2_DRM_XE_VM_BIND_DEBUG_DATA_PSEUDO_MODULE_AREA
 *  - %PRELIM_V2_DRM_XE_VM_BIND_DEBUG_DATA_PSEUDO_SBA_AREA
 *  - %PRELIM_V2_DRM_XE_VM_BIND_DEBUG_DATA_PSEUDO_SIP_AREA
 *
 */
struct prelim_v2_drm_xe_vm_bind_op_ext_debug_data {
	/** @base: Base user extension */
	struct drm_xe_user_extension base;

	/** @addr: Address of the debug data mapping */
	__u64 addr;

	/** @range: Range of the debug data mapping */
	__u64 range;

#define PRELIM_V2_DRM_XE_VM_BIND_DEBUG_DATA_FLAG_PSEUDO (1 << 0)
	/** @flags: Debug data flags */
	__u64 flags;

	/**
	 * @offset: Offset into the debug data file, MBZ when
	 * %PRELIM_V2_DRM_XE_VM_BIND_DEBUG_DATA_FLAG_PSEUDO is set
	 */
	__u64 offset;

	/** @reserved: Reserved */
	__u64 reserved;

	union {
#define PRELIM_V2_DRM_XE_VM_BIND_DEBUG_DATA_PSEUDO_MODULE_AREA	0x1
#define PRELIM_V2_DRM_XE_VM_BIND_DEBUG_DATA_PSEUDO_SBA_AREA	0x2
#define PRELIM_V2_DRM_XE_VM_BIND_DEBUG_DATA_PSEUDO_SIP_AREA	0x3
		/**
		 * @pseudopath: Pseudopath used when
		 * %PRELIM_V2_DRM_XE_VM_BIND_DEBUG_DATA_FLAG_PSEUDO is set
		 */
		__u64 pseudopath;

		/** @pathname: Path to the debug data file */
		char pathname[PATH_MAX];
	};
};


#endif /* _UAPI_XE_DRM_PRELIM_H_ */
