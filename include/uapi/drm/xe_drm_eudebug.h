/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef _UAPI_XE_DRM_EUDEBUG_H_
#define _UAPI_XE_DRM_EUDEBUG_H_

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * DOC: DRM_XE_EUDEBUG_IOCTL_READ_EVENT
 *
 * Receive one event from the connection returned by
 * &DRM_IOCTL_XE_EUDEBUG_CONNECT. The argument is a pointer to a
 * &struct drm_xe_eudebug_event filled in as described there.
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
#define DRM_XE_EUDEBUG_IOCTL_READ_EVENT		_IOWR('j', 0x40, struct drm_xe_eudebug_event)
#define DRM_XE_EUDEBUG_IOCTL_ACK_EVENT		_IOW('j', 0x41, struct drm_xe_eudebug_ack)
#define DRM_XE_EUDEBUG_IOCTL_VM_OPEN		_IOW('j', 0x42, struct drm_xe_eudebug_vm_open)
#define DRM_XE_EUDEBUG_IOCTL_EU_CONTROL		_IOWR('j', 0x43, struct drm_xe_eudebug_eu_control)

/**
 * struct drm_xe_eudebug_event - Base type of event delivered by xe_eudebug.
 *
 * Base event for xe_eudebug interface.
 *
 * For receiving events :c:member:`drm_xe_eudebug_event.type` has to
 * be DRM_XE_EUDEBUG_EVENT_READ. On return, this is set to the type
 * of event received. :c:member:`drm_xe_eudebug_event.len` has to be
 * set to maximum size that can be received. On return, len will be set
 * to the event size. If the pending event was larger than this size,
 * -EMSGSIZE is returned instead of 0 and the caller should retry with a larger
 * allocated receive length.
 *
 * :c:member:`drm_xe_eudebug_event.seqno` can be used to form a timeline
 * as event delivery order does not guarantee event creation
 * order. Must be set to zero.
 *
 * :c:member:`drm_xe_eudebug_event.flags` will indicate if a resource was
 * created, destroyed, or if its state changed. Must be set to zero.
 *
 * If DRM_XE_EUDEBUG_EVENT_NEED_ACK is set, xe_eudebug
 * will hold the said resource until it is acked by userspace
 * using the acking ioctl with the seqno of the said event.
 *
 * %DRM_XE_EUDEBUG_EVENT_NONE and %DRM_XE_EUDEBUG_EVENT_READ are never
 * delivered: the first is unused and the second is the request type for
 * the read ioctl.
 *
 * :c:member:`drm_xe_eudebug_event.len` is authoritative for the event that
 * was read, and events may grow fields in later kernels. Step to the next
 * event and skip a type this debugger does not know by @len rather than by
 * the size of the matching struct in the header it was built against.
 */
struct drm_xe_eudebug_event {
	/** @len: Length */
	__u32 len;

	/** @type: Type */
	__u16 type;
#define DRM_XE_EUDEBUG_EVENT_NONE		0
#define DRM_XE_EUDEBUG_EVENT_READ		1
#define DRM_XE_EUDEBUG_EVENT_VM			2
#define DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE		3
#define DRM_XE_EUDEBUG_EVENT_VM_BIND		4
#define DRM_XE_EUDEBUG_EVENT_VM_BIND_OP_DEBUG_DATA	5
#define DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE	6
#define DRM_XE_EUDEBUG_EVENT_EU_ATTENTION	7
#define DRM_XE_EUDEBUG_EVENT_PAGEFAULT		8

	/** @flags: Flags */
	__u16 flags;
#define DRM_XE_EUDEBUG_EVENT_CREATE		(1 << 0)
#define DRM_XE_EUDEBUG_EVENT_DESTROY		(1 << 1)
#define DRM_XE_EUDEBUG_EVENT_STATE_CHANGE	(1 << 2)
#define DRM_XE_EUDEBUG_EVENT_NEED_ACK		(1 << 3)

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
	 * event: %DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE and
	 * &drm_xe_eudebug_ack.seqno identify the event they belong to this
	 * way. Do not use it as a delivery order guarantee.
	 */
	__u64 seqno;

	/** @reserved: Reserved field, must be zero. */
	__u64 reserved;
};

