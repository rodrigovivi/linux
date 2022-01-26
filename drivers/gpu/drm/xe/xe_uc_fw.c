// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <linux/bitfield.h>
#include <linux/firmware.h>

#include "xe_device.h"
#include "xe_uc_fw.h"
#include "xe_bo.h"

#define XE_UC_FIRMWARE_URL "https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/xe"

static struct xe_device *
__uc_fw_to_xe(struct xe_uc_fw *uc_fw, enum xe_uc_fw_type type)
{
	if (type == XE_UC_FW_TYPE_GUC)
		return container_of(uc_fw, struct xe_device, uc.guc.fw);

	XE_BUG_ON(type != XE_UC_FW_TYPE_HUC);
	return container_of(uc_fw, struct xe_device, uc.huc.fw);
}

static struct xe_device *uc_fw_to_xe(struct xe_uc_fw *uc_fw)
{
	return __uc_fw_to_xe(uc_fw, uc_fw->type);
}

/*
 * List of required GuC and HuC binaries per-platform.
 * Must be ordered based on platform + revid, from newer to older.
 */
#define XE_GUC_FIRMWARE_DEFS(fw_def, guc_def) \
	fw_def(DG1,          0, guc_def(dg1,  69, 0, 3)) \
	fw_def(TIGERLAKE,    0, guc_def(tgl,  69, 0, 3))

#define XE_HUC_FIRMWARE_DEFS(fw_def, huc_def) \
	fw_def(DG1,          0, huc_def(dg1,  7, 9, 3)) \
	fw_def(TIGERLAKE,    0, huc_def(tgl,  7, 9, 3))

#define __MAKE_UC_FW_PATH(prefix_, name_, major_, minor_, patch_) \
	"xe/" \
	__stringify(prefix_) name_ \
	__stringify(major_) "." \
	__stringify(minor_) "." \
	__stringify(patch_) ".bin"

#define MAKE_GUC_FW_PATH(prefix_, major_, minor_, patch_) \
	__MAKE_UC_FW_PATH(prefix_, "_guc_", major_, minor_, patch_)

#define MAKE_HUC_FW_PATH(prefix_, major_, minor_, bld_num_) \
	__MAKE_UC_FW_PATH(prefix_, "_huc_", major_, minor_, bld_num_)

/* All blobs need to be declared via MODULE_FIRMWARE() */
#define XE_UC_MODULE_FW(platform_, revid_, uc_) \
	MODULE_FIRMWARE(uc_);

XE_GUC_FIRMWARE_DEFS(XE_UC_MODULE_FW, MAKE_GUC_FW_PATH)
XE_HUC_FIRMWARE_DEFS(XE_UC_MODULE_FW, MAKE_HUC_FW_PATH)

/* The below structs and macros are used to iterate across the list of blobs */
struct __packed uc_fw_blob {
	u8 major;
	u8 minor;
	const char *path;
};

#define UC_FW_BLOB(major_, minor_, path_) \
	{ .major = major_, .minor = minor_, .path = path_ }

#define GUC_FW_BLOB(prefix_, major_, minor_, patch_) \
	UC_FW_BLOB(major_, minor_, \
		   MAKE_GUC_FW_PATH(prefix_, major_, minor_, patch_))

#define HUC_FW_BLOB(prefix_, major_, minor_, bld_num_) \
	UC_FW_BLOB(major_, minor_, \
		   MAKE_HUC_FW_PATH(prefix_, major_, minor_, bld_num_))

struct __packed uc_fw_platform_requirement {
	enum xe_platform p;
	u8 rev; /* first platform rev using this FW */
	const struct uc_fw_blob blob;
};

#define MAKE_FW_LIST(platform_, revid_, uc_) \
{ \
	.p = XE_##platform_, \
	.rev = revid_, \
	.blob = uc_, \
},

struct fw_blobs_by_type {
	const struct uc_fw_platform_requirement *blobs;
	u32 count;
};

static void
uc_fw_auto_select(struct xe_device *xe, struct xe_uc_fw *uc_fw)
{
	static const struct uc_fw_platform_requirement blobs_guc[] = {
		XE_GUC_FIRMWARE_DEFS(MAKE_FW_LIST, GUC_FW_BLOB)
	};
	static const struct uc_fw_platform_requirement blobs_huc[] = {
		XE_HUC_FIRMWARE_DEFS(MAKE_FW_LIST, HUC_FW_BLOB)
	};
	static const struct fw_blobs_by_type blobs_all[XE_UC_FW_NUM_TYPES] = {
		[XE_UC_FW_TYPE_GUC] = { blobs_guc, ARRAY_SIZE(blobs_guc) },
		[XE_UC_FW_TYPE_HUC] = { blobs_huc, ARRAY_SIZE(blobs_huc) },
	};
	static const struct uc_fw_platform_requirement *fw_blobs;
	enum xe_platform p = xe->info.platform;
	u32 fw_count;
	u8 rev = xe->info.revid;
	int i;

	XE_BUG_ON(uc_fw->type >= ARRAY_SIZE(blobs_all));
	fw_blobs = blobs_all[uc_fw->type].blobs;
	fw_count = blobs_all[uc_fw->type].count;

	for (i = 0; i < fw_count && p <= fw_blobs[i].p; i++) {
		if (p == fw_blobs[i].p && rev >= fw_blobs[i].rev) {
			const struct uc_fw_blob *blob = &fw_blobs[i].blob;

			uc_fw->path = blob->path;
			uc_fw->major_ver_wanted = blob->major;
			uc_fw->minor_ver_wanted = blob->minor;
			break;
		}
	}
}

