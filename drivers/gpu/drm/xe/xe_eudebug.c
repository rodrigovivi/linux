// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023-2025 Intel Corporation
 */

#include <linux/anon_inodes.h>
#include <linux/cleanup.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/poll.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include <drm/drm_drv.h>
#include <drm/drm_managed.h>
#include <uapi/drm/xe_drm.h>

#include "xe_assert.h"
#include "xe_debug_data_types.h"
#include "xe_device.h"
#include "xe_eudebug.h"
#include "xe_eudebug_hw.h"
#include "xe_eudebug_types.h"
#include "xe_eudebug_vm.h"
#include "xe_exec_queue.h"
#include "xe_gt.h"
#include "xe_gt_debug.h"
#include "xe_guc_submit.h"
#include "xe_hw_engine.h"
#include "xe_macros.h"
#include "xe_oa.h"
#include "xe_pm.h"
#include "xe_sriov.h"
#include "xe_sriov_pf.h"
#include "xe_sync.h"
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

bool xe_eudebug_detached(struct xe_eudebug *d)
{
	return !READ_ONCE(d->target.xef);
}

static void xe_eudebug_free(struct kref *ref)
{
	struct xe_eudebug *d = container_of(ref, typeof(*d), ref);

	WARN_ON(work_pending(&d->discovery_work));

	xe_assert(d->xe, xe_eudebug_detached(d));

	xe_eudebug_resources_destroy(d);
	XE_WARN_ON(d->target.xef);

	kvfree(d->events.fifo_buf);
	kvfree(d->events.staging);
	kvfree(d->events.pending);
	kfree(d);
}

void xe_eudebug_put(struct xe_eudebug *d)
{
	kref_put(&d->ref, xe_eudebug_free);
}

struct xe_eudebug_ack {
	struct rb_node rb_node;
	u64 seqno;
	u64 ts_insert;
	struct xe_user_fence *ufence;
};

#define fetch_ack(x) rb_entry(x, struct xe_eudebug_ack, rb_node)

static int compare_ack(const u64 a, const u64 b)
{
	if (a < b)
		return -1;
	else if (a > b)
		return 1;

	return 0;
}

static int ack_insert_cmp(struct rb_node * const node,
			  const struct rb_node * const p)
{
	return compare_ack(fetch_ack(node)->seqno,
			   fetch_ack(p)->seqno);
}

static int ack_lookup_cmp(const void * const key,
			  const struct rb_node * const node)
{
	return compare_ack(*(const u64 *)key,
			   fetch_ack(node)->seqno);
}

static struct xe_eudebug_ack *remove_ack(struct xe_eudebug *d, u64 seqno)
{
	struct rb_root * const root = &d->acks.tree;
	struct rb_node *node;

	spin_lock(&d->acks.lock);
	node = rb_find(&seqno, root, ack_lookup_cmp);
	if (node)
		rb_erase(node, root);
	spin_unlock(&d->acks.lock);

	if (!node)
		return NULL;

	return rb_entry_safe(node, struct xe_eudebug_ack, rb_node);
}

static void ufence_signal_worker(struct work_struct *w)
{
	struct xe_user_fence * const ufence =
		container_of(w, struct xe_user_fence, eudebug.worker);

	xe_sync_ufence_signal(ufence);

	xe_sync_ufence_put(ufence);
}

static void kick_ufence_worker(struct xe_user_fence *f)
{
	queue_work(f->xe->eudebug.ufence_wq, &f->eudebug.worker);
}

static void handle_ack(struct xe_eudebug *d, struct xe_eudebug_ack *ack,
		       bool on_disconnect)
{
	struct xe_user_fence *f = ack->ufence;
	struct xe_eudebug *debugger = NULL;
	u64 signalled_by;
	bool signal = false;

	spin_lock(&f->eudebug.lock);
	if (!f->eudebug.signalled_seqno) {
		f->eudebug.signalled_seqno = ack->seqno;
		f->eudebug.bind_ref_seqno = 0;
		debugger = f->eudebug.debugger;
		f->eudebug.debugger = NULL;
		signal = true;
	}
	signalled_by = f->eudebug.signalled_seqno;
	spin_unlock(&f->eudebug.lock);

	if (signal)
		kick_ufence_worker(f);
	else
		xe_sync_ufence_put(f);

	eu_dbg(d, "ACK: seqno=%llu: signalled by %llu (%s) (held %lluus)",
	       ack->seqno, signalled_by,
	       on_disconnect ? "disconnect" : "debugger",
	       ktime_us_delta(ktime_get(), ack->ts_insert));

	kfree(ack);

	if (debugger)
		xe_eudebug_put(debugger);
}

