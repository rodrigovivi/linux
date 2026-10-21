// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include "xe_debug_data.h"
#include "xe_debug_data_types.h"
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

static bool
debug_data_overlaps(const struct drm_xe_vm_bind_op_ext_debug_data *a,
		    const struct xe_debug_data *b)
{
	u64 a_end, b_end;

	/*
	 * Ranges that wrap u64 are rejected by debug_data_ext_validate()
	 * before we get here, so neither side should ever overflow. Be
	 * defensive anyway and report overlap, which makes callers reject
	 * the bind rather than act on a bogus range.
	 */
	if (check_add_overflow(a->addr, a->range, &a_end) ||
	    check_add_overflow(b->addr, b->range, &b_end))
		return true;

	return a->addr < b_end && b->addr < a_end;
}

static bool
debug_data_matches(const struct drm_xe_vm_bind_op_ext_debug_data *a,
		   const struct xe_debug_data *b)
{
	return a->addr == b->addr && a->range == b->range;
}

static bool
debug_data_is_empty(const struct drm_xe_vm_bind_op_ext_debug_data *ext)
{
	return !ext->flags && !ext->offset && !ext->reserved &&
		mem_is_zero(ext->pathname, sizeof(ext->pathname));
}

/*
 * Validate a debug data extension copied from userspace.
 *
 * This is the single source of truth for what the extension is allowed
 * to contain. It is called from the check phase and again from the
 * commit phase (on a freshly-copied snapshot) so that the kernel never
 * trusts the result of an earlier copy_from_user(): see Sashiko's TOCTOU
 * comment on v1.
 */
static int
debug_data_ext_validate(struct xe_device *xe, u32 operation,
			const struct drm_xe_vm_bind_op_ext_debug_data *ext)
{
	u64 end;

	if (XE_IOCTL_DBG(xe, operation != DRM_XE_VM_BIND_OP_ADD_DEBUG_DATA &&
			 operation != DRM_XE_VM_BIND_OP_REMOVE_DEBUG_DATA))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, ext->reserved))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, check_add_overflow(ext->addr, ext->range, &end)))
		return -EINVAL;

	/* Remove takes only addr+range; everything else must be zero. */
	if (operation == DRM_XE_VM_BIND_OP_REMOVE_DEBUG_DATA) {
		if (XE_IOCTL_DBG(xe, !debug_data_is_empty(ext)))
			return -EINVAL;
		if (XE_IOCTL_DBG(xe, !ext->range))
			return -EINVAL;
		return 0;
	}

	/* Add. */
	if (XE_IOCTL_DBG(xe, !ext->range))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, ext->flags & ~DRM_XE_VM_BIND_DEBUG_DATA_FLAG_PSEUDO))
		return -EINVAL;

	if (ext->flags & DRM_XE_VM_BIND_DEBUG_DATA_FLAG_PSEUDO) {
		if (XE_IOCTL_DBG(xe, ext->offset))
			return -EINVAL;
		if (XE_IOCTL_DBG(xe,
				 ext->pseudopath < DRM_XE_VM_BIND_DEBUG_DATA_PSEUDO_MODULE_AREA ||
				 ext->pseudopath > DRM_XE_VM_BIND_DEBUG_DATA_PSEUDO_SIP_AREA))
			return -EINVAL;
	} else {
		if (XE_IOCTL_DBG(xe, strnlen(ext->pathname, sizeof(ext->pathname)) >=
				 sizeof(ext->pathname)))
			return -EINVAL;
	}

	return 0;
}

int xe_debug_data_check_extension(struct xe_vm *vm, u32 operation, u64 extension)
{
	const void __user * const address = u64_to_user_ptr(extension);
	struct drm_xe_vm_bind_op_ext_debug_data *ext;
	int ret;

	ext = kzalloc_obj(*ext, GFP_KERNEL);
	if (!ext)
		return -ENOMEM;

	if (copy_from_user(ext, address, sizeof(*ext))) {
		ret = -EFAULT;
		goto out;
	}

	ret = debug_data_ext_validate(vm->xe, operation, ext);
	/*
	 * Intentionally no list walk here: any check we make against
	 * vm->debug_data.list outside the commit critical section can be
	 * invalidated before commit runs (concurrent ioctls). The commit
	 * phase re-validates and re-checks under the list lock; this
	 * function exists only for early rejection of syntactically
	 * malformed input.
	 */
out:
	kfree(ext);
	return ret;
}

static int xe_debug_data_add(struct xe_vm *vm, struct xe_vma_op *vma_op,
			     struct drm_xe_vm_bind_op_ext_debug_data *ext)
{
	struct xe_debug_data *dd, *iter;
	int ret = 0;

