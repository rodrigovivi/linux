// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include "i915_drv.h"
#include "intel_display_types.h"
#include "intel_dpt.h"
#include "intel_fb.h"
#include "intel_fb_pin.h"
#include "xe_ggtt.h"
#include "xe_gt.h"

#include <drm/ttm/ttm_bo_driver.h>

static void
write_dpt_rotated(struct xe_bo *bo, struct iosys_map *map, u32 *dpt_ofs, u32 bo_ofs,
		  u32 width, u32 height, u32 src_stride, u32 dst_stride)
{
	u32 column, row;
	/* TODO: Maybe rewrite so we can traverse the bo addresses sequentially,
	 * by writing dpt/ggtt in a different order?
	 */

	for (column = 0; column < width; column++) {
		u32 src_idx = src_stride * (height - 1) + column + bo_ofs;

		for (row = 0; row < height; row++) {
			iosys_map_wr(map, *dpt_ofs, u64,
				     xe_ggtt_pte_encode(bo, src_idx * GEN8_PAGE_SIZE));
			*dpt_ofs += 8;
			src_idx -= src_stride;
		}

		/* The DE ignores the PTEs for the padding tiles */
		*dpt_ofs += (dst_stride - height) * 8;
	}

	/* Align to next page */
	*dpt_ofs = ALIGN(*dpt_ofs, 4096);
}

static int __xe_pin_fb_vma_dpt(struct intel_framebuffer *fb,
			       const struct i915_gtt_view *view,
			       struct i915_vma *vma)
{
	struct xe_device *xe = to_xe_device(fb->base.dev);
	struct xe_bo *bo = intel_fb_obj(&fb->base), *dpt;
	u32 dpt_size, size = bo->ttm.base.size;

	if (view->type == I915_GTT_VIEW_NORMAL)
		dpt_size = ALIGN(size / GEN8_PAGE_SIZE * 8, GEN8_PAGE_SIZE);
	else
		/* display uses 4K tiles instead of bytes here, convert to entries.. */
		dpt_size = ALIGN(intel_rotation_info_size(&view->rotated) * 8, GEN8_PAGE_SIZE);

	dpt = xe_bo_create_pin_map(xe, to_gt(xe), NULL, dpt_size,
				  ttm_bo_type_kernel,
				  XE_BO_CREATE_VRAM0_BIT |
				  XE_BO_CREATE_GGTT_BIT);
	if (IS_ERR(dpt))
		dpt = xe_bo_create_pin_map(xe, to_gt(xe), NULL, dpt_size,
					   ttm_bo_type_kernel,
					   XE_BO_CREATE_STOLEN_BIT |
					   XE_BO_CREATE_GGTT_BIT);
	if (IS_ERR(dpt))
		dpt = xe_bo_create_pin_map(xe, to_gt(xe), NULL, dpt_size,
					   ttm_bo_type_kernel,
					   XE_BO_CREATE_SYSTEM_BIT |
					   XE_BO_CREATE_GGTT_BIT);
	if (IS_ERR(dpt))
		return PTR_ERR(dpt);

	if (view->type == I915_GTT_VIEW_NORMAL) {
		u32 x;

		for (x = 0; x < size / GEN8_PAGE_SIZE; x++)
			iosys_map_wr(&dpt->vmap, x * 8, u64,
				     xe_ggtt_pte_encode(bo, x * GEN8_PAGE_SIZE));
	} else {
		const struct intel_rotation_info *rot_info = &view->rotated;
		u32 i, dpt_ofs = 0;

		for (i = 0; i < ARRAY_SIZE(rot_info->plane); i++)
			write_dpt_rotated(bo, &dpt->vmap, &dpt_ofs,
					  rot_info->plane[i].offset,
					  rot_info->plane[i].width,
					  rot_info->plane[i].height,
					  rot_info->plane[i].src_stride,
					  rot_info->plane[i].dst_stride);
	}

	vma->dpt = dpt;
	vma->node = dpt->ggtt_node;
	return 0;
}

static void
write_ggtt_rotated(struct xe_bo *bo, struct xe_ggtt *ggtt, u32 *ggtt_ofs, u32 bo_ofs,
		   u32 width, u32 height, u32 src_stride, u32 dst_stride)
{
	u32 column, row;

	for (column = 0; column < width; column++) {
		u32 src_idx = src_stride * (height - 1) + column + bo_ofs;

		for (row = 0; row < height; row++) {
			xe_ggtt_set_pte(ggtt, *ggtt_ofs,
					xe_ggtt_pte_encode(bo, src_idx * GEN8_PAGE_SIZE));
			*ggtt_ofs += GEN8_PAGE_SIZE;
			src_idx -= src_stride;
		}

		/* The DE ignores the PTEs for the padding tiles */
		*ggtt_ofs += (dst_stride - height) * GEN8_PAGE_SIZE;
	}
}

static int __xe_pin_fb_vma_ggtt(struct intel_framebuffer *fb,
				const struct i915_gtt_view *view,
				struct i915_vma *vma)
{
	struct xe_bo *bo = intel_fb_obj(&fb->base);
	struct xe_device *xe = to_xe_device(fb->base.dev);
	struct xe_ggtt *ggtt = to_gt(xe)->mem.ggtt;
	int ret;