static void release_acks(struct xe_eudebug *d)
{
	struct xe_eudebug_ack *ack, *n;
	struct rb_root root;

	spin_lock(&d->acks.lock);
	root = d->acks.tree;
	d->acks.tree = RB_ROOT;
	spin_unlock(&d->acks.lock);

	rbtree_postorder_for_each_entry_safe(ack, n, &root, rb_node)
		handle_ack(d, ack, true);
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
		scoped_guard(spinlock, &target->eudebug.lock)
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

	release_acks(d);

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
__xe_eudebug_get(struct xe_file *xef)
{
	struct xe_device *xe = xef->xe;
	struct xe_eudebug *d;

	if (READ_ONCE(xe->eudebug.cap_state) == XE_EUDEBUG_CAP_NOT_SUPPORTED)
		return NULL;

	spin_lock(&xef->eudebug.lock);
	d = xef->eudebug.debugger;
	if (d && !kref_get_unless_zero(&d->ref))
		d = NULL;
	spin_unlock(&xef->eudebug.lock);

	if (d && xe_eudebug_detached(d)) {
		xe_eudebug_put(d);
		d = NULL;
	}

	return d;
}

static struct xe_eudebug *
_xe_eudebug_get(struct xe_file *xef)
{
	struct xe_eudebug *d;

	d = __xe_eudebug_get(xef);

	if (d && !completion_done(&d->discovery)) {
		xe_eudebug_put(d);
		return NULL;
	}

	return d;
}

static struct xe_eudebug *
xe_eudebug_get(struct xe_file *xef)
{
	lockdep_assert_held(&xef->eudebug.ioctl_lock);

	return _xe_eudebug_get(xef);
}

struct xe_eudebug *
xe_eudebug_get_nolock(struct xe_file *xef)
{
	return _xe_eudebug_get(xef);
}

/* Returns eudebug instance even if discovery is running */
struct xe_eudebug *
xe_eudebug_get_nolock_with_discovery(struct xe_file *xef)
{
	return __xe_eudebug_get(xef);
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

static int find_handle(struct xe_eudebug *d,
		       const int type,
		       const void *p)
{
	const u64 key = (uintptr_t)p;
	struct xe_eudebug_resource *r;
	struct xe_eudebug_handle *h;
	int id;

	if (XE_WARN_ON(!key))
		return -EINVAL;

	r = resource_from_type(d, type);

	mutex_lock(&r->lock);
	h = __find_handle(r, key);
	id = h ? h->id : -ENOENT;
	mutex_unlock(&r->lock);

	return id;
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

	XE_WARN_ON(!completion_done(&d->discovery));

	ret = _xe_eudebug_remove_handle(d, type, p, seqno);

	eu_dbg(d, "handle type %d handle %p removed: %d\n", type, p, ret);

	return ret;
}

static void *find_resource__unlocked(struct xe_eudebug_resource *r,
				     u32 id)
{
	struct xe_eudebug_handle *h;

	h = xa_load(&r->xa, id);

	return h ? (void *)(uintptr_t)h->key : NULL;
}

struct xe_vm *xe_eudebug_vm_get(struct xe_eudebug *d, u32 id)
{
	struct xe_eudebug_resource *r =
		resource_from_type(d, XE_EUDEBUG_RES_TYPE_VM);
	struct xe_vm *vm;

	mutex_lock(&r->lock);
	vm = find_resource__unlocked(r, id);
	if (vm)
		xe_vm_get(vm);
	mutex_unlock(&r->lock);

	return vm;
}

struct xe_exec_queue *xe_eudebug_exec_queue_get(struct xe_eudebug *d, u32 id)
{
	struct xe_eudebug_resource *r =
		resource_from_type(d, XE_EUDEBUG_RES_TYPE_EXEC_QUEUE);
	struct xe_exec_queue *eq;

	mutex_lock(&r->lock);
	eq = find_resource__unlocked(r, id);
	if (eq)
		xe_exec_queue_get(eq);
	mutex_unlock(&r->lock);

	return eq;
}

struct xe_lrc *xe_eudebug_find_lrc(struct xe_eudebug *d, u32 id)
{
	struct xe_eudebug_resource *r = resource_from_type(d, XE_EUDEBUG_RES_TYPE_LRC);
	struct xe_lrc *lrc;

	mutex_lock(&r->lock);
	lrc = find_resource__unlocked(r, id);
	mutex_unlock(&r->lock);

	return lrc;
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

static const u16 xe_to_user_engine_class[] = {
	[XE_ENGINE_CLASS_RENDER] = DRM_XE_ENGINE_CLASS_RENDER,
	[XE_ENGINE_CLASS_COPY] = DRM_XE_ENGINE_CLASS_COPY,
	[XE_ENGINE_CLASS_VIDEO_DECODE] = DRM_XE_ENGINE_CLASS_VIDEO_DECODE,
	[XE_ENGINE_CLASS_VIDEO_ENHANCE] = DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE,
	[XE_ENGINE_CLASS_COMPUTE] = DRM_XE_ENGINE_CLASS_COMPUTE,
};

static int send_exec_queue_event(struct xe_eudebug *d, u32 flags,
				 u64 vm_handle, u64 exec_queue_handle,
				 enum xe_engine_class class,
				 u32 width, u64 *lrc_handles, u64 seqno)
{
	struct drm_xe_eudebug_event *event;
	struct drm_xe_eudebug_event_exec_queue *e;
	const u32 sz = struct_size(e, lrc_handle, width);
	u32 xe_engine_class;
	int err;

	if (!xe_engine_supports_eudebug(class))
		return -EINVAL;

	xe_engine_class = xe_to_user_engine_class[class];

	spin_lock(&d->events.lock);
	event = xe_eudebug_prepare_event(d, DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE,
					 seqno, flags, sz);
	e = cast_event(e, event);

	e->vm_handle = vm_handle;
	e->exec_queue_handle = exec_queue_handle;
	e->engine_class = xe_engine_class;
	e->width = width;

	memcpy(e->lrc_handle, lrc_handles, width * sizeof(u64));

	err = xe_eudebug_queue_event(d, event);
	spin_unlock(&d->events.lock);

	if (err)
		xe_eudebug_disconnect(d, err);

	return err;
}

/*
 * A debuggable queue needs only an LR vm, and a fault mode vm can be
 * destroyed while its queues live on: xe_vm_destroy_ioctl() only refuses
 * a vm with preempt fence mode queues. The debugger has then already seen
 * the vm go, so report such a queue against vm handle 0 rather than
 * treating the missing handle as a diverged resource map.
 */
static int exec_queue_vm_handle(struct xe_eudebug *d, struct xe_exec_queue *q)
{
	int h_vm = find_handle(d, XE_EUDEBUG_RES_TYPE_VM, q->vm);

	return h_vm == -ENOENT ? 0 : h_vm;
}

static int exec_queue_create_event(struct xe_eudebug *d,
				   struct xe_file *xef, struct xe_exec_queue *q)
{
	int h_vm, h_queue;
	u64 h_lrc[XE_HW_ENGINE_MAX_INSTANCE], seqno;
	int i;
	int ret;

	if (!xe_exec_queue_is_debuggable(q))
		return 0;

	h_vm = exec_queue_vm_handle(d, q);
	if (h_vm < 0)
		return h_vm;

	if (XE_WARN_ON(q->width > XE_HW_ENGINE_MAX_INSTANCE))
		return -EINVAL;

	ret = 0;
	for (i = 0; i < q->width; i++) {
		ret = _xe_eudebug_add_handle(d,
					     XE_EUDEBUG_RES_TYPE_LRC,
					     q->lrc[i],
					     NULL);

		if (ret < 0)
			break;

		XE_WARN_ON(!ret);

		h_lrc[i] = ret;
	}

	if (ret < 0)
		return ret;

	h_queue = xe_eudebug_add_handle(d, XE_EUDEBUG_RES_TYPE_EXEC_QUEUE, q, &seqno);
	if (h_queue < 0)
		return h_queue;

	/* No need to cleanup for added handles on error as if we fail
	 * we disconnect
	 */

	ret = send_exec_queue_event(d, DRM_XE_EUDEBUG_EVENT_CREATE,
				    h_vm, h_queue, q->class,
				    q->width, h_lrc, seqno);

	if (ret)
		eu_dbg(d, "send_exec_queue_event create error %d", ret);

	return ret;
}

static int exec_queue_destroy_event(struct xe_eudebug *d,
				    struct xe_file *xef,
				    struct xe_exec_queue *q)
{
	int h_vm, h_queue;
	u64 h_lrc[XE_HW_ENGINE_MAX_INSTANCE], seqno;
	int i;
	int ret, err = 0;

	if (!xe_exec_queue_is_debuggable(q))
		return 0;

	/*
	 * A queue we never handed out to the debugger needs no destroy event.
	 * The queue handle is added last on create, so its presence implies
	 * the lrc handles are there too. Past this point a missing handle
	 * means our resource map and the client have diverged, and that has
	 * to drop the session.
	 */
	if (find_handle(d, XE_EUDEBUG_RES_TYPE_EXEC_QUEUE, q) == -ENOENT)
		return 0;

	h_vm = exec_queue_vm_handle(d, q);
	if (h_vm < 0)
		return h_vm;

	if (XE_WARN_ON(q->width > XE_HW_ENGINE_MAX_INSTANCE))
		return -EINVAL;

	for (i = 0; i < q->width; i++) {
		ret = _xe_eudebug_remove_handle(d,
						XE_EUDEBUG_RES_TYPE_LRC,
						q->lrc[i],
						NULL);
		if (ret < 0) {
			XE_WARN_ON(ret);
			if (!err)
				err = ret;
			h_lrc[i] = 0;
			continue;
		}

		XE_WARN_ON(!ret);
		h_lrc[i] = ret;
	}

	if (err)
		return err;

	h_queue = xe_eudebug_remove_handle(d,
					   XE_EUDEBUG_RES_TYPE_EXEC_QUEUE,
					   q,
					   &seqno);
	if (h_queue < 0)
		return h_queue;

	ret = send_exec_queue_event(d, DRM_XE_EUDEBUG_EVENT_DESTROY,
				    h_vm, h_queue, q->class,
				    q->width, h_lrc, seqno);

	if (ret)
		eu_dbg(d, "send_exec_queue_event destroy error %d\n", ret);

	return ret;
}

void xe_eudebug_exec_queue_create(struct xe_file *xef, struct xe_exec_queue *q)
{
	struct xe_eudebug *d;
	int err;

	if (!xe_engine_supports_eudebug(q->class))
		return;

	d = xe_eudebug_get(xef);
	if (!d)
		return;

	err = exec_queue_create_event(d, xef, q);
	if (err)
		xe_eudebug_disconnect(d, err);

	xe_eudebug_put(d);
}

void xe_eudebug_exec_queue_destroy(struct xe_file *xef, struct xe_exec_queue *q)
{
	struct xe_eudebug *d;
	int err;

	if (!xe_engine_supports_eudebug(q->class))
		return;

	d = xe_eudebug_get(xef);
	if (!d)
		return;

	err = exec_queue_destroy_event(d, xef, q);
	if (err)
		xe_eudebug_disconnect(d, err);

	xe_eudebug_put(d);
}

static int send_vm_bind_event(struct xe_eudebug *d,
			      struct xe_vm *vm,
			      u64 vm_handle,
			      u32 bind_flags,
			      u32 num_ops, u64 *seqno)
{
	struct drm_xe_eudebug_event_vm_bind *e;
	struct drm_xe_eudebug_event *event;
	const u32 sz = sizeof(*e);
	const u32 base_flags = DRM_XE_EUDEBUG_EVENT_STATE_CHANGE;
	int err;

	/* Stamped at generation, see @events.seqno in xe_eudebug_types.h */
	*seqno = atomic64_inc_return(&d->events.seqno);

	spin_lock(&d->events.lock);
	event = xe_eudebug_prepare_event(d, DRM_XE_EUDEBUG_EVENT_VM_BIND,
					 *seqno, base_flags, sz);
	e = cast_event(e, event);

	e->vm_handle = vm_handle;
	e->flags = bind_flags;
	e->num_bind_ops = num_ops;

	err = xe_eudebug_queue_event(d, event);
	spin_unlock(&d->events.lock);

	if (err)
		xe_eudebug_disconnect(d, err);

	return err;
}

static int vm_bind_event(struct xe_eudebug *d,
			 struct xe_vm *vm,
			 u32 flags,
			 u32 num_ops,
			 u64 *seqno)
{
	int h_vm;

	h_vm = find_handle(d, XE_EUDEBUG_RES_TYPE_VM, vm);
	if (h_vm < 0)
		return h_vm;

	return send_vm_bind_event(d, vm, h_vm, flags,
				  num_ops, seqno);
}

static int vm_bind_op_event(struct xe_eudebug *d,
			    struct xe_vm *vm,
			    const u32 flags,
			    const u64 bind_ref_seqno,
			    const struct xe_debug_data *debug_data,
			    u64 *op_seqno)
{
	struct drm_xe_eudebug_event_vm_bind_op_debug_data *e;
	struct drm_xe_eudebug_event *event;
	const bool pseudo = debug_data->flags & DRM_XE_VM_BIND_DEBUG_DATA_FLAG_PSEUDO;
	/* NUL included, so that the debugger can read the tail as a string */
	const size_t path_sz = pseudo ? 0 : strlen(debug_data->pathname) + 1;
	const u32 sz = round_up(struct_size(e, pathname, path_sz), 8);
	int err;

	/* A path of the size the bind extension accepts has to fit an event */
	BUILD_BUG_ON(sizeof(*e) + sizeof(debug_data->pathname) >
		     DRM_XE_EUDEBUG_EVENT_MAX_SIZE);

	*op_seqno = atomic64_inc_return(&d->events.seqno);

	spin_lock(&d->events.lock);
	event = xe_eudebug_prepare_event(d, DRM_XE_EUDEBUG_EVENT_VM_BIND_OP_DEBUG_DATA,
					 *op_seqno, flags, sz);
	e = cast_event(e, event);

	e->vm_bind_ref_seqno = bind_ref_seqno;
	e->addr = debug_data->addr;
	e->range = debug_data->range;
	e->flags = debug_data->flags;
	e->offset = debug_data->offset;

	/*
	 * The event is sized to the path, and xe_eudebug_prepare_event() has
	 * zeroed it, so the terminator and any alignment padding are there
	 * without writing them.
	 */
	if (pseudo)
		e->pseudopath = debug_data->pseudopath;
	else
		memcpy(e->pathname, debug_data->pathname, path_sz - 1);

	err = xe_eudebug_queue_event(d, event);
	spin_unlock(&d->events.lock);

	if (err)
		xe_eudebug_disconnect(d, err);

	return err;
}

static int vm_bind_op(struct xe_eudebug *d, struct xe_vm *vm,
		      const u32 flags, const u64 bind_ref_seqno,
		      const struct xe_debug_data *debug_data)
{
	u64 op_seqno;

	return vm_bind_op_event(d, vm, flags, bind_ref_seqno,
				debug_data, &op_seqno);
}

void xe_eudebug_ufence_init(struct xe_user_fence *ufence)
{
	spin_lock_init(&ufence->eudebug.lock);
	ufence->eudebug.debugger = NULL;
	ufence->eudebug.bind_ref_seqno = 0;
	ufence->eudebug.signalled_seqno = 0;
	INIT_WORK(&ufence->eudebug.worker, ufence_signal_worker);
}

void xe_eudebug_ufence_fini(struct xe_user_fence *ufence)
{
	struct xe_eudebug *d;

	/*
	 * A fence can die still holding its bind reference. A bind of debug
	 * data ops only has no page table work, and its fences are signalled
	 * by xe_vm_bind_ioctl() after xe_eudebug_vm_bind_execute() attached
	 * this one. If that signalling fails, for instance on an in-syncobj,
	 * which the ufence path rejects, the fence is never signalled and
	 * nothing will ever track or ack it. There is no event to send for a
	 * fence that never signals, so just drop the reference.
	 */
	WRITE_ONCE(ufence->eudebug.bind_ref_seqno, 0);

	d = READ_ONCE(ufence->eudebug.debugger);
	if (!d)
		return;

	/* last ref */
	ufence->eudebug.debugger = NULL;

	xe_eudebug_put(d);
}

static int xe_eudebug_track_ufence(struct xe_eudebug *d,
				   struct xe_user_fence *f,
				   u64 seqno)
{
	struct xe_eudebug_ack *ack;
	struct rb_node *old;

	/*
	 * A debugger that reads events but never acks them keeps these
	 * around indefinitely, so charge them to whoever is doing it rather
	 * than to the kernel at large. Matches the event fifo allocations.
	 */
	ack = kzalloc_obj(*ack, GFP_KERNEL | __GFP_ACCOUNT);
	if (!ack) {
		eu_dbg(d, "ack allocation fail");
		return -ENOMEM;
	}

	ack->seqno = seqno;
	ack->ts_insert = ktime_get();

	__xe_sync_ufence_get(f);

	scoped_guard(spinlock, &d->acks.lock) {
		old = rb_find_add(&ack->rb_node,
				  &d->acks.tree, ack_insert_cmp);
		if (!old)
			ack->ufence = f;
	}

	if (!old)
		return 0;

	xe_sync_ufence_put(f);
	kfree(ack);

	return -EEXIST;
}

static int track_ufence(struct xe_eudebug *d,
			struct xe_user_fence *ufence,
			bool *tracked)
{
	struct drm_xe_eudebug_event *event;
	struct drm_xe_eudebug_event_vm_bind_ufence *e;
	const u32 sz = sizeof(*e);
	const u32 flags = DRM_XE_EUDEBUG_EVENT_CREATE |
		DRM_XE_EUDEBUG_EVENT_NEED_ACK;
	u64 seqno;
	int ret;

	*tracked = false;

	if (XE_WARN_ON(!ufence->eudebug.bind_ref_seqno))
		return -EINVAL;

	seqno = atomic64_inc_return(&d->events.seqno);

	ret = xe_eudebug_track_ufence(d, ufence, seqno);
	if (ret)
		return ret;

	*tracked = true;

	spin_lock(&d->events.lock);
	event = xe_eudebug_prepare_event(d, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
					 seqno, flags, sz);

	e = cast_event(e, event);
	e->vm_bind_ref_seqno = ufence->eudebug.bind_ref_seqno;

	ret = xe_eudebug_queue_event(d, event);
	spin_unlock(&d->events.lock);

	return ret;
}

/*
 * Signalling without an ack: the fence will not be tracked, so it must not
 * keep the debugger either. Drop the reference taken in
 * xe_eudebug_vm_bind_execute() now, as handle_ack() does for a tracked
 * fence, instead of leaving it to the fence destructor, which can be as
 * late as the vma the fence is attached to going away.
 */
static void ufence_drop_debugger(struct xe_user_fence *ufence)
{
	struct xe_eudebug *d;

	scoped_guard(spinlock, &ufence->eudebug.lock) {
		d = ufence->eudebug.debugger;
		ufence->eudebug.debugger = NULL;
		ufence->eudebug.bind_ref_seqno = 0;
		ufence->eudebug.signalled_seqno = 0;
	}

	if (d)
		xe_eudebug_put(d);
}

/**
 * xe_eudebug_ufence_track - Track the ufence for eudebug
 * @ufence : user fence that might be applicable to tracking
 *
 * If this user fence was part of bind sequence, we need
 * to track it so that we can hold the client signalling on behalf
 * of debugger and thus deliver event to debugger.
 *
 * Return: true debugger will track, false debugger not interested
 *
 */
bool xe_eudebug_ufence_track(struct xe_user_fence *ufence)
{
	struct xe_eudebug *d;
	bool tracked = false;
	int ret;

	spin_lock(&ufence->eudebug.lock);
	d = ufence->eudebug.debugger;
	if (d && !kref_get_unless_zero(&d->ref))
		d = NULL;

	if (!d) {
		ufence->eudebug.bind_ref_seqno = 0;
		ufence->eudebug.signalled_seqno = 0;
	}
	spin_unlock(&ufence->eudebug.lock);

	if (!d)
		return false;

	if (xe_eudebug_detached(d)) {
		/* Nothing will ack this fence any more */
		ufence_drop_debugger(ufence);
		xe_eudebug_put(d);
		return false;
	}

	ret = track_ufence(d, ufence, &tracked);
	if (ret) {
		/*
		 * If we failed before the ack made it into the tree, the
		 * disconnect below has nothing to release for this fence.
		 */
		if (!tracked)
			ufence_drop_debugger(ufence);

		xe_eudebug_disconnect(d, ret);
	}

	xe_eudebug_put(d);

	return tracked;
}

/*
 * The debug data entry a bind op adds or removes, and the event flag for
 * it, or NULL if the op is not a debug data op. vm_bind_ioctl_check_args()
 * requires the extension, so a debug data op always has its entry here.
 * Counting and sending both go through this, so that num_bind_ops always
 * matches the op events that follow.
 */
static const struct xe_debug_data *bind_op_debug_data(struct xe_vma_op *op,
						      u32 *flags)
{
	if (op->base.op != DRM_GPUVA_OP_DRIVER)
		return NULL;

	if (op->subop == XE_VMA_SUBOP_ADD_DEBUG_DATA) {
		*flags = DRM_XE_EUDEBUG_EVENT_CREATE;
		return op->modify_debug_data.added;
	}

	if (op->subop == XE_VMA_SUBOP_REMOVE_DEBUG_DATA) {
		*flags = DRM_XE_EUDEBUG_EVENT_DESTROY;
		return op->modify_debug_data.removed;
	}

	return NULL;
}

void xe_eudebug_vm_bind_execute(struct xe_vm *vm,
				struct xe_vma_ops *ops)
{
	struct xe_user_fence *ufence = NULL;
	struct xe_eudebug *d;
	struct xe_vma_op *op;
	unsigned int i;
	u64 bind_seqno = 0;
	u32 num_ops;
	int err;

	if (!xe_vm_in_lr_mode(vm))
		return;

	d = xe_eudebug_get(vm->xef);
	if (!d)
		return;

	for (i = 0; i < ops->num_syncs; i++) {
		struct xe_sync_entry *se = &ops->syncs[i];

		if (xe_sync_is_ufence(se)) {
			xe_assert(vm->xe, !ufence);
			ufence = se->ufence;
		}
	}

	num_ops = 0;
	list_for_each_entry(op, &ops->list, link) {
		u32 flags;

		if (bind_op_debug_data(op, &flags))
			num_ops++;
	}

	lockdep_assert_held(&vm->lock);

	if (!num_ops) {
		xe_eudebug_put(d);
		return;
	}

	err = vm_bind_event(d, vm,
			    ufence ? DRM_XE_EUDEBUG_EVENT_VM_BIND_FLAG_UFENCE : 0,
			    num_ops, &bind_seqno);
	if (err)
		goto out_err;

	list_for_each_entry(op, &ops->list, link) {
		const struct xe_debug_data *dd;
		u32 flags;

		dd = bind_op_debug_data(op, &flags);
		if (!dd)
			continue;

		err = vm_bind_op(d, vm, flags, bind_seqno, dd);
		if (err)
			goto out_err;
	}

	if (ufence) {
		spin_lock(&ufence->eudebug.lock);
		kref_get(&d->ref);
		ufence->eudebug.debugger = d;
		ufence->eudebug.bind_ref_seqno = bind_seqno;
		spin_unlock(&ufence->eudebug.lock);
	}

out_err:
	if (err)
		xe_eudebug_disconnect(d, err);

	xe_eudebug_put(d);
}

static struct xe_file *xe_eudebug_target_get(struct xe_eudebug *d)
{
	struct xe_file *xef = NULL;

	mutex_lock(&d->xe->eudebug.lock);
	if (d->target.xef)
		xef = xe_file_get(d->target.xef);
	mutex_unlock(&d->xe->eudebug.lock);

	return xef;
}

static int vm_discover_binds(struct xe_eudebug *d, struct xe_vm *vm)
{
	struct xe_debug_data *dd;
	struct list_head *pos;
	unsigned int ops, count;
	u64 ref_seqno;
	int err;

	/*
	 * Only long running vms are announced to the debugger, so only those
	 * have a handle to reference here. Nothing stops a client adding
	 * debug data to a vm that is not long running, and without this the
	 * bind event below would fail to resolve the vm and take down the
	 * whole discovery. Matches xe_eudebug_vm_bind_execute().
	 */
	if (!xe_vm_in_lr_mode(vm))
		return 0;

	/*
	 * Walked without vm->debug_data.lock. The caller holds the target's
	 * eudebug.ioctl_lock for write, and everything that adds to or
	 * removes from this list runs under DRM_XE_VM_BIND, which takes that
	 * same rwsem for read, so the list cannot change under us here.
	 */
	if (list_empty(&vm->debug_data.list))
		return 0;

	count = 0;
	list_for_each(pos, &vm->debug_data.list)
		count++;

	ops = count;
	ref_seqno = 0;
	err = vm_bind_event(d, vm, 0, ops, &ref_seqno);
	if (err) {
		eu_dbg(d, "vm_bind_event error %d\n", err);
		return err;
	}

	list_for_each_entry(dd, &vm->debug_data.list, link) {
		err = vm_bind_op(d, vm, DRM_XE_EUDEBUG_EVENT_CREATE, ref_seqno, dd);
		if (err) {
			eu_dbg(d, "vm_bind_op error %d\n", err);
			return err;
		}

		ops--;
	}

	XE_WARN_ON(ops);

	return ops ? -EIO : count;
}

static void discover_client(struct xe_eudebug *d)
{
	struct xe_file *xef;
	struct xe_exec_queue *q;
	struct xe_vm *vm;
	unsigned long i;
	unsigned int vm_count = 0, eq_count = 0, ops_count = 0;
	int err = 0;

	xef = xe_eudebug_target_get(d);
	if (!xef) {
		complete_all(&d->discovery);
		return;
	}

	/*
	 * An xe_eudebug ref is taken for the discovery worker, which holds a
	 * ref to the target xe_file; the xe_file in turn holds the vm and
	 * exec_queue refs.
	 *
	 * All the relevant ioctls that manipulate the xe_file resources eudebug
	 * cares about are gated behind down_read(&xef->eudebug.ioctl_lock).
	 * Thus taking the write lock excludes all those ioctls, which lets us
	 * peek into the resource lists without taking their individual locks.
	 */
	down_write(&xef->eudebug.ioctl_lock);

	if (xe_eudebug_detached(d))
		goto out;

	eu_dbg(d, "Discovery start for %lld", d->session);

	/* vm lifetime guaranteed by the ioctl_lock */
	xa_for_each(&xef->vm.xa, i, vm) {
		err = vm_create_event(d, vm);
		if (err)
			goto out;
		vm_count++;

		err = vm_discover_binds(d, vm);
		if (err < 0)
			goto out;

		ops_count += err;
	}

	xa_for_each(&xef->exec_queue.xa, i, q) {
		if (!xe_engine_supports_eudebug(q->class))
			continue;

		err = exec_queue_create_event(d, xef, q);
		if (err)
			goto out;

		eq_count++;
	}

out:
	complete_all(&d->discovery);

	up_write(&xef->eudebug.ioctl_lock);

	if (vm_count || eq_count || ops_count)
		eu_dbg(d, "Discovery found %u vms, %u exec_queues, %u bind_ops",
		       vm_count, eq_count, ops_count);

	xe_file_put(xef);

	eu_dbg(d, "Discovery end for %lld: %d", d->session, err);

	if (err < 0)
		xe_eudebug_disconnect(d, err);
}

static void discovery_work_fn(struct work_struct *work)
{
	struct xe_eudebug *d = container_of(work, typeof(*d),
					    discovery_work);

	if (xe_eudebug_detached(d))
		complete_all(&d->discovery);
	else
		discover_client(d);

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
	scoped_guard(spinlock, &xef->eudebug.lock)
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

static long
xe_eudebug_ack_event_ioctl(struct xe_eudebug *d,
			   const u64 arg)
{
	struct drm_xe_eudebug_ack __user * const user_ptr =
		u64_to_user_ptr(arg);
	struct drm_xe_eudebug_ack user_arg;
	struct xe_eudebug_ack *ack;
	struct xe_device *xe = d->xe;

	if (XE_IOCTL_DBG(xe, copy_from_user(&user_arg,
					    user_ptr,
					    sizeof(user_arg))))
		return -EFAULT;

	if (XE_IOCTL_DBG(xe, user_arg.type))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, user_arg.flags))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, user_arg.reserved))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, xe_eudebug_detached(d)))
		return -ENOTCONN;

	ack = remove_ack(d, user_arg.seqno);
	if (XE_IOCTL_DBG(xe, !ack))
		return -EINVAL;

	handle_ack(d, ack, false);

	return 0;
}

