// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023-2025 Intel Corporation
 */

#include "xe_eudebug_vm.h"

#include <linux/anon_inodes.h>
#include <linux/dma-resv.h>
#include <linux/fs.h>
#include <linux/sched/mm.h>
#include <linux/string_choices.h>
#include <linux/vmalloc.h>

#include <drm/drm_drv.h>

#include "xe_bo.h"
#include "xe_device.h"
#include "xe_eudebug.h"
#include "xe_eudebug_types.h"
#include "xe_force_wake.h"
#include "xe_gt.h"
#include "xe_mmio.h"
#include "xe_pm.h"
#include "xe_vm.h"

#include "regs/xe_gt_regs.h"
#include "regs/xe_engine_regs.h"

static int xe_eudebug_vma_access(struct xe_vma *vma,
				 const u64 offset_in_vma,
				 void *buf, const size_t len, bool write)
{
	struct xe_bo *bo;
	u64 bytes;

	lockdep_assert_held_write(&xe_vma_vm(vma)->lock);

	if (offset_in_vma >= xe_vma_size(vma))
		return -EINVAL;

	if (vma->gpuva.flags & XE_VMA_DESTROYED)
		return -EINVAL;

	bytes = min_t(u64, len, xe_vma_size(vma) - offset_in_vma);
	if (!bytes)
		return 0;

	bo = xe_bo_get(xe_vma_bo(vma));
	if (bo) {
		const u64 bo_offset = xe_vma_bo_offset(vma) + offset_in_vma;
		int ret;
		long lret;

		/*
		 * A bo that has just been moved into place is cleared, or
		 * copied, by a dma engine, and that work is published only as
		 * a DMA_RESV_USAGE_KERNEL fence on its dma_resv. Nothing below
		 * ttm_bo_access_locked() waits for it, so without this the
		 * debugger reads whatever the pages held before the clear
		 * landed. Those are recycled pages, and vram is a device wide
		 * pool, so their previous owner need not be the target being
		 * debugged.
		 *
		 * dma-resv.h states the rule plainly for this usage class:
		 * drivers always must wait for these fences before accessing
		 * the resource.
		 *
		 * Wait and access under one reservation. vm->lock keeps this
		 * vm's rebinds out, but not a cpu fault migration or an
		 * eviction, and either can install a new placement with a new
		 * kernel fence the moment the reservation is dropped.
		 */
		ret = xe_bo_lock(bo, true);
		if (ret) {
			xe_bo_put(bo);
			return ret;
		}

		lret = dma_resv_wait_timeout(bo->ttm.base.resv,
					     DMA_RESV_USAGE_KERNEL, true,
					     MAX_SCHEDULE_TIMEOUT);
		if (lret < 0)
			ret = lret;
		else if (!lret)
			ret = -EBUSY;
		else
			ret = ttm_bo_access_locked(&bo->ttm, bo_offset, buf,
						   bytes, write);

		xe_bo_unlock(bo);
		xe_bo_put(bo);

		return ret;
	} else if (xe_vma_is_userptr(vma)) {
		struct xe_userptr *userptr = &to_userptr_vma(vma)->userptr;
		struct mm_struct *mm;
		int ret;

		if (XE_WARN_ON(!userptr->eudebug.task))
			return -EINVAL;

		/*
		 * XXX FIXME: access_remote_vm() is the preferred
		 *
		 * access_remote_vm() would fit as userptr notifier has
		 * mm ref so we would not need to carry task ref at all.
		 * But access_remote_vm is not exported. access_process_vm()
		 * is exported so use it instead.
		 *
		 * Going through the task means the address space is resolved
		 * from it on every access, and the task outlives its mm: after
		 * an execve the same task carries a different address space,
		 * and after the thread exits it carries none at all even
		 * though the process and this mapping are still alive.
		 *
		 * Confirm the mm the userptr was bound against is still the
		 * one the task holds, so that a replaced address space is
		 * refused instead of handing the debugger bytes from an
		 * unrelated program. access_process_vm() resolves the mm
		 * again on its own, so this narrows that window rather than
		 * closing it. Keying the access on the mm instead, with
		 * get_user_pages_remote() on userptr->notifier.mm, would
		 * remove the task from the picture entirely.
		 */
		mm = get_task_mm(userptr->eudebug.task);
		if (!mm)
			return -ESRCH;

		ret = mm == userptr->notifier.mm ? 0 : -ESRCH;
		mmput(mm);
		if (ret)
			return ret;

		return access_process_vm(userptr->eudebug.task,
					 xe_vma_userptr(vma) + offset_in_vma,
					 buf, bytes,
					 write ? FOLL_WRITE : 0);
	}

	return -EINVAL;
}

