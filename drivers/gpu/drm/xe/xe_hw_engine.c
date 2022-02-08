/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#include "xe_hw_engine.h"

#include <drm/drm_managed.h>

#include "xe_bo.h"
#include "xe_execlist.h"
#include "xe_gt.h"
#include "xe_hw_fence.h"
#include "xe_lrc.h"
#include "xe_macros.h"
#include "xe_sched_job.h"

#include "../i915/i915_reg.h"

#define MAX_MMIO_BASES 3
struct engine_info {
	const char *name;
	unsigned int class : 8;
	unsigned int instance : 8;
	/* mmio bases table *must* be sorted in reverse graphics_ver order */
	struct engine_mmio_base {
		unsigned int graphics_ver : 8;
		unsigned int base : 24;
	} mmio_bases[MAX_MMIO_BASES];
};

static const struct engine_info engine_infos[] = {
	[XE_HW_ENGINE_RCS0] = {
		.name = "rcs0",
		.class = XE_ENGINE_CLASS_RENDER,
		.instance = 0,
		.mmio_bases = {
			{ .graphics_ver = 1, .base = RENDER_RING_BASE }
		},
	},
	[XE_HW_ENGINE_BCS0] = {
		.name = "bcs0",
		.class = XE_ENGINE_CLASS_COPY,
		.instance = 0,
		.mmio_bases = {
			{ .graphics_ver = 6, .base = BLT_RING_BASE }
		},
	},
	[XE_HW_ENGINE_VCS0] = {
		.name = "vcs0",
		.class = XE_ENGINE_CLASS_VIDEO_DECODE,
		.instance = 0,
		.mmio_bases = {
			{ .graphics_ver = 11, .base = GEN11_BSD_RING_BASE },
			{ .graphics_ver = 6, .base = GEN6_BSD_RING_BASE },
			{ .graphics_ver = 4, .base = BSD_RING_BASE }
		},
	},
	[XE_HW_ENGINE_VCS1] = {
		.name = "vcs1",
		.class = XE_ENGINE_CLASS_VIDEO_DECODE,
		.instance = 1,
		.mmio_bases = {
			{ .graphics_ver = 11, .base = GEN11_BSD2_RING_BASE },
			{ .graphics_ver = 8, .base = GEN8_BSD2_RING_BASE }
		},
	},
	[XE_HW_ENGINE_VCS2] = {
		.name = "vcs2",
		.class = XE_ENGINE_CLASS_VIDEO_DECODE,
		.instance = 2,
		.mmio_bases = {
			{ .graphics_ver = 11, .base = GEN11_BSD3_RING_BASE }
		},
	},
	[XE_HW_ENGINE_VCS3] = {
		.name = "vcs3",
		.class = XE_ENGINE_CLASS_VIDEO_DECODE,
		.instance = 3,
		.mmio_bases = {
			{ .graphics_ver = 11, .base = GEN11_BSD4_RING_BASE }
		},
	},
	[XE_HW_ENGINE_VCS4] = {
		.name = "vcs4",
		.class = XE_ENGINE_CLASS_VIDEO_DECODE,
		.instance = 4,
		.mmio_bases = {
			{ .graphics_ver = 12, .base = XEHP_BSD5_RING_BASE }
		},
	},
	[XE_HW_ENGINE_VCS5] = {
		.name = "vcs5",
		.class = XE_ENGINE_CLASS_VIDEO_DECODE,
		.instance = 5,
		.mmio_bases = {
			{ .graphics_ver = 12, .base = XEHP_BSD6_RING_BASE }
		},
	},
	[XE_HW_ENGINE_VCS6] = {
		.name = "vcs6",
		.class = XE_ENGINE_CLASS_VIDEO_DECODE,
		.instance = 6,
		.mmio_bases = {
			{ .graphics_ver = 12, .base = XEHP_BSD7_RING_BASE }
		},
	},
	[XE_HW_ENGINE_VCS7] = {
		.name = "vcs7",
		.class = XE_ENGINE_CLASS_VIDEO_DECODE,
		.instance = 7,
		.mmio_bases = {
			{ .graphics_ver = 12, .base = XEHP_BSD8_RING_BASE }
		},
	},
	[XE_HW_ENGINE_VECS0] = {
		.name = "vecs0",
		.class = XE_ENGINE_CLASS_VIDEO_ENHANCE,
		.instance = 0,
		.mmio_bases = {
			{ .graphics_ver = 11, .base = GEN11_VEBOX_RING_BASE },
			{ .graphics_ver = 7, .base = VEBOX_RING_BASE }
		},
	},
	[XE_HW_ENGINE_VECS1] = {
		.name = "vecs1",
		.class = XE_ENGINE_CLASS_VIDEO_ENHANCE,
		.instance = 1,
		.mmio_bases = {
			{ .graphics_ver = 11, .base = GEN11_VEBOX2_RING_BASE }
		},
	},
	[XE_HW_ENGINE_VECS2] = {
		.name = "vecs2",
		.class = XE_ENGINE_CLASS_VIDEO_ENHANCE,
		.instance = 2,
		.mmio_bases = {
			{ .graphics_ver = 12, .base = XEHP_VEBOX3_RING_BASE }
		},
	},
	[XE_HW_ENGINE_VECS3] = {
		.name = "vecs3",
		.class = XE_ENGINE_CLASS_VIDEO_ENHANCE,
		.instance = 3,
		.mmio_bases = {
			{ .graphics_ver = 12, .base = XEHP_VEBOX4_RING_BASE }
		},
	},
};

