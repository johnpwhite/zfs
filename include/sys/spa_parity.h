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

#ifndef _SYS_SPA_PARITY_H
#define	_SYS_SPA_PARITY_H

#include <sys/spa.h>
#include <sys/spa_lag.h>

#ifdef	__cplusplus
extern "C" {
#endif

/* Parity ioctl interfaces. */

/* Levels of the write throttle. */
#define	SPA_PARITY_THROTTLE_NONE	0
#define	SPA_PARITY_THROTTLE_SOFT	1
#define	SPA_PARITY_THROTTLE_HARD	2

/* Bits of spa_parity_info_t.pi_flags. */
/* The stall policy dropped the records: the set must resync the pool. */
#define	SPA_PARITY_F_UNPROTECTED	(1ULL << 0)
/* A record header says: some recorded checksums may be out of date. */
#define	SPA_PARITY_F_FIXUPS_INCOMPLETE	(1ULL << 1)
/* A drop is requested or under way. */
#define	SPA_PARITY_F_DROP_PENDING	(1ULL << 2)

/* A snapshot of the state of the pool's parity set. */
typedef struct spa_parity_info {
	uint64_t	pi_state;	/* parity_state_t */
	uint64_t	pi_start_txg;
	uint64_t	pi_stop_txg;
	uint64_t	pi_generation;
	uint64_t	pi_set_guid;
	uint64_t	pi_applied_txg;
	uint64_t	pi_first_txg;	/* released watermark + 1 */
	uint64_t	pi_last_txg;	/* newest txg with unreleased records */
	uint64_t	pi_nrecs;
	uint64_t	pi_pending_bytes;
	uint64_t	pi_held_bytes;
	uint64_t	pi_held_dspace;
	uint64_t	pi_leaked_bytes;	/* held, not freed by a drop */
	uint64_t	pi_record_bytes;
	uint64_t	pi_throttle;	/* SPA_PARITY_THROTTLE_* */
	uint64_t	pi_flags;	/* SPA_PARITY_F_* */
	/* Core counters and limits. */
	uint64_t	pi_pending_txgs;
	uint64_t	pi_max_record_bytes;	/* the limit in force */
	uint64_t	pi_max_held_bytes;	/* the limit in force */
} spa_parity_info_t;

/* From the core (spa_parity.c; spa_lag.h has the spa_lag.c ones). */
extern void spa_parity_init(void);
extern void spa_parity_fini(void);
extern spa_lag_t *spa_parity_lag(spa_t *);
extern void spa_parity_get_info(spa_t *, spa_parity_info_t *);
extern uint64_t spa_parity_leaked_bytes(spa_t *);

/*
 * The ioctl functions (spa_parity_ioctl.c).  Each takes an open spa and
 * returns an errno.
 */
extern int spa_parity_ioc_info(spa_t *, boolean_t have_txg, uint64_t txg,
    nvlist_t *out);
extern int spa_parity_ioc_records(spa_t *, uint64_t from_txg, uint64_t cursor,
    uint64_t max_entries, nvlist_t *out);
extern int spa_parity_ioc_read(spa_t *, const uint64_t *entry, uint_t nwords,
    const uint64_t *index, nvlist_t *out);
extern int spa_parity_ioc_readv(spa_t *, const uint64_t *entries,
    uint_t nwords, const uint64_t *indexes, uint_t nindexes, nvlist_t *out);
extern int spa_parity_ioc_verify(spa_t *, const uint64_t *entry, uint_t nwords,
    const uint64_t *index, const uint8_t *data, uint_t len, boolean_t *match,
    boolean_t *verified);
extern int spa_parity_ioc_map(spa_t *, uint64_t vdev, uint64_t txg,
    uint64_t from_offset, uint64_t max_ranges, nvlist_t *out);
extern int spa_parity_ioc_blocks(spa_t *, uint64_t vdev, uint64_t txg,
    uint64_t from_offset, uint64_t max_entries, nvlist_t *out);
extern int spa_parity_ioc_blocks_windows(spa_t *, uint64_t vdev, uint64_t txg,
    const uint64_t *windows, uint_t nwin,
    const uint64_t resume[PARITY_BLOCKS_RESUME_WORDS],
    uint64_t max_entries, nvlist_t *out);
extern int spa_parity_ioc_current(spa_t *, uint64_t vdev, uint64_t offset,
    nvlist_t *out);
extern int spa_parity_ioc_label(spa_t *, uint64_t vdev, uint64_t txg,
    nvlist_t *out);
/* the vdev input of the calls below for the first data column */
#define	SPA_PARITY_VDEV_FIRST	UINT64_MAX
/* the optional ZPARITY_VDEV input, or the first data column */
extern uint64_t spa_parity_ioc_vdev(nvlist_t *innvl);
extern int spa_parity_ioc_applied(spa_t *, uint64_t set_guid,
	uint64_t generation, uint64_t txg);
extern int spa_parity_ioc_drop(spa_t *, uint64_t set_guid,
	uint64_t generation);
extern int spa_parity_ioc_reclaim(spa_t *, uint64_t txg,
	uint64_t pool_guid, uint64_t flags, const uint64_t *ranges,
	uint_t nwords, nvlist_t *out);
extern void spa_parity_reclaim_clear(spa_t *);
extern int spa_parity_ioc_repair(spa_t *, uint64_t request,
    const uint8_t *data, uint_t len, boolean_t *repaired);
extern void spa_parity_repair_global_init(void);
extern void spa_parity_repair_global_fini(void);
extern int spa_parity_repair_provider_register(void *, uint64_t pool_guid);
extern void spa_parity_repair_provider_unregister(void *);
extern void spa_parity_repair_repost(spa_t *);
extern boolean_t spa_parity_repair_provider_available(spa_t *);
extern boolean_t spa_parity_repair_provider_authorized(void *,
    uint64_t pool_guid);
extern int spa_parity_repair_supply(spa_t *, uint64_t request,
    const uint8_t *data, uint_t len, boolean_t decline, void *provider);

/*
 * Post the event parity_records (at most once per zfs_parity_event_ms for each
 * pool).  Returns B_FALSE when the rate limit held it back; the core then asks
 * again after a later txg.
 */
extern boolean_t spa_parity_event_records(spa_t *, uint64_t txg);

/* Lineage and repair interfaces. */

struct zio;

/*
 * Data device, epoch and lineage.
 *
 * The data device of a member is its one normal-class top-level vdev, not
 * always child 0 of the root: a log device may come first, and a removed log
 * leaves a hole.  While a replace runs, the top-level vdev is a replacing (or
 * spare) vdev and its data leaf is its first child.
 */

/* Callers of the first three hold a config lock (SCL_CONFIG or SCL_STATE). */
extern vdev_t *spa_parity_data_tvd(spa_t *);
extern vdev_t *spa_parity_data_leaf(spa_t *);
extern vdev_t *spa_parity_tvd_leaf(vdev_t *);
/* Needs no lock; a log device, a hole or a missing id is not a data vdev. */
extern boolean_t spa_parity_is_data_vdev(spa_t *, uint64_t vdev_id);

/*
 * What a daemon needs to see that the history of a pool changed (a rewound
 * import, a lost write, a new epoch).
 *
 * The epoch id is random, not zero.  It is set when records start, and
 * changes at each new epoch, at each DROP, and at each import that loads an
 * uberblock older than the newest one on disk.  A daemon that knows an
 * older epoch must resync the pool.  The generation grows with every change
 * of epoch and is only a counter.
 *
 * The lineage is the root block checksum of the uberblock of one txg, as this
 * pool has it.  The daemon keeps the root checksum of its applied txg and asks
 * for that txg: a known txg with another checksum is another history.  The
 * pool keeps the newest PARITY_UB_RING uberblocks of the txgs it synced since
 * the import; an older txg is "not known" and only the epoch decides.
 */
typedef struct spa_parity_lineage {
	uint64_t	pl_epoch;
	uint64_t	pl_generation;
	uint64_t	pl_start_txg;
	uint64_t	pl_last_synced_txg;	/* newest uberblock known */
	uint64_t	pl_last_root_cksum[4];
	uint64_t	pl_load_ub_txg;		/* uberblock of the import */
	uint64_t	pl_load_latest_txg;	/* newest on disk then */
	boolean_t	pl_rewound;		/* an older one was used */
	boolean_t	pl_unprotected;		/* import override */
	uint64_t	pl_data_vdev_id;	/* top-level vdev of the data */
	uint64_t	pl_data_leaf_guid;
	/* The uberblock of the txg that was asked for. */
	boolean_t	pl_ub_known;
	uint64_t	pl_ub_txg;
	uint64_t	pl_ub_timestamp;
	uint64_t	pl_ub_root_cksum[4];
} spa_parity_lineage_t;

/* ENOENT: the pool is not a member in this import (or read-only). */
extern int spa_parity_lineage(spa_t *, uint64_t txg, spa_parity_lineage_t *);

/*
 * Parity repair.
 *
 * A read of an applied member block that fails with ECKSUM or EIO waits for
 * the registered provider when one is available.  The daemon rebuilds bytes
 * from parity and supplies them with spa_parity_repair_supply().  ZFS checks
 * them against the block pointer checksum before it uses them, then writes
 * the bad bytes in place.  Without a provider, in a syncing context, or for a
 * block newer than the applied txg, the read fails at once and the event path
 * continues.
 */

/*
 * Called from zio_done() for a logical read that failed; it ignores
 * everything that is not a real failure of a member's data block.
 */
extern uint64_t spa_parity_repair_note(spa_t *, struct zio *);
extern boolean_t spa_parity_repair_wait(spa_t *, struct zio *, uint64_t);

/*
 * The REPAIR ioctl: request = the id of the zevent, buf = the rebuilt bytes
 * of that DVA (len must be the asize of the DVA).  *repaired is B_TRUE only
 * when the bytes matched the checksum and were written.  It returns ENOENT
 * for an unknown, expired or invalidated request.  It returns EBUSY while
 * another call works on the request, EINVAL for a wrong length, EROFS on a
 * read-only pool, or an I/O error when the write fails.  A mismatch is not an
 * error: repaired is B_FALSE, nothing is written and the request stays.
 */
extern int spa_parity_repair(spa_t *, uint64_t request, const void *buf,
    size_t len, boolean_t *repaired);

/* Requests that wait for an answer (for INFO). */
extern uint64_t spa_parity_repair_npending(spa_t *);

/*
 * Check bytes against the checksum of a block pointer, as the repair does:
 * buf holds the on-disk bytes of one DVA of bp (len >= the psize that the
 * checksum covers; for a gang block pointer the DVA is the gang header).  The
 * bytes are not changed.  Returns 0 (match), ECKSUM (no match), EINVAL (len
 * too small, or a block with no usable checksum).
 */
extern int spa_parity_bp_verify(spa_t *, const blkptr_t *, const void *buf,
    size_t len);

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_SPA_PARITY_H */
