// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */
#include <drm/drm_pt_walk.h>

static const u64 xe_normal_pt_shifts[] = {12, 21, 30, 39, 48};
static const u64 xe_compact_pt_shifts[] = {16, 21, 30, 39, 48};

#define XE_PT_HIGHEST_LEVEL (ARRAY_SIZE(xe_normal_pt_shifts) - 1)

/**
 * DOC: pagetable building
 *
 * Below we use the term "page-table" for both page-directories, containing
 * pointers to lower level page-directories or page-tables, and level 0
 * page-tables that contain only page-table-entries pointing to memory pages.
 *
 * When inserting an address range in an already existing page-table tree
 * there will typically be a set of page-tables that are shared with other
 * address ranges, and a set that are private to this address range.
 * The set of shared page-tables can be at most two per level,
 * and those can't be updated immediately because the entries of those
 * page-tables may still be in use by the gpu for other mappings. Therefore
 * when inserting entries into those, we instead stage those insertions by
 * adding insertion data into struct xe_vm_pgtable_update structures. This
 * data, (subtrees for the cpu and page-table-entries for the gpu) is then
 * added in a separate commit step. CPU-data is committed while still under the
 * vm lock, the object lock and for userptr, the notifier lock in read mode.
 * The GPU async data is committed either by the GPU or CPU after fulfilling
 * relevant dependencies.
 * For non-shared page-tables (and, in fact, for shared ones that aren't
 * existing at the time of staging), we add the data in-place without the
 * special update structures. This private part of the page-table tree will
 * remain disconnected from the vm page-table tree until data is committed to
 * the shared page tables of the vm tree in the commit phase.
 */

struct xe_pt_update {
	/** @update: The update structure we're building for this parent. */
	struct xe_vm_pgtable_update *update;
	/** @parent: The parent. Used to detect a parent change. */
	struct xe_pt *parent;
	/** @preexisint: Whether the parent was pre-existing or allocated */
	bool preexisting;
};

struct xe_pt_stage_bind_walk {
	/** drm: The base class. */
	struct drm_pt_walk drm;

	/* Input parameters for the walk */
	/** @vm: The vm we're building for. */
	struct xe_vm *vm;
	/** @gt: The gt we're building for. */
	struct xe_gt *gt;
	/** @cache: Desired cache level for the ptes */
	enum xe_cache_level cache;
	/** @default_pte: PTE flag only template. No address is associated */
	u64 default_pte;
	/** @dma_offset: DMA offset to add to the PTE. */
	u64 dma_offset;
	/**
	 * @needs_64k: This address range enforces 64K alignment and
	 * granularity.
	 */
	bool needs_64K;
	/**
	 * @pte_flags: Flags determining PTE setup. These are not flags
	 * encoded directly in the PTE. See @default_pte for those.
	 */
	u32 pte_flags;

	/* Also input, but is updated during the walk*/
	/** @curs: The DMA address cursor. */
	struct xe_res_cursor *curs;
	/** @va_curs_start: The Virtual address coresponding to @curs->start */
	u64 va_curs_start;

	/* Output */
	struct xe_walk_update {
		/** @wupd.entries: Caller provided storage. */
		struct xe_vm_pgtable_update *entries;
		/** @wupd.num_used_entries: Number of update @entries used. */
		unsigned int num_used_entries;
		/** @wupd.updates: Tracks the update entry at a given level */
		struct xe_pt_update updates[XE_VM_MAX_LEVEL + 1];
	} wupd;

	/* Walk state */
	/**
	 * @l0_end_addr: The end address of the current l0 leaf. Used for
	 * 64K granularity detection.
	 */
	u64 l0_end_addr;
	/** @addr_64K: The start address of the current 64K chunk. */
	u64 addr_64K;
	/** @found_64: Whether @add_64K actually points to a 64K chunk. */
	bool found_64K;
};

