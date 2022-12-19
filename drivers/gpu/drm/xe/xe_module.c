// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include <linux/init.h>
#include <linux/module.h>

#include "xe_drv.h"
#include "xe_hw_fence.h"
#include "xe_pci.h"
#include "xe_sched_job.h"

struct init_funcs {
	int (*init)(void);
	void (*exit)(void);
};
#define MAKE_INIT_EXIT_FUNCS(name)		\
	{ .init = xe_##name##_module_init,	\
	  .exit = xe_##name##_module_exit, }
static const struct init_funcs init_funcs[] = {
	MAKE_INIT_EXIT_FUNCS(hw_fence),
	MAKE_INIT_EXIT_FUNCS(sched_job),
};

static int __init xe_init(void)
{
	int err, i;

	for (i = 0; i < ARRAY_SIZE(init_funcs); i++) {
		err = init_funcs[i].init();
		if (err) {
			while (i--)
				init_funcs[i].exit();
			return err;
		}
	}

	return xe_register_pci_driver();
}

static void __exit xe_exit(void)
{
	int i;

	xe_unregister_pci_driver();

	for (i = ARRAY_SIZE(init_funcs) - 1; i >= 0; i--)
		init_funcs[i].exit();
}

module_init(xe_init);
module_exit(xe_exit);

MODULE_AUTHOR("Intel Corporation");

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL and additional rights");
