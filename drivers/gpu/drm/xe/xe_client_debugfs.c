// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <linux/debugfs.h>
#include <linux/seq_file.h>

#include "xe_client_debugfs.h"
#include "xe_debug_data.h"
#include "xe_debug_data_types.h"
#include "xe_device.h"
#include "xe_device_types.h"
#include "xe_vm_types.h"

static void debug_data_show_vm(struct seq_file *m, unsigned long vm_id,
			       struct xe_vm *vm)
{
	struct xe_debug_data *dd;

	guard(mutex)(&vm->debug_data.lock);

	list_for_each_entry(dd, &vm->debug_data.list, link) {
		/* Format: <vm id> <begin>-<end> <flags> <offset>\t<pathname> */
		seq_printf(m, "%lu 0x%llx-0x%llx 0x%llx 0x%llx\t",
			   vm_id,
			   dd->addr,
			   dd->addr + dd->range,
			   dd->flags,
			   dd->offset);

		if (dd->flags & DRM_XE_VM_BIND_DEBUG_DATA_FLAG_PSEUDO)
			seq_puts(m, xe_debug_data_pseudo_path_to_string(dd->pseudopath));
		else
			/*
			 * The pathname originates from userspace; escape it the
			 * same way seq_path() does for /proc/pid/maps so that a
			 * newline cannot break the line oriented format.
			 */
			seq_escape(m, dd->pathname, "\n");

		seq_putc(m, '\n');
	}
}

static int debug_data_show(struct seq_file *m, void *data)
{
	struct xe_file *xef = m->private;
	unsigned long vm_id;
	struct xe_vm *vm;

	guard(mutex)(&xef->vm.lock);

	xa_for_each(&xef->vm.xa, vm_id, vm)
		debug_data_show_vm(m, vm_id, vm);

	return 0;
}

static int debug_data_open(struct inode *inode, struct file *file)
{
	struct xe_file *xef = inode->i_private;
	int ret;

	ret = single_open(file, debug_data_show, xef);
	if (ret)
		return ret;

	xe_file_get(xef);

	return 0;
}

static int debug_data_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;
	struct xe_file *xef = m->private;
	int ret;

	ret = single_release(inode, file);
	xe_file_put(xef);

	return ret;
}

static const struct file_operations debug_data_fops = {
	.owner = THIS_MODULE,
	.open = debug_data_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = debug_data_release,
};

/**
 * xe_client_debugfs_register() - Register the per-client debugfs files
 * @xef: The xe file to register the files for
 *
 * Create the debugfs entries exposed under the DRM client directory of @xef.
 * Called from xe_file_open(), after DRM has created the client directory.
 *
 * The files are removed by drm_file_free() before ->postclose runs, so they
 * do not need to be torn down by the driver.
 */
void xe_client_debugfs_register(struct xe_file *xef)
{
	debugfs_create_file("debug_data", 0444, xef->drm->debugfs_client, xef,
			    &debug_data_fops);
}
