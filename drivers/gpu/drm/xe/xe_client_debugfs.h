/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef _XE_CLIENT_DEBUGFS_H_
#define _XE_CLIENT_DEBUGFS_H_

struct xe_file;

#ifdef CONFIG_DEBUG_FS
void xe_client_debugfs_register(struct xe_file *xef);
#else
static inline void xe_client_debugfs_register(struct xe_file *xef) { }
#endif

#endif
