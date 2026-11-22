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

#ifndef _SYS_SPA_LAG_H
#define	_SYS_SPA_LAG_H

#include <sys/spa.h>
#include <sys/dmu.h>
#include <sys/range_tree.h>
#include <sys/avl.h>
#include <sys/nvpair.h>
#include <zfeature_common.h>

#ifdef	__cplusplus
extern "C" {
#endif

#define	SPA_LAG_MAX	2

typedef struct spa_lag spa_lag_t;

/*
 * On disk, only while a consumer is active:
 *
 *  - a MOS directory entry (the consumer's slo_dir_name) of
 *    SPA_LAG_DIRENT_WORDS uint64s: the version and the index ZAP object.
 *    It is written only when the store is created and removed when it is
 *    retired: open context may also write the MOS directory (the txg time
 *    log at unload), and a syncing-context write every txg would race it;
 *  - an index ZAP (uint64 keys): txg -> spa_lag_rec_phys_t, and under key
 *    SPA_LAG_STATE_KEY the state, SPA_LAG_DIR_WORDS uint64s indexed by the
 *    SLD_* values below (no record has txg 0);
 *  - one record object per txg (DMU_OTN_UINT64_METADATA): an array of
 *    entries of slo_entry_words uint64s each.  The entry format belongs to the
 *    consumer.
 *
 * Every word is a uint64, so the generic byteswap works.
 */
/*
 * Version 3 widens the state and record headers before this format is
 * released.  Older versions are refused because their state arrays are
 * shorter and do not have the new record fields.
 */
#define	SPA_LAG_VERSION		3
#define	SPA_LAG_DIRENT_WORDS	2
#define	SPA_LAG_STATE_KEY	0ULL
#define	SPA_LAG_DIR_WORDS	16
#define	SPA_LAG_DIR_RESERVED_START	9	/* first reserved state word */
#define	SLD_VERSION		0
#define	SLD_INDEX		1	/* index ZAP object */
#define	SLD_STATE		2	/* consumer state (consumer's values) */
#define	SLD_START		3	/* first recorded txg of this epoch */
#define	SLD_STOP		4	/* last ALLOC txg (draining) */
#define	SLD_GENERATION		5
#define	SLD_APPLIED		6	/* newest txg the outside applied */
#define	SLD_EPOCH		7	/* random id of this history */
#define	SLD_SET_GUID		8	/* owning parity set; 0: unclaimed */

/* pr_flags */
#define	SPA_LAG_REC_FIXUPS_INCOMPLETE	(1ULL << 0)

#define	SPA_LAG_REC_BLOCKSIZE	(128 * 1024)
/* Room for one uberblock_t; spa_lag.c checks the size. */
#define	SPA_LAG_UB_WORDS	32

typedef struct spa_lag_rec_phys {
	uint64_t	pr_version;
	uint64_t	pr_time;	/* record creation time, in seconds */
	uint64_t	pr_obj;		/* record object */
	uint64_t	pr_nentries;	/* entries written, VOIDs included */
	uint64_t	pr_nfixups;	/* leading entries: pr_prev_txg fixes */
	uint64_t	pr_alloc_asize;
	uint64_t	pr_held_asize;
	uint64_t	pr_flags;
	uint64_t	pr_rel_index;	/* release progress: next entry */
	uint64_t	pr_rel_asize;	/* release progress: bytes released */
	uint64_t	pr_prev_txg;	/* the record txg before this one */
	uint64_t	pr_prev_ub[SPA_LAG_UB_WORDS];
	uint64_t	pr_reserved[4];
} spa_lag_rec_phys_t;

#define	SPA_LAG_REC_WORDS	\
	(sizeof (spa_lag_rec_phys_t) / sizeof (uint64_t))

/* In memory: one per record that is not released yet. */
typedef struct spa_lag_rec {
	avl_node_t		lr_node;
	uint64_t		lr_txg;
	boolean_t		lr_dirty;	/* header changed in this txg */
	spa_lag_rec_phys_t	lr_phys;
} spa_lag_rec_t;

/* Flags of spa_lag_applied(). */
#define	SPA_LAG_FORCE	0x1	/* release now, do not wait for a write */

typedef enum spa_lag_level {
	SPA_LAG_LEVEL_NONE = 0,
	SPA_LAG_LEVEL_SOFT,
	SPA_LAG_LEVEL_HARD
} spa_lag_level_t;

/* What a consumer reports to the throttle. */
typedef struct spa_lag_usage {
	uint64_t	slu_record_bytes;	/* record space */
	uint64_t	slu_held_bytes;		/* held frees, not released */
	boolean_t	slu_limited;		/* the limits apply now */
} spa_lag_usage_t;

/* A sum over the unreleased records. */
typedef struct spa_lag_summary {
	uint64_t	sls_nrecs;
	uint64_t	sls_first_txg;
	uint64_t	sls_last_txg;
	uint64_t	sls_pending_bytes;	/* ALLOC, txg > applied */
	uint64_t	sls_pending_txgs;
	uint64_t	sls_held_bytes;	/* pr_held_asize - pr_rel_asize */
	uint64_t	sls_record_bytes;	/* pr_nentries * entry size */
	uint64_t	sls_flags;		/* OR of pr_flags */
} spa_lag_summary_t;

typedef struct spa_lag_ops {
	const char	*slo_name;
	spa_feature_t	slo_feature;
	const char	*slo_dir_name;		/* MOS directory entry */
	uint64_t	slo_entry_words;	/* size of one record entry */
	void (*slo_note_alloc)(void *, const blkptr_t *, uint64_t txg);
	void (*slo_note_unalloc)(void *, const blkptr_t *, uint64_t txg);
	void (*slo_write_ready)(void *, const blkptr_t *, uint64_t txg);
	/*
	 * Returns a mask: bit d set means DVA d of the BP is held, so the
	 * caller must not free it.  The consumer decides per DVA.
	 */
	uint_t (*slo_hold_free)(void *, const blkptr_t *, uint64_t txg);
	/* Pass 1, before the no-op test: release, fix-ups. */
	void (*slo_sync_pass)(void *, dmu_tx_t *, boolean_t noop);
	/* Every pass, at its end: write what this pass captured. */
	void (*slo_flush)(void *, dmu_tx_t *);
	/*
	 * Release the entries of one applied record, within *budget entries.
	 * Returns B_TRUE when the record is done and may be destroyed.
	 */
	boolean_t (*slo_release_rec)(void *, spa_lag_rec_t *,
	    uint64_t *budget, dmu_tx_t *);
	void (*slo_txg_synced)(void *, uint64_t txg);
	/* Debug: the ranges a metaslab allocated in this txg and pass. */
	void (*slo_verify_alloc)(void *, vdev_t *, zfs_range_tree_t *,
	    uint64_t txg);
	uint64_t (*slo_held_dspace)(void *);
	/* The numbers the throttle and the limits are measured on. */
	void (*slo_usage)(void *, spa_lag_usage_t *);
	/*
	 * At the hard limit with parity_onstall=continue: drop the records
	 * and go to a resync state.  Returns B_TRUE when the drop is under way.
	 */
	boolean_t (*slo_over_limit)(void *);
	/* Ask for a user drop; ENOENT if there is nothing to drop. */
	int (*slo_drop)(void *);
	/* B_TRUE while a requested drop is not complete. */
	boolean_t (*slo_drop_pending)(void *);
	/* Release without waiting for the next write (back-pressure). */
	void (*slo_force)(void *);
	/* The outside applied every record up to txg (the larger one wins). */
	void (*slo_applied)(void *, uint64_t txg, boolean_t force);
	/* Add the consumer's own keys to an event payload. */
	void (*slo_event_payload)(void *, nvlist_t *);
	const char *slo_event_throttle;		/* zevent: level changed */
	const char *slo_event_unprotected;	/* zevent: not protected */
} spa_lag_ops_t;

spa_lag_t *spa_lag_register(spa_t *, const spa_lag_ops_t *, void *);
void	spa_lag_unregister(spa_lag_t *);

/* The record store, for the consumer (spa_lag.c). */
int	spa_lag_load(spa_lag_t *);
int	spa_lag_adopt(spa_lag_t *);
int	spa_lag_dir_read(objset_t *, const char *, uint64_t *);
boolean_t spa_lag_dir_exists(spa_lag_t *);
void	spa_lag_dir_create(spa_lag_t *, dmu_tx_t *);
uint64_t spa_lag_dir_get(spa_lag_t *, int word);
void	spa_lag_dir_set(spa_lag_t *, int word, uint64_t value);
void	spa_lag_dir_sync(spa_lag_t *, dmu_tx_t *);
void	spa_lag_retire(spa_lag_t *, dmu_tx_t *);
uint64_t spa_lag_nrecs(spa_lag_t *);
uint64_t spa_lag_first_txg(spa_lag_t *, uint64_t released);
spa_lag_rec_t *spa_lag_rec_find(spa_lag_t *, uint64_t txg);
spa_lag_rec_t *spa_lag_rec_first(spa_lag_t *);
spa_lag_rec_t *spa_lag_rec_last(spa_lag_t *);
spa_lag_rec_t *spa_lag_rec_next(spa_lag_t *, spa_lag_rec_t *);
spa_lag_rec_t *spa_lag_rec_get(spa_lag_t *, dmu_tx_t *);
uint64_t spa_lag_rec_append(spa_lag_t *, spa_lag_rec_t *, const uint64_t *,
    uint64_t nentries, dmu_tx_t *);
void	spa_lag_rec_write_word(spa_lag_t *, spa_lag_rec_t *, uint64_t entry,
    uint64_t word, uint64_t value, dmu_tx_t *);
int	spa_lag_rec_read(spa_lag_t *, spa_lag_rec_t *, uint64_t first,
    uint64_t nentries, uint64_t *buf);
void	spa_lag_rec_sync(spa_lag_t *, spa_lag_rec_t *, dmu_tx_t *);
void	spa_lag_release_sync(spa_lag_t *, uint64_t applied_txg,
    uint64_t budget, dmu_tx_t *);
void	spa_lag_summarize(spa_lag_t *, uint64_t applied_txg,
    spa_lag_summary_t *);

/*
 * Keep the release (and so the free of a record object) away while the caller
 * reads records.  May be held across blocking I/O; the release only tries the
 * lock and skips that txg.  Pause: the same for a long walk (MAP, BLOCKS).
 */
void	spa_lag_recs_hold(spa_lag_t *);
void	spa_lag_recs_rele(spa_lag_t *);
void	spa_lag_pause_release(spa_lag_t *);
void	spa_lag_resume_release(spa_lag_t *);

/*
 * The outside has applied every record up to txg.  Returns 0, EINVAL (txg is
 * newer than the last synced txg) or EROFS.  A lower txg than before changes
 * nothing.  With SPA_LAG_FORCE the call waits for a txg to sync, so the
 * release has run when it returns.
 */
int	spa_lag_applied(spa_lag_t *, uint64_t txg, int flags);

/*
 * The outside makes progress (spa_lag_applied() calls it too).  Pacing
 * (parity_pace) follows the writers to the outside only while it does.
 */
void	spa_lag_progress(spa_lag_t *);
uint64_t spa_lag_throttled_ms(spa_lag_t *);
uint64_t spa_lag_applied_age_ms(spa_lag_t *);

/*
 * Drop all records and release all held frees; the outside must resync.
 * Returns 0 when done (also when there was nothing to drop), EINTR on a
 * signal (the drop goes on), ESHUTDOWN if the pool suspends.
 */
int	spa_lag_drop(spa_t *);

/* Free a held block: every DVA of the BP.  Syncing context. */
void	spa_lag_free_held(spa_t *, const blkptr_t *, dmu_tx_t *);

/*
 * The write throttle (dmu_tx.c) and the pacing of syncing-context frees.  Core
 * callers use the inline wrappers without "_impl" (sys/spa_impl.h).
 */
hrtime_t spa_lag_delay_impl(spa_t *);
int	spa_lag_wait_impl(spa_t *);
boolean_t spa_lag_free_pause_impl(spa_t *);
uint64_t spa_lag_max_records(spa_t *);
uint64_t spa_lag_max_held(spa_t *);
spa_lag_level_t spa_lag_level(spa_lag_t *);

/* Zevents. */
void	spa_lag_post_unprotected(spa_lag_t *, const char *reason);
void	spa_lag_wake(spa_lag_t *);

/* Take and drop the lock of the record tree (never across I/O). */
void	spa_lag_lock(spa_lag_t *);
void	spa_lag_unlock(spa_lag_t *);

/*
 * Core hooks.  Core callers use the inline wrappers without "_impl"
 * (sys/spa_impl.h): they test spa_lag[0] first, so a pool with no consumer
 * pays one load and one branch and no call.
 */
void	spa_lag_note_alloc_impl(spa_t *, const blkptr_t *, uint64_t txg);
void	spa_lag_note_unalloc_impl(spa_t *, const blkptr_t *, uint64_t txg);
uint_t spa_lag_hold_free_impl(spa_t *, const blkptr_t *, uint64_t txg);
void	spa_lag_write_ready_impl(spa_t *, const blkptr_t *, uint64_t txg);
void	spa_lag_sync_pass_impl(spa_t *, dmu_tx_t *, boolean_t noop);
void	spa_lag_flush_impl(spa_t *, dmu_tx_t *);
void	spa_lag_txg_synced_impl(spa_t *, uint64_t txg);
void	spa_lag_verify_alloc_impl(spa_t *, vdev_t *, zfs_range_tree_t *,
    uint64_t txg);
uint64_t spa_lag_held_dspace_impl(spa_t *);

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_SPA_LAG_H */
