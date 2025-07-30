/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2025 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


/* FIXME: THIS Copyright message conflicts with the MIT declaration.. need to check*/


#ifndef _DRM_NETLINK_H_
#define _DRM_NETLINK_H_

#define DRM_GENL_VERSION 1

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * enum drm_genl_error_cmds - Supported error commands
 *
 */
enum drm_genl_error_cmds {
	DRM_CMD_UNSPEC,
	/**
	 * @DRM_RAS_CMD_QUERY: Command to list all errors names with config-id in verbose mode.
	 * In normal mode will list IP blocks, total instances available and error types supported
	 */
	DRM_RAS_CMD_QUERY,
	/** @DRM_RAS_CMD_READ_ONE: Command to get a counter for a specific error */
	DRM_RAS_CMD_READ_ONE,
	/** @DRM_RAS_CMD_READ_BLOCK: Command to get a counter of specific error type from an IP
	 * block
	 */
	DRM_RAS_CMD_READ_BLOCK,
	/** @DRM_RAS_CMD_READ_ALL: Command to get counters of all errors */
	DRM_RAS_CMD_READ_ALL,

	__DRM_CMD_MAX,
	DRM_CMD_MAX = __DRM_CMD_MAX - 1,
};

enum drm_cmd_request_type {
	DRM_RAS_CMD_QUERY_VERBOSE = 1,
	DRM_RAS_CMD_QUERY_NORMAL = 2,
};

/**
 * enum drm_error_attr - Attributes to use with drm_genl_error_cmds
 *
 */
enum drm_error_attr {
	DRM_ATTR_UNSPEC,
	DRM_ATTR_PAD = DRM_ATTR_UNSPEC,
	/**
	 * @DRM_RAS_ATTR_QUERY: Should be used with DRM_RAS_CMD_QUERY,
	 * DRM_RAS_CMD_READ_ALL
	 */
	DRM_RAS_ATTR_QUERY, /* NLA_U8 */
	/**
	 * @DRM_RAS_ATTR_READ_ALL: Should be used with DRM_RAS_CMD_READ_ALL
	 */
	DRM_RAS_ATTR_READ_ALL, /* NLA_U8 */
	/**
	 * @DRM_RAS_ATTR_QUERY_REPLY: First Nested attributed sent as a
	 * response to DRM_RAS_CMD_QUERY, DRM_RAS_CMD_READ_ALL commands.
	 */
	DRM_RAS_ATTR_QUERY_REPLY, /* NLA_NESTED */
	/** @DRM_RAS_ATTR_ERROR_NAME: Used to pass error name */
	DRM_RAS_ATTR_ERROR_NAME, /* NLA_NUL_STRING */
	/** @DRM_RAS_ATTR_ERROR_ID: Used to pass error id, should be used with
	 * DRM_RAS_CMD_READ_ONE, DRM_RAS_CMD_READ_BLOCK
	 */
	DRM_RAS_ATTR_ERROR_ID, /* NLA_U64 */
	/** @DRM_RAS_ATTR_ERROR_VALUE: Used to pass error value */
	DRM_RAS_ATTR_ERROR_VALUE, /* NLA_U64 */

	__DRM_ATTR_MAX,
	DRM_ATTR_MAX = __DRM_ATTR_MAX - 1,
};

#if defined(__cplusplus)
}
#endif

#endif
