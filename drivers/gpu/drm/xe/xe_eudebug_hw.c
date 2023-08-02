// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023-2025 Intel Corporation
 */

#include "xe_eudebug_hw.h"

#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <generated/xe_wa_oob.h>

#include "regs/xe_gt_regs.h"
#include "regs/xe_engine_regs.h"

#include "xe_eudebug.h"
#include "xe_eudebug_types.h"
#include "xe_exec_queue.h"
#include "xe_exec_queue_types.h"
#include "xe_force_wake.h"
#include "xe_gt.h"
#include "xe_gt_debug.h"
#include "xe_gt_mcr.h"
#include "xe_hw_engine.h"
#include "xe_lrc.h"
#include "xe_macros.h"
#include "xe_mmio.h"
#include "xe_pm.h"
#include "xe_reg_sr.h"
#include "xe_rtp.h"
#include "xe_wa.h"

static void add_sr_entry(struct xe_hw_engine *hwe,
			 struct xe_reg_mcr mcr_reg,
			 u32 mask, bool enable)
{
	const struct xe_reg_sr_entry sr_entry = {
		.reg = mcr_reg.__reg,
		.clr_bits = mask,
		.set_bits = enable ? mask : 0,
		.read_mask = mask,
	};

	xe_reg_sr_add(&hwe->reg_sr, &sr_entry, hwe->gt, true);
}

void xe_eudebug_init_hw_engine(struct xe_hw_engine *hwe, bool enable)
{
	struct xe_gt *gt = hwe->gt;
	struct xe_device *xe = gt_to_xe(gt);

	if (!xe_rtp_match_first_render_or_compute(xe, gt, hwe))
		return;

	if (XE_GT_WA(gt, 18022722726))
		add_sr_entry(hwe, ROW_CHICKEN,
			     STALL_DOP_GATING_DISABLE, enable);

	if (XE_GT_WA(gt, 14015474168))
		add_sr_entry(hwe, ROW_CHICKEN2,
			     XEHPC_DISABLE_BTB,
			     enable);

	if (xe->info.graphics_verx100 >= 1200)
		add_sr_entry(hwe, TD_CTL,
			     TD_CTL_BREAKPOINT_ENABLE |
			     TD_CTL_FORCE_THREAD_BREAKPOINT_ENABLE |
			     TD_CTL_FEH_AND_FEE_ENABLE,
			     enable);

	if (xe->info.graphics_verx100 >= 1250)
		add_sr_entry(hwe, TD_CTL,
			     TD_CTL_GLOBAL_DEBUG_ENABLE, enable);
}

static int read_hw_lrca_fw(struct xe_hw_engine *hwe, u32 *lrc_hw)
{
	u32 lrc_reg;

	lrc_reg = xe_hw_engine_mmio_read32(hwe, RING_CURRENT_LRCA(0));

	if (!(lrc_reg & CURRENT_LRCA_VALID))
		return -ENOENT;

	*lrc_hw = lrc_reg & GENMASK(31, 12);

	return 0;
}

static int read_hw_lrca(struct xe_hw_engine *hwe, u32 *lrc_hw)
{
	unsigned int fw_ref;
	int ret;

	fw_ref = xe_force_wake_get(gt_to_fw(hwe->gt), hwe->domain);
	if (!fw_ref)
		return -ETIMEDOUT;

	ret = read_hw_lrca_fw(hwe, lrc_hw);

	xe_force_wake_put(gt_to_fw(hwe->gt), fw_ref);

	return ret;
}

static bool lrca_equals(u32 a, u32 b)
{
	return (a & GENMASK(31, 12)) == (b & GENMASK(31, 12));
}

static int match_exec_queue_lrca(struct xe_exec_queue *q, u32 lrc_hw)
{
	int i;

	for (i = 0; i < q->width; i++)
		if (lrca_equals(lower_32_bits(xe_lrc_descriptor(q->lrc[i])), lrc_hw))
			return i;

	return -1;
}

static int rcu_debug1_engine_index(const struct xe_hw_engine * const hwe)
{
	if (hwe->class == XE_ENGINE_CLASS_RENDER) {
		XE_WARN_ON(hwe->instance);
		return 0;
	}

	XE_WARN_ON(hwe->instance > 3);

	return hwe->instance + 1;
}

