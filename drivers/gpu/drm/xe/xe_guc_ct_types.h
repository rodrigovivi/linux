/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GUC_CT_TYPES_H_
#define _XE_GUC_CT_TYPES_H_

#include <linux/dma-buf-map.h>
#include <linux/interrupt.h>
#include <linux/spinlock_types.h>
#include <linux/wait.h>
#include <linux/xarray.h>

struct xe_bo;

/**
 * struct guc_ctb - GuC command transport buffer (CTB)
 */
struct guc_ctb {
	/** @desc: dma buffer map for CTB descriptor */
	struct dma_buf_map desc;
	/** @cmds: dma buffer map for CTB commands */
	struct dma_buf_map cmds;
	/** @size: size of CTB commands (DW) */
	u32 size;
	/** @resv_space: reserved space of CTB commands (DW) */
	u32 resv_space;
	/** @head: head of CTB commands (DW) */
	u32 head;
	/** @tail: tail of CTB commands (DW) */
	u32 tail;
	/** @space: space in CTB commands (DW) */
	u32 space;
	/** @broken: channel broken */
	bool broken;
};

/**
 * struct xe_guc_ct - GuC command transport (CT) layer
 *
 * Includes a pair of CT buffers for bi-directional communication and tracking
 * for the H2G and G2H requests sent and received through the buffers.
 */
struct xe_guc_ct {
	/** @bo: XE BO for CT */
	struct xe_bo *bo;
	/** @lock: protects everything in CT layer */
	struct mutex lock;
	/** @ctbs: buffers for sending and receiving commands */
	struct {
		/** @send: Host to GuC (H2G, send) channel */
		struct guc_ctb h2g;
		/** @recv: GuC to Host (G2H, receive) channel */
		struct guc_ctb g2h;
	} ctbs;
	/** @g2h_outstanding: number of outstanding G2H */
	u32 g2h_outstanding;
	/** @g2h_worker: worker to process G2H messages */
	struct work_struct g2h_worker;
	/** @enabled: CT enabled */
	bool enabled;
	/** @fence_lock: G2H fences */
	spinlock_t fence_lock;
	/** @fence: G2H fence seqno - 32 bits used by dma fence, 16 used by CT */
	u32 fence_seqno;
	/** @fence_context: context for G2H fence */
	u64 fence_context;
	/** @fence_lookup: G2H fence lookup */
	struct xarray fence_lookup;
	/** @wq: wait queue used for reliable CT sends and freeing G2H credits */
	wait_queue_head_t wq;
};

#endif	/* _XE_GUC_CT_TYPES_H_ */