static int
xe_pt_new_shared(struct xe_walk_update *wupd, struct xe_pt *parent,
		 pgoff_t offset, bool alloc_entries)
{
	struct xe_pt_update *upd = &wupd->updates[parent->level];
	struct xe_vm_pgtable_update *entry;

	/*
	 * For *each level*, we could only have one active
	 * struct xt_pt_update at any one time. Once we move on to a
	 * new parent and page-directory, the old one is complete, and
	 * updates are either already stored in the build tree or in
	 * @wupd->entries
	 */
	if (likely(upd->parent == parent))
		return 0;

	upd->parent = parent;
	upd->preexisting = true;

	if (wupd->num_used_entries == XE_VM_MAX_LEVEL * 2 + 1)
		return -EINVAL;

	entry = wupd->entries + wupd->num_used_entries++;
	upd->update = entry;
	entry->ofs = offset;
	entry->pt_bo = parent->bo;
	entry->pt = parent;
	entry->flags = 0;
	entry->qwords = 0;

	if (alloc_entries) {
		entry->pt_entries = kmalloc_array(GEN8_PDES,
						  sizeof(*entry->pt_entries),
						  GFP_KERNEL);
		if (!entry->pt_entries)
			return -ENOMEM;
	}

	return 0;
}

/*
 * NOTE: This is a very frequently called function so we allow ourselves
 * to annotate (using branch prediction hints) the fastpath of updating a
 * non-pre-existing pagetable with leaf ptes.
 */
static int
xe_pt_insert_entry(struct xe_pt_stage_bind_walk *xe_walk, struct xe_pt *parent,
		   pgoff_t offset, struct xe_pt *xe_child, u64 pte)
{
	struct xe_pt_update *upd = &xe_walk->wupd.updates[parent->level];
	struct xe_pt_update *child_upd = xe_child ?
		&xe_walk->wupd.updates[xe_child->level] : NULL;
	int ret;

	ret = xe_pt_new_shared(&xe_walk->wupd, parent, offset, true);
	if (unlikely(ret))
		return ret;

	/*
	 * Register this new pagetable so that it won't be recognized as
	 * a shared pagetable by a subsequent insertion.
	 */
	if (unlikely(child_upd)) {
		child_upd->update = NULL;
		child_upd->parent = xe_child;
		child_upd->preexisting = false;
	}

	if (likely(!upd->preexisting)) {
		/* Continue building a non-connected subtree. */
		struct iosys_map *map = &parent->bo->vmap;

		if (unlikely(xe_child))
			parent->drm.dir->entries[offset] = &xe_child->drm;

		xe_pt_write(xe_walk->vm->xe, map, offset, pte);
		parent->num_live++;
	} else {
		/* Shared pt. Stage update. */
		unsigned int idx;
		struct xe_vm_pgtable_update *entry = upd->update;

		idx = offset - entry->ofs;
		entry->pt_entries[idx].pt = xe_child;
		entry->pt_entries[idx].pte = pte;
		entry->qwords++;
	}

	return 0;
}

static void xe_pt_init(struct xe_pt *pt, struct xe_pt_stage_bind_walk *xe_walk)
{
	struct xe_vm *vm = xe_walk->vm;
	struct xe_gt *gt = xe_walk->gt;

	if (!vm->scratch_bo[gt->info.id]) {
		xe_map_memset(vm->xe, &pt->bo->vmap, 0, 0, SZ_4K);
	} else {
		u64 empty = __xe_vm_empty_pte(gt, vm, pt->level);
		int i;

		for (i = 0; i < 512; ++i)
			xe_pt_write(vm->xe, &pt->bo->vmap, i, empty);
	}
}

static bool xe_pt_hugepte_possible(u64 addr, u64 next, unsigned int level,
				   struct xe_pt_stage_bind_walk *xe_walk)
{
	u64 size, dma;

	/* Does the virtual range requested cover a huge pte? */
	if (!drm_pt_covers(addr, next, level, &xe_walk->drm))
		return false;

	/* Does the DMA segment cover the whole pte? */
	if (next - xe_walk->va_curs_start > xe_walk->curs->size)
		return false;

	/* Is the DMA address huge PTE size aligned? */
	size = next - addr;
	dma = addr - xe_walk->va_curs_start + xe_res_dma(xe_walk->curs);

	return IS_ALIGNED(dma, size);
}

/*
 * Scan the requested mapping to check whether it can be done entirely
 * with 64K PTEs.
 */
static bool
xe_pt_scan_64K(u64 addr, u64 next, struct xe_pt_stage_bind_walk *xe_walk)
{
	struct xe_res_cursor curs = *xe_walk->curs;

	if (!IS_ALIGNED(addr, SZ_64K))
		return false;

	if (next > xe_walk->l0_end_addr)
		return false;

	xe_res_next(&curs, addr - xe_walk->va_curs_start);
	for (; addr < next; addr += SZ_64K) {
		if (!IS_ALIGNED(xe_res_dma(&curs), SZ_64K) || curs.size < SZ_64K)
			return false;

		xe_res_next(&curs, SZ_64K);
	}

	return addr == next;
}

