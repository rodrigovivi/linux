/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef _XE_EUDEBUG_PROTO_H_
#define _XE_EUDEBUG_PROTO_H_

/*
 * Staging copy of the eudebug uAPI, not ABI: moved to
 * include/uapi/drm/xe_drm_eudebug.h by the eudebug uAPI patch.
 */

#include <uapi/drm/xe_drm.h>

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

	/** @engine_class: engine class for the exec queue */
	__u32 engine_class;

	/** @width: width of exec queue, how many lrcs (handles) it has */
	__u32 width;

	/** @lrc_handle: array of lrc handles */
	__u64 lrc_handle[];
};

/*
 * To be added to xe_drm.h when the eudebug uAPI is exposed.
 */

#define DRM_XE_EUDEBUG_CONNECT		0x10
#define DRM_IOCTL_XE_EUDEBUG_CONNECT		DRM_IOW(DRM_COMMAND_BASE + DRM_XE_EUDEBUG_CONNECT, struct drm_xe_eudebug_connect)

/**
 * struct drm_xe_eudebug_connect - Input of &DRM_IOCTL_XE_EUDEBUG_CONNECT
 *
 * This structure is used to connect to an eudebug interface of target drm file.
 *
 * On connect, the resources the target already has are first relayed to
 * the debugger as events. This is called discovery.
 */
struct drm_xe_eudebug_connect {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

	/** @fd: Debug target DRM client fd */
	__u64 fd;

	/** @flags: Flags, MBZ */
	__u64 flags;

	/** @reserved: MBZ */
	__u64 reserved;
};

#if defined(__cplusplus)
}
#endif

#endif /* _XE_EUDEBUG_PROTO_H_ */