static u32 engine_status_xe1(const struct xe_hw_engine * const hwe,
			     u32 rcu_debug1)
{
	const unsigned int first = 7;
	const unsigned int incr = 3;
	const unsigned int i = rcu_debug1_engine_index(hwe);
	const unsigned int shift = first + (i * incr);

	return (rcu_debug1 >> shift) & RCU_DEBUG_1_ENGINE_STATUS;
}

static u32 engine_status_xe2(const struct xe_hw_engine * const hwe,
			     u32 rcu_debug1)
{
	const unsigned int first = 7;
	const unsigned int incr = 4;
	const unsigned int i = rcu_debug1_engine_index(hwe);
	const unsigned int shift = first + (i * incr);

	return (rcu_debug1 >> shift) & RCU_DEBUG_1_ENGINE_STATUS;
}

static u32 engine_status_xe3(const struct xe_hw_engine * const hwe,
			     u32 rcu_debug1)
{
	const unsigned int first = 6;
	const unsigned int incr = 4;
	const unsigned int i = rcu_debug1_engine_index(hwe);
	const unsigned int shift = first + (i * incr);

	return (rcu_debug1 >> shift) & RCU_DEBUG_1_ENGINE_STATUS;
}

static u32 engine_status(const struct xe_hw_engine * const hwe,
			 u32 rcu_debug1)
{
	u32 status = 0;

	if (GRAPHICS_VER(gt_to_xe(hwe->gt)) < 20)
		status = engine_status_xe1(hwe, rcu_debug1);
	else if (GRAPHICS_VER(gt_to_xe(hwe->gt)) < 30)
		status = engine_status_xe2(hwe, rcu_debug1);
	else if (GRAPHICS_VER(gt_to_xe(hwe->gt)) < 35)
		status = engine_status_xe3(hwe, rcu_debug1);
	else
		XE_WARN_ON(GRAPHICS_VER(gt_to_xe(hwe->gt)));

	return status;
}

static bool engine_has_runalone_set(const struct xe_hw_engine * const hwe,
				    u32 rcu_debug1)
{
	return engine_status(hwe, rcu_debug1) & RCU_DEBUG_1_RUNALONE_ACTIVE;
}

static bool engine_has_context_set(const struct xe_hw_engine * const hwe,
				   u32 rcu_debug1)
{
	return engine_status(hwe, rcu_debug1) & RCU_DEBUG_1_CONTEXT_ACTIVE;
}

static struct xe_hw_engine *get_runalone_active_hw_engine(struct xe_gt *gt)
{
	struct xe_hw_engine *hwe, *first = NULL;
	unsigned int num_active, id, fw_ref;
	u32 val;

	fw_ref = xe_force_wake_get(gt_to_fw(gt), XE_FW_GT);
	if (!fw_ref) {
		drm_dbg(&gt_to_xe(gt)->drm, "eudbg: runalone failed to get force wake\n");
		return ERR_PTR(-ETIMEDOUT);
	}

	val = xe_mmio_read32(&gt->mmio, RCU_DEBUG_1);
	xe_force_wake_put(gt_to_fw(gt), fw_ref);

	drm_dbg(&gt_to_xe(gt)->drm, "eudbg: runalone RCU_DEBUG_1 = 0x%08x\n", val);

	/*
	 * An all-ones read means the mmio access did not land - the device is
	 * gone or mid-reset. Decoding it would flag every engine as runalone
	 * and active and spuriously report several active engines, so bail.
	 */
	if (val == ~0u)
		return ERR_PTR(-EIO);

	num_active = 0;
	xe_eudebug_for_each_hw_engine(hwe, gt, id) {
		bool runalone, ctx;

		runalone = engine_has_runalone_set(hwe, val);
		ctx = engine_has_context_set(hwe, val);

		drm_dbg(&gt_to_xe(gt)->drm, "eudbg: engine %s: runalone=%s, context=%s\n",
			hwe->name, runalone ? "active" : "inactive",
			ctx ? "active" : "inactive");

		/*
		 * On discrete parts the context status can read idle while
		 * the context has raised attention, so do not rely on the
		 * active bit there.
		 */
		if (IS_DGFX(gt_to_xe(gt)))
			ctx = true;

		if (runalone && ctx) {
			num_active++;

			drm_dbg(&gt_to_xe(gt)->drm, "eudbg: runalone engine %s %s\n",
				hwe->name, first ? "ignored" : "selected");
			if (!first)
				first = hwe;
		}
	}

