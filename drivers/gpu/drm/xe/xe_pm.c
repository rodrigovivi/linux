// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_pm.h"

#include <linux/pm_runtime.h>

#include <drm/drm_managed.h>
#include <drm/ttm/ttm_placement.h>

#include "xe_bo.h"
#include "xe_bo_evict.h"
#include "xe_device.h"
#include "xe_device_sysfs.h"
#include "xe_display.h"
#include "xe_ggtt.h"
#include "xe_gt.h"
#include "xe_guc.h"
#include "xe_irq.h"
#include "xe_pcode.h"
#include "xe_wa.h"

/**
 * DOC: Xe Power Management
 *
 * Xe PM implements the main routines for both system level suspend states and
 * for the opportunistic runtime suspend states.
 *
 * System Level Suspend (S-States) - In general this is OS initiated suspend
 * driven by ACPI for achieving S0ix (a.k.a. S2idle, freeze), S3 (suspend to ram),
 * S4 (disk). The main functions here are `xe_pm_suspend` and `xe_pm_resume` that
 * are the main point for the suspend to and resume from these states.
 *
 * Runtime Suspend (D-States) - This is the opportunistic PCIe device low power
 * state D3. Xe PM component provides `xe_pm_runtime_suspend` and
 * `xe_pm_runtime_resume` systems that PCI subsystem will call before transition
 * to D3. Also, Xe PM provides get and put functions that Xe driver will use to
 * indicate activity. In order to avoid locking complications with the memory
 * management, whenever possible, these get and put functions needs to be called
 * from the higher/outer levels.
 *
 * The main cases that need to be protected from the outer levels are: IOCLT,
 * sysfs, debugfs, dma-buf sharing, GPU execution.
 *
 * PCI D3 is special and can mean D3hot, where Vcc power is on for keeping memory
 * alive and quicker low latency resume or D3Cold where Vcc power is off for
 * better power savings.
 * The Vcc control of PCI hierarchy can only be controlled at the PCI root port
 * level, while the device driver can be behind multiple bridges/switches and
 * paired with other devices. For this reason, the PCI subsystem cannot perform
 * the transition towards D3Cold. The lowest runtime PM possible from the PCI
 * subsystem is D3hot. Then, if all these paired devices in the same root port
 * are in D3hot, ACPI will assist here and run its _PR3 and _OFF methods to
 * perform the transition from D3hot to D3cold. Xe may disallow this transition
 * based on runtime conditions such as VRAM usage for a quick and low latency
 * resume for instance.
 *
 * Intel systems are capable of taking the system to S0ix when devices are on
 * D3hot through the runtime PM. This is also called as 'opportunistic-S0iX'.
 * But in this case, the `xe_pm_suspend` and `xe_pm_resume` won't be called for
 * S0ix.
 *
 * This component is no responsible for GT idleness (RC6) nor GT frequency
 * management (RPS).
 */


/**
 * xe_pm_suspend - Helper for System suspend, i.e. S0->S3 / S0->S2idle
 * @xe: xe device instance
 *
 * Return: 0 on success
 */
int xe_pm_suspend(struct xe_device *xe)
{
	struct xe_gt *gt;
	u8 id;
	int err;

	for_each_gt(gt, xe, id)
		xe_gt_suspend_prepare(gt);

	/* FIXME: Super racey... */
	err = xe_bo_evict_all(xe);
	if (err)
		return err;

	xe_display_pm_suspend(xe);

	for_each_gt(gt, xe, id) {
		err = xe_gt_suspend(gt);
		if (err) {
			xe_display_pm_resume(xe);
			return err;
		}
	}

	xe_irq_suspend(xe);

	xe_display_pm_suspend_late(xe);

	return 0;
}

/**
 * xe_pm_resume - Helper for System resume S3->S0 / S2idle->S0
 * @xe: xe device instance
 *
 * Return: 0 on success
 */
