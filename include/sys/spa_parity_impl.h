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
 * Parity set: the state of the BP-specific lagging consumer.  A parity set
 * keeps shared parity over several single-disk pools.  Each member pool
 * records, per txg, the block pointers it allocated and freed, and holds its
 * frees until the parity has caught up (see spa_parity.c).
 */

#ifndef _SYS_SPA_PARITY_IMPL_H
#define	_SYS_SPA_PARITY_IMPL_H

#include <sys/spa.h>
#include <sys/spa_lag.h>
#include <sys/spa_parity.h>
#include <sys/uberblock_impl.h>
#include <sys/avl.h>
#include <sys/list.h>
#include <sys/range_tree.h>
#include <sys/txg.h>

#ifdef	__cplusplus
extern "C" {
#endif

/* Values of the SLD_STATE word of the directory entry. */
typedef enum parity_state {
	PARITY_OFF = 0,
	PARITY_ON,
	PARITY_DRAINING,
	PARITY_DROPPING
} parity_state_t;

#define	PARITY_DIR_NAME	"org.openzfs:parity_records"
/*
 * Held space that a drop could not free, because the record that names it was
 * unreadable.  A uint64 in the MOS directory; it stays after
 * the records are gone, so the lost capacity stays visible.  Stock OpenZFS
 * ignores the entry.
 */
#define	PARITY_LEAKED_NAME	"org.openzfs:parity_leaked_bytes"
#define	PARITY_RECLAIM_BATCH_MAX	4096
#define	PARITY_RECLAIM_RANGES_MAX	65536

/*
 * The SLD_STATE word holds the state in its low byte.  This flag keeps
 * ps_unprotected (a drop forced by a limit) across an export and import.
 */
#define	PARITY_DIR_STATE_MASK	0xffULL
#define	PARITY_DIR_UNPROTECTED	(1ULL << 8)

/*
 * A record entry is 18 uint64s: w0 = txg (for a FIXUP, the txg it fixes),
 * w1 = kind, w2-w17 = the blkptr_t.
 *
 * A FREE entry holds a BP with only the DVAs that were held, packed to the
 * front: a free may release some DVAs of a dedup BP at once (the ones born
 * in the same txg are cancelled, not held).
 */
#define	PARITY_ENTRY_WORDS	18
#define	PARITY_ENTRY_BYTES	(PARITY_ENTRY_WORDS * sizeof (uint64_t))
#define	PARITY_EW_TXG		0
#define	PARITY_EW_KIND		1
#define	PARITY_EW_BP		2

/* Entries per write or read of a record: about 1 MiB. */
#define	PARITY_FLUSH_CHUNK	((1 << 20) / (PARITY_ENTRY_WORDS * 8))

#define	PARITY_KIND_VOID	0
#define	PARITY_KIND_ALLOC	1
#define	PARITY_KIND_FREE	2
#define	PARITY_KIND_FIXUP	3

_Static_assert(PARITY_EW_BP * sizeof (uint64_t) + sizeof (blkptr_t) ==
    PARITY_ENTRY_WORDS * sizeof (uint64_t),
	    "parity: a record entry is the txg, the kind and one blkptr_t");

/*
 * One pending entry of a txg.  An ALLOC entry is keyed by DVA[0] (vdev,
 * offset) in pt_tree; until it is flushed it is also on pt_unflushed.  When
 * an entry that was flushed is cancelled, it leaves the tree and goes to
 * pt_voids, so that the next flush marks its slot VOID.  A FREE entry is only
 * on pt_unflushed, and after its flush in pt_frees (by slot) until the txg
 * ends; it is never cancelled.
 */
typedef struct parity_pend {
	avl_node_t	pp_node;
	list_node_t	pp_lnode;
	avl_node_t	pp_snode;	/* flushed FREE entries, by slot */
	uint64_t	pp_vdev;
	uint64_t	pp_offset;
	blkptr_t	pp_bp;
	uint64_t	pp_asize;	/* ALLOC: its bytes; FREE: bytes held */
	uint64_t	pp_slot;	/* UINT64_MAX: not flushed */
	uint8_t		pp_kind;
	uint8_t		pp_late;	/* BP changed after the flush */
} parity_pend_t;

/* A range allocated and freed in the same txg (debug verifier only). */
typedef struct parity_cancel {
	list_node_t	pc_node;
	uint64_t	pc_vdev;
	uint64_t	pc_offset;
	uint64_t	pc_size;
} parity_cancel_t;

/*
 * Per-txg capture table.  pt_lock is a leaf lock.  It is one mutex for each
 * txg and each pool, and every recorded allocation and free takes it.  Do not
 * change the design before a measurement shows that
 * the lock contends.
 */
typedef struct parity_txg {
	kmutex_t	pt_lock;
	avl_tree_t	pt_tree;
	avl_tree_t	pt_frees;
	list_t		pt_unflushed;
	list_t		pt_voids;
	list_t		pt_cancel;
	uint64_t	pt_nunflushed;
	uint64_t	pt_nvoids;
	/* zfs_parity_verify, taken once for the whole txg. */
	uint_t		pt_verify;
} parity_txg_t;

/* Ranges per vdev, for the verifier. */
typedef struct parity_vrt {
	avl_node_t	pv_node;
	uint64_t	pv_vdev;
	zfs_range_tree_t *pv_rt;
} parity_vrt_t;

/* The uberblock of a synced txg, as far as the lineage check needs it. */
#define	PARITY_UB_RING	128

typedef struct parity_ub {
	uint64_t	pu_txg;			/* 0: slot not used */
	uint64_t	pu_timestamp;
	uint64_t	pu_cksum[4];		/* checksum of the root bp */
} parity_ub_t;

typedef struct spa_parity {
	spa_t		*ps_spa;
	spa_lag_t	*ps_lag;
	kmutex_t	ps_lock;
	/*
	 * RECORDS, READ and MAP hold it as reader while they use a record; the
	 * release only tries it as writer and skips that txg.
	 */
	krwlock_t	ps_rec_rwlock;
	/*
	 * Written only in syncing context (or at load); read without a lock.
	 * A writer sets the other fields, then membar_producer(), then
	 * ps_state; a reader reads ps_state, then membar_consumer().
	 */
	parity_state_t	ps_state;
	uint64_t	ps_capture_from_txg;
	uint64_t	ps_start_txg;
	uint64_t	ps_stop_txg;
	uint64_t	ps_generation;
	/* Protected by ps_lock; dirty until the directory update is synced. */
	uint64_t	ps_set_guid;
	boolean_t	ps_set_guid_dirty;
	/*
	 * Under ps_lock (the throttle reads the counters without it: a stale
	 * value only moves the delay by one txg).
	 */
	uint64_t	ps_applied_txg;
	uint64_t	ps_record_bytes;	/* record objects */
	uint64_t	ps_held_bytes;
	uint64_t	ps_held_dspace;
	boolean_t	ps_release_forced;
	/* The property parityset, as the last prop sync set it. */
	boolean_t	ps_prop_on;
	uint_t		ps_drop_request;	/* PARITY_DROP_* */
	boolean_t	ps_unprotected;	/* a limit drop */
	boolean_t	ps_fixups_incomplete;	/* for the next record */
	/*
	 * The parity_records event.  ps_event_pending is the newest txg whose
	 * event is not posted yet (0: none); ps_event_last is the time of the
	 * last post.  Both are used by the thread that ends spa_sync() only,
	 * and by the sync thread before it.
	 */
	uint64_t	ps_event_pending;
	hrtime_t	ps_event_last;
	/* Capture tables. */
	parity_txg_t	ps_txg[TXG_SIZE];
	/*
	 * Fix-ups (changed in syncing context or at load, under ps_lock; the
	 * ioctls read them under ps_lock).  ps_late: the
	 * final BPs of the entries of record ps_late_txg that changed after
	 * their flush; they become the first entries (FIXUP) of the next
	 * record.  ps_fixups: the ones written into record ps_fixups_txg, kept
	 * until that txg is verified.
	 */
	list_t		ps_late;
	uint64_t	ps_nlate;
	uint64_t	ps_late_txg;
	list_t		ps_fixups;
	uint64_t	ps_nfixups;
	uint64_t	ps_fixups_txg;
	/*
	 * Verifier: ranges seen by metaslab_sync() in the syncing txg, and
	 * (bit 3) the ranges that are held and not released.
	 */
	kmutex_t	ps_verify_lock;
	avl_tree_t	ps_verify;
	avl_tree_t	ps_vheld;
	uint64_t	ps_vbad;
	/* Statistics (syncing context). */
	uint64_t	ps_stat_txgs;		/* txgs that wrote a record */
	uint64_t	ps_stat_passes;		/* their sync passes, summed */
	uint64_t	ps_stat_max_passes;
	uint64_t	ps_stat_entries;
	uint64_t	ps_stat_voids;
	uint64_t	ps_stat_late;
	uint64_t	ps_stat_fixups;		/* FIXUP entries written */
	uint64_t	ps_stat_lost;		/* not found on disk */
	uint64_t	ps_stat_released;	/* records released */
	uint64_t	ps_stat_held;		/* FREE entries that hold */
	uint64_t	ps_stat_rel_entries;	/* entries the release read */
	uint64_t	ps_stat_rel_errors;
	uint64_t	ps_stat_drops;
	/* Repair on read (atomic: any context). */
	uint64_t	ps_stat_repair_requests;	/* blocks that asked */
	uint64_t	ps_stat_repair_supplied;	/* bytes for a read */
	uint64_t	ps_stat_repair_declined;	/* no bytes */
	uint64_t	ps_stat_repair_rejected;	/* bad checksum */
	uint64_t	ps_stat_repair_written;	/* REPAIR wrote */
	uint64_t	ps_stat_read_bytes;	/* READ and READV disk reads */
	/*
	 * Epoch and lineage state.  ps_epoch is written in syncing context or
	 * at load, with ps_generation; the ring is under ps_lock.
	 */
	uint64_t	ps_epoch;
	uint64_t	ps_load_ub_txg;
	uint64_t	ps_load_latest_txg;
	boolean_t	ps_rewound;
	boolean_t	ps_readonly;		/* read-only import: no write */
	boolean_t	ps_unreadable;		/* import override */
	uint_t		ps_ub_next;
	parity_ub_t	ps_ub[PARITY_UB_RING];
	/* Repair requests, created with this structure. */
	struct parity_repair *ps_repair;
} spa_parity_t;

/* Gating: a member pool stays one disk while it holds records. */
boolean_t spa_parity_busy(spa_t *);
boolean_t spa_parity_member_on_disk(spa_t *);
void	spa_parity_txg_done(spa_t *);
boolean_t spa_parity_expand_blocked(spa_t *);
uint64_t spa_parity_grow_cap(spa_t *);
uint64_t spa_parity_add_cap(spa_t *);
int	spa_parity_eligible(spa_t *);
int	spa_parity_eligible_root(vdev_t *);
boolean_t spa_parity_column_ok(vdev_t *);
/* the most data columns (top-level vdevs) of one member pool */
#define	SPA_PARITY_MAX_COLS	64
/*
 * a mirror data column has at most this many sides.  Each side is a disk,
 * or two while it is replaced or a hot spare stands in, so a column has at
 * most 2 x SPA_PARITY_MAX_SIDES leaves; INFO lists all of them.
 */
#define	SPA_PARITY_MAX_SIDES	8
uint64_t spa_parity_ncols(spa_t *);
boolean_t spa_parity_checksum_refused(uint64_t);
int	spa_parity_check_checksums(spa_t *);

/* Pool property and load. */
void	spa_parity_prop_sync(spa_t *, uint64_t on, dmu_tx_t *);
int	spa_ld_parity_load(spa_t *);
void	spa_parity_unload(spa_t *);

/* For zdb: the MOS objects of the records, read from disk. */
int	spa_parity_mos_objs(spa_t *, void (*)(uint64_t));

/*
 * Epoch and lineage (spa_parity_lineage.c).  The renew call is for every
 * place that starts a new history: a new epoch, a DROP (syncing context or
 * load only).
 */
uint64_t spa_parity_new_epoch(void);
void	spa_parity_epoch_renew(spa_parity_t *);
void	spa_parity_lineage_note(spa_parity_t *, const uberblock_t *);

/* Repair requests (spa_parity_repair.c). */
void	spa_parity_repair_init(spa_parity_t *);
void	spa_parity_repair_fini(spa_parity_t *);
void	spa_parity_repair_invalidate(spa_parity_t *, const blkptr_t *);
void	spa_parity_repair_barrier(spa_t *);

extern uint_t zfs_parity_import_override;
extern uint_t zfs_parity_verify;
extern int zfs_parity_allow_expand;
extern uint64_t zfs_parity_release_limit;
extern uint64_t zfs_parity_test_auto_apply;
extern uint_t zfs_parity_test_inject;
extern uint_t zfs_parity_max_record_pct;
extern int zfs_parity_suspend_release;

/* Reasons for a drop (ps_drop_request, and the event payload). */
#define	PARITY_DROP_NONE	0
#define	PARITY_DROP_USER	1
#define	PARITY_DROP_LIMIT	2

#define	PARITY_VERIFY_ALLOC	(1U << 0)	/* alloc ranges == captured */
#define	PARITY_VERIFY_RECORD	(1U << 1)	/* on disk == in memory */
#define	PARITY_VERIFY_FINAL	(1U << 2)	/* recorded BP == tree BP */
#define	PARITY_VERIFY_RELEASE	(1U << 3)	/* each held range freed once */

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_SPA_PARITY_IMPL_H */
