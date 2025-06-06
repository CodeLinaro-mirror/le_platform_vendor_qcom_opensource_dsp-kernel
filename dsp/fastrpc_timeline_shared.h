// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __FASTRPC_TIMELINE_SHARED_H__
#define __FASTRPC_TIMELINE_SHARED_H__

#include <linux/types.h>

/* Holds timeline events information */
struct fastrpc_timeline_event {
	/* Event timestamp - qtimer value */
	uint64_t timestamp;
	/* tid of the thread requesting to record */
	uint32_t group_id;
	/* Timeline event ID */
	uint32_t event_id;
	/* Reserved */
	uint64_t unused;
};

/* Buffer used by HLOS/DSP to store timeline events */
struct fastrpc_timeline_buffer {
	/* Stores timeline version of kernel */
	uint32_t version;
	/* Indicate current write index */
	uint32_t atomic_index;
	/* Total number of events in the buffer */
	uint32_t event_list_length;
	/* Reserved */
	uint32_t reserved0;
	/* Array of fastrpc_timeline_event structures */
	struct fastrpc_timeline_event timeline_event_list[];
};

#endif /* __FASTRPC_TIMELINE_SHARED_H__ */
