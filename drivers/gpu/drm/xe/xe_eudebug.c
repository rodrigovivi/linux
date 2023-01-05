// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023-2025 Intel Corporation
 */

#include <linux/anon_inodes.h>
#include <linux/cleanup.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include <drm/drm_drv.h>
#include <drm/drm_managed.h>
#include <uapi/drm/xe_drm.h>

#include "xe_assert.h"
#include "xe_device.h"
#include "xe_eudebug.h"
#include "xe_eudebug_hw.h"
#include "xe_eudebug_types.h"
#include "xe_gt.h"
#include "xe_macros.h"
#include "xe_oa.h"
#include "xe_pm.h"
#include "xe_sriov.h"
#include "xe_sriov_pf.h"
#include "xe_vm.h"

/**
 * DOC: EU debug mode
 *
 * In EU debug mode the render and compute engines run with the thread
 * debug controls in TD_CTL enabled, together with the workarounds that
 * debugging needs. A breakpoint or an exception then halts the EU thread
 * that hit it and raises attention, instead of being ignored.
 *
 * The mode is off by default and is toggled per device. The new state is
 * applied through a GT reset. While no debugger interface is merged, the
 * only way to toggle it is the eudebug file in the device debugfs
 * directory.
 */

#define cast_event(T, event) container_of((event), typeof(*(T)), base)

static const struct rhashtable_params rhash_res = {
	.head_offset = offsetof(struct xe_eudebug_handle, rh_head),
	.key_len = sizeof_field(struct xe_eudebug_handle, key),
	.key_offset = offsetof(struct xe_eudebug_handle, key),
	.automatic_shrinking = true,
};

static struct xe_eudebug_resource *
resource_from_type(struct xe_eudebug *d, int t)
{
	return &d->target.res[t];
}

static int
xe_eudebug_resources_init(struct xe_eudebug *d)
{
	int ret;
	int i;

	ret = 0;
	for (i = 0; i < XE_EUDEBUG_RES_TYPE_COUNT; i++) {
		struct xe_eudebug_resource *r = resource_from_type(d, i);

		mutex_init(&r->lock);
		xa_init_flags(&r->xa, XA_FLAGS_ALLOC1);
		ret = rhashtable_init(&r->rh, &rhash_res);

		if (ret) {
			xa_destroy(&r->xa);
			mutex_destroy(&r->lock);
			break;
		}
	}

	if (!ret)
		return 0;

	while (i--) {
		struct xe_eudebug_resource *r = resource_from_type(d, i);

		xa_destroy(&r->xa);
		rhashtable_destroy(&r->rh);
		mutex_destroy(&r->lock);
	}

	return ret;
}

/*
 * Drop every handle, keeping the maps themselves usable.
 *
 * The maps hold raw pointers to the target's resources, and the reference
 * that keeps those alive belongs to the target, not to us. Once the target
 * is detached it is free to destroy them, so the handles have to go at the
 * same time or a later lookup resolves one and takes a reference to freed
 * memory. Lookups take r->lock too, so one that gets in first still sees a
 * live resource.
 */
static void
xe_eudebug_resources_clear(struct xe_eudebug *d)
{
	unsigned long j;
	int err;
	int i;

	for (i = 0; i < XE_EUDEBUG_RES_TYPE_COUNT; i++) {
		struct xe_eudebug_resource *r = resource_from_type(d, i);
		struct xe_eudebug_handle *h;

		mutex_lock(&r->lock);
		xa_for_each(&r->xa, j, h) {
			struct xe_eudebug_handle *t;

			err = rhashtable_remove_fast(&r->rh,
						     &h->rh_head,
						     rhash_res);
			xe_eudebug_assert(d, !err);
			t = xa_erase(&r->xa, h->id);
			if (XE_WARN_ON(!t))
				continue;

			xe_eudebug_assert(d, t == h);
			kfree(t);
		}
		mutex_unlock(&r->lock);
	}
}

static void
xe_eudebug_resources_destroy(struct xe_eudebug *d)
{
	int i;

	xe_eudebug_resources_clear(d);

	for (i = 0; i < XE_EUDEBUG_RES_TYPE_COUNT; i++) {
		struct xe_eudebug_resource *r = resource_from_type(d, i);

		rhashtable_destroy(&r->rh);
		xe_eudebug_assert(d, xa_empty(&r->xa));
		xa_destroy(&r->xa);
		mutex_destroy(&r->lock);
	}
}

static bool xe_eudebug_detached(struct xe_eudebug *d)
{
	return !READ_ONCE(d->target.xef);
}

