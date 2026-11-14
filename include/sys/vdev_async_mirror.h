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

#ifndef _SYS_VDEV_ASYNC_MIRROR_H
#define	_SYS_VDEV_ASYNC_MIRROR_H

#include <sys/spa.h>
#include <sys/vdev.h>
#include <sys/dmu_tx.h>
#include <sys/range_tree.h>
#include <sys/space_map.h>
#include <sys/metaslab.h>
#include <sys/zio.h>

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * The async mirror keeps one top-level mirror's second child behind
 * the first child.  See module/zfs/vdev_async_mirror.c.
 */
struct vdev_async_mirror;

/*
 * One record of the record ZAP (DMU_POOL_ASYNC_MIRROR_ZAP), with its txg as
 * the uint64 key.  The space map holds the blocks to copy (SM_ALLOC) and the
 * held ranges (SM_FREE) of the txg.  The uberblock is that of the previous
 * record: the copy writes it to the lagging child once that txg is copied.
 * It is kept in a fixed number of words, so that a change of uberblock_t
 * cannot change this format unnoticed.
 */
#define	ASYNC_MIRROR_REC_VERSION	1
#define	ASYNC_MIRROR_REC_UB_WORDS	32

typedef struct async_mirror_rec_phys {
	uint64_t	amr_version;	/* ASYNC_MIRROR_REC_VERSION */
	uint64_t	amr_sm_obj;	/* space map of the txg */
	uint64_t	amr_leaf_guid;	/* the lagging leaf */
	uint64_t	amr_ub[ASYNC_MIRROR_REC_UB_WORDS];
} async_mirror_rec_phys_t;

#define	ASYNC_MIRROR_REC_WORDS	\
	(sizeof (async_mirror_rec_phys_t) / sizeof (uint64_t))

/*
 * The lineage entry of the MOS directory (DMU_POOL_ASYNC_MIRROR_LINEAGE).
 * It exists while the pool has a lineage; see "The lineage" in
 * vdev_async_mirror_lineage.c.
 */
#define	ASYNC_MIRROR_LINEAGE_VERSION	1

/* aml_flags: a rewind import made the open fork. */
#define	ASYNC_MIRROR_FORK_REWIND	(1ULL << 0)
/* aml_flags: the entry holds a reference on the feature. */
#define	ASYNC_MIRROR_LINEAGE_ACTIVE	(1ULL << 1)

typedef struct async_mirror_lineage_phys {
	uint64_t	aml_version;	/* ASYNC_MIRROR_LINEAGE_VERSION */
	uint64_t	aml_lineage;	/* this line of history, not 0 */
	uint64_t	aml_parent;	/* lineage of the open fork, or 0 */
	uint64_t	aml_fork_txg;	/* last txg shared with aml_parent */
	uint64_t	aml_absent;	/* the child missing at the fork */
	uint64_t	aml_seen;	/* rewind fork: newest txg dropped */
	uint64_t	aml_flags;	/* ASYNC_MIRROR_FORK_REWIND, ... */
} async_mirror_lineage_phys_t;

#define	ASYNC_MIRROR_LINEAGE_WORDS	\
	(sizeof (async_mirror_lineage_phys_t) / sizeof (uint64_t))

/* What one import of the pool decides about the async mirror. */
typedef struct async_mirror_import {
	uint64_t	ami_keep;	/* the copy that the operator keeps */
	uint64_t	ami_erased;	/* the child that this import erased */
	uint64_t	ami_resync[2];	/* children out of the async mirror */
} async_mirror_import_t;

extern uint64_t spa_async_mirror_lineage(spa_t *spa);

/* Called for each record, in ZAP order; a nonzero return stops the walk. */
typedef int async_mirror_rec_cb_t(uint64_t txg,
    const async_mirror_rec_phys_t *rec, void *arg);
extern int async_mirror_rec_iterate(objset_t *mos, uint64_t zap,
    async_mirror_rec_cb_t *cb, void *arg);

extern int vdev_async_mirror_activate(spa_t *spa);
extern void vdev_async_mirror_stop(spa_t *spa);
extern void vdev_async_mirror_fini(spa_t *spa);
extern void vdev_async_mirror_spawn(spa_t *spa);
extern void vdev_async_mirror_zthr_cancel(spa_t *spa);
extern void vdev_async_mirror_zthr_resume(spa_t *spa);

extern int vdev_async_mirror_set_check(spa_t *spa, vdev_t *vd,
    uint64_t value);
extern int vdev_async_mirror_set(spa_t *spa, uint64_t guid, uint64_t value);
extern const char *vdev_async_mirror_state(vdev_t *vd);

extern int vdev_async_mirror_check_lineage(spa_t *spa);
extern void vdev_async_mirror_load_marked(spa_t *spa);
extern boolean_t vdev_async_mirror_stale(vdev_t *vd, uint64_t txg);
extern boolean_t vdev_async_mirror_read_fenced(vdev_t *vd,
    const zio_t *zio);
extern boolean_t vdev_async_mirror_no_repair(vdev_t *vd,
    const zio_t *zio);

extern boolean_t vdev_async_mirror_is_async(vdev_t *vd);
extern boolean_t vdev_async_mirror_skip_labels(const vdev_t *vd);
extern boolean_t vdev_async_mirror_skip_uberblock(const vdev_t *vd);
extern boolean_t vdev_async_mirror_intercept_write(zio_t *zio, vdev_t *vd);
extern boolean_t vdev_async_mirror_active(spa_t *spa);
extern boolean_t vdev_async_mirror_is_child(const vdev_t *vd);
extern boolean_t vdev_async_mirror_tracks(const vdev_t *vd);
extern boolean_t vdev_async_mirror_in_use(spa_t *spa);
extern boolean_t vdev_async_mirror_removal_refused(const vdev_t *vd);
/* True if a free from this top-level vdev must be held. */
extern boolean_t vdev_async_mirror_should_hold(const vdev_t *vd,
    metaslab_free_kind_t kind);

extern void vdev_async_mirror_claim_committed(spa_t *spa, const blkptr_t *bp,
    uint64_t txg);
extern void vdev_async_mirror_claimed(spa_t *spa);

extern void vdev_async_mirror_sync_start(spa_t *spa, dmu_tx_t *tx);
extern void vdev_async_mirror_sync_record(spa_t *spa, dmu_tx_t *tx);
extern void vdev_async_mirror_sync_lineage(spa_t *spa, dmu_tx_t *tx);
extern void vdev_async_mirror_sync_write(spa_t *spa, vdev_t *vd,
    zfs_range_tree_t *rt, maptype_t type, dmu_tx_t *tx);
extern void vdev_async_mirror_sync_list(spa_t *spa, vdev_t *vd,
    zfs_range_tree_t *alloctree, zfs_range_tree_t *nolist, dmu_tx_t *tx);
extern void vdev_async_mirror_txg_synced(spa_t *spa, uint64_t txg);
extern hrtime_t vdev_async_mirror_delay(spa_t *spa);
extern void vdev_async_mirror_wait(spa_t *spa);

/* In vdev_label.c: erase every uberblock of a leaf, in two phases. */
extern int vdev_uberblock_erase(vdev_t *vd, uint64_t max_txg);

extern uint64_t spa_get_async_mirror_held_space(spa_t *spa);

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_VDEV_ASYNC_MIRROR_H */