static int xe_eudebug_vm_access(struct xe_vm *vm, const u64 offset,
				void *buf, const size_t len, bool write)
{
	struct xe_vma *vma;
	int ret;

	down_write(&vm->lock);

	if (offset >= vm->size || len > vm->size - offset) {
		ret = -EINVAL;
		goto out;
	}

	vma = xe_vm_find_overlapping_vma(vm, offset, len);
	if (vma) {
		u64 vma_local_offset;

		/*
		 * xe_vm_find_overlapping_vma() may return a VMA whose start is
		 * below @offset when the VMA's range still overlaps
		 * [offset, offset+len). Reject anything that doesn't actually
		 * cover @offset itself.
		 */
		if (offset < xe_vma_start(vma) ||
		    offset >= (xe_vma_start(vma) + xe_vma_size(vma))) {
			ret = -EINVAL;
			goto out;
		}

		/* Translate VM-space offset to VMA-local offset. */
		vma_local_offset = offset - xe_vma_start(vma);
		ret = xe_eudebug_vma_access(vma, vma_local_offset, buf, len, write);
	} else {
		ret = -EINVAL;
	}

out:
	up_write(&vm->lock);

	return ret;
}

struct vm_file {
	struct xe_eudebug *debugger;
	struct xe_vm *vm;
	u64 flags;
	u64 vm_handle;
	u32 timeout_us;
};

static ssize_t __vm_read_write(struct xe_vm *vm,
			       void *bb,
			       char __user *r_buffer,
			       const char __user *w_buffer,
			       const u64 offset,
			       size_t len,
			       const bool write)
{
	ssize_t ret;

	if (!len)
		return 0;

	if (write) {
		ret = copy_from_user(bb, w_buffer, len);
		if (ret)
			return -EFAULT;

		ret = xe_eudebug_vm_access(vm, offset, bb, len, true);
		if (ret <= 0)
			return ret;

		len = ret;
	} else {
		ret = xe_eudebug_vm_access(vm, offset, bb, len, false);
		if (ret <= 0)
			return ret;

		len = ret;

		ret = copy_to_user(r_buffer, bb, len);
		if (ret)
			return -EFAULT;
	}

	return len;
}

