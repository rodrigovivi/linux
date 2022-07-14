/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_BO_TYPES_H_
#define _XE_BO_TYPES_H_

#include <linux/iosys-map.h>

#include <drm/drm_mm.h>
#include <drm/ttm/ttm_bo_api.h>
#include <drm/ttm/ttm_device.h>
#include <drm/ttm/ttm_execbuf_util.h>
#include <drm/ttm/ttm_placement.h>

struct xe_device;
struct xe_vm;

#define XE_BO_MAX_PLACEMENTS	3

/** @xe_bo: XE buffer object */
struct xe_bo {
	/** @ttm: TTM base buffer object */
	struct ttm_buffer_object ttm;
	/** @size: Size of this buffer object */
	size_t size;
	/** @flags: flags for this buffer object */
	uint32_t flags;
	/** @vm: VM this BO is attached to, for extobj this will be NULL */
	struct xe_vm *vm;
	/** @vmas: List of VMAs for this BO */
	struct list_head vmas;
	/** @placements: valid placements for this BO */
	struct ttm_place placements[XE_BO_MAX_PLACEMENTS];
	/** @placement: current placement for this BO */
	struct ttm_placement placement;
	/** @ggtt_node: GGTT node if this BO is mapped in the GGTT */
	struct drm_mm_node ggtt_node;
	/** @vmap: iosys map of this buffer */
	struct iosys_map vmap;
	/** @extobj_tv: used during exec to lock all external BOs */
	struct ttm_validate_buffer extobj_tv;
	/** @pinned_link: link to present / evicted list of pinned BO */
	struct list_head pinned_link;
};

#endif	/* _XE_BO_TYPES_H_ */
