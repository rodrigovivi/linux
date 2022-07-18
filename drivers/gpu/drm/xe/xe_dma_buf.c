// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <linux/dma-buf.h>

#include <drm/drm_device.h>
#include <drm/drm_prime.h>

#include <drm/ttm/ttm_tt.h>

#include "xe_bo.h"
#include "xe_device.h"
#include "xe_dma_buf.h"
#include "xe_ttm_vram_mgr.h"
#include "xe_vm.h"

MODULE_IMPORT_NS(DMA_BUF);

static int xe_dma_buf_attach(struct dma_buf *dmabuf,
			     struct dma_buf_attachment *attach)
{
	/* TODO: Grab PM ref */
	return 0;
}

static void xe_dma_buf_detach(struct dma_buf *dmabuf,
			      struct dma_buf_attachment *attach)
{
	/* TODO: Drop a PM ref */
}

static int xe_dma_buf_pin(struct dma_buf_attachment *attach)
{
	struct drm_gem_object *obj = attach->dmabuf->priv;
	struct xe_bo *bo = gem_to_xe_bo(obj);

	xe_bo_pin(bo);
	return 0;
}

static void xe_dma_buf_unpin(struct dma_buf_attachment *attach)
{
	struct drm_gem_object *obj = attach->dmabuf->priv;
	struct xe_bo *bo = gem_to_xe_bo(obj);

	xe_bo_unpin(bo);
}

static struct sg_table *xe_dma_buf_map(struct dma_buf_attachment *attach,
				       enum dma_data_direction dir)
{
	struct dma_buf *dma_buf = attach->dmabuf;
	struct drm_gem_object *obj = dma_buf->priv;
	struct xe_bo *bo = gem_to_xe_bo(obj);
	struct sg_table *sgt;
	long r;

	if (!xe_bo_is_pinned(bo)) {
		r = xe_bo_validate(bo, NULL);
		if (r)
			return ERR_PTR(r);
	}

	switch (bo->ttm.resource->mem_type) {
	case TTM_PL_TT:
		sgt = drm_prime_pages_to_sg(obj->dev,
					    bo->ttm.ttm->pages,
					    bo->ttm.ttm->num_pages);
		if (IS_ERR(sgt))
			return sgt;

		if (dma_map_sgtable(attach->dev, sgt, dir,
				    DMA_ATTR_SKIP_CPU_SYNC))
			goto error_free;
		break;

	case TTM_PL_VRAM:
		r = xe_ttm_vram_mgr_alloc_sgt(xe_bo_device(bo),
					      bo->ttm.resource, 0,
					      bo->ttm.base.size, attach->dev,
					      dir, &sgt);
		if (r)
			return ERR_PTR(r);
		break;
	default:
		return ERR_PTR(-EINVAL);
	}

	return sgt;

error_free:
	sg_free_table(sgt);
	kfree(sgt);
	return ERR_PTR(-EBUSY);
}

static void xe_dma_buf_unmap(struct dma_buf_attachment *attach,
			     struct sg_table *sgt,
			     enum dma_data_direction dir)
{
	if (sgt->sgl->page_link) {
		dma_unmap_sgtable(attach->dev, sgt, dir, 0);
		sg_free_table(sgt);
		kfree(sgt);
	} else {
		xe_ttm_vram_mgr_free_sgt(attach->dev, dir, sgt);
	}
}

static int xe_dma_buf_begin_cpu_access(struct dma_buf *dma_buf,
				       enum dma_data_direction direction)
{
	/* TODO: Migrate object CPU if allowed */

	return 0;
}

const struct dma_buf_ops xe_dmabuf_ops = {
	.attach = xe_dma_buf_attach,
	.detach = xe_dma_buf_detach,
	.pin = xe_dma_buf_pin,
	.unpin = xe_dma_buf_unpin,
	.map_dma_buf = xe_dma_buf_map,
	.unmap_dma_buf = xe_dma_buf_unmap,
	.release = drm_gem_dmabuf_release,
	.begin_cpu_access = xe_dma_buf_begin_cpu_access,
	.mmap = drm_gem_dmabuf_mmap,
	.vmap = drm_gem_dmabuf_vmap,
	.vunmap = drm_gem_dmabuf_vunmap,
};

struct dma_buf *xe_gem_prime_export(struct drm_gem_object *obj, int flags)
{
	struct xe_bo *bo = gem_to_xe_bo(obj);
	struct dma_buf *buf;

	if (bo->vm)
		return ERR_PTR(-EPERM);

	buf = drm_gem_prime_export(obj, flags);
	if (!IS_ERR(buf))
		buf->ops = &xe_dmabuf_ops;

	return buf;
}

static struct drm_gem_object *
xe_dma_buf_create_obj(struct drm_device *dev, struct dma_buf *dma_buf)
{
	struct dma_resv *resv = dma_buf->resv;
	struct xe_device *xe = to_xe_device(dev);
	struct xe_bo *bo;
	int ret;

	dma_resv_lock(resv, NULL);
	bo = __xe_bo_create_locked(xe, resv, dma_buf->size, ttm_bo_type_sg,
				   XE_BO_CREATE_SYSTEM_BIT);
	if (IS_ERR(bo)) {
		ret = PTR_ERR(bo);
		goto error;
	}
	dma_resv_unlock(resv);

	return &bo->ttm.base;

error:
	dma_resv_unlock(resv);
	return ERR_PTR(ret);
}

static void
xe_dma_buf_move_notify(struct dma_buf_attachment *attach)
{
	struct drm_gem_object *obj = attach->importer_priv;
	struct xe_bo *bo = gem_to_xe_bo(obj);

	xe_bo_trigger_rebind(bo);
}

static const struct dma_buf_attach_ops xe_dma_buf_attach_ops = {
	.allow_peer2peer = true,
	.move_notify = xe_dma_buf_move_notify
};

struct drm_gem_object *xe_gem_prime_import(struct drm_device *dev,
					   struct dma_buf *dma_buf)
{
	struct dma_buf_attachment *attach;
	struct drm_gem_object *obj;

	if (dma_buf->ops == &xe_dmabuf_ops) {
		obj = dma_buf->priv;
		if (obj->dev == dev) {
			/*
			 * Importing dmabuf exported from out own gem increases
			 * refcount on gem itself instead of f_count of dmabuf.
			 */
			drm_gem_object_get(obj);
			return obj;
		}
	}

	obj = xe_dma_buf_create_obj(dev, dma_buf);
	if (IS_ERR(obj))
		return obj;

	attach = dma_buf_dynamic_attach(dma_buf, dev->dev,
					&xe_dma_buf_attach_ops, obj);
	if (IS_ERR(attach))
		return ERR_CAST(attach);

	get_dma_buf(dma_buf);
	obj->import_attach = attach;
	return obj;
}