static ssize_t __xe_eudebug_vm_access(struct file *file,
				      char __user *r_buffer,
				      const char __user *w_buffer,
				      size_t count, loff_t *__pos)
{
	struct vm_file *vmf = file->private_data;
	struct xe_eudebug * const d = vmf->debugger;
	struct xe_device * const xe = d->xe;
	const bool write = !!w_buffer;
	struct xe_vm *vm;
	ssize_t copied = 0;
	ssize_t bytes_left;
	ssize_t ret;
	unsigned long alloc_len;
	loff_t pos = *__pos;
	void *k_buffer;

	if (XE_IOCTL_DBG(d->xe, xe_eudebug_detached(d)))
		return -ENOTCONN;

	if (XE_IOCTL_DBG(xe, write && r_buffer))
		return -EINVAL;

	vm = xe_eudebug_vm_get(d, vmf->vm_handle);
	if (XE_IOCTL_DBG(xe, !vm))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, vm != vmf->vm)) {
		eu_warn(d, "vm_access(%s): vm handle mismatch vm_handle=%llu, flags=0x%llx, pos=%llu, count=%zu\n",
			str_write_read(write),
			vmf->vm_handle, vmf->flags, pos, count);
		xe_vm_put(vm);
		return -EINVAL;
	}

	if (!count) {
		xe_vm_put(vm);
		return 0;
	}

	if (pos >= vm->size) {
		xe_vm_put(vm);
		return write ? -ENOSPC : 0;
	}

	count = min_t(u64, count, vm->size - pos);
	bytes_left = count;

	alloc_len = min_t(unsigned long, ALIGN(count, PAGE_SIZE), 64 * SZ_1M);
	do  {
		k_buffer = vmalloc(alloc_len);
		if (k_buffer)
			break;

		alloc_len >>= 1;
	} while (alloc_len >= PAGE_SIZE);

	if (XE_IOCTL_DBG(xe, !k_buffer)) {
		xe_vm_put(vm);
		return -ENOMEM;
	}

	do {
		const ssize_t len = min_t(ssize_t, bytes_left, alloc_len);

		ret = __vm_read_write(vm, k_buffer,
				      write ? NULL : r_buffer + copied,
				      write ? w_buffer + copied : NULL,
				      (u64)(pos + copied),
				      len,
				      write);
		if (ret <= 0)
			break;

		bytes_left -= ret;
		copied += ret;
	} while (bytes_left > 0);

	vfree(k_buffer);
	xe_vm_put(vm);

	if (XE_WARN_ON(copied < 0))
		copied = 0;

	*__pos += copied;

	return copied ?: ret;
}

static ssize_t xe_eudebug_vm_read(struct file *file,
				  char __user *buffer,
				  size_t count, loff_t *pos)
{
	struct vm_file *vmf = file->private_data;
	ssize_t ret;

	/*
	 * These are plain file operations, not DRM ioctls, so nothing above
	 * us is holding a runtime PM reference. The access reaches VRAM and
	 * MMIO, which is not allowed while the device is suspended.
	 */
	xe_pm_runtime_get(vmf->debugger->xe);
	ret = __xe_eudebug_vm_access(file, buffer, NULL, count, pos);
	xe_pm_runtime_put(vmf->debugger->xe);

	return ret;
}

static ssize_t xe_eudebug_vm_write(struct file *file,
				   const char __user *buffer,
				   size_t count, loff_t *pos)
{
	struct vm_file *vmf = file->private_data;
	ssize_t ret;

	xe_pm_runtime_get(vmf->debugger->xe);
	ret = __xe_eudebug_vm_access(file, NULL, buffer, count, pos);
	xe_pm_runtime_put(vmf->debugger->xe);

	return ret;
}

