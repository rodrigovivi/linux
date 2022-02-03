/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#ifndef _XE_EXECLIST_H_
#define _XE_EXECLIST_H_

#include "xe_execlist_types.h"

struct xe_device;

#define xe_execlist_port_assert_held(port) lockdep_assert_held(&(port)->lock);

struct xe_execlist_port *xe_execlist_port_create(struct xe_device *xe,
						 struct xe_hw_engine *hwe);
void xe_execlist_port_destroy(struct xe_execlist_port *port);

struct xe_execlist *xe_execlist_create(struct xe_engine *e);
void xe_execlist_destroy(struct xe_execlist *exl);

#endif /* _XE_EXECLIST_H_ */
