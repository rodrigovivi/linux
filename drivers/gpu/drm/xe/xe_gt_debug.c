// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <drm/drm_print.h>

#include "regs/xe_gt_regs.h"
#include "xe_device.h"
#include "xe_eudebug.h"
#include "xe_force_wake.h"
#include "xe_gt.h"
#include "xe_gt_topology.h"
#include "xe_gt_debug.h"
#include "xe_gt_mcr.h"
#include "xe_pm.h"
#include "xe_macros.h"

unsigned int xe_gt_eu_att_regs(struct xe_gt *gt)
{
	return (GRAPHICS_VERx100(gt_to_xe(gt)) >= 3000) ? 2u : 1u;
}

int xe_gt_foreach_dss_group_instance(struct xe_gt *gt,
				     int (*fn)(struct xe_gt *gt,
					       void *data,
					       u16 group,
					       u16 instance,
					       bool present),
				     void *data)
{
	struct xe_device *xe = gt_to_xe(gt);
	const enum xe_force_wake_domains fw_domains = XE_FW_GT;
	xe_dss_mask_t dss_mask;
	unsigned int dss, fw_ref;
	u16 group, instance;
	int ret = 0;

	if (!xe_pm_runtime_get_if_active(xe))
		return -EIO;

	fw_ref = xe_force_wake_get(gt_to_fw(gt), fw_domains);
	if (!fw_ref) {
		ret = -ETIMEDOUT;
		goto out_pm;
	}

	bitmap_or(dss_mask, gt->fuse_topo.g_dss_mask, gt->fuse_topo.c_dss_mask,
		  XE_MAX_DSS_FUSE_BITS);

	/* If by any chance dss_mask is all 0s */
	if (!bitmap_weight(dss_mask, XE_MAX_DSS_FUSE_BITS)) {
		ret = -EINVAL;
		goto out_fw;
	}

	/*
	 * Note: This removes terminating zeros when last dss is fused out!
	 * In order bitmask to be exactly the same as on with i915 we would
	 * need to figure out max dss for given platform, most probably by
	 * querying hwconfig
	 */

	for (dss = 0;
	     dss <= find_last_bit(dss_mask, XE_MAX_DSS_FUSE_BITS);
	     dss++) {
		xe_gt_mcr_get_dss_steering(gt, dss, &group, &instance);

		ret = fn(gt, data, group, instance, test_bit(dss, dss_mask));
		if (ret)
			break;
	}

out_fw:
	xe_force_wake_put(gt_to_fw(gt), fw_ref);
out_pm:
	xe_pm_runtime_put(xe);

	return ret;
}

#define MAX_EUS_PER_ROW 4u
#define MAX_THREADS 8u

/**
 * xe_gt_eu_attention_bitmap_size - query size of the attention bitmask
 *
 * @gt: pointer to struct xe_gt
 *
 * The bitmask is u32 sized hardware register content, one bit per thread,
 * in native endianness. It is a byte array rather than a kernel bitmap,
 * and its size is expressed in bytes.
 *
 * Return: size in bytes, always a multiple of sizeof(u32).
 */
int xe_gt_eu_attention_bitmap_size(struct xe_gt *gt)
{
	xe_dss_mask_t dss_mask;

	/*
	 * The readers move through the buffer one u32 register at a time, so
	 * every dss/register/row has to contribute a whole number of them.
	 * Everything else in the size below is integer multiplication, which
	 * makes the total u32 aligned by construction rather than by luck.
	 */
	BUILD_BUG_ON(MAX_THREADS * MAX_EUS_PER_ROW / 8 != sizeof(u32));

	bitmap_or(dss_mask, gt->fuse_topo.c_dss_mask,
		  gt->fuse_topo.g_dss_mask, XE_MAX_DSS_FUSE_BITS);

	if (XE_WARN_ON(bitmap_empty(dss_mask, XE_MAX_DSS_FUSE_BITS)))
		return 0;

	return (find_last_bit(dss_mask, XE_MAX_DSS_FUSE_BITS) + 1) *
		XE_GT_EU_ATT_ROWS * xe_gt_eu_att_regs(gt) * MAX_THREADS *
		MAX_EUS_PER_ROW / 8;
}