/*
 * For non-compact "normal" 4K level-0 pagetables, we want to try to group
 * addresses together in 64K-contigous regions to add a 64K TLB hint for the
 * device to the PTE.
 * This function determines whether the address is part of such a
 * segment. For VRAM in normal pagetables, this is strictly necessary on
 * some devices.
 */
static bool
xe_pt_is_pte_ps64K(u64 addr, u64 next, struct xe_pt_stage_bind_walk *xe_walk)
{
	/* Address is within an already found 64k region */
	if (xe_walk->found_64K && addr - xe_walk->addr_64K < SZ_64K)
		return true;

	xe_walk->found_64K = xe_pt_scan_64K(addr, addr + SZ_64K, xe_walk);
	xe_walk->addr_64K = addr;

	return xe_walk->found_64K;
}

static int
xe_pt_stage_bind_entry(struct drm_pt *parent, pgoff_t offset,
		       unsigned int level, u64 addr, u64 next,
		       struct drm_pt **child,
		       enum page_walk_action *action,
		       struct drm_pt_walk *walk)
{
	struct xe_pt_stage_bind_walk *xe_walk =
		container_of(walk, typeof(*xe_walk), drm);
	struct xe_pt *xe_parent = container_of(parent, typeof(*xe_parent), drm);
	struct xe_pt *xe_child;
	bool covers;
	int ret = 0;
	u64 pte;

	/* Is this a leaf entry ?*/
	if (level == 0 || xe_pt_hugepte_possible(addr, next, level, xe_walk)) {
		struct xe_res_cursor *curs = xe_walk->curs;

		XE_WARN_ON(xe_walk->va_curs_start != addr);

		pte = __gen8_pte_encode(xe_res_dma(curs) + xe_walk->dma_offset,
					xe_walk->cache, xe_walk->pte_flags,
					level);
		pte |= xe_walk->default_pte;

		/*
		 * Set the GEN12_PTE_PS64 hint if possible, otherwise if
		 * this device *requires* 64K PTE size for VRAM, fail.
		 */
		if (level == 0 && !xe_parent->is_compact) {
			if (xe_pt_is_pte_ps64K(addr, next, xe_walk))
				pte |= GEN12_PTE_PS64;
			else if (XE_WARN_ON(xe_walk->needs_64K))
				return -EINVAL;
		}

		ret = xe_pt_insert_entry(xe_walk, xe_parent, offset, NULL, pte);
		if (unlikely(ret))
			return ret;

		xe_res_next(curs, next - addr);
		xe_walk->va_curs_start = next;
		*action = ACTION_CONTINUE;

		return ret;
	}

	/*
	 * Descending to lower level. Determine if we need to allocate a
	 * new page table or -directory, which we do if there is no
	 * previous one or there is one we can completely replace.
	 */
	if (level == 1) {
		walk->shifts = xe_normal_pt_shifts;
		xe_walk->l0_end_addr = next;
	}

	covers = drm_pt_covers(addr, next, level, &xe_walk->drm);
	if (covers || !*child) {
		u64 flags = 0;

		xe_child = xe_pt_create(xe_walk->vm, xe_walk->gt, level - 1);
		if (IS_ERR(xe_child))
			return PTR_ERR(xe_child);

		if (!covers)
			xe_pt_init(xe_child, xe_walk);

		*child = &xe_child->drm;

		/*
		 * Prefer the compact pagetable layout for L0 if possible.
		 * TODO: Suballocate the pt bo to avoid wasting a lot of
		 * memory.
		 */
		if (GRAPHICS_VERx100(xe_walk->gt->xe) >= 1250 && level == 1 &&
		    covers && xe_pt_scan_64K(addr, next, xe_walk)) {
			walk->shifts = xe_compact_pt_shifts;
			flags |= GEN12_PDE_64K;
			xe_child->is_compact = true;
		}

		pte = gen8_pde_encode(xe_child->bo, 0, xe_walk->cache) | flags;
		ret = xe_pt_insert_entry(xe_walk, xe_parent, offset, xe_child,
					 pte);
	}

	*action = ACTION_SUBTREE;
	return ret;
}

