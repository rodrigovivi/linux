/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023-2025 Intel Corporation
 */

#ifndef _XE_EUDEBUG_PAGEFAULT_H_
#define _XE_EUDEBUG_PAGEFAULT_H_

#include <linux/types.h>

struct xe_device;
struct xe_eudebug;
struct xe_gt;
struct xe_pagefault;
struct xe_eudebug_pagefault;
struct xe_vm;
struct xe_vma;
struct xe_file;

#if IS_ENABLED(CONFIG_DRM_XE_EUDEBUG)
void xe_eudebug_pagefault_fini(struct xe_eudebug *d);
int xe_eudebug_handle_pagefaults(struct xe_gt *gt);

void xe_eudebug_pagefault_gt_init(struct xe_gt *gt);
int xe_eudebug_pagefault_start(struct xe_vm *vm, struct xe_vma *vma,
			       struct xe_pagefault *pf);

struct xe_vma *xe_eudebug_create_vma(struct xe_vm *vm, struct xe_pagefault *pf);
void xe_eudebug_pagefault_end(void *private, u32 asid, int err);
/*
 * The (struct xe_pagefault *)->producer.private is a pointer which, for now,
 * stores the pointer guc.
 * EU Debug intercepts this pointer to store struct xe_eudebug_pagefault.
 * Original pointer can be obtained via eudebug function below called with
 * mentioned producer's private field.
 */
#define XE_EUDEBUG_PAGEFAULT_PRIVATE_EUDEBUG	0x1
void xe_eudebug_pagefault_set_private(struct xe_pagefault *pf,
				      struct xe_vm *vm);
void *xe_eudebug_pagefault_get_private(void *private);

void xe_eudebug_pagefault_signal(struct xe_eudebug *d);

bool xe_eudebug_pagefault_creatable(struct xe_gt *gt, struct xe_vm *vm);

void xe_eudebug_pagefault_reset(struct xe_device *xe, struct xe_gt *gt);
#else
#include <linux/errno.h>
#include <linux/err.h>

static inline void xe_eudebug_pagefault_fini(struct xe_eudebug *d)
{
}

static inline int xe_eudebug_handle_pagefaults(struct xe_gt *gt)
{
	return -EOPNOTSUPP;
}

static inline void xe_eudebug_pagefault_gt_init(struct xe_gt *gt)
{
}

static inline int
xe_eudebug_pagefault_start(struct xe_vm *vm, struct xe_vma *vma, struct xe_pagefault *pf)
{
	return -EOPNOTSUPP;
}

static inline void xe_eudebug_pagefault_end(void *private, u32 asid, int err)
{
}

static inline struct xe_vma *xe_eudebug_create_vma(struct xe_vm *vm,
						   struct xe_pagefault *pf)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline void xe_eudebug_pagefault_set_private(struct xe_pagefault *pf,
						    struct xe_vm *vm)
{
}

static inline void *xe_eudebug_pagefault_get_private(void *private)
{
	return private;
}

static inline void xe_eudebug_pagefault_signal(struct xe_eudebug *d)
{
}

static inline bool xe_eudebug_pagefault_creatable(struct xe_gt *gt,
						  struct xe_vm *vm)
{
	return false;
}

static inline void xe_eudebug_pagefault_reset(struct xe_device *xe,
					      struct xe_gt *gt)
{
}
#endif

#endif /* _XE_EUDEBUG_PAGEFAULT_H_ */
