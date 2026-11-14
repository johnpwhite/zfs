// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 */

/*
 * Copyright (c) 2026, John White.
 */

/*
 * Private definitions of the async mirror, shared by the files
 * module/zfs/vdev_async_mirror*.c.  The public interface is in
 * sys/vdev_async_mirror.h.
 */

#ifndef _SYS_VDEV_ASYNC_MIRROR_IMPL_H
#define	_SYS_VDEV_ASYNC_MIRROR_IMPL_H

#include <sys/spa.h>
#include <sys/vdev_impl.h>
#include <sys/uberblock_impl.h>
#include <sys/vdev_async_mirror.h>
#include <sys/dmu_tx.h>
#include <sys/range_tree.h>
#include <sys/space_map.h>
#include <sys/zthr.h>
#include <sys/zio.h>

#ifdef	__cplusplus
extern "C" {
#endif

/* The kind of error that stopped the copy of a txg. */
typedef enum async_mirror_err {
	ASYNC_MIRROR_ERR_WRITE,	/* a write to the lagging child */
	ASYNC_MIRROR_ERR_META,	/* a read of the walk: a metadata block */
	ASYNC_MIRROR_ERR_STOP,	/* the pool stops: copy again at import */
} async_mirror_err_t;

/*
 * A data block of a record that no copy could supply on the last try of the
 * copy, and that is tried again before the record is released.  See
 * "Deferred blocks" below.
 */
typedef struct async_mirror_defer {
	list_node_t	ad_node;
	blkptr_t	ad_bp;
	zbookmark_phys_t ad_zb;
	boolean_t	ad_done;	/* recovered, or reported as lost */
} async_mirror_defer_t;

/* The MOS entry of a deferred block: the txg, the bp and the bookmark. */
#define	ASYNC_MIRROR_DEFER_WORDS	(1 + \
	sizeof (blkptr_t) / sizeof (uint64_t) + \
	sizeof (zbookmark_phys_t) / sizeof (uint64_t))

/* One record per txg that has changes. */
typedef struct async_mirror_txg {
	list_node_t	at_node;
	uint64_t	at_txg;
	uint64_t	at_obj;		/* space map object */
	space_map_t	*at_sm;		/* open only while being written */
	uint64_t	at_sm_len;	/* its length, once at_ready is set */
	uint64_t	at_held;	/* bytes held in this txg */
	uint64_t	at_bytes;	/* bytes to copy, from SM_ALLOC */
	boolean_t	at_ready;	/* the txg has fully synced */
	boolean_t	at_release;	/* only a release changed this txg */
	uberblock_t	at_ub;		/* valid once at_ready is set */
	hrtime_t	at_ready_time;
	list_t		at_defer;	/* the deferred blocks of the txg */
	uint_t		at_ndefer_count; /* deferred blocks in at_defer */
	uint_t		at_ndefer;	/* deferred blocks not yet done */
	uint_t		at_defer_tries;	/* tries of them made so far */
} async_mirror_txg_t;

/*
 * The life cycle of the async mirror.  The states from AMS_ACTIVE to AMS_FAILED
 * have a lagging child: am_lagging and am_leader are set.
 *
 *	OFF -> STARTING -> ACTIVE -> PASSTHROUGH -> DRAINING
 *	                     |           |
 *	                     +--> FROZEN, FAILED -> DRAINING
 *
 * A frozen child becomes a failed one when the property turns the async mirror
 * off.  An import enters ACTIVE, FROZEN or DRAINING directly.
 */
typedef enum async_mirror_state {
	AMS_OFF = 0,		/* no lagging child, no record */
	AMS_STARTING,		/* the switch on waits for am_on_txg */
	AMS_ACTIVE,		/* writes skip the child; the worker copies */
	AMS_PASSTHROUGH,	/* writes reach the child; the copy ends */
	AMS_FROZEN,		/* the child stays at am_applied_txg, no I/O */
	AMS_FAILED,		/* the copy stopped; the child is resilvered */
	AMS_DRAINING,		/* ended; the records are released */
} async_mirror_state_t;

/* What the worker does with the deferred blocks of the txg it copies. */
typedef enum async_mirror_persist {
	AMP_NONE = 0,
	AMP_WAIT,		/* it waits for the pool threads to run */
	AMP_SYNC,		/* it waits for the txg that stores them */
} async_mirror_persist_t;

