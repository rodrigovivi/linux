/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_BO_EVICT_H_
#define _XE_BO_EVICT_H_

struct xe_device;

int xe_bo_evict_all(struct xe_device *xe);
int xe_bo_restore_all(struct xe_device *xe);

#endif /* _XE_BO_EVICT_H_ */