struct attn_read_iter {
	struct xe_gt *gt;
	unsigned int i;
	unsigned int size;
	u8 *bits;
};

static int read_eu_attentions_mcr(struct xe_gt *gt, void *data,
				  u16 group, u16 instance, bool present)
{
	struct attn_read_iter * const iter = data;
	unsigned int reg, row;

	for (reg = 0; reg < xe_gt_eu_att_regs(gt); reg++) {
		for (row = 0; row < XE_GT_EU_ATT_ROWS; row++) {
			u32 val;

			if (iter->i + sizeof(val) > iter->size)
				return 0;

			if (present)
				val = xe_gt_mcr_unicast_read(gt, EU_ATT(reg, row), group, instance);
			else
				val = 0;

			memcpy(&iter->bits[iter->i], &val, sizeof(val));
			iter->i += sizeof(val);
		}
	}

	return 0;
}

/**
 * xe_gt_eu_attention_bitmap - query host attention
 *
 * @gt: pointer to struct xe_gt
 * @bits: buffer to fill with the attention bits, one u32 per dss, register
 *	and row, in the order xe_gt_foreach_dss_group_instance() walks them
 * @bitmap_size: size of @bits in bytes, a multiple of sizeof(u32)
 *
 * Return: 0 on success, negative otherwise.
 */
int xe_gt_eu_attention_bitmap(struct xe_gt *gt, u8 *bits,
			      unsigned int bitmap_size)
{
	struct attn_read_iter iter = {
		.gt = gt,
		.i = 0,
		.size = bitmap_size,
		.bits = bits
	};

	/* One u32 of attention state per dss, register and row */
	BUILD_BUG_ON(MAX_THREADS * MAX_EUS_PER_ROW / 8 != sizeof(u32));

	if (XE_WARN_ON(!IS_ALIGNED(bitmap_size, sizeof(u32))))
		return -EINVAL;

	return xe_gt_foreach_dss_group_instance(gt, read_eu_attentions_mcr, &iter);
}

static int eu_attention_show(struct seq_file *m, void *data)
{
	struct drm_printer p = drm_seq_file_printer(m);
	struct xe_gt *gt = m->private;
	unsigned int dss_size, size, i, j;
	u8 *bits;
	int ret;

	dss_size = xe_gt_eu_att_regs(gt) * XE_GT_EU_ATT_ROWS * sizeof(u32);
	size = xe_gt_eu_attention_bitmap_size(gt);
	if (!size)
		return -ENODEV;

	bits = kzalloc(size, GFP_KERNEL);
	if (!bits)
		return -ENOMEM;

	scoped_guard(xe_pm_runtime, gt_to_xe(gt))
		ret = xe_gt_eu_attention_bitmap(gt, bits, size);
	if (ret)
		goto out;

	drm_printf(&p, "Threads in attention: %zu\n", memweight(bits, size));

	for (i = 0; i + dss_size <= size; i += dss_size) {
		if (!memchr_inv(&bits[i], 0, dss_size))
			continue;

		drm_printf(&p, "DSS%u:", i / dss_size);
		for (j = 0; j < dss_size; j += sizeof(u32)) {
			u32 val;

			memcpy(&val, &bits[i + j], sizeof(val));
			drm_printf(&p, " 0x%08x", val);
		}
		drm_puts(&p, "\n");
	}

out:
	kfree(bits);
	return ret;
}
DEFINE_SHOW_ATTRIBUTE(eu_attention);

/**
 * xe_gt_debug_debugfs_register - Add the EU attention debugfs file
 * @gt: the &xe_gt
 * @root: the GT debugfs directory
 */
void xe_gt_debug_debugfs_register(struct xe_gt *gt, struct dentry *root)
{
	if (!xe_eudebug_is_supported(gt_to_xe(gt)) || !xe_gt_is_main_type(gt))
		return;

	debugfs_create_file("eu_attention", 0400, root, gt, &eu_attention_fops);
}