static void xe_eudebug_free(struct kref *ref)
{
	struct xe_eudebug *d = container_of(ref, typeof(*d), ref);

	xe_assert(d->xe, xe_eudebug_detached(d));

	xe_eudebug_resources_destroy(d);
	XE_WARN_ON(d->target.xef);

	kvfree(d->events.fifo_buf);
	kvfree(d->events.staging);
	kvfree(d->events.pending);
	kfree(d);
}

static void xe_eudebug_put(struct xe_eudebug *d)
{
	kref_put(&d->ref, xe_eudebug_free);
}

static bool xe_eudebug_detach(struct xe_eudebug *d,
			      const int err)
{
	struct xe_file *target = NULL;

	XE_WARN_ON(err > 0);

	mutex_lock(&d->xe->eudebug.lock);
	if (d->target.xef) {
		target = d->target.xef;
		WRITE_ONCE(d->target.err, err);
		WRITE_ONCE(d->target.xef, NULL);

		XE_WARN_ON(target->eudebug.debugger != d);
		target->eudebug.debugger = NULL;

		d->xe->eudebug.attached--;

		eu_dbg(d, "session %lld detached with %d", d->session, err);
	}
	mutex_unlock(&d->xe->eudebug.lock);

	wake_up_all(&d->events.write_done);

	if (target) {
		xe_eudebug_put(d);
		xe_file_put(target);
	}

	/*
	 * The target may start tearing its resources down the moment it has
	 * been detached, and the fd close path does exactly that. Drop the
	 * handles before returning so nothing can be resolved afterwards.
	 */
	if (target)
		xe_eudebug_resources_clear(d);

	return !!target;
}

#define xe_eudebug_disconnect(_d, _err) ({ \
	struct xe_eudebug *__disc_d = (_d); \
	int __disc_err = (_err); \
	if (xe_eudebug_detach(__disc_d, __disc_err)) { \
		if (__disc_err == 0 || __disc_err == -ETIMEDOUT) \
			eu_dbg(__disc_d, "Session closed (%d)", __disc_err); \
		else \
			eu_err(__disc_d, "Session disconnected, err = %d (%s:%d)", \
			       __disc_err, __func__, __LINE__); \
	} \
})

static int event_fifo_pending(struct xe_eudebug *d,
			      struct drm_xe_eudebug_event **pending)
{
	struct drm_xe_eudebug_event *e = d->events.pending;
	unsigned int len, copied;

	lockdep_assert_held(&d->events.lock);

	*pending = NULL;

	if (xe_eudebug_detached(d))
		return -ENOTCONN;

	if (d->events.pending_occupied) {
		*pending = e;
		return 0;
	}

	if (kfifo_out_peek(&d->events.fifo, e, sizeof(*e)) < sizeof(*e))
		return -ENOENT;

	len = e->len;
	if (len <= sizeof(*e) || len > DRM_XE_EUDEBUG_EVENT_MAX_SIZE) {
		eu_dbg(d, "bad event len %u", len);
		return -EIO;
	}

	copied = kfifo_out(&d->events.fifo, e, len);
	if (copied != len) {
		eu_dbg(d, "fifo inconsistency");
		return -EIO;
	}

	d->events.pending_occupied = true;
	*pending = e;

	return 0;
}

static struct xe_eudebug *
xe_eudebug_get(struct xe_file *xef)
{
	struct xe_device *xe = xef->xe;
	struct xe_eudebug *d;

	if (READ_ONCE(xe->eudebug.cap_state) == XE_EUDEBUG_CAP_NOT_SUPPORTED)
		return NULL;

	mutex_lock(&xe->eudebug.lock);
	d = xef->eudebug.debugger;
	if (d && !kref_get_unless_zero(&d->ref))
		d = NULL;
	mutex_unlock(&xe->eudebug.lock);

	if (d && xe_eudebug_detached(d)) {
		xe_eudebug_put(d);
		d = NULL;
	}

	return d;
}

static int xe_eudebug_queue_event(struct xe_eudebug *d,
				  struct drm_xe_eudebug_event *event)
{
	unsigned int copied;

	lockdep_assert_held(&d->events.lock);

	xe_eudebug_assert(d, event->len > sizeof(struct drm_xe_eudebug_event));
	xe_eudebug_assert(d, event->type);
	xe_eudebug_assert(d, event->type != DRM_XE_EUDEBUG_EVENT_READ);
	xe_eudebug_assert(d, event->len <= DRM_XE_EUDEBUG_EVENT_MAX_SIZE);
	xe_eudebug_assert(d, event == d->events.staging);

	if (xe_eudebug_detached(d))
		return -ENOTCONN;

	if (kfifo_avail(&d->events.fifo) < event->len)
		return -ENOSPC;