static const struct drm_pt_walk_ops xe_pt_stage_bind_ops = {
	.pt_entry = xe_pt_stage_bind_entry,
};

/**
 * xe_pt_stage_bind - Build a disconnected page-table tree for a given address
 * range.
 * @gt: The gt we're building for.
 * @vma: The vma indicating the address range.
 * @entries: Storage for the update entries used for connecting the tree to
 * the main tree at commit time.
 * @num_entries: On output contains the number of @entries used.
 *
 * This function builds a disconnected page-table tree for a given address
 * range. The tree is connected to the main vm tree for the gpu using
 * xe_migrate_update_pgtables() and for the cpu using xe_pt_commit_bind().
 * The function builds xe_vm_pgtable_update structures for already existing
 * shared page-tables, and non-existing shared and non-shared page-tables
 * are built and populated directly.
 *
 * Return 0 on success, negative error code on error.
 */
static int
xe_pt_stage_bind(struct xe_gt *gt, struct xe_vma *vma,
		 struct xe_vm_pgtable_update *entries, u32 *num_entries)
{
	struct xe_bo *bo = vma->bo;
	bool is_vram = !xe_vma_is_userptr(vma) && bo && xe_bo_is_vram(bo);
	struct xe_res_cursor curs;
	struct xe_pt_stage_bind_walk xe_walk = {
		.drm = {
			.ops = &xe_pt_stage_bind_ops,
			.shifts = xe_normal_pt_shifts,
			.max_level = XE_PT_HIGHEST_LEVEL,
		},
		.vm = vma->vm,
		.gt = gt,
		.curs = &curs,
		.va_curs_start = vma->start,
		.cache = XE_CACHE_WB,
		.pte_flags = vma->pte_flags,
		.wupd.entries = entries,
		.needs_64K = (vma->vm->flags & XE_VM_FLAGS_64K) && is_vram,
	};
	struct xe_pt *pt = vma->vm->pt_root[gt->info.id];
	int ret;

	if (is_vram) {
		xe_walk.default_pte = GEN12_PPGTT_PTE_LM;
		if (vma && vma->use_atomic_access_pte_bit)
			xe_walk.default_pte |= GEN12_USM_PPGTT_PTE_AE;
		xe_walk.dma_offset = gt->mem.vram.io_start -
			gt_to_xe(gt)->mem.vram.io_start;
	}

	xe_bo_assert_held(bo);
	if (xe_vma_is_userptr(vma))
		xe_res_first_dma(vma->userptr.dma_address, 0,
				 vma->end - vma->start + 1, &curs);
	else if (xe_bo_is_vram(bo))
		xe_res_first(bo->ttm.resource, vma->bo_offset,
			     vma->end - vma->start + 1, &curs);
	else
		xe_res_first_dma(bo->ttm.ttm->dma_address, vma->bo_offset,
				 vma->end - vma->start + 1, &curs);

	ret = drm_pt_walk_range(&pt->drm, pt->level, vma->start, vma->end + 1,
				&xe_walk.drm);

	*num_entries = xe_walk.wupd.num_used_entries;
	return ret;
}

/**
 * xe_pt_nonshared_offsets - Determine the non-shared entry offsets of a shared
 * pagetable
 * @addr: The start address within the non-shared pagetable.
 * @end: The end address within the non-shared pagetable.
 * @level: The level of the non-shared pagetable.
 * @walk: Walk info. The function adjusts the walk action.
 * @offset: Ignored on input, First non-shared entry on output.
 * @end_offset: Ignored on input, Last non-shared entry + 1 on output.
 *
 * A non-shared page-table has some entries that belong to the address range
 * and others that don't. This function determines the entries that belong
 * fully to the address range. Depending on level, some entries may
 * partially belong to the address range (that can't happen at level 0).
 * The function detects that and adjust those offsets to not include those
 * partial entries. Iff it does detect partial entries, we know that there must
 * be shared page tables also at lower levels, so it adjusts the walk action
 * accordingly.
 *
 * Note that the function is not device-specific so could be made a drm
 * pagewalk helper.
 *
 * Return: true if there were non-shared entries, false otherwise.
 */