/*
 * The state of the async mirror of a pool.  am_lock protects each
 * field, with these exceptions:
 *
 * am_spa is constant.  am_release_zthr is set before the threads start,
 * and cleared at export after they stop.
 *
 * am_state, am_lagging and am_leader change under am_lock.  am_lagging and
 * am_leader change only in syncing context, at the start of a txg, or at
 * load, so the sync thread and the free path read them with no lock.  A
 * reader with no lock reads am_state with atomic_load_32(), and the I/O
 * path reads the copy in the lagging leaf; see async_mirror_set_state().
 *
 * The write throttle reads am_pending_bytes, am_pending_txgs, am_thread and
 * am_stop with no lock, as it reads the dirty data of the pool: a stale value
 * only changes the delay of one transaction; see vdev_async_mirror_delay().
 *
 * The objects in the MOS (am_zap_obj, am_defer_zap, am_off_entry,
 * am_frozen_entry), am_claim_rt, am_claim_txg, am_last_ub, and at_sm of a
 * record change in syncing context or at load, with am_lock held when
 * another thread can read them.  The sync thread reads them with no lock.
 */
typedef struct vdev_async_mirror {
	spa_t		*am_spa;
	kmutex_t	am_lock;
	kcondvar_t	am_cv;		/* the worker waits for work */
	kcondvar_t	am_drain_cv;	/* waits for progress of the worker */
	uint32_t	am_state;	/* async_mirror_state_t */
	boolean_t	am_off;		/* the property asks to leave */
	vdev_t		*am_leader;
	vdev_t		*am_lagging;
	uint64_t	am_on_guid;	/* AMS_STARTING: the leaf to start */
	uint64_t	am_on_txg;	/* AMS_STARTING: txg of the switch */
	uint64_t	am_pass_txg;	/* newest txg that can miss a write */

	/* The records and the copy. */
	list_t		am_txgs;	/* sorted by txg */
	uint64_t	am_zap_obj;	/* the record ZAP, or 0 */
	uberblock_t	am_last_ub;	/* uberblock of the newest record */
	uint64_t	am_applied_txg; /* newest txg on the lagging child */
	uint64_t	am_synced_txg;	/* newest txg on disk */
	uint64_t	am_pending_bytes;
	uint64_t	am_pending_txgs;
	uint64_t	am_held_bytes;
	uint64_t	am_held_dspace;
	uint64_t	am_held_log_bytes; /* held on log devices */
	zfs_range_tree_t *am_claim_rt;	/* log blocks to list at the claim */
	uint64_t	am_claim_txg;	/* the claim txg */
	kthread_t	*am_thread;	/* the worker; it clears this at exit */
	boolean_t	am_stop;	/* the worker must exit */

	/* The end of a copy that froze or failed. */
	uint64_t	am_fail_from;	/* oldest txg that the child lacks */
	uint64_t	am_fail_open_txg; /* newest txg that can miss a write */
	uint64_t	am_halt_txg;	/* txg that records the halt, or 0 */
	uint64_t	am_dtl_txg;	/* txg that sets the DTLs, or 0 */
	boolean_t	am_frozen_entry; /* the MOS has the freeze entry */
	boolean_t	am_off_entry;	/* the MOS has the switch-off entry */

	/* The release thread. */
	zthr_t		*am_release_zthr;
	uint64_t	am_last_release_txg;
	uint64_t	am_release_only_txg; /* txg changed only by a release */
	uint64_t	am_release_queued_txg; /* txg of the queued release */
	boolean_t	am_release_more; /* the last release hit its limit */

	/* The deferred blocks. */
	uint64_t	am_defer_zap;	/* the deferred blocks in the MOS */
	uint64_t	am_defer_txg;	/* oldest txg with them, 0 for none */
	uint32_t	am_persist;	/* async_mirror_persist_t */
	uint_t		am_quiesce;	/* the pool threads are suspended */
	boolean_t	am_defer_io;	/* deferred blocks are being written */
	boolean_t	am_defer_failed; /* a deferred block write failed */
	boolean_t	am_defer_quick;	/* defer a lost data block at once */
} vdev_async_mirror_t;

