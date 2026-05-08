// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/ktime.h>
#include <linux/atomic.h>
#include "fastrpc_timeline.h"
#include <asm/barrier.h>
#include <asm/arch_timer.h>

void fastrpc_timeline_buffer_init(struct fastrpc_timeline_buffer *buf,
	u32 num_events)
{
	buf->version = TIMELINE_VERSION;
	buf->atomic_index = 0;
	buf->event_list_length = num_events;
	buf->reserved0 = 0;
	memset(buf->timeline_event_list, 0,
		sizeof(struct fastrpc_timeline_event) * num_events);
}

int fastrpc_update_timeline_version(struct fastrpc_timeline *timeline)
{
	u32 min_version = 0;

	if (!timeline)
		return -ENOENT;
	min_version = MIN(timeline->timeline_buf_dsp_k->version,
		timeline->timeline_buf_hlos_k->version);
	timeline->version = min_version;
	if (!min_version) {
		fastrpc_timeline_deinit(timeline);
		pr_err("%s: failed as DSP does not support timeline\n",
			__func__);
		return -EINVAL;
	}
	/*
	 * Timeline initialization must not rely on untrusted userspace
	 * arguments. Instead, only initialize the timeline if the version
	 * is non-zero, which confirms DSP support. If the version is zero,
	 * the timeline is unsupported.
	 */
	timeline->initialized = true;
	return 0;
}

int __fastrpc_timeline_record(u64 event_id, u32 group_id,
	struct fastrpc_timeline *timeline)
{
	struct fastrpc_timeline_buffer *buf = timeline->timeline_buf_hlos_k;
	struct fastrpc_timeline_event *event = NULL;
	int index = 0;

	if (buf) {
		index = atomic_fetch_add_relaxed(1, timeline->atomic_index) %
			buf->event_list_length;
		event = &(buf->timeline_event_list[index]);
		event->timestamp = __arch_counter_get_cntvct();
		event->group_id = group_id;
		event->event_id = event_id;
	} else {
		pr_err("%s: failed for invalid timeline buffer\n", __func__);
		return -EINVAL;
	}
	return 0;
}

void fastrpc_timeline_init(struct fastrpc_timeline *timeline,
	u32 user_version, struct fastrpc_timeline_buffer *buf_hlos_k,
	struct fastrpc_buf *buf_dsp_k, struct fastrpc_buf *buf_dsp_u)
{
	timeline->timeline_buf_hlos_k = buf_hlos_k;
	timeline->timeline_buf_dsp_k = buf_dsp_k->virt;
	timeline->timeline_buf_dsp_u = buf_dsp_u->virt;
	timeline->fastrpc_buf_dsp_k = buf_dsp_k;
	timeline->fastrpc_buf_dsp_u = buf_dsp_u;
	/*
	 * Version Control. Kernel compares its version with userspace and
	 * chooses the lowest of the two.
	 */
	if (user_version < TIMELINE_VERSION)
		timeline->version = user_version;
	else
		timeline->version = TIMELINE_VERSION;
	timeline->timeline_buf_hlos_k->version = timeline->version;
	timeline->timeline_buf_dsp_k->version = timeline->version;
	timeline->timeline_buf_dsp_u->version = timeline->version;
	timeline->atomic_index =
		(atomic_t *)(&timeline->timeline_buf_hlos_k->atomic_index);
	atomic_set(timeline->atomic_index, 0);
	timeline->initialized = false;
}

void fastrpc_timeline_deinit(struct fastrpc_timeline *timeline)
{
	if (!timeline)
		return;
	timeline->initialized = false;
	kvfree(timeline->timeline_buf_hlos_k);
	if (timeline->fastrpc_buf_dsp_k)
		fastrpc_buf_free(timeline->fastrpc_buf_dsp_k, false);
	if (timeline->fastrpc_buf_dsp_u)
		fastrpc_buf_free(timeline->fastrpc_buf_dsp_u, false);
}
