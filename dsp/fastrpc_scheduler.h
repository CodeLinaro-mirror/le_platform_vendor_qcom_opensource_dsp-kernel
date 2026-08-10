/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */

#ifndef __FASTRPC_SCHEDULER_H__
#define __FASTRPC_SCHEDULER_H__

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/hashtable.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>

#define FASTRPC_APP_HASH_BITS	8

struct task_struct;
struct fastrpc_user;

/*
 * Work lifecycle states, transitioned atomically via atomic_cmpxchg.
 *
 * INCOMING  - work is on incoming_list, kthread has not processed it yet
 * PENDING   - work is in the pending rbtree + per-app hashtable list
 * ADMITTED  - work passed admission, on executing_list, caller unblocked
 * ABORTED   - abort requested before admission; caller unblocked
 * DONE      - executing work completed via work_remove; awaiting kfree
 */
enum fastrpc_work_state {
	WORK_STATE_INCOMING	= 0,
	WORK_STATE_PENDING	= 1,
	WORK_STATE_ADMITTED	= 2,
	WORK_STATE_ABORTED	= 3,
	WORK_STATE_DONE		= 4,
};

/*
 * Node representing one pending or executing work.
 *
 * Lifetime: allocated by fastrpc_work_add(), freed exclusively by the
 * scheduler kthread.  The IOCTL caller never calls kfree on this.
 *
 * The caller's stack holds a DECLARE_COMPLETION_ONSTACK and an int result.
 * Pointers wait_done / result point into that stack frame.  The kthread
 * writes *result and calls complete(wait_done) to unblock the caller,
 * and may kfree this node at any later time — the stack variables
 * outlive the work struct because the caller only returns after
 * wait_for_completion().
 */
struct fastrpc_work_node {
	struct rb_node		rb_node;	/* rbtree linkage (pending_tree) */
	struct list_head	exec_node;	/* executing_list linkage */
	struct list_head	app_node;	/* per-app work list linkage */
	struct list_head	staging_node;	/* incoming / abort / done list */
	struct list_head	user_node;	/* per-user work list linkage */
	struct fastrpc_user	*fl;		/* owning user; for per-user cleanup */
	u64			handle;		/* work handle; tiebreaker in tree */
	u32			work_type;	/* FASTRPC or DSPQUEUE */
	u32			work_prio;	/* work priority from userspace */
	u32			eff_prio;	/* app_prio + work_prio; sort key */
	int			app_id;		/* process tgid */
	char			*group_id;	/* inference group ID string */
	char			*feature_id;	/* feature ID string */
	u32			work_id;	/* monotonic id for workinfo correlation */
	atomic_t		state;		/* enum fastrpc_work_state */
	u32			end_reason;	/* NPU_WORK_REASON_END_* for WORK_ENDED */
	struct completion	*wait_done;	/* caller's on-stack completion */
	int			*result;	/* caller's on-stack return code */
};

/* Per-app entry in the scheduler hashtable */
struct fastrpc_app_entry {
	struct hlist_node	hash_node;	/* hashtable linkage */
	int			app_id;		/* process tgid */
	u32			cur_prio;	/* last applied app priority */
	struct list_head	works;		/* pending works for this app */
};

/*
 * Per-channel scheduler state.
 *
 * A single spinlock (lock) protects all scheduler data structures:
 * incoming_list, abort_list, done_list, pending_tree, executing_list,
 * app_works hashtable, ref_prio, and prio_update_pending.
 *
 * IOCTL callers hold the lock briefly for O(1) list_add (work_add) or
 * O(K) list scans (work_remove).  The kthread holds it for bulk
 * processing: draining staging lists, rbtree operations, and admission.
 */
struct fastrpc_scheduler {
	struct rb_root		pending_tree;	/* rbtree of pending works */
	struct list_head	executing_list;	/* currently executing works */
	struct list_head	incoming_list;	/* new works from IOCTL callers */
	struct list_head	abort_list;	/* aborted PENDING works for cleanup */
	struct list_head	done_list;	/* completed ADMITTED works for cleanup */
	DECLARE_HASHTABLE(app_works, FASTRPC_APP_HASH_BITS);
	spinlock_t		lock;		/* protects all scheduler state */
	wait_queue_head_t	wq;		/* wakes kthread */
	struct task_struct	*kthread;	/* scheduler worker thread */
	u32			ref_prio;	/* min eff_prio of executing works */
	atomic_t		workid_seq;	/* monotonic counter for workinfo id */
	bool			prio_update_pending;
	bool			stop;		/* set to stop kthread */
};

struct fastrpc_ioctl_remote_work;

#if IS_ENABLED(CONFIG_QCOM_FASTRPC_TRUSTED)

static inline int fastrpc_scheduler_init(struct fastrpc_scheduler *sched)
{ return 0; }

static inline void fastrpc_scheduler_abort_all(struct fastrpc_scheduler *sched)
{ return; }

static inline void fastrpc_scheduler_deinit(struct fastrpc_scheduler *sched)
{ return; }

static inline void fastrpc_scheduler_notify_prio_update(struct fastrpc_scheduler *sched)
{ return; }

static inline void fastrpc_scheduler_prio_update_begin(struct fastrpc_scheduler *sched)
{ return; }

static inline void fastrpc_scheduler_prio_update_finish(struct fastrpc_scheduler *sched)
{ return; }

static inline int fastrpc_work_add(struct fastrpc_user *fl,
				   struct fastrpc_ioctl_remote_work *work)
{ return 0; }

static inline int fastrpc_work_remove(struct fastrpc_user *fl,
				      struct fastrpc_ioctl_remote_work *work)
{ return 0; }

static inline void fastrpc_scheduler_user_cleanup(struct fastrpc_user *fl)
{ return; }

static inline bool fastrpc_scheduler_handle_is_executing(struct fastrpc_scheduler *sched,
							 struct fastrpc_user *fl,
							 u64 handle)
{ return false; }

#else

int fastrpc_scheduler_init(struct fastrpc_scheduler *sched);
void fastrpc_scheduler_abort_all(struct fastrpc_scheduler *sched);
void fastrpc_scheduler_deinit(struct fastrpc_scheduler *sched);
void fastrpc_scheduler_notify_prio_update(struct fastrpc_scheduler *sched);
void fastrpc_scheduler_prio_update_begin(struct fastrpc_scheduler *sched);
void fastrpc_scheduler_prio_update_finish(struct fastrpc_scheduler *sched);
int fastrpc_work_add(struct fastrpc_user *fl,
		    struct fastrpc_ioctl_remote_work *work);
int fastrpc_work_remove(struct fastrpc_user *fl,
		       struct fastrpc_ioctl_remote_work *work);
void fastrpc_scheduler_user_cleanup(struct fastrpc_user *fl);
bool fastrpc_scheduler_handle_is_executing(struct fastrpc_scheduler *sched,
					   struct fastrpc_user *fl,
					   u64 handle);

#endif /* IS_ENABLED(CONFIG_QCOM_FASTRPC_TRUSTED) */

#endif /* __FASTRPC_SCHEDULER_H__ */