/* True in the states that have a lagging child. */
static inline boolean_t
async_mirror_has_child(uint32_t state)
{
	return (state == AMS_ACTIVE || state == AMS_PASSTHROUGH ||
	    state == AMS_FROZEN || state == AMS_FAILED);
}

/* True while the worker copies the records to the lagging child. */
static inline boolean_t
async_mirror_copying(uint32_t state)
{
	return (state == AMS_ACTIVE || state == AMS_PASSTHROUGH);
}

/* True when every record is released, copied or not. */
static inline boolean_t
async_mirror_draining(uint32_t state)
{
	return (state == AMS_FROZEN || state == AMS_FAILED ||
	    state == AMS_DRAINING);
}

/* True once the txg is on disk.  A txg of 0 has not happened. */
static inline boolean_t
async_mirror_durable(const vdev_async_mirror_t *am, uint64_t txg)
{
	return (txg != 0 && txg <= am->am_synced_txg);
}

/* The delay before each try of a copy after a read error. */
#define	ASYNC_MIRROR_RETRY_MS	3000

/* Set in vdev_async_mirror.c. */
extern uint_t zfs_vdev_async_mirror_read_retries;
extern uint64_t zfs_vdev_async_mirror_copy_limit;

/* In vdev_async_mirror.c: the state, the stats, the threads, the marks. */
extern void async_mirror_set_state(vdev_async_mirror_t *,
    async_mirror_state_t);
extern void async_mirror_update_stats(vdev_async_mirror_t *);
extern __attribute__((noreturn)) void async_mirror_thread(void *);
extern boolean_t async_mirror_release_thread_check(void *, zthr_t *);
extern void async_mirror_release_thread(void *, zthr_t *);
extern vdev_async_mirror_t *async_mirror_create(spa_t *);
extern void async_mirror_mark(spa_t *, vdev_t *, uint64_t);
extern void async_mirror_fail_sync(vdev_async_mirror_t *, dmu_tx_t *);
extern void async_mirror_publish_txg(vdev_async_mirror_t *);
extern void async_mirror_mark_dtl(spa_t *, vdev_t *, uint64_t, uint64_t);
extern void async_mirror_note_resync(spa_t *, vdev_t *);
extern boolean_t async_mirror_child_usable(vdev_t *);
extern boolean_t async_mirror_child_needs_resilver(spa_t *, vdev_t *);
extern void async_mirror_read_leaf_ub(spa_t *, vdev_t *, uberblock_t *,
    uberblock_t *);
extern int async_mirror_can_start(spa_t *, vdev_t *, vdev_t **);

/* In vdev_async_mirror_copy.c: the copy, the deferred blocks, the halt. */
extern int async_mirror_copy(vdev_async_mirror_t *, async_mirror_txg_t *,
    boolean_t, async_mirror_err_t *, list_t *, uint_t *);
extern int async_mirror_commit(vdev_async_mirror_t *, async_mirror_txg_t *,
    boolean_t, list_t *, uint_t, async_mirror_err_t *);
extern void async_mirror_retry_event(vdev_async_mirror_t *, uint64_t, int);
extern boolean_t async_mirror_retry_wait(vdev_async_mirror_t *);
extern void async_mirror_halt(vdev_async_mirror_t *, uint64_t, int,
    async_mirror_err_t);
extern void async_mirror_defer_list_create(list_t *);
extern void async_mirror_defer_list_free(list_t *);
extern void async_mirror_defer_floor(vdev_async_mirror_t *);
extern void async_mirror_defer_drop(vdev_async_mirror_t *,
    async_mirror_txg_t *);
extern void async_mirror_defer_clear(vdev_async_mirror_t *, uint64_t,
    dmu_tx_t *);
extern async_mirror_txg_t *async_mirror_defer_next(vdev_async_mirror_t *);
extern void async_mirror_defer_retry(vdev_async_mirror_t *, zthr_t *);

/* In vdev_async_mirror_lineage.c: the lineage, the keep point, the erase. */
extern void async_mirror_lineage_start(spa_t *);
extern void async_mirror_lineage_update(spa_t *, boolean_t);
extern int async_mirror_erase_skipped(spa_t *);
extern void async_mirror_mark_erased(spa_t *);

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_VDEV_ASYNC_MIRROR_IMPL_H */