	copied = kfifo_in(&d->events.fifo, event, event->len);
	if (XE_WARN_ON(copied != event->len))
		return -EIO;

	wake_up_all(&d->events.write_done);

	return 0;
}

static struct xe_eudebug_handle *
alloc_handle(const u64 key)
{
	struct xe_eudebug_handle *h;

	h = kzalloc_obj(*h, GFP_KERNEL);
	if (!h)
		return NULL;

	h->key = key;

	return h;
}

static struct xe_eudebug_handle *
__find_handle(struct xe_eudebug_resource *r,
	      const u64 key)
{
	struct xe_eudebug_handle *h;

	h = rhashtable_lookup_fast(&r->rh,
				   &key,
				   rhash_res);
	return h;
}

static int _xe_eudebug_add_handle(struct xe_eudebug *d,
				  int type,
				  void *p,
				  u64 *seqno)
{
	const u64 key = (uintptr_t)p;
	struct xe_eudebug_resource *r;
	struct xe_eudebug_handle *h, *o;
	int id, err;

	if (XE_WARN_ON(!p))
		return -EINVAL;

	h = alloc_handle(key);
	if (!h)
		return -ENOMEM;

	r = resource_from_type(d, type);

	/*
	 * Reserve the id, and the nodes backing it, before taking the lock.
	 * A reserved entry reads back as NULL, so nothing can observe the id
	 * until we store the handle below, and that store is guaranteed to
	 * find the slot already there and so never has to allocate.
	 */
	err = xa_alloc(&r->xa, &h->id, XA_ZERO_ENTRY, xa_limit_31b, GFP_KERNEL);
	if (err) {
		kfree(h);
		return err;
	}

	mutex_lock(&r->lock);
	o = __find_handle(r, key);
	if (o) {
		err = -EEXIST;
	} else if (xe_eudebug_detached(d)) {
		/*
		 * A detach clears the map under this lock after marking us
		 * detached, so checking here means a handle either goes in
		 * before that clear and is dropped by it, or not at all.
		 */
		err = -ENOTCONN;
	} else {
		err = rhashtable_insert_fast(&r->rh,
					     &h->rh_head,
					     rhash_res);
		if (!err) {
			xa_store(&r->xa, h->id, h, GFP_ATOMIC);
			/*
			 * Stamped at generation, not at queueing, so
			 * concurrent producers can take N and N+1 and then
			 * queue them the other way round. That is by design:
			 * the number is the debugger's timeline, not the
			 * delivery order. See drm_xe_eudebug_event.seqno.
			 */
			if (seqno)
				*seqno = atomic64_inc_return(&d->events.seqno);
		}
	}
	id = h->id;
	mutex_unlock(&r->lock);

	if (err) {
		xa_erase(&r->xa, id);
		kfree(h);
		XE_WARN_ON(err > 0);
		return err;
	}

	xe_eudebug_assert(d, id);

	return id;
}

static int xe_eudebug_add_handle(struct xe_eudebug *d,
				 int type,
				 void *p,
				 u64 *seqno)
{
	int ret;

	ret = _xe_eudebug_add_handle(d, type, p, seqno);

	eu_dbg(d, "handle type %d handle %p added: %d\n", type, p, ret);

	return ret;
}

static int _xe_eudebug_remove_handle(struct xe_eudebug *d, int type, void *p,
				     u64 *seqno)
{
	const u64 key = (uintptr_t)p;
	struct xe_eudebug_resource *r;
	struct xe_eudebug_handle *h, *xa_h;
	int ret;

	if (XE_WARN_ON(!key))
		return -EINVAL;

	r = resource_from_type(d, type);

	guard(mutex)(&r->lock);
	h = __find_handle(r, key);
	if (!h)
		return -ENOENT;

	xa_h = xa_load(&r->xa, h->id);
	if (XE_WARN_ON(!xa_h || xa_h != h))
		return -EIO;

	ret = rhashtable_remove_fast(&r->rh,
				     &h->rh_head,
				     rhash_res);
	if (XE_WARN_ON(ret))
		return -EIO;

	xa_h = xa_erase(&r->xa, h->id);
	if (XE_WARN_ON(xa_h != h))
		return -EIO;

	ret = h->id;
	if (seqno)
		*seqno = atomic64_inc_return(&d->events.seqno);

	kfree(h);

	return ret;
}

static int xe_eudebug_remove_handle(struct xe_eudebug *d, int type, void *p,
				    u64 *seqno)
{
	int ret;

	ret = _xe_eudebug_remove_handle(d, type, p, seqno);

	eu_dbg(d, "handle type %d handle %p removed: %d\n", type, p, ret);

	return ret;
}