	if (num_active > 1)
		drm_err(&gt_to_xe(gt)->drm, "eudbg: %u runalone engines active!\n",
			num_active);

	if (!first)
		return ERR_PTR(-ENOENT);

	return first;
}

static struct xe_exec_queue *active_hwe_to_exec_queue(struct xe_hw_engine *hwe,
						      int *lrc_idx)
{
	struct xe_device *xe = gt_to_xe(hwe->gt);
	struct xe_gt *gt = hwe->gt;
	struct xe_exec_queue *q, *found = NULL;
	int idx, err;
	u32 lrc_hw;

	err = read_hw_lrca(hwe, &lrc_hw);
	if (err)
		return ERR_PTR(err);

	mutex_lock(&xe->eudebug.lock);
	list_for_each_entry(q, &xe->eudebug.targets, eudebug.target_link) {
		if (q->gt != gt)
			continue;

		if (q->class != hwe->class)
			continue;

		idx = match_exec_queue_lrca(q, lrc_hw);
		if (idx < 0)
			continue;

		found = xe_exec_queue_get_unless_zero(q);
		if (!found)
			continue;

		if (lrc_idx)
			*lrc_idx = idx;

		break;
	}
	mutex_unlock(&xe->eudebug.lock);

	if (!found)
		return ERR_PTR(-ENOENT);

	/*
	 * The context can switch out while we walk the target list, so
	 * both of these are ordinary outcomes rather than driver bugs.
	 */
	if (read_hw_lrca(hwe, &lrc_hw)) {
		xe_exec_queue_put(found);
		return ERR_PTR(-ENOENT);
	}

	if (match_exec_queue_lrca(found, lrc_hw) < 0) {
		xe_exec_queue_put(found);
		return ERR_PTR(-ENOENT);
	}

	return found;
}

struct xe_exec_queue *
xe_gt_runalone_active_queue_get(struct xe_gt *gt, struct xe_hw_engine **active_hwe,
				int *lrc_idx)
{
	struct xe_hw_engine *active;

	active = get_runalone_active_hw_engine(gt);
	if (IS_ERR(active))
		return ERR_CAST(active);

	if (active_hwe)
		*active_hwe = active;

	return active_hwe_to_exec_queue(active, lrc_idx);
}

static int check_lrc_sanity(const struct xe_exec_queue *q,
			    const struct xe_lrc *lrc,
			    const int lrc_idx)
{
	if (lrc_idx >= q->width)
		return -EINVAL;

	if (q->lrc[lrc_idx] != lrc)
		return -EINVAL;

	return 0;
}

/*
 * The active context was sampled before the bitmask was copied in from
 * userspace and before eudebug.lock was taken, and the hardware may have
 * switched since. EU_ATT and EU_ATT_CLR are gt wide, so check again just
 * before touching them, as xe_eu_control_interrupt_all() does for TD_CTL,
 * rather than report or clear attention of some other context.
 */
static int check_lrc_active(struct xe_hw_engine *hwe, struct xe_lrc *lrc)
{
	u32 lrc_hw;
	int err;

	err = read_hw_lrca(hwe, &lrc_hw);
	if (err)
		return err;

	if (!lrca_equals(lower_32_bits(xe_lrc_descriptor(lrc)), lrc_hw))
		return -EBUSY;

	return 0;
}

static int do_eu_control(struct xe_eudebug *d,
			 const struct drm_xe_eudebug_eu_control * const arg,
			 struct drm_xe_eudebug_eu_control __user * const user_ptr)
{
	void __user * const bitmask_ptr = u64_to_user_ptr(arg->bitmask_ptr);
	struct xe_device *xe = d->xe;
	struct xe_exec_queue *q, *active;
	struct xe_hw_engine *active_hwe;
	struct xe_lrc *lrc;
	unsigned int hw_attn_size, attn_size;
	u8 *bits = NULL;
	u64 seqno;
	int lrc_idx;
	int ret;

	if (xe_eudebug_detached(d))
		return -ENOTCONN;

