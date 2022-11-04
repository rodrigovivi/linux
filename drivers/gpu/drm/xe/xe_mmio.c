// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include "xe_mmio.h"

#include <drm/drm_managed.h>
#include <drm/xe_drm.h>

#include "xe_device.h"
#include "xe_gt.h"
#include "xe_gt_mcr.h"
#include "xe_macros.h"

#include "../i915/i915_reg.h"
#include "../i915/gt/intel_engine_regs.h"
#include "../i915/gt/intel_gt_regs.h"

#define XEHP_MTCFG_ADDR		_MMIO(0x101800)
#define TILE_COUNT		REG_GENMASK(15, 8)

static int xe_set_dma_info(struct xe_device *xe)
{
	unsigned int mask_size = xe->info.dma_mask_size;
	int err;

	/*
	 * We don't have a max segment size, so set it to the max so sg's
	 * debugging layer doesn't complain
	 */
	dma_set_max_seg_size(xe->drm.dev, UINT_MAX);

	err = dma_set_mask(xe->drm.dev, DMA_BIT_MASK(mask_size));
	if (err)
		goto mask_err;

	err = dma_set_coherent_mask(xe->drm.dev, DMA_BIT_MASK(mask_size));
	if (err)
		goto mask_err;

	return 0;

mask_err:
	drm_err(&xe->drm, "Can't set DMA mask/consistent mask (%d)\n", err);
	return err;
}

int xe_mmio_probe_vram(struct xe_device *xe)
{
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);
	struct xe_gt *gt;
	u8 id;

	if (!IS_DGFX(xe)) {
		xe->mem.vram.mapping = 0;
		xe->mem.vram.size = 0;
		xe->mem.vram.io_start = 0;

		for_each_gt(gt, xe, id) {
			gt->mem.vram.mapping = 0;
			gt->mem.vram.size = 0;
			gt->mem.vram.io_start = 0;
		}
		return 0;
	}

	gt = xe_device_get_gt(xe, 0);
	xe->mem.vram.size = xe_mmio_read64(gt, GEN12_GSMBASE.reg);
	xe->mem.vram.io_start = pci_resource_start(pdev, 2);

	if (xe->mem.vram.size > pci_resource_len(pdev, 2)) {
		xe->mem.vram.size = pci_resource_len(pdev, 2);
		drm_warn(&xe->drm, "Restricting VRAM size to PCI resource size.\n");
	}

#ifdef CONFIG_64BIT
	xe->mem.vram.mapping = ioremap_wc(xe->mem.vram.io_start,
					  xe->mem.vram.size);
#endif

	drm_info(&xe->drm, "TOTAL VRAM: %pa, %pa\n",
		 &xe->mem.vram.io_start, &xe->mem.vram.size);

	if (xe->info.has_flat_ccs)  {
		int err;
		u64 lmem_size;
		u64 flat_ccs_base;
		u64 remove_len = 0;
		u32 reg;

		err = xe_force_wake_get(gt_to_fw(gt), XE_FW_GT);
		if (err)
			return err;
		reg = xe_gt_mcr_unicast_read_any(gt, XEHP_TILE0_ADDR_RANGE);
		lmem_size = (u64)REG_FIELD_GET(GENMASK(14, 8), reg) * SZ_1G;
		reg = xe_gt_mcr_unicast_read_any(gt, XEHP_FLAT_CCS_BASE_ADDR);
		flat_ccs_base = (u64)REG_FIELD_GET(GENMASK(31, 8), reg) * SZ_64K;
		if (lmem_size > flat_ccs_base)
			remove_len = lmem_size - flat_ccs_base;
		xe->mem.vram.size -= remove_len;
		drm_info(&xe->drm, "lmem_size: 0x%llx flat_ccs_base: 0x%llx remove_len: 0x%llx\n",
			 lmem_size, flat_ccs_base, remove_len);

		err = xe_force_wake_put(gt_to_fw(gt), XE_FW_GT);
		if (err)
			return err;
	}

	/* FIXME: Assuming equally partitioned VRAM, incorrect */
	if (xe->info.tile_count > 1) {
		u8 adj_tile_count = xe->info.tile_count;
		resource_size_t size, io_start;

		for_each_gt(gt, xe, id)
			if (xe_gt_is_media_type(gt))
				--adj_tile_count;

		XE_BUG_ON(!adj_tile_count);

		size = xe->mem.vram.size / adj_tile_count;
		io_start = xe->mem.vram.io_start;

		for_each_gt(gt, xe, id) {
			if (id && !xe_gt_is_media_type(gt))
				io_start += size;

			gt->mem.vram.size = size;
			gt->mem.vram.io_start = io_start;
			gt->mem.vram.mapping = xe->mem.vram.mapping +
				(io_start - xe->mem.vram.io_start);

			drm_info(&xe->drm, "VRAM[%u, %u]: %pa, %pa\n",
				 id, gt->info.vram_id, &gt->mem.vram.io_start,
				 &gt->mem.vram.size);
		}
	} else {
		gt->mem.vram.size = xe->mem.vram.size;
		gt->mem.vram.io_start = xe->mem.vram.io_start;
		gt->mem.vram.mapping = xe->mem.vram.mapping;

		drm_info(&xe->drm, "VRAM: %pa\n", &gt->mem.vram.size);
	}
	return 0;
}

