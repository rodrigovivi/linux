.. SPDX-License-Identifier: (GPL-2.0+ OR MIT)

===================================
Xe EU Debug (eudebug)
===================================

.. The uapi header is also included in full by driver-uapi.rst, so keep
   the structs pulled in here out of the global C namespace to avoid
   duplicate declaration warnings.

.. c:namespace:: xe_eudebug

Overview
========

The EU Debug (eudebug) subsystem provides an interface for debugging
Execution Unit (EU) workloads on Intel Xe GPUs, independently from
CPU process debugging. This enables tools like GDB (via gdbserver-intelgt)
to debug GPU compute workloads through the Level-Zero Debug API.
In practice, this allows SYCL/DPC++ workloads submitted by the
compute-runtime to be debugged interactively.


Connection Establishment
========================

.. kernel-doc:: drivers/gpu/drm/xe/xe_eudebug.c
   :doc: Connection Establishment

File Descriptor Acquisition Methods
-----------------------------------

.. kernel-doc:: drivers/gpu/drm/xe/xe_eudebug.c
   :doc: File Descriptor Acquisition Methods

Security Model
--------------
.. kernel-doc:: drivers/gpu/drm/xe/xe_eudebug.c
   :doc: Security Model

Events
======

.. kernel-doc:: drivers/gpu/drm/xe/xe_eudebug.c
   :doc: Events

Reading Events
--------------

.. kernel-doc:: include/uapi/drm/xe_drm_eudebug.h
   :doc: DRM_XE_EUDEBUG_IOCTL_READ_EVENT

Base Event Header
-----------------

The debugger receives events with the DRM_XE_EUDEBUG_IOCTL_READ_EVENT
ioctl on the eudebug file descriptor. Every event has
:c:type:`drm_xe_eudebug_event` as a header.

.. kernel-doc:: include/uapi/drm/xe_drm_eudebug.h
   :identifiers: drm_xe_eudebug_event

Resource Event Types
--------------------

.. kernel-doc:: include/uapi/drm/xe_drm_eudebug.h
   :identifiers: drm_xe_eudebug_event_vm

.. kernel-doc:: include/uapi/drm/xe_drm_eudebug.h
   :identifiers: drm_xe_eudebug_event_exec_queue

.. kernel-doc:: include/uapi/drm/xe_drm_eudebug.h
   :identifiers: drm_xe_eudebug_event_vm_bind

.. kernel-doc:: include/uapi/drm/xe_drm_eudebug.h
   :identifiers: drm_xe_eudebug_event_vm_bind_op_debug_data

.. kernel-doc:: include/uapi/drm/xe_drm_eudebug.h
   :identifiers: drm_xe_eudebug_event_vm_bind_ufence

.. kernel-doc:: include/uapi/drm/xe_drm_eudebug.h
   :identifiers: drm_xe_eudebug_event_eu_attention

VM Access
=========

.. kernel-doc:: include/uapi/drm/xe_drm_eudebug.h
   :identifiers: drm_xe_eudebug_vm_open

EU/HW Control
=============

.. kernel-doc:: include/uapi/drm/xe_drm_eudebug.h
   :identifiers: drm_xe_eudebug_eu_control