	/* Accept only hardware reg granularity mask */
	if (XE_IOCTL_DBG(xe, !IS_ALIGNED(arg->bitmask_size, sizeof(u32))))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, arg->seqno))
		return -EINVAL;

	/* Handles are u32 internally, do not let the upper half alias them */
	if (XE_IOCTL_DBG(xe, arg->exec_queue_handle > U32_MAX ||
			 arg->lrc_handle > U32_MAX))
		return -EINVAL;

	q = xe_eudebug_exec_queue_get(d, arg->exec_queue_handle);
	if (XE_IOCTL_DBG(xe, !q))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, !xe_exec_queue_is_debuggable(q))) {
		ret = -EINVAL;
		goto queue_put;
	}

	lrc = xe_eudebug_find_lrc(d, arg->lrc_handle);
	if (XE_IOCTL_DBG(xe, !lrc)) {
		ret = -EINVAL;
		goto queue_put;
	}

	if (!xe_pm_runtime_get_if_active(xe)) {
		ret = -EIO;
		goto queue_put;
	}

	active = xe_gt_runalone_active_queue_get(q->gt, &active_hwe, &lrc_idx);
	if (XE_IOCTL_DBG(xe, IS_ERR(active))) {
		ret = PTR_ERR(active);
		goto out_pm;
	}

	if (XE_IOCTL_DBG(xe, q != active)) {
		ret = -EBUSY;
		goto active_put;
	}

	ret = check_lrc_sanity(q, lrc, lrc_idx);
	if (XE_IOCTL_DBG(xe, ret))
		goto active_put;

	hw_attn_size = xe_gt_eu_attention_bitmap_size(q->gt);
	attn_size = arg->bitmask_size;

	if (attn_size > hw_attn_size)
		attn_size = hw_attn_size;

	/*
	 * Keep the zero size case without a buffer: memdup_user() would
	 * hand back a ZERO_SIZE_PTR, and the copy back to userspace below
	 * expects NULL when there is nothing to copy.
	 */
	if (attn_size > 0) {
		bits = memdup_user(bitmask_ptr, attn_size);
		if (IS_ERR(bits)) {
			ret = PTR_ERR(bits);
			bits = NULL;
			goto active_put;
		}
	}

	ret = -EINVAL;
	mutex_lock(&xe->eudebug.lock);

	switch (arg->cmd) {
	case DRM_XE_EUDEBUG_EU_CONTROL_CMD_INTERRUPT_ALL:
		/* Make sure we dont promise anything but interrupting all */
		if (!attn_size)
			ret = d->ops->interrupt_all(d, active, active_hwe, lrc);
		else
			ret = -EINVAL;
		break;
	case DRM_XE_EUDEBUG_EU_CONTROL_CMD_STOPPED:
		ret = check_lrc_active(active_hwe, lrc);
		if (!ret)
			ret = d->ops->stopped(d, active, lrc, bits, attn_size);
		break;
	case DRM_XE_EUDEBUG_EU_CONTROL_CMD_RESUME:
		ret = check_lrc_active(active_hwe, lrc);
		if (!ret)
			ret = d->ops->resume(d, active, lrc, bits, attn_size);
		break;
	default:
		break;
	}

	if (ret == 0)
		seqno = atomic64_inc_return(&d->events.seqno);

	mutex_unlock(&xe->eudebug.lock);

	if (ret)
		goto out_free;

	if (put_user(seqno, &user_ptr->seqno)) {
		ret = -EFAULT;
		goto out_free;
	}

	if (copy_to_user(bitmask_ptr, bits, attn_size)) {
		ret = -EFAULT;
		goto out_free;
	}

	if (hw_attn_size != arg->bitmask_size)
		if (put_user(hw_attn_size, &user_ptr->bitmask_size))
			ret = -EFAULT;

out_free:
	kfree(bits);
active_put:
	xe_exec_queue_put(active);
out_pm:
	xe_pm_runtime_put(xe);
queue_put:
	xe_exec_queue_put(q);

	return ret;
}

static int xe_eu_control_interrupt_all(struct xe_eudebug *d,
				       struct xe_exec_queue *active,
				       struct xe_hw_engine *hwe,
				       struct xe_lrc *lrc)
{
	unsigned int fw_ref = 0;
	u32 lrc_hw, lrc_hw_post;
	u32 td_ctl;
	int err = -EINVAL;

