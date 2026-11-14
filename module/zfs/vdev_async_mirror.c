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
 * Async mirror overview.
 *
 * An async mirror is a two-way top-level mirror with one leader and one
 * lagging child.  Foreground writes complete on the leader.  A worker copies
 * each transaction group (txg) to the lagging child in order.  The child
 * stays a complete older copy while the worker catches up.
 *
 * A free on the mirror can refer to a block that the worker still needs to
 * copy.  The free stays held until the lagging child has passed its txg.
 * Each txg has a record in the MOS.  The record tracks the ranges to hold
 * and the blocks to copy.  After the copy, the held ranges are released
 * through the normal free path.
 *
 * The vdev property async_mirror turns the async mirror on or off.  A
 * property change takes effect at a txg boundary.  An import reads the
 * property from the leaf's ZAP and can resume the async mirror.  The code
 * below also handles crashes, imports with a missing child, and back-pressure
 * when the copy falls behind.
 *
 * Txg flow:
 *
 *  txg N: write to leader -> record held frees -> copy to lagging child
 *         in txg order -> release held frees
 *
 * Corner cases:
 *
 * A space map object for each txg records held ranges as SM_FREE entries
 * and blocks to copy as SM_ALLOC entries.  One MOS ZAP maps txgs to these
 * objects.  The ZAP value, an async_mirror_rec_phys_t, holds the uberblock
 * of the previous record because an uberblock cannot be stored in its own
 * txg.  Replay copies txgs from the leader to the lagging child in order,
 * flushes, writes the newest uberblock, and flushes again; see
 * async_mirror_commit().
 *
 * A sync task releases a record's SM_FREE entries after the lagging child
 * has passed the record's txg.  It releases records oldest first, with a
 * limit per call, and removes each record.  The space then goes through the
 * normal defer.  The release is durable after the flush of the lagging
 * child.  A crash between the flush and the next txg repeats the release,
 * which is harmless.  The release joins the next txg that the pool syncs;
 * it does not force its own txg, so replay does not use up the uberblock
 * ring.  See async_mirror_release_thread().
 *
 * The property update is stored in a sync task.  The switch runs at the
 * start of spa_sync() in a later txg, before that txg issues writes, frees
 * blocks, or syncs metaslabs; see vdev_async_mirror_sync_start().  When the
 * async mirror starts, spa_sync() has waited for writes of the previous
 * txg, including writes from dmu_sync() and Direct I/O in open context, and
 * for its uberblock.  The children are equal at that txg.  The uberblock of
 * the property txg starts the record chain, as the imported uberblock does
 * at import.  Writes in open context can still reach the child after the
 * switch.  Their ranges are allocated in their txg, so the record includes
 * them and replay writes the same bytes again.  The range was free in the
 * child's txg, so it remains a valid copy of the previous txg.  Frees from
 * the switch txg onward are held, and that txg has the first record.  The
 * mark goes into the config of the switch txg.
 *
 * Turning the async mirror off has two steps because a write dropped before
 * the switch is not part of a later txg.  dmu_sync() and Direct I/O write
 * in open context, with the birth of an open txg, and the write is dropped
 * when it is issued.  First, at the start of txg S, writes reach the child
 * again ("passthrough").  Records and held frees continue.  A write dropped
 * before this point has a birth no later than S + 2, the newest txg that
 * can be open while S syncs (am_pass_txg).  The child still gets labels and
 * uberblocks only from replay.
 *
 * Second, at the start of a txg F after am_pass_txg, replay has copied every
 * record up to am_pass_txg, and no deferred block waits.  The child then
 * holds every txg before F, including each dropped write.  Writes in later
 * txgs reached the child from the pool after the first step.  Replay does
 * not copy records after am_pass_txg, and the switch does not wait for a
 * copy of the newest txg.  From F onward, the pool writes the child's
 * labels and uberblocks again, and clears the mark in the config of F.
 * This dirties the mirror config, so vdev_config_sync() flushes the child
 * after F's even labels and before F's uberblock.  The writes that reached
 * the child after the first step are then on disk before an uberblock points
 * to them.  No record is made for F or later.  Old records are released
 * (AMS_DRAINING), and the feature returns to enabled when they are gone.
 * An entry in the MOS directory notes F while records remain.  After a
 * crash, an import uses it to treat the child as a normal child without a
 * resilver.  A dropped intent log block is not copied: it holds only writes
 * of txgs before F, which the child has from the pool itself.
 *
 * A crash before F leaves the records and the property off.  Import releases
 * the records and resilvers the txgs that the child did not get.  An export
 * copies every record, so a clean export leaves the child equal to its
 * mirror.
 *
 * The copy backlog is the records that are ready and not copied,
 * am_pending_bytes and am_pending_txgs.  The write throttle slows dataset
 * transactions as the backlog grows, as it does for dirty data.  Past
 * zfs_delay_min_dirty_percent of zfs_vdev_async_mirror_max_pending_bytes
 * or _txgs, each transaction takes at least
 * zfs_delay_scale * (backlog - min) / (max - backlog).  Above either limit,
 * dmu_tx_wait() waits until the copy is below it; see
 * vdev_async_mirror_delay() and dmu_tx_delay().  Write latency rises with
 * the backlog, and the pool runs at the speed of the lagging child when
 * the backlog stays high.  The sync thread does not wait for the copy.  A
 * stalled lagging child holds back new writes, not pool txgs, and the copy
 * I/O stays visible to the deadman.  Transactions assigned before the
 * backlog reaches a limit can take it past that limit, by at most the
 * dirty data of the open and quiescing txgs.  Sync tasks are not throttled:
 * their changes are small, and some tasks, such as release and switch-off,
 * shrink the backlog.
 *
 * A frozen child, or one whose copy failed, is not equal to its mirror.
 * Turning the async mirror off then ends the freeze, or waits for the
 * failure to end, and the child is resilvered as at an import with the
 * async mirror off.
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_async_mirror.h>
#include <sys/vdev_async_mirror_impl.h>
#include <sys/uberblock_impl.h>
#include <sys/metaslab_impl.h>
#include <sys/zap.h>
#include <sys/zfeature.h>
#include <sys/dmu.h>
#include <sys/dmu_tx.h>
#include <sys/zio.h>
#include <sys/abd.h>
#include <sys/txg.h>
#include <sys/zthr.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_synctask.h>
#include <sys/dsl_scan.h>
#include <sys/dsl_dataset.h>
#include <sys/dmu_traverse.h>
#include <sys/dmu_objset.h>
#include <sys/dnode.h>
#include <sys/arc.h>
#include <sys/zio_checksum.h>
#include <sys/zil.h>

/* The block size of the space map of a record. */
#define	ASYNC_MIRROR_RECORD_BLKSZ	SPA_OLD_MAXBLOCKSIZE

/* The block shift of the record ZAP, as for a fat ZAP by default. */
#define	ASYNC_MIRROR_ZAP_SHIFT	14

/* About the most bytes of copies that one commit covers. */
#define	ASYNC_MIRROR_COMMIT_BYTES	(256ULL << 20)

/*
 * Used by the test suite.  zfs_vdev_async_mirror_suspend_copy stops the copy
 * to the lagging child, and the throttle of writes for the backlog, so that
 * an export leaves the child behind, as a crash does.
 * zfs_vdev_async_mirror_suspend_release stops the release of records.  The
 * failure or the freeze of a copy is then not recorded in the MOS either, and
 * no free of the records is reused, so a rewind can go back further.
 */
static int zfs_vdev_async_mirror_suspend_copy = 0;
static int zfs_vdev_async_mirror_suspend_release = 0;

/* Back-pressure: writes slow down near these limits, and wait above them. */
static uint64_t zfs_vdev_async_mirror_max_pending_bytes = 2ULL << 30;
static uint64_t zfs_vdev_async_mirror_max_pending_txgs = 256;

/* Entries that one release sync task may process. */
static uint64_t zfs_vdev_async_mirror_release_limit = 256 * 1024;

/* The most bytes of the copy in flight; see "The I/O of the walk". */
uint64_t zfs_vdev_async_mirror_copy_limit = 64ULL << 20;

/*
 * A read error during the copy of a txg can pass: the leader drops off the
 * bus for a few seconds, or its controller resets.  So the copy of the txg
 * is tried again, up to zfs_vdev_async_mirror_read_retries times, with
 * ASYNC_MIRROR_RETRY_MS between two tries.  Only then is the read error
 * final.
 */
uint_t zfs_vdev_async_mirror_read_retries = 10;

/* The value of the freeze entry in the MOS directory, and its reason. */
#define	ASYNC_MIRROR_FROZEN_WORDS	3
#define	ASYNC_MIRROR_FROZEN_METADATA	1

_Static_assert(sizeof (uberblock_t) <= sizeof (uint64_t) *
	ASYNC_MIRROR_REC_UB_WORDS, "uberblock_t must fit in amr_ub");
_Static_assert(ASYNC_MIRROR_REC_WORDS == 3 + ASYNC_MIRROR_REC_UB_WORDS,
	"async_mirror_rec_phys_t must have no padding");

/*
 * The deflated size of a held range, which the space accounting of the pool
 * subtracts.  A range on a log device is not in that space.
 */
static uint64_t
async_mirror_held_dspace(vdev_t *vd, uint64_t size)
{
	return (vd->vdev_islog ? 0 : vdev_deflated_space(vd, size));
}

/*
 * Copy the counters to the kstat of the pool.  The caller holds am_lock.
 * The counters that only this file changes are in spa_async_mirror_stats.
 */
void
async_mirror_update_stats(vdev_async_mirror_t *am)
{
	spa_async_mirror_stats_t *sams = &am->am_spa->spa_async_mirror_stats;

	ASSERT(MUTEX_HELD(&am->am_lock));

	sams->sams_active = async_mirror_has_child(am->am_state);
	sams->sams_failed = (am->am_state == AMS_FROZEN ||
	    am->am_state == AMS_FAILED);
	sams->sams_applied_txg = am->am_applied_txg;
	sams->sams_pending_bytes = am->am_pending_bytes;
	sams->sams_pending_txgs = am->am_pending_txgs;
	sams->sams_held_bytes = am->am_held_bytes;
	sams->sams_frozen = (am->am_state == AMS_FROZEN);
	sams->sams_frozen_txg = sams->sams_frozen ? am->am_applied_txg : 0;
	spa_async_mirror_stats_update(am->am_spa);
}

/*
 * Set the state of the async mirror.  A state with no lagging child lets go
 * of both children.  The caller holds am_lock.
 *
 * The I/O path reads the state of a leaf, vdev_async_state, with no lock.
 * A stale value there is harmless: each change happens at the start of a
 * txg, before that txg issues a write, or is covered by the newest open txg
 * that the change records (am_pass_txg, am_fail_open_txg), so a reader that
 * sees the old value acts as an I/O issued just before the change.
 */
void
async_mirror_set_state(vdev_async_mirror_t *am, async_mirror_state_t state)
{
	ASSERT(MUTEX_HELD(&am->am_lock));
	ASSERT(!async_mirror_has_child(state) || am->am_lagging != NULL);

	if (am->am_lagging != NULL) {
		atomic_store_32(&am->am_lagging->vdev_async_state,
		    async_mirror_has_child(state) ? state : AMS_OFF);
	}
	if (!async_mirror_has_child(state)) {
		am->am_lagging = NULL;
		am->am_leader = NULL;
	}
	atomic_store_32(&am->am_state, state);
	/* Wake the writes that wait for the copy. */
	cv_broadcast(&am->am_drain_cv);
}

/*
 * True while each txg needs a complete record and its frees must stay held:
 * the async mirror is on, or the copy failed or froze and the txg that records
 * this in the MOS is not on disk yet.  Until that txg is on disk, a crash
 * leaves the lagging child at its txg, with no DTL and no freeze entry, and
 * the next import resumes the replay from the records.  So the records must
 * still list every block of every txg, and the blocks that the child's txg
 * still uses must not be reused.  The failure is decided in the middle of a
 * txg: a write issued before it is dropped for the child, and that txg's
 * list is written after it.
 */
static boolean_t
async_mirror_tracking(vdev_async_mirror_t *am)
{
	uint32_t state = atomic_load_32(&am->am_state);

	return (async_mirror_copying(state) ||
	    ((state == AMS_FROZEN || state == AMS_FAILED) &&
	    !async_mirror_durable(am, am->am_halt_txg)));
}

/* Allocate the in-memory part of a record. */
static async_mirror_txg_t *
async_mirror_txg_alloc(uint64_t txg, uint64_t obj)
{
	async_mirror_txg_t *at = kmem_zalloc(sizeof (*at), KM_SLEEP);

	ASSERT3U(txg, !=, 0);
	ASSERT3U(obj, !=, 0);

	at->at_txg = txg;
	at->at_obj = obj;
	async_mirror_defer_list_create(&at->at_defer);
	return (at);
}

/*
 * Find or create the record for this txg.  Create the MOS ZAP first if
 * this is the first record.  Only the sync thread calls this.
 */
static async_mirror_txg_t *
async_mirror_record_get(vdev_async_mirror_t *am, dmu_tx_t *tx)
{
	spa_t *spa = am->am_spa;
	objset_t *mos = spa_meta_objset(spa);
	uint64_t txg = dmu_tx_get_txg(tx);
	async_mirror_txg_t *at = list_tail(&am->am_txgs);

	ASSERT(dsl_pool_sync_context(dmu_objset_pool(mos)));
	ASSERT3U(txg, ==, spa_syncing_txg(spa));
	ASSERT3P(am->am_lagging, !=, NULL);

	if (at != NULL && at->at_txg == txg)
		return (at);
	ASSERT(at == NULL || at->at_txg < txg);

	if (am->am_zap_obj == 0) {
		uint64_t zap = zap_create_flags(mos, 0,
		    ZAP_FLAG_HASH64 | ZAP_FLAG_UINT64_KEY, DMU_OTN_ZAP_METADATA,
		    ASYNC_MIRROR_ZAP_SHIFT, ASYNC_MIRROR_ZAP_SHIFT, DMU_OT_NONE,
		    0, tx);

		VERIFY0(zap_add(mos, DMU_POOL_DIRECTORY_OBJECT,
		    DMU_POOL_ASYNC_MIRROR_ZAP, sizeof (uint64_t), 1, &zap, tx));
		spa_feature_incr(spa, SPA_FEATURE_ASYNC_MIRROR, tx);
		mutex_enter(&am->am_lock);
		am->am_zap_obj = zap;
		mutex_exit(&am->am_lock);
	}

	uint64_t obj = space_map_alloc(mos, ASYNC_MIRROR_RECORD_BLKSZ, tx);
	async_mirror_rec_phys_t rec = {
		.amr_version = ASYNC_MIRROR_REC_VERSION,
		.amr_sm_obj = obj,
		.amr_leaf_guid = am->am_lagging->vdev_guid,
	};

	VERIFY3U(obj, !=, 0);
	memcpy(rec.amr_ub, &am->am_last_ub, sizeof (uberblock_t));
	VERIFY0(zap_add_uint64(mos, am->am_zap_obj, &txg, 1,
	    sizeof (uint64_t), ASYNC_MIRROR_REC_WORDS, &rec, tx));

	at = async_mirror_txg_alloc(txg, obj);
	/*
	 * Sync tasks run before the metaslabs sync, so a release in this
	 * txg has already set am_release_only_txg.
	 */
	at->at_release = (am->am_release_only_txg == txg);
	/*
	 * Like the log space map, accept any segment.  Two-word entries are
	 * always used, because each entry carries a vdev id.
	 */
	VERIFY0(space_map_open(&at->at_sm, mos, obj, 0, UINT64_MAX,
	    SPA_MINBLOCKSHIFT));

	mutex_enter(&am->am_lock);
	list_insert_tail(&am->am_txgs, at);
	mutex_exit(&am->am_lock);
	ASSERT3P(at->at_sm, !=, NULL);
	ASSERT(!at->at_ready);
	return (at);
}

/*
 * True if the freeze entry must go: the freeze has ended, at an import, or
 * by the switch off, in the txg that sets the DTLs of the child.
 */
static boolean_t
async_mirror_frozen_clear_due(vdev_async_mirror_t *am)
{
	ASSERT(MUTEX_HELD(&am->am_lock) ||
	    dsl_pool_sync_context(spa_get_dsl(am->am_spa)));

	return (am->am_frozen_entry && am->am_state != AMS_FROZEN &&
	    (am->am_state == AMS_DRAINING || am->am_dtl_txg != 0));
}