static bool xe_pt_nonshared_offsets(u64 addr, u64 next, unsigned int level,
				    struct drm_pt_walk *walk,
				    enum page_walk_action *action,
				    pgoff_t *offset, pgoff_t *end_offset)
{
	u64 size = 1ull << walk->shifts[level];

	*offset = drm_pt_offset(addr, level, walk);
	*end_offset = drm_pt_num_entries(addr, next, level, walk) + *offset;

	if (!level)
		return true;

	/*
	 * If addr or next are not size aligned, there are shared pts at lower
	 * level, so in that case traverse down the subtree
	 */
	*action = ACTION_CONTINUE;
	if (!IS_ALIGNED(addr, size)) {
		*action = ACTION_SUBTREE;
		(*offset)++;
	}

	if (!IS_ALIGNED(next, size)) {
		*action = ACTION_SUBTREE;
		(*end_offset)--;
	}

	return *end_offset > *offset;
}

struct xe_pt_build_leaves_walk {
	/** @drm: The walk base-class */
	struct drm_pt_walk drm;

	/* Input parameters for the walk */
	/** @gt: The gt we're building for */
	struct xe_gt *gt;

	/* Output */
	/** @leaves: Pointer to the leaves structure we're building */
	struct xe_vma_usm *leaves;
};

static int xe_pt_build_leaves_entry(struct drm_pt *parent, pgoff_t offset,
				    unsigned int level, u64 addr, u64 next,
				    struct drm_pt **child,
				    enum page_walk_action *action,
				    struct drm_pt_walk *walk)
{
	struct xe_pt_build_leaves_walk *xe_walk =
		container_of(walk, typeof(*xe_walk), drm);
	struct xe_pt *xe_child = container_of(*child, typeof(*xe_child), drm);
	pgoff_t end_offset;

	XE_BUG_ON(!*child);
	XE_BUG_ON(!level && xe_child->is_compact);

	/*
	 * Note that we're called from an entry callback, and we're dealing
	 * with the child of that entry rather than the parent, so need to
	 * adjust level down.
	 */
	if (xe_pt_nonshared_offsets(addr, next, --level, walk, action, &offset,
				    &end_offset))
		vma_usm_add_leaf(xe_walk->gt, xe_walk->leaves, xe_child,
				 offset * sizeof(u64),
				 (end_offset - offset) * sizeof(u64));

	return 0;
}

static const struct drm_pt_walk_ops xe_pt_build_leaves_ops = {
	.pt_entry = xe_pt_build_leaves_entry,
};

/**
 * xe_pt_build_leaves - Build leaves information for quick GPU PTE zapping.
 * @gt: The gt we're building for.
 * @vma: GPU VMA detailing address range and holding the usm structure.
 *
 * Eviction and Userptr invalidation needs to be able to zap the
 * gpu ptes of a given address range with special locking requirements.
 * This is done using the xe_vm_invalidate_vma() function. In order to
 * be able to do that, that function needs access to the shared page-table
 * leaves, so it can either clear the leaf PTEs or clear the pointers to
 * lower-level page-tables. This function builds that necessary information
 * for a pre-existing connected page-table tree. The function needs to be
 * called in the same critical section that commits the bind operation for
 * the vma.
 */
static void xe_pt_build_leaves(struct xe_gt *gt, struct xe_vma *vma)
{
	struct xe_pt_build_leaves_walk xe_walk = {
		.drm = {
			.ops = &xe_pt_build_leaves_ops,
			.shifts = xe_normal_pt_shifts,
			.max_level = XE_PT_HIGHEST_LEVEL,
		},
		.gt = gt,
		.leaves = &vma->usm,
	};
	struct xe_pt *pt = vma->vm->pt_root[gt->info.id];

	vma->usm.gt[gt->info.id].num_leaves = 0;
	(void)drm_pt_walk_shared(&pt->drm, pt->level, vma->start, vma->end + 1,
				 &xe_walk.drm);
}

struct xe_pt_stage_unbind_walk {
	/** @drm: The pagewalk base-class. */
	struct drm_pt_walk drm;

	/* Input parameters for the walk */
	/** @gt: The gt we're unbinding from. */
	struct xe_gt *gt;

	/**
	 * @modified_start: Walk range start, modified to include any
	 * shared pagetables that we're the only user of and can thus
	 * treat as private.
	 */
	u64 modified_start;
	/** @modified_end: Walk range start, modified like @modified_start. */
	u64 modified_end;

	/* Output */
	/* @wupd: Structure to track the page-table updates we're building */
	struct xe_walk_update wupd;
};