	vm_dbg(&vm->xe->drm,
	       "ADD_DEBUG_DATA: addr=0x%016llx, range=0x%016llx, offset=0x%08llx, flags=0x%016llx, path=%s\n",
	       ext->addr, ext->range, ext->offset, ext->flags,
	       debug_data_path_str(ext->flags, ext->pseudopath, ext->pathname));

	dd = kzalloc_obj(*dd, GFP_KERNEL_ACCOUNT);
	if (!dd)
		return -ENOMEM;

	dd->addr = ext->addr;
	dd->range = ext->range;
	dd->flags = ext->flags;
	dd->offset = ext->offset;

	if (ext->flags & DRM_XE_VM_BIND_DEBUG_DATA_FLAG_PSEUDO) {
		dd->pseudopath = ext->pseudopath;
	} else if (strscpy(dd->pathname, ext->pathname,
			   sizeof(dd->pathname)) == -E2BIG) {
		/* Defence in depth: validate() should have caught this. */
		ret = -ENAMETOOLONG;
		goto err_free;
	}

	/*
	 * Overlap-check-and-insert atomically under the list lock.
	 * Doing the check in a separate critical section would race
	 * with concurrent adds on the same vm.
	 */
	mutex_lock(&vm->debug_data.lock);
	list_for_each_entry(iter, &vm->debug_data.list, link) {
		if (debug_data_matches(ext, iter)) {
			ret = -EEXIST;
			goto err_unlock;
		}
		if (debug_data_overlaps(ext, iter)) {
			ret = -EINVAL;
			goto err_unlock;
		}
	}
	list_add_tail(&dd->link, &vm->debug_data.list);
	mutex_unlock(&vm->debug_data.lock);

	/*
	 * Borrowed reference for the unwind path and for the bind event.
	 * The list owns it from here, so this must not be freed through
	 * the op.
	 */
	vma_op->modify_debug_data.added = dd;

	vma_op->flags |= XE_VMA_OP_COMMITTED;

	return 0;

err_unlock:
	mutex_unlock(&vm->debug_data.lock);
err_free:
	kfree(dd);
	return ret;
}

static int xe_debug_data_remove(struct xe_vm *vm, struct xe_vma_op *vma_op,
				struct drm_xe_vm_bind_op_ext_debug_data *ext)
{
	struct xe_debug_data *dd, *found = NULL;

	vm_dbg(&vm->xe->drm,
	       "REMOVE_DEBUG_DATA: addr=0x%016llx, range=0x%016llx\n",
	       ext->addr, ext->range);

	mutex_lock(&vm->debug_data.lock);
	list_for_each_entry(dd, &vm->debug_data.list, link) {
		if (debug_data_matches(ext, dd)) {
			list_del(&dd->link);
			found = dd;
			break;
		}
	}
	mutex_unlock(&vm->debug_data.lock);

	if (!found) {
		drm_dbg(&vm->xe->drm,
			"Debug data to remove not found: addr=0x%llx range=0x%llx\n",
			ext->addr, ext->range);
		return -ENOENT;
	}

	vma_op->modify_debug_data.removed = found;
	vma_op->flags |= XE_VMA_OP_COMMITTED;

	return 0;
}

int xe_debug_data_process_extension(struct xe_vm *vm, struct drm_gpuva_ops *ops,
				    u32 operation, u64 extension)
{
	const void __user * const address = u64_to_user_ptr(extension);
	struct drm_xe_vm_bind_op_ext_debug_data *ext;
	struct xe_vma_op *vma_op;
	struct drm_gpuva_op *op;
	int ret;

	ext = kzalloc_obj(*ext, GFP_KERNEL);
	if (!ext)
		return -ENOMEM;

	if (copy_from_user(ext, address, sizeof(*ext))) {
		ret = -EFAULT;
		goto out;
	}

	/*
	 * Re-validate the freshly-copied extension. The check phase
	 * cannot be trusted: userspace may have mutated the backing
	 * memory between check and commit (TOCTOU). All decisions below
	 * are made against this kernel-local copy.
	 */
	ret = debug_data_ext_validate(vm->xe, operation, ext);
	if (ret)
		goto out;

	op = drm_gpuva_first_op(ops);
	if (XE_WARN_ON(op != drm_gpuva_last_op(ops))) {
		ret = -EINVAL;
		goto out;
	}

	vma_op = gpuva_op_to_vma_op(op);

	if (vma_op->subop == XE_VMA_SUBOP_ADD_DEBUG_DATA)
		ret = xe_debug_data_add(vm, vma_op, ext);
	else if (vma_op->subop == XE_VMA_SUBOP_REMOVE_DEBUG_DATA)
		ret = xe_debug_data_remove(vm, vma_op, ext);
	else
		ret = -EINVAL;

out:
	kfree(ext);
	return ret;
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