/*
 * Remove the freeze entry of a freeze that has ended.  The first txg that
 * writes the MOS does it; an import waits for that txg, so an export right
 * after the import cannot leave the entry behind.  Otherwise the next
 * import would end the freeze again, and mark the child, which had normal
 * writes since, for a full resilver.
 */
static void
async_mirror_frozen_clear(vdev_async_mirror_t *am, dmu_tx_t *tx)
{
	spa_t *spa = am->am_spa;

	if (!async_mirror_frozen_clear_due(am))
		return;
	VERIFY0(zap_remove(spa_meta_objset(spa), DMU_POOL_DIRECTORY_OBJECT,
	    DMU_POOL_ASYNC_MIRROR_FROZEN, tx));
	spa_feature_decr(spa, SPA_FEATURE_ASYNC_MIRROR, tx);
	mutex_enter(&am->am_lock);
	am->am_frozen_entry = B_FALSE;
	mutex_exit(&am->am_lock);
	spa_history_log_internal(spa, "async mirror", tx,
	    "the freeze of the lagging child is cleared");
}

/*
 * Make sure that a txg which changes the MOS has a record.  A txg can have
 * an uberblock and no dirty metaslab on our vdev, and the replay still needs
 * a record for it.
 */
void
vdev_async_mirror_sync_record(spa_t *spa, dmu_tx_t *tx)
{
	vdev_async_mirror_t *am = spa->spa_async_mirror;

	ASSERT(dsl_pool_sync_context(dmu_objset_pool(spa_meta_objset(spa))));

	if (am == NULL)
		return;
	async_mirror_frozen_clear(am, tx);
	if (!async_mirror_tracking(am))
		return;
	(void) async_mirror_record_get(am, tx);

	if (am->am_claim_rt != NULL && dmu_tx_get_txg(tx) == am->am_claim_txg &&
	    !zfs_range_tree_is_empty(am->am_claim_rt)) {
		vdev_async_mirror_sync_write(spa, am->am_lagging->vdev_top,
		    am->am_claim_rt, SM_ALLOC, tx);
		zfs_range_tree_vacate(am->am_claim_rt, NULL, NULL);
	}
}

/*
 * The claim at import found a block that a txg on disk allocated, so the
 * claim does not allocate it again, and the record of the claim txg
 * would not have it.  A log block was written while the async mirror was on, so
 * the lagging child does not have it: list it in the claim txg.  See
 * vdev_async_mirror_intercept_write().  A data block of a write record is
 * in the list of its own txg.  The load calls this before the sync thread
 * starts.
 */
void
vdev_async_mirror_claim_committed(spa_t *spa, const blkptr_t *bp,
    uint64_t txg)
{
	vdev_async_mirror_t *am = spa->spa_async_mirror;

	if (am == NULL || am->am_lagging == NULL ||
	    BP_GET_TYPE(bp) != DMU_OT_INTENT_LOG)
		return;

	ASSERT(!spa->spa_sync_on);

	/* The claim can run in a thread that holds no config lock. */
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		const dva_t *dva = &bp->blk_dva[d];
		vdev_t *vd = vdev_lookup_top(spa, DVA_GET_VDEV(dva));
		uint64_t offset = DVA_GET_OFFSET(dva);
		uint64_t asize = DVA_GET_ASIZE(dva);

		if (vd == NULL || !vdev_async_mirror_tracks(vd) ||
		    DVA_GET_GANG(dva) || asize == 0)
			continue;

		mutex_enter(&am->am_lock);
		if (am->am_claim_rt == NULL) {
			am->am_claim_rt = zfs_range_tree_create(NULL,
			    ZFS_RANGE_SEG64, NULL, 0, 0);
		}
		ASSERT(am->am_claim_txg == 0 || am->am_claim_txg == txg);
		am->am_claim_txg = txg;
		zfs_range_tree_clear(am->am_claim_rt, offset, asize);
		zfs_range_tree_add(am->am_claim_rt, offset, asize);
		mutex_exit(&am->am_lock);
	}
	spa_config_exit(spa, SCL_VDEV, FTAG);
}

/* The range of birth txgs of the blocks of the claimed logs. */
typedef struct async_mirror_log_arg {
	uint64_t	ala_lo;
	uint64_t	ala_hi;
} async_mirror_log_arg_t;

static void
async_mirror_log_note(async_mirror_log_arg_t *ala, uint64_t birth)
{
	ala->ala_lo = MIN(ala->ala_lo, birth);
	ala->ala_hi = MAX(ala->ala_hi, birth);
}

static int
async_mirror_log_blk_cb(zilog_t *zilog, const blkptr_t *bp, void *arg,
    uint64_t claim_txg)
{
	(void) zilog, (void) claim_txg;

	if (!BP_IS_HOLE(bp))
		async_mirror_log_note(arg, BP_GET_BIRTH(bp));
	return (0);
}

/*
 * A write record points to a data block that dmu_sync() wrote.  A block
 * born at or after the claim txg belongs to a txg that never synced, so
 * only the claim made it part of the pool, as dsl_scan_zil_record() does.
 */
static int
async_mirror_log_lr_cb(zilog_t *zilog, const lr_t *lrc, void *arg,
    uint64_t claim_txg)
{
	(void) zilog;

	if (lrc->lrc_txtype != TX_WRITE ||
	    lrc->lrc_reclen < sizeof (lr_write_t))
		return (0);

	const blkptr_t *bp = &((const lr_write_t *)lrc)->lr_blkptr;

	if (!BP_IS_HOLE(bp) && BP_GET_BIRTH(bp) >= claim_txg)
		async_mirror_log_note(arg, BP_GET_BIRTH(bp));
	return (0);
}

static int
async_mirror_log_ds_cb(dsl_pool_t *dp, dsl_dataset_t *ds, void *arg)
{
	objset_t *os;

	if (ds->ds_is_snapshot || dmu_objset_from_ds(ds, &os) != 0)
		return (0);

	zil_header_t *zh = &os->os_zil_header;

	if (zh->zh_claim_txg == 0 || BP_IS_HOLE(&zh->zh_log))
		return (0);

	zilog_t *zilog = zil_alloc(dp->dp_meta_objset, zh);

	(void) zil_parse(zilog, async_mirror_log_blk_cb, async_mirror_log_lr_cb,
	    arg, zh->zh_claim_txg, B_FALSE);
	zil_free(zilog);
	return (0);
}

/*
 * This import took a child out of the async mirror, and the pool keeps it as a
 * normal child of the mirror, with DTLs up to the txg of the import.
 * While the async mirror was on, every intent log block was dropped for that
 * child, and so was each block that dmu_sync() wrote in a txg that did
 * not sync.  A log that is claimed, at this import or at an earlier one,
 * is part of the pool until its replay frees it, and the replay may run
 * later, or on that child alone.  The resilver visits the claimed logs,
 * but only blocks born in the DTLs: log blocks can be older than the
 * child's txg, and the claimed data blocks are newer than the import.  So
 * add the birth txgs of every block of every claimed log to the DTLs of
 * each such child.  The load calls this after the claim, before the sync
 * thread starts.
 */
void
vdev_async_mirror_claimed(spa_t *spa)
{
	dsl_pool_t *dp = spa_get_dsl(spa);
	const uint64_t *resync = spa->spa_async_mirror_import.ami_resync;
	async_mirror_log_arg_t ala = { .ala_lo = UINT64_MAX, .ala_hi = 0 };

	ASSERT(!spa->spa_sync_on);

	if (!spa_writeable(spa) || resync[0] == 0)
		return;

	(void) dmu_objset_find_dp(dp, dp->dp_root_dir_obj,
	    async_mirror_log_ds_cb, &ala, DS_FIND_CHILDREN);
	if (ala.ala_lo > ala.ala_hi)
		return;

	for (int i = 0; i < 2; i++) {
		vdev_t *vd = (resync[i] == 0) ? NULL :
		    vdev_lookup_by_guid(spa->spa_root_vdev, resync[i]);

		if (vd == NULL || !vd->vdev_ops->vdev_op_leaf)
			continue;
		zfs_dbgmsg("async mirror: the claimed logs of this import "
		    "are on txgs %llu to %llu", (u_longlong_t)ala.ala_lo,
		    (u_longlong_t)ala.ala_hi);
		async_mirror_mark_dtl(spa, vd, ala.ala_lo,
		    ala.ala_hi - ala.ala_lo + 1);
	}
}

/*
 * Append rt to this txg's space map.  The caller drops ms_lock across this
 * call, as metaslab_sync() does for the other space map writes.
 */
void
vdev_async_mirror_sync_write(spa_t *spa, vdev_t *vd, zfs_range_tree_t *rt,
    maptype_t type, dmu_tx_t *tx)
{
	vdev_async_mirror_t *am = spa->spa_async_mirror;
	async_mirror_txg_t *at = async_mirror_record_get(am, tx);
	uint64_t space = zfs_range_tree_space(rt);

	ASSERT(dsl_pool_sync_context(dmu_objset_pool(spa_meta_objset(spa))));
	ASSERT(type == SM_ALLOC || type == SM_FREE);
	ASSERT(am->am_lagging->vdev_top == vd ||
	    (type == SM_FREE && vd->vdev_islog));
	ASSERT3U(at->at_txg, ==, dmu_tx_get_txg(tx));
	ASSERT3P(at->at_sm, !=, NULL);
	ASSERT(!at->at_ready);

	space_map_write(at->at_sm, rt, type, vd->vdev_id, tx);

	mutex_enter(&am->am_lock);
	if (type == SM_ALLOC) {
		at->at_bytes += space;
	} else {
		at->at_held += space;
		am->am_held_bytes += space;
		am->am_held_dspace += async_mirror_held_dspace(vd, space);
		if (vd->vdev_islog)
			am->am_held_log_bytes += space;
		async_mirror_update_stats(am);
	}
	mutex_exit(&am->am_lock);
}

/* Remove one range from the range tree in arg. */
static void
async_mirror_rt_clear_cb(void *arg, uint64_t start, uint64_t size)
{
	zfs_range_tree_clear(arg, start, size);
}

/*
 * Append this sync pass's allocations to the record, minus nolist.
 * nolist, which can be NULL, holds ranges that are only on the leader, for
 * example intent log blocks.  This runs in each sync pass, so the list's own
 * MOS blocks are recorded as the passes converge, as the log space map does.
 */
void
vdev_async_mirror_sync_list(spa_t *spa, vdev_t *vd, zfs_range_tree_t *alloctree,
    zfs_range_tree_t *nolist, dmu_tx_t *tx)
{
	if (zfs_range_tree_is_empty(alloctree))
		return;

	if (nolist == NULL || zfs_range_tree_is_empty(nolist)) {
		vdev_async_mirror_sync_write(spa, vd, alloctree, SM_ALLOC, tx);
		return;
	}

	zfs_range_tree_t *rt = zfs_range_tree_create(NULL, alloctree->rt_type,
	    NULL, alloctree->rt_start, alloctree->rt_shift);

	zfs_range_tree_walk(alloctree, zfs_range_tree_add, rt);
	zfs_range_tree_walk(nolist, async_mirror_rt_clear_cb, rt);
	if (!zfs_range_tree_is_empty(rt))
		vdev_async_mirror_sync_write(spa, vd, rt, SM_ALLOC, tx);
	zfs_range_tree_vacate(rt, NULL, NULL);
	zfs_range_tree_destroy(rt);
}

/*
 * True if vd is the lagging child, so normal writes must not touch it.  A
 * frozen child stays one: it keeps its older txg.  Reads of older blocks
 * can still use it; see vdev_async_mirror_read_fenced().
 */
boolean_t
vdev_async_mirror_is_async(vdev_t *vd)
{
	uint32_t state = atomic_load_32(&vd->vdev_async_state);

	return (async_mirror_copying(state) || state == AMS_FROZEN);
}

/*
 * True if the pool must not write the labels and uberblocks of this child.
 * After a failed replay, normal mirror writes resume at once, but the child
 * must not get a current uberblock until the txg that marks it for a
 * resilver is on disk.  Otherwise a crash in between leaves the child
 * claiming a txg it does not hold, with nothing to say that it is stale.
 */
boolean_t
vdev_async_mirror_skip_labels(const vdev_t *vd)
{
	vdev_async_mirror_t *am = vd->vdev_spa->spa_async_mirror;

	return (am != NULL && am->am_lagging == vd &&
	    !async_mirror_durable(am, am->am_dtl_txg));
}

/*
 * True if the pool must not write uberblocks to this leaf: the lagging
 * child (see vdev_async_mirror_skip_labels()), and a leaf of a log device
 * while the lagging child still holds an older txg.  A txg with a dirty
 * config writes its uberblock to every top-level vdev, including log
 * devices.  An import selects the newest uberblock from any readable leaf.
 * The MOS of a newer uberblock on a log device may refer to blocks that the
 * lagging child does not hold.  The lagging child must remain importable at
 * its own txg.  The leader still gets every uberblock.
 */
boolean_t
vdev_async_mirror_skip_uberblock(const vdev_t *vd)
{
	vdev_async_mirror_t *am = vd->vdev_spa->spa_async_mirror;

	if (vdev_async_mirror_skip_labels(vd))
		return (B_TRUE);
	return (am != NULL && am->am_lagging != NULL &&
	    !async_mirror_durable(am, am->am_dtl_txg) &&
	    vd->vdev_top != NULL && vd->vdev_top->vdev_islog);
}

/*
 * True if a write through the mirror must not reach this child.  While the
 * async mirror is on, every such write to the lagging child is dropped here.
 * Replay copies a txg's blocks from the leader, leaf by leaf and in txg
 * order, and finds them through the durable record.
 *
 * This includes repair writes.  vdev_indirect_repair() writes through the
 * mirror with ZIO_FLAG_IO_REPAIR, and ddt_repair_entry() writes with txg 0.
 * Either would write the lagging child out of txg order.  Resilver and
 * self-heal writes do not come here: vdev_mirror_io_done() sends them to
 * the leaf directly, and keeps them off the lagging child.
 *
 * Intent log blocks that this import writes stay on the leader: the replay
 * list leaves them out.  An import of the lagging child alone finds no
 * valid log chain and treats the log as empty, which is correct for its
 * older txg: those writes are newer than it.  A log that an import claims
 * is different.  From the claim txg on it is part of the pool, and the
 * replay at mount puts its writes in the tree.  So the claim lists its
 * blocks in the claim txg, and their frees are held; see metaslab_claim()
 * and metaslab_free().
 *
 * In the first step of a switch off, writes reach the child again; see
 * "The property" above.
 */
boolean_t
vdev_async_mirror_intercept_write(zio_t *zio, vdev_t *vd)
{
	uint32_t state = atomic_load_32(&vd->vdev_async_state);

	ASSERT3U(zio->io_type, ==, ZIO_TYPE_WRITE);

	return (state == AMS_ACTIVE || state == AMS_FROZEN);
}

/* True if this pool uses, or is still draining, an async mirror. */
boolean_t
vdev_async_mirror_in_use(spa_t *spa)
{
	vdev_async_mirror_t *am = spa->spa_async_mirror;

	if (am == NULL)
		return (B_FALSE);

	uint32_t state = atomic_load_32(&am->am_state);

	return (async_mirror_has_child(state) || state == AMS_DRAINING);
}

/*
 * True if this top-level vdev cannot be removed.  Its records and holds
 * need it, and a free on a removing vdev bypasses the hold.  While records
 * drain, refuse removal of every vdev, as this is short.  A log device
 * cannot be removed while a claimed log is held on a log device: the
 * removal needs every block of the device to be free.
 */
boolean_t
vdev_async_mirror_removal_refused(const vdev_t *vd)
{
	vdev_async_mirror_t *am = vd->vdev_spa->spa_async_mirror;

	ASSERT3P(vd->vdev_top, ==, vd);

	if (am == NULL)
		return (B_FALSE);

	mutex_enter(&am->am_lock);
	boolean_t refused = (am->am_lagging != NULL &&
	    am->am_lagging->vdev_top == vd) ||
	    (vd->vdev_islog && am->am_held_log_bytes != 0) ||
	    (async_mirror_draining(am->am_state) && am->am_zap_obj != 0);
	mutex_exit(&am->am_lock);
	return (refused);
}

/* True if the async mirror is on for this pool. */
boolean_t
vdev_async_mirror_active(spa_t *spa)
{
	vdev_async_mirror_t *am = spa->spa_async_mirror;

	return (am != NULL &&
	    async_mirror_has_child(atomic_load_32(&am->am_state)));
}

/*
 * True if the async mirror is on and vd is one of its two children.  These
 * children cannot be detached, replaced, split or offlined because the code
 * keeps pointers to both.  Other paths must not leave the lagging child
 * behind.  Turn the async mirror off first.
 */