int xe_uc_fw_init(struct xe_uc_fw *uc_fw)
{
	struct xe_device *xe = uc_fw_to_xe(uc_fw);
	struct device *dev = xe->drm.dev;
	const struct firmware *fw = NULL;
	struct uc_css_header *css;
	struct xe_bo *obj;
	size_t size;
	int err;

	/*
	 * we use FIRMWARE_UNINITIALIZED to detect checks against uc_fw->status
	 * before we're looked at the HW caps to see if we have uc support
	 */
	BUILD_BUG_ON(XE_UC_FIRMWARE_UNINITIALIZED);
	XE_BUG_ON(uc_fw->status);
	XE_BUG_ON(uc_fw->path);

	uc_fw_auto_select(xe, uc_fw);
	xe_uc_fw_change_status(uc_fw, uc_fw->path ? *uc_fw->path ?
			       XE_UC_FIRMWARE_SELECTED :
			       XE_UC_FIRMWARE_DISABLED :
			       XE_UC_FIRMWARE_NOT_SUPPORTED);

	err = request_firmware(&fw, uc_fw->path, dev);
	if (err)
		goto fail;

	/* Check the size of the blob before examining buffer contents */
	if (unlikely(fw->size < sizeof(struct uc_css_header))) {
		drm_warn(&xe->drm, "%s firmware %s: invalid size: %zu < %zu\n",
			 xe_uc_fw_type_repr(uc_fw->type), uc_fw->path,
			 fw->size, sizeof(struct uc_css_header));
		err = -ENODATA;
		goto fail;
	}

	css = (struct uc_css_header *)fw->data;

	/* Check integrity of size values inside CSS header */
	size = (css->header_size_dw - css->key_size_dw - css->modulus_size_dw -
		css->exponent_size_dw) * sizeof(u32);
	if (unlikely(size != sizeof(struct uc_css_header))) {
		drm_warn(&xe->drm,
			 "%s firmware %s: unexpected header size: %zu != %zu\n",
			 xe_uc_fw_type_repr(uc_fw->type), uc_fw->path,
			 fw->size, sizeof(struct uc_css_header));
		err = -EPROTO;
		goto fail;
	}

	/* uCode size must calculated from other sizes */
	uc_fw->ucode_size = (css->size_dw - css->header_size_dw) * sizeof(u32);

	/* now RSA */
	uc_fw->rsa_size = css->key_size_dw * sizeof(u32);

	/* At least, it should have header, uCode and RSA. Size of all three. */
	size = sizeof(struct uc_css_header) + uc_fw->ucode_size + uc_fw->rsa_size;
	if (unlikely(fw->size < size)) {
		drm_warn(&xe->drm, "%s firmware %s: invalid size: %zu < %zu\n",
			 xe_uc_fw_type_repr(uc_fw->type), uc_fw->path,
			 fw->size, size);
		err = -ENOEXEC;
		goto fail;
	}

	/* Get version numbers from the CSS header */
	uc_fw->major_ver_found = FIELD_GET(CSS_SW_VERSION_UC_MAJOR,
					   css->sw_version);
	uc_fw->minor_ver_found = FIELD_GET(CSS_SW_VERSION_UC_MINOR,
					   css->sw_version);

	if (uc_fw->major_ver_found != uc_fw->major_ver_wanted ||
	    uc_fw->minor_ver_found < uc_fw->minor_ver_wanted) {
		drm_notice(&xe->drm, "%s firmware %s: unexpected version: %u.%u != %u.%u\n",
			   xe_uc_fw_type_repr(uc_fw->type), uc_fw->path,
			   uc_fw->major_ver_found, uc_fw->minor_ver_found,
			   uc_fw->major_ver_wanted, uc_fw->minor_ver_wanted);
		if (!xe_uc_fw_is_overridden(uc_fw)) {
			err = -ENOEXEC;
			goto fail;
		}
	}

	if (uc_fw->type == XE_UC_FW_TYPE_GUC)
		uc_fw->private_data_size = css->private_data_size;

	obj = xe_bo_create_from_data(xe, fw->data, fw->size,
				     ttm_bo_type_kernel,
				     XE_BO_CREATE_VRAM_IF_DGFX(xe) |
				     XE_BO_CREATE_GGTT_BIT);
	if (IS_ERR(obj)) {
		drm_notice(&xe->drm, "%s firmware %s: failed to create / populate bo",
			   xe_uc_fw_type_repr(uc_fw->type), uc_fw->path);
		err = PTR_ERR(obj);
		goto fail;
	}

	uc_fw->bo = obj;
	uc_fw->size = fw->size;
	xe_uc_fw_change_status(uc_fw, XE_UC_FIRMWARE_AVAILABLE);

	release_firmware(fw);
	return 0;

fail:
	xe_uc_fw_change_status(uc_fw, err == -ENOENT ?
			       XE_UC_FIRMWARE_MISSING :
			       XE_UC_FIRMWARE_ERROR);

	drm_notice(&xe->drm, "%s firmware %s: fetch failed with error %d\n",
		   xe_uc_fw_type_repr(uc_fw->type), uc_fw->path, err);
	drm_info(&xe->drm, "%s firmware(s) can be downloaded from %s\n",
		 xe_uc_fw_type_repr(uc_fw->type), XE_UC_FIRMWARE_URL);

	release_firmware(fw);		/* OK even if fw is NULL */
	return err;
}

void xe_uc_fw_fini(struct xe_uc_fw *uc_fw)
{
	if (!xe_uc_fw_is_available(uc_fw))
		return;

	xe_bo_unpin_map_no_vm(uc_fw->bo);
	xe_uc_fw_change_status(uc_fw, XE_UC_FIRMWARE_SELECTED);
}
