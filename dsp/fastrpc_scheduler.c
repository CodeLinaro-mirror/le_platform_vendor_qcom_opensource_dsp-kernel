// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/hashtable.h>
#include <linux/kthread.h>
#include <linux/limits.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/rbtree.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/wait.h>
#include "../include/uapi/misc/fastrpc.h"
#include "fastrpc_shared.h"

#define FASTRPC_APP_PRIORITY_DEFAULT	1000

/*
 * Build and post a workinfo notification for a work.
 * fastrpc_npu_post_workinfo() uses GFP_ATOMIC internally, so this is
 * safe to call under sched->lock.
 */
static void fastrpc_workinfo_notify(struct fastrpc_scheduler *sched,
				    struct fastrpc_work_node *work,
				    int event, u32 reason)
{
	struct fastrpc_channel_ctx *cctx =
		container_of(sched, struct fastrpc_channel_ctx,
			     scheduler);
	struct npu_work_info info = {};

	info.timestamp_ms	 = ktime_to_ms(ktime_get_real());
	info.event		 = (u32)event;
	info.reason		 = reason;
	info.id			 = (s32)work->work_id;
	info.uid		 = work->app_id;
	info.debug_pid		 = work->app_id;
	info.domain		 = cctx->domain_id;
	info.job_priority	 = (s32)work->work_prio;
	info.effective_priority	 = (s32)work->eff_prio;
	/* group_id and debug_feature_id: encoding TBD with userspace */
	pr_debug("%s: event=%s reason=%u handle=0x%llx app_id=%d work_prio=%d eff_prio=%d domain=%d\n",
		__func__,
		event == WORK_STARTED ? "STARTED" :
		event == WORK_ENDED   ? "ENDED"   : "REQUESTED",
		reason, work->handle, work->app_id,
		(int)work->work_prio, (int)work->eff_prio,
		cctx->domain_id);
	fastrpc_npu_post_workinfo(cctx, &info);
}

/*
 * Look up the priority for a given app_id (uid) in the NPU app priority
 * table.  Returns the matched priority, or FASTRPC_APP_PRIORITY_DEFAULT
 * if no entry matches.
 */
static u32 fastrpc_npu_lookup_prio(struct fastrpc_channel_ctx *cctx,
				    int app_id)
{
	struct npu_app_prio_table *table = cctx->npu_app_prio;
	unsigned long flags;
	u32 i, prio = FASTRPC_APP_PRIORITY_DEFAULT;

	/*
	 * Acquire cctx->lock to guard against a concurrent
	 * fastrpc_npu_app_prio_set() write.  All call sites hold
	 * sched->lock on entry; the writer never takes sched->lock,
	 * so the lock ordering sched->lock -> cctx->lock
	 * is deadlock-free.
	 */
	spin_lock_irqsave(&cctx->lock, flags);
	for (i = 0; i < table->num_entries; i++) {
		if (table->entries[i].uid == app_id) {
			prio = (u32)table->entries[i].priority;
			break;
		}
	}
	spin_unlock_irqrestore(&cctx->lock, flags);
	return prio;
}

/*
 * Validate that the calling process (caller_uid) is allowed to submit
 * work targeting a given appid, based on the NPU priority table
 * configured by the AIDL scheduling service.
 *
 * Access rules enforced (see struct fastrpc_npu_app_prio_config):
 *
 *   0. appid < 0 (infrastructure sentinel):  always allowed.  Handles
 *      opened without the _appid URI token carry appid -1; these bypass
 *      the access gate entirely.
 *
 *   1. Table not populated:  always allowed.  Systems that have not
 *      received an NPU_PRIORITY_WORKINFO ioctl from the AIDL service
 *      operate without access control.
 *
 *   2. caller_uid not in table:  denied.  Only UIDs explicitly
 *      registered by the scheduling service may submit work.  This
 *      blocks new apps that lack an Android manifest permission entry.
 *
 *   3. caller_uid in table, appid == caller_uid:  allowed only if
 *      has_direct_access is set.  Apps with a manifest change or AI
 *      Core itself carry this flag; apps that rely on AI Core to proxy
 *      their work do not, so they cannot submit work directly.
 *
 *   4. caller_uid in table, appid != caller_uid:  allowed only if
 *      can_attribute_other_uid is set.  Only trusted proxies such as
 *      AI Core carry this flag, enabling them to submit work attributed
 *      to a client UID so that the correct per-client priority is used.
 *
 * Acquires cctx->lock for the table scan.  This function is called
 * before sched->lock is taken in fastrpc_work_add(), so only cctx->lock
 * is held — consistent with the lock ordering documented in
 * fastrpc_npu_lookup_prio().
 *
 * Returns 0 if access is granted, -EACCES if denied.
 */
