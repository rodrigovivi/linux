/* SPDX-Liense-Identifier: GPL-2.0 */
/*
 * Copyright © 2022 Intel Corporation
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM xe

#if !defined(_XE_TRACE_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _XE_TRACE_H_

#include <linux/types.h>
#include <linux/tracepoint.h>

#include "xe_engine_types.h"
#include "xe_guc_engine_types.h"
#include "xe_sched_job.h"

DECLARE_EVENT_CLASS(xe_engine,
		    TP_PROTO(struct xe_engine *e),
		    TP_ARGS(e),

		    TP_STRUCT__entry(
			     __field(enum xe_engine_class, class)
			     __field(u32, logical_mask)
			     __field(u16, width)
			     __field(u16, guc_id)
			     __field(u32, guc_state)
			     __field(u32, flags)
			     ),

		    TP_fast_assign(
			   __entry->class = e->class;
			   __entry->logical_mask = e->logical_mask;
			   __entry->width = e->width;
			   __entry->guc_id = e->guc->id;
			   __entry->guc_state = e->guc->state;
			   __entry->flags = e->flags;
			   ),

		    TP_printk("%d:0x%x, width=%d, guc_id=%d, guc_state=0x%x, flags=0x%x",
			      __entry->class, __entry->logical_mask,
			      __entry->width, __entry->guc_id,
			      __entry->guc_state, __entry->flags)
);

DEFINE_EVENT(xe_engine, xe_engine_create,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_scheduling_enable,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_scheduling_disable,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_scheduling_done,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_register,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_deregister,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_deregister_done,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_close,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_kill,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_cleanup_entity,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_destroy,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_reset,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_stop,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_resubmit,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DECLARE_EVENT_CLASS(xe_sched_job,
		    TP_PROTO(struct xe_sched_job *job),
		    TP_ARGS(job),

		    TP_STRUCT__entry(
			     __field(u32, seqno)
			     __field(u16, guc_id)
			     __field(u32, guc_state)
			     __field(u32, flags)
			     __field(int, error)
			     ),

		    TP_fast_assign(
			   __entry->seqno = xe_sched_job_seqno(job);
			   __entry->guc_id = job->engine->guc->id;
			   __entry->guc_state = job->engine->guc->state;
			   __entry->flags = job->engine->flags;
			   __entry->error = job->fence->error;
			   ),

		    TP_printk("seqno=%u, guc_id=%d, guc_state=0x%x, flags=0x%x, error=%d",
			      __entry->seqno, __entry->guc_id,
			      __entry->guc_state, __entry->flags,
			      __entry->error)
);

DEFINE_EVENT(xe_sched_job, xe_sched_job_exec,
	     TP_PROTO(struct xe_sched_job *job),
	     TP_ARGS(job)
);

DEFINE_EVENT(xe_sched_job, xe_sched_job_run,
	     TP_PROTO(struct xe_sched_job *job),
	     TP_ARGS(job)
);

DEFINE_EVENT(xe_sched_job, xe_sched_job_free,
	     TP_PROTO(struct xe_sched_job *job),
	     TP_ARGS(job)
);

DEFINE_EVENT(xe_sched_job, xe_sched_job_timedout,
	     TP_PROTO(struct xe_sched_job *job),
	     TP_ARGS(job)
);

DEFINE_EVENT(xe_sched_job, xe_sched_job_set_error,
	     TP_PROTO(struct xe_sched_job *job),
	     TP_ARGS(job)
);

DEFINE_EVENT(xe_sched_job, xe_sched_job_ban,
	     TP_PROTO(struct xe_sched_job *job),
	     TP_ARGS(job)
);

DECLARE_EVENT_CLASS(drm_sched_msg,
		    TP_PROTO(struct drm_sched_msg *msg),
		    TP_ARGS(msg),

		    TP_STRUCT__entry(
			     __field(u32, opcode)
			     ),

		    TP_fast_assign(
			   __entry->opcode = msg->opcode;
			   ),

		    TP_printk("opcode=%u", __entry->opcode)
);

DEFINE_EVENT(drm_sched_msg, drm_sched_msg_add,
	     TP_PROTO(struct drm_sched_msg *msg),
	     TP_ARGS(msg)
);

DEFINE_EVENT(drm_sched_msg, drm_sched_msg_recv,
	     TP_PROTO(struct drm_sched_msg *msg),
	     TP_ARGS(msg)
);

#endif /* _XE_TRACE_H_ */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_PATH ../../drivers/gpu/drm/xe
#define TRACE_INCLUDE_FILE xe_trace
#include <trace/define_trace.h>