boolean_t
vdev_async_mirror_is_child(const vdev_t *vd)
{
	vdev_async_mirror_t *am = vd->vdev_spa->spa_async_mirror;

	if (am == NULL)
		return (B_FALSE);

	mutex_enter(&am->am_lock);
	boolean_t child = (am->am_lagging != NULL &&
	    (vd == am->am_lagging || vd == am->am_leader));
	mutex_exit(&am->am_lock);
	return (child);
}

/*
 * True if this top-level vdev has an asynchronous child whose txgs need
 * records and held frees.  See async_mirror_tracking().
 */
boolean_t
vdev_async_mirror_tracks(const vdev_t *vd)
{
	vdev_async_mirror_t *am = vd->vdev_spa->spa_async_mirror;

	ASSERT3P(vd->vdev_top, ==, vd);

	return (am != NULL && am->am_lagging != NULL &&
	    am->am_lagging->vdev_top == vd && async_mirror_tracking(am));
}

/*
 * True if a free from this top-level vdev must be held.  kind says what the
 * block is; see metaslab_free_kind_t.  The replay never copies a log block
 * of this import, METASLAB_FREE_ZIL.
 *
 * Every other free on the mirror is held, also of a block that the lagging
 * child already has: until the child has passed the free, its txg points to
 * the block.  Writes can reach the child before then, in the first step of
 * a switch off and after a failed copy, so a reuse of the range would damage
 * the txg that the child holds.
 *
 * A claimed log is part of the pool from the claim txg to the txg of the
 * free, so the lagging child's txgs in that range point to its blocks.  On
 * the async mirror, the replay copies them in the claim txg; see
 * async_mirror_walk_zil().  On a log device, an import of the lagging
 * child with the log device reads them there.  Either way they must not be
 * reused until the lagging child has passed the free.  Nothing else on a
 * log device is held.  The lagging child's txgs also point to the log
 * that this import writes, but that log holds writes newer than those
 * txgs, which an import of the child alone does not keep in any case.
 */
boolean_t
vdev_async_mirror_should_hold(const vdev_t *vd, metaslab_free_kind_t kind)
{
	vdev_async_mirror_t *am = vd->vdev_spa->spa_async_mirror;

	if (kind == METASLAB_FREE_ZIL)
		return (B_FALSE);
	if (vd->vdev_islog) {
		return (kind == METASLAB_FREE_ZIL_CLAIMED && am != NULL &&
		    am->am_lagging != NULL && async_mirror_tracking(am) &&
		    !vd->vdev_removing);
	}
	return (vdev_async_mirror_tracks(vd));
}

/* Held space, in deflated bytes.  The space accounting subtracts it. */
uint64_t
spa_get_async_mirror_held_space(spa_t *spa)
{
	vdev_async_mirror_t *am = spa->spa_async_mirror;

	return (am != NULL ? am->am_held_dspace : 0);
}

/*
 * True if the lagging child gets every write of the txg of at from the pool:
 * it is a txg after the first step of a switch off.  Such a record is not in
 * the backlog, and the switch off does not wait for its copy; see "The
 * property" above.
 */
static boolean_t
async_mirror_passed(vdev_async_mirror_t *am, async_mirror_txg_t *at)
{
	ASSERT(MUTEX_HELD(&am->am_lock));

	return (am->am_state == AMS_PASSTHROUGH &&
	    at->at_txg > am->am_pass_txg);
}

/*
 * The txg's uberblock is on disk.  Close the record's space map, keep the
 * uberblock for the next record, and let the worker copy this txg.
 */
void
vdev_async_mirror_txg_synced(spa_t *spa, uint64_t txg)
{
	vdev_async_mirror_t *am = spa->spa_async_mirror;

	if (am == NULL)
		return;

	mutex_enter(&am->am_lock);
	async_mirror_txg_t *at = list_tail(&am->am_txgs);

	ASSERT3U(txg, ==, spa_syncing_txg(spa));

	/*
	 * A txg after spa_freeze() writes no uberblock, and its frees are
	 * dropped, so it never reaches the disk.  Its record is never ready:
	 * the lagging child must not get a txg that the leader lacks, or the
	 * next import selects it and the frozen txgs leak their frees.
	 */
	if (txg > spa_freeze_txg(spa)) {
		if (at != NULL && at->at_txg == txg && at->at_sm != NULL) {
			space_map_close(at->at_sm);
			at->at_sm = NULL;
		}
		mutex_exit(&am->am_lock);
		return;
	}

	am->am_synced_txg = txg;

	if (at != NULL && at->at_txg == txg && !at->at_ready) {
		if (at->at_sm != NULL) {
			at->at_sm_len = space_map_length(at->at_sm);
			space_map_close(at->at_sm);
			at->at_sm = NULL;
		}
		at->at_ub = spa->spa_uberblock;
		at->at_ready = B_TRUE;
		at->at_ready_time = gethrtime();
		am->am_last_ub = at->at_ub;
		if (!async_mirror_passed(am, at)) {
			am->am_pending_bytes += at->at_bytes;
			am->am_pending_txgs++;
		}
		ASSERT3U(at->at_txg, >, am->am_applied_txg);
		am->am_spa->spa_async_mirror_stats.sams_synced_txg = txg;
		async_mirror_update_stats(am);
		cv_broadcast(&am->am_cv);
	}
	mutex_exit(&am->am_lock);
}

/*
 * True while writes wait for the copy: it runs and can make progress.  A
 * reader with no lock gets a hint, which only changes the delay of one
 * transaction.
 */
static boolean_t
async_mirror_throttling(vdev_async_mirror_t *am)
{
	return (async_mirror_copying(atomic_load_32(&am->am_state)) &&
	    am->am_thread != NULL && !am->am_stop &&
	    !zfs_vdev_async_mirror_suspend_copy);
}

/* True if the backlog of the copy is above one of its limits. */
static boolean_t
async_mirror_over_limit(vdev_async_mirror_t *am)
{
	return (async_mirror_throttling(am) &&
	    (am->am_pending_bytes > zfs_vdev_async_mirror_max_pending_bytes ||
	    am->am_pending_txgs > zfs_vdev_async_mirror_max_pending_txgs));
}

/*
 * The least time of a transaction for a backlog of pending units against a
 * limit of max units: the curve of the dirty data throttle; see
 * dmu_tx_delay().  At and above the limit, the most delay.
 */
static hrtime_t
async_mirror_delay_curve(uint64_t pending, uint64_t max)
{
	if (max > UINT64_MAX / 100)
		return (0);

	uint64_t min = max * zfs_delay_min_dirty_percent / 100;

	if (pending <= min)
		return (0);
	if (pending >= max)
		return (INT64_MAX);
	return (zfs_delay_scale * (pending - min) / (max - pending));
}

/*
 * The least time that a transaction of a dataset takes for the backlog of
 * the copy, or 0; dmu_tx_delay() caps it at its most delay.  The bytes are
 * counted in MiB, so that the product with zfs_delay_scale cannot overflow.
 * See "Back-pressure" at the top of this file.
 */
hrtime_t
vdev_async_mirror_delay(spa_t *spa)
{
	vdev_async_mirror_t *am = spa->spa_async_mirror;

	if (am == NULL || !async_mirror_throttling(am))
		return (0);
	if (async_mirror_over_limit(am))
		return (INT64_MAX);

	hrtime_t bytes = async_mirror_delay_curve(am->am_pending_bytes >> 20,
	    zfs_vdev_async_mirror_max_pending_bytes >> 20);
	hrtime_t txgs = async_mirror_delay_curve(am->am_pending_txgs,
	    zfs_vdev_async_mirror_max_pending_txgs);

	return (MAX(bytes, txgs));
}

/*
 * Wait while the backlog of the copy is above one of its limits, as
 * dmu_tx_wait() waits while the dirty data is above zfs_dirty_data_max.
 * The caller holds no lock and no txg.  The worker wakes the waiters after
 * each copy, and a change of state wakes them too; the timeout only notices
 * a change of zfs_vdev_async_mirror_suspend_copy.
 */
void
vdev_async_mirror_wait(spa_t *spa)
{
	vdev_async_mirror_t *am = spa->spa_async_mirror;

	if (am == NULL || !async_mirror_over_limit(am))
		return;

	hrtime_t start = gethrtime();

	mutex_enter(&am->am_lock);
	while (async_mirror_over_limit(am)) {
		(void) cv_timedwait(&am->am_drain_cv, &am->am_lock,
		    ddi_get_lbolt() + hz);
	}
	spa->spa_async_mirror_stats.sams_throttled_ms +=
	    NSEC2MSEC(gethrtime() - start);
	async_mirror_update_stats(am);
	mutex_exit(&am->am_lock);
}

/*
 * The oldest record that the lagging child has not got, if it is ready and
 * the replay copies it now.  An export copies the records that the switch
 * off need not wait for, so that the child is equal to its mirror.
 */
static async_mirror_txg_t *
async_mirror_next_ready(vdev_async_mirror_t *am)
{
	ASSERT(MUTEX_HELD(&am->am_lock));

	for (async_mirror_txg_t *at = list_head(&am->am_txgs); at != NULL;
	    at = list_next(&am->am_txgs, at)) {
		if (at->at_txg > am->am_applied_txg) {
			return (at->at_ready && (am->am_stop ||
			    !async_mirror_passed(am, at)) ? at : NULL);
		}
	}
	return (NULL);
}

/*
 * The replay worker.  Copy the ready records to the lagging child, oldest
 * first.  Stop when asked to and no ready record is left, when a switch
 * off has made the child a normal child, or on the first error.  After an
 * error the records are released and the lagging child is marked for a
 * resilver.  It is a plain thread, not a zthr: spa_async_suspend() cancels
 * the zthrs, and an export still needs the worker to copy what is left;
 * see vdev_async_mirror_stop().
 *
 * A read error is not final at once: the copy of the txg is tried again,
 * zfs_vdev_async_mirror_read_retries times, with a delay before each try,
 * and one event announces each burst of tries.  An export ends the wait;
 * the records stay, and the next import resumes the copy.
 *
 * The freeze.  When a block of a txg that is not a lost data block cannot be
 * read, also after the retries, the lagging child stays at the txg before: the
 * newest state that it holds in full.  The records are released and no new
 * ones are written, as after a failure, but the child is not resilvered and
 * gets no I/O: no read, no write, no uberblock.  Normal mirror writes would
 * move it to the new txgs, with that block missing.  A freeze entry in the
 * MOS keeps the state across imports; see async_mirror_frozen_load().
 *
 * A write of a deferred block that failed in the release thread fails the
 * copy here, as a write error of the replay does.
 *
 * One commit takes the ready txgs up to about ASYNC_MIRROR_COMMIT_BYTES of
 * copies; see async_mirror_commit().  The bound limits the copy that a
 * crash repeats, and the time for which the child's newest uberblock lags
 * behind the copy.
 */
__attribute__((noreturn)) void
async_mirror_thread(void *arg)
{
	vdev_async_mirror_t *am = arg;
	spa_async_mirror_stats_t *sams = &am->am_spa->spa_async_mirror_stats;

	mutex_enter(&am->am_lock);
	for (;;) {
		boolean_t suspended = zfs_vdev_async_mirror_suspend_copy;
		async_mirror_txg_t *at = suspended ? NULL :
		    async_mirror_next_ready(am);

		if (am->am_defer_failed && async_mirror_copying(am->am_state)) {
			async_mirror_halt(am, am->am_applied_txg,
			    SET_ERROR(EIO), ASYNC_MIRROR_ERR_WRITE);
			break;
		}
		if (at == NULL) {
			if (am->am_stop || !async_mirror_copying(am->am_state))
				break;
			if (suspended) {
				/* Nothing signals the end of the suspend. */
				(void) cv_timedwait_idle(&am->am_cv,
				    &am->am_lock, ddi_get_lbolt() + hz);
			} else {
				cv_wait_idle(&am->am_cv, &am->am_lock);
			}
			continue;
		}

		ASSERT(at->at_ready);
		ASSERT3U(at->at_txg, >, am->am_applied_txg);
		/* In an error episode, defer a lost data block at once. */
		boolean_t quick = am->am_defer_quick;
		mutex_exit(&am->am_lock);
		async_mirror_err_t kind;
		uint_t retries = zfs_vdev_async_mirror_read_retries;
		uint_t tries = 0;
		uint_t ndef = 0;
		list_t def;

		async_mirror_defer_list_create(&def);
		int err = async_mirror_copy(am, at, retries == 0 || quick,
		    &kind, &def, &ndef);
		boolean_t stopped = B_FALSE;

		/* A read error can pass.  Wait, and try the txg again. */
		while (err != 0 && kind != ASYNC_MIRROR_ERR_WRITE &&
		    tries < retries) {
			if (tries == 0)
				async_mirror_retry_event(am, at->at_txg, err);
			if (!async_mirror_retry_wait(am)) {
				stopped = B_TRUE;
				break;
			}
			tries++;
			err = async_mirror_copy(am, at, tries == retries,
			    &kind, &def, &ndef);
		}

		/*
		 * Copy the next ready txgs too, and commit them together; see
		 * async_mirror_commit().  A txg with deferred blocks ends the
		 * commit.  A write error fails the copy.  After a read error,
		 * commit the txgs before; the txg that failed is then the
		 * first of the next commit, with its own tries.
		 */
		async_mirror_txg_t *last = at;
		async_mirror_txg_t *failed = at;
		uint64_t bytes = at->at_bytes;

		while (err == 0 && !stopped && ndef == 0 &&
		    bytes < ASYNC_MIRROR_COMMIT_BYTES) {
			async_mirror_txg_t *next;

			mutex_enter(&am->am_lock);
			next = list_next(&am->am_txgs, last);

			if (next != NULL && (!next->at_ready ||
			    zfs_vdev_async_mirror_suspend_copy ||
			    (!am->am_stop && async_mirror_passed(am, next))))
				next = NULL;
			boolean_t nquick = am->am_defer_quick;

			mutex_exit(&am->am_lock);
			if (next == NULL)
				break;

			async_mirror_err_t nkind;
			int nerr = async_mirror_copy(am, next,
			    retries == 0 || nquick, &nkind, &def, &ndef);

			if (nerr != 0) {
				if (nkind == ASYNC_MIRROR_ERR_WRITE) {
					err = nerr;
					kind = nkind;
					failed = next;
				}
				break;
			}
			last = next;
			bytes += next->at_bytes;
			quick = nquick;
		}
		if (err == 0 && !stopped) {
			failed = last;
			err = async_mirror_commit(am, last, bytes > 0, &def,
			    ndef, &kind);
		}
		async_mirror_defer_list_free(&def);
		list_destroy(&def);
		mutex_enter(&am->am_lock);

		/*
		 * An export: the records stay, and the next import resumes.
		 * The same if the pool stopped before the deferred blocks of
		 * the txg were durable: its uberblock is not written.
		 */
		if (stopped || (err != 0 && kind == ASYNC_MIRROR_ERR_STOP))
			break;
		if (err != 0) {
			async_mirror_halt(am, failed->at_txg, err, kind);
			break;
		}
		if (quick && tries == 0 && last->at_ndefer_count > 0) {
			zfs_dbgmsg("async mirror: txg %llu: earlier deferred "
			    "blocks wait for their tries; these were deferred "
			    "with no tries of the copy",
			    (u_longlong_t)last->at_txg);
		}

		/* The child holds every txg up to the one of last. */
		zfs_dbgmsg("async mirror: txgs %llu to %llu are on the lagging "
		    "child, %llu bytes", (u_longlong_t)at->at_txg,
		    (u_longlong_t)last->at_txg, (u_longlong_t)bytes);
		hrtime_t now = gethrtime();

		for (;;) {
			uint64_t lag = NSEC2MSEC(now - at->at_ready_time);

			sams->sams_last_lag_ms = lag;
			if (lag > sams->sams_max_lag_ms)
				sams->sams_max_lag_ms = lag;
			if (!async_mirror_passed(am, at)) {
				ASSERT3U(am->am_pending_txgs, >, 0);
				ASSERT3U(am->am_pending_bytes, >=,
				    at->at_bytes);
				am->am_pending_bytes -= at->at_bytes;
				am->am_pending_txgs--;
			}
			if (at == last)
				break;
			at = list_next(&am->am_txgs, at);
		}
		am->am_applied_txg = last->at_txg;
		async_mirror_publish_txg(am);
		async_mirror_update_stats(am);
		cv_broadcast(&am->am_drain_cv);
		if (am->am_release_zthr != NULL) {
			mutex_exit(&am->am_lock);
			zthr_wakeup(am->am_release_zthr);
			mutex_enter(&am->am_lock);
		}
	}
	ASSERT(am->am_stop || !async_mirror_copying(am->am_state));
	am->am_thread = NULL;
	cv_broadcast(&am->am_drain_cv);
	mutex_exit(&am->am_lock);
	thread_exit();
}