static uint32_t engine_info_mmio_base(const struct engine_info *info,
				      unsigned int graphics_ver)
{
	int i;

	for (i = 0; i < MAX_MMIO_BASES; i++)
		if (graphics_ver >= info->mmio_bases[i].graphics_ver)
			break;

	XE_BUG_ON(i == MAX_MMIO_BASES);
	XE_BUG_ON(!info->mmio_bases[i].base);

	return info->mmio_bases[i].base;
}

static void hw_engine_fini(struct drm_device *drm, void *arg)
{
	struct xe_hw_engine *hwe = arg;

	xe_hw_fence_irq_finish(&hwe->fence_irq);
	xe_execlist_port_destroy(hwe->exl_port);
	xe_lrc_finish(&hwe->kernel_lrc);

	xe_bo_unpin_map_no_vm(hwe->hwsp);

	hwe->gt = NULL;
}

int xe_hw_engine_init(struct xe_gt *gt, struct xe_hw_engine *hwe,
		      enum xe_hw_engine_id id)
{
	struct xe_device *xe = gt_to_xe(gt);
	const struct engine_info *info = &engine_infos[id];
	int err;

	if (WARN_ON(id >= ARRAY_SIZE(engine_infos) || info->name == NULL))
		return -EINVAL;

	XE_BUG_ON(hwe->gt);

	if (!(gt->info.engine_mask & BIT(id)))
		return 0;

	hwe->gt = gt;
	hwe->class = info->class;
	hwe->instance = info->instance;
	hwe->mmio_base = engine_info_mmio_base(info, GRAPHICS_VER(xe));

	hwe->hwsp = xe_bo_create_locked(xe, NULL, SZ_4K, ttm_bo_type_kernel,
					XE_BO_CREATE_VRAM_IF_DGFX(xe) |
					XE_BO_CREATE_GGTT_BIT);
	if (IS_ERR(hwe->hwsp))
		return PTR_ERR(hwe->hwsp);

	err = xe_bo_pin(hwe->hwsp);
	if (err)
		goto err_unlock_put_hwsp;

	err = xe_bo_vmap(hwe->hwsp);
	if (err)
		goto err_unpin_hwsp;

	xe_bo_unlock_no_vm(hwe->hwsp);

	err = xe_lrc_init(&hwe->kernel_lrc, hwe, NULL, SZ_16K);
	if (err)
		goto err_hwsp;

	hwe->exl_port = xe_execlist_port_create(xe, hwe);
	if (IS_ERR(hwe->exl_port)) {
		err = PTR_ERR(hwe->exl_port);
		goto err_kernel_lrc;
	}

	xe_hw_fence_irq_init(&hwe->fence_irq);

	err = drmm_add_action_or_reset(&xe->drm, hw_engine_fini, hwe);
	if (err)
		return err;

	hwe->name = info->name;

	return 0;

err_unpin_hwsp:
	xe_bo_unpin(hwe->hwsp);
err_unlock_put_hwsp:
	xe_bo_unlock_no_vm(hwe->hwsp);
	xe_bo_put(hwe->hwsp);
err_kernel_lrc:
	xe_lrc_finish(&hwe->kernel_lrc);
err_hwsp:
	xe_bo_put(hwe->hwsp);
	return err;
}

void xe_hw_engine_handle_irq(struct xe_hw_engine *hwe, uint16_t intr_vec)
{
	if (hwe->irq_handler)
		hwe->irq_handler(hwe, intr_vec);

	if (intr_vec & GT_RENDER_USER_INTERRUPT)
		xe_hw_fence_irq_run(&hwe->fence_irq);
}