static struct drm_xe_eudebug_event *
xe_eudebug_prepare_event(struct xe_eudebug *d, u16 type, u64 seqno, u16 flags,
			 u32 len)
{
	const u16 known_flags =
		DRM_XE_EUDEBUG_EVENT_CREATE |
		DRM_XE_EUDEBUG_EVENT_DESTROY |
		DRM_XE_EUDEBUG_EVENT_STATE_CHANGE |
		DRM_XE_EUDEBUG_EVENT_NEED_ACK;
	struct drm_xe_eudebug_event *event = d->events.staging;

	lockdep_assert_held(&d->events.lock);

	xe_eudebug_assert(d, type <= XE_EUDEBUG_MAX_EVENT_TYPE);
	xe_eudebug_assert(d, !(~known_flags & flags));
	xe_eudebug_assert(d, len > sizeof(*event));
	xe_eudebug_assert(d, len <= DRM_XE_EUDEBUG_EVENT_MAX_SIZE);

	memset(event, 0, len);

	event->len = len;
	event->type = type;
	event->flags = flags;
	event->seqno = seqno;

	return event;
}

static int send_vm_event(struct xe_eudebug *d, u32 flags,
			 const u64 vm_handle,
			 const u64 seqno)
{
	struct drm_xe_eudebug_event *event;
	struct drm_xe_eudebug_event_vm *e;
	int err;

	scoped_guard(spinlock, &d->events.lock) {
		event = xe_eudebug_prepare_event(d, DRM_XE_EUDEBUG_EVENT_VM,
						 seqno, flags, sizeof(*e));
		e = cast_event(e, event);
		e->vm_handle = vm_handle;

		err = xe_eudebug_queue_event(d, event);
	}

	if (err)
		xe_eudebug_disconnect(d, err);

	return err;
}

static int vm_create_event(struct xe_eudebug *d, struct xe_vm *vm)
{
	int vm_id;
	u64 seqno;
	int ret;

	if (!xe_vm_in_lr_mode(vm))
		return 0;

	vm_id = xe_eudebug_add_handle(d, XE_EUDEBUG_RES_TYPE_VM, vm, &seqno);
	if (vm_id < 0)
		return vm_id;

	ret = send_vm_event(d, DRM_XE_EUDEBUG_EVENT_CREATE, vm_id, seqno);
	if (ret)
		eu_dbg(d, "send_vm_event create error %d\n", ret);

	return ret;
}

static int vm_destroy_event(struct xe_eudebug *d, struct xe_vm *vm)
{
	int vm_id;
	u64 seqno;
	int ret;

	if (!xe_vm_in_lr_mode(vm))
		return 0;

	vm_id = xe_eudebug_remove_handle(d, XE_EUDEBUG_RES_TYPE_VM, vm, &seqno);
	if (vm_id < 0)
		return vm_id;

	ret = send_vm_event(d, DRM_XE_EUDEBUG_EVENT_DESTROY, vm_id, seqno);
	if (ret)
		eu_dbg(d, "send_vm_event destroy error %d\n", ret);

	return ret;
}

void xe_eudebug_vm_create(struct xe_file *xef, struct xe_vm *vm)
{
	struct xe_eudebug *d;
	int err;

	if (!xe_vm_in_lr_mode(vm))
		return;

	d = xe_eudebug_get(xef);
	if (!d)
		return;

	err = vm_create_event(d, vm);
	if (err)
		xe_eudebug_disconnect(d, err);

	xe_eudebug_put(d);
}

void xe_eudebug_vm_destroy(struct xe_file *xef, struct xe_vm *vm)
{
	struct xe_eudebug *d;
	int err;

	if (!xe_vm_in_lr_mode(vm))
		return;

	d = xe_eudebug_get(xef);
	if (!d)
		return;

	/*
	 * A vm we never handed out to the debugger needs no destroy event.
	 * Only a genuine bookkeeping inconsistency should drop the session.
	 */
	err = vm_destroy_event(d, vm);
	if (err && err != -ENOENT)
		xe_eudebug_disconnect(d, err);

	xe_eudebug_put(d);
}

static int add_debugger(struct xe_device *xe, struct xe_eudebug *d,
			struct drm_file *target)
{
	struct xe_file *xef = target->driver_priv;

	guard(mutex)(&xe->eudebug.lock);

	if (!xe_eudebug_is_enabled(xe))
		return -EOPNOTSUPP;

	if (xef->eudebug.debugger)
		return -EBUSY;

	d->target.xef = xe_file_get(xef);
	d->target.pid = xef->pid;
	kref_get(&d->ref);
	xef->eudebug.debugger = d;