/* Free the in-memory part of a record.  The record stays on disk. */
static void
async_mirror_txg_free(async_mirror_txg_t *at)
{
	ASSERT(!list_link_active(&at->at_node));

	if (at->at_sm != NULL)
		space_map_close(at->at_sm);
	async_mirror_defer_list_free(&at->at_defer);
	list_destroy(&at->at_defer);
	kmem_free(at, sizeof (*at));
}

/*
 * Set the persistent mark on the leaf lag, and clear it on every other leaf
 * of a top-level mirror.  lag is NULL to clear every mark.  txg is the
 * newest txg on lag.  A top-level vdev whose marks change gets a dirty
 * config, so the next txg writes the marks to the leader's labels and to
 * the MOS config.  The labels of the lagging child are not written while it
 * lags, so they never have the mark.
 *
 * The mark says that the leaf may hold an older txg than its mirror.  The
 * pool reads it before its first metadata read.  The caller holds
 * SCL_CONFIG as writer, or is the sync thread.
 */
void
async_mirror_mark(spa_t *spa, vdev_t *lag, uint64_t txg)
{
	vdev_t *rvd = spa->spa_root_vdev;

	ASSERT(spa_writeable(spa));
	ASSERT(spa_config_held(spa, SCL_CONFIG, RW_WRITER) ||
	    (dsl_pool_sync_context(spa_get_dsl(spa)) &&
	    spa_config_held(spa, SCL_CONFIG, RW_READER)));

	for (uint64_t c = 0; c < rvd->vdev_children; c++) {
		vdev_t *tvd = rvd->vdev_child[c];
		boolean_t dirty = B_FALSE;

		if (tvd->vdev_ops != &vdev_mirror_ops)
			continue;
		for (uint64_t i = 0; i < tvd->vdev_children; i++) {
			vdev_t *leaf = tvd->vdev_child[i];
			uint64_t want = (leaf == lag);

			if (leaf->vdev_async_lagging == want)
				continue;
			leaf->vdev_async_lagging = want;
			atomic_store_64(&leaf->vdev_async_txg, want ? txg : 0);
			dirty = B_TRUE;
			zfs_dbgmsg("async mirror: %s the mark of leaf guid "
			    "%llu", want ? "set" : "cleared",
			    (u_longlong_t)leaf->vdev_guid);
		}
		if (dirty)
			vdev_config_dirty(tvd);
	}
}

/*
 * The lagging child has missing txgs because its copy failed or its freeze
 * ended.  Normal writes reach it again, but a write issued
 * before, with the birth of a txg up to am_fail_open_txg, was dropped.  So
 * wait for that txg, and in it set the DTLs from am_fail_from to it and
 * clear the mark.  The DTLs then hold every txg that the child lacks, and no
 * txg that has not synced.  Until then the mark fences the reads of newer
 * blocks, the child gets no label, and the records stay, so a crash leaves
 * the child a valid copy of its own txg.  Called at the start of each txg.
 */
void
async_mirror_fail_sync(vdev_async_mirror_t *am, dmu_tx_t *tx)
{
	spa_t *spa = am->am_spa;
	vdev_t *lag = am->am_lagging;
	vdev_t *rvd = spa->spa_root_vdev;
	uint64_t txg = dmu_tx_get_txg(tx);
	uint64_t from = am->am_fail_from;

	ASSERT3U(am->am_state, ==, AMS_FAILED);
	ASSERT0(am->am_dtl_txg);
	ASSERT3U(from, <=, txg);

	if (txg < am->am_fail_open_txg ||
	    zfs_vdev_async_mirror_suspend_release)
		return;

	for (vdev_t *vd = lag; vd != rvd; vd = vd->vdev_parent)
		vdev_dtl_dirty(vd, DTL_PARTIAL, from, txg - from + 1);
	vdev_dtl_dirty(lag, DTL_MISSING, from, txg - from + 1);
	vdev_dirty(lag->vdev_top, VDD_DTL, lag, txg);
	async_mirror_mark(spa, NULL, 0);
	spa_async_request(spa, SPA_ASYNC_RESILVER);

	/* An ended freeze loses its entry in this txg too. */
	mutex_enter(&am->am_lock);
	am->am_dtl_txg = txg;
	if (am->am_halt_txg == 0)
		am->am_halt_txg = txg;
	mutex_exit(&am->am_lock);

	spa_history_log_internal(spa, "async mirror", tx, "the lagging child "
	    "guid %llu misses txgs %llu to %llu; it is resilvered",
	    (u_longlong_t)lag->vdev_guid, (u_longlong_t)from,
	    (u_longlong_t)txg);
	if (am->am_release_zthr != NULL)
		zthr_wakeup(am->am_release_zthr);
}

/*
 * True after spa_freeze(): no later txg reaches the disk, and the frees of
 * such a txg are dropped; see metaslab_free_impl().  A release in such a
 * txg would free held ranges in memory only.  The allocator could then put
 * intent log blocks there, and the next import could not claim them,
 * because the ranges are still held on disk: the replay would miss their
 * records.  So nothing is released while the pool is frozen.
 */
static boolean_t
async_mirror_release_frozen(vdev_async_mirror_t *am)
{
	return (spa_freeze_txg(am->am_spa) != UINT64_MAX);
}

/*
 * True if the oldest record can be released now.
 *
 * While the async mirror is on, a release writes MOS blocks in a txg, so it
 * creates a record for that txg.  If each record triggered another release,
 * an idle pool would keep writing forever: each release makes a record that
 * triggers the next release after it is copied.  A record of a txg
 * that only a release changed does not start a release on its own.  It is
 * released together with the next record that the pool writes for another
 * reason.  The records left behind on an idle pool are only those of the
 * last releases.  A release that joins a txg with other changes does not
 * mark its record: on a busy pool most txgs have a release, and their
 * records must still start the next one.
 *
 * Do not test only the newest record for this.  The copy of the newest
 * record is usually still in progress when the record before it has been
 * copied, so the newest record and the oldest record are not the same.
 *
 * A release that stopped at its entry limit always continues, whatever the
 * kind of the record that it did not finish.
 *
 * A record with deferred blocks that are not done yet stops the release, and
 * so does every newer record; see "Deferred blocks".
 */
static boolean_t
async_mirror_release_pending(vdev_async_mirror_t *am)
{
	ASSERT(MUTEX_HELD(&am->am_lock));

	async_mirror_txg_t *at = list_head(&am->am_txgs);

	boolean_t draining = async_mirror_draining(am->am_state);

	if ((am->am_state == AMS_FROZEN && !am->am_frozen_entry) ||
	    async_mirror_frozen_clear_due(am))
		return (B_TRUE);
	if (at == NULL) {
		return (draining && (am->am_zap_obj != 0 ||
		    am->am_defer_zap != 0 || am->am_off_entry));
	}
	if (draining || am->am_release_more)
		return (B_TRUE);

	for (; at != NULL && at->at_ready && at->at_txg <= am->am_applied_txg;
	    at = list_next(&am->am_txgs, at)) {
		if (at->at_ndefer > 0)
			return (B_FALSE);
		if (!at->at_release)
			return (B_TRUE);
	}
	return (B_FALSE);
}

typedef struct async_mirror_release_arg {
	vdev_async_mirror_t	*ara_am;
	uint64_t	ara_txg;
	uint64_t	ara_limit;
	uint64_t	ara_bytes;	/* released from the current record */
	uint64_t	ara_dspace;
	uint64_t	ara_log_bytes;
} async_mirror_release_arg_t;

static int
async_mirror_release_cb(space_map_entry_t *sme, void *arg)
{
	async_mirror_release_arg_t *ara = arg;
	vdev_async_mirror_t *am = ara->ara_am;

	if (ara->ara_limit == 0)
		return (SET_ERROR(EINTR));

	ara->ara_limit--;
	if (sme->sme_type != SM_FREE)
		return (0);

	spa_t *spa = am->am_spa;
	vdev_t *vd = vdev_lookup_top(spa, sme->sme_vdev);

	/* The import checked each entry; see async_mirror_scan_cb(). */
	ASSERT3P(vd, !=, NULL);
	ASSERT3U(sme->sme_offset >> vd->vdev_ms_shift, <, vd->vdev_ms_count);

	metaslab_t *msp = vd->vdev_ms[sme->sme_offset >> vd->vdev_ms_shift];

	ASSERT3U(sme->sme_offset + sme->sme_run, <=,
	    msp->ms_start + msp->ms_size);

	mutex_enter(&msp->ms_lock);
	if (zfs_range_tree_is_empty(msp->ms_freeing))
		vdev_dirty(vd, VDD_METASLAB, msp, ara->ara_txg);
	zfs_range_tree_add(msp->ms_freeing, sme->sme_offset, sme->sme_run);
	mutex_exit(&msp->ms_lock);

	ara->ara_bytes += sme->sme_run;
	ara->ara_dspace += async_mirror_held_dspace(vd, sme->sme_run);
	if (vd->vdev_islog)
		ara->ara_log_bytes += sme->sme_run;
	return (0);
}

/* Take the space that the callback released off the held space. */
static void
async_mirror_release_account(async_mirror_release_arg_t *ara)
{
	vdev_async_mirror_t *am = ara->ara_am;

	mutex_enter(&am->am_lock);
	ASSERT3U(am->am_held_bytes, >=, ara->ara_bytes);
	ASSERT3U(am->am_held_dspace, >=, ara->ara_dspace);
	ASSERT3U(am->am_held_log_bytes, >=, ara->ara_log_bytes);
	am->am_held_bytes -= ara->ara_bytes;
	am->am_held_dspace -= ara->ara_dspace;
	am->am_held_log_bytes -= ara->ara_log_bytes;
	async_mirror_update_stats(am);
	mutex_exit(&am->am_lock);
	ara->ara_bytes = 0;
	ara->ara_dspace = 0;
	ara->ara_log_bytes = 0;
}

/*
 * Write the freeze entry of a child that froze: the frozen txg, the guid of
 * the child, and the reason.  It holds a reference on the feature of its
 * own, so that the feature stays active after the last record is gone, and
 * software without the feature cannot import the pool.
 */
static void
async_mirror_freeze_sync(vdev_async_mirror_t *am, dmu_tx_t *tx)
{
	spa_t *spa = am->am_spa;

	mutex_enter(&am->am_lock);
	boolean_t freeze = (am->am_state == AMS_FROZEN && !am->am_frozen_entry);
	mutex_exit(&am->am_lock);
	if (!freeze)
		return;

	uint64_t fz[ASYNC_MIRROR_FROZEN_WORDS] = {
		am->am_applied_txg, am->am_lagging->vdev_guid,
		ASYNC_MIRROR_FROZEN_METADATA
	};

	VERIFY0(zap_add(spa_meta_objset(spa), DMU_POOL_DIRECTORY_OBJECT,
	    DMU_POOL_ASYNC_MIRROR_FROZEN, sizeof (uint64_t),
	    ASYNC_MIRROR_FROZEN_WORDS, fz, tx));
	spa_feature_incr(spa, SPA_FEATURE_ASYNC_MIRROR, tx);
	mutex_enter(&am->am_lock);
	am->am_frozen_entry = B_TRUE;
	am->am_halt_txg = dmu_tx_get_txg(tx);
	mutex_exit(&am->am_lock);
	spa_history_log_internal(spa, "async mirror", tx, "a block cannot be "
	    "read; the lagging child guid %llu is frozen at txg %llu",
	    (u_longlong_t)fz[1], (u_longlong_t)fz[0]);
}

/*
 * Release records, oldest first, up to the entry limit.  Return true if a
 * record that can be released is left.
 */
static boolean_t
async_mirror_release_records(vdev_async_mirror_t *am, dmu_tx_t *tx)
{
	objset_t *mos = spa_meta_objset(am->am_spa);
	async_mirror_release_arg_t ara = {
		.ara_am = am,
		.ara_txg = dmu_tx_get_txg(tx),
		.ara_limit = zfs_vdev_async_mirror_release_limit
	};

	/*
	 * A release of every record frees ranges that a write of a deferred
	 * block can still target.  No such write starts once the state
	 * drains, so wait for the ones in progress.
	 */
	mutex_enter(&am->am_lock);
	while (async_mirror_draining(am->am_state) && am->am_defer_io)
		cv_wait(&am->am_drain_cv, &am->am_lock);
	/* After a failure, keep the records until the DTLs are set. */
	boolean_t keep = (am->am_state == AMS_FAILED && am->am_dtl_txg == 0);
	mutex_exit(&am->am_lock);

	for (;;) {
		mutex_enter(&am->am_lock);
		async_mirror_txg_t *at = list_head(&am->am_txgs);
		boolean_t draining = async_mirror_draining(am->am_state);
		boolean_t ok = at != NULL && !keep && (draining ||
		    (at->at_ready && at->at_txg <= am->am_applied_txg &&
		    at->at_ndefer == 0));
		mutex_exit(&am->am_lock);
		if (!ok)
			return (B_FALSE);
		if (ara.ara_limit == 0)
			return (B_TRUE);

		ASSERT3U(am->am_zap_obj, !=, 0);

		space_map_t *sm = NULL;

		VERIFY0(space_map_open(&sm, mos, at->at_obj, 0, UINT64_MAX,
		    SPA_MINBLOCKSHIFT));
		int error = space_map_incremental_destroy(sm,
		    async_mirror_release_cb, &ara, tx);
		async_mirror_release_account(&ara);
		if (error != 0) {
			if (error != EINTR) {
				zfs_panic_recover("zfs: error %d releasing "
				    "async mirror record of txg %llu", error,
				    (u_longlong_t)at->at_txg);
			}
			space_map_close(sm);
			return (error == EINTR);
		}
		space_map_free(sm, tx);
		space_map_close(sm);

		VERIFY0(zap_remove_uint64(mos, am->am_zap_obj, &at->at_txg, 1,
		    tx));
		if (am->am_defer_zap != 0)
			async_mirror_defer_clear(am, at->at_txg, tx);

		mutex_enter(&am->am_lock);
		if (at->at_ndefer > 0)
			async_mirror_defer_drop(am, at);
		list_remove(&am->am_txgs, at);
		mutex_exit(&am->am_lock);
		async_mirror_txg_free(at);
	}
}

/*
 * When the async mirror ends and its last record is gone, remove the record
 * ZAP, the deferred-block ZAP and the switch-off entry, and deactivate the
 * feature.
 */
static void
async_mirror_release_done(vdev_async_mirror_t *am, dmu_tx_t *tx)
{
	spa_t *spa = am->am_spa;
	objset_t *mos = spa_meta_objset(spa);

	mutex_enter(&am->am_lock);
	boolean_t drained = async_mirror_draining(am->am_state) &&
	    list_is_empty(&am->am_txgs);
	mutex_exit(&am->am_lock);
	if (!drained)
		return;

	if (am->am_zap_obj != 0) {
		ASSERT0(am->am_held_bytes);
		ASSERT0(am->am_held_dspace);
		ASSERT0(am->am_held_log_bytes);
		VERIFY0(zap_destroy(mos, am->am_zap_obj, tx));
		VERIFY0(zap_remove(mos, DMU_POOL_DIRECTORY_OBJECT,
		    DMU_POOL_ASYNC_MIRROR_ZAP, tx));
		spa_feature_decr(spa, SPA_FEATURE_ASYNC_MIRROR, tx);
		mutex_enter(&am->am_lock);
		am->am_zap_obj = 0;
		mutex_exit(&am->am_lock);
		spa_history_log_internal(spa, "async mirror", tx,
		    "released the last record");
	}
	if (am->am_defer_zap != 0) {
		VERIFY0(zap_destroy(mos, am->am_defer_zap, tx));
		VERIFY0(zap_remove(mos, DMU_POOL_DIRECTORY_OBJECT,
		    DMU_POOL_ASYNC_MIRROR_DEFERRED, tx));
		mutex_enter(&am->am_lock);
		am->am_defer_zap = 0;
		mutex_exit(&am->am_lock);
	}
	if (am->am_off_entry) {
		VERIFY0(zap_remove(mos, DMU_POOL_DIRECTORY_OBJECT,
		    DMU_POOL_ASYNC_MIRROR_OFF, tx));
		mutex_enter(&am->am_lock);
		am->am_off_entry = B_FALSE;
		mutex_exit(&am->am_lock);
	}
}

/*
 * The release sync task: record a freeze or the end of one, and release
 * records.  Sync tasks run in pass 1, before vdev_sync(), so the frees are
 * written in this same txg.
 */
