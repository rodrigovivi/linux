#ifndef I915_VMA_H
#define I915_VMA_H

#include <drm/drm_mm.h>

struct xe_bo;

struct i915_vma {
	struct xe_bo *bo, *dpt;
	struct drm_mm_node node;
};

#endif