static int engine_rcu_flush(struct xe_eudebug *d,
			    struct xe_hw_engine *hwe,
			    u32 timeout_us)
{
	const struct xe_reg psmi_addr = RING_PSMI_CTL(hwe->mmio_base);
	struct xe_gt *gt = hwe->gt;
	unsigned int fw_ref;
	u32 mask = RCU_ASYNC_FLUSH_AND_INVALIDATE_ALL;
	u32 psmi_ctrl;
	u32 id;
	int ret;

	if (hwe->class == XE_ENGINE_CLASS_RENDER)
		id = 0;
	else if (hwe->class == XE_ENGINE_CLASS_COMPUTE)
		id = hwe->instance + 1;
	else
		return -EINVAL;

	if (id < 8)
		mask |= id << RCU_ASYNC_FLUSH_ENGINE_ID_SHIFT;
	else
		mask |= (id - 8) << RCU_ASYNC_FLUSH_ENGINE_ID_SHIFT |
			RCU_ASYNC_FLUSH_ENGINE_ID_DECODE1;

	fw_ref = xe_force_wake_get(gt_to_fw(gt), hwe->domain);
	if (!fw_ref)
		return -ETIMEDOUT;

	/* Prevent concurrent flushes */
	mutex_lock(&d->xe->eudebug.lock);
	psmi_ctrl = xe_mmio_read32(&gt->mmio, psmi_addr);
	if (!(psmi_ctrl & IDLE_MSG_DISABLE))
		xe_mmio_write32(&gt->mmio, psmi_addr,
				REG_MASKED_FIELD_ENABLE(IDLE_MSG_DISABLE));

	/* XXX: Timeout is per operation but in here we flush previous */
	ret = xe_mmio_wait32(&gt->mmio, RCU_ASYNC_FLUSH,
			     RCU_ASYNC_FLUSH_IN_PROGRESS, 0,
			     timeout_us, NULL, false);
	if (ret)
		goto out;

	xe_mmio_write32(&gt->mmio, RCU_ASYNC_FLUSH, mask);

	ret = xe_mmio_wait32(&gt->mmio, RCU_ASYNC_FLUSH,
			     RCU_ASYNC_FLUSH_IN_PROGRESS, 0,
			     timeout_us, NULL, false);
out:
	if (!(psmi_ctrl & IDLE_MSG_DISABLE))
		xe_mmio_write32(&gt->mmio, psmi_addr,
				REG_MASKED_FIELD_DISABLE(IDLE_MSG_DISABLE));

	mutex_unlock(&d->xe->eudebug.lock);
	xe_force_wake_put(gt_to_fw(gt), fw_ref);

	return ret;
}

static int xe_eudebug_vm_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct vm_file *vmf = file->private_data;
	struct xe_eudebug *d = vmf->debugger;
	struct xe_gt *gt;
	int gt_id;
	int ret = -EINVAL;

	eu_dbg(d, "vm_fsync: vm_handle=%llu, flags=0x%llx, start=%llu, end=%llu datasync=%d\n",
	       vmf->vm_handle, vmf->flags, start, end, datasync);

	if (XE_IOCTL_DBG(d->xe, xe_eudebug_detached(d)))
		return -ENOTCONN;

	/* Not an ioctl, so take our own runtime PM ref before touching MMIO */
	xe_pm_runtime_get(d->xe);

	for_each_gt(gt, d->xe, gt_id) {
		struct xe_hw_engine *hwe;
		enum xe_hw_engine_id id;

		/* XXX: vm open per engine? */
		xe_eudebug_for_each_hw_engine(hwe, gt, id) {
			ret = engine_rcu_flush(d, hwe, vmf->timeout_us);
			if (ret)
				goto out;
		}
	}
out:
	xe_pm_runtime_put(d->xe);

	return ret;
}

static int xe_eudebug_vm_release(struct inode *inode, struct file *file)
{
	struct vm_file *vmf = file->private_data;
	struct xe_eudebug *d = vmf->debugger;

	eu_dbg(d, "vm_release: vm_handle=%llu, flags=0x%llx",
	       vmf->vm_handle, vmf->flags);

	xe_vm_put(vmf->vm);
	drm_dev_put(&d->xe->drm);
	xe_eudebug_put(d);

	kfree(vmf);

	return 0;
}

/* Ceiling for fsync, see xe_eudebug_vm_open_ioctl() */
#define XE_EUDEBUG_VM_SYNC_MAX_TIMEOUT_NS (10ull * NSEC_PER_SEC)

static const struct file_operations vm_fops = {
	.owner   = THIS_MODULE,
	.llseek  = generic_file_llseek,
	.read    = xe_eudebug_vm_read,
	.write   = xe_eudebug_vm_write,
	.fsync   = xe_eudebug_vm_fsync,
	.mmap    = NULL,
	.release = xe_eudebug_vm_release,
};