static void
async_mirror_release_sync(void *arg, dmu_tx_t *tx)
{
	vdev_async_mirror_t *am = arg;
	spa_t *spa = am->am_spa;
	objset_t *mos = spa_meta_objset(spa);
	uint64_t txg = dmu_tx_get_txg(tx);

	ASSERT(dsl_pool_sync_context(dmu_objset_pool(mos)));
	ASSERT3U(txg, ==, spa_syncing_txg(spa));

	/* See async_mirror_release_frozen(). */
	if (txg > spa_freeze_txg(spa))
		return;

	/*
	 * Did anything but a release change this txg?  Sync tasks run after
	 * the MOS of the first pass is written.  So if the txg has other
	 * changes, the root block is already of this txg, or a sync task that
	 * ran before this one left the MOS dirty.  A sync task that runs
	 * after this one is not seen, and the txg counts as a release only;
	 * that only delays the next release.
	 */
	boolean_t only = BP_GET_LOGICAL_BIRTH(&spa->spa_uberblock.ub_rootbp) <
	    txg && !dmu_objset_is_dirty(mos, txg);

	async_mirror_freeze_sync(am, tx);
	async_mirror_frozen_clear(am, tx);

	/*
	 * The record of this txg, if any, does not exist yet, so that
	 * async_mirror_record_get() can mark it as a release record.
	 */
	mutex_enter(&am->am_lock);
	ASSERT(list_is_empty(&am->am_txgs) ||
	    ((async_mirror_txg_t *)list_tail(&am->am_txgs))->at_txg < txg);
	/* A second release in this txg sees the first one's MOS changes. */
	if (am->am_last_release_txg != txg)
		am->am_release_only_txg = only ? txg : 0;
	am->am_last_release_txg = txg;
	mutex_exit(&am->am_lock);

	boolean_t more = async_mirror_release_records(am, tx);

	mutex_enter(&am->am_lock);
	am->am_release_more = more;
	mutex_exit(&am->am_lock);
	async_mirror_release_done(am, tx);

	/*
	 * Records copied while this release waited for its txg, and the rest
	 * of a release that hit its limit, go in a later txg.
	 */
	if (am->am_release_zthr != NULL)
		zthr_wakeup(am->am_release_zthr);
}

/*
 * True if a release must run now, in a txg of its own: a freeze to record,
 * a freeze entry to remove, or every record to release.  Each happens once,
 * not once per copied txg.
 */
static boolean_t
async_mirror_release_urgent(vdev_async_mirror_t *am)
{
	ASSERT(MUTEX_HELD(&am->am_lock));

	return (async_mirror_draining(am->am_state) ||
	    (am->am_state == AMS_FROZEN && !am->am_frozen_entry) ||
	    async_mirror_frozen_clear_due(am));
}

/*
 * True if the release thread has work: a release is pending, and it is
 * urgent or no release waits for its txg yet.
 */
static boolean_t
async_mirror_release_wanted(vdev_async_mirror_t *am)
{
	ASSERT(MUTEX_HELD(&am->am_lock));

	if (!async_mirror_release_pending(am))
		return (B_FALSE);
	return (async_mirror_release_urgent(am) ||
	    am->am_release_queued_txg <= am->am_last_release_txg);
}

/*
 * The zthr check function: true if a record can be released, or deferred
 * blocks wait for a try.
 */
boolean_t
async_mirror_release_thread_check(void *arg, zthr_t *zthr)
{
	(void) zthr;
	vdev_async_mirror_t *am = arg;

	if (zfs_vdev_async_mirror_suspend_release ||
	    async_mirror_release_frozen(am))
		return (B_FALSE);

	mutex_enter(&am->am_lock);
	boolean_t wanted = async_mirror_release_wanted(am) ||
	    async_mirror_defer_next(am) != NULL;
	mutex_exit(&am->am_lock);
	return (wanted);
}

/* The most records whose space maps one release prefetches. */
#define	ASYNC_MIRROR_PREFETCH_RECS	32

/*
 * Prefetch the space maps that the release sync task will read, so that
 * the sync thread does not wait for reads.  A record that a sync task
 * releases meanwhile only makes a prefetch useless.
 */
static void
async_mirror_release_prefetch(vdev_async_mirror_t *am)
{
	objset_t *mos = spa_meta_objset(am->am_spa);
	uint64_t obj[ASYNC_MIRROR_PREFETCH_RECS];
	uint64_t len[ASYNC_MIRROR_PREFETCH_RECS];
	uint_t n = 0;

	mutex_enter(&am->am_lock);
	for (async_mirror_txg_t *at = list_head(&am->am_txgs);
	    at != NULL && at->at_ready && n < ASYNC_MIRROR_PREFETCH_RECS;
	    at = list_next(&am->am_txgs, at)) {
		if (!async_mirror_draining(am->am_state) &&
		    at->at_txg > am->am_applied_txg)
			break;
		obj[n] = at->at_obj;
		len[n] = at->at_sm_len;
		n++;
	}
	mutex_exit(&am->am_lock);

	for (uint_t i = 0; i < n; i++) {
		dmu_prefetch(mos, obj[i], 0, 0, len[i],
		    ZIO_PRIORITY_ASYNC_READ);
	}
}

/*
 * The zthr function: release records with sync tasks.
 *
 * A normal release does not force a txg.  It is queued in the open txg and
 * runs when that txg syncs for its own reasons: the writes of the pool, or
 * the txg timeout.  At most one release waits at a time, and it releases
 * every record that the lagging child has passed by then.  This avoids an
 * extra txg for every copied txg.  Without this limit, fast replay of small
 * txgs can sync tens of txgs a second and push older uberblocks, and their
 * rewind points, out of the ring.
 *
 * The wait changes no safety rule.  A record and its holds are released
 * only after the lagging child holds its txg, flushed, with the uberblock
 * of that txg or of a later one; see async_mirror_commit().  A record that
 * stays longer only keeps its blocks held longer.  A crash before the
 * release txg is on disk leaves the records, and the import releases or
 * resumes them as after any crash.
 *
 * An urgent release (see async_mirror_release_urgent()) still waits for its
 * own txg: it records a state change that other code waits for.
 *
 * The thread also tries the deferred blocks, after it has queued the release
 * of the records before them; see async_mirror_defer_retry().
 */
void
async_mirror_release_thread(void *arg, zthr_t *zthr)
{
	vdev_async_mirror_t *am = arg;
	spa_t *spa = am->am_spa;
	dsl_pool_t *dp = spa_get_dsl(spa);

	for (;;) {
		if (zthr_iscancelled(zthr) ||
		    zfs_vdev_async_mirror_suspend_release ||
		    async_mirror_release_frozen(am))
			return;

		mutex_enter(&am->am_lock);
		if (!async_mirror_release_wanted(am)) {
			boolean_t defer = (async_mirror_defer_next(am) != NULL);

			mutex_exit(&am->am_lock);
			if (!defer)
				return;
			async_mirror_defer_retry(am, zthr);
			continue;
		}
		boolean_t urgent = async_mirror_release_urgent(am);
		mutex_exit(&am->am_lock);
		async_mirror_release_prefetch(am);

		if (urgent) {
			VERIFY0(dsl_sync_task(spa_name(spa), NULL,
			    async_mirror_release_sync, am, 0,
			    ZFS_SPACE_CHECK_NONE));
			continue;
		}

		dmu_tx_t *tx = dmu_tx_create_dd(dp->dp_mos_dir);

		VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT | DMU_TX_SUSPEND));
		mutex_enter(&am->am_lock);
		am->am_release_queued_txg = dmu_tx_get_txg(tx);
		mutex_exit(&am->am_lock);
		dsl_sync_task_nowait(dp, async_mirror_release_sync, am, tx);
		dmu_tx_commit(tx);

		mutex_enter(&am->am_lock);
		boolean_t defer = (async_mirror_defer_next(am) != NULL);
		mutex_exit(&am->am_lock);
		if (!defer)
			return;
		async_mirror_defer_retry(am, zthr);
	}
}

/*
 * Create the thread that releases records.  Called as the pool opens, if
 * the import kept a state.  A pool with no state gets one, and this thread,
 * when the property turns the async mirror on; see async_mirror_state_get().
 */
void
vdev_async_mirror_spawn(spa_t *spa)
{
	vdev_async_mirror_t *am = spa->spa_async_mirror;

	if (am == NULL)
		return;

	ASSERT3P(am->am_release_zthr, ==, NULL);
	am->am_release_zthr = zthr_create("z_async_mirror_release",
	    async_mirror_release_thread_check, async_mirror_release_thread, am,
	    minclsyspri);
}

/*
 * Stop the release thread, as the other pool threads stop.  Also hold the
 * worker before it waits for a txg to make deferred blocks durable, and wait
 * for such a txg in progress: an export stops the sync thread after this.
 */
void
vdev_async_mirror_zthr_cancel(spa_t *spa)
{
	vdev_async_mirror_t *am = spa->spa_async_mirror;

	if (am == NULL)
		return;
	mutex_enter(&am->am_lock);
	am->am_quiesce++;
	while (am->am_persist == AMP_SYNC)
		cv_wait(&am->am_drain_cv, &am->am_lock);
	mutex_exit(&am->am_lock);
	if (am->am_release_zthr != NULL)
		zthr_cancel(am->am_release_zthr);
}

/* Let the release thread and the worker run again. */
void
vdev_async_mirror_zthr_resume(spa_t *spa)
{
	vdev_async_mirror_t *am = spa->spa_async_mirror;

	if (am == NULL)
		return;
	mutex_enter(&am->am_lock);
	if (am->am_quiesce > 0)
		am->am_quiesce--;
	cv_broadcast(&am->am_cv);
	mutex_exit(&am->am_lock);
	if (am->am_release_zthr != NULL)
		zthr_resume(am->am_release_zthr);
}

/* The state of one record read from the ZAP at import. */
typedef struct async_mirror_load_rec {
	uint64_t	alr_txg;
	uint64_t	alr_obj;
	uint64_t	alr_leaf;
	uberblock_t	alr_prev_ub;
} async_mirror_load_rec_t;

typedef struct async_mirror_scan_arg {
	spa_t		*asa_spa;
	uint64_t	asa_alloc;
	uint64_t	asa_held;
	uint64_t	asa_held_dspace;
	uint64_t	asa_held_log;
} async_mirror_scan_arg_t;

/*
 * Check one entry of a record at import, and add up its allocated or held
 * bytes.  The release and the copy trust each entry in syncing context, so
 * an entry that is not inside one metaslab fails the import here.
 */
static int
async_mirror_scan_cb(space_map_entry_t *sme, void *arg)
{
	async_mirror_scan_arg_t *asa = arg;
	vdev_t *vd = vdev_lookup_top(asa->asa_spa, sme->sme_vdev);
	uint64_t offset = sme->sme_offset;
	uint64_t run = sme->sme_run;

	if (vd == NULL || run == 0 || offset + run < offset ||
	    (offset >> vd->vdev_ms_shift) >= vd->vdev_ms_count)
		return (SET_ERROR(ECKSUM));

	metaslab_t *msp = vd->vdev_ms[offset >> vd->vdev_ms_shift];

	if (offset + run > msp->ms_start + msp->ms_size)
		return (SET_ERROR(ECKSUM));

	if (sme->sme_type == SM_ALLOC) {
		asa->asa_alloc += run;
	} else {
		asa->asa_held += run;
		asa->asa_held_dspace += async_mirror_held_dspace(vd, run);
		if (vd->vdev_islog)
			asa->asa_held_log += run;
	}
	return (0);
}

/*
 * Call cb for each record of the record ZAP zap.  Return EINVAL for an
 * entry that is not a record of this version, or the first error of a
 * read or of cb.  zdb uses this too.
 */
int
async_mirror_rec_iterate(objset_t *mos, uint64_t zap,
    async_mirror_rec_cb_t *cb, void *arg)
{
	async_mirror_rec_phys_t rec;
	zap_cursor_t zc;
	zap_attribute_t *za = zap_attribute_alloc();
	int error;

	for (zap_cursor_init(&zc, mos, zap);
	    (error = zap_cursor_retrieve(&zc, za)) == 0;
	    zap_cursor_advance(&zc)) {
		if (za->za_integer_length != sizeof (uint64_t) ||
		    za->za_num_integers != ASYNC_MIRROR_REC_WORDS) {
			error = SET_ERROR(EINVAL);
			break;
		}
		const uint64_t *txg = (const uint64_t *)za->za_name;

		error = zap_lookup_uint64(mos, zap, txg, 1, sizeof (uint64_t),
		    ASYNC_MIRROR_REC_WORDS, &rec);
		if (error == 0 && rec.amr_version != ASYNC_MIRROR_REC_VERSION)
			error = SET_ERROR(EINVAL);
		if (error == 0)
			error = cb(*txg, &rec, arg);
		if (error != 0)
			break;
	}
	zap_cursor_fini(&zc);
	zap_attribute_free(za);
	return (error == ENOENT ? 0 : error);
}

typedef struct async_mirror_load_arg {
	async_mirror_load_rec_t	*ala_recs;
	uint64_t		ala_count;
	uint64_t		ala_n;
} async_mirror_load_arg_t;

static int
async_mirror_load_rec_cb(uint64_t txg, const async_mirror_rec_phys_t *rec,
    void *arg)
{
	async_mirror_load_arg_t *ala = arg;

	if (ala->ala_n == ala->ala_count)
		return (SET_ERROR(EINVAL));

	async_mirror_load_rec_t *r = &ala->ala_recs[ala->ala_n++];

	r->alr_txg = txg;
	r->alr_obj = rec->amr_sm_obj;
	r->alr_leaf = rec->amr_leaf_guid;
	memcpy(&r->alr_prev_ub, rec->amr_ub, sizeof (uberblock_t));
	return (0);
}

static int
async_mirror_load_rec_compare(const void *a, const void *b)
{
	const async_mirror_load_rec_t *ra = a;
	const async_mirror_load_rec_t *rb = b;

	return (TREE_CMP(ra->alr_txg, rb->alr_txg));
}

/*
 * Read every record from the ZAP, sorted by txg.  Each record must name
 * the same leaf: an async mirror starts only with no record left, and each
 * async mirror has one lagging child.  On success the caller owns *recsp
 * (*np entries).
 */
static int
async_mirror_load_zap(spa_t *spa, uint64_t zap,
    async_mirror_load_rec_t **recsp, uint64_t *np)
{
	objset_t *mos = spa_meta_objset(spa);
	uint64_t count = 0;
	int error = zap_count(mos, zap, &count);

	*recsp = NULL;
	*np = 0;
	if (error != 0 || count == 0)
		return (error);

	size_t size = count * sizeof (async_mirror_load_rec_t);
	async_mirror_load_arg_t ala = {
		.ala_recs = kmem_zalloc(size, KM_SLEEP),
		.ala_count = count,
	};

	error = async_mirror_rec_iterate(mos, zap, async_mirror_load_rec_cb,
	    &ala);
	if (error == 0 && ala.ala_n != count)
		error = SET_ERROR(EINVAL);
	for (uint64_t i = 1; error == 0 && i < count; i++) {
		if (ala.ala_recs[i].alr_leaf != ala.ala_recs[0].alr_leaf)
			error = SET_ERROR(EINVAL);
	}
	if (error != 0) {
		kmem_free(ala.ala_recs, size);
		return (error);
	}

	qsort(ala.ala_recs, count, sizeof (async_mirror_load_rec_t),
	    async_mirror_load_rec_compare);
	*recsp = ala.ala_recs;
	*np = count;
	return (0);
}

/* One deferred block read from the MOS at import. */
typedef struct async_mirror_load_defer {
	uint64_t	ald_txg;
	blkptr_t	ald_bp;
	zbookmark_phys_t ald_zb;
} async_mirror_load_defer_t;

/*
 * Read the deferred blocks from their ZAP.  On success the caller owns
 * *defsp (*np entries).
 */
