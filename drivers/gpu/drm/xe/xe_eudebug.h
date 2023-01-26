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
struct xe_device;
struct xe_file;
struct xe_vm;
struct xe_exec_queue;

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

int xe_eudebug_connect_ioctl(struct drm_device *dev,
			     void *data,
			     struct drm_file *file);

void xe_eudebug_init(struct xe_device *xe);
void xe_eudebug_init_early(struct xe_device *xe);
bool xe_eudebug_is_supported(struct xe_device *xe);
bool xe_eudebug_is_enabled(struct xe_device *xe);

void xe_eudebug_file_close(struct xe_file *xef);

void xe_eudebug_vm_create(struct xe_file *xef, struct xe_vm *vm);
void xe_eudebug_vm_destroy(struct xe_file *xef, struct xe_vm *vm);
int xe_eudebug_enable(struct xe_device *xe, bool enable);

struct xe_eudebug *xe_eudebug_get_nolock(struct xe_file *xef);
struct xe_eudebug *xe_eudebug_get_nolock_with_discovery(struct xe_file *xef);
void xe_eudebug_put(struct xe_eudebug *d);

void xe_eudebug_exec_queue_create(struct xe_file *xef, struct xe_exec_queue *q);
void xe_eudebug_exec_queue_destroy(struct xe_file *xef, struct xe_exec_queue *q);

#else

static inline int xe_eudebug_connect_ioctl(struct drm_device *dev,
					   void *data,
					   struct drm_file *file) { return -EOPNOTSUPP; }

static inline void xe_eudebug_init(struct xe_device *xe) { }
static inline void xe_eudebug_init_early(struct xe_device *xe) { }
static inline bool xe_eudebug_is_enabled(struct xe_device *xe) { return false; }

static inline void xe_eudebug_file_close(struct xe_file *xef) { }

static inline void xe_eudebug_vm_create(struct xe_file *xef, struct xe_vm *vm) { }
static inline void xe_eudebug_vm_destroy(struct xe_file *xef, struct xe_vm *vm) { }

static inline void xe_eudebug_exec_queue_create(struct xe_file *xef, struct xe_exec_queue *q) { }
static inline void xe_eudebug_exec_queue_destroy(struct xe_file *xef, struct xe_exec_queue *q) { }

#endif /* CONFIG_DRM_XE_EUDEBUG */

#endif /* _XE_EUDEBUG_H_ */
