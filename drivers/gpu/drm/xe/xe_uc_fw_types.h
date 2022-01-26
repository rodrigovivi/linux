/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_UC_FW_TYPES_H_
#define _XE_UC_FW_TYPES_H_

#include <linux/types.h>

struct xe_bo;

/*
 * +------------+---------------------------------------------------+
 * |   PHASE    |           FIRMWARE STATUS TRANSITIONS             |
 * +============+===================================================+
 * |            |               UNINITIALIZED                       |
 * +------------+-               /   |   \                         -+
 * |            |   DISABLED <--/    |    \--> NOT_SUPPORTED        |
 * | init_early |                    V                              |
 * |            |                 SELECTED                          |
 * +------------+-               /   |   \                         -+
 * |            |    MISSING <--/    |    \--> ERROR                |
 * |   fetch    |                    V                              |
 * |            |                 AVAILABLE                         |
 * +------------+-                   |   \                         -+
 * |            |                    |    \--> INIT FAIL            |
 * |   init     |                    V                              |
 * |            |        /------> LOADABLE <----<-----------\       |
 * +------------+-       \         /    \        \           \     -+
 * |            |    LOAD FAIL <--<      \--> TRANSFERRED     \     |
 * |   upload   |                  \           /   \          /     |
 * |            |                   \---------/     \--> RUNNING    |
 * +------------+---------------------------------------------------+
 */

/*
 * FIXME: Ported from the i915 and this is state machine is way too complicated.
 * Circle back and simplify this.
 */
enum xe_uc_fw_status {
	XE_UC_FIRMWARE_NOT_SUPPORTED = -1, /* no uc HW */
	XE_UC_FIRMWARE_UNINITIALIZED = 0, /* used to catch checks done too early */
	XE_UC_FIRMWARE_DISABLED, /* disabled */
	XE_UC_FIRMWARE_SELECTED, /* selected the blob we want to load */
	XE_UC_FIRMWARE_MISSING, /* blob not found on the system */
	XE_UC_FIRMWARE_ERROR, /* invalid format or version */
	XE_UC_FIRMWARE_AVAILABLE, /* blob found and copied in mem */
	XE_UC_FIRMWARE_INIT_FAIL, /* failed to prepare fw objects for load */
	XE_UC_FIRMWARE_LOADABLE, /* all fw-required objects are ready */
	XE_UC_FIRMWARE_LOAD_FAIL, /* failed to xfer or init/auth the fw */
	XE_UC_FIRMWARE_TRANSFERRED, /* dma xfer done */
	XE_UC_FIRMWARE_RUNNING /* init/auth done */
};

enum xe_uc_fw_type {
	XE_UC_FW_TYPE_GUC = 0,
	XE_UC_FW_TYPE_HUC
};
#define XE_UC_FW_NUM_TYPES 2

/**
 * struct xe_uc_fw - XE micro controller firmware
 */
struct xe_uc_fw {
	/** @type: type uC firmware */
	enum xe_uc_fw_type type;
	union {
		const enum xe_uc_fw_status status;
		enum xe_uc_fw_status __status; /* no accidental overwrites */
	};
	/** @path: path to uC firmware */
	const char *path;
	/** @user_overridden: user provided path to uC firmware via modparam */
	bool user_overridden;
	/** @size: size of uC firmware including css header */
	size_t size;

	/** @obj: XE BO for uC firmware */
	struct xe_bo *bo;

	/*
	 * The firmware build process will generate a version header file with major and
	 * minor version defined. The versions are built into CSS header of firmware.
	 * i915 kernel driver set the minimal firmware version required per platform.
	 */
	u16 major_ver_wanted;
	u16 minor_ver_wanted;
	u16 major_ver_found;
	u16 minor_ver_found;

	/** @rsa_size: RSA size */
	u32 rsa_size;
	/** @ucode_size: micro kernel size */
	u32 ucode_size;

	/** @private_data_size: size of private data found in uC css header */
	u32 private_data_size;
};

#endif	/* _XE_UC_FW_TYPES_H_ */