static void xe_mmio_probe_tiles(struct xe_device *xe)
{
	struct xe_gt *gt = xe_device_get_gt(xe, 0);
	u32 mtcfg;
	u8 adj_tile_count;
	u8 id;

	if (xe->info.tile_count == 1)
		return;

	mtcfg = xe_mmio_read64(gt, XEHP_MTCFG_ADDR.reg);
	adj_tile_count = xe->info.tile_count =
		REG_FIELD_GET(TILE_COUNT, mtcfg) + 1;
	if (xe->info.media_ver >= 13)
		xe->info.tile_count *= 2;

	drm_info(&xe->drm, "tile_count: %d, adj_tile_count %d\n",
		 xe->info.tile_count, adj_tile_count);

	if (xe->info.tile_count > 1) {
		const int mmio_bar = 0;
		size_t size;
		void *regs;

		if (adj_tile_count > 1) {
			pci_iounmap(to_pci_dev(xe->drm.dev), xe->mmio.regs);
			xe->mmio.size = SZ_16M * adj_tile_count;
			xe->mmio.regs = pci_iomap(to_pci_dev(xe->drm.dev),
						  mmio_bar, xe->mmio.size);
		}

		size = xe->mmio.size / adj_tile_count;
		regs = xe->mmio.regs;

		for_each_gt(gt, xe, id) {
			if (id && !xe_gt_is_media_type(gt))
				regs += size;
			gt->mmio.size = size;
			gt->mmio.regs = regs;
		}
	}
}

static void mmio_fini(struct drm_device *drm, void *arg)
{
	struct xe_device *xe = arg;

	pci_iounmap(to_pci_dev(xe->drm.dev), xe->mmio.regs);
	if (xe->mem.vram.mapping)
		iounmap(xe->mem.vram.mapping);
}

int xe_mmio_init(struct xe_device *xe)
{
	struct xe_gt *gt = xe_device_get_gt(xe, 0);
	const int mmio_bar = 0;
	int err;

	/*
	 * Map the entire BAR, which includes registers (0-4MB), reserved space
	 * (4MB-8MB), and GGTT (8MB-16MB). Other parts of the driver (GTs,
	 * GGTTs) will derive the pointers they need from the mapping in the
	 * device structure.
	 */
	xe->mmio.size = SZ_16M;
	xe->mmio.regs = pci_iomap(to_pci_dev(xe->drm.dev), mmio_bar,
				  xe->mmio.size);
	if (xe->mmio.regs == NULL) {
		drm_err(&xe->drm, "failed to map registers\n");
		return -EIO;
	}

	err = drmm_add_action_or_reset(&xe->drm, mmio_fini, xe);
	if (err)
		return err;

	/* 1 GT for now, 1 to 1 mapping, may change on multi-GT devices */
	gt->mmio.size = xe->mmio.size;
	gt->mmio.regs = xe->mmio.regs;

	/*
	 * The boot firmware initializes local memory and assesses its health.
	 * If memory training fails, the punit will have been instructed to
	 * keep the GT powered down; we won't be able to communicate with it
	 * and we should not continue with driver initialization.
	 */
	if (IS_DGFX(xe) && !(xe_mmio_read32(gt, GU_CNTL.reg) & LMEM_INIT)) {
		drm_err(&xe->drm, "LMEM not initialized by firmware\n");
		return -ENODEV;
	}

	err = xe_set_dma_info(xe);
	if (err)
		return err;

	xe_mmio_probe_tiles(xe);

	return 0;
}

