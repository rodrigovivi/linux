/* SPDX-License-Identifier: GPL-2.0 AND MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef __XE_TEST_H__
#define __XE_TEST_H__

#include <linux/types.h>

#if IS_ENABLED(CONFIG_DRM_XE_KUNIT_TEST)

#define XE_TEST_DECLARE(x) x
#define XE_TEST_ONLY(x) unlikely(x)
#define XE_TEST_EXPORT
#define xe_cur_kunit() current->kunit_test
#define xe_cur_kunit_priv() (xe_cur_kunit() ? xe_cur_kunit()->priv : NULL)

#else /* if IS_ENABLED(CONFIG_DRM_XE_KUNIT_TEST) */

#define XE_TEST_DECLARE(x)
#define XE_TEST_ONLY(x) 0
#define XE_TEST_EXPORT static
#define xe_cur_kunit() NULL
#define xe_cur_kunit_priv() NULL

#endif
#endif
