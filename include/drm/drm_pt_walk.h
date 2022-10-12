/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright © 2022 Intel Corporation
 */
#ifndef __DRM_PT_WALK__
#define __DRM_PT_WALK__

#include <linux/pagewalk.h>
#include <linux/types.h>

struct drm_pt_dir;

/**
 * struct drm_pt - base class for driver pagetable subclassing.
 * @dir: Pointer to an array of children if any.
 *
 * Drivers could subclass this, and if it's a page-directory, typically
 * embed the drm_pt_dir::entries array in the same allocation.
 */
struct drm_pt {
	struct drm_pt_dir *dir;
};

/**
 * struct drm_pt_dir - page directory structure
 * @entries: Array holding page directory children.
 *
 * It is the responsibility of the user to ensure @entries is
 * correctly sized.
 */
struct drm_pt_dir {
	struct drm_pt *entries[0];
};

/**
 * struct drm_pt_walk - Embeddable struct for walk parameters
 */
struct drm_pt_walk {
	/** @ops: The walk ops used for the pagewalk */
	const struct drm_pt_walk_ops *ops;
	/**
	 * @shifts: Array of page-table entry shifts used for the
	 * different levels, starting out with the leaf level 0
	 * page-shift as the first entry. It's legal for this pointer to be
	 * changed during the walk.
	 */
	const u64 *shifts;
	/** @max_level: Highest populated level in @sizes */
	unsigned int max_level;
	/**
	 * @shared_pt_mode: Whether to skip all entries that are private
	 * to the address range and called only for entries that are
	 * shared with other address ranges. Such entries are referred to
	 * as shared pagetables.
	 */
	bool shared_pt_mode;
};

/**
 * typedef drm_pt_entry_fn - gpu page-table-walk callback-function
 * @parent: The parent page.table.
 * @offset: The offset (number of entries) into the page table.
 * @level: The level of @parent.
 * @addr: The virtual address.
 * @next: The virtual address for the next call, or end address.
 * @child: Pointer to pointer to child page-table at this @offset. The
 * function may modify the value pointed to if, for example, allocating a
 * child page table.
 * @action: The walk action to take upon return. See <linux/pagewalk.h>.
 * @walk: The walk parameters.
 */
typedef int (*drm_pt_entry_fn)(struct drm_pt *parent, pgoff_t offset,
			       unsigned int level, u64 addr, u64 next,
			       struct drm_pt **child,
			       enum page_walk_action *action,
			       struct drm_pt_walk *walk);

/**
 * struct drm_pt_walk_ops - Walk callbacks.
 */
struct drm_pt_walk_ops {
	/**
	 * @pt_entry: Callback to be called for each page table entry prior
	 * to descending to the next level. The returned value of the action
	 * function parameter is honored.
	 */
	drm_pt_entry_fn pt_entry;
	/**
	 * @pt_post_descend: Callback to be called for each page table entry
	 * after return from descending to the next level. The returned value
	 * of the action function parameter is ignored.
	 */
	drm_pt_entry_fn pt_post_descend;
};

int drm_pt_walk_range(struct drm_pt *parent, unsigned int level,
		      u64 addr, u64 end, struct drm_pt_walk *walk);

int drm_pt_walk_shared(struct drm_pt *parent, unsigned int level,
		       u64 addr, u64 end, struct drm_pt_walk *walk);

/**
 * drm_pt_covers - Whether the address range covers an entire entry in @level
 * @addr: Start of the range.
 * @end: End of range + 1.
 * @level: Page table level.
 * @walk: Page table walk info.
 *
 * This function is a helper to aid in determining whether a leaf page table
 * entry can be inserted at this @level.
 *
 * Return: Whether the range provided covers exactly an entry at this level.
 */
static inline bool drm_pt_covers(u64 addr, u64 end, unsigned int level,
				 const struct drm_pt_walk *walk)
{
	u64 pt_size = 1ull << walk->shifts[level];

	return end - addr == pt_size && IS_ALIGNED(addr, pt_size);
}

/**
 * drm_pt_num_entries: Number of page-table entries of a given range at this
 * level
 * @addr: Start address.
 * @end: End address.
 * @level: Page table level.
 * @walk: Walk info.
 *
 * Return: The number of page table entries at this level between @start and
 * @end.
 */
static inline pgoff_t
drm_pt_num_entries(u64 addr, u64 end, unsigned int level,
		   const struct drm_pt_walk *walk)
{
	u64 pt_size = 1ull << walk->shifts[level];

	return (round_up(end, pt_size) - round_down(addr, pt_size)) >>
		walk->shifts[level];
}

/**
 * drm_pt_offset: Offset of the page-table entry for a given address.
 * @addr: The address.
 * @level: Page table level.
 * @walk: Walk info.
 *
 * Return: The page table entry offset for the given address in a
 * page table with size indicated by @level.
 */
static inline pgoff_t
drm_pt_offset(u64 addr, unsigned int level, const struct drm_pt_walk *walk)
{
	if (level < walk->max_level)
		addr &= ((1ull << walk->shifts[level + 1]) - 1);

	return addr >> walk->shifts[level];
}

#endif