	xe->eudebug.attached++;

	do {
		d->session = ++xe->eudebug.session_count;
	} while (!d->session);

	return 0;
}

static int
xe_eudebug_attach(struct xe_device *xe, struct drm_file *parent_file,
		  struct xe_eudebug *d, u64 target_fd)
{
	struct file *file __free(fput) = NULL;
	struct drm_file *drm_file;
	struct xe_file *target_xef;
	int ret;

	if (XE_IOCTL_DBG(xe, target_fd > INT_MAX))
		return -EBADF;

	file = fget(target_fd);
	if (XE_IOCTL_DBG(xe, !file))
		return -EBADF;

	drm_file = file->private_data;
	if (XE_IOCTL_DBG(xe, !drm_file))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, parent_file->filp->f_op != file->f_op))
		return -EINVAL;

	target_xef = drm_file->driver_priv;
	if (XE_IOCTL_DBG(xe, !target_xef))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, xe != target_xef->xe))
		return -EINVAL;

	ret = add_debugger(xe, d, drm_file);
	if (XE_IOCTL_DBG(xe, ret))
		return ret;

	eu_dbg(d, "session %lld attached to %s", d->session,
	       parent_file == drm_file ? "self" : "remote");

	return 0;
}

static int xe_eudebug_release(struct inode *inode, struct file *file)
{
	struct xe_eudebug *d = file->private_data;
	struct drm_device *drm = &d->xe->drm;

	xe_eudebug_disconnect(d, 0);
	xe_eudebug_put(d);

	drm_dev_put(drm);

	return 0;
}

/*
 * This is racy as we dont take the lock for read but all the
 * callsites can handle the race so we can live without lock.
 */
__no_kcsan
static unsigned int
event_fifo_len(const struct xe_eudebug * const d)
{
	return kfifo_len(&d->events.fifo);
}

static unsigned int
event_fifo_has_events(struct xe_eudebug *d)
{
	/* Allow all waiters to proceed to check their state */
	if (xe_eudebug_detached(d))
		return 1;

	if (READ_ONCE(d->events.pending_occupied))
		return 1;

	return event_fifo_len(d) >
		sizeof(struct drm_xe_eudebug_event);
}

static __poll_t xe_eudebug_poll(struct file *file, poll_table *wait)
{
	struct xe_eudebug * const d = file->private_data;
	__poll_t ret = 0;

	poll_wait(file, &d->events.write_done, wait);

	if (xe_eudebug_detached(d)) {
		ret |= EPOLLHUP;
		if (READ_ONCE(d->target.err))
			ret |= EPOLLERR;
	}

	if (event_fifo_has_events(d))
		ret |= EPOLLIN;

	return ret;
}

static void xe_eudebug_reader_clear(struct xe_eudebug *d)
{
	if (!d)
		return;

	clear_bit_unlock(XE_EUDEBUG_READER_ACTIVE, &d->flags);
}

DEFINE_FREE(reader_active, struct xe_eudebug *, xe_eudebug_reader_clear(_T))

