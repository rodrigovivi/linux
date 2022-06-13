/*
   w
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GT_TYPES_H_
#define _XE_GT_TYPES_H_

#include "xe_hw_engine_types.h"
#include "xe_hw_fence_types.h"
#include "xe_sa_types.h"
#include "xe_uc_types.h"

struct xe_engine_ops;
struct xe_force_wake;
struct xe_ggtt;
struct xe_migrate;
struct xe_ring_ops;
struct xe_ttm_gtt_mgr;
struct xe_ttm_vram_mgr;

/**
 * struct xe_gt - Top level struct of a graphics tile
 *
 * A graphics tile may be a physical split (duplicate pieces of silicon,
 * different GGTT + VRAM) or a virtual split (shared GGTT + VRAM). Either way
 * this structure encapsulates of everything a GT is (MMIO, VRAM, memory
 * management, microcontrols, and a hardware set of engines).
 */
struct xe_gt {
	/** @info: GT info */
	struct {
		/** @id: id of GT */
		u8 id;
		/** @engine_mask: mask of engines present on GT */
		u64 engine_mask;
		/** @enable_guc: GuC submission enabled */
		bool enable_guc;
	} info;

	/**
	 * @mmio: mmio info for GT, can be subset of the global device mmio
	 * space
	 */
	struct {
		/** @size: size of MMIO space on GT */
		size_t size;
		/** @regs: pointer to MMIO space on GT */
		void *regs;
		/**
		 * @fw: force wake for GT, making a pointer to future proof
		 * against virtual GTs sharing FW domains
		 */
		struct xe_force_wake *fw;
	} mmio;

	/**
	 * @mem: memory management info for GT, multiple GTs can point to same
	 * objects (virtual split)
	 */
	struct {
		/**
		 * @vram: VRAM info for GT, multiple GTs can point to same info
		 * (virtual split)
		 */
		struct {
			/** @io_start: start address of VRAM */
			resource_size_t io_start;
			/** @size: size of VRAM */
			resource_size_t size;
			/** @mapping: pointer to VRAM mappable space */
			void *__iomem mapping;
		} vram;
		/** @vram_mgr: VRAM TTM manager */
		struct xe_ttm_vram_mgr *vram_mgr;
		/** @gtt_mr: GTT TTM manager */
		struct xe_ttm_gtt_mgr *gtt_mgr;
		/** @ggtt: Global graphics translation table */
		struct xe_ggtt *ggtt;
	} mem;

	/** @reset: state for GT resets */
	struct {
		/**
		 * @worker: work so GT resets can done async allowing to reset
		 * code to safely flush all code paths
		 */
		struct work_struct worker;
	} reset;

	/** @ordered_wq: used to serialize GT resets and TDRs */
	struct workqueue_struct *ordered_wq;

	/** @uc: micro controllers on the GT */
	struct xe_uc uc;

	/** @engine_ops: submission backend engine operations */
	const struct xe_engine_ops *engine_ops;

	/**
	 * @ring_ops: ring operations for this hw engine (1 per engine class)
	 */
	const struct xe_ring_ops *ring_ops[XE_ENGINE_CLASS_MAX];

	/** @fence_irq: fence IRQs (1 per engine class) */
	struct xe_hw_fence_irq fence_irq[XE_ENGINE_CLASS_MAX];

	/** @hw_engines: hardware engines on the GT */
	struct xe_hw_engine hw_engines[XE_NUM_HW_ENGINES];

	/** kernel_bb_pool: Pool from which batchbuffers are allocated */
	struct xe_sa_manager kernel_bb_pool;

	/** @migrate: Migration helper for vram blits and clearing */
	struct xe_migrate *migrate;
};

#endif	/* _XE_GT_TYPES_H_ */
