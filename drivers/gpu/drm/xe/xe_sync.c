#include "xe_sync.h"

#include <linux/uaccess.h>
#include <drm/xe_drm.h>
#include <drm/drm_print.h>
#include <drm/drm_syncobj.h>

#include "xe_device_types.h"
#include "xe_sched_job_types.h"
#include "xe_macros.h"

#define SYNC_FLAGS_TYPE_MASK 0x3

int xe_sync_entry_parse(struct xe_device *xe, struct xe_file *xef,
			struct xe_sync_entry *sync,
			struct drm_xe_sync __user *sync_user)
{
	struct drm_xe_sync sync_in;
	int err;

	if (copy_from_user(&sync_in, sync_user, sizeof(*sync_user)))
		return -EFAULT;

	if (XE_IOCTL_ERR(xe, sync_in.flags & ~(SYNC_FLAGS_TYPE_MASK | DRM_XE_SYNC_SIGNAL)))
		return -EINVAL;

	switch (sync_in.flags & SYNC_FLAGS_TYPE_MASK) {
	case DRM_XE_SYNC_SYNCOBJ:
		sync->syncobj = drm_syncobj_find(xef->drm, sync_in.handle);
		if (XE_IOCTL_ERR(xe, !sync->syncobj))
			return -ENOENT;

		if (!(sync_in.flags & DRM_XE_SYNC_SIGNAL)) {
			sync->fence = drm_syncobj_fence_get(sync->syncobj);
			if (XE_IOCTL_ERR(xe, !sync->fence))
				return -EINVAL;
		}
		break;

	case DRM_XE_SYNC_TIMELINE_SYNCOBJ:
		if (XE_IOCTL_ERR(xe, sync_in.timeline_value == 0))
			return -EINVAL;

		sync->syncobj = drm_syncobj_find(xef->drm, sync_in.handle);
		if (XE_IOCTL_ERR(xe, !sync->syncobj))
			return -ENOENT;

		if (sync_in.flags & DRM_XE_SYNC_SIGNAL) {
			sync->chain_fence = dma_fence_chain_alloc();
			if (!sync->chain_fence)
				return -ENOMEM;
		} else {
			sync->fence = drm_syncobj_fence_get(sync->syncobj);
			if (XE_IOCTL_ERR(xe, !sync->fence))
				return -EINVAL;

			err = dma_fence_chain_find_seqno(&sync->fence,
							 sync_in.timeline_value);
			if (err)
				return err;
		}
		break;

	case DRM_XE_SYNC_DMA_BUF:
		if (XE_IOCTL_ERR(xe, "TODO"))
			return -EINVAL;
		break;

	default:
		return -EINVAL;
	}

	sync->flags = sync_in.flags;
	sync->timeline_value = sync_in.timeline_value;

	return 0;
}

int xe_sync_entry_add_deps(struct xe_sync_entry *sync, struct xe_sched_job *job)
{
	int err;

	if (sync->fence) {
		err = drm_sched_job_add_dependency(&job->drm, sync->fence);
		sync->fence = NULL;
		if (err)
			return err;
	}

	return 0;
}

void xe_sync_entry_signal(struct xe_sync_entry *sync, struct dma_fence *fence)
{
	if (!(sync->flags & DRM_XE_SYNC_SIGNAL))
		return;

	if (sync->chain_fence) {
		drm_syncobj_add_point(sync->syncobj, sync->chain_fence,
				      fence, sync->timeline_value);
		/*
		 * The chain's ownership is transferred to the
		 * timeline.
		 */
		sync->chain_fence = NULL;
	} else if (sync->syncobj) {
		drm_syncobj_replace_fence(sync->syncobj, fence);
	}

	/* TODO: external BO? */
}

void xe_sync_entry_cleanup(struct xe_sync_entry *sync)
{
	if (sync->syncobj)
		drm_syncobj_put(sync->syncobj);
	if (sync->fence)
		dma_fence_put(sync->fence);
	if (sync->chain_fence)
		dma_fence_put(&sync->chain_fence->base);
}