static long xe_eudebug_read_event(struct xe_eudebug *d,
				  const u64 arg,
				  const bool wait)
{
	struct xe_device *xe = d->xe;
	struct drm_xe_eudebug_event __user * const user_orig =
		u64_to_user_ptr(arg);
	struct xe_eudebug *reader __free(reader_active) = NULL;
	struct drm_xe_eudebug_event *event_out __free(kvfree) = NULL;
	struct drm_xe_eudebug_event user_event;
	struct drm_xe_eudebug_event *pending;
	long ret = 0;
	int pending_len = 0;
	int fifo_ret;

	if (XE_IOCTL_DBG(xe, copy_from_user(&user_event, user_orig, sizeof(user_event))))
		return -EFAULT;

	if (XE_IOCTL_DBG(xe, user_event.type != DRM_XE_EUDEBUG_EVENT_READ))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, user_event.len < sizeof(*user_orig)))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, user_event.flags))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, user_event.seqno))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, user_event.reserved))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, xe_eudebug_detached(d)))
		return -ENOTCONN;

	if (test_and_set_bit_lock(XE_EUDEBUG_READER_ACTIVE, &d->flags))
		return -EBUSY;

	reader = d;

	/* XXX: define wait time in connect arguments ? */
	if (wait) {
		ret = wait_event_interruptible_timeout(d->events.write_done,
						       event_fifo_has_events(d),
						       secs_to_jiffies(5));

		if (XE_IOCTL_DBG(xe, ret < 0))
			return ret;
	}

	/*
	 * Bounce buffer for the copy out, so that the event can be released
	 * from under events.lock before faulting on the user pointer. An event
	 * larger than what the caller asked for is answered with -EMSGSIZE, so
	 * user_event.len is an upper bound for what we will ever copy.
	 */
	event_out = kvzalloc(min_t(u32, user_event.len,
				   DRM_XE_EUDEBUG_EVENT_MAX_SIZE), GFP_KERNEL);
	if (!event_out)
		return -ENOMEM;

	spin_lock(&d->events.lock);
	fifo_ret = event_fifo_pending(d, &pending);
	if (fifo_ret == 0) {
		if (user_event.len < pending->len) {
			pending_len = pending->len;
			ret = -EMSGSIZE;
		} else if (!access_ok(user_orig, pending->len)) {
			ret = -EFAULT;
		} else {
			memcpy(event_out, pending, pending->len);
			ret = 0;
		}
	} else if (fifo_ret == -ENOENT) {
		ret = wait ? -ETIMEDOUT : -EAGAIN;
	} else {
		ret = fifo_ret;		/* -ENOTCONN or -EIO */
	}
	spin_unlock(&d->events.lock);

	/* disconnect can sleep, so do it only after dropping the spinlock */
	if (fifo_ret == -EIO)
		xe_eudebug_disconnect(d, -EIO);

	if (ret == -EMSGSIZE) {
		if (XE_IOCTL_DBG(xe, put_user(pending_len, &user_orig->len)))
			ret = -EFAULT;
	}

	if (!ret && __copy_to_user(user_orig, event_out, event_out->len))
		ret = -EFAULT;

	if (!ret) {
		spin_lock(&d->events.lock);
		d->events.pending_occupied = false;
		spin_unlock(&d->events.lock);
	}
	eu_dbg(d, "event read=%ld: type=%u, flags=0x%x, seqno=%llu", ret,
	       event_out->type, event_out->flags, event_out->seqno);

	return ret;
}

/**
 * xe_eudebug_ioctl - Issue a command to eudebug interface
 *
 * @file : eudebug file (returned from connect)
 * @cmd : cmd
 * @arg : arguments depending on cmd
 *
 * Issue a eudebug command
 *
 * Return: 0 on success, negative error code on failure. -ENOTTY for an
 * unknown command.
 */
static long xe_eudebug_ioctl(struct file *file,
			     unsigned int cmd,
			     unsigned long arg)
{
	struct xe_eudebug * const d = file->private_data;
	long ret;

	switch (cmd) {
	case DRM_XE_EUDEBUG_IOCTL_READ_EVENT:
		ret = xe_eudebug_read_event(d, arg,
					    !(file->f_flags & O_NONBLOCK));
		break;
	default:
		ret = -ENOTTY;
	}

	return ret;
}

static const struct file_operations fops = {
	.owner		= THIS_MODULE,
	.release	= xe_eudebug_release,
	.poll		= xe_eudebug_poll,
	.unlocked_ioctl	= xe_eudebug_ioctl,
	.compat_ioctl   = xe_eudebug_ioctl,
};

static int
xe_eudebug_connect(struct xe_device *xe,
		   struct drm_file *drm_file,
		   struct drm_xe_eudebug_connect *param)
{
	const u64 known_open_flags = 0;
	struct xe_eudebug *d;
	struct file *file;
	int fd, err;

	if (XE_IOCTL_DBG(xe, param->extensions))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, param->flags & ~known_open_flags))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, param->reserved))
		return -EINVAL;

	if (!xe_eudebug_is_enabled(xe))
		return -EOPNOTSUPP;

	d = kzalloc_obj(*d, GFP_KERNEL | __GFP_ACCOUNT);
	if (XE_IOCTL_DBG(xe, !d))
		return -ENOMEM;

	d->xe = xe;

	kref_init(&d->ref);
	init_waitqueue_head(&d->events.write_done);

	spin_lock_init(&d->events.lock);

	err = xe_eudebug_resources_init(d);
	if (XE_IOCTL_DBG(xe, err)) {
		kfree(d);
		return err;
	}

	d->events.pending = kvzalloc(DRM_XE_EUDEBUG_EVENT_MAX_SIZE,
				     GFP_KERNEL | __GFP_ACCOUNT);
	if (!d->events.pending) {
		err = -ENOMEM;
		goto err_put;
	}

	d->events.staging = kvzalloc(DRM_XE_EUDEBUG_EVENT_MAX_SIZE,
				     GFP_KERNEL | __GFP_ACCOUNT);
	if (!d->events.staging) {
		err = -ENOMEM;
		goto err_put;
	}

	d->events.fifo_buf = kvmalloc(XE_EUDEBUG_FIFO_SIZE,
				      GFP_KERNEL | __GFP_ACCOUNT);
	if (!d->events.fifo_buf) {
		err = -ENOMEM;
		goto err_put;
	}

	err = kfifo_init(&d->events.fifo, d->events.fifo_buf, XE_EUDEBUG_FIFO_SIZE);
	if (XE_IOCTL_DBG(xe, err))
		goto err_put;

	err = xe_eudebug_attach(xe, drm_file, d, param->fd);
	if (XE_IOCTL_DBG(xe, err))
		goto err_put;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		err = fd;
		goto err_detach;
	}

	file = anon_inode_getfile("[xe_eudebug]", &fops, d, 0);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_fd;
	}

	eu_dbg(d, "connected session %lld", d->session);

	drm_dev_get(&xe->drm);

	fd_install(fd, file);

	return fd;