static int
async_mirror_load_defer(spa_t *spa, uint64_t zap,
    async_mirror_load_defer_t **defsp, uint64_t *np)
{
	objset_t *mos = spa_meta_objset(spa);
	uint64_t count = 0;
	int error = zap_count(mos, zap, &count);

	*defsp = NULL;
	*np = 0;
	if (error != 0 || count == 0)
		return (error);

	async_mirror_load_defer_t *defs = kmem_zalloc(count * sizeof (*defs),
	    KM_SLEEP);
	uint64_t val[ASYNC_MIRROR_DEFER_WORDS];
	uint64_t n = 0;
	zap_cursor_t zc;
	zap_attribute_t *za = zap_attribute_alloc();

	for (zap_cursor_init(&zc, mos, zap);
	    (error = zap_cursor_retrieve(&zc, za)) == 0;
	    zap_cursor_advance(&zc)) {
		if (n == count || za->za_integer_length != sizeof (uint64_t) ||
		    za->za_num_integers != ASYNC_MIRROR_DEFER_WORDS) {
			error = SET_ERROR(EINVAL);
			break;
		}
		error = zap_lookup(mos, zap, za->za_name, sizeof (uint64_t),
		    ASYNC_MIRROR_DEFER_WORDS, val);
		if (error != 0)
			break;

		async_mirror_load_defer_t *d = &defs[n++];

		d->ald_txg = val[0];
		memcpy(&d->ald_bp, &val[1], sizeof (blkptr_t));
		memcpy(&d->ald_zb,
		    &val[1 + sizeof (blkptr_t) / sizeof (uint64_t)],
		    sizeof (zbookmark_phys_t));
	}
	zap_cursor_fini(&zc);
	zap_attribute_free(za);

	if (error == ENOENT)
		error = (n == count) ? 0 : SET_ERROR(EINVAL);
	if (error != 0) {
		kmem_free(defs, count * sizeof (*defs));
		return (error);
	}
	*defsp = defs;
	*np = count;
	return (0);
}

/*
 * Attach the deferred blocks to their records, at an import that resumes the
 * copy.  A block of a txg above the lagging child's txg is not attached: the
 * copy of that txg is done again, and finds it again if it still fails.  Its
 * entry goes with the record.
 */
static void
async_mirror_defer_attach(vdev_async_mirror_t *am,
    const async_mirror_load_defer_t *defs, uint64_t nd)
{
	spa_async_mirror_stats_t *sams = &am->am_spa->spa_async_mirror_stats;

	for (async_mirror_txg_t *at = list_head(&am->am_txgs); at != NULL &&
	    at->at_txg <= am->am_applied_txg;
	    at = list_next(&am->am_txgs, at)) {
		uint_t k = 0;

		ASSERT(list_is_empty(&at->at_defer));
		for (uint64_t i = 0; i < nd; i++) {
			if (defs[i].ald_txg != at->at_txg)
				continue;

			async_mirror_defer_t *ad = kmem_zalloc(sizeof (*ad),
			    KM_SLEEP);

			ad->ad_bp = defs[i].ald_bp;
			ad->ad_zb = defs[i].ald_zb;
			list_insert_tail(&at->at_defer, ad);
			k++;
		}
		if (k == 0)
			continue;
		at->at_ndefer_count = k;
		at->at_ndefer = k;
		sams->sams_data_deferred += k;
		zfs_dbgmsg("async mirror: txg %llu: %u deferred data blocks "
		    "to try", (u_longlong_t)at->at_txg, k);
	}
	mutex_enter(&am->am_lock);
	async_mirror_defer_floor(am);
	mutex_exit(&am->am_lock);
}

/* Set the DTLs of the lagging child, so that a resilver repairs the range. */
void
async_mirror_mark_dtl(spa_t *spa, vdev_t *lag, uint64_t from, uint64_t size)
{
	vdev_t *rvd = spa->spa_root_vdev;

	ASSERT(lag->vdev_ops->vdev_op_leaf);
	ASSERT3U(size, >, 0);

	for (vdev_t *vd = lag; vd != rvd; vd = vd->vdev_parent)
		vdev_dtl_dirty(vd, DTL_PARTIAL, from, size);
	vdev_dtl_dirty(lag, DTL_MISSING, from, size);
	vdev_dirty(lag->vdev_top, VDD_DTL, lag, spa_first_txg(spa));
	spa->spa_async_mirror_stats.sams_resync_from = from;
	spa_async_mirror_stats_update(spa);
	zfs_dbgmsg("async mirror: lagging child guid %llu marked for "
	    "resilver of txgs %llu to %llu", (u_longlong_t)lag->vdev_guid,
	    (u_longlong_t)from, (u_longlong_t)(from + size - 1));
}

/*
 * Note a child that this import takes out of the async mirror while the pool
 * keeps it, so vdev_async_mirror_claimed() resilvers claimed logs on it.
 */
void
async_mirror_note_resync(spa_t *spa, vdev_t *vd)
{
	uint64_t *resync = spa->spa_async_mirror_import.ami_resync;

	for (int i = 0; i < 2; i++) {
		if (resync[i] == vd->vdev_guid)
			return;
		if (resync[i] == 0) {
			resync[i] = vd->vdev_guid;
			return;
		}
	}
}

/* Allocate and initialize the state of the pool.  There is no thread yet. */
vdev_async_mirror_t *
async_mirror_create(spa_t *spa)
{
	vdev_async_mirror_t *am = kmem_zalloc(sizeof (*am), KM_SLEEP);

	am->am_spa = spa;
	am->am_last_ub = spa->spa_uberblock;
	am->am_synced_txg = spa->spa_uberblock.ub_txg;
	mutex_init(&am->am_lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&am->am_cv, NULL, CV_DEFAULT, NULL);
	cv_init(&am->am_drain_cv, NULL, CV_DEFAULT, NULL);
	list_create(&am->am_txgs, sizeof (async_mirror_txg_t),
	    offsetof(async_mirror_txg_t, at_node));
	return (am);
}

/* Free the in-memory part of every record. */
static void
async_mirror_free_index(vdev_async_mirror_t *am)
{
	async_mirror_txg_t *at;

	while ((at = list_remove_head(&am->am_txgs)) != NULL)
		async_mirror_txg_free(at);
}

/* Free the state.  Its threads have exited. */
static void
async_mirror_destroy(vdev_async_mirror_t *am)
{
	ASSERT(list_is_empty(&am->am_txgs));
	ASSERT3P(am->am_thread, ==, NULL);
	ASSERT3P(am->am_release_zthr, ==, NULL);

	list_destroy(&am->am_txgs);
	cv_destroy(&am->am_cv);
	cv_destroy(&am->am_drain_cv);
	mutex_destroy(&am->am_lock);
	kmem_free(am, sizeof (*am));
}

/* True if vd is a leaf that the pool can read and write. */
boolean_t
async_mirror_child_usable(vdev_t *vd)
{
	return (vd != NULL && vd->vdev_ops->vdev_op_leaf &&
	    vdev_readable(vd) && vdev_writeable(vd));
}

/*
 * Read the uberblocks of this leaf.  ub gets the newest one at or below
 * spa_load_max_txg, so on a rewind it can be older than the newest one on
 * the leaf.  newest, if not NULL, gets the newest one with no filter.
 * vdev_uberblock_load() also sets spa_load_latest_ub_txg, so restore it.
 */
void
async_mirror_read_leaf_ub(spa_t *spa, vdev_t *leaf, uberblock_t *ub,
    uberblock_t *newest)
{
	nvlist_t *cfg = NULL;
	uint64_t saved = spa->spa_load_latest_ub_txg;

	if (newest != NULL)
		vdev_uberblock_load_newest(leaf, ub, newest, &cfg);
	else
		vdev_uberblock_load(leaf, ub, &cfg);
	spa->spa_load_latest_ub_txg = saved;
	if (cfg != NULL)
		nvlist_free(cfg);
}

/*
 * Read the newest txg of each leaf that the config marks as a lagging
 * child.  The load calls this before the first read of metadata through
 * the new vdev tree: once for the config that it is given, and again for
 * the trusted config from the MOS.  It reads one uberblock ring for each
 * marked leaf, and nothing for a pool without a mark.  The caller holds no
 * config lock.
 *
 * On a rewind the txg is the newest at or below spa_load_max_txg.  A leaf
 * that cannot be read gets 0, so every block is stale on it.
 *
 * A leaf that holds a txg above spa_load_max_txg is ahead of the rewind.
 * It holds txgs of the history that the rewind discards, and those can
 * have overwritten blocks that are live at the rewind txg.  So it also
 * gets 0 while another child of its mirror can be read.  When no other
 * child can be read, the leaf is the pool, as in a rewind of a pool
 * without the async mirror, so it keeps the newest txg at or below the limit.
 */
void
vdev_async_mirror_load_marked(spa_t *spa)
{
	vdev_t *rvd = spa->spa_root_vdev;

	for (uint64_t c = 0; c < rvd->vdev_children; c++) {
		vdev_t *tvd = rvd->vdev_child[c];

		if (tvd->vdev_ops != &vdev_mirror_ops)
			continue;
		for (uint64_t i = 0; i < tvd->vdev_children; i++) {
			vdev_t *leaf = tvd->vdev_child[i];
			uberblock_t ub, newest;
			boolean_t other = B_FALSE;

			/* The copy that a keep discards holds no txg. */
			if (leaf->vdev_guid == spa->spa_async_mirror_skip) {
				leaf->vdev_async_lagging = 1;
				leaf->vdev_async_txg = 0;
				continue;
			}
			if (!leaf->vdev_async_lagging)
				continue;
			leaf->vdev_async_txg = 0;
			if (!vdev_readable(leaf))
				continue;
			for (uint64_t j = 0; j < tvd->vdev_children; j++) {
				vdev_t *cvd = tvd->vdev_child[j];

				if (cvd != leaf && vdev_readable(cvd))
					other = B_TRUE;
			}
			async_mirror_read_leaf_ub(spa, leaf, &ub, &newest);
			if (newest.ub_txg > ub.ub_txg && other) {
				zfs_dbgmsg("async mirror: marked leaf guid "
				    "%llu holds txg %llu, above the rewind "
				    "txg; it is stale",
				    (u_longlong_t)leaf->vdev_guid,
				    (u_longlong_t)newest.ub_txg);
				continue;
			}
			leaf->vdev_async_txg = ub.ub_txg;
			zfs_dbgmsg("async mirror: marked leaf guid %llu holds "
			    "txg %llu", (u_longlong_t)leaf->vdev_guid,
			    (u_longlong_t)ub.ub_txg);
		}
	}
}

/*
 * True if vd is a marked lagging child and a block born in txg is newer
 * than its txg.  The block is not on this child, or not yet: the range can
 * hold older data, or another block that is free in the pool.
 */
boolean_t
vdev_async_mirror_stale(vdev_t *vd, uint64_t txg)
{
	return (vd->vdev_async_lagging &&
	    txg > atomic_load_64(&vd->vdev_async_txg));
}

/*
 * Publish the txg of the lagging child for the fence: the newest txg that
 * the copy has finished, or the txg before the oldest deferred block, which
 * is not on the child yet.  The caller holds am_lock.
 */
void
async_mirror_publish_txg(vdev_async_mirror_t *am)
{
	vdev_t *lag = am->am_lagging;
	uint64_t txg = am->am_applied_txg;

	ASSERT(MUTEX_HELD(&am->am_lock));

	if (!async_mirror_copying(am->am_state) || !lag->vdev_async_lagging)
		return;
	if (am->am_defer_txg != 0)
		txg = MIN(txg, am->am_defer_txg - 1);
	atomic_store_64(&lag->vdev_async_txg, txg);
}

/* True if vd is a frozen lagging child, which gets no I/O. */
static boolean_t
async_mirror_frozen(vdev_t *vd)
{
	return (atomic_load_32(&vd->vdev_async_state) == AMS_FROZEN);
}

/*
 * True if a mirror read must not use this child, not even as the last
 * resort.  Otherwise the mirror reads a marked lagging child only when no
 * other child can supply the block, and a scrub reads it too; see
 * vdev_mirror_child_select().
 *
 * A marked child holds every block born at or before its txg, also while
 * the replay copies newer txgs to it.  A block that is live now was live
 * in each txg after its birth, so no copied txg wrote over its range.
 * So when the leader cannot supply such a block, for
 * example because a copy is damaged, or because the leader missed the
 * txg, the lagging child is the only good copy.  Fencing it off then
 * turns a block that the pool can read into an I/O error, and for
 * metadata into a suspended pool.
 *
 * The read is fenced when the block is newer than the child's txg, and
 * when the read has no block pointer, so there is no birth txg and no
 * checksum to catch old data.  An active child without a mark has no
 * known txg, and a frozen child gets no reads, so both are fenced for
 * every read.
 *
 * An intent log block is different.  The replay copies it only once a
 * claim lists it, in the claim txg, not in its birth txg, so its birth
 * does not tell whether the child has it.  Each log block has its own
 * checksum, which an old block or a stale range fails.  While another
 * child can be read, the child is fenced for log blocks, so that the end
 * of a chain that it never got is not counted as checksum errors on it.
 * When it is the only child that can be read, it is the pool: read it.
 * A log that it does not hold then ends at a checksum error, and the
 * import treats it as empty, as for an older txg.  A fence there makes
 * the chain end in ENXIO, which fails the import.
 */
static boolean_t
async_mirror_other_readable(const vdev_t *vd)
{
	vdev_t *pvd = vd->vdev_parent;

	for (uint64_t c = 0; pvd != NULL && c < pvd->vdev_children; c++) {
		vdev_t *cvd = pvd->vdev_child[c];

		if (cvd != vd && vdev_readable(cvd))
			return (B_TRUE);
	}
	return (B_FALSE);
}

boolean_t
vdev_async_mirror_read_fenced(vdev_t *vd, const zio_t *zio)
{
	if (async_mirror_frozen(vd))
		return (B_TRUE);
	if (!vd->vdev_async_lagging)
		return (vdev_async_mirror_is_async(vd));
	if (zio->io_bp == NULL)
		return (B_TRUE);
	if (BP_GET_TYPE(zio->io_bp) == DMU_OT_INTENT_LOG)
		return (async_mirror_other_readable(vd));
	return (vdev_async_mirror_stale(vd, zio->io_txg));
}

/*
 * True if no repair write of the block of zio may go to this child.  A
 * marked child holds the blocks born at or before its txg with the bytes
 * of the leader (see above), so a repair of such a block writes what the
 * child must hold, in any txg order, and a scrub can repair it.  A newer
 * block, or an intent log block, can be missing on the child, and its
 * range can hold a block of the child's own older txg.  An active child
 * without a mark and a frozen child get no repair.
 */
boolean_t
vdev_async_mirror_no_repair(vdev_t *vd, const zio_t *zio)
{
	if (async_mirror_frozen(vd))
		return (B_TRUE);
	if (!vd->vdev_async_lagging)
		return (vdev_async_mirror_is_async(vd));
	return (zio->io_bp == NULL ||
	    BP_GET_TYPE(zio->io_bp) == DMU_OT_INTENT_LOG ||
	    vdev_async_mirror_stale(vd, zio->io_txg));
}

/*
 * True if this child still needs a resilver: its DTLs are not empty, or a
 * resilver is running or scheduled.  Such a child cannot become the lagging
 * child: the repair writes of the resilver would be dropped, the resilver
 * would finish, and the child would look healthy with holes in it.
 * Nor can it be the leader: the leader is the source of every copy and
 * then the only child that new writes reach, and it lacks the txgs that
 * its DTLs name.  This happens when the lagging child was imported alone
 * and written, and the leader returns.
 */
boolean_t
async_mirror_child_needs_resilver(spa_t *spa, vdev_t *vd)
{
	return (!vdev_dtl_empty(vd, DTL_MISSING) ||
	    !vdev_dtl_empty(vd, DTL_PARTIAL) ||
	    dsl_scan_resilvering(spa_get_dsl(spa)) ||
	    dsl_scan_resilver_scheduled(spa_get_dsl(spa)));
}

/*
 * True if the pool has only top-level vdevs that the async mirror supports.
 * Holds exist only on the async mirror, so any other vdev with pool
 * data would free and reuse blocks that the lagging child's older txg still
 * needs.  A log device is allowed: the lagging child never has log blocks,
 * and its older txg refers to no block on the log device, except to the
 * blocks of a claimed log, whose frees are held; see
 * vdev_async_mirror_should_hold().  Holes and indirect vdevs hold no blocks
 * of their own.
 */
static boolean_t
async_mirror_layout_ok(spa_t *spa, vdev_t *lag)
{
	vdev_t *rvd = spa->spa_root_vdev;

	for (uint64_t c = 0; c < rvd->vdev_children; c++) {
		vdev_t *tvd = rvd->vdev_child[c];

		if (tvd != lag->vdev_top && vdev_is_concrete(tvd) &&
		    !tvd->vdev_islog)
			return (B_FALSE);
	}
	return (B_TRUE);
}

/* True if both uberblocks point to the same root block. */
static boolean_t
async_mirror_same_root(const uberblock_t *a, const uberblock_t *b)
{
	return (memcmp(&a->ub_rootbp, &b->ub_rootbp, sizeof (blkptr_t)) == 0);
}

