/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _I915_GEM_OBJECT_H_
#define _I915_GEM_OBJECT_H_

#define i915_gem_object_is_shmem(obj) ((obj)->flags & XE_BO_CREATE_SYSTEM_BIT)

#endif