/*
 * Check whether this range is the only one populating this pagetable,
 * and in that case, update the walk range checks so that higher levels don't
 * view us as a shared pagetable.
 */
static bool xe_pt_check_kill(u64 addr, u64 next, unsigned int level,
			     const struct xe_pt *child,
			     enum page_walk_action *action,
			     struct drm_pt_walk *walk)
{
	struct xe_pt_stage_unbind_walk *xe_walk =
		container_of(walk, typeof(*xe_walk), drm);
	unsigned int shift = walk->shifts[level];
	u64 size = 1ull << shift;

	if (IS_ALIGNED(addr, size) && IS_ALIGNED(next, size) &&
	    ((next - addr) >> shift) == child->num_live) {
		u64 size = 1ull << walk->shifts[level + 1];

		*action = ACTION_CONTINUE;

		if (xe_walk->modified_start >= addr)
			xe_walk->modified_start = round_down(addr, size);
		if (xe_walk->modified_end <= next)
			xe_walk->modified_end = round_up(next, size);

		return true;
	}

	return false;
}

static int xe_pt_stage_unbind_entry(struct drm_pt *parent, pgoff_t offset,
				    unsigned int level, u64 addr, u64 next,
				    struct drm_pt **child,
				    enum page_walk_action *action,
				    struct drm_pt_walk *walk)
{
	struct xe_pt *xe_child = container_of(*child, typeof(*xe_child), drm);

	XE_BUG_ON(!*child);
	XE_BUG_ON(!level && xe_child->is_compact);

	xe_pt_check_kill(addr, next, level - 1, xe_child, action, walk);

	return 0;
}

static int
xe_pt_stage_unbind_post_descend(struct drm_pt *parent, pgoff_t offset,
				unsigned int level, u64 addr, u64 next,
				struct drm_pt **child,
				enum page_walk_action *action,
				struct drm_pt_walk *walk)
{
	struct xe_pt_stage_unbind_walk *xe_walk =
		container_of(walk, typeof(*xe_walk), drm);
	struct xe_pt *xe_child = container_of(*child, typeof(*xe_child), drm);
	pgoff_t end_offset;
	u64 size = 1ull << walk->shifts[--level];

	if (!IS_ALIGNED(addr, size))
		addr = xe_walk->modified_start;
	if (!IS_ALIGNED(next, size))
		next = xe_walk->modified_end;

	/* Parent == *child is the root pt. Don't kill it. */
	if (parent != *child &&
	    xe_pt_check_kill(addr, next, level, xe_child, action, walk))
		return 0;

	if (!xe_pt_nonshared_offsets(addr, next, level, walk, action, &offset,
				     &end_offset))
		return 0;

	(void)xe_pt_new_shared(&xe_walk->wupd, xe_child, offset, false);
	xe_walk->wupd.updates[level].update->qwords = end_offset - offset;

	return 0;
}

static const struct drm_pt_walk_ops xe_pt_stage_unbind_ops = {
	.pt_entry = xe_pt_stage_unbind_entry,
	.pt_post_descend = xe_pt_stage_unbind_post_descend,
};

/**
 * xe_pt_stage_unbind - Build page-table update structures for an unbind
 * operation
 * @gt: The gt we're unbinding for.
 * @vma: The vma we're unbinding.
 * @entries: Caller-provided storage for the update structures.
 *
 * Builds page-table update structures for an unbind operation. The function
 * will attempt to remove all page-tables that we're the only user
 * of, and for that to work, the unbind operation must be committed in the
 * same critical section that blocks racing binds to the same page-table tree.
 *
 * Return: The number of entries used.
 */
static unsigned int xe_pt_stage_unbind(struct xe_gt *gt, struct xe_vma *vma,
				       struct xe_vm_pgtable_update *entries)
{
	struct xe_pt_stage_unbind_walk xe_walk = {
		.drm = {
			.ops = &xe_pt_stage_unbind_ops,
			.shifts = xe_normal_pt_shifts,
			.max_level = XE_PT_HIGHEST_LEVEL,
		},
		.gt = gt,
		.modified_start = vma->start,
		.modified_end = vma->end + 1,
		.wupd.entries = entries,
	};
	struct xe_pt *pt = vma->vm->pt_root[gt->info.id];

	(void)drm_pt_walk_shared(&pt->drm, pt->level, vma->start, vma->end + 1,
				 &xe_walk.drm);

	return xe_walk.wupd.num_used_entries;
}