	fw_ref = xe_force_wake_get(gt_to_fw(active->gt), hwe->domain);
	if (!fw_ref) {
		err = -ETIMEDOUT;
		goto out;
	}

	/* Additional check just before issuing MMIO writes */
	err = read_hw_lrca_fw(hwe, &lrc_hw);
	if (err)
		goto out;

	if (!lrca_equals(lower_32_bits(xe_lrc_descriptor(lrc)), lrc_hw)) {
		err = -EBUSY;
		goto out;
	}

	td_ctl = xe_gt_mcr_unicast_read_any(active->gt, TD_CTL);

	/* Halt on next thread dispatch */
	if (!(td_ctl & TD_CTL_FORCE_EXTERNAL_HALT))
		xe_gt_mcr_multicast_write(active->gt, TD_CTL,
					  td_ctl | TD_CTL_FORCE_EXTERNAL_HALT);
	else
		eu_warn(d, "TD_CTL force external halt bit already set!\n");

	/*
	 * The sleep is needed because some interrupts are ignored
	 * by the HW, hence we allow the HW some time to acknowledge
	 * that.
	 */
	usleep_range(100, 110);

	/* Halt regardless of thread dependencies */
	if (!(td_ctl & TD_CTL_FORCE_EXCEPTION))
		xe_gt_mcr_multicast_write(active->gt, TD_CTL,
					  td_ctl | TD_CTL_FORCE_EXCEPTION |
					  TD_CTL_FORCE_EXTERNAL_HALT);
	else
		eu_warn(d, "TD_CTL force exception bit already set!\n");

	usleep_range(100, 110);

	xe_gt_mcr_multicast_write(active->gt, TD_CTL, td_ctl &
				  ~(TD_CTL_FORCE_EXTERNAL_HALT | TD_CTL_FORCE_EXCEPTION));

	/*
	 * In case of stopping wrong ctx emit warning.
	 * Nothing else we can do for now.
	 */
	if (!read_hw_lrca_fw(hwe, &lrc_hw_post) &&
	    !lrca_equals(lrc_hw, lrc_hw_post))
		eu_warn(d, "interrupted wrong context 0x%08x, wanted 0x%08x\n",
			lrc_hw_post, lrc_hw);

out:
	if (fw_ref)
		xe_force_wake_put(gt_to_fw(active->gt), fw_ref);

	return err;
}

struct ss_iter {
	struct xe_eudebug *debugger;
	unsigned int i;

	unsigned int size;
	u8 *bits;
};

static int check_attn_mcr(struct xe_gt *gt, void *data,
			  u16 group, u16 instance, bool present)
{
	struct ss_iter *iter = data;
	struct xe_eudebug *d = iter->debugger;
	unsigned int reg, row;

	for (reg = 0; reg < xe_gt_eu_att_regs(gt); reg++) {
		for (row = 0; row < XE_GT_EU_ATT_ROWS; row++) {
			u32 val, cur = 0;

			if (iter->i + sizeof(val) > iter->size)
				return 0;

			memcpy(&val, &iter->bits[iter->i], sizeof(val));
			iter->i += sizeof(val);

			if (present)
				cur = xe_gt_mcr_unicast_read(gt, EU_ATT(reg, row), group, instance);

			if ((val | cur) != cur) {
				eu_dbg(d,
				       "WRONG CLEAR (%u:%u:%u:%u) EU_ATT_CLR: 0x%08x; EU_ATT: 0x%08x\n",
				       group, instance, reg, row, val, cur);
				return -EINVAL;
			}
		}
	}

	return 0;
}

static int clear_attn_mcr(struct xe_gt *gt, void *data,
			  u16 group, u16 instance, bool present)
{
	struct ss_iter *iter = data;
	struct xe_eudebug *d = iter->debugger;
	unsigned int reg, row;

	for (reg = 0; reg < xe_gt_eu_att_regs(gt); reg++) {
		for (row = 0; row < XE_GT_EU_ATT_ROWS; row++) {
			u32 val;

			if (iter->i + sizeof(val) > iter->size)
				return 0;

			memcpy(&val, &iter->bits[iter->i], sizeof(val));
			iter->i += sizeof(val);

			if (!val)
				continue;

			if (present) {
				xe_gt_mcr_unicast_write(gt, EU_ATT_CLR(reg, row), val,
							group, instance);

				eu_dbg(d,
				       "EU_ATT_CLR: (%u:%u:%u:%u): 0x%08x\n",
				       group, instance, reg, row, val);
			} else {
				eu_dbg(d,
				       "EU_ATT_CLR: (%u:%u:%u:%u): 0x%08x to fused off dss\n",
				       group, instance, reg, row, val);
			}
		}
	}

	return 0;
}