/**
 * struct drm_xe_eudebug_event_vm - VM event
 *
 * VM event is delivered when vm is created or destroyed. Only vms created
 * with %DRM_XE_VM_CREATE_FLAG_LR_MODE are reported; other vms of the
 * target are not visible to the debugger.
 */
struct drm_xe_eudebug_event_vm {
	/** @base: base event */
	struct drm_xe_eudebug_event base;

	/** @vm_handle: unique handle for vm */
	__u64 vm_handle;
};

/**
 * struct drm_xe_eudebug_event_exec_queue - Exec Queue resource event
 *
 * Resource creation/destruction event for an Exec Queue
 */
struct drm_xe_eudebug_event_exec_queue {
	/** @base: base event */
	struct drm_xe_eudebug_event base;

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
 * struct drm_xe_eudebug_event_vm_bind - VM Bind Event
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
 * not sent, even with DRM_XE_EUDEBUG_EVENT_VM_BIND_FLAG_UFENCE
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
 * DRM_XE_EUDEBUG_IOCTL_ACK_EVENT with the event_ufence.base.seqno.
 * This will signal the fence, .value will update and the wait will
 * complete allowing the client to continue.
 *
 */
struct drm_xe_eudebug_event_vm_bind {
	/** @base: Base event */
	struct drm_xe_eudebug_event base;

	/** @vm_handle: VM handle for this bind */
	__u64 vm_handle;

	/** @flags: Bind specific flags */
	__u32 flags;
#define DRM_XE_EUDEBUG_EVENT_VM_BIND_FLAG_UFENCE (1 << 0)

	/** @num_bind_ops: How many [ADD|REMOVE]_DEBUG_DATA operations this bind has */
	__u32 num_bind_ops;
};

/**
 * struct drm_xe_eudebug_event_vm_bind_op_debug_data - VM Bind Op Debug Data Event
 *
 * When the target drm client issues a vm bind, each operation of type
 * %DRM_XE_VM_BIND_OP_ADD_DEBUG_DATA or %DRM_XE_VM_BIND_OP_REMOVE_DEBUG_DATA
 * will generate an event of this type. For reference see
 * :c:type:`drm_xe_vm_bind_op_ext_debug_data`.
 */
struct drm_xe_eudebug_event_vm_bind_op_debug_data {
	/** @base: Base event */
	struct drm_xe_eudebug_event base;

	/** @vm_bind_ref_seqno: Parent :c:member:`drm_xe_eudebug_event_vm_bind.base.seqno` */
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
	 * %DRM_XE_VM_BIND_DEBUG_DATA_FLAG_PSEUDO is set in @flags, zero
	 * otherwise
	 */
	__u64 pseudopath;

	/**
	 * @pathname: Path to the debug data file
	 *
	 * Present only when %DRM_XE_VM_BIND_DEBUG_DATA_FLAG_PSEUDO is not
	 * set in @flags, and covered by
	 * :c:member:`drm_xe_eudebug_event.len` rather than by a fixed size:
	 * the event is as long as the path it carries. The path is NUL
	 * terminated and the terminator is included in @len, which may then
	 * be padded with further zeroes to keep the event size aligned, so
	 * read it as a string rather than taking the whole tail of the
	 * event as the name.
	 */
	char pathname[];
};

/**
 * struct drm_xe_eudebug_event_vm_bind_ufence - User Fence Event
 *
 * When target drm client does vm bind with associated user fence,
 * this event will be delivered. This event will have
 * DRM_XE_EUDEBUG_EVENT_NEED_ACK set in :c:member:`drm_xe_eudebug_event.flags`
 * and upon receiving this event you need to ack it with
 * DRM_XE_EUDEBUG_IOCTL_ACK_EVENT.
 *
 */
struct drm_xe_eudebug_event_vm_bind_ufence {
	/** @base: Base event */
	struct drm_xe_eudebug_event base;