#define VALID_MMIO_FLAGS (\
	DRM_XE_MMIO_BITS_MASK |\
	DRM_XE_MMIO_READ |\
	DRM_XE_MMIO_WRITE)

static const i915_reg_t mmio_read_whitelist[] = {
	RING_TIMESTAMP(RENDER_RING_BASE),
};

int xe_mmio_ioctl(struct drm_device *dev, void *data,
		  struct drm_file *file)
{
	struct xe_device *xe = to_xe_device(dev);
	struct drm_xe_mmio *args = data;
	unsigned int bits_flag, bytes;
	bool allowed;
	int ret = 0;

	if (XE_IOCTL_ERR(xe, args->extensions))
		return -EINVAL;

	if (XE_IOCTL_ERR(xe, args->flags & ~VALID_MMIO_FLAGS))
		return -EINVAL;

	if (XE_IOCTL_ERR(xe, !(args->flags & DRM_XE_MMIO_WRITE) && args->value))
		return -EINVAL;

	allowed = capable(CAP_SYS_ADMIN);
	if (!allowed && ((args->flags & ~DRM_XE_MMIO_BITS_MASK) == DRM_XE_MMIO_READ)) {
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(mmio_read_whitelist); i++) {
			if (mmio_read_whitelist[i].reg == args->addr) {
				allowed = true;
				break;
			}
		}
	}

	if (XE_IOCTL_ERR(xe, !allowed))
		return -EPERM;

	bits_flag = args->flags & DRM_XE_MMIO_BITS_MASK;
	bytes = 1 << bits_flag;
	if (XE_IOCTL_ERR(xe, args->addr + bytes > xe->mmio.size))
		return -EINVAL;

	xe_force_wake_get(gt_to_fw(&xe->gt[0]), XE_FORCEWAKE_ALL);

	if (args->flags & DRM_XE_MMIO_WRITE) {
		switch (bits_flag) {
		case DRM_XE_MMIO_8BIT:
			return -EINVAL; /* TODO */
		case DRM_XE_MMIO_16BIT:
			return -EINVAL; /* TODO */
		case DRM_XE_MMIO_32BIT:
			if (XE_IOCTL_ERR(xe, args->value > U32_MAX))
				return -EINVAL;
			xe_mmio_write32(to_gt(xe), args->addr, args->value);
			break;
		case DRM_XE_MMIO_64BIT:
			xe_mmio_write64(to_gt(xe), args->addr, args->value);
			break;
		default:
			drm_WARN(&xe->drm, 1, "Invalid MMIO bit size");
			ret = -EINVAL;
			goto exit;
		}
	}

	if (args->flags & DRM_XE_MMIO_READ) {
		switch (bits_flag) {
		case DRM_XE_MMIO_8BIT:
			return -EINVAL; /* TODO */
		case DRM_XE_MMIO_16BIT:
			return -EINVAL; /* TODO */
		case DRM_XE_MMIO_32BIT:
			args->value = xe_mmio_read32(to_gt(xe), args->addr);
			break;
		case DRM_XE_MMIO_64BIT:
			args->value = xe_mmio_read64(to_gt(xe), args->addr);
			break;
		default:
			drm_WARN(&xe->drm, 1, "Invalid MMIO bit size");
			ret = -EINVAL;
		}
	}

exit:
	xe_force_wake_put(gt_to_fw(&xe->gt[0]), XE_FORCEWAKE_ALL);

	return ret;
}
