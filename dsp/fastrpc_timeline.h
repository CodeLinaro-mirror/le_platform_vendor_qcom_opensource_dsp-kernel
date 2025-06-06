// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __FASTRPC_TIMELINE_H__
#define __FASTRPC_TIMELINE_H__

#include <linux/types.h>
#include "fastrpc_shared.h"
#include "fastrpc_timeline_shared.h"

/* kernel supported timeline version */
#define TIMELINE_VERSION 1
/* max timeline buffer count */
#define TIMELINE_BUF_COUNT 4
/* get timeline per buffer event based on total number of events */
#define GET_TIMELINE_BUF_LEN(num_events) (num_events/TIMELINE_BUF_COUNT)
/* get timeline buffer size based on num events */
#define GET_TIMELINE_BUF_SIZE(num_events) \
	(sizeof(struct fastrpc_timeline_buffer) + \
	(num_events * sizeof(struct fastrpc_timeline_event)))
/**
 * Max number of events count for all 4 timeline buffers
 * Prevents user from allocating a buffer bigger than 1MB for timeline.
 * (MAX_BUFFER_SIZE(1MB) - fastrpc_timeline_buffer size(16 bytes)) /
 * fastrpc_timeline_event(24 bytes) = 43,690 events maximum
 * Rounding off to 40960 to keep it within safe limits.
 * This is a hard limit that cannot be exceeded. If user tries to set
 * more than this value, it will be returned with failure.
 */
#define MAX_TIMELINE_EVENT_COUNT 40960

struct fastrpc_user;

/* Timeline object holding kernel/dsp buffers and timeline info */
struct fastrpc_timeline {
	/* kernel timeline buffer used for event logging */
	struct fastrpc_timeline_buffer *timeline_buf_hlos_k;
	/* dsp root PD buffer for dsp timeline events */
	struct fastrpc_timeline_buffer *timeline_buf_dsp_k;
	/* dsp user PD buffer for dsp event logging */
	struct fastrpc_timeline_buffer *timeline_buf_dsp_u;
	/* buffer object for @timeline_buf_dsp_u */
	struct fastrpc_buf *fastrpc_buf_dsp_u;
	/* buffer object for @timeline_buf_dsp_k */
	struct fastrpc_buf *fastrpc_buf_dsp_k;
	/* timeline version; 0 if unsupported */
	u32 version;
	/* indicate current index in kernel timeline buf */
	atomic_t *atomic_index;
	/* timeline initialization status */
	bool initialized;
};

/**
 * __fastrpc_timeline_record() - Records a timeline event.
 * @event_id:   timeline event id.
 * @group_id:   timeline group id.
 * @timeline:   timeline object to be initialized.
 *
 * Return: 0 on success, negative error code on failure.
 */
int __fastrpc_timeline_record(u64 event_id, u32 group_id,
	struct fastrpc_timeline *timeline);

/* Inline wrapper to avoid function call overhead when timeline is disabled */
static __always_inline int fastrpc_timeline_record(u64 event_id, u32 group_id,
	struct fastrpc_timeline *timeline)
{
	/* avoid timeline record call if timeline is NULL */
	if (!timeline || !READ_ONCE(timeline->initialized))
		return -ENOENT;

	return __fastrpc_timeline_record(event_id, group_id, timeline);
}

/**
 * fastrpc_timeline_buffer_init() - Initializes timeline buffer.
 * @buf:        timeline buffer
 * @num_events: total number of timeline events this buffer can store
 */
void fastrpc_timeline_buffer_init(struct fastrpc_timeline_buffer *buf,
	u32 num_events);

/**
 * fastrpc_update_timeline_version() - Updates the timeline obj with the
 *                                     min version and set init to true.
 * @timeline:       timeline object.
 *
 * version value of 0 means timeline not supported on dsp.
 *
 * return 0 on success, negative error code otherwise.
 */
int fastrpc_update_timeline_version(struct fastrpc_timeline *timeline);

/**
 * fastrpc_timeline_init() - Initialize timeline object.
 * @timeline:       timeline object to be initialized.
 * @user_version:   userspace version.
 * @buf_hlos_k:     hlos kernel fastrpc_timeline_buffer.
 * @buf_dsp_k:      dsp kernel fastrpc_buf.
 * @buf_dsp_u:      dsp userspace fastrpc_buf.
 */
void fastrpc_timeline_init(struct fastrpc_timeline *timeline,
	u32 user_version, struct fastrpc_timeline_buffer *buf_hlos_k,
	struct fastrpc_buf *buf_dsp_k, struct fastrpc_buf *buf_dsp_u);

/**
 * fastrpc_timeline_deinit() - de-initialize the timeline object.
 * @timeline:   timeline object to be de-initialized.
 */
void fastrpc_timeline_deinit(struct fastrpc_timeline *timeline);

#endif /* __FASTRPC_TIMELINE_H__ */