	/** @vm_bind_ref_seqno: Parent :c:member:`drm_xe_eudebug_event_vm_bind.base.seqno` */
	__u64 vm_bind_ref_seqno;
};

/**
 * struct drm_xe_eudebug_ack - Deliver ack for an event
 *
 * If event base.flags has DRM_XE_EUDEBUG_EVENT_NEED_ACK set,
 * then the associated resource processing is held for client and
 * thus held for the debugger. In order to release the client,
 * ack needs to be delivered with DRM_XE_EUDEBUG_IOCTL_ACK_EVENT.
 *
 * The ioctl returns -EBUSY while discovery is still in progress. Retry.
 */
struct drm_xe_eudebug_ack {
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
 * struct drm_xe_eudebug_vm_open - Open a target vm
 *
 * Open target VM for reading and writing with DRM_XE_EUDEBUG_IOCTL_VM_OPEN.
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
struct drm_xe_eudebug_vm_open {
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
 * struct drm_xe_eudebug_eu_control - Control EU states
 *
 * Issue commands to execution units in hardware.
 *
 * With DRM_XE_EUDEBUG_IOCTL_EU_CONTROL debugger can
 * interrupt all threads on execution units, query thread
 * state and resume execution.
 *
 * The ioctl returns -EBUSY while discovery is still in progress. Retry.
 *
 * :c:member:`drm_xe_eudebug_eu_control.seqno`
 * will be updated to the timeline point when
 * the command was issued.
 *
 * :c:member:`drm_xe_eudebug_eu_control.cmd` can
 * be following:
 *
 * *DRM_XE_EUDEBUG_EU_CONTROL_CMD_INTERRUPT_ALL*
 *  will instruct hardware to stop all threads on EUs
 *  for exec_queue:lrc. This command takes no bitmask, so
 *  :c:member:`drm_xe_eudebug_eu_control.bitmask_size`
 *  must be set to zero.
 *
 * *DRM_XE_EUDEBUG_EU_CONTROL_CMD_STOPPED*
 *  returns the bitmask for threads that are
 *  in so called attention state.
 *
 * *DRM_XE_EUDEBUG_EU_CONTROL_CMD_RESUME*
 *  resumes the threads whose bit is set in the bitmask.
 *
 */
struct drm_xe_eudebug_eu_control {
	/** @cmd: Command for execution units */
#define DRM_XE_EUDEBUG_EU_CONTROL_CMD_INTERRUPT_ALL	0
#define DRM_XE_EUDEBUG_EU_CONTROL_CMD_STOPPED		1
#define DRM_XE_EUDEBUG_EU_CONTROL_CMD_RESUME		2
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
 * struct drm_xe_eudebug_event_eu_attention - EU Attention Event
 *
 * Whenever there is any thread in halted/attentions state, this
 * event will be delivered. The event will be delivered periodically
 * until there are no attentions detected.
 *
 */
struct drm_xe_eudebug_event_eu_attention {
	/** @base: base event */
	struct drm_xe_eudebug_event base;

	/** @exec_queue_handle: Exec queue handle for the attentions */
	__u64 exec_queue_handle;

	/** @lrc_handle: LRC handle for the attentions */
	__u64 lrc_handle;

	/**
	 * @flags: Reserved for future use, reads as zero
	 *
	 * Event wide flags are carried in
	 * :c:member:`drm_xe_eudebug_event_eu_attention.base`.
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
 * struct drm_xe_eudebug_event_pagefault - EU Pagefault Event
 *
 * Delivered when a thread of a debuggable exec queue takes a pagefault that
 * the debugger is expected to resolve. The EUs are halted while the fault is
 * handled, and the attention state is captured on the way in and on the way
 * out, so that the threads the fault itself stopped can be told apart from
 * the ones that were already halted.
 */
struct drm_xe_eudebug_event_pagefault {
	/** @base: base event */
	struct drm_xe_eudebug_event base;

	/** @exec_queue_handle: Exec queue handle for the faulting threads */
	__u64 exec_queue_handle;

	/** @lrc_handle: LRC handle for the faulting threads */
	__u64 lrc_handle;

	/**
	 * @flags: Reserved for future use, reads as zero
	 *
	 * Event wide flags are carried in
	 * :c:member:`drm_xe_eudebug_event_pagefault.base`.
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
	 * :c:member:`drm_xe_eudebug_event_eu_attention.bitmask`.
	 */
	__u8 bitmask[];
};

#if defined(__cplusplus)
}
#endif

#endif /* _UAPI_XE_DRM_EUDEBUG_H_ */