int xe_pm_resume(struct xe_device *xe)
{
	struct xe_tile *tile;
	struct xe_gt *gt;
	u8 id;
	int err;

	for_each_tile(tile, xe, id)
		xe_wa_apply_tile_workarounds(tile);

	for_each_gt(gt, xe, id) {
		err = xe_pcode_init(gt);
		if (err)
			return err;
	}

	xe_display_pm_resume_early(xe);

	/*
	 * This only restores pinned memory which is the memory required for the
	 * GT(s) to resume.
	 */
	err = xe_bo_restore_kernel(xe);
	if (err)
		return err;

	xe_irq_resume(xe);

	xe_display_pm_resume(xe);

	for_each_gt(gt, xe, id)
		xe_gt_resume(gt);

	err = xe_bo_restore_user(xe);
	if (err)
		return err;

	return 0;
}

static bool xe_pm_pci_d3cold_capable(struct pci_dev *pdev)
{
	struct pci_dev *root_pdev;

	root_pdev = pcie_find_root_port(pdev);
	if (!root_pdev)
		return false;

	/* D3Cold requires PME capability and _PR3 power resource */
	if (!pci_pme_capable(root_pdev, PCI_D3cold) || !pci_pr3_present(root_pdev))
		return false;

	return true;
}

static void xe_pm_runtime_init(struct xe_device *xe)
{
	struct device *dev = xe->drm.dev;

	/*
	 * Disable the system suspend direct complete optimization.
	 * We need to ensure that the regular device suspend/resume functions
	 * are called since our runtime_pm cannot guarantee local memory
	 * eviction for d3cold.
	 * TODO: Check HDA audio dependencies claimed by i915, and then enforce
	 *       this option to integrated graphics as well.
	 */
	if (IS_DGFX(xe))
		dev_pm_set_driver_flags(dev, DPM_FLAG_NO_DIRECT_COMPLETE);

	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_set_active(dev);
	pm_runtime_allow(dev);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put(dev);
}

void xe_pm_init_early(struct xe_device *xe)
{
	INIT_LIST_HEAD(&xe->mem_access.vram_userfault.list);
	drmm_mutex_init(&xe->drm, &xe->mem_access.vram_userfault.lock);
}

/**
 * xe_pm_init - Initialize Xe Power Management
 * @xe: xe device instance
 *
 * This component is responsible for System and Device sleep states.
 */
void xe_pm_init(struct xe_device *xe)
{
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);

	/* For now suspend/resume is only allowed with GuC */
	if (!xe_device_uc_enabled(xe))
		return;

	drmm_mutex_init(&xe->drm, &xe->d3cold.lock);

	xe->d3cold.capable = xe_pm_pci_d3cold_capable(pdev);

	if (xe->d3cold.capable) {
		xe_device_sysfs_init(xe);
		xe_pm_set_vram_threshold(xe, DEFAULT_VRAM_THRESHOLD);
	}

	xe_pm_runtime_init(xe);
}

/**
 * xe_pm_runtime_fini - Finalize Runtime PM
 * @xe: xe device instance
 */
void xe_pm_runtime_fini(struct xe_device *xe)
{
	struct device *dev = xe->drm.dev;

	pm_runtime_get_sync(dev);
	pm_runtime_forbid(dev);
}

static void xe_pm_write_callback_task(struct xe_device *xe,
				      struct task_struct *task)
{
	WRITE_ONCE(xe->pm_callback_task, task);

	/*
	 * Just in case it's somehow possible for our writes to be reordered to
	 * the extent that something else re-uses the task written in
	 * pm_callback_task. For example after returning from the callback, but
	 * before the reordered write that resets pm_callback_task back to NULL.
	 */
	smp_mb(); /* pairs with xe_pm_read_callback_task */
}

struct task_struct *xe_pm_read_callback_task(struct xe_device *xe)
{
	smp_mb(); /* pairs with xe_pm_write_callback_task */

	return READ_ONCE(xe->pm_callback_task);
}

/**
 * xe_pm_runtime_suspended - Inspect the current runtime_pm state.
 * @xe: xe device instance
 *
 * This does not provide any guarantee that the device is going to continue
 * suspended as it might be racing with the runtime state transitions.
 * It can be used only as a non-reliable assertion, to ensure that we are not in
 * the sleep state while trying to access some memory for instance.
 *
 * Returns true if PCI device is suspended, false otherwise.
 */
bool xe_pm_runtime_suspended(struct xe_device *xe)
{
	return pm_runtime_suspended(xe->drm.dev);
}

