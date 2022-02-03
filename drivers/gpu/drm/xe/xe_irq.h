/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_IRQ_H_
#define _XE_IRQ_H_

struct xe_device;

int xe_irq_install(struct xe_device *xe);
void xe_irq_uninstall(struct xe_device *xe);

#endif	/* _XE_IRQ_H_ */
