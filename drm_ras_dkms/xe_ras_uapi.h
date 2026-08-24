/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 *
 * xe drm-ras severity/component definitions, extracted verbatim from the RAS
 * block of the upstream include/uapi/drm/xe_drm.h so the standalone 5.15
 * backport of the xe drm-ras consumer keeps identical node-id/error-id and
 * name mappings without pulling in the whole xe UAPI.
 *
 * The driver registers one DRM RAS node per error severity; each node exposes
 * one error counter per error component.
 */

#ifndef _XE_RAS_UAPI_H_
#define _XE_RAS_UAPI_H_

/**
 * enum drm_xe_ras_error_severity - DRM RAS error severity (maps to node-id).
 */
enum drm_xe_ras_error_severity {
	/** @DRM_XE_RAS_ERR_SEV_CORRECTABLE: Correctable Error */
	DRM_XE_RAS_ERR_SEV_CORRECTABLE = 0,
	/** @DRM_XE_RAS_ERR_SEV_UNCORRECTABLE: Uncorrectable Error */
	DRM_XE_RAS_ERR_SEV_UNCORRECTABLE,
	/** @DRM_XE_RAS_ERR_SEV_MAX: Max severity */
	DRM_XE_RAS_ERR_SEV_MAX /* non-ABI */
};

/**
 * enum drm_xe_ras_error_component - DRM RAS error component (maps to error-id).
 */
enum drm_xe_ras_error_component {
	/** @DRM_XE_RAS_ERR_COMP_CORE_COMPUTE: Core Compute Error */
	DRM_XE_RAS_ERR_COMP_CORE_COMPUTE = 1,
	/** @DRM_XE_RAS_ERR_COMP_SOC_INTERNAL: SoC Internal Error */
	DRM_XE_RAS_ERR_COMP_SOC_INTERNAL,
	/** @DRM_XE_RAS_ERR_COMP_MAX: Max Error */
	DRM_XE_RAS_ERR_COMP_MAX	/* non-ABI */
};

/*
 * Error severity to name mapping.
 */
#define DRM_XE_RAS_ERROR_SEVERITY_NAMES {				\
	[DRM_XE_RAS_ERR_SEV_CORRECTABLE] = "correctable-errors",	\
	[DRM_XE_RAS_ERR_SEV_UNCORRECTABLE] = "uncorrectable-errors",	\
}

/*
 * Error component to name mapping.
 */
#define DRM_XE_RAS_ERROR_COMPONENT_NAMES {				\
	[DRM_XE_RAS_ERR_COMP_CORE_COMPUTE] = "core-compute",		\
	[DRM_XE_RAS_ERR_COMP_SOC_INTERNAL] = "soc-internal"		\
}

#endif /* _XE_RAS_UAPI_H_ */