/**
 * xe_pm_runtime_suspend - Prepare our device for D3hot/D3Cold
 * @xe: xe device instance
 *
 * Returns 0 for success, negative error code otherwise.
 */
int xe_pm_runtime_suspend(struct xe_device *xe)
{
	struct xe_bo *bo, *on;
	struct xe_gt *gt;
	u8 id;
	int err = 0;

	/* Disable access_ongoing asserts and prevent recursive pm calls */
	xe_pm_write_callback_task(xe, current);

	/*
	 * Applying lock for entire list op as xe_ttm_bo_destroy and xe_bo_move_notify
	 * also checks and delets bo entry from user fault list.
	 */
	mutex_lock(&xe->mem_access.vram_userfault.lock);
	list_for_each_entry_safe(bo, on,
				 &xe->mem_access.vram_userfault.list, vram_userfault_link)
		xe_bo_runtime_pm_release_mmap_offset(bo);
	mutex_unlock(&xe->mem_access.vram_userfault.lock);

	if (xe->d3cold.allowed) {
		err = xe_bo_evict_all(xe);
		if (err)
			goto out;
	}

	for_each_gt(gt, xe, id) {
		err = xe_gt_suspend(gt);
		if (err)
			goto out;
	}

	xe_irq_suspend(xe);

	if (xe->d3cold.allowed)
		xe_display_pm_runtime_suspend(xe);
out:
	xe_pm_write_callback_task(xe, NULL);
	return err;
}

/**
 * xe_pm_runtime_resume - Waking up from D3hot/D3Cold
 * @xe: xe device instance
 *
 * Returns 0 for success, negative error code otherwise.
 */
int xe_pm_runtime_resume(struct xe_device *xe)
{
	struct xe_gt *gt;
	u8 id;
	int err = 0;

	/* Disable access_ongoing asserts and prevent recursive pm calls */
	xe_pm_write_callback_task(xe, current);

	if (xe->d3cold.allowed) {
		for_each_gt(gt, xe, id) {
			err = xe_pcode_init(gt);
			if (err)
				goto out;
		}

		xe_display_pm_runtime_resume(xe);

		/*
		 * This only restores pinned memory which is the memory
		 * required for the GT(s) to resume.
		 */
		err = xe_bo_restore_kernel(xe);
		if (err)
			goto out;
	}

	xe_irq_resume(xe);

	for_each_gt(gt, xe, id)
		xe_gt_resume(gt);

	if (xe->d3cold.allowed) {
		err = xe_bo_restore_user(xe);
		if (err)
			goto out;
	}
out:
	xe_pm_write_callback_task(xe, NULL);
	return err;
}

/**
 * xe_pm_runtime_get - Get a runtime_pm reference and resume synchronously
 * @xe: xe device instance
 */
void xe_pm_runtime_get(struct xe_device *xe)
{
	pm_runtime_get_noresume(xe->drm.dev);

	if (xe_pm_read_callback_task(xe) == current)
		return;

	pm_runtime_resume(xe->drm.dev);
}

/**
 * xe_pm_runtime_put - Put the runtime_pm reference back and mark as idle
 * @xe: xe device instance
 */
void xe_pm_runtime_put(struct xe_device *xe)
{
	if (xe_pm_read_callback_task(xe) == current) {
		pm_runtime_put_noidle(xe->drm.dev);
	} else {
		pm_runtime_mark_last_busy(xe->drm.dev);
		pm_runtime_put(xe->drm.dev);
	}
}

/**
 * xe_pm_runtime_get_sync - Get a runtime_pm reference and resume synchronously
 * @xe: xe device instance
 *
 * Returns: Any number grater than or equal to 0 for success, negative error
 * code otherwise.
 */
int xe_pm_runtime_get_sync(struct xe_device *xe)
{
	if (WARN_ON(xe_pm_read_callback_task(xe) == current))
		return -ELOOP;

	return pm_runtime_get_sync(xe->drm.dev);
}

/**
 * xe_pm_runtime_get_if_active - Get a runtime_pm reference if device active
 * @xe: xe device instance
 *
 * Returns: Any number grater than or equal to 0 for success, negative error
 * code otherwise.
 */
