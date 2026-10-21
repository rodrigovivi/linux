/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef _PRELIM_XE_EUDEBUG_V2_ABI_H_
#define _PRELIM_XE_EUDEBUG_V2_ABI_H_

#include <linux/build_bug.h>
#include <uapi/drm/xe_drm_prelim.h>

#include "xe_eudebug_proto.h"

/*
 * The PRELIM v2 eudebug uAPI is the eudebug core uAPI under other names,
 * so the core can serve it as is. Generated, keep in sync.
 */

static_assert(sizeof(struct prelim_v2_drm_xe_eudebug_ack) == sizeof(struct drm_xe_eudebug_ack));
static_assert(sizeof(struct prelim_v2_drm_xe_eudebug_connect) == sizeof(struct drm_xe_eudebug_connect));
static_assert(sizeof(struct prelim_v2_drm_xe_eudebug_eu_control) == sizeof(struct drm_xe_eudebug_eu_control));
static_assert(sizeof(struct prelim_v2_drm_xe_eudebug_event) == sizeof(struct drm_xe_eudebug_event));
static_assert(sizeof(struct prelim_v2_drm_xe_eudebug_event_eu_attention) == sizeof(struct drm_xe_eudebug_event_eu_attention));
static_assert(sizeof(struct prelim_v2_drm_xe_eudebug_event_exec_queue) == sizeof(struct drm_xe_eudebug_event_exec_queue));
static_assert(sizeof(struct prelim_v2_drm_xe_eudebug_event_pagefault) == sizeof(struct drm_xe_eudebug_event_pagefault));
static_assert(sizeof(struct prelim_v2_drm_xe_eudebug_event_vm) == sizeof(struct drm_xe_eudebug_event_vm));
static_assert(sizeof(struct prelim_v2_drm_xe_eudebug_event_vm_bind) == sizeof(struct drm_xe_eudebug_event_vm_bind));
static_assert(sizeof(struct prelim_v2_drm_xe_eudebug_event_vm_bind_op_debug_data) == sizeof(struct drm_xe_eudebug_event_vm_bind_op_debug_data));
static_assert(sizeof(struct prelim_v2_drm_xe_eudebug_event_vm_bind_ufence) == sizeof(struct drm_xe_eudebug_event_vm_bind_ufence));
static_assert(sizeof(struct prelim_v2_drm_xe_eudebug_vm_open) == sizeof(struct drm_xe_eudebug_vm_open));
static_assert(sizeof(struct prelim_v2_drm_xe_vm_bind_op_ext_debug_data) == sizeof(struct drm_xe_vm_bind_op_ext_debug_data));

static_assert(PRELIM_V2_DRM_XE_EUDEBUG_IOCTL_READ_EVENT == DRM_XE_EUDEBUG_IOCTL_READ_EVENT);
static_assert(PRELIM_V2_DRM_XE_EUDEBUG_IOCTL_ACK_EVENT == DRM_XE_EUDEBUG_IOCTL_ACK_EVENT);
static_assert(PRELIM_V2_DRM_XE_EUDEBUG_IOCTL_VM_OPEN == DRM_XE_EUDEBUG_IOCTL_VM_OPEN);
static_assert(PRELIM_V2_DRM_XE_EUDEBUG_IOCTL_EU_CONTROL == DRM_XE_EUDEBUG_IOCTL_EU_CONTROL);
static_assert(PRELIM_V2_DRM_XE_EUDEBUG_EVENT_NONE == DRM_XE_EUDEBUG_EVENT_NONE);
static_assert(PRELIM_V2_DRM_XE_EUDEBUG_EVENT_READ == DRM_XE_EUDEBUG_EVENT_READ);
static_assert(PRELIM_V2_DRM_XE_EUDEBUG_EVENT_VM == DRM_XE_EUDEBUG_EVENT_VM);
static_assert(PRELIM_V2_DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE == DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE);
static_assert(PRELIM_V2_DRM_XE_EUDEBUG_EVENT_VM_BIND == DRM_XE_EUDEBUG_EVENT_VM_BIND);
static_assert(PRELIM_V2_DRM_XE_EUDEBUG_EVENT_VM_BIND_OP_DEBUG_DATA == DRM_XE_EUDEBUG_EVENT_VM_BIND_OP_DEBUG_DATA);
static_assert(PRELIM_V2_DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE == DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE);
static_assert(PRELIM_V2_DRM_XE_EUDEBUG_EVENT_EU_ATTENTION == DRM_XE_EUDEBUG_EVENT_EU_ATTENTION);
static_assert(PRELIM_V2_DRM_XE_EUDEBUG_EVENT_PAGEFAULT == DRM_XE_EUDEBUG_EVENT_PAGEFAULT);
static_assert(PRELIM_V2_DRM_XE_EUDEBUG_EVENT_CREATE == DRM_XE_EUDEBUG_EVENT_CREATE);
static_assert(PRELIM_V2_DRM_XE_EUDEBUG_EVENT_DESTROY == DRM_XE_EUDEBUG_EVENT_DESTROY);
static_assert(PRELIM_V2_DRM_XE_EUDEBUG_EVENT_STATE_CHANGE == DRM_XE_EUDEBUG_EVENT_STATE_CHANGE);
static_assert(PRELIM_V2_DRM_XE_EUDEBUG_EVENT_NEED_ACK == DRM_XE_EUDEBUG_EVENT_NEED_ACK);
static_assert(PRELIM_V2_DRM_XE_EUDEBUG_EVENT_VM_BIND_FLAG_UFENCE == DRM_XE_EUDEBUG_EVENT_VM_BIND_FLAG_UFENCE);
static_assert(PRELIM_V2_DRM_XE_EUDEBUG_EU_CONTROL_CMD_INTERRUPT_ALL == DRM_XE_EUDEBUG_EU_CONTROL_CMD_INTERRUPT_ALL);
static_assert(PRELIM_V2_DRM_XE_EUDEBUG_EU_CONTROL_CMD_STOPPED == DRM_XE_EUDEBUG_EU_CONTROL_CMD_STOPPED);
static_assert(PRELIM_V2_DRM_XE_EUDEBUG_EU_CONTROL_CMD_RESUME == DRM_XE_EUDEBUG_EU_CONTROL_CMD_RESUME);
static_assert(PRELIM_V2_DRM_XE_VM_BIND_DEBUG_DATA_FLAG_PSEUDO == DRM_XE_VM_BIND_DEBUG_DATA_FLAG_PSEUDO);
static_assert(PRELIM_V2_DRM_XE_VM_BIND_DEBUG_DATA_PSEUDO_MODULE_AREA == DRM_XE_VM_BIND_DEBUG_DATA_PSEUDO_MODULE_AREA);
static_assert(PRELIM_V2_DRM_XE_VM_BIND_DEBUG_DATA_PSEUDO_SBA_AREA == DRM_XE_VM_BIND_DEBUG_DATA_PSEUDO_SBA_AREA);
static_assert(PRELIM_V2_DRM_XE_VM_BIND_DEBUG_DATA_PSEUDO_SIP_AREA == DRM_XE_VM_BIND_DEBUG_DATA_PSEUDO_SIP_AREA);

#endif
