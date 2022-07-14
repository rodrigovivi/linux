/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_PM_H_
#define _XE_PM_H_

struct xe_device;

int xe_pm_suspend(struct xe_device *xe);
int xe_pm_resume(struct xe_device *xe);

#endif