/*
 * Build the in-memory index from the records, and check each record.  The
 * uberblock of a record is the prev_ub of the next record, or the imported
 * uberblock for the newest record.  On an error the caller frees the index.
 */
static int
async_mirror_load_index(spa_t *spa, vdev_async_mirror_t *am,
    const async_mirror_load_rec_t *recs, uint64_t n)
{
	objset_t *mos = spa_meta_objset(spa);
	int error = 0;

	ASSERT(list_is_empty(&am->am_txgs));
	ASSERT0(am->am_held_bytes);

	spa_config_enter(spa, SCL_CONFIG, FTAG, RW_READER);
	for (uint64_t i = 0; i < n; i++) {
		const async_mirror_load_rec_t *r = &recs[i];
		async_mirror_scan_arg_t asa = { .asa_spa = spa };
		space_map_t *sm = NULL;

		ASSERT(i == 0 || recs[i - 1].alr_txg < r->alr_txg);
		if (r->alr_txg == 0 || r->alr_txg > spa->spa_uberblock.ub_txg ||
		    r->alr_obj == 0) {
			error = SET_ERROR(ECKSUM);
			break;
		}
		error = space_map_open(&sm, mos, r->alr_obj, 0, UINT64_MAX,
		    SPA_MINBLOCKSHIFT);
		if (error != 0)
			break;

		uint64_t sm_len = space_map_length(sm);

		error = space_map_iterate(sm, sm_len,
		    async_mirror_scan_cb, &asa);
		space_map_close(sm);
		if (error != 0)
			break;

		async_mirror_txg_t *at = async_mirror_txg_alloc(r->alr_txg,
		    r->alr_obj);

		at->at_sm_len = sm_len;
		at->at_bytes = asa.asa_alloc;
		at->at_held = asa.asa_held;
		am->am_held_bytes += asa.asa_held;
		am->am_held_dspace += asa.asa_held_dspace;
		am->am_held_log_bytes += asa.asa_held_log;
		at->at_ub = (i + 1 < n) ? recs[i + 1].alr_prev_ub :
		    spa->spa_uberblock;
		at->at_ready = B_TRUE;
		at->at_ready_time = gethrtime();
		list_insert_tail(&am->am_txgs, at);
	}
	spa_config_exit(spa, SCL_CONFIG, FTAG);
	return (error);
}

/* Count the records that the lagging child has not got. */
static void
async_mirror_load_pending(vdev_async_mirror_t *am)
{
	for (async_mirror_txg_t *at = list_head(&am->am_txgs); at != NULL;
	    at = list_next(&am->am_txgs, at)) {
		if (at->at_txg > am->am_applied_txg) {
			am->am_pending_bytes += at->at_bytes;
			am->am_pending_txgs++;
		}
	}
}

/*
 * A rewind selected import_txg, below the newest txg on the lagging child:
 * the child holds txgs of the history that the rewind discards.  Erase its
 * uberblocks.  The sync thread has not started, so nothing is written yet,
 * and the erase finishes before the first write.  The caller marks the
 * child for a full resilver.
 */
static void
async_mirror_erase_ahead(spa_t *spa, vdev_t *lag, uint64_t newest)
{
	uint64_t import_txg = spa->spa_uberblock.ub_txg;

	ASSERT(!spa->spa_sync_on);
	ASSERT3U(newest, >, import_txg);

	spa_load_note(spa, "async mirror: lagging child guid %llu "
	    "holds txg %llu, above the rewind txg %llu; it is erased "
	    "and resilvered", (u_longlong_t)lag->vdev_guid,
	    (u_longlong_t)newest, (u_longlong_t)import_txg);
	spa_history_log_internal(spa, "async mirror", NULL,
	    "rewind to txg %llu discards txgs up to %llu on the "
	    "lagging child guid %llu; the child is resilvered",
	    (u_longlong_t)import_txg, (u_longlong_t)newest,
	    (u_longlong_t)lag->vdev_guid);

	int error = vdev_uberblock_erase(lag, import_txg);

	if (error != 0) {
		/*
		 * Keep the child out of the pool.  Do not fail the import:
		 * the operator is recovering the pool.
		 */
		spa_load_note(spa, "async mirror: cannot erase the "
		    "uberblocks of the lagging child guid %llu, error "
		    "%d; the child is faulted",
		    (u_longlong_t)lag->vdev_guid, error);
		spa_config_enter(spa, SCL_STATE_ALL, FTAG, RW_WRITER);
		vdev_set_state(lag, B_FALSE, VDEV_STATE_FAULTED,
		    VDEV_AUX_IO_FAILURE);
		spa_config_exit(spa, SCL_STATE_ALL, FTAG);
	}
}

/* Set the state that the import found, before the threads start. */
static void
async_mirror_load_state(vdev_async_mirror_t *am, vdev_t *lag, vdev_t *lead,
    async_mirror_state_t state)
{
	ASSERT(!am->am_spa->spa_sync_on);
	ASSERT3B(lag != NULL, ==, async_mirror_has_child(state));

	mutex_enter(&am->am_lock);
	async_mirror_set_state(am, AMS_OFF);
	am->am_lagging = lag;
	am->am_leader = lead;
	async_mirror_set_state(am, state);
	mutex_exit(&am->am_lock);
}

/*
 * Decide what the records in the ZAP mean for this import.
 *
 * import_txg is the txg of the imported uberblock, and child_txg the txg of
 * the newest uberblock on the lagging child at or below the txg that the
 * import asks for.  Each record stores the previous record's uberblock, so
 * the record after child_txg must store exactly the child's uberblock.  That
 * links the child to this pool.  It also catches a gap.  Comparing child_txg
 * to the oldest record's txg would not work: a txg without changes has no
 * record, so the txgs of the records are not contiguous.
 *
 * A rewind can select import_txg while the child holds newer uberblocks.
 * The child then holds txgs of the history that the rewind discards: they
 * can have overwritten blocks that are live at import_txg, and an import of
 * the child alone would select them.  So when the newest uberblock on the
 * child is above import_txg and the leader is usable, the child is
 * divergent: erase its uberblocks before the pool writes anything, and
 * resilver it in full.  The test uses the newest uberblock with no filter,
 * because child_txg is never above import_txg.
 *
 * Replay resumes only when the async_mirror property of this child is still
 * lagging, and the link holds.  Otherwise the records are released.  Then,
 * if the child has no valid copy of the pool at import_txg, it gets a
 * resilver.  A resilver of the full pool is used when the child's state is
 * not known to be a prefix of the pool.  When the child is the only usable
 * copy, it is the pool, so it gets no resilver and no erase.
 *
 * With a freeze entry, replay never resumes, and only the erase is done
 * here.
 *
 * defer_txg is the oldest txg with deferred blocks, or UINT64_MAX.  The
 * resilver of a valid older child starts there if that is older than its
 * txg, so that it copies the deferred blocks; see "Deferred blocks".
 *
 * want is the guid of the leaf whose property is lagging, or 0.  off_txg is
 * the txg of a switch off that left these records, or 0: the child has been
 * a normal child of its mirror since then, so the records are released, and
 * nothing else is done.
 */
static void
async_mirror_resume_or_drain(spa_t *spa, vdev_async_mirror_t *am,
    const async_mirror_load_rec_t *recs, uint64_t n, boolean_t frozen,
    uint64_t defer_txg, uint64_t want, uint64_t off_txg)
{
	vdev_t *rvd = spa->spa_root_vdev;
	uint64_t leaf_guid = recs[0].alr_leaf;
	uint64_t import_txg = spa->spa_uberblock.ub_txg;

	ASSERT3U(n, >, 0);
	ASSERT3P(am->am_lagging, ==, NULL);

	if (off_txg != 0) {
		async_mirror_load_state(am, NULL, NULL, AMS_DRAINING);
		am->am_applied_txg = UINT64_MAX;
		zfs_dbgmsg("async mirror: leaf guid %llu left the async mirror "
		    "at txg %llu; releasing %llu records",
		    (u_longlong_t)leaf_guid, (u_longlong_t)off_txg,
		    (u_longlong_t)n);
		return;
	}

	vdev_t *lag = vdev_lookup_by_guid(rvd, leaf_guid);
	boolean_t usable = async_mirror_child_usable(lag) &&
	    lag->vdev_parent->vdev_ops == &vdev_mirror_ops &&
	    lag->vdev_parent->vdev_children == 2 &&
	    lag->vdev_parent == lag->vdev_top;
	boolean_t have_child = B_FALSE;
	uberblock_t child_ub, child_newest;
	vdev_t *lead = NULL;

	if (usable) {
		vdev_t *pvd = lag->vdev_parent;

		async_mirror_read_leaf_ub(spa, lag, &child_ub, &child_newest);
		have_child = B_TRUE;
		lead = pvd->vdev_child[pvd->vdev_child[0] == lag ? 1 : 0];
	}

	uint64_t child_txg = have_child ? child_ub.ub_txg : 0;
	boolean_t lead_ok = async_mirror_child_usable(lead);
	boolean_t ahead = have_child && lead_ok &&
	    child_newest.ub_txg > import_txg;
	boolean_t linked = B_FALSE;

	/*
	 * Each txg that writes the MOS while the records are kept has a
	 * record, so the newest record is the txg of the root block of the
	 * imported uberblock.
	 * If the records stopped before the pool did, the newest one is
	 * older.  Then the records do not list every block that the child
	 * lacks, and frees were not held, so a block that the child's txg
	 * uses can have been reused.  The child is not a valid older pool.
	 */
	uint64_t root_txg = BP_GET_LOGICAL_BIRTH(&spa->spa_uberblock.ub_rootbp);
	boolean_t complete = (recs[n - 1].alr_txg == root_txg);

	if (!complete) {
		zfs_dbgmsg("async mirror: the newest record is of txg %llu, "
		    "but the pool's root block is of txg %llu",
		    (u_longlong_t)recs[n - 1].alr_txg, (u_longlong_t)root_txg);
	}

	if (have_child && !ahead && child_txg <= import_txg && complete) {
		const async_mirror_load_rec_t *q = NULL;

		for (uint64_t i = 0; i < n; i++) {
			if (recs[i].alr_txg > child_txg) {
				q = &recs[i];
				break;
			}
		}
		if (q != NULL) {
			linked = (q->alr_prev_ub.ub_txg == child_txg &&
			    async_mirror_same_root(&q->alr_prev_ub, &child_ub));
		} else {
			linked = (child_txg == import_txg &&
			    async_mirror_same_root(&child_ub,
			    &spa->spa_uberblock));
		}
	}

	boolean_t resume = !frozen && linked && lead_ok &&
	    want == leaf_guid &&
	    !spa_feature_is_active(spa, SPA_FEATURE_POOL_CHECKPOINT) &&
	    !spa_multihost(spa) &&
	    async_mirror_layout_ok(spa, lag) &&
	    !async_mirror_child_needs_resilver(spa, lag) &&
	    !async_mirror_child_needs_resilver(spa, lead);

	if (resume) {
		async_mirror_load_state(am, lag, lead, AMS_ACTIVE);
		am->am_applied_txg = child_txg;
		zfs_dbgmsg("async mirror: resume, leaf guid %llu at txg %llu, "
		    "pool at txg %llu", (u_longlong_t)leaf_guid,
		    (u_longlong_t)child_txg, (u_longlong_t)import_txg);
		return;
	}

	/* Release the records.  The child keeps normal mirror writes. */
	async_mirror_load_state(am, NULL, NULL, AMS_DRAINING);
	am->am_applied_txg = UINT64_MAX;

	if (ahead)
		async_mirror_erase_ahead(spa, lag, child_newest.ub_txg);

	/* async_mirror_frozen_load() decides what the child needs. */
	if (frozen)
		return;

	if (lag == NULL || !lag->vdev_ops->vdev_op_leaf) {
		zfs_dbgmsg("async mirror: leaf guid %llu is gone; dropping "
		    "%llu records", (u_longlong_t)leaf_guid, (u_longlong_t)n);
	} else if (have_child && !lead_ok) {
		/* The child is the only copy, so it is the pool. */
		zfs_dbgmsg("async mirror: leaf guid %llu is the only copy; "
		    "dropping %llu records", (u_longlong_t)leaf_guid,
		    (u_longlong_t)n);
	} else if (have_child && linked) {
		/*
		 * The child is a valid older pool.  A resilver of the txgs
		 * it has not got and of its deferred blocks is enough, or
		 * nothing if it is at import_txg and has none.
		 */
		uint64_t from = MIN(child_txg + 1, defer_txg);

		if (from <= import_txg)
			async_mirror_mark_dtl(spa, lag, from,
			    import_txg - from + 1);
		async_mirror_note_resync(spa, lag);
	} else {
		/* Gap, lagging child ahead, not linked, or unreadable. */
		async_mirror_mark_dtl(spa, lag, TXG_INITIAL,
		    import_txg - TXG_INITIAL + 1);
		async_mirror_note_resync(spa, lag);
	}
}

/*
 * The MOS has a freeze entry: fz holds the frozen txg, frozen_txg, and the
 * guid of the lagging child.  Runs after the records are handled, so after
 * the erase of a child that is ahead of a rewind.
 *
 * The child stays frozen if its async_mirror property is still lagging
 * (want is its guid), both children are usable and need no resilver, and
 * its newest uberblock is still of frozen_txg.  It then keeps its mark, gets
 * no worker and no I/O, and the pool runs on the leader alone.
 *
 * Otherwise the freeze ends.  The entry is removed at the next release.  A
 * child that is still exactly at frozen_txg, a valid older copy of the pool,
 * is resilvered from there; any other child is resilvered in full.  A child
 * that is the only usable copy is the pool, so it gets no resilver.
 */
static void
async_mirror_frozen_load(spa_t *spa, vdev_async_mirror_t *am,
    const uint64_t *fz, uint64_t n, uint64_t want)
{
	vdev_t *rvd = spa->spa_root_vdev;
	uint64_t frozen_txg = fz[0];
	uint64_t leaf_guid = fz[1];
	uint64_t import_txg = spa->spa_uberblock.ub_txg;
	vdev_t *lag = vdev_lookup_by_guid(rvd, leaf_guid);
	vdev_t *lead = NULL;
	boolean_t at_frozen = B_FALSE;
	boolean_t usable = async_mirror_child_usable(lag) &&
	    lag->vdev_parent->vdev_ops == &vdev_mirror_ops &&
	    lag->vdev_parent->vdev_children == 2 &&
	    lag->vdev_parent == lag->vdev_top;

	ASSERT3P(am->am_lagging, ==, NULL);

	if (usable) {
		vdev_t *pvd = lag->vdev_parent;
		uberblock_t ub, newest;

		lead = pvd->vdev_child[pvd->vdev_child[0] == lag ? 1 : 0];
		async_mirror_read_leaf_ub(spa, lag, &ub, &newest);
		/* With records, async_mirror_resume_or_drain() erased it. */
		if (n == 0 && newest.ub_txg > import_txg &&
		    async_mirror_child_usable(lead)) {
			async_mirror_erase_ahead(spa, lag, newest.ub_txg);
		} else {
			at_frozen = (ub.ub_txg == frozen_txg &&
			    newest.ub_txg == frozen_txg);
		}
	}
	boolean_t lead_ok = async_mirror_child_usable(lead);

	if (usable && lead_ok && at_frozen && frozen_txg <= import_txg &&
	    want == leaf_guid &&
	    !async_mirror_child_needs_resilver(spa, lag) &&
	    !async_mirror_child_needs_resilver(spa, lead)) {
		async_mirror_load_state(am, lag, lead, AMS_FROZEN);
		am->am_applied_txg = frozen_txg;
		am->am_halt_txg = import_txg;
		spa_load_note(spa, "async mirror: the lagging child guid %llu "
		    "stays frozen at txg %llu", (u_longlong_t)leaf_guid,
		    (u_longlong_t)frozen_txg);
		return;
	}

	async_mirror_load_state(am, NULL, NULL, AMS_DRAINING);
	spa_load_note(spa, "async mirror: the freeze of the lagging child "
	    "guid %llu at txg %llu ends", (u_longlong_t)leaf_guid,
	    (u_longlong_t)frozen_txg);

	if (lag == NULL || !lag->vdev_ops->vdev_op_leaf) {
		zfs_dbgmsg("async mirror: frozen leaf guid %llu is gone",
		    (u_longlong_t)leaf_guid);
	} else if (usable && !lead_ok) {
		zfs_dbgmsg("async mirror: frozen leaf guid %llu is the only "
		    "copy", (u_longlong_t)leaf_guid);
	} else if (usable && at_frozen && frozen_txg <= import_txg) {
		if (frozen_txg < import_txg)
			async_mirror_mark_dtl(spa, lag, frozen_txg + 1,
			    import_txg - frozen_txg);
		async_mirror_note_resync(spa, lag);
	} else {
		async_mirror_mark_dtl(spa, lag, TXG_INITIAL,
		    import_txg - TXG_INITIAL + 1);
		async_mirror_note_resync(spa, lag);
	}
}