static int fastrpc_npu_check_access(struct fastrpc_channel_ctx *cctx,
				    uid_t caller_uid, s32 appid)
{
	struct npu_app_prio_table *table = cctx->npu_app_prio;
	unsigned long flags;
	u32 i, has_direct_access = 0, can_attribute_other_uid = 0;
	bool found = false;

	/* Infrastructure/legacy sentinel (s32 = -1): always allowed */
	if (appid < 0)
		return 0;

	/* Rule 2: no table yet means no access control */
	if (!table || table->num_entries == 0) {
		dev_dbg(cctx->dev, "%s: NPU priority table not populated, skipping access check\n",
			__func__);
		return 0;
	}

	spin_lock_irqsave(&cctx->lock, flags);
	for (i = 0; i < table->num_entries; i++) {
		if (table->entries[i].uid == caller_uid) {
			has_direct_access = table->entries[i].has_direct_access;
			can_attribute_other_uid =
				table->entries[i].can_attribute_other_uid;
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&cctx->lock, flags);

	/* Rule 3: caller not registered in the priority table */
	if (!found) {
		dev_err(cctx->dev, "%s: uid %u not in NPU priority table, rejecting appid %d\n",
			__func__, caller_uid, appid);
		return -EACCES;
	}

	if ((uid_t)appid == caller_uid) {
		/* Rule 4: direct submission — requires has_direct_access */
		if (!has_direct_access) {
			dev_err(cctx->dev, "%s: uid %u lacks direct access (has_direct_access=0)\n",
				__func__, caller_uid);
			return -EACCES;
		}
	} else {
		/* Rule 5: cross-uid attribution — requires can_attribute_other_uid */
		if (!can_attribute_other_uid) {
			dev_err(cctx->dev, "%s: uid %u cannot attribute work to appid %d"
				" (can_attribute_other_uid=0)\n",
				__func__, caller_uid, appid);
			return -EACCES;
		}
	}

	return 0;
}

/*
 * Recalculate ref_prio as the minimum eff_prio across all executing works.
 * Must be called under sched->lock.
 */
static void fastrpc_recalc_ref_prio(struct fastrpc_scheduler *sched)
{
	struct fastrpc_work_node *work;
	u32 min = U32_MAX;

	list_for_each_entry(work, &sched->executing_list, exec_node)
		if (work->eff_prio < min)
			min = work->eff_prio;
	sched->ref_prio = min;
}

/*
 * Insert a work into the pending rbtree, keyed by (eff_prio, handle).
 * Lower eff_prio sorts left (higher priority).  Handle breaks ties.
 * Must be called under sched->lock.
 */
static void fastrpc_pending_tree_insert(struct fastrpc_scheduler *sched,
					struct fastrpc_work_node *new_work)
{
	struct rb_node **link = &sched->pending_tree.rb_node;
	struct rb_node *parent = NULL;

	while (*link) {
		struct fastrpc_work_node *cur =
			rb_entry(*link, struct fastrpc_work_node,
				 rb_node);

		parent = *link;
		if (new_work->eff_prio < cur->eff_prio ||
		    (new_work->eff_prio == cur->eff_prio &&
		     new_work->handle < cur->handle))
			link = &(*link)->rb_left;
		else
			link = &(*link)->rb_right;
	}
	rb_link_node(&new_work->rb_node, parent, link);
	rb_insert_color(&new_work->rb_node, &sched->pending_tree);
}

/*
 * Find or create the per-app entry for a given app_id in the hashtable.
 * Must be called under sched->lock.
 */
static struct fastrpc_app_entry *fastrpc_app_entry_get(
	struct fastrpc_scheduler *sched, int app_id)
{
	struct fastrpc_channel_ctx *cctx =
		container_of(sched, struct fastrpc_channel_ctx, scheduler);
	struct fastrpc_app_entry *entry;

	hash_for_each_possible(sched->app_works, entry,
			       hash_node, app_id) {
		if (entry->app_id == app_id)
			return entry;
	}

	entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		return NULL;

	entry->app_id = app_id;
	/*
	 * Initialise cur_prio from the live NPU priority table so that
	 * the first work for this app uses the correct priority immediately.
	 * Falls back to FASTRPC_APP_PRIORITY_DEFAULT if the table has not
	 * been populated yet or does not contain an entry for this app_id.
	 * Lock ordering (sched->lock -> npu_app_prio->lock) is safe here
	 * per the rule documented in fastrpc_npu_lookup_prio.
	 */
	if (cctx->npu_app_prio)
		entry->cur_prio = fastrpc_npu_lookup_prio(cctx,
							  app_id);
	else
		entry->cur_prio = FASTRPC_APP_PRIORITY_DEFAULT;
	INIT_LIST_HEAD(&entry->works);
	hash_add(sched->app_works, &entry->hash_node, app_id);
	return entry;
}

/*
 * Find an active work by (handle, app_id) using the per-app hashtable.
 * Returns a work in INCOMING, PENDING, or ADMITTED state only.
 * DONE and ABORTED works are skipped: they remain on the per-app list
 * until the kthread drains done_list/abort_list but are no longer
 * logically active.
 * Must be called under sched->lock.
 */
static struct fastrpc_work_node *fastrpc_find_work(
	struct fastrpc_scheduler *sched, u64 handle, int app_id)
{
	struct fastrpc_app_entry *entry;
	struct fastrpc_work_node *work;
	int state;

	hash_for_each_possible(sched->app_works, entry,
			       hash_node, app_id) {
		if (entry->app_id != app_id)
			continue;
		list_for_each_entry(work, &entry->works, app_node) {
			if (work->handle != handle)
				continue;
			state = atomic_read(&work->state);
			if (state == WORK_STATE_DONE ||
			    state == WORK_STATE_ABORTED)
				continue;
			return work;
		}
		return NULL;
	}
	return NULL;
}

/*
 * Scan hashtable for apps whose priority changed in the live table.
 * For each changed app: erase its pending works from the rbtree, update
 * eff_prio, and reinsert.  Skips empty and unchanged buckets.
 * Must be called under sched->lock.
 */
static void fastrpc_apply_prio_updates(struct fastrpc_scheduler *sched)
{
	struct fastrpc_channel_ctx *cctx =
		container_of(sched, struct fastrpc_channel_ctx,
			     scheduler);
	struct fastrpc_app_entry *entry;
	struct fastrpc_work_node *work, *tmp;
	int bkt;

	sched->prio_update_pending = false;

	if (!cctx->npu_app_prio)
		return;

	hash_for_each(sched->app_works, bkt, entry, hash_node) {
		u32 new_prio;

		if (list_empty(&entry->works))
			continue;

		new_prio = fastrpc_npu_lookup_prio(cctx,
						   entry->app_id);
		if (new_prio == entry->cur_prio)
			continue;

		pr_debug("%s: prio update app_id=%d old=%u new=%u\n",
			__func__, entry->app_id, entry->cur_prio, new_prio);

		/* Re-key only PENDING works for this app */
		list_for_each_entry_safe(work, tmp,
					 &entry->works, app_node) {
			if (atomic_read(&work->state) !=
			    WORK_STATE_PENDING)
				continue;
			rb_erase(&work->rb_node,
				 &sched->pending_tree);
			work->eff_prio = new_prio + work->work_prio;
			fastrpc_pending_tree_insert(sched, work);
		}
		entry->cur_prio = new_prio;
	}
}

static void fastrpc_work_node_free(struct fastrpc_work_node *work)
{
	kfree(work->group_id);
	kfree(work->feature_id);
	kfree(work);
}

/*
 * Drain the incoming_list: move new works into the rbtree, or free
 * them if already aborted.
 *
 * Works are already in the per-app hashtable (inserted by work_add).
 * For each work on the incoming_list:
 *  - If the abort path already transitioned it to ABORTED, the caller
 *    has been unblocked.  Remove from hashtable and per-user list,
 *    then kfree.
 *  - Otherwise compute eff_prio, transition to PENDING, and insert
 *    into the rbtree.
 *
 * Must be called under sched->lock.
 */
static void fastrpc_drain_incoming(struct fastrpc_scheduler *sched)
{
	struct fastrpc_channel_ctx *cctx =
		container_of(sched, struct fastrpc_channel_ctx,
			     scheduler);
	struct fastrpc_work_node *work, *tmp;
	struct fastrpc_app_entry *entry;
	u32 app_prio;

	list_for_each_entry_safe(work, tmp,
				 &sched->incoming_list,
				 staging_node) {
		/* Remove from incoming staging list */
		list_del(&work->staging_node);

		/*
		 * Check if work was aborted while still on
		 * incoming_list.  The abort path set state to
		 * ABORTED and already completed the waiter.
		 * Remove from hashtable and per-user list,
		 * then free.
		 */
		if (atomic_read(&work->state) == WORK_STATE_ABORTED) {
			pr_debug("%s: handle=0x%llx app_id=%d aborted, freeing\n",
				__func__, work->handle, work->app_id);
			list_del(&work->app_node);
			list_del_init(&work->user_node);
			fastrpc_work_node_free(work);
			continue;
		}

		/*
		 * Look up the per-app entry to read app priority.
		 * The entry was created by fastrpc_work_add() before
		 * the work was placed on incoming_list, so it is
		 * guaranteed to exist in the hashtable here.
		 * fastrpc_app_entry_get() will find it via
		 * hash_for_each_possible() and return without
		 * reaching the kzalloc path.  A NULL return
		 * therefore indicates hashtable corruption and is
		 * not expected under normal operation.
		 */
		entry = fastrpc_app_entry_get(sched, work->app_id);
		if (WARN_ON(!entry)) {
			list_del(&work->app_node);
			WRITE_ONCE(*work->result, -ENOMEM);
			complete(work->wait_done);
			list_del_init(&work->user_node);
			fastrpc_work_node_free(work);
			continue;
		}

		/* Compute effective priority from app table */
		if (cctx->npu_app_prio)
			app_prio = fastrpc_npu_lookup_prio(cctx,
							   work->app_id);
		else
			app_prio = entry->cur_prio;

		work->eff_prio = app_prio + work->work_prio;

		pr_debug("%s: handle=0x%llx app_id=%d app_prio=%u work_prio=%u eff_prio=%u -> PENDING\n",
			__func__, work->handle, work->app_id, app_prio,
			work->work_prio, work->eff_prio);

		/* Transition to PENDING and insert into rbtree */
		atomic_set(&work->state, WORK_STATE_PENDING);
		fastrpc_pending_tree_insert(sched, work);

		/* Notify that the work has been received and queued */
		fastrpc_workinfo_notify(sched, work, WORK_REQUESTED,
					NPU_WORK_REASON_NONE);
	}
}

/*
 * Drain the abort_list: remove aborted PENDING works from the rbtree
 * and per-app list, then free them.  The abort path already completed
 * the waiter and transitioned state to ABORTED.  The rbtree removal
 * is deferred here because only the kthread touches the rbtree.
 *
 * Must be called under sched->lock.
 */
static void fastrpc_drain_abort_list(struct fastrpc_scheduler *sched)
{
	struct fastrpc_work_node *work, *tmp;

	list_for_each_entry_safe(work, tmp,
				 &sched->abort_list,
				 staging_node) {
		list_del(&work->staging_node);

		pr_debug("%s: handle=0x%llx app_id=%d freeing aborted PENDING\n",
			__func__, work->handle, work->app_id);

		/* Remove from rbtree, per-app list, and per-user list */
		rb_erase(&work->rb_node, &sched->pending_tree);
		list_del(&work->app_node);
		list_del_init(&work->user_node);
		fastrpc_work_node_free(work);
	}
}

/*
 * Drain the done_list: fully clean up completed ADMITTED works.
 *
 * The work_remove / user_cleanup path staged the work on done_list after
 * the ADMITTED -> DONE state transition.  All structural work happens
 * here in the kthread, consistent with the kthread-only model used for
 * the rbtree (abort_list path):
 *
 *   - Remove from executing_list and per-app work list.
 *   - Recalculate ref_prio so the admission loop (Stage 5, which runs
 *     immediately after) sees the correct minimum priority across the
 *     remaining executing works.
 *   - Send the WORK_ENDED notification.
 *   - Remove from per-user list (idempotent — callers also do this to
 *     protect against fl being freed after user_cleanup returns).
 *   - Free the node.
 *
 * Must be called under sched->lock.
 */
static void fastrpc_drain_done_list(struct fastrpc_scheduler *sched)
{
	struct fastrpc_work_node *work, *tmp;

	list_for_each_entry_safe(work, tmp,
				 &sched->done_list,
				 staging_node) {
		list_del(&work->staging_node);
		list_del(&work->exec_node);
		list_del(&work->app_node);
		list_del_init(&work->user_node);
		fastrpc_recalc_ref_prio(sched);
		pr_debug("%s: handle=0x%llx app_id=%d DONE, freeing\n",
			__func__, work->handle, work->app_id);
		fastrpc_workinfo_notify(sched, work, WORK_ENDED,
					work->end_reason);
		fastrpc_work_node_free(work);
	}
}

/*
 * Scheduler worker kthread.
 *
 * Wakes when there is work to do: new works on incoming_list, aborted
 * or completed works to clean up, priority updates to apply, or a stop
 * signal.  Under sched->lock it performs, in order:
 *
 *   1. Drain abort_list   — rb_erase aborted PENDING works and kfree.
 *                           Done first so prio updates do not waste
 *                           time re-keying nodes that are about to be
 *                           deleted.
 *   2. Apply prio updates — re-key rbtree if app priorities changed.
 *                           Runs before drain_incoming so that newly
 *                           arriving works read the fresh entry->cur_prio
 *                           and are inserted with the correct eff_prio
 *                           on their first (and only) rbtree insert.
 *   3. Drain incoming_list — move new works into the rbtree with the
 *                           updated app priority already in place.
 *   4. Drain done_list    — remove completed ADMITTED works from
 *                           executing_list, recalc ref_prio, send
 *                           WORK_ENDED notification, and kfree.
 *                           Runs before the admission loop so ref_prio
 *                           and executing_list are accurate at Stage 5.
 *   5. Admission loop     — admit highest-priority pending works whose
 *                           eff_prio <= ref_prio (or if nothing is
 *                           running), move to executing_list, unblock
 *                           the waiting IOCTL caller
 */
static int fastrpc_scheduler_thread(void *data)
{
	struct fastrpc_scheduler *sched = data;

	while (!kthread_should_stop()) {
		/* Sleep until there is work to process */
		if (wait_event_interruptible(sched->wq,
			!list_empty(&sched->incoming_list) ||
			!list_empty(&sched->abort_list) ||
			!list_empty(&sched->done_list) ||
			READ_ONCE(sched->prio_update_pending) ||
			kthread_should_stop())) {
			if (signal_pending(current)) {
				/* Interrupted by signal; flush to avoid busy-spin */
				flush_signals(current);
				continue;
			}
		}
		if (kthread_should_stop())
			break;

		spin_lock(&sched->lock);

		/*
		 * Stage 1: remove aborted PENDING works from the rbtree
		 * before re-keying so prio updates skip doomed nodes.
		 */
		fastrpc_drain_abort_list(sched);

		/*
		 * Stage 2: re-key rbtree with updated app priorities.
		 * Running before drain_incoming means new works will
		 * read entry->cur_prio that is already up to date and
		 * are inserted with the correct eff_prio directly,
		 * avoiding a redundant erase+reinsert per new work.
		 */
		if (sched->prio_update_pending)
			fastrpc_apply_prio_updates(sched);

		/* Stage 3: move new works from incoming_list to rbtree */
		fastrpc_drain_incoming(sched);

		/* Stage 4: free completed executing works */
		fastrpc_drain_done_list(sched);

		/*
		 * Stage 5: admission loop.  Admit the highest-priority
		 * pending work if no works are running or its eff_prio
		 * is at least as good as the best running work.
		 */
		while (!RB_EMPTY_ROOT(&sched->pending_tree)) {
			struct rb_node *leftmost =
				rb_first(&sched->pending_tree);
			struct fastrpc_work_node *work =
				rb_entry(leftmost,
					 struct fastrpc_work_node,
					 rb_node);
			bool no_running =
				list_empty(&sched->executing_list);

			if (!no_running &&
			    work->eff_prio > sched->ref_prio) {
				pr_debug("%s: admission: handle=0x%llx eff_prio=%u > ref_prio=%u, holding\n",
					__func__, work->handle, work->eff_prio,
					sched->ref_prio);
				break;
			}

			/* Admit this work */
			rb_erase(&work->rb_node,
				 &sched->pending_tree);

			pr_debug("%s: admission: handle=0x%llx app_id=%d eff_prio=%u ref_prio=%u -> ADMITTED\n",
				__func__, work->handle, work->app_id,
				work->eff_prio, sched->ref_prio);

			/*
			 * Transition to ADMITTED.  Move to
			 * executing_list, update ref_prio,
			 * post the STARTED workinfo notification,
			 * then unblock the waiting IOCTL caller.
			 * The work stays on the per-user list
			 * (fl->sched_works) until work_remove or
			 * user cleanup.
			 *
			 * Admission only adds one work, so the new
			 * minimum can only decrease: take the cheaper
			 * min() instead of a full O(n) rescan.
			 */
			atomic_set(&work->state, WORK_STATE_ADMITTED);
			list_add_tail(&work->exec_node,
				      &sched->executing_list);
			sched->ref_prio = min(work->eff_prio, sched->ref_prio);

			/*
			 * Post the STARTED notification before unblocking
			 * the IOCTL caller so the event is observable in
			 * the workinfo queue by the time START returns.
			 */
			fastrpc_workinfo_notify(sched, work,
						WORK_STARTED,
						NPU_WORK_REASON_START_INITIAL);

			/* Unblock the work_add caller with success */
			WRITE_ONCE(*work->result, 0);
			complete(work->wait_done);
		}
		spin_unlock(&sched->lock);
	}
	return 0;
}

/*
 * Notify the scheduler that the app priority table has been updated.
 * Sets the dirty flag and wakes the kthread to rebalance pending works.
 */
void fastrpc_scheduler_notify_prio_update(
	struct fastrpc_scheduler *sched)
{
	spin_lock(&sched->lock);
	sched->prio_update_pending = true;
	spin_unlock(&sched->lock);

	wake_up(&sched->wq);
}
EXPORT_SYMBOL(fastrpc_scheduler_notify_prio_update);

/*
 * IOCTL handler for FASTRPC_INVOKE_REMOTE_WORK (status=START).
 *
 * Copies group_id and feature_id strings from userspace (if provided),
 * allocates a work node, places it on the incoming_list, and sleeps
 * until the scheduler kthread admits the work (returns 0) or until the
 * work is aborted by a concurrent REMOTE_WORK (status=END) (returns -ECANCELED).
 *
 * The completion and result variables live on this function's stack.
 * The work node stores pointers to them.  The kthread writes *result
 * and calls complete(wait_done) to unblock this caller.  The kthread
 * may kfree the work node at any later time — the stack variables
 * outlive the node because this function only returns after
 * wait_for_completion().
 *
 * The work node takes ownership of the allocated group_id and feature_id
 * strings.  The kthread is the sole entity that frees them via
 * fastrpc_work_node_free.
 */
int fastrpc_work_add(struct fastrpc_user *fl,
		    struct fastrpc_ioctl_remote_work *work)
{
	DECLARE_COMPLETION_ONSTACK(done);
	struct fastrpc_scheduler *sched = &fl->cctx->scheduler;
	struct fastrpc_work_node *wnode;
	char *group_id = NULL;
	char *feature_id = NULL;
	u32 cap = 0;
	int result = 0;
	int err = 0;

	/*
	 * Gate: verify the caller is permitted to submit work for this
	 * appid before allocating any resources.  See the access rules
	 * documented above fastrpc_npu_check_access().
	 */
	err = fastrpc_npu_check_access(fl->cctx, fl->uid, work->appid);
	if (err)
		return err;

	if (work->group_id && work->group_id_len > 0) {
		if (work->group_id_len > NPU_MAX_WORKINFO_FIELD_LEN)
			return -EINVAL;
		group_id = strndup_user(
			(void __user *)(uintptr_t)work->group_id,
			work->group_id_len + 1);
		if (IS_ERR(group_id))
			return PTR_ERR(group_id);
	}

	if (work->feature_id && work->feature_id_len > 0) {
		if (work->feature_id_len > NPU_MAX_WORKINFO_FIELD_LEN) {
			kfree(group_id);
			return -EINVAL;
		}
		feature_id = strndup_user(
			(void __user *)(uintptr_t)work->feature_id,
			work->feature_id_len + 1);
		if (IS_ERR(feature_id)) {
			kfree(group_id);
			return PTR_ERR(feature_id);
		}
	}

	/* Allocate the wnode node; kthread will kfree it */
	wnode = kzalloc(sizeof(*wnode), GFP_KERNEL);
	if (!wnode) {
		kfree(group_id);
		kfree(feature_id);
		return -ENOMEM;
	}

	/* Initialize wnode fields from the IOCTL payload */
	wnode->handle = work->handle;
	wnode->work_type = work->type;
	wnode->app_id = work->appid;

	/*
	 * Scale the wnode priority to a 1000-unit range using the
	 * HANDLE_PRIORITY_SUPPORT capability from the DSP attribute table.
	 * new_priority = original_priority * (1000 / capability)
	 * Multiply before divide to avoid integer truncation.
	 * If the capability is 0 or not populated, use the raw priority.
	 */

	cap = fl->cctx->dsp_attributes[HANDLE_PRIORITY_SUPPORT];
	if (cap > 0)
		wnode->work_prio = (u32)((u64)work->priority * 1000 / cap);
	else
		wnode->work_prio = work->priority;

	wnode->group_id = group_id;
	wnode->feature_id = feature_id;
	wnode->work_id = (u32)atomic_fetch_add(1, &sched->workid_seq) & INT_MAX;
	wnode->fl = fl;
	atomic_set(&wnode->state, WORK_STATE_INCOMING);
	/*
	 * Default end_reason for normal completion (work_remove path).
	 * fastrpc_scheduler_user_cleanup() overrides this to
	 * NPU_WORK_REASON_END_CANCELLED for cleanup-driven DONE transitions.
	 */
	wnode->end_reason = NPU_WORK_REASON_END_COMPLETED;

	/*
	 * Point into the caller's stack frame.  These pointers remain
	 * valid until wait_for_completion returns below.
	 */
	wnode->wait_done = &done;
	wnode->result = &result;

	/*
	 * Place wnode on the incoming_list, per-user list, and per-app
	 * hashtable.  The hashtable insert happens here (not in the
	 * kthread) so that fastrpc_work_remove() can find the work by
	 * (handle, app_id) even before the kthread processes it.
	 */
	spin_lock(&sched->lock);
	{
		struct fastrpc_app_entry *entry;

		/*
		 * Defense-in-depth: reject new work if abort_all() has
		 * already stopped the scheduler (SSR teardown).  This
		 * closes the narrow window where an IOCTL passes the
		 * cctx->teardown gate before it is set but arrives here
		 * after abort_all() has already scanned the incoming_list.
		 */
		if (READ_ONCE(sched->stop)) {
			spin_unlock(&sched->lock);
			pr_debug("%s: handle=0x%llx scheduler stopped, rejecting\n",
				 __func__, work->handle);
			fastrpc_work_node_free(wnode);
			return -ESHUTDOWN;
		}

		/* Reject duplicate: same handle already active for this app */
		if (fastrpc_find_work(sched, work->handle, work->appid)) {
			spin_unlock(&sched->lock);
			pr_err("%s: handle=0x%llx app_id=%d rejected, duplicate\n",
				__func__, work->handle, work->appid);
			fastrpc_work_node_free(wnode);
			return -EEXIST;
		}

		entry = fastrpc_app_entry_get(sched, work->appid);
		if (!entry) {
			spin_unlock(&sched->lock);
			pr_debug("%s: handle=0x%llx app_id=%d, app_entry alloc failed\n",
				__func__, work->handle, work->appid);
			fastrpc_work_node_free(wnode);
			return -ENOMEM;
		}
		list_add_tail(&wnode->app_node, &entry->works);
		list_add_tail(&wnode->staging_node,
			      &sched->incoming_list);
		list_add_tail(&wnode->user_node, &fl->sched_works);
		pr_debug("%s: handle=0x%llx app_id=%d raw_prio=%u work_prio=%u group=%s feature=%s -> INCOMING\n",
			__func__, work->handle, work->appid, work->priority,
			wnode->work_prio,
			wnode->group_id ? wnode->group_id : "(none)",
			wnode->feature_id ? wnode->feature_id : "(none)");
	}
	spin_unlock(&sched->lock);

	/* Wake the kthread to drain the incoming_list */
	wake_up(&sched->wq);

	/*
	 * Block until the kthread admits the work (result == 0) or
	 * the abort path cancels it (result == -ECANCELED).
	 * After this returns, result is read from the stack.
	 * The work node may already be freed by the kthread.
	 */
	wait_for_completion(&done);

	pr_debug("%s: handle=0x%llx app_id=%d result=%d\n",
		__func__, work->handle, work->appid, result);

	return result;
}

/*
 * IOCTL handler for FASTRPC_INVOKE_REMOTE_WORK (status=END).
 *
 * Looks up the work by (handle, app_id) in the per-app hashtable — a
 * single lookup that works regardless of work state because all works
 * are inserted into the hashtable at work_add time.  Then switches on
 * the atomic state to decide the removal path:
 *
 *   INCOMING -> ABORTED: unblock waiter with -ECANCELED.  Leave on
 *                        incoming_list; kthread kfrees during drain.
 *   PENDING  -> ABORTED: unblock waiter, move to abort_list.  Kthread
 *                        will rb_erase + kfree.
 *   ADMITTED -> DONE:    stage on done_list.  Kthread removes from
 *                        executing_list, recalcs ref_prio, sends
 *                        WORK_ENDED notification, and kfrees.
 *   ABORTED / DONE:      already being cleaned up; return -ENOENT.
 *
 * Returns 0 on success, -ENOENT if work not found or already removed.
 * This function does NOT call kfree.  The kthread is the sole owner.
 */
int fastrpc_work_remove(struct fastrpc_user *fl,
		       struct fastrpc_ioctl_remote_work *work)
{
	struct fastrpc_work_node *wnode;
	struct fastrpc_scheduler *sched = &fl->cctx->scheduler;
	int old, state;

	spin_lock(&sched->lock);

	/* Single hashtable lookup — finds works in any state */
	wnode = fastrpc_find_work(sched, work->handle, work->appid);
	if (!wnode) {
		spin_unlock(&sched->lock);
		pr_debug("%s: handle=0x%llx app_id=%d not found\n",
			__func__, work->handle, work->appid);
		return -ENOENT;
	}

	state = atomic_read(&wnode->state);
	switch (state) {
	case WORK_STATE_INCOMING:
		/*
		 * Work is on incoming_list, kthread has not
		 * processed it.  Transition to ABORTED and
		 * unblock the waiter.  The wnode stays on the
		 * incoming_list; the kthread will see the
		 * ABORTED state during drain and kfree it.
		 */
		old = atomic_cmpxchg(&wnode->state,
				     WORK_STATE_INCOMING,
				     WORK_STATE_ABORTED);
		if (old != WORK_STATE_INCOMING)
			break;
		pr_debug("%s: handle=0x%llx app_id=%d INCOMING -> ABORTED\n",
			__func__, wnode->handle, wnode->app_id);
		/*
		 * Remove user_node now to protect against fl being freed
		 * before the kthread drains incoming_list and calls
		 * list_del_init(&work->user_node) in fastrpc_drain_incoming().
		 */
		list_del_init(&wnode->user_node);
		WRITE_ONCE(*wnode->result, -ECANCELED);
		complete(wnode->wait_done);
		spin_unlock(&sched->lock);
		wake_up(&sched->wq);
		return 0;

	case WORK_STATE_PENDING:
		/*
		 * Work is in rbtree + hashtable.  Transition to
		 * ABORTED, move to abort_list for the kthread
		 * to rb_erase + kfree.  Unblock the waiter.
		 */
		old = atomic_cmpxchg(&wnode->state,
				     WORK_STATE_PENDING,
				     WORK_STATE_ABORTED);
		if (old != WORK_STATE_PENDING)
			break;
		pr_debug("%s: handle=0x%llx app_id=%d PENDING -> ABORTED\n",
			__func__, wnode->handle, wnode->app_id);
		/*
		 * Remove user_node now to protect against fl being freed
		 * before the kthread drains abort_list and calls
		 * list_del_init(&work->user_node) in fastrpc_drain_abort_list().
		 */
		list_del_init(&wnode->user_node);
		list_add_tail(&wnode->staging_node,
			      &sched->abort_list);
		WRITE_ONCE(*wnode->result, -ECANCELED);
		complete(wnode->wait_done);
		spin_unlock(&sched->lock);
		wake_up(&sched->wq);
		return 0;

	case WORK_STATE_ADMITTED:
		/*
		 * Work was admitted and is on executing_list.
		 * Transition to DONE and stage on done_list.
		 * The kthread performs the structural cleanup
		 * (exec_node removal, ref_prio recalc, notification)
		 * and kfree via fastrpc_drain_done_list.
		 * user_node is removed here to protect against fl
		 * being freed before the kthread processes done_list.
		 */
		old = atomic_cmpxchg(&wnode->state,
				     WORK_STATE_ADMITTED,
				     WORK_STATE_DONE);
		if (old != WORK_STATE_ADMITTED)
			break;
		pr_debug("%s: handle=0x%llx app_id=%d ADMITTED -> DONE\n",
			__func__, wnode->handle, wnode->app_id);
		list_del_init(&wnode->user_node);
		list_add_tail(&wnode->staging_node,
			      &sched->done_list);
		spin_unlock(&sched->lock);
		wake_up(&sched->wq);
		return 0;

	default:
		/* ABORTED or DONE: already being cleaned up */
		break;
	}

	spin_unlock(&sched->lock);
	return -ENOENT;
}

/*
 * Initialize per-channel scheduler state and start the scheduler
 * kthread. Called during channel context probe (rpmsg).
 */
int fastrpc_scheduler_init(struct fastrpc_scheduler *sched)
{
	sched->pending_tree = RB_ROOT;
	INIT_LIST_HEAD(&sched->executing_list);
	INIT_LIST_HEAD(&sched->incoming_list);
	INIT_LIST_HEAD(&sched->abort_list);
	INIT_LIST_HEAD(&sched->done_list);
	hash_init(sched->app_works);
	spin_lock_init(&sched->lock);
	init_waitqueue_head(&sched->wq);
	sched->ref_prio = U32_MAX;
	atomic_set(&sched->workid_seq, 0);
	sched->prio_update_pending = false;
	sched->stop = false;
	sched->kthread = NULL;

	sched->kthread = kthread_run(fastrpc_scheduler_thread, sched,
				     "fastrpc_sched");
	if (IS_ERR(sched->kthread)) {
		int ret = PTR_ERR(sched->kthread);

		pr_err("%s: kthread creation failed: %d\n",
		       __func__, ret);
		sched->kthread = NULL;
		return ret;
	}

	pr_info("%s: kthread started\n", __func__);
	return 0;
}

/*
 * Abort all blocked scheduler waiters during SSR.
 *
 * Stops the kthread, then walks incoming_list and pending_tree to
 * unblock any ioctl threads stuck in fastrpc_work_add() with
 * -ECANCELED.  Must be called BEFORE waiting on invoke_cnt in
 * fastrpc_rpmsg_remove() to break the circular dependency:
 *
 *   fastrpc_work_add() holds invoke_cnt while waiting for a completion
 *   that only fastrpc_scheduler_deinit() signals, but deinit is ordered
 *   after the invoke_cnt wait, creating a deadlock when the DSP goes
 *   down with a START ioctl in flight.
 *
 * Work nodes are NOT freed here — fastrpc_scheduler_deinit() handles
 * structural cleanup (kfree, rb_erase, hashtable teardown) after
 * invoke_cnt drains and all ioctl stack frames have unwound.
 *
 * Uses atomic_cmpxchg for state transitions, matching the pattern in
 * fastrpc_scheduler_user_cleanup(), so concurrent work_remove() or
 * user_cleanup() calls are safe.
 */
void fastrpc_scheduler_abort_all(struct fastrpc_scheduler *sched)
{
	struct fastrpc_work_node *work, *wtmp;
	struct rb_node *node, *next;

	/*
	 * Stop the kthread to prevent new admissions or state changes.
	 * Set sched->stop = true first to reject any in-flight work_add() calls.
	 * Then call kthread_stop() to signal
	 * the kthread to exit via kthread_should_stop().
	 */
	WRITE_ONCE(sched->stop, true);
	if (sched->kthread && !IS_ERR(sched->kthread)) {
		kthread_stop(sched->kthread);
		sched->kthread = NULL;
	}
	pr_info("%s: kthread stopped, aborting all waiters\n", __func__);

	spin_lock(&sched->lock);

	/*
	 * Unblock INCOMING waiters.  Nodes stay on incoming_list;
	 * fastrpc_scheduler_deinit() will kfree them after invoke_cnt
	 * drains.
	 */
	list_for_each_entry_safe(work, wtmp,
				 &sched->incoming_list,
				 staging_node) {
		if (atomic_cmpxchg(&work->state,
				   WORK_STATE_INCOMING,
				   WORK_STATE_ABORTED) == WORK_STATE_INCOMING) {
			pr_debug("%s: handle=0x%llx app_id=%d INCOMING -> ABORTED\n",
				 __func__, work->handle, work->app_id);
			WRITE_ONCE(*work->result, -ECANCELED);
			complete(work->wait_done);
		}
	}

	/*
	 * Unblock PENDING waiters.  Nodes stay in the rbtree;
	 * fastrpc_scheduler_deinit() will rb_erase + kfree them.
	 */
	for (node = rb_first(&sched->pending_tree); node;
	     node = next) {
		next = rb_next(node);
		work = rb_entry(node, struct fastrpc_work_node,
			       rb_node);
		if (atomic_cmpxchg(&work->state,
				   WORK_STATE_PENDING,
				   WORK_STATE_ABORTED) == WORK_STATE_PENDING) {
			pr_debug("%s: handle=0x%llx app_id=%d PENDING -> ABORTED\n",
				 __func__, work->handle, work->app_id);
			WRITE_ONCE(*work->result, -ECANCELED);
			complete(work->wait_done);
		}
	}

	spin_unlock(&sched->lock);
}

/*
 * Tear down the scheduler: stop the kthread, then clean up all
 * remaining works across every data structure.  Waiters still blocked
 * in fastrpc_work_add() are unblocked with -ECANCELED.
 *
 * After kthread_stop() returns, the kthread is guaranteed to have
 * exited, so no concurrent access to scheduler state is possible.
 * All cleanup below runs single-threaded without locks.
 */
void fastrpc_scheduler_deinit(struct fastrpc_scheduler *sched)
{
	struct fastrpc_work_node *work, *wtmp;
	struct fastrpc_app_entry *entry;
	struct hlist_node *htmp;
	struct rb_node *node, *next;
	int bkt;

	/*
	 * Signal the kthread to stop and wait for it to exit.
	 * Set sched->stop = true first to reject any in-flight work_add() calls.
	 * Then call kthread_stop() to signalthe kthread to exit via kthread_should_stop().
	 * Only call kthread_stop() if the kthread is still valid (not already stopped
	 * by abort_all()).
	 */
	WRITE_ONCE(sched->stop, true);
	if (sched->kthread && !IS_ERR(sched->kthread)) {
		kthread_stop(sched->kthread);
		sched->kthread = NULL;
	}
	pr_info("%s: kthread stopped\n", __func__);

	/*
	 * Drain incoming_list: unblock any waiters whose works were
	 * never processed by the kthread, then free the nodes.
	 * Works are in the hashtable since work_add, so remove app_node.
	 */
	list_for_each_entry_safe(work, wtmp,
				 &sched->incoming_list,
				 staging_node) {
		list_del(&work->staging_node);
		if (atomic_read(&work->state) == WORK_STATE_INCOMING) {
			WRITE_ONCE(*work->result, -ECANCELED);
			complete(work->wait_done);
		}
		list_del(&work->app_node);
		list_del_init(&work->user_node);
		fastrpc_work_node_free(work);
	}

	/*
	 * Drain abort_list: these PENDING works were marked ABORTED
	 * and their waiters already unblocked.  Remove from rbtree
	 * and per-app list, then free.
	 */
	list_for_each_entry_safe(work, wtmp,
				 &sched->abort_list,
				 staging_node) {
		list_del(&work->staging_node);
		rb_erase(&work->rb_node, &sched->pending_tree);
		list_del(&work->app_node);
		list_del_init(&work->user_node);
		fastrpc_work_node_free(work);
	}

	/* Drain done_list: clean up and free completed executing works */
	list_for_each_entry_safe(work, wtmp,
				 &sched->done_list,
				 staging_node) {
		list_del(&work->staging_node);
		list_del(&work->exec_node);
		list_del(&work->app_node);
		list_del_init(&work->user_node);
		/*
		 * Send WORK_ENDED during teardown so the HAL's event
		 * stream is not silently truncated.  The consumer may
		 * have already returned -EPIPE; the nodes will be freed
		 * by the workinfo queue drain that follows deinit.
		 */
		fastrpc_workinfo_notify(sched, work, WORK_ENDED,
					work->end_reason);
		fastrpc_work_node_free(work);
	}

	/*
	 * Drain remaining pending works in the rbtree.  These were
	 * PENDING but never admitted or aborted.  Unblock their
	 * waiters with -ECANCELED and free the nodes.
	 */
	for (node = rb_first(&sched->pending_tree); node;
	     node = next) {
		next = rb_next(node);
		work = rb_entry(node, struct fastrpc_work_node,
			       rb_node);
		rb_erase(node, &sched->pending_tree);
		list_del(&work->app_node);
		/*
		 * If fastrpc_scheduler_abort_all() already transitioned
		 * this work to ABORTED and signalled the waiter, do not
		 * call complete() again.
		 */
		if (atomic_read(&work->state) != WORK_STATE_ABORTED) {
			WRITE_ONCE(*work->result, -ECANCELED);
			complete(work->wait_done);
		}
		list_del_init(&work->user_node);
		fastrpc_work_node_free(work);
	}

	/*
	 * Drain executing_list: these admitted works have no waiter
	 * (the work_add caller already returned 0).  Just free them.
	 */
	list_for_each_entry_safe(work, wtmp,
				 &sched->executing_list,
				 exec_node) {
		list_del(&work->exec_node);
		list_del(&work->app_node);
		list_del_init(&work->user_node);
		fastrpc_work_node_free(work);
	}

	/* Free all per-app hashtable entries */
	hash_for_each_safe(sched->app_works, bkt, htmp,
			   entry, hash_node) {
		hash_del(&entry->hash_node);
		kfree(entry);
	}
}

/*
 * Abort or remove all scheduler works owned by a specific user.
 *
 * Called during fd close or process exit cleanup.  Walks the per-user
 * work list (fl->sched_works) and for each work performs the same state
 * transitions as fastrpc_work_remove():
 *
 *   INCOMING -> ABORTED: unblock waiter, leave on incoming_list for
 *                        kthread to kfree during drain.
 *   PENDING  -> ABORTED: unblock waiter, move to abort_list for
 *                        kthread to rb_erase + kfree.
 *   ADMITTED -> DONE:    stage on done_list; kthread removes from
 *                        executing_list, recalcs ref_prio, sends
 *                        WORK_ENDED notification, and kfrees.
 *
 * Works in ABORTED or DONE state are already being cleaned up and
 * are skipped.
 */
void fastrpc_scheduler_user_cleanup(struct fastrpc_user *fl)
{
	struct fastrpc_scheduler *sched = &fl->cctx->scheduler;
	struct fastrpc_work_node *work, *tmp;
	int old;

	spin_lock(&sched->lock);

	list_for_each_entry_safe(work, tmp, &fl->sched_works,
				 user_node) {
		int state = atomic_read(&work->state);

		switch (state) {
		case WORK_STATE_INCOMING:
			/*
			 * Work is on incoming_list, kthread has not
			 * processed it.  Transition to ABORTED and
			 * unblock the waiter.  The kthread will
			 * kfree during incoming_list drain.
			 */
			old = atomic_cmpxchg(&work->state,
					     WORK_STATE_INCOMING,
					     WORK_STATE_ABORTED);
			if (old == WORK_STATE_INCOMING) {
				pr_debug("%s: handle=0x%llx app_id=%d INCOMING -> ABORTED\n",
					__func__, work->handle, work->app_id);
				list_del_init(&work->user_node);
				WRITE_ONCE(*work->result,
					   -ECANCELED);
				complete(work->wait_done);
			}
			break;
		case WORK_STATE_PENDING:
			/*
			 * Work is in rbtree + per-app list.
			 * Transition to ABORTED, move to abort_list
			 * for the kthread to rb_erase + kfree.
			 * Unblock the waiter.
			 */
			old = atomic_cmpxchg(&work->state,
					     WORK_STATE_PENDING,
					     WORK_STATE_ABORTED);
			if (old == WORK_STATE_PENDING) {
				pr_debug("%s: handle=0x%llx app_id=%d PENDING -> ABORTED\n",
					__func__, work->handle, work->app_id);
				list_del_init(&work->user_node);
				list_add_tail(&work->staging_node,
					      &sched->abort_list);
				WRITE_ONCE(*work->result,
					   -ECANCELED);
				complete(work->wait_done);
			}
			break;
		case WORK_STATE_ADMITTED:
			/*
			 * Work was admitted and is on executing_list.
			 * Transition to DONE and stage on done_list.
			 * The kthread performs structural cleanup
			 * and kfree via fastrpc_drain_done_list.
			 * user_node is removed here because fl may
			 * be freed before the kthread processes it.
			 * Mark as cancelled: this is an fd-close or SSR
			 * path, not a normal work_remove completion.
			 */
			old = atomic_cmpxchg(&work->state,
					     WORK_STATE_ADMITTED,
					     WORK_STATE_DONE);
			if (old == WORK_STATE_ADMITTED) {
				pr_debug("%s: handle=0x%llx app_id=%d ADMITTED -> DONE\n",
					__func__, work->handle, work->app_id);
				work->end_reason = NPU_WORK_REASON_END_CANCELLED;
				list_del_init(&work->user_node);
				list_add_tail(&work->staging_node,
					      &sched->done_list);
			}
			break;
		default:
			/* ABORTED or DONE: already being cleaned up */
			break;
		}
	}

	spin_unlock(&sched->lock);

	/* Wake kthread to process abort_list and done_list */
	wake_up(&sched->wq);
}

/*
 * Check if a work for the given (fl, handle) pair is currently in the
 * executing list.  Returns true if a matching admitted work is found,
 * false otherwise.
 *
 * Called from fastrpc_internal_invoke() to enforce that an untrusted
 * process may only invoke handles that have an active scheduler work.
 */
bool fastrpc_scheduler_handle_is_executing(struct fastrpc_scheduler *sched,
					   struct fastrpc_user *fl,
					   u64 handle)
{
	struct fastrpc_work_node *work;
	bool found = false;

	spin_lock(&sched->lock);
	list_for_each_entry(work, &sched->executing_list, exec_node) {
		if (work->fl == fl && work->handle == handle &&
		    atomic_read(&work->state) == WORK_STATE_ADMITTED) {
			found = true;
			break;
		}
	}
	spin_unlock(&sched->lock);

	return found;
}