err_fd:
	put_unused_fd(fd);
err_detach:
	xe_eudebug_detach(d, err);
err_put:
	xe_eudebug_put(d);

	return err;
}

void xe_eudebug_file_close(struct xe_file *xef)
{
	struct xe_eudebug *d;

	d = xe_eudebug_get(xef);
	if (d) {
		xe_eudebug_detach(d, 0);
		xe_eudebug_put(d);
	}
}

bool xe_eudebug_is_supported(struct xe_device *xe)
{
	return READ_ONCE(xe->eudebug.cap_state) != XE_EUDEBUG_CAP_NOT_SUPPORTED;
}

bool xe_eudebug_is_enabled(struct xe_device *xe)
{
	return READ_ONCE(xe->eudebug.cap_state) == XE_EUDEBUG_CAP_ENABLED;
}

struct eudebug_hw_update {
	struct work_struct work;
	struct xe_gt *gt;
	bool enable;
};

static void eudebug_hw_update_work(struct work_struct *w)
{
	struct eudebug_hw_update *u = container_of(w, typeof(*u), work);
	struct xe_gt *gt = u->gt;
	int i;

	for (i = 0; i < ARRAY_SIZE(gt->hw_engines); i++) {
		if (!(gt->info.engine_mask & BIT(i)))
			continue;

		xe_eudebug_init_hw_engine(&gt->hw_engines[i], u->enable);
	}

	/*
	 * If a reset is already pending this does not queue another one,
	 * but that reset is queued behind us and applies the new state.
	 */
	xe_gt_reset_async(gt);
}

/*
 * The new state only reaches the hardware through a reset applying
 * hwe->reg_sr. Update the lists from gt->ordered_wq, which is where
 * gt_reset_worker() runs, so that no reset can be walking them meanwhile,
 * and so that a reset already in flight, which would make
 * xe_gt_reset_async() a no-op, has either finished before the update or
 * runs after it.
 */
static void eudebug_hw_update(struct xe_gt *gt, bool enable)
{
	struct eudebug_hw_update u = {
		.gt = gt,
		.enable = enable,
	};

	INIT_WORK_ONSTACK(&u.work, eudebug_hw_update_work);
	queue_work(gt->ordered_wq, &u.work);
	flush_work(&u.work);
	destroy_work_on_stack(&u.work);

	flush_work(&gt->reset.worker);
}

/**
 * xe_eudebug_enable - Enable or disable EU debug mode
 * @xe: the &xe_device
 * @enable: the new state
 *
 * EU debug mode and SR-IOV are exclusive: on a PF, enabling it locks
 * down VF enabling until it is disabled again. It is also exclusive with
 * OA streams.
 *
 * Return: 0 on success, -EPERM if the device does not support EU debug,
 * -EBUSY if an OA stream is open, or a negative error code if VFs are
 * enabled.
 */
