// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include "xe_debug_data.h"
#include "xe_debug_data_types.h"
#include "xe_eudebug_proto.h"
#include "xe_vm.h"

const char *xe_debug_data_pseudo_path_to_string(u64 pseudopath)
{
	switch (pseudopath) {
	case DRM_XE_VM_BIND_DEBUG_DATA_PSEUDO_MODULE_AREA:
		return "[module_area]";
	case DRM_XE_VM_BIND_DEBUG_DATA_PSEUDO_SBA_AREA:
		return "[sba_area]";
	case DRM_XE_VM_BIND_DEBUG_DATA_PSEUDO_SIP_AREA:
		return "[sip_area]";
	default:
		return "[unknown]";
	}
}

static const char *
debug_data_path_str(u64 flags, u64 pseudopath, const char *pathname)
{
	return (flags & DRM_XE_VM_BIND_DEBUG_DATA_FLAG_PSEUDO) ?
		xe_debug_data_pseudo_path_to_string(pseudopath) : pathname;
}

static int xe_debug_data_op_unwind_add(struct xe_vm *vm, struct xe_vma_op *vma_op)
{
	const struct xe_debug_data *op_data = vma_op->modify_debug_data.added;
	struct xe_debug_data *dd, *found = NULL;

	if (!op_data)
		return 0;

	vm_dbg(&vm->xe->drm,
	       "Reverting debug data add: addr=0x%016llx, range=0x%016llx, offset=0x%08llx, flags=0x%016llx, path=%s\n",
	       op_data->addr, op_data->range, op_data->offset, op_data->flags,
	       debug_data_path_str(op_data->flags, op_data->pseudopath, op_data->pathname));

	mutex_lock(&vm->debug_data.lock);
	list_for_each_entry(dd, &vm->debug_data.list, link) {
		if (dd->addr == op_data->addr && dd->range == op_data->range) {
			list_del(&dd->link);
			found = dd;
			break;
		}
	}
	mutex_unlock(&vm->debug_data.lock);

	if (!found) {
		/*
		 * Internal invariant: we are unwinding an add that
		 * previously succeeded, so the entry must be on the
		 * list unless somebody else removed it behind our
		 * back. There is no such "somebody else" in the
		 * current locking design.
		 */
		drm_warn(&vm->xe->drm,
			 "unwind add: entry missing addr=0x%llx range=0x%llx\n",
			 op_data->addr, op_data->range);
		return -ENOENT;
	}

	kfree(found);
	vma_op->modify_debug_data.added = NULL;

	return 0;
}

static int xe_debug_data_op_unwind_remove(struct xe_vm *vm, struct xe_vma_op *vma_op)
{
	struct xe_debug_data *dd = vma_op->modify_debug_data.removed;

	if (!dd)
		return 0;

	vm_dbg(&vm->xe->drm,
	       "Reverting debug data remove: addr=0x%016llx, range=0x%016llx, offset=0x%08llx, flags=0x%016llx, path=%s\n",
	       dd->addr, dd->range, dd->offset, dd->flags,
	       debug_data_path_str(dd->flags, dd->pseudopath, dd->pathname));

	mutex_lock(&vm->debug_data.lock);
	list_add_tail(&dd->link, &vm->debug_data.list);
	mutex_unlock(&vm->debug_data.lock);

	vma_op->modify_debug_data.removed = NULL;

	return 0;
}

int xe_debug_data_op_unwind(struct xe_vm *vm, struct xe_vma_op *vma_op)
{
	switch (vma_op->subop) {
	case XE_VMA_SUBOP_ADD_DEBUG_DATA:
		return xe_debug_data_op_unwind_add(vm, vma_op);
	case XE_VMA_SUBOP_REMOVE_DEBUG_DATA:
		return xe_debug_data_op_unwind_remove(vm, vma_op);
	default:
		drm_err(&vm->xe->drm, "Invalid debug data subop %d\n", vma_op->subop);
		return -EINVAL;
	}
}

void xe_debug_data_destroy(struct xe_vm *vm)
{
	struct xe_debug_data *dd, *tmp;

	mutex_lock(&vm->debug_data.lock);
	list_for_each_entry_safe(dd, tmp, &vm->debug_data.list, link) {
		list_del(&dd->link);
		kfree(dd);
	}
	mutex_unlock(&vm->debug_data.lock);
}