int xe_pm_runtime_get_if_active(struct xe_device *xe)
{
	return pm_runtime_get_if_active(xe->drm.dev, true);
}

/**
 * xe_pm_runtime_get_if_in_use - Get a runtime_pm reference and resume if needed
 * @xe: xe device instance
 *
 * Returns: True if device is awake and the reference was taken, false otherwise.
 */
bool xe_pm_runtime_get_if_in_use(struct xe_device *xe)
{
	if (xe_pm_read_callback_task(xe) == current) {
		/* The device is awake, grab the ref and move on */
		pm_runtime_get_noresume(xe->drm.dev);
		return true;
	}

	return pm_runtime_get_if_in_use(xe->drm.dev) >= 0;
}

/**
 * xe_pm_runtime_resume_and_get - Resume, then get a runtime_pm ref if awake.
 * @xe: xe device instance
 *
 * Returns: True if device is awake and the reference was taken, false otherwise.
 */
bool xe_pm_runtime_resume_and_get(struct xe_device *xe)
{
	if (xe_pm_read_callback_task(xe) == current) {
		/* The device is awake, grab the ref and move on */
		pm_runtime_get_noresume(xe->drm.dev);
		return true;
	}

	return pm_runtime_resume_and_get(xe->drm.dev) >= 0;
}

/**
 * xe_pm_assert_unbounded_bridge - Disable PM on unbounded pcie parent bridge
 * @xe: xe device instance
 */
void xe_pm_assert_unbounded_bridge(struct xe_device *xe)
{
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);
	struct pci_dev *bridge = pci_upstream_bridge(pdev);

	if (!bridge)
		return;

	if (!bridge->driver) {
		drm_warn(&xe->drm, "unbounded parent pci bridge, device won't support any PM support.\n");
		device_set_pm_not_required(&pdev->dev);
	}
}

/**
 * xe_pm_set_vram_threshold - Set a vram threshold for allowing/blocking D3Cold
 * @xe: xe device instance
 * @threshold: VRAM size in bites for the D3cold threshold
 *
 * Returns 0 for success, negative error code otherwise.
 */
int xe_pm_set_vram_threshold(struct xe_device *xe, u32 threshold)
{
	struct ttm_resource_manager *man;
	u32 vram_total_mb = 0;
	int i;

	for (i = XE_PL_VRAM0; i <= XE_PL_VRAM1; ++i) {
		man = ttm_manager_type(&xe->ttm, i);
		if (man)
			vram_total_mb += DIV_ROUND_UP_ULL(man->size, 1024 * 1024);
	}

	drm_dbg(&xe->drm, "Total vram %u mb\n", vram_total_mb);

	if (threshold > vram_total_mb)
		return -EINVAL;

	mutex_lock(&xe->d3cold.lock);
	xe->d3cold.vram_threshold = threshold;
	mutex_unlock(&xe->d3cold.lock);

	return 0;
}

/**
 * xe_pm_d3cold_allowed_toggle - Check conditions to toggle d3cold.allowed
 * @xe: xe device instance
 *
 * To be called during runtime_pm idle callback.
 * Check for all the D3Cold conditions ahead of runtime suspend.
 */
void xe_pm_d3cold_allowed_toggle(struct xe_device *xe)
{
	struct ttm_resource_manager *man;
	u32 total_vram_used_mb = 0;
	u64 vram_used;
	int i;

	if (!xe->d3cold.capable) {
		xe->d3cold.allowed = false;
		return;
	}

	for (i = XE_PL_VRAM0; i <= XE_PL_VRAM1; ++i) {
		man = ttm_manager_type(&xe->ttm, i);
		if (man) {
			vram_used = ttm_resource_manager_usage(man);
			total_vram_used_mb += DIV_ROUND_UP_ULL(vram_used, 1024 * 1024);
		}
	}

	mutex_lock(&xe->d3cold.lock);

	if (total_vram_used_mb < xe->d3cold.vram_threshold)
		xe->d3cold.allowed = true;
	else
		xe->d3cold.allowed = false;

	mutex_unlock(&xe->d3cold.lock);

	drm_dbg(&xe->drm,
		"d3cold: allowed=%s\n", str_yes_no(xe->d3cold.allowed));
}