/*
 * Ioctls that act on the target's resources wait for discovery to have
 * relayed the existing ones first. The debugger retries on -EBUSY.
 */
static long discovery_pending(struct xe_eudebug *d)
{
	return completion_done(&d->discovery) ? 0 : -EBUSY;
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
 * unknown command, -EBUSY while discovery is still in progress.
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
	case DRM_XE_EUDEBUG_IOCTL_ACK_EVENT:
		ret = discovery_pending(d) ?: xe_eudebug_ack_event_ioctl(d, arg);
		eu_dbg(d, "ioctl cmd=EVENT_ACK ret=%ld\n", ret);
		break;
	case DRM_XE_EUDEBUG_IOCTL_VM_OPEN:
		ret = discovery_pending(d) ?: xe_eudebug_vm_open_ioctl(d, arg);
		eu_dbg(d, "ioctl cmd=VM_OPEN ret=%ld\n", ret);
		break;
	case DRM_XE_EUDEBUG_IOCTL_EU_CONTROL:
		ret = discovery_pending(d) ?: xe_eudebug_eu_control(d, arg);
		eu_dbg(d, "ioctl cmd=EU_CONTROL ret=%ld\n", ret);
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

/*
 * The uapi tells userspace it may scan the attention bitmask as u32 or u64
 * words. Both hold today because the preceding members are u64, but a field
 * added ahead of the array could quietly break it, so pin it down here.
 */
static_assert(__alignof__(struct drm_xe_eudebug_event_eu_attention) >=
	      sizeof(__u64));
static_assert(offsetof(struct drm_xe_eudebug_event_eu_attention, bitmask) %
	      sizeof(__u64) == 0);

static int send_attention_event(struct xe_eudebug *d, struct xe_exec_queue *q,
				int lrc_idx, void *bitmap, unsigned int size)
{
	struct drm_xe_eudebug_event_eu_attention *e;
	struct drm_xe_eudebug_event *event;
	const u32 sz = struct_size(e, bitmask, size);
	int h_queue, h_lrc;
	int ret;

	if (lrc_idx < 0 || lrc_idx >= q->width)
		return -EINVAL;

	h_queue = find_handle(d, XE_EUDEBUG_RES_TYPE_EXEC_QUEUE, q);
	if (h_queue < 0)
		return h_queue;

	h_lrc = find_handle(d, XE_EUDEBUG_RES_TYPE_LRC, q->lrc[lrc_idx]);
	if (h_lrc < 0)
		return h_lrc;

	spin_lock(&d->events.lock);
	event = xe_eudebug_prepare_event(d, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION, 0,
					 DRM_XE_EUDEBUG_EVENT_STATE_CHANGE, sz);

	event->seqno = atomic64_inc_return(&d->events.seqno);
	e = cast_event(e, event);
	e->exec_queue_handle = h_queue;
	e->lrc_handle = h_lrc;
	e->bitmask_size = size;

	memcpy(e->bitmask, bitmap, size);
	ret = xe_eudebug_queue_event(d, event);
	spin_unlock(&d->events.lock);

	return ret;
}

static int xe_send_gt_attention(struct xe_gt *gt, void *bitmap, unsigned int size)
{
	struct xe_eudebug *d;
	struct xe_exec_queue *q;
	int ret, lrc_idx;

	q = xe_gt_runalone_active_queue_get(gt, NULL, &lrc_idx);
	if (IS_ERR(q))
		return PTR_ERR(q);

	if (!xe_exec_queue_is_debuggable(q)) {
		ret = -EPERM;
		goto err_exec_queue_put;
	}

	d = xe_eudebug_get_nolock(q->vm->xef);
	if (!d) {
		ret = -ENOTCONN;
		goto err_exec_queue_put;
	}

	if (!completion_done(&d->discovery)) {
		eu_dbg(d, "discovery not yet done\n");
		ret = -EBUSY;
		goto err_eudebug_put;
	}

	ret = send_attention_event(d, q, lrc_idx, bitmap, size);
	if (ret)
		xe_eudebug_disconnect(d, ret);

err_eudebug_put:
	xe_eudebug_put(d);
err_exec_queue_put:
	xe_exec_queue_put(q);

	return ret;
}

static int xe_eudebug_handle_gt_attention(struct xe_gt *gt)
{
	struct xe_device *xe = gt_to_xe(gt);
	const u32 size = xe_gt_eu_attention_bitmap_size(gt);
	int ret;
	void *bitmask;

	if (!READ_ONCE(xe->eudebug.send_attentions))
		return 0;

	ret = xe_gt_eu_threads_needing_attention(gt);
	if (ret <= 0)
		return ret;

	/* If we fail at this time, assume we manage to send eventually */
	bitmask = kvzalloc(size, GFP_KERNEL);
	if (!bitmask)
		return 0;

	ret = xe_gt_eu_attention_bitmap(gt, bitmask, size);
	if (ret)
		goto out;

	if (mem_is_zero(bitmask, size))
		goto out;

	ret = xe_send_gt_attention(gt, bitmask, size);
out:
	kvfree(bitmask);

	/* Discovery in progress, fake it */
	if (ret == -EBUSY)
		return 0;

	return ret;
}

static void handle_attention_fail(struct xe_gt *gt, int gt_id, int ret)
{
	/*
	 * The owner of the attention cannot be resolved, but that covers two
	 * different things. The context may have switched out between reading
	 * the bits and looking it up, for instance after a debugger resumed
	 * its threads, and then there is nothing left to service. Or the
	 * threads are still waiting, in a workload that is not in debug mode,
	 * and nobody is ever going to answer them.
	 *
	 * The error tells the two apart no better than the bits do: a
	 * workload not in debug mode is not runalone either, so it fails the
	 * same lookup. Read the attention again instead. If it is gone, the
	 * scan raced something that serviced it; if it is still there, it is
	 * stuck and the gt has to be reset to get the engine back.
	 */
	if (!xe_gt_eu_threads_needing_attention(gt))
		return;

	/* TODO: error capture */
	drm_err_ratelimited(&gt_to_xe(gt)->drm,
			    "gt:%d unable to handle eu attention ret = %d\n",
			    gt_id, ret);

	xe_gt_reset_async(gt);
}

/*
 * A gt reset is in flight from the moment xe_gt_reset_async() marks GuC
 * submission stopped until the restart at the end of gt_reset_worker()
 * starts it again. The attention poll takes forcewake and does steered
 * reads on every dss, none of which may race a reset of any gt on the
 * device, so it has to stay out for that whole window.
 */
static bool attention_poll_reset_in_flight(struct xe_device *xe)
{
	struct xe_gt *gt;
	u8 id;

	if (xe_device_is_in_reset(xe))
		return true;

	for_each_gt(gt, xe, id) {
		if (xe_guc_read_stopped(&gt->uc.guc))
			return true;

		if (work_busy(&gt->reset.worker))
			return true;
	}

	return false;
}

static void attention_poll_work(struct work_struct *work)
{
	struct xe_device *xe = container_of(work, typeof(*xe),
					    eudebug.attention_dwork.work);
	const unsigned int poll_interval_ms = 100;
	long delay = msecs_to_jiffies(poll_interval_ms);
	struct xe_gt *gt;
	u8 gt_id;

	if (!READ_ONCE(xe->eudebug.send_attentions))
		return;

	/* Skip this round and look again at the poll interval */
	if (attention_poll_reset_in_flight(xe))
		goto requeue;

	if (xe_pm_runtime_get_if_active(xe)) {
		for_each_gt(gt, xe, gt_id) {
			int ret;

			if (gt->info.type != XE_GT_TYPE_MAIN)
				continue;

			ret = xe_eudebug_handle_gt_attention(gt);
			if (ret)
				handle_attention_fail(gt, gt_id, ret);
		}

		xe_pm_runtime_put(xe);
	}

	/*
	 * Poll fast while the device carries any debuggable exec queue,
	 * whether or not a debugger is attached to its client. Attention has
	 * to be noticed with low latency to be worth anything, and for an
	 * offline capture there is no debugger to key off at all.
	 *
	 * Poll fast for an attached debugger too, even with no debuggable
	 * queue: attention from a workload that is not in debug mode has to
	 * be spotted and the gt reset, and a debugger is the one waiting for
	 * that to happen.
	 *
	 * Non critical if we get it wrong, just longer delay on race.
	 */
	if (data_race(list_empty(&xe->eudebug.targets)) &&
	    !data_race(xe->eudebug.attached))
		delay = 5 * HZ;

	if (delay >= HZ)
		delay = round_jiffies_up_relative(delay);

requeue:
	if (READ_ONCE(xe->eudebug.send_attentions))
		queue_delayed_work(xe->eudebug.attention_wq,
				   &xe->eudebug.attention_dwork, delay);
}

/*
 * The cancel is deliberately the async one. This runs under
 * xe->eudebug.lock and attention_poll_work() reaches that same lock
 * through xe_gt_runalone_active_queue_get(), so waiting for the worker
 * here would deadlock.
 *
 * Clearing send_attentions first is therefore what actually stops the
 * poll: a worker already past its own requeue check can arm the work one
 * more time, and that wakeup returns at the top guard without rearming.
 */
static void xe_eudebug_attention_poll_stop(struct xe_device *xe)
{
	WRITE_ONCE(xe->eudebug.send_attentions, false);
	cancel_delayed_work(&xe->eudebug.attention_dwork);
}

static void xe_eudebug_attention_poll_start(struct xe_device *xe)
{
	WRITE_ONCE(xe->eudebug.send_attentions, true);
	mod_delayed_work(xe->eudebug.attention_wq, &xe->eudebug.attention_dwork, 0);
}

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

	init_completion(&d->discovery);
	INIT_WORK(&d->discovery_work, discovery_work_fn);

	spin_lock_init(&d->acks.lock);
	d->acks.tree = RB_ROOT;

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

	xe_eudebug_hw_init(d);

	kref_get(&d->ref); /* for discovery */
	queue_work(xe->eudebug.discovery_wq, &d->discovery_work);

	/*
	 * add_debugger() dropped eudebug.lock, and the target closing its
	 * fd since then detaches us and lets a disable run to completion,
	 * lockdown release included. Starting the poll regardless would
	 * leave it reading gt registers with eudebug off and VFs allowed.
	 */
	scoped_guard(mutex, &xe->eudebug.lock) {
		if (xe_eudebug_is_enabled(xe))
			xe_eudebug_attention_poll_start(xe);
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

	down_write(&xef->eudebug.ioctl_lock);

	d = __xe_eudebug_get(xef);
	if (d) {
		xe_eudebug_detach(d, 0);
		xe_eudebug_put(d);
	}

	up_write(&xef->eudebug.ioctl_lock);
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

static int __xe_eudebug_enable(struct xe_device *xe, bool enable,
			       bool *end_lockdown)
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

	if (enable) {
		xe_eudebug_attention_poll_start(xe);
	} else {
		xe_eudebug_attention_poll_stop(xe);
		/*
		 * The lockdown must outlive the attention worker: it reads GT
		 * mmio, and VFs must not be enabled while it does. Stopping it
		 * for good needs a synchronous cancel, which cannot happen
		 * under eudebug.lock because the worker takes that same lock.
		 * Hand the release to the caller, which runs after we drop it.
		 */
		*end_lockdown = IS_SRIOV_PF(xe);
	}

	return 0;
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
	bool end_lockdown = false;
	int ret;

	ret = __xe_eudebug_enable(xe, enable, &end_lockdown);
	if (ret || !end_lockdown)
		return ret;

	/*
	 * eudebug.lock is dropped by now, so the worker can finish and this
	 * cannot deadlock against it. Only once it is truly stopped is it
	 * safe to let VFs be enabled again.
	 */
	cancel_delayed_work_sync(&xe->eudebug.attention_dwork);

	/*
	 * An enable may have got in between dropping eudebug.lock and the
	 * cancel, and the cancel then took out the poll it queued. That
	 * enable holds a lockdown of its own, so only the poll needs putting
	 * back.
	 */
	scoped_guard(mutex, &xe->eudebug.lock) {
		if (xe_eudebug_is_enabled(xe))
			xe_eudebug_attention_poll_start(xe);
	}

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

/*
 * xe_eudebug_fini() only runs at the final drm_dev_put(), which an open
 * file, or a debugger connection, can hold off long past unbind. devm
 * teardown unmaps the registers the poll reads well before that, and the
 * device is left runtime active, so stop the poll at unbind. devm actions
 * unwind in reverse, so this runs once nothing can enable eudebug and
 * restart it.
 */
static void xe_eudebug_attention_fini(void *arg)
{
	struct xe_device *xe = arg;

	xe_eudebug_attention_poll_stop(xe);
	cancel_delayed_work_sync(&xe->eudebug.attention_dwork);
}

static void xe_eudebug_fini(struct drm_device *dev, void *__unused)
{
	struct xe_device *xe = to_xe_device(dev);

	xe_assert(xe, !xe->eudebug.attached);
	xe_assert(xe, list_empty(&xe->eudebug.targets));

	xe_eudebug_attention_poll_stop(xe);
	cancel_delayed_work_sync(&xe->eudebug.attention_dwork);
}

/**
 * xe_eudebug_init_early - Initialize EU debug mode state
 * @xe: the &xe_device
 */
void xe_eudebug_init_early(struct xe_device *xe)
{
	struct drm_device *dev = &xe->drm;
	int err;

	INIT_LIST_HEAD(&xe->eudebug.targets);
	INIT_DELAYED_WORK(&xe->eudebug.attention_dwork, attention_poll_work);

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
	struct drm_device *dev = &xe->drm;
	struct workqueue_struct *wq;
	int err;

	/* early init failed */
	if (xe->eudebug.cap_state == XE_EUDEBUG_CAP_NOT_SUPPORTED)
		return;

	/*
	 * Deliberately not ordered. discover_client() takes the target's
	 * eudebug.ioctl_lock for write, and that lock is held across whole
	 * ioctls - including the copy_from_user() inside them, which
	 * userspace can stall for as long as it likes by pointing the
	 * argument at a userfaultfd or FUSE mapping. On an ordered queue
	 * (max_active = 1) one such client would block discovery for every
	 * other debugger connection on the device; here it only delays its
	 * own.
	 */
	wq = alloc_workqueue("xe-eudebug-discovery", WQ_UNBOUND, 0);
	if (!wq) {
		err = -ENOMEM;
		goto out_err;
	}

	err = drmm_add_action_or_reset(dev, __drmm_workqueue_release, wq);
	if (err)
		goto out_err;
	xe->eudebug.discovery_wq = wq;

	wq = alloc_workqueue("xe-eudebug-ufence", WQ_UNBOUND, 0);
	if (!wq) {
		err = -ENOMEM;
		goto out_err;
	}

	err = drmm_add_action_or_reset(dev, __drmm_workqueue_release, wq);
	if (err)
		goto out_err;
	xe->eudebug.ufence_wq = wq;

	wq = drmm_alloc_ordered_workqueue(dev, "xe-eudebug-attn", 0);
	if (IS_ERR(wq)) {
		err = PTR_ERR(wq);
		goto out_err;
	}
	xe->eudebug.attention_wq = wq;

	err = drmm_add_action_or_reset(&xe->drm, xe_eudebug_fini, NULL);
	if (err)
		goto out_err;

	err = devm_add_action_or_reset(dev->dev, xe_eudebug_attention_fini, xe);
	if (err)
		goto out_err;

	/* Last: once the file exists it can enable us */
	debugfs_create_file("eudebug", 0600, xe->drm.primary->debugfs_root,
			    xe, &eudebug_fops);

	return;

out_err:
	drm_warn(&xe->drm, "eudebug disabled, init fail: %d\n", err);

	WRITE_ONCE(xe->eudebug.cap_state, XE_EUDEBUG_CAP_NOT_SUPPORTED);
}

/**
 * xe_eudebug_track_exec_queue - Make an exec queue findable on attention
 * @q: The exec queue
 *
 * Adds a debuggable exec queue into the device wide target list, so that
 * the attention handling can resolve the hardware context that raised
 * attention back to the exec queue owning it. Membership does not depend
 * on a debugger being attached to the owning client.
 *
 * Must be called only after the queue is fully initialized, as the lookup
 * dereferences the lrc array of every queue it walks.
 */
void xe_eudebug_track_exec_queue(struct xe_exec_queue *q)
{
	struct xe_device *xe = gt_to_xe(q->gt);

	if (!xe_exec_queue_is_debuggable(q))
		return;

	guard(mutex)(&xe->eudebug.lock);

	if (list_empty(&q->eudebug.target_link))
		list_add_tail(&q->eudebug.target_link, &xe->eudebug.targets);

	/*
	 * Attention polling falls back to a lazy interval while the device
	 * carries nothing debuggable. A queue that arrives during such a
	 * sleep would have to wait it out before any attention of its own
	 * is noticed, which is long enough for the job timeout to kill the
	 * workload first. Wake the poll instead.
	 *
	 * Waking it for every queue, rather than only for the one that
	 * finds the list empty, is deliberate: attention_poll_work() reads
	 * the list without the lock and then picks its delay, so a queue
	 * added between those two points can find a non-empty list and
	 * still face a worker that is about to sleep the lazy interval out.
	 * A stopped poll returns at its own guard, and queue creation is
	 * not a hot path.
	 *
	 * The worker takes eudebug.lock itself, but mod_delayed_work() only
	 * queues it, so doing this under the lock cannot deadlock.
	 */
	mod_delayed_work(xe->eudebug.attention_wq,
			 &xe->eudebug.attention_dwork, 0);
}

/**
 * xe_eudebug_untrack_exec_queue - Remove an exec queue from the target list
 * @q: The exec queue
 *
 * Reverses xe_eudebug_track_exec_queue(). Safe to call on a queue that was
 * never tracked, and safe to repeat.
 */
void xe_eudebug_untrack_exec_queue(struct xe_exec_queue *q)
{
	struct xe_device *xe = gt_to_xe(q->gt);

	if (!xe_exec_queue_is_debuggable(q))
		return;

	guard(mutex)(&xe->eudebug.lock);

	list_del_init(&q->eudebug.target_link);
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