int xe_eudebug_enable(struct xe_device *xe, bool enable)
{
	struct xe_gt *gt;
	int ret;
	u8 id;

	guard(mutex)(&xe->eudebug.lock);

	if (xe->eudebug.cap_state == XE_EUDEBUG_CAP_NOT_SUPPORTED)
		return -EPERM;

	if (!enable && xe->eudebug.attached)
		return -EBUSY;

	if (enable == xe_eudebug_is_enabled(xe))
		return 0;

	/*
	 * eudebug and OA are mutually exclusive: both own EU state, see
	 * xe_oa_stream_open_ioctl_locked(). Publish the new state before
	 * looking for streams, and take it back if one is found. An OA open
	 * racing this is then either seen here, because it took the gt_lock
	 * first and published its stream, or sees eudebug enabled under that
	 * same lock and is refused.
	 */
	if (enable) {
		WRITE_ONCE(xe->eudebug.cap_state, XE_EUDEBUG_CAP_ENABLED);
		if (xe_oa_stream_active(xe)) {
			WRITE_ONCE(xe->eudebug.cap_state, XE_EUDEBUG_CAP_DISABLED);
			drm_dbg(&xe->drm, "Can't enable eudebug: OA stream is open\n");
			return -EBUSY;
		}
	}

	if (enable && IS_SRIOV_PF(xe)) {
		ret = xe_sriov_pf_lockdown(xe);
		if (ret) {
			/* Take back what the check above published */
			WRITE_ONCE(xe->eudebug.cap_state, XE_EUDEBUG_CAP_DISABLED);
			drm_dbg(&xe->drm, "Can't enable eudebug: VFs are enabled\n");
			return ret;
		}
	}

	xe_pm_runtime_get(xe);
	for_each_gt(gt, xe, id) {
		/* No engine there to program, so no reason to reset it */
		if (xe_gt_is_media_type(gt))
			continue;

		eudebug_hw_update(gt, enable);
	}
	xe_pm_runtime_put(xe);

	WRITE_ONCE(xe->eudebug.cap_state, enable ?
		   XE_EUDEBUG_CAP_ENABLED : XE_EUDEBUG_CAP_DISABLED);

	if (!enable && IS_SRIOV_PF(xe))
		xe_sriov_pf_end_lockdown(xe);

	return 0;
}

static ssize_t eudebug_show(struct file *f, char __user *ubuf,
			    size_t size, loff_t *pos)
{
	struct xe_device *xe = file_inode(f)->i_private;
	char buf[8];
	int len;

	len = scnprintf(buf, sizeof(buf), "%u\n", xe_eudebug_is_enabled(xe));

	return simple_read_from_buffer(ubuf, size, pos, buf, len);
}

static ssize_t eudebug_set(struct file *f, const char __user *ubuf,
			   size_t size, loff_t *pos)
{
	struct xe_device *xe = file_inode(f)->i_private;
	bool enable;
	int ret;

	ret = kstrtobool_from_user(ubuf, size, &enable);
	if (ret)
		return ret;

	ret = xe_eudebug_enable(xe, enable);
	if (ret)
		return ret;

	return size;
}

static const struct file_operations eudebug_fops = {
	.owner = THIS_MODULE,
	.read = eudebug_show,
	.write = eudebug_set,
};

/**
 * xe_eudebug_init_early - Initialize EU debug mode state
 * @xe: the &xe_device
 */
void xe_eudebug_init_early(struct xe_device *xe)
{
	struct drm_device *dev = &xe->drm;
	int err;

	WRITE_ONCE(xe->eudebug.cap_state, XE_EUDEBUG_CAP_NOT_SUPPORTED);

	err = drmm_mutex_init(dev, &xe->eudebug.lock);
	if (err) {
		drm_warn(&xe->drm, "eudebug disabled, early init fail: %d\n", err);
	} else if (IS_SRIOV_VF(xe)) {
		drm_dbg(&xe->drm, "eudebug not available in SR-IOV VF mode\n");
	} else if (GRAPHICS_VER(xe) >= 35) {
		/* debug register layouts are only known up to Xe3 */
		drm_dbg(&xe->drm, "eudebug not supported on graphics version %u\n",
			GRAPHICS_VER(xe));
	} else {
		WRITE_ONCE(xe->eudebug.cap_state, XE_EUDEBUG_CAP_DISABLED);
	}
}

/**
 * xe_eudebug_init - Initialize EU debug mode
 * @xe: the &xe_device
 */
void xe_eudebug_init(struct xe_device *xe)
{
	/* early init failed */
	if (xe->eudebug.cap_state == XE_EUDEBUG_CAP_NOT_SUPPORTED)
		return;

	/* Last: once the file exists it can enable us */
	debugfs_create_file("eudebug", 0600, xe->drm.primary->debugfs_root,
			    xe, &eudebug_fops);
}

/**
 * xe_eudebug_connect_ioctl - Connect to eudebug interface
 * @dev : DRM device
 * @data : ioctl data, (struct drm_xe_eudebug_connect)
 * @file : DRM file
 *
 * Connect to the eudebug interface.
 *
 * Return: eudebug filedesriptor on success, negative error code on failure.
 *
 */
int xe_eudebug_connect_ioctl(struct drm_device *dev,
			     void *data,
			     struct drm_file *file)
{
	struct xe_device *xe = to_xe_device(dev);
	struct drm_xe_eudebug_connect * const param = data;

	return xe_eudebug_connect(xe, file, param);
}

#if IS_ENABLED(CONFIG_DRM_XE_KUNIT_TEST)
#include "tests/xe_eudebug_test.c"
#endif