long xe_eudebug_vm_open_ioctl(struct xe_eudebug *d, unsigned long arg)
{
	struct drm_xe_eudebug_vm_open param;
	struct xe_device * const xe = d->xe;
	struct vm_file *vmf = NULL;
	struct xe_vm *vm;
	struct file *file;
	long ret = 0;
	int fd;

	BUILD_BUG_ON(_IOC_SIZE(DRM_XE_EUDEBUG_IOCTL_VM_OPEN) != sizeof(param));
	BUILD_BUG_ON(!(_IOC_DIR(DRM_XE_EUDEBUG_IOCTL_VM_OPEN) & _IOC_WRITE));

	if (XE_IOCTL_DBG(xe, copy_from_user(&param, (void __user *)arg, sizeof(param))))
		return -EFAULT;

	if (XE_IOCTL_DBG(xe, param.flags))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, param.extensions))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, param.vm_handle > U32_MAX))
		return -EINVAL;

	/*
	 * The flush waits on mmio while holding eudebug.lock, so an
	 * unbounded timeout here is a device wide stall for everyone else.
	 * The hardware flush is a microsecond scale operation; the cap is
	 * generous rather than tight.
	 */
	if (XE_IOCTL_DBG(xe, param.timeout_ns > XE_EUDEBUG_VM_SYNC_MAX_TIMEOUT_NS))
		return -EINVAL;

	/*
	 * The map keys vms by pointer without holding a reference. That is
	 * safe because a handle always goes before its vm can: a destroy
	 * removes it ahead of the final put, and a closing client detaches,
	 * which clears the map, before it tears its vms down.
	 * xe_eudebug_vm_get() takes its reference under the map's own lock,
	 * so it cannot return a vm on its way out.
	 *
	 * eudebug.lock only orders the detached check against a detach, so
	 * that a debugger detached meanwhile gets -ENOTCONN, not -EINVAL.
	 */
	scoped_guard(mutex, &xe->eudebug.lock) {
		if (XE_IOCTL_DBG(xe, xe_eudebug_detached(d)))
			return -ENOTCONN;

		vm = xe_eudebug_vm_get(d, param.vm_handle);
	}

	if (XE_IOCTL_DBG(xe, !vm))
		return -EINVAL;

	vmf = kzalloc_obj(*vmf, GFP_KERNEL);
	if (XE_IOCTL_DBG(xe, !vmf)) {
		ret = -ENOMEM;
		goto out_vm_put;
	}

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (XE_IOCTL_DBG(xe, fd < 0)) {
		ret = fd;
		goto out_free;
	}

	kref_get(&d->ref);
	vmf->debugger = d;
	vmf->vm = vm;
	vmf->flags = param.flags;
	vmf->vm_handle = param.vm_handle;
	/*
	 * 0 would make the flush sample its status once and give up, which
	 * is not what leaving the timeout unset reads as, so it selects the
	 * maximum. Round up, so that a short timeout does not truncate to 0.
	 */
	vmf->timeout_us = div64_u64(param.timeout_ns ?: XE_EUDEBUG_VM_SYNC_MAX_TIMEOUT_NS,
				    NSEC_PER_USEC);
	if (param.timeout_ns % NSEC_PER_USEC)
		vmf->timeout_us++;

	file = anon_inode_getfile("[xe_eudebug.vm]", &vm_fops, vmf, O_RDWR);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		XE_IOCTL_DBG(xe, ret);
		file = NULL;
		goto out_fd_put;
	}

	eu_dbg(d, "vm_open: handle=%llu, flags=0x%llx, fd=%d",
	       vmf->vm_handle, vmf->flags, fd);

	drm_dev_get(&xe->drm);

	file->f_mode |= FMODE_PREAD | FMODE_PWRITE |
		FMODE_READ | FMODE_WRITE | FMODE_LSEEK;

	fd_install(fd, file);

	return fd;

out_fd_put:
	put_unused_fd(fd);
	xe_eudebug_put(d);
out_free:
	kfree(vmf);
out_vm_put:
	xe_vm_put(vm);

	return ret;
}
