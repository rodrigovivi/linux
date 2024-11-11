/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023-2025 Intel Corporation
 */

#ifndef _XE_EUDEBUG_H_
#define _XE_EUDEBUG_H_

#include <linux/errno.h>
#include <linux/types.h>

struct drm_device;
struct drm_file;
struct xe_debug_data;
struct xe_device;
struct xe_file;
struct xe_gt;
struct xe_vm;
struct xe_exec_queue;
struct xe_vma;
struct xe_vma_ops;
struct xe_user_fence;
struct xe_eudebug;
struct xe_eudebug_pagefault;

#if IS_ENABLED(CONFIG_DRM_XE_EUDEBUG)

#define XE_EUDEBUG_DBG_STR  "eudbg: %lld:%lld:%s (%d/%d) -> (%d): "

#define __eu_print(d, func, fmt, ...) \
	do { \
		struct xe_eudebug *__pd = (d); \
		func(&__pd->xe->drm, XE_EUDEBUG_DBG_STR fmt, \
		     __pd->session, \
		     atomic64_read(&__pd->events.seqno), \
		     (!READ_ONCE(__pd->target.xef) ? "disconnected" : ""), \
		     current->pid, \
		     task_tgid_nr(current), \
		     __pd->target.pid, \
		     ##__VA_ARGS__); \
	} while (0)

#define eu_err(d, fmt, ...)   __eu_print(d, drm_err, fmt, ##__VA_ARGS__)
#define eu_warn(d, fmt, ...)  __eu_print(d, drm_warn, fmt, ##__VA_ARGS__)
#define eu_dbg(d, fmt, ...)   __eu_print(d, drm_dbg, fmt, ##__VA_ARGS__)

#define xe_eudebug_assert(d, ...) xe_assert((d)->xe, ##__VA_ARGS__)

#define xe_eudebug_for_each_hw_engine(__hwe, __gt, __id) \
	for_each_hw_engine(__hwe, __gt, __id)	       \
		for_each_if(xe_hw_engine_has_eudebug(__hwe))

bool xe_eudebug_detach(struct xe_eudebug *d, const int err);

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

int xe_eudebug_connect_ioctl(struct drm_device *dev,
			     void *data,
			     struct drm_file *file);

void xe_eudebug_init(struct xe_device *xe);
void xe_eudebug_init_early(struct xe_device *xe);
bool xe_eudebug_is_supported(struct xe_device *xe);
bool xe_eudebug_is_enabled(struct xe_device *xe);

void xe_eudebug_track_exec_queue(struct xe_exec_queue *q);
void xe_eudebug_untrack_exec_queue(struct xe_exec_queue *q);

void xe_eudebug_file_close(struct xe_file *xef);

void xe_eudebug_vm_create(struct xe_file *xef, struct xe_vm *vm);
void xe_eudebug_vm_destroy(struct xe_file *xef, struct xe_vm *vm);

bool xe_eudebug_detached(struct xe_eudebug *d);

int xe_eudebug_enable(struct xe_device *xe, bool enable);

struct xe_eudebug *xe_eudebug_get_nolock(struct xe_file *xef);
struct xe_eudebug *xe_eudebug_get_nolock_with_discovery(struct xe_file *xef);

struct xe_eudebug *xe_eudebug_instance_get(struct xe_eudebug *d);
void xe_eudebug_put(struct xe_eudebug *d);

struct xe_vm *xe_eudebug_vm_get(struct xe_eudebug *d, u32 vm_id);

void xe_eudebug_exec_queue_create(struct xe_file *xef, struct xe_exec_queue *q);
void xe_eudebug_exec_queue_destroy(struct xe_file *xef, struct xe_exec_queue *q);
struct xe_exec_queue *xe_eudebug_exec_queue_get(struct xe_eudebug *d, u32 id);
struct xe_lrc *xe_eudebug_find_lrc(struct xe_eudebug *d, u32 id);

void xe_eudebug_vm_bind_execute(struct xe_vm *vm, struct xe_vma_ops *ops);

void xe_eudebug_ufence_init(struct xe_user_fence *ufence);
void xe_eudebug_ufence_fini(struct xe_user_fence *ufence);
bool xe_eudebug_ufence_track(struct xe_user_fence *ufence);

int xe_eudebug_send_pagefault_event(struct xe_eudebug *d,
				    struct xe_eudebug_pagefault *pf);

void xe_eudebug_attention_poll_stop(struct xe_device *xe);
void xe_eudebug_attention_poll_start(struct xe_device *xe);

#else

static inline int xe_eudebug_connect_ioctl(struct drm_device *dev,
					   void *data,
					   struct drm_file *file) { return -EOPNOTSUPP; }

static inline void xe_eudebug_init(struct xe_device *xe) { }
static inline void xe_eudebug_init_early(struct xe_device *xe) { }
static inline bool xe_eudebug_is_enabled(struct xe_device *xe) { return false; }
static inline void xe_eudebug_track_exec_queue(struct xe_exec_queue *q) { }
static inline void xe_eudebug_untrack_exec_queue(struct xe_exec_queue *q) { }

static inline void xe_eudebug_file_close(struct xe_file *xef) { }

static inline void xe_eudebug_vm_create(struct xe_file *xef, struct xe_vm *vm) { }
static inline void xe_eudebug_vm_destroy(struct xe_file *xef, struct xe_vm *vm) { }

static inline void xe_eudebug_exec_queue_create(struct xe_file *xef, struct xe_exec_queue *q) { }
static inline void xe_eudebug_exec_queue_destroy(struct xe_file *xef, struct xe_exec_queue *q) { }

static inline void xe_eudebug_vm_bind_execute(struct xe_vm *vm, struct xe_vma_ops *ops) { }

static inline void xe_eudebug_ufence_init(struct xe_user_fence *ufence) { }
static inline void xe_eudebug_ufence_fini(struct xe_user_fence *ufence) { }
static inline bool xe_eudebug_ufence_track(struct xe_user_fence *ufence) { return false; }

#endif /* CONFIG_DRM_XE_EUDEBUG */

#endif /* _XE_EUDEBUG_H_ */