/* Call async_mirror_mark() at import, before the sync thread starts. */
static void
async_mirror_mark_at_load(spa_t *spa, vdev_t *lag, uint64_t txg)
{
	ASSERT(!spa->spa_sync_on);

	spa_config_enter(spa, SCL_CONFIG, FTAG, RW_WRITER);
	async_mirror_mark(spa, lag, txg);
	spa_config_exit(spa, SCL_CONFIG, FTAG);
}

/*
 * The guid of the leaf whose async_mirror property is lagging, or 0.  The
 * property names one leaf at most; if more than one leaf names itself,
 * none is taken.
 */
static uint64_t
async_mirror_wanted(spa_t *spa)
{
	vdev_t *rvd = spa->spa_root_vdev;
	uint64_t want = 0;

	for (uint64_t c = 0; c < rvd->vdev_children; c++) {
		vdev_t *tvd = rvd->vdev_child[c];

		if (tvd->vdev_ops != &vdev_mirror_ops)
			continue;
		for (uint64_t i = 0; i < tvd->vdev_children; i++) {
			vdev_t *leaf = tvd->vdev_child[i];

			if (leaf->vdev_async_mirror !=
			    VDEV_ASYNC_MIRROR_LAGGING)
				continue;
			if (want != 0) {
				zfs_dbgmsg("async mirror: more than one leaf "
				    "is lagging; taking none");
				return (0);
			}
			want = leaf->vdev_guid;
		}
	}
	return (want);
}

/*
 * Return 0 if the leaf lag can become the lagging child, with *leadp set
 * to the leader, or the error that says why not.  The import and the
 * property use the same rules.  The two children must be in sync, and
 * that is for the caller to check: at an import, by their uberblocks; for
 * the property, by the txg of the switch.
 */
int
async_mirror_can_start(spa_t *spa, vdev_t *lag, vdev_t **leadp)
{
	ASSERT(lag->vdev_ops->vdev_op_leaf);

	if (!spa_feature_is_enabled(spa, SPA_FEATURE_ASYNC_MIRROR))
		return (SET_ERROR(ENOTSUP));

	vdev_t *pvd = lag->vdev_parent;

	if (pvd->vdev_ops != &vdev_mirror_ops || pvd->vdev_children != 2 ||
	    pvd != lag->vdev_top || !async_mirror_layout_ok(spa, lag))
		return (SET_ERROR(ENOTSUP));
	/* MMP writes uberblocks to the lagging child. */
	if (spa_multihost(spa))
		return (SET_ERROR(ENOTSUP));
	if (spa_feature_is_active(spa, SPA_FEATURE_POOL_CHECKPOINT))
		return (SET_ERROR(ZFS_ERR_CHECKPOINT_EXISTS));

	vdev_t *lead = pvd->vdev_child[pvd->vdev_child[0] == lag ? 1 : 0];

	if (!async_mirror_child_usable(lag) || !async_mirror_child_usable(lead))
		return (SET_ERROR(ENXIO));
	if (async_mirror_child_needs_resilver(spa, lag) ||
	    async_mirror_child_needs_resilver(spa, lead))
		return (SET_ERROR(EBUSY));
	*leadp = lead;
	return (0);
}

/* What the MOS holds of the async mirror, read at import. */
typedef struct async_mirror_entries {
	uint64_t	ame_zap;	/* the record ZAP, or 0 */
	async_mirror_load_rec_t *ame_recs; /* its records, by txg */
	uint64_t	ame_nrecs;
	uint64_t	ame_dzap;	/* the deferred-block ZAP, or 0 */
	async_mirror_load_defer_t *ame_defs;
	uint64_t	ame_ndefs;
	uint64_t	ame_defer_txg;	/* oldest txg of a deferred block */
	boolean_t	ame_frozen;	/* the freeze entry exists */
	uint64_t	ame_fz[ASYNC_MIRROR_FROZEN_WORDS];
	boolean_t	ame_off;	/* the switch-off entry exists */
	uint64_t	ame_off_txg;
} async_mirror_entries_t;

/* Read an entry of the MOS directory.  A missing entry is no error. */
static int
async_mirror_dir_lookup(objset_t *mos, const char *name, uint64_t words,
    uint64_t *val, boolean_t *found)
{
	int error = zap_lookup(mos, DMU_POOL_DIRECTORY_OBJECT, name,
	    sizeof (uint64_t), words, val);

	*found = (error == 0);
	return (error == ENOENT ? 0 : error);
}

/* Read the entries of the async mirror.  The caller frees them in all cases. */
static int
async_mirror_entries_load(spa_t *spa, async_mirror_entries_t *ame)
{
	objset_t *mos = spa_meta_objset(spa);
	boolean_t zap, dzap;
	int error;

	memset(ame, 0, sizeof (*ame));
	ame->ame_defer_txg = UINT64_MAX;

	error = async_mirror_dir_lookup(mos, DMU_POOL_ASYNC_MIRROR_ZAP, 1,
	    &ame->ame_zap, &zap);
	if (error == 0 && zap) {
		error = async_mirror_load_zap(spa, ame->ame_zap,
		    &ame->ame_recs, &ame->ame_nrecs);
	}
	if (error == 0) {
		error = async_mirror_dir_lookup(mos,
		    DMU_POOL_ASYNC_MIRROR_FROZEN, ASYNC_MIRROR_FROZEN_WORDS,
		    ame->ame_fz, &ame->ame_frozen);
	}
	if (error == 0) {
		error = async_mirror_dir_lookup(mos,
		    DMU_POOL_ASYNC_MIRROR_DEFERRED, 1, &ame->ame_dzap, &dzap);
	}
	if (error == 0 && dzap) {
		error = async_mirror_load_defer(spa, ame->ame_dzap,
		    &ame->ame_defs, &ame->ame_ndefs);
	}
	if (error == 0) {
		error = async_mirror_dir_lookup(mos, DMU_POOL_ASYNC_MIRROR_OFF,
		    1, &ame->ame_off_txg, &ame->ame_off);
	}
	for (uint64_t i = 0; i < ame->ame_ndefs; i++) {
		ame->ame_defer_txg = MIN(ame->ame_defer_txg,
		    ame->ame_defs[i].ald_txg);
	}
	return (error);
}

static void
async_mirror_entries_free(async_mirror_entries_t *ame)
{
	if (ame->ame_recs != NULL) {
		kmem_free(ame->ame_recs,
		    ame->ame_nrecs * sizeof (*ame->ame_recs));
	}
	if (ame->ame_defs != NULL) {
		kmem_free(ame->ame_defs,
		    ame->ame_ndefs * sizeof (*ame->ame_defs));
	}
}

/*
 * The MOS holds nothing of an earlier async mirror, and the leaf property
 * want is lagging: start the async mirror if both children hold the imported
 * txg.
 */
static void
async_mirror_load_new(spa_t *spa, vdev_async_mirror_t *am, uint64_t want)
{
	vdev_t *lag = vdev_lookup_by_guid(spa->spa_root_vdev, want);
	vdev_t *lead = NULL;
	uint64_t import_txg = spa->spa_uberblock.ub_txg;
	uberblock_t ub, newest;

	if (lag == NULL || !lag->vdev_ops->vdev_op_leaf ||
	    async_mirror_can_start(spa, lag, &lead) != 0) {
		zfs_dbgmsg("async mirror: lagging leaf not found or not "
		    "usable");
		return;
	}

	async_mirror_read_leaf_ub(spa, lag, &ub, &newest);
	if (ub.ub_txg < import_txg) {
		/*
		 * A crash left no durable record.  Replay is in order, so a
		 * resilver of the newer txgs is enough.  Keep the normal
		 * mirror.
		 */
		async_mirror_mark_dtl(spa, lag, ub.ub_txg + 1,
		    import_txg - ub.ub_txg);
	} else if (newest.ub_txg > import_txg ||
	    !async_mirror_same_root(&ub, &spa->spa_uberblock)) {
		zfs_dbgmsg("async mirror: child ahead or divergent, not "
		    "activating");
	} else {
		async_mirror_load_state(am, lag, lead, AMS_ACTIVE);
		am->am_applied_txg = import_txg;
	}
}

/* Decide the state of the async mirror at import, from the entries and want. */
static void
async_mirror_load_mode(spa_t *spa, vdev_async_mirror_t *am,
    const async_mirror_entries_t *ame, uint64_t want)
{
	if (ame->ame_nrecs > 0) {
		async_mirror_resume_or_drain(spa, am, ame->ame_recs,
		    ame->ame_nrecs, ame->ame_frozen, ame->ame_defer_txg, want,
		    ame->ame_off ? MAX(ame->ame_off_txg, 1) : 0);
		async_mirror_load_pending(am);
		if (async_mirror_copying(am->am_state)) {
			async_mirror_defer_attach(am, ame->ame_defs,
			    ame->ame_ndefs);
		}
	} else if (ame->ame_zap != 0 || ame->ame_frozen ||
	    ame->ame_dzap != 0 || ame->ame_off) {
		/*
		 * A leftover empty ZAP: destroy it.  A freeze entry with
		 * no records: async_mirror_frozen_load() decides.
		 */
		async_mirror_load_state(am, NULL, NULL, AMS_DRAINING);
	} else if (want != 0) {
		async_mirror_load_new(spa, am, want);
	}

	if (ame->ame_frozen)
		async_mirror_frozen_load(spa, am, ame->ame_fz, ame->ame_nrecs,
		    want);
}

/*
 * Pick the lagging child at import.  Runs before the sync thread starts.
 * See async_mirror_resume_or_drain() for what the records in the ZAP mean.
 * An error reading the async mirror entries, or an invalid record,
 * fails the import: without the records, the lagging child would look like
 * a complete child of a normal mirror.
 *
 * The lagging child is the leaf whose async_mirror property is lagging.
 * A pool with no records, no freeze, and no leaf that asks for the async mirror
 * keeps no state.
 */
int
vdev_async_mirror_activate(spa_t *spa)
{
	async_mirror_entries_t ame;
	vdev_async_mirror_t *am = NULL;
	uint64_t want;
	int error;

	if (!spa_writeable(spa))
		return (0);

	spa->spa_async_mirror_import.ami_resync[0] = 0;
	spa->spa_async_mirror_import.ami_resync[1] = 0;
	error = async_mirror_erase_skipped(spa);
	if (error != 0)
		return (error);
	async_mirror_mark_erased(spa);

	error = async_mirror_entries_load(spa, &ame);
	if (error != 0)
		goto out;

	want = async_mirror_wanted(spa);
	am = async_mirror_create(spa);
	am->am_zap_obj = ame.ame_zap;
	am->am_defer_zap = ame.ame_dzap;
	am->am_off_entry = ame.ame_off;
	am->am_frozen_entry = ame.ame_frozen;

	/* Check every record before anything is changed. */
	error = async_mirror_load_index(spa, am, ame.ame_recs, ame.ame_nrecs);
	if (error != 0)
		goto out;
	async_mirror_load_mode(spa, am, &ame, want);

	/* The async mirror needs a lineage. */
	if (am->am_lagging != NULL && spa_async_mirror_lineage(spa) == 0)
		async_mirror_lineage_start(spa);
	async_mirror_lineage_update(spa,
	    ame.ame_zap != 0 || ame.ame_frozen || want != 0);

	/*
	 * Mark the lagging child, or clear the mark if this import keeps no
	 * lagging child.  A child that is drained gets DTLs above, so it
	 * needs no mark.
	 */
	async_mirror_mark_at_load(spa, am->am_lagging, am->am_applied_txg);

	/*
	 * Keep the state while the async mirror is on, records drain, or a leaf
	 * asks for the async mirror.  A pool that does not use it gets no state
	 * and no thread; the property creates them when it is set to lagging.
	 */
	if (am->am_state == AMS_OFF && want == 0)
		goto out;

	spa_async_mirror_stats_init(spa);
	spa->spa_async_mirror = am;
	mutex_enter(&am->am_lock);
	async_mirror_update_stats(am);
	if (async_mirror_copying(am->am_state)) {
		ASSERT3P(am->am_leader->vdev_parent, ==,
		    am->am_lagging->vdev_parent);
		am->am_thread = thread_create(NULL, 0, async_mirror_thread, am,
		    0, &p0, TS_RUN, minclsyspri);
		zfs_dbgmsg("async mirror: active, lagging child guid %llu at "
		    "txg %llu", (u_longlong_t)am->am_lagging->vdev_guid,
		    (u_longlong_t)am->am_applied_txg);
	}
	mutex_exit(&am->am_lock);
	am = NULL;
out:
	if (error != 0) {
		spa_load_note(spa, "async mirror: cannot load the records of "
		    "the async mirror: error %d", error);
	}
	if (am != NULL) {
		async_mirror_free_index(am);
		async_mirror_destroy(am);
	}
	async_mirror_entries_free(&ame);
	return (error);
}

/*
 * Called from spa_unload() after the sync thread has stopped: copy what is
 * left, so a clean export leaves both children at the same txg.
 */
void
vdev_async_mirror_stop(spa_t *spa)
{
	vdev_async_mirror_t *am = spa->spa_async_mirror;

	ASSERT(!spa->spa_sync_on);

	if (am == NULL)
		return;

	mutex_enter(&am->am_lock);
	am->am_stop = B_TRUE;
	cv_broadcast(&am->am_cv);
	while (am->am_thread != NULL)
		cv_wait(&am->am_drain_cv, &am->am_lock);
	mutex_exit(&am->am_lock);

	/* spa_async_suspend() cancelled the release thread before this. */
	if (am->am_release_zthr != NULL) {
		zthr_destroy(am->am_release_zthr);
		am->am_release_zthr = NULL;
	}
}

/*
 * Free the state.  spa_unload() calls this after it has waited for the
 * asynchronous I/O of the pool, because a read in flight still looks at
 * this state in vdev_mirror.
 */
void
vdev_async_mirror_fini(spa_t *spa)
{
	vdev_async_mirror_t *am = spa->spa_async_mirror;

	if (am == NULL)
		return;

	async_mirror_free_index(am);
	if (am->am_claim_rt != NULL) {
		zfs_range_tree_vacate(am->am_claim_rt, NULL, NULL);
		zfs_range_tree_destroy(am->am_claim_rt);
	}
	/* The vdevs outlive the state: take the state off the leaf. */
	mutex_enter(&am->am_lock);
	async_mirror_set_state(am, AMS_OFF);
	mutex_exit(&am->am_lock);

	spa->spa_async_mirror = NULL;
	spa->spa_async_mirror_stats.sams_active = 0;
	spa->spa_async_mirror_stats.sams_data_deferred = 0;
	spa_async_mirror_stats_destroy(spa);
	async_mirror_destroy(am);
}

ZFS_MODULE_PARAM(zfs_vdev_async_mirror, zfs_vdev_async_mirror_,
	read_retries, UINT, ZMOD_RW,
	"Times to try the copy of a txg again after a read error");

ZFS_MODULE_PARAM(zfs_vdev_async_mirror, zfs_vdev_async_mirror_,
	suspend_copy, INT, ZMOD_RW,
	"Stop the copy to the lagging child (for testing)");

ZFS_MODULE_PARAM(zfs_vdev_async_mirror, zfs_vdev_async_mirror_,
	suspend_release, INT, ZMOD_RW,
	"Stop the release of the records of the copy (for testing)");

ZFS_MODULE_PARAM(zfs_vdev_async_mirror, zfs_vdev_async_mirror_,
	max_pending_bytes, U64, ZMOD_RW,
	"Slow writes as the bytes that wait to be copied to the lagging child "
	"approach this limit, and wait above it");

ZFS_MODULE_PARAM(zfs_vdev_async_mirror, zfs_vdev_async_mirror_,
	max_pending_txgs, U64, ZMOD_RW,
	"Slow writes as the txgs that wait to be copied to the lagging child "
	"approach this limit, and wait above it");

ZFS_MODULE_PARAM(zfs_vdev_async_mirror, zfs_vdev_async_mirror_,
	copy_limit, U64, ZMOD_RW,
	"Maximum bytes of the copy to the lagging child in flight");

ZFS_MODULE_PARAM(zfs_vdev_async_mirror, zfs_vdev_async_mirror_,
	release_limit, U64, ZMOD_RW,
	"Maximum number of space map entries that one release sync task "
	"may process");