	/* TODO: Consider sharing framebuffer mapping?
	 * embed i915_vma inside intel_framebuffer
	 */
	ret = mutex_lock_interruptible(&ggtt->lock);
	if (ret)
		return ret;

	if (view->type == I915_GTT_VIEW_NORMAL) {
		u32 x, size = bo->ttm.base.size;

		ret = xe_ggtt_insert_special_node_locked(ggtt, &vma->node, size,
							GEN8_PAGE_SIZE, 0);
		if (ret)
			goto out;

		for (x = 0; x < size; x += GEN8_PAGE_SIZE)
			xe_ggtt_set_pte(ggtt, vma->node.start + x, xe_ggtt_pte_encode(bo, x));
	} else {
		u32 i, ggtt_ofs;
		const struct intel_rotation_info *rot_info = &view->rotated;

		/* display seems to use tiles instead of bytes here, so convert it back.. */
		u32 size = intel_rotation_info_size(rot_info) * GEN8_PAGE_SIZE;

		ret = xe_ggtt_insert_special_node_locked(ggtt, &vma->node, size,
							GEN8_PAGE_SIZE, 0);
		if (ret)
			goto out;

		ggtt_ofs = vma->node.start;

		for (i = 0; i < ARRAY_SIZE(rot_info->plane); i++)
			write_ggtt_rotated(bo, ggtt, &ggtt_ofs,
					   rot_info->plane[i].offset,
					   rot_info->plane[i].width,
					   rot_info->plane[i].height,
					   rot_info->plane[i].src_stride,
					   rot_info->plane[i].dst_stride);
	}

	xe_ggtt_invalidate(to_gt(xe));

out:
	mutex_unlock(&ggtt->lock);
	return ret;
}

static struct i915_vma *__xe_pin_fb_vma(struct intel_framebuffer *fb,
					const struct i915_gtt_view *view)
{
	struct i915_vma *vma = kzalloc(sizeof(*vma), GFP_KERNEL);
	struct xe_bo *bo = intel_fb_obj(&fb->base);
	int ret;

	if (!vma)
		return ERR_PTR(-ENODEV);

	/* Remapped view is only required on ADL-P, which xe doesn't support. */
	if (XE_WARN_ON(view->type == I915_GTT_VIEW_REMAPPED)) {
		ret = -ENODEV;
		goto err;
	}

	/*
	 * Pin the framebuffer, we can't use xe_bo_(un)pin functions as the
	 * assumptions are incorrect for framebuffers
	 */
	ret = ttm_bo_reserve(&bo->ttm, false, false, NULL);
	if (ret)
		goto err;

	ret = xe_bo_validate(bo, NULL, true);
	if (!ret)
		ttm_bo_pin(&bo->ttm);
	ttm_bo_unreserve(&bo->ttm);
	if (ret)
		goto err;

	vma->bo = bo;
	if (intel_fb_uses_dpt(&fb->base))
		ret = __xe_pin_fb_vma_dpt(fb, view, vma);
	else
		ret = __xe_pin_fb_vma_ggtt(fb, view, vma);
	if (ret)
		goto err_unpin;

	return vma;

err_unpin:
	ttm_bo_reserve(&bo->ttm, false, false, NULL);
	ttm_bo_unpin(&bo->ttm);
	ttm_bo_unreserve(&bo->ttm);
err:
	kfree(vma);
	return ERR_PTR(ret);
}

static void __xe_unpin_fb_vma(struct i915_vma *vma)
{
	struct xe_device *xe = to_xe_device(vma->bo->ttm.base.dev);
	struct xe_ggtt *ggtt = to_gt(xe)->mem.ggtt;

	if (vma->dpt)
		xe_bo_unpin_map_no_vm(vma->dpt);
	else
		xe_ggtt_remove_node(ggtt, &vma->node);

	ttm_bo_reserve(&vma->bo->ttm, false, false, NULL);
	ttm_bo_unpin(&vma->bo->ttm);
	ttm_bo_unreserve(&vma->bo->ttm);
	kfree(vma);
}

struct i915_vma *
intel_pin_and_fence_fb_obj(struct drm_framebuffer *fb,
			   bool phys_cursor,
			   const struct i915_gtt_view *view,
			   bool uses_fence,
			   unsigned long *out_flags)
{
	*out_flags = 0;

	return __xe_pin_fb_vma(to_intel_framebuffer(fb), view);
}

void intel_unpin_fb_vma(struct i915_vma *vma, unsigned long flags)
{
	__xe_unpin_fb_vma(vma);
}

int intel_plane_pin_fb(struct intel_plane_state *plane_state)
{
	struct drm_framebuffer *fb = plane_state->hw.fb;
        struct xe_bo *bo = intel_fb_obj(fb);
	struct i915_vma *vma;

	/* We reject creating !SCANOUT fb's, so this is weird.. */
	drm_WARN_ON(bo->ttm.base.dev, !(bo->flags & XE_BO_SCANOUT_BIT));

        vma = __xe_pin_fb_vma(to_intel_framebuffer(fb), &plane_state->view.gtt);
	if (IS_ERR(vma))
		return PTR_ERR(vma);

	plane_state->ggtt_vma = vma;
	return 0;
}

void intel_plane_unpin_fb(struct intel_plane_state *old_plane_state)
{
	__xe_unpin_fb_vma(old_plane_state->ggtt_vma);
	old_plane_state->ggtt_vma = NULL;
}