static int xe_eu_control_resume(struct xe_eudebug *d,
				struct xe_exec_queue *active,
				struct xe_lrc *lrc,
				u8 *bits, unsigned int bitmask_size)
{
	struct ss_iter iter = {
		.debugger = d,
		.i = 0,
		.size = bitmask_size,
		.bits = bits
	};

	/*
	 * hsdes: 18021122357
	 * We need to avoid clearing attention bits that are not set
	 * in order to avoid the EOT hang on PVC.
	 */
	if (GRAPHICS_VERx100(d->xe) == 1260) {
		int err;

		err = xe_gt_foreach_dss_group_instance(active->gt, check_attn_mcr, &iter);
		if (err)
			return err;

		iter.i = 0;
	}

	return xe_gt_foreach_dss_group_instance(active->gt, clear_attn_mcr, &iter);
}

static int xe_eu_control_stopped(struct xe_eudebug *d,
				 struct xe_exec_queue *active,
				 struct xe_lrc *lrc,
				 u8 *bits, unsigned int bitmask_size)
{
	return xe_gt_eu_attention_bitmap(active->gt, bits, bitmask_size);
}

static const struct xe_eudebug_eu_control_ops eu_control = {
	.interrupt_all = xe_eu_control_interrupt_all,
	.stopped = xe_eu_control_stopped,
	.resume = xe_eu_control_resume,
};

void xe_eudebug_hw_init(struct xe_eudebug *d)
{
	d->ops = &eu_control;
}

long xe_eudebug_eu_control(struct xe_eudebug *d, const u64 arg)
{
	struct drm_xe_eudebug_eu_control __user * const user_ptr =
		u64_to_user_ptr(arg);
	struct drm_xe_eudebug_eu_control user_arg;
	struct xe_device *xe = d->xe;
	int ret;

	BUILD_BUG_ON(!(_IOC_DIR(DRM_XE_EUDEBUG_IOCTL_EU_CONTROL) & _IOC_WRITE));
	BUILD_BUG_ON(!(_IOC_DIR(DRM_XE_EUDEBUG_IOCTL_EU_CONTROL) & _IOC_READ));
	BUILD_BUG_ON(_IOC_SIZE(DRM_XE_EUDEBUG_IOCTL_EU_CONTROL) != sizeof(user_arg));

	if (copy_from_user(&user_arg,
			   user_ptr,
			   sizeof(user_arg)))
		return -EFAULT;

	if (XE_IOCTL_DBG(xe, user_arg.flags))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, user_arg.reserved))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe,
			 user_arg.cmd != DRM_XE_EUDEBUG_EU_CONTROL_CMD_INTERRUPT_ALL &&
			 user_arg.cmd != DRM_XE_EUDEBUG_EU_CONTROL_CMD_STOPPED &&
			 user_arg.cmd != DRM_XE_EUDEBUG_EU_CONTROL_CMD_RESUME))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, IS_SRIOV_VF(xe)))
		return -EPERM;

	if (!access_ok(u64_to_user_ptr(user_arg.bitmask_ptr), user_arg.bitmask_size))
		return -EFAULT;

	eu_dbg(d,
	       "eu_control: cmd=%u, flags=0x%x, exec_queue_handle=%llu, bitmask_size=%u\n",
	       user_arg.cmd, user_arg.flags, user_arg.exec_queue_handle,
	       user_arg.bitmask_size);

	ret = do_eu_control(d, &user_arg, user_ptr);

	eu_dbg(d,
	       "eu_control: cmd=%u, flags=0x%x, exec_queue_handle=%llu, bitmask_size=%u ret=%d\n",
	       user_arg.cmd, user_arg.flags, user_arg.exec_queue_handle,
	       user_arg.bitmask_size, ret);

	return ret;
}
