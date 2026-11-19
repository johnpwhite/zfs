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
 * Parity set: the BP-specific lagging consumer.
 *
 * OVERVIEW
 *
 * A parity set keeps shared, lagging parity over several single-disk pools.
 * Each member pool (property parityset=on) tells an outside service, per txg,
 * which blocks it allocated and which it freed, as block pointers, so that the
 * service can read and verify the bytes and keep parity over the raw disks.
 * The service applies a txg some time after the pool wrote it.  Until then
 * the pool must keep the old bytes of every block that it freed: the parity
 * still counts them.  So a free is "held": the block stays allocated in the
 * space map, and the free is done later, when the service says that it has
 * applied the txg.  The generic parts (the record store, the release, the
 * throttle) are in spa_lag.c; this file has what depends on the block pointer.
 *
 * Every piece is inert when the pool has no parity state: the core files make
 * one NULL test (spa->spa_lag[0]).
 *
 * WHAT IS RECORDED
 *
 * One record object per txg, in the MOS, written in the txg it describes (so
 * the records are atomic with the data).  An entry is the txg, a kind and one
 * blkptr_t:
 *
 *  ALLOC  a block allocated in this txg.  Every allocation passes
 *         metaslab_alloc_range() (and metaslab_claim() for the intent log
 *         blocks that an import claims), so the set of DVAs is complete by
 *         construction.  Entries are keyed by DVA[0].
 *  FREE   a block freed in this txg.  Only the DVAs that are held are in
 *         the entry (packed to the front of the BP).  A free of an intent
 *         log block is recorded (the service must see and skip it) but is
 *         not held.
 *  FIXUP  the final form of an ALLOC entry of the previous record (below).
 *  VOID   an entry that was cancelled after it was written.
 *
 * A block that is allocated and freed in the same txg is recorded as neither:
 * the free cancels the ALLOC entry.  The decision is per DVA, by membership in
 * this txg's table, not by the birth txg of the BP: a deduplicated BP can
 * carry DVAs of several txgs, and the birth of a claimed or cloned block can be
 * newer than its allocation.
 *
 * THE CAPTURE WINDOW
 *
 * Capture is armed in syncing context.  Turning the property on at txg S
 * starts the capture at S + TXG_CONCURRENT_STATES: the open txgs S+1 and S+2
 * may already have allocated.  Each txg is captured completely or not at all:
 * a state change never moves the window over a txg that has captured entries
 * (the state only changes in syncing context, and a txg that already captured
 * keeps its end of the window).  The baseline of the outside service is the
 * allocated space of the txg before the start.
 *
 * THE FLUSH, VOID AND CONVERGENCE
 *
 * At the end of every sync pass the new entries of the txg are appended to its
 * record object (spa_lag_flush()); an entry that is cancelled after it was
 * written gets the kind VOID.  A BP can change after its entry was written
 * (the checksum of a metadata block that is rewritten in place in a later
 * sync pass).  Such an entry is marked late and is not written again in this
 * txg, or the sync loop would never converge.  The record object grows by a
 * few entries per pass, like a space map; the last pass has no allocation and
 * no free, so it writes nothing.
 *
 * LATE ENTRIES AND FIX-UPS
 *
 * When the txg has synced, the BPs of its late entries are final.  They are
 * written as FIXUP entries at the start of the next record (the header counts
 * them).  After a crash or an export the late entries of the newest record are
 * rebuilt at load by a traversal of the blocks born in that txg.  If a late
 * free removed the final BP, load checks the bytes at DVA[0] against the
 * recorded BP.  A mismatch gets a FIXUP with a zero checksum, so a consumer
 * knows that ZFS cannot verify those bytes.  If load cannot account for a
 * fix-up, the next record has SPA_LAG_REC_FIXUPS_INCOMPLETE: a rebuild against
 * a stale checksum is refused, never accepted wrongly.
 *
 * HELD FREES AND RELEASE
 *
 * A free in syncing context (metaslab_free()) asks spa_lag_hold_free() which
 * of the DVAs the consumer holds.  A held DVA is not passed to
 * metaslab_free_dva(): it stays allocated in the space map, so it never
 * reaches ms_freeing, ms_allocatable or ms_trim; TRIM, initialize, condense and
 * device expansion need no code; and the hold is crash safe (the free never
 * reached the space map).  The held bytes are taken from the available space
 * of the pool (spa_update_dspace()), so the DSL reports "no space" long before
 * the metaslabs are full.
 *
 * The outside service reports the newest txg that it has applied
 * (spa_lag_applied()).  In syncing context, in pass 1, the oldest records up
 * to that txg are released within a budget of entries per txg
 * (zfs_parity_release_limit): each held DVA of a FREE entry is freed as a
 * normal free (it is deferred for two txgs), the progress is kept in the
 * header (so a crash repeats a harmless part), and a record that is done is
 * destroyed.  The release does not wake an idle pool: a txg that is a no-op
 * stays a no-op.
 * The release is forced (it runs in a no-op txg) only under back-pressure, on
 * request, and while the pool drains or drops.
 *
 * STATES
 *
 *  OFF       nothing recorded, no directory entry on disk.
 *  ON        capture armed, frees held.
 *  DRAINING  parityset=off.  No new ALLOC entries after the stop txg.  A free
 *            of a block born at or before the stop txg is still recorded and
 *            held: the state that the service has applied must stay on disk
 *            until the service has applied the rest.  The drain ends in a txg
 *            with no record, no held free and nothing captured yet; the
 *            directory entry is removed and the feature returns to enabled.
 *  DROPPING  the records are discarded: the outside must resync this pool.
 *            Entered by an explicit drop, by the limit with parity_onstall=
 *            continue, and by turning the property on during a drain (the
 *            frees between the stop and the restart were not recorded, so the
 *            old records cannot continue).  Nothing is held any more, and the
 *            release ignores the applied txg.  When no record is left, the
 *            pool starts a new epoch if the property is on, else it is OFF;
 *            the generation counter rises by one in both cases.
 *
 * STALL POLICY
 *
 * The record space and the held space are limited per pool (spa_lag.c).  At
 * the hard limit, parity_onstall=continue (the default) drops the records and
 * the pool goes on, unprotected, and posts the event parity_unprotected;
 * parity_onstall=wait makes the writers wait.  A large destroy that frees in
 * syncing context stops when the counters pass the start of the delay curve
 * and goes on in a later txg (spa_lag_free_pause()).
 *
 * WHAT A HOST THAT DOES NOT KNOW THE FEATURE SEES
 *
 * The feature is read-only compatible.  While it is active (records exist or
 * the state is not OFF), another OpenZFS host can import the pool read-only.
 * It can import it read-write after a drain.  Each disk is an ordinary
 * single-disk pool: it needs neither the parity disks nor the service.
 *
 * INVARIANTS THAT THE VERIFIERS CHECK (zfs_parity_verify, one bit each)
 *
 *  bit 0  The DVA ranges of the ALLOC entries of a txg, with the ranges that
 *         were cancelled, are exactly the ranges the metaslabs allocated in
 *         this txg, and they do not overlap.
 *  bit 1  The record on disk equals the capture in memory (header, every live
 *         entry, the VOIDs, the fix-ups, and the sums in the header).
 *  bit 2  Every recorded ALLOC BP equals the BP in the block tree (after the
 *         fix-ups), and every block born in the txg was recorded.
 *         Bits 0 and 2 check only the txgs of the ALLOC window.
 *  bit 3  (release)  Every held DVA range is freed exactly once, and no held
 *         range leaks.  The verifier keeps a shadow set of the held ranges: a
 *         range enters it when a free is held and leaves it when the release
 *         frees it.  At the release the range must be in the set (so it is not
 *         freed twice, and it was held), it must be in none of ms_freeing,
 *         ms_freed, ms_defer and ms_allocatable (nobody freed it behind our
 *         back), and after the free it must be in ms_freeing (the free was
 *         done).  No range that a metaslab allocates may overlap the set (a
 *         held range cannot be reused).  At the end of each txg the size of
 *         the set equals the held-bytes counter and the sum of the record
 *         headers; and when no record is left, the set is empty.
 *
 * Fault injection (zfs_parity_test_inject) lets a test prove that each
 * verifier fires.
 */

#include <sys/spa_impl.h>
#include <sys/spa_lag.h>
#include <sys/spa_parity.h>
#include <sys/spa_parity_impl.h>
#include <sys/sysevent/eventdefs.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_rebuild.h>
#include <sys/metaslab.h>
#include <sys/dmu_tx.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_scan.h>
#include <sys/zfeature.h>
#include <sys/zap.h>
#include <sys/zio.h>
#include <sys/abd.h>
#include <sys/dmu_traverse.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_prop.h>
#include <sys/dmu_objset.h>
#include <sys/bpobj.h>
#include <sys/bptree.h>
#include <zfs_prop.h>

/* Bit mask of debug checks; ztest turns them on.  See spa_parity_impl.h. */
uint_t zfs_parity_verify = 0;
/* Allow online expansion while a parity set holds records. */
int zfs_parity_allow_expand = 0;
/* Release at most this many entries per txg. */
uint64_t zfs_parity_release_limit = 65536;
/*
 * Test only: treat every txg older than this many txgs as applied.  This
 * stands in for the parity service until it can call in.  0 = off.
 */
uint64_t zfs_parity_test_auto_apply = 0;
/*
 * Test only: inject a fault, so a test can prove that a verifier fires.
 * 1: now and then, do not capture an allocation (bit 0 must fire);
 * 2: now and then, do not write a VOID (bit 1 must fire);
 * 4: now and then, do not refresh a BP at write_ready (bit 2 must fire);
 * 8: now and then, do not free a held range at its release (bit 3 must fire).
 * 16: once, fail the parity load of a set member with EIO (the bit clears
 *     itself), so a rewind import (zpool import -F) retries after the pool
 *     properties loaded and before the records did.
 * 32: fail each read of a record during its release with EIO, so a drop
 *     must end without the entries.
 */
uint_t zfs_parity_test_inject = 0;
/* Test only: do not release records while this is set. */
int zfs_parity_suspend_release = 0;

/*
 * Import override, set for one import by an explicit act of the
 * operator: records of a parity set member that cannot be read do not stop
 * the read-write import.  The pool then runs with no records, and the set must
 * resync it.  0: such a pool does not import read-write.
 */
uint_t zfs_parity_import_override = 0;

/*
 * Gating.
 */

/*
 * Is this pool a set member on disk?  True when the parityset property is on
 * or the parity directory exists, also before spa_ld_parity_load() has made
 * spa_parity.  The MOS must be open; without it, false.
 */
boolean_t
spa_parity_member_on_disk(spa_t *spa)
{
	objset_t *mos = spa->spa_meta_objset;
	uint64_t on = 0;

	if (mos == NULL)
		return (B_FALSE);
	if (spa->spa_pool_props_object != 0)
		(void) zap_lookup(mos, spa->spa_pool_props_object,
		    zpool_prop_to_name(ZPOOL_PROP_PARITYSET),
		    sizeof (uint64_t), 1, &on);
	return (on != 0 || zap_contains(mos, DMU_POOL_DIRECTORY_OBJECT,
	    PARITY_DIR_NAME) == 0);
}

boolean_t
spa_parity_busy(spa_t *spa)
{
	spa_parity_t *ps = spa->spa_parity;

	if (ps == NULL)
		return (B_FALSE);
	return (ps->ps_state != PARITY_OFF || spa_lag_nrecs(ps->ps_lag) != 0);
}

/*
 * Growth of a member while it holds records.  The set must know the new
 * size first: parity covers each member up to a size, so the set grants
 * growth with the pool property parity_grow_max (bytes of top-level vdev
 * asize).  Without a grant a busy member does not grow.
 */
boolean_t
spa_parity_expand_blocked(spa_t *spa)
{
	return (spa_parity_busy(spa) && !zfs_parity_allow_expand &&
	    spa->spa_parity_grow_max == 0);
}

/* The largest asize a top-level vdev of this pool may grow to now. */
uint64_t
spa_parity_grow_cap(spa_t *spa)
{
	if (!spa_parity_busy(spa) || zfs_parity_allow_expand)
		return (UINT64_MAX);
	return (spa->spa_parity_grow_max);
}

/*
 * The largest asize a new data vdev of this pool may have now: the
 * largest data vdev it has (parity covers each of them), or the growth the
 * set granted (parity_grow_max), whichever is larger.  The kernel does not
 * know the size of the parity disks; a larger vdev could never join the set,
 * and a busy member cannot remove it again.
 */
uint64_t
spa_parity_add_cap(spa_t *spa)
{
	vdev_t *rvd = spa->spa_root_vdev;
	uint64_t cap = spa_parity_grow_cap(spa);

	if (cap == UINT64_MAX)
		return (cap);
	for (uint64_t c = 0; rvd != NULL && c < rvd->vdev_children; c++)
		if (spa_parity_is_data_vdev(spa, c))
			cap = MAX(cap, rvd->vdev_child[c]->vdev_asize);
	return (cap);
}

/*
 * A single disk: a leaf, or the replacing or spare vdev that a replace of a
 * leaf makes (two leaves).
 */
static boolean_t
parity_single_disk(vdev_t *vd)
{
	if (vd->vdev_ops->vdev_op_leaf)
		return (B_TRUE);
	if (vd->vdev_ops != &vdev_replacing_ops &&
	    vd->vdev_ops != &vdev_spare_ops)
		return (B_FALSE);
	return (vd->vdev_children == 2 &&
	    vd->vdev_child[0]->vdev_ops->vdev_op_leaf &&
	    vd->vdev_child[1]->vdev_ops->vdev_op_leaf);
}

/*
 * a data column is a single disk, or a mirror of single disks (each side
 * may be under a replace).  NULL, or why the top-level vdev is not one.
 */
static const char *
parity_column_why(vdev_t *tvd)
{
	if (parity_single_disk(tvd))
		return (NULL);
	if (tvd->vdev_ops == &vdev_replacing_ops ||
	    tvd->vdev_ops == &vdev_spare_ops)
		return ("a replace that is not between two single disks");
	if (tvd->vdev_ops != &vdev_mirror_ops)
		return ("a data vdev with its own parity (raidz, draid or "
		    "anyraid)");
	if (tvd->vdev_children > SPA_PARITY_MAX_SIDES)
		return ("a mirror with more sides than a parity set takes");
	for (uint64_t c = 0; c < tvd->vdev_children; c++) {
		if (!parity_single_disk(tvd->vdev_child[c]))
			return ("a mirror side that is not a single disk");
	}
	return (NULL);
}

/* a new top-level vdev that can join a member as a data column. */
boolean_t
spa_parity_column_ok(vdev_t *tvd)
{
	return (tvd->vdev_alloc_bias == VDEV_BIAS_NONE &&
	    parity_column_why(tvd) == NULL);
}

/* the data columns of a pool now.  The caller holds a config lock. */
uint64_t
spa_parity_ncols(spa_t *spa)
{
	vdev_t *rvd = spa->spa_root_vdev;
	uint64_t n = 0;

	for (uint64_t c = 0; rvd != NULL && c < rvd->vdev_children; c++)
		if (spa_parity_is_data_vdev(spa, c))
			n++;
	return (n);
}

/*
 * parityset=on needs one or more normal-class top-level vdevs, each a data
 * column: a single disk (a disk, a file, or the replacing or spare vdev
 * that a replace makes of it) or a mirror of single disks.  Log devices are
 * allowed (and cache devices are not top-level vdevs).  Holes (left by a
 * removed log) hold no data.  Special and dedup classes, raidz, draid and
 * removed (indirect) vdevs are refused.  The caller holds the config lock, or
 * is creating the pool.
 */
int
spa_parity_eligible_root(vdev_t *rvd)
{
	int normal = 0;
	/*
	 * The state is read without a lock as uint64_t (a pointer-sized load
	 * is atomic only on a 64-bit host), so a 32-bit host is refused.
	 */
	const char *why = (sizeof (void *) < 8) ? "a 32-bit host" : NULL;

	for (uint64_t c = 0; c < rvd->vdev_children && why == NULL; c++) {
		vdev_t *tvd = rvd->vdev_child[c];

		if (tvd->vdev_ishole)
			continue;
		if (tvd->vdev_ops == &vdev_indirect_ops) {
			why = "a removed (indirect) vdev";
		} else if (tvd->vdev_alloc_bias == VDEV_BIAS_SPECIAL ||
		    tvd->vdev_alloc_bias == VDEV_BIAS_DEDUP) {
			why = "a special or dedup vdev";
		} else if (tvd->vdev_islog) {
			continue;
		} else if ((why = parity_column_why(tvd)) == NULL) {
			normal++;
		}
	}
	if (why == NULL && normal == 0)
		why = "no data vdev";
	if (why == NULL && normal > SPA_PARITY_MAX_COLS)
		why = "more data vdevs than a parity set takes";
	if (why != NULL) {
		/* The caller returns only ENOTSUP: say why in the debug log. */
		zfs_dbgmsg("parity: a pool set member needs data vdevs that "
		    "are single disks or mirrors, this pool has %s", why);
		return (SET_ERROR(ENOTSUP));
	}
	return (0);
}

/*
 * Is a healing resilver running, scheduled, or still needed (a leaf with a
 * DTL that no sequential rebuild covers)?  A replace or spare attach that
 * starts while parity is off and has no records is a healing resilver
 * (spa_vdev_attach() forces a sequential rebuild only while the pool is
 * busy).  It copies only the live blocks: a block freed and held after
 * parity starts, before the resilver reaches it, is never copied to the new
 * disk, and the held bytes are lost when the old disk detaches.  So parity
 * does not start until it is done.  The caller holds SCL_CONFIG.
 */
static boolean_t
parity_resilver_pending(spa_t *spa)
{
	dsl_pool_t *dp = spa->spa_dsl_pool;
	vdev_t *rvd = spa->spa_root_vdev;

	if (dp != NULL && dp->dp_scan != NULL &&
	    (dsl_scan_resilvering(dp) || dsl_scan_resilver_scheduled(dp)))
		return (B_TRUE);
	return (!vdev_rebuild_active(rvd) &&
	    vdev_resilver_needed(rvd, NULL, NULL));
}

int
spa_parity_eligible(spa_t *spa)
{
	int error;

	spa_config_enter(spa, SCL_CONFIG, FTAG, RW_READER);
	error = spa_parity_eligible_root(spa->spa_root_vdev);
	if (error == 0 && parity_resilver_pending(spa))
		error = SET_ERROR(ZFS_ERR_RESILVER_IN_PROGRESS);
	spa_config_exit(spa, SCL_CONFIG, FTAG);
	return (error);
}

/*
 * checksum=off and checksum=noparity accept any bytes, so a member pool could
 * neither verify nor repair a block that is written with them.  (A verify
 * flag in the value does not change the function.)
 */
boolean_t
spa_parity_checksum_refused(uint64_t cksum)
{
	return ((cksum & ZIO_CHECKSUM_MASK) == ZIO_CHECKSUM_OFF ||
	    (cksum & ZIO_CHECKSUM_MASK) == ZIO_CHECKSUM_NOPARITY);
}

static int
spa_parity_checksum_cb(const char *dsname, void *arg)
{
	uint64_t cksum;
	int error;

	(void) arg;
	error = dsl_prop_get_integer(dsname,
	    zfs_prop_to_name(ZFS_PROP_CHECKSUM), &cksum, NULL);
	if (error != 0)
		return (error);
	if (spa_parity_checksum_refused(cksum)) {
		zfs_dbgmsg("parity: dataset %s has checksum=off or noparity",
		    dsname);
		return (SET_ERROR(ZFS_ERR_PARITY_CHECKSUM));
	}
	return (0);
}

/*
 * parityset=on is refused when any dataset or volume of the pool has an
 * effective checksum of off or noparity (set there or inherited).  This runs
 * in open context, from the property check, and reads the effective value of
 * each dataset.  While the pool is a member, zfs_check_settable() and the
 * inherit and receive paths refuse new off and noparity values, so the walk
 * is needed only when the property is turned on.  Returns
 * ZFS_ERR_PARITY_CHECKSUM, which libzfs reports with the reason.
 */
int
spa_parity_check_checksums(spa_t *spa)
{
	return (dmu_objset_find(spa_name(spa), spa_parity_checksum_cb, NULL,
	    DS_FIND_CHILDREN));
}

/*
 * The pending entry of each recorded allocation or free comes from its own
 * cache: a recorded allocation is on the write path, and a cache avoids the
 * general allocator and its fragmentation.  Created by spa_init(), destroyed
 * by spa_fini().
 */
static kmem_cache_t *parity_pend_cache;

void
spa_parity_init(void)
{
	parity_pend_cache = kmem_cache_create("parity_pend_cache",
	    sizeof (parity_pend_t), 0, NULL, NULL, NULL, NULL, NULL, 0);
}

void
spa_parity_fini(void)
{
	kmem_cache_destroy(parity_pend_cache);
	parity_pend_cache = NULL;
}

static parity_pend_t *
parity_pend_alloc(void)
{
	parity_pend_t *pp = kmem_cache_alloc(parity_pend_cache, KM_SLEEP);

	memset(pp, 0, sizeof (*pp));
	return (pp);
}

static void
parity_pend_free(parity_pend_t *pp)
{
	kmem_cache_free(parity_pend_cache, pp);
}

/*
 * Capture tables.
 */

static int
parity_pend_compare(const void *a, const void *b)
{
	const parity_pend_t *pa = a, *pb = b;
	int c = TREE_CMP(pa->pp_vdev, pb->pp_vdev);

	if (c != 0)
		return (c);
	return (TREE_CMP(pa->pp_offset, pb->pp_offset));
}

static int
parity_slot_compare(const void *a, const void *b)
{
	const parity_pend_t *pa = a, *pb = b;

	return (TREE_CMP(pa->pp_slot, pb->pp_slot));
}

static int
parity_vrt_compare(const void *a, const void *b)
{
	const parity_vrt_t *pa = a, *pb = b;

	return (TREE_CMP(pa->pv_vdev, pb->pv_vdev));
}

static void
parity_cancel_add(parity_txg_t *pt, uint64_t vdev, uint64_t offset,
    uint64_t size)
{
	parity_cancel_t *pc = kmem_alloc(sizeof (*pc), KM_SLEEP);

	pc->pc_vdev = vdev;
	pc->pc_offset = offset;
	pc->pc_size = size;
	list_insert_tail(&pt->pt_cancel, pc);
}

/* Size of the allocation of one DVA.  A gang DVA is the gang header. */
static uint64_t
parity_dva_size(spa_t *spa, const dva_t *dva)
{
	if (DVA_GET_GANG(dva)) {
		vdev_t *vd = vdev_lookup_top(spa, DVA_GET_VDEV(dva));

		ASSERT3P(vd, !=, NULL);
		return (vdev_gang_header_asize(vd));
	}
	return (DVA_GET_ASIZE(dva));
}

static uint64_t
parity_bp_asize(spa_t *spa, const blkptr_t *bp)
{
	uint64_t asize = 0;

	for (int d = 0; d < BP_GET_NDVAS(bp); d++)
		asize += parity_dva_size(spa, &bp->blk_dva[d]);
	return (asize);
}

/*
 * The same allocation: the same DVAs by vdev, offset and gang bit.  The
 * asize of a gang DVA changes after the allocation (it becomes the size of
 * the whole gang tree), so it is not compared.
 */
static boolean_t
parity_same_dvas(const blkptr_t *a, const blkptr_t *b)
{
	if (BP_GET_NDVAS(a) != BP_GET_NDVAS(b))
		return (B_FALSE);
	for (int d = 0; d < BP_GET_NDVAS(a); d++) {
		const dva_t *x = &a->blk_dva[d], *y = &b->blk_dva[d];

		if (DVA_GET_VDEV(x) != DVA_GET_VDEV(y) ||
		    DVA_GET_OFFSET(x) != DVA_GET_OFFSET(y) ||
		    DVA_GET_GANG(x) != DVA_GET_GANG(y))
			return (B_FALSE);
	}
	return (B_TRUE);
}

static parity_vrt_t *
parity_vrt_get(avl_tree_t *t, uint64_t vdev)
{
	parity_vrt_t search, *pv;
	avl_index_t where;

	search.pv_vdev = vdev;
	pv = avl_find(t, &search, &where);
	if (pv == NULL) {
		pv = kmem_zalloc(sizeof (*pv), KM_SLEEP);
		pv->pv_vdev = vdev;
		pv->pv_rt = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64, NULL,
		    0, 0);
		avl_insert(t, pv, where);
	}
	return (pv);
}

static parity_vrt_t *
parity_vrt_find(avl_tree_t *t, uint64_t vdev)
{
	parity_vrt_t search;

	search.pv_vdev = vdev;
	return (avl_find(t, &search, NULL));
}

static void
parity_vrt_destroy(avl_tree_t *t)
{
	parity_vrt_t *pv;
	void *cookie = NULL;

	while ((pv = avl_destroy_nodes(t, &cookie)) != NULL) {
		zfs_range_tree_vacate(pv->pv_rt, NULL, NULL);
		zfs_range_tree_destroy(pv->pv_rt);
		kmem_free(pv, sizeof (*pv));
	}
	avl_destroy(t);
}

static void
parity_txg_init(parity_txg_t *pt)
{
	mutex_init(&pt->pt_lock, NULL, MUTEX_DEFAULT, NULL);
	avl_create(&pt->pt_tree, parity_pend_compare,
	    sizeof (parity_pend_t), offsetof(parity_pend_t, pp_node));
	avl_create(&pt->pt_frees, parity_slot_compare,
	    sizeof (parity_pend_t), offsetof(parity_pend_t, pp_snode));
	list_create(&pt->pt_unflushed, sizeof (parity_pend_t),
	    offsetof(parity_pend_t, pp_lnode));
	list_create(&pt->pt_voids, sizeof (parity_pend_t),
	    offsetof(parity_pend_t, pp_lnode));
	list_create(&pt->pt_cancel, sizeof (parity_cancel_t),
	    offsetof(parity_cancel_t, pc_node));
	pt->pt_verify = zfs_parity_verify;
}

/*
 * Drop everything of one table, and take the verify bits for the txg that
 * will use this table next.  The caller has excluded other users.
 */
static void
parity_txg_clear(parity_txg_t *pt)
{
	parity_pend_t *pp;
	parity_cancel_t *pc;
	void *cookie = NULL;

	mutex_enter(&pt->pt_lock);
	/*
	 * An ALLOC entry in the tree may also be on pt_unflushed; a FREE entry
	 * is on pt_unflushed or, once written, in pt_frees, never in the tree.
	 */
	while ((pp = list_remove_head(&pt->pt_unflushed)) != NULL) {
		if (pp->pp_kind == PARITY_KIND_FREE)
			parity_pend_free(pp);
	}
	while ((pp = avl_destroy_nodes(&pt->pt_frees, &cookie)) != NULL)
		parity_pend_free(pp);
	avl_create(&pt->pt_frees, parity_slot_compare,
	    sizeof (parity_pend_t), offsetof(parity_pend_t, pp_snode));
	cookie = NULL;
	while ((pp = avl_destroy_nodes(&pt->pt_tree, &cookie)) != NULL)
		parity_pend_free(pp);
	avl_create(&pt->pt_tree, parity_pend_compare,
	    sizeof (parity_pend_t), offsetof(parity_pend_t, pp_node));
	while ((pp = list_remove_head(&pt->pt_voids)) != NULL)
		parity_pend_free(pp);
	while ((pc = list_remove_head(&pt->pt_cancel)) != NULL)
		kmem_free(pc, sizeof (*pc));
	pt->pt_nunflushed = 0;
	pt->pt_nvoids = 0;
	pt->pt_verify = zfs_parity_verify;
	mutex_exit(&pt->pt_lock);
}

static void
parity_txg_fini(parity_txg_t *pt)
{
	parity_txg_clear(pt);
	avl_destroy(&pt->pt_tree);
	avl_destroy(&pt->pt_frees);
	list_destroy(&pt->pt_unflushed);
	list_destroy(&pt->pt_voids);
	list_destroy(&pt->pt_cancel);
	mutex_destroy(&pt->pt_lock);
}

static void
parity_list_free(list_t *l, uint64_t *count)
{
	parity_pend_t *pp;

	while ((pp = list_remove_head(l)) != NULL)
		parity_pend_free(pp);
	*count = 0;
}

/*
 * The blocks of one txg, as they are on disk: a map from each DVA to the BP
 * that holds it.  Used at load, to rebuild the fix-ups of the newest record,
 * and by verifier bit 2.  A recorded entry is compared only when it is an
 * allocation of that txg and not a log block (log blocks are not in the
 * tree; a claimed block has an older birth).
 */
static boolean_t
parity_bp_mapped(const blkptr_t *bp)
{
	return (bp != NULL && !BP_IS_HOLE(bp) && !BP_IS_EMBEDDED(bp) &&
	    !BP_IS_REDACTED(bp) && BP_GET_TYPE(bp) != DMU_OT_INTENT_LOG);
}

static boolean_t
parity_born_wanted(const blkptr_t *bp, uint64_t txg)
{
	return (parity_bp_mapped(bp) && BP_GET_PHYSICAL_BIRTH(bp) == txg);
}

/*
 * The kind of a map node (pp_kind), best first:
 *  BORN:  allocated in txg (physical birth), the exact BP from the tree or a
 *         gang header;
 *  FREED: allocated in txg and freed in a late pass of it: from the
 *         deferred-free list, which keeps the DVAs but clears the checksum
 *         (not for dedup), the fill and one property bit;
 *  REF:   a dedup BP written in txg (logical birth) for an older DDT entry; a
 *         copy added to the entry in txg has an ALLOC entry of txg.
 * pp_slot is the index of the DVA the node is keyed by.
 */
#define	PARITY_MAP_BORN	1
#define	PARITY_MAP_FREED	2
#define	PARITY_MAP_REF		3

static void
parity_map_add(avl_tree_t *map, const blkptr_t *bp, uint8_t kind)
{
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		parity_pend_t *pp = parity_pend_alloc();
		parity_pend_t *old;
		avl_index_t where;

		pp->pp_vdev = DVA_GET_VDEV(&bp->blk_dva[d]);
		pp->pp_offset = DVA_GET_OFFSET(&bp->blk_dva[d]);
		pp->pp_bp = *bp;
		pp->pp_slot = d;
		pp->pp_kind = kind;
		/* A block can be reached twice (dedup, dmu_sync()). */
		old = avl_find(map, pp, &where);
		if (old == NULL) {
			avl_insert(map, pp, where);
			continue;
		}
		if (kind < old->pp_kind) {
			old->pp_bp = *bp;
			old->pp_slot = d;
			old->pp_kind = kind;
		}
		parity_pend_free(pp);
	}
}

static void
parity_map_destroy(avl_tree_t *map)
{
	parity_pend_t *pp;
	void *cookie = NULL;

	while ((pp = avl_destroy_nodes(map, &cookie)) != NULL)
		parity_pend_free(pp);
	avl_destroy(map);
}

/*
 * The children of a gang block are not in the tree: read the gang header
 * (as zio_gang_tree_assemble() does) and add the children born in txg.
 *
 * Only ztest (verifier bit 2) and the load run this.  A read with no parent
 * is fine for the checksum path; but if the pool suspended (ENXIO with
 * failmode=wait) outside a load, zio_reexecute() would not expect it.
 */
static int
parity_map_gang(spa_t *spa, avl_tree_t *map, const blkptr_t *bp,
    uint64_t txg, int depth)
{
	uint64_t size = SPA_OLD_GANGBLOCKSIZE, alloc;
	zio_gang_node_t *gn;
	abd_t *abd;
	int error;

	if (depth > 8)
		return (SET_ERROR(ELOOP));
	if (spa_dynamic_gang_header(spa)) {
		size = UINT64_MAX;
		spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
		for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
			vdev_t *vd = vdev_lookup_top(spa,
			    DVA_GET_VDEV(&bp->blk_dva[d]));
			size = MIN(size, vdev_gang_header_psize(vd));
		}
		spa_config_exit(spa, SCL_VDEV, FTAG);
	}
	alloc = size;
	/* zio_checksum_error() sets gn_gangblocksize for an old header. */
	gn = kmem_zalloc(sizeof (*gn), KM_SLEEP);
	gn->gn_gbh = zio_buf_alloc(alloc);
	gn->gn_gangblocksize = size;
	abd = abd_get_from_buf(gn->gn_gbh, alloc);
	error = zio_wait(zio_read(NULL, spa, bp, abd, alloc, NULL, gn,
	    ZIO_PRIORITY_SYNC_READ, ZIO_FLAG_CANFAIL | ZIO_FLAG_GANG_CHILD,
	    NULL));
	abd_free(abd);
	if (error == 0) {
		size = gn->gn_gangblocksize;
		if (BP_SHOULD_BYTESWAP(bp))
			byteswap_uint64_array(gn->gn_gbh, size);
		for (int g = 0; g < gbh_nblkptrs(size) && error == 0; g++) {
			const blkptr_t *gbp = gbh_bp(gn->gn_gbh, g);

			if (!parity_born_wanted(gbp, txg))
				continue;
			parity_map_add(map, gbp, PARITY_MAP_BORN);
			if (BP_IS_GANG(gbp)) {
				error = parity_map_gang(spa, map, gbp, txg,
				    depth + 1);
			}
		}
	}
	zio_buf_free(gn->gn_gbh, alloc);
	kmem_free(gn, sizeof (*gn));
	return (error);
}

typedef struct parity_born_arg {
	spa_t		*pb_spa;
	avl_tree_t	*pb_map;
	uint64_t	pb_txg;
	boolean_t	pb_freed;	/* the BPs come from a free list */
} parity_born_arg_t;

static int
parity_born_add(parity_born_arg_t *pb, const blkptr_t *bp)
{
	if (!parity_bp_mapped(bp))
		return (0);
	if (BP_GET_PHYSICAL_BIRTH(bp) != pb->pb_txg) {
		if (!pb->pb_freed && BP_GET_DEDUP(bp) &&
		    BP_GET_LOGICAL_BIRTH(bp) == pb->pb_txg)
			parity_map_add(pb->pb_map, bp, PARITY_MAP_REF);
		return (0);
	}
	parity_map_add(pb->pb_map, bp,
	    pb->pb_freed ? PARITY_MAP_FREED : PARITY_MAP_BORN);
	if (BP_IS_GANG(bp))
		return (parity_map_gang(pb->pb_spa, pb->pb_map, bp,
		    pb->pb_txg, 0));
	return (0);
}

static int
parity_born_cb(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const dnode_phys_t *dnp, void *arg)
{
	(void) spa, (void) zilog, (void) zb, (void) dnp;
	return (parity_born_add(arg, bp));
}

/*
 * Blocks freed in a late sync pass go to the deferred-free list and are
 * freed in the next txg, so a block born in this txg can be there and no
 * longer in the tree.  Its record entry is right: it was allocated in this
 * txg, and the free belongs to the next one.
 */
static int
parity_born_deferred_cb(void *arg, const blkptr_t *bp, boolean_t bp_freed,
    dmu_tx_t *tx)
{
	(void) bp_freed, (void) tx;
	return (parity_born_add(arg, bp));
}

/*
 * A dataset destroyed (or rolled back) in this txg: its tree moved to the
 * async-destroy bptree, and its blocks are freed over the next txgs.  They
 * keep their final BPs there.
 */
static int
parity_born_bptree_cb(void *arg, const blkptr_t *bp, dmu_tx_t *tx)
{
	(void) tx;
	return (parity_born_add(arg, bp));
}

/*
 * traverse_pool() from the newest root: at the end of spa_sync() the root of
 * the txg that just converged is in spa_uberblock, while traverse_pool() uses
 * spa_ubsync, which spa_sync() updates last.  At load the two are the same.
 * The MOS goes through traverse_dataset_destroyed(), which takes the root BP
 * (the objset id it sets is used only in bookmarks).
 */
static int
parity_traverse(spa_t *spa, uint64_t txg_start, int flags,
    blkptr_cb_t func, void *arg)
{
	dsl_pool_t *dp = spa_get_dsl(spa);
	objset_t *mos = dp->dp_meta_objset;
	int err;

	err = traverse_dataset_destroyed(spa, &spa->spa_uberblock.ub_rootbp,
	    txg_start, NULL, flags, func, arg);
	if (err != 0)
		return (err);

	for (uint64_t obj = 1; err == 0;
	    err = dmu_object_next(mos, &obj, B_FALSE, txg_start)) {
		dmu_object_info_t doi;
		dsl_dataset_t *ds;
		uint64_t txg = txg_start;

		err = dmu_object_info(mos, obj, &doi);
		if (err != 0)
			break;
		if (doi.doi_bonus_type != DMU_OT_DSL_DATASET)
			continue;
		dsl_pool_config_enter(dp, FTAG);
		err = dsl_dataset_hold_obj(dp, obj, FTAG, &ds);
		dsl_pool_config_exit(dp, FTAG);
		if (err != 0)
			break;
		if (dsl_dataset_phys(ds)->ds_prev_snap_txg > txg)
			txg = dsl_dataset_phys(ds)->ds_prev_snap_txg;
		err = traverse_dataset(ds, txg, flags, func, arg);
		dsl_dataset_rele(ds, FTAG);
	}
	return (err == ESRCH ? 0 : err);
}

/*
 * Build the map for txg: the tree, the deferred-free list and the
 * async-destroy bptree.  The pool must not change on disk meanwhile: the
 * caller is the sync thread after the txg converged, or the load.  Clone
 * livelists waiting for deletion are not walked: see parity_map_partial().
 */
static int
parity_born_map(spa_t *spa, uint64_t txg, avl_tree_t *map)
{
	dsl_pool_t *dp = spa_get_dsl(spa);
	parity_born_arg_t pb = {
	    .pb_spa = spa, .pb_map = map, .pb_txg = txg, .pb_freed = B_FALSE };
	int error;

	avl_create(map, parity_pend_compare, sizeof (parity_pend_t),
	    offsetof(parity_pend_t, pp_node));
	error = parity_traverse(spa, txg - 1,
	    TRAVERSE_PRE | TRAVERSE_NO_DECRYPT, parity_born_cb, &pb);
	if (error == 0 && dp->dp_bptree_obj != 0) {
		error = bptree_iterate(dp->dp_meta_objset, dp->dp_bptree_obj,
		    B_FALSE, parity_born_bptree_cb, &pb, NULL);
	}
	if (error == 0) {
		pb.pb_freed = B_TRUE;
		error = bpobj_iterate_nofree(&spa->spa_deferred_bpobj,
		    parity_born_deferred_cb, &pb, NULL);
	}
	if (error != 0)
		parity_map_destroy(map);
	return (error);
}

/*
 * The map may miss blocks of the txg: a destroyed clone's blocks wait on a
 * livelist, which is not walked.  Then a recorded entry that is not in the
 * map is only counted.
 */
static boolean_t
parity_map_partial(spa_t *spa)
{
	uint64_t n = 0;

	if (spa->spa_livelists_to_delete == 0)
		return (B_FALSE);
	(void) zap_count(spa->spa_meta_objset, spa->spa_livelists_to_delete,
	    &n);
	return (n != 0);
}

/*
 * A recorded allocation is the block in the map: the same BP; for a FREED
 * node, the same DVAs; for a dedup BP, the recorded DVAs are some of its DVAs
 * with the same checksum (a copy added to the DDT entry later).
 */
static boolean_t
parity_map_match(const parity_pend_t *f, const blkptr_t *bp)
{
	if (f->pp_kind == PARITY_MAP_BORN &&
	    memcmp(&f->pp_bp, bp, sizeof (*bp)) == 0)
		return (B_TRUE);
	if (f->pp_kind == PARITY_MAP_FREED)
		return (parity_same_dvas(&f->pp_bp, bp));
	if (!BP_GET_DEDUP(&f->pp_bp) ||
	    !ZIO_CHECKSUM_EQUAL(f->pp_bp.blk_cksum, bp->blk_cksum))
		return (B_FALSE);
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		boolean_t in = B_FALSE;

		for (int e = 0; e < BP_GET_NDVAS(&f->pp_bp); e++) {
			if (DVA_GET_VDEV(&bp->blk_dva[d]) ==
			    DVA_GET_VDEV(&f->pp_bp.blk_dva[e]) &&
			    DVA_GET_OFFSET(&bp->blk_dva[d]) ==
			    DVA_GET_OFFSET(&f->pp_bp.blk_dva[e]))
				in = B_TRUE;
		}
		if (!in)
			return (B_FALSE);
	}
	return (B_TRUE);
}

/*
 * Windows.  Read without a lock: the state changes only in syncing context,
 * and the syncing context checks the window again before it writes anything.
 *
 * The record window: txgs whose records are written (allocations, cancels,
 * and later held frees).  The ALLOC window is the record window, minus the
 * txgs after the stop txg while draining.
 */
static boolean_t
parity_rec_window(spa_parity_t *ps, uint64_t txg)
{
	parity_state_t state = ps->ps_state;

	membar_consumer();
	return (state != PARITY_OFF && txg >= ps->ps_capture_from_txg);
}

static boolean_t
parity_alloc_window(spa_parity_t *ps, uint64_t txg)
{
	parity_state_t state = ps->ps_state;

	membar_consumer();
	if (txg < ps->ps_capture_from_txg)
		return (B_FALSE);
	switch (state) {
	case PARITY_ON:
		return (B_TRUE);
	case PARITY_DRAINING:
	case PARITY_DROPPING:
		return (txg <= ps->ps_stop_txg);
	default:
		return (B_FALSE);
	}
}

/*
 * The hooks.
 */

static void
parity_note_alloc(void *arg, const blkptr_t *bp, uint64_t txg)
{
	spa_parity_t *ps = arg;
	parity_txg_t *pt = &ps->ps_txg[txg & TXG_MASK];
	parity_pend_t *pp;
	avl_index_t where;

	if (!parity_alloc_window(ps, txg) ||
	    txg > spa_freeze_txg(ps->ps_spa))
		return;

	ASSERT(!BP_IS_HOLE(bp));
	ASSERT(!BP_IS_EMBEDDED(bp));

	if ((zfs_parity_test_inject & 1) && random_in_range(1000) == 0)
		return;

	pp = parity_pend_alloc();
	pp->pp_vdev = DVA_GET_VDEV(&bp->blk_dva[0]);
	pp->pp_offset = DVA_GET_OFFSET(&bp->blk_dva[0]);
	pp->pp_bp = *bp;
	pp->pp_kind = PARITY_KIND_ALLOC;
	pp->pp_slot = UINT64_MAX;

	mutex_enter(&pt->pt_lock);
	/*
	 * An allocation is entered once: a free in the same txg removes the
	 * entry (cancel), so a second entry for the same DVA is a bug.
	 */
	if (avl_find(&pt->pt_tree, pp, &where) != NULL) {
		mutex_exit(&pt->pt_lock);
		zfs_panic_recover("parity: DVA %llu:%llx allocated twice in "
		    "txg %llu", (u_longlong_t)pp->pp_vdev,
		    (u_longlong_t)pp->pp_offset, (u_longlong_t)txg);
		parity_pend_free(pp);
		return;
	}
	avl_insert(&pt->pt_tree, pp, where);
	list_insert_tail(&pt->pt_unflushed, pp);
	pt->pt_nunflushed++;
	mutex_exit(&pt->pt_lock);
}

static parity_pend_t *
parity_lookup_dva(parity_txg_t *pt, const dva_t *dva)
{
	parity_pend_t search;

	search.pp_vdev = DVA_GET_VDEV(dva);
	search.pp_offset = DVA_GET_OFFSET(dva);
	return (avl_find(&pt->pt_tree, &search, NULL));
}

/*
 * Remove an entry: it is not written, or, if it was written already, its
 * slot becomes VOID at the next flush.  Under pt_lock.
 */
static void
parity_cancel(parity_txg_t *pt, parity_pend_t *pp)
{
	ASSERT(MUTEX_HELD(&pt->pt_lock));
	avl_remove(&pt->pt_tree, pp);
	if (pp->pp_slot == UINT64_MAX) {
		list_remove(&pt->pt_unflushed, pp);
		pt->pt_nunflushed--;
		parity_pend_free(pp);
	} else {
		pp->pp_kind = PARITY_KIND_VOID;
		list_insert_tail(&pt->pt_voids, pp);
		pt->pt_nvoids++;
	}
}

/*
 * The entries a free (or an undone allocation) of bp cancels: one per DVA.
 * A DVA of a dedup BP may belong to another entry than DVA[0] (a later write
 * with more copies adds a DVA to the DDT entry), so each DVA is looked up.
 *
 * Returns the mask of the DVAs of bp that were born in this txg: the DVAs of
 * the cancelled entries (an entry is freed whole).  The caller holds the
 * others.
 */
static uint_t
parity_cancel_bp(spa_parity_t *ps, parity_txg_t *pt, const blkptr_t *bp,
    boolean_t verify_ranges)
{
	uint_t covered = 0;

	ASSERT(MUTEX_HELD(&pt->pt_lock));
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		parity_pend_t *pp;

		if (covered & (1U << d))
			continue;
		pp = parity_lookup_dva(pt, &bp->blk_dva[d]);
		if (pp == NULL)
			continue;
		covered |= 1U << d;
		for (int e = 0; e < BP_GET_NDVAS(&pp->pp_bp); e++) {
			const dva_t *x = &pp->pp_bp.blk_dva[e];

			for (int g = 0; g < BP_GET_NDVAS(bp); g++) {
				const dva_t *y = &bp->blk_dva[g];

				if (DVA_GET_VDEV(x) == DVA_GET_VDEV(y) &&
				    DVA_GET_OFFSET(x) == DVA_GET_OFFSET(y))
					covered |= 1U << g;
			}
		}
		if (verify_ranges) {
			/*
			 * Born and freed in this txg: the range stays in this
			 * txg's metaslab allocation tree, so the verifier
			 * must still count it.
			 */
			for (int e = 0; e < BP_GET_NDVAS(&pp->pp_bp); e++) {
				const dva_t *dva = &pp->pp_bp.blk_dva[e];

				parity_cancel_add(pt, DVA_GET_VDEV(dva),
				    DVA_GET_OFFSET(dva),
				    parity_dva_size(ps->ps_spa, dva));
			}
		}
		parity_cancel(pt, pp);
	}
	return (covered);
}

/* metaslab_free(now == B_TRUE): the allocation was undone. */
static void
parity_note_unalloc(void *arg, const blkptr_t *bp, uint64_t txg)
{
	spa_parity_t *ps = arg;
	parity_txg_t *pt = &ps->ps_txg[txg & TXG_MASK];

	if (!parity_rec_window(ps, txg))
		return;
	mutex_enter(&pt->pt_lock);
	(void) parity_cancel_bp(ps, pt, bp, B_FALSE);
	mutex_exit(&pt->pt_lock);
}

/*
 * The BP may have changed after the allocation (zio_ready(), and zil.c for
 * log blocks).  Only a BP with the same DVA set is the same allocation: a
 * dedup parent's BP can carry DVAs of other allocations, and must not
 * replace them.
 */
static void
parity_write_ready(void *arg, const blkptr_t *bp, uint64_t txg)
{
	spa_parity_t *ps = arg;
	parity_txg_t *pt = &ps->ps_txg[txg & TXG_MASK];
	parity_pend_t *pp;

	if (!parity_alloc_window(ps, txg))
		return;
	if ((zfs_parity_test_inject & 4) && random_in_range(100) == 0)
		return;

	mutex_enter(&pt->pt_lock);
	pp = parity_lookup_dva(pt, &bp->blk_dva[0]);
	if (pp != NULL && parity_same_dvas(&pp->pp_bp, bp)) {
		if (pp->pp_slot != UINT64_MAX &&
		    memcmp(&pp->pp_bp, bp, sizeof (*bp)) != 0)
			pp->pp_late = 1;
		pp->pp_bp = *bp;
	}
	mutex_exit(&pt->pt_lock);
}

/*
 * The shadow set of the release verifier (bit 3): the DVA ranges that are held
 * and not released.  A range enters when a free is held and leaves when the
 * release frees it.  A range that enters twice, or leaves without being in the
 * set, is a bug: it counts in ps_vbad, and the end of the txg stops there.
 */
static void
parity_vheld_add(spa_parity_t *ps, const dva_t *dva)
{
	parity_vrt_t *pv;
	uint64_t offset = DVA_GET_OFFSET(dva);
	uint64_t size = parity_dva_size(ps->ps_spa, dva);
	uint64_t ostart, osize;

	mutex_enter(&ps->ps_verify_lock);
	pv = parity_vrt_get(&ps->ps_vheld, DVA_GET_VDEV(dva));
	if (zfs_range_tree_find_in(pv->pv_rt, offset, size, &ostart, &osize)) {
		cmn_err(CE_WARN, "parity verify: held range %llu:%llx+%llx "
		    "overlaps a held range %llx+%llx",
		    (u_longlong_t)DVA_GET_VDEV(dva), (u_longlong_t)offset,
		    (u_longlong_t)size, (u_longlong_t)ostart,
		    (u_longlong_t)osize);
		ps->ps_vbad++;
	} else {
		zfs_range_tree_add(pv->pv_rt, offset, size);
	}
	mutex_exit(&ps->ps_verify_lock);
}

static boolean_t
parity_vheld_remove(spa_parity_t *ps, const dva_t *dva)
{
	parity_vrt_t *pv;
	uint64_t offset = DVA_GET_OFFSET(dva);
	uint64_t size = parity_dva_size(ps->ps_spa, dva);
	boolean_t ok = B_FALSE;

	mutex_enter(&ps->ps_verify_lock);
	pv = parity_vrt_find(&ps->ps_vheld, DVA_GET_VDEV(dva));
	if (pv != NULL && zfs_range_tree_contains(pv->pv_rt, offset, size)) {
		zfs_range_tree_remove(pv->pv_rt, offset, size);
		ok = B_TRUE;
	} else {
		cmn_err(CE_WARN, "parity verify: released range %llu:%llx+%llx "
		    "is not held", (u_longlong_t)DVA_GET_VDEV(dva),
		    (u_longlong_t)offset, (u_longlong_t)size);
		ps->ps_vbad++;
	}
	mutex_exit(&ps->ps_verify_lock);
	return (ok);
}

/*
 * The BP of a FREE entry: the DVAs that are held, packed to the front.  With
 * no held DVA (an intent log block) it is the BP as it is.
 */
static void
parity_free_bp(blkptr_t *dst, const blkptr_t *bp, uint_t held)
{
	int n = 0;

	*dst = *bp;
	if (held == 0)
		return;
	memset(dst->blk_dva, 0, sizeof (dst->blk_dva));
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		if (held & (1U << d))
			dst->blk_dva[n++] = bp->blk_dva[d];
	}
}

/*
 * metaslab_free(now == B_FALSE), syncing context.  Decide per DVA.
 *
 *  - A DVA born in this txg (it has an ALLOC entry in this txg's table) is
 *    cancelled: neither entry is written, and the DVA is freed as usual.
 *    Membership in the table decides, not the birth txg of the BP.
 *  - A free of an intent log block is recorded (as a FREE entry) and not held.
 *  - While draining, a block born after the stop txg was never recorded: it is
 *    freed as usual.  While dropping, nothing is held.
 *  - Every other DVA is held: it is recorded in a FREE entry, and the caller
 *    does not free it.
 *
 * Returns the mask of the held DVAs.  One BP can mix the cases (a dedup BP
 * has DVAs of several txgs).
 */
static uint_t
parity_hold_free(void *arg, const blkptr_t *bp, uint64_t txg)
{
	spa_parity_t *ps = arg;
	spa_t *spa = ps->ps_spa;
	parity_txg_t *pt = &ps->ps_txg[txg & TXG_MASK];
	parity_pend_t *pp;
	parity_state_t state;
	boolean_t is_log, can_hold, in_win;
	uint_t covered, held = 0;
	uint64_t asize = 0;

	/*
	 * Repair: a pending repair request of each DVA of this block is void.
	 * This only marks or removes requests: the caller may be the syncing
	 * thread or a zio free taskq, and neither may wait for repair I/O.
	 */
	spa_parity_repair_invalidate(ps, bp);

	if (txg > spa_freeze_txg(spa) || !parity_rec_window(ps, txg))
		return (0);

	state = ps->ps_state;
	membar_consumer();
	is_log = (BP_GET_TYPE(bp) == DMU_OT_INTENT_LOG);
	/*
	 * In the window: ON, or DRAINING for a block born up to the stop txg.
	 * A block born after the stop txg has no ALLOC entry, so a FREE entry
	 * of it would never be cancelled and a drain of a busy intent log
	 * would never end.
	 */
	in_win = (state == PARITY_ON || (state == PARITY_DRAINING &&
	    BP_GET_PHYSICAL_BIRTH(bp) <= ps->ps_stop_txg));
	can_hold = !is_log && in_win;

	mutex_enter(&pt->pt_lock);
	covered = parity_cancel_bp(ps, pt, bp,
	    (pt->pt_verify & PARITY_VERIFY_ALLOC) != 0);
	if (can_hold) {
		for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
			if (covered & (1U << d))
				continue;
			held |= 1U << d;
			asize += parity_dva_size(spa, &bp->blk_dva[d]);
		}
	}
	if (held != 0 || (is_log && covered == 0 && in_win)) {
		/* No capture after the final dirty txg (see parity_flush()). */
		ASSERT3U(txg, <=, spa_final_dirty_txg(spa));
		pp = parity_pend_alloc();
		parity_free_bp(&pp->pp_bp, bp, held);
		pp->pp_vdev = DVA_GET_VDEV(&pp->pp_bp.blk_dva[0]);
		pp->pp_offset = DVA_GET_OFFSET(&pp->pp_bp.blk_dva[0]);
		pp->pp_kind = PARITY_KIND_FREE;
		pp->pp_slot = UINT64_MAX;
		pp->pp_asize = asize;
		list_insert_tail(&pt->pt_unflushed, pp);
		pt->pt_nunflushed++;
	}
	mutex_exit(&pt->pt_lock);

	if (held != 0) {
		mutex_enter(&ps->ps_lock);
		ps->ps_held_bytes += asize;
		ps->ps_held_dspace += asize;
		ps->ps_stat_held++;
		mutex_exit(&ps->ps_lock);
		if (pt->pt_verify & PARITY_VERIFY_RELEASE) {
			for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
				if (held & (1U << d))
					parity_vheld_add(ps, &bp->blk_dva[d]);
			}
		}
	}
	return (held);
}

/*
 * Fix-ups: the first entries of a new record are the final BPs of the previous
 * record's entries that changed after their flush (kind FIXUP, word 0 = the
 * txg they fix).  If that record is gone (released), they are not needed any
 * more.  If the fix-ups of the previous record could not be rebuilt at load,
 * the header of this record says so.
 */
static void
parity_write_fixups(spa_parity_t *ps, spa_lag_rec_t *lr, dmu_tx_t *tx)
{
	uint64_t n = ps->ps_nlate, chunk, *buf, i;
	parity_pend_t *pp;

	ASSERT0(lr->lr_phys.pr_nentries);
	if (ps->ps_fixups_incomplete) {
		lr->lr_phys.pr_flags |= SPA_LAG_REC_FIXUPS_INCOMPLETE;
		lr->lr_dirty = B_TRUE;
		ps->ps_fixups_incomplete = B_FALSE;
	}
	if (n == 0)
		return;
	if (ps->ps_late_txg != lr->lr_phys.pr_prev_txg ||
	    spa_lag_rec_find(ps->ps_lag, ps->ps_late_txg) == NULL) {
		mutex_enter(&ps->ps_lock);
		parity_list_free(&ps->ps_late, &ps->ps_nlate);
		mutex_exit(&ps->ps_lock);
		return;
	}

	chunk = MIN(n, PARITY_FLUSH_CHUNK);
	buf = vmem_alloc(chunk * PARITY_ENTRY_BYTES, KM_SLEEP);
	pp = list_head(&ps->ps_late);
	for (uint64_t done = 0; done < n; done += i) {
		for (i = 0; i < chunk && pp != NULL; i++) {
			uint64_t *w = &buf[i * PARITY_ENTRY_WORDS];

			w[PARITY_EW_TXG] = ps->ps_late_txg;
			w[PARITY_EW_KIND] = PARITY_KIND_FIXUP;
			memcpy(&w[PARITY_EW_BP], &pp->pp_bp,
			    sizeof (blkptr_t));
			pp = list_next(&ps->ps_late, pp);
		}
		VERIFY3U(i, >, 0);
		VERIFY3U(spa_lag_rec_append(ps->ps_lag, lr, buf, i, tx), ==,
		    done);
	}
	vmem_free(buf, chunk * PARITY_ENTRY_BYTES);
	/*
	 * The header says the fix-ups are in this record before the list moves
	 * out of ps_late: a reader under ps_lock finds them in one of the two.
	 */
	lr->lr_phys.pr_nfixups = n;
	lr->lr_dirty = B_TRUE;
	mutex_enter(&ps->ps_lock);
	ps->ps_record_bytes += n * PARITY_ENTRY_BYTES;
	/* Kept for verifier bit 1 of this txg. */
	parity_list_free(&ps->ps_fixups, &ps->ps_nfixups);
	list_move_tail(&ps->ps_fixups, &ps->ps_late);
	ps->ps_nfixups = n;
	ps->ps_fixups_txg = lr->lr_txg;
	ps->ps_nlate = 0;
	mutex_exit(&ps->ps_lock);
	ps->ps_stat_fixups += n;
}

/*
 * The flush, at the end of every sync pass: append the new entries to this
 * txg's record and VOID the slots of cancelled ones.  Only new allocations,
 * new frees and cancels are written; a BP that changes after it was written is
 * not (it is late: written as a fix-up in the next record), so the sync loop
 * converges as it does for space maps.
 *
 * Nothing else changes the table of the syncing txg now: every allocation
 * and free of this pass has completed, and open context uses other txgs.
 */
static void
parity_flush(void *arg, dmu_tx_t *tx)
{
	spa_parity_t *ps = arg;
	spa_t *spa = ps->ps_spa;
	uint64_t txg = dmu_tx_get_txg(tx);
	parity_txg_t *pt = &ps->ps_txg[txg & TXG_MASK];
	spa_lag_rec_t *lr;
	parity_pend_t *pp;
	list_t voids;
	uint64_t n, nv, *buf, first, i, chunk, done;

	ASSERT(!ps->ps_readonly);	/* a read-only pool writes no record */
	if (txg > spa_freeze_txg(spa) || !parity_rec_window(ps, txg))
		return;

	mutex_enter(&pt->pt_lock);
	n = pt->pt_nunflushed;
	nv = pt->pt_nvoids;
	mutex_exit(&pt->pt_lock);
	if (n == 0 && nv == 0)
		return;
	/*
	 * The txgs after the final dirty txg only clear the deferred frees:
	 * they capture nothing (a capture would dirty the MOS, and dbuf.c
	 * would panic far from here).
	 */
	ASSERT3U(txg, <=, spa_final_dirty_txg(spa));

	lr = spa_lag_rec_get(ps->ps_lag, tx);
	if (lr->lr_phys.pr_nentries == 0)
		parity_write_fixups(ps, lr, tx);
	list_create(&voids, sizeof (parity_pend_t),
	    offsetof(parity_pend_t, pp_lnode));

	/* The voids first: their slots are from earlier passes. */
	mutex_enter(&pt->pt_lock);
	VERIFY3U(pt->pt_nunflushed, ==, n);
	VERIFY3U(pt->pt_nvoids, ==, nv);
	list_move_tail(&voids, &pt->pt_voids);
	pt->pt_nvoids = 0;
	mutex_exit(&pt->pt_lock);

	/* The new entries, in chunks: the sync thread needs little memory. */
	chunk = MIN(n, PARITY_FLUSH_CHUNK);
	buf = (chunk != 0) ? vmem_alloc(chunk * PARITY_ENTRY_BYTES, KM_SLEEP) :
	    NULL;
	for (done = 0; done < n; done += i) {
		first = lr->lr_phys.pr_nentries;
		mutex_enter(&pt->pt_lock);
		for (i = 0; i < chunk &&
		    (pp = list_remove_head(&pt->pt_unflushed)) != NULL; i++) {
			uint64_t *w = &buf[i * PARITY_ENTRY_WORDS];

			w[PARITY_EW_TXG] = txg;
			w[PARITY_EW_KIND] = pp->pp_kind;
			memcpy(&w[PARITY_EW_BP], &pp->pp_bp,
			    sizeof (blkptr_t));
			pp->pp_slot = first + i;
			if (pp->pp_kind == PARITY_KIND_FREE) {
				lr->lr_phys.pr_held_asize += pp->pp_asize;
				avl_add(&pt->pt_frees, pp);
			} else {
				pp->pp_asize = parity_bp_asize(spa, &pp->pp_bp);
				lr->lr_phys.pr_alloc_asize += pp->pp_asize;
			}
		}
		pt->pt_nunflushed -= i;
		mutex_exit(&pt->pt_lock);
		VERIFY3U(i, >, 0);
		VERIFY3U(spa_lag_rec_append(ps->ps_lag, lr, buf, i, tx), ==,
		    first);
	}
	VERIFY0(pt->pt_nunflushed);
	if (buf != NULL)
		vmem_free(buf, chunk * PARITY_ENTRY_BYTES);

	while ((pp = list_remove_head(&voids)) != NULL) {
		/* inject 2 skips every VOID, so a short run cannot miss it */
		if (!(zfs_parity_test_inject & 2)) {
			spa_lag_rec_write_word(ps->ps_lag, lr, pp->pp_slot,
			    PARITY_EW_KIND, PARITY_KIND_VOID, tx);
		}
		ASSERT3U(lr->lr_phys.pr_alloc_asize, >=, pp->pp_asize);
		lr->lr_phys.pr_alloc_asize -= pp->pp_asize;
		lr->lr_dirty = B_TRUE;
		parity_pend_free(pp);
	}
	list_destroy(&voids);
	spa_lag_rec_sync(ps->ps_lag, lr, tx);

	mutex_enter(&ps->ps_lock);
	ps->ps_record_bytes += n * PARITY_ENTRY_BYTES;
	mutex_exit(&ps->ps_lock);
	ps->ps_stat_entries += n;
	ps->ps_stat_voids += nv;
}

/*
 * The release of one record, syncing context.  Read the entries from the
 * release progress on; free each held DVA of a FREE entry; keep the progress
 * in the header.  Returns B_TRUE when the record is done and can be destroyed.
 * Frees that are not held (log blocks) and every other kind change nothing.
 *
 * The verifier (bit 3) checks each DVA before and after: it must be held, it
 * must be in no state of the freeing pipeline, and after the free it must be
 * in ms_freeing.
 */
static void
parity_release_bp(spa_parity_t *ps, const blkptr_t *bp, dmu_tx_t *tx,
    boolean_t verify)
{
	spa_t *spa = ps->ps_spa;
	boolean_t skip = (zfs_parity_test_inject & 8) &&
	    random_in_range(100) == 0;

	if (verify) {
		for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
			const dva_t *dva = &bp->blk_dva[d];

			(void) parity_vheld_remove(ps, dva);
			if (metaslab_range_freed(spa, DVA_GET_VDEV(dva),
			    DVA_GET_OFFSET(dva), parity_dva_size(spa, dva))) {
				cmn_err(CE_WARN, "parity verify: held range "
				    "%llu:%llx was freed or allocatable before "
				    "its release",
				    (u_longlong_t)DVA_GET_VDEV(dva),
				    (u_longlong_t)DVA_GET_OFFSET(dva));
				ps->ps_vbad++;
			}
		}
	}

	if (!skip)
		spa_lag_free_held(spa, bp, tx);

	if (verify) {
		for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
			const dva_t *dva = &bp->blk_dva[d];

			if (!metaslab_range_freeing(spa, DVA_GET_VDEV(dva),
			    DVA_GET_OFFSET(dva), parity_dva_size(spa, dva))) {
				cmn_err(CE_WARN, "parity verify: range "
				    "%llu:%llx is not in ms_freeing: the held "
				    "space leaks",
				    (u_longlong_t)DVA_GET_VDEV(dva),
				    (u_longlong_t)DVA_GET_OFFSET(dva));
				ps->ps_vbad++;
			}
		}
	}
}

/*
 * Add bytes to the lasting count of held space that a drop could not free.
 */
static void
parity_add_leaked(spa_t *spa, uint64_t bytes, dmu_tx_t *tx)
{
	objset_t *mos = spa->spa_meta_objset;
	uint64_t v = 0;

	if (bytes == 0)
		return;
	v = spa_parity_leaked_bytes(spa);
	v += bytes;
	VERIFY0(zap_update(mos, DMU_POOL_DIRECTORY_OBJECT, PARITY_LEAKED_NAME,
	    sizeof (uint64_t), 1, &v, tx));
	atomic_store_64(&spa->spa_parity_leaked, v);
}

static boolean_t
parity_release_rec(void *arg, spa_lag_rec_t *lr, uint64_t *budget,
    dmu_tx_t *tx)
{
	spa_parity_t *ps = arg;
	spa_t *spa = ps->ps_spa;
	spa_lag_rec_phys_t *p = &lr->lr_phys;
	boolean_t dropping = (ps->ps_state == PARITY_DROPPING);
	boolean_t verify;
	uint64_t n = p->pr_nentries, i = p->pr_rel_index, maxchunk, *buf = NULL;
	int error = 0;

	verify = (ps->ps_txg[dmu_tx_get_txg(tx) & TXG_MASK].pt_verify &
	    PARITY_VERIFY_RELEASE) != 0;
	maxchunk = MIN(n > i ? n - i : 0, PARITY_FLUSH_CHUNK);
	if (maxchunk != 0)
		buf = vmem_alloc(maxchunk * PARITY_ENTRY_BYTES, KM_SLEEP);

	while (i < n && *budget > 0) {
		uint64_t cnt = MIN(MIN(n - i, maxchunk), *budget);
		uint64_t rel = 0;

		error = spa_lag_rec_read(ps->ps_lag, lr, i, cnt, buf);
		if (error == 0 && (zfs_parity_test_inject & 32))
			error = SET_ERROR(EIO);
		if (error != 0)
			break;
		for (uint64_t j = 0; j < cnt; j++) {
			const uint64_t *w = &buf[j * PARITY_ENTRY_WORDS];
			const blkptr_t *bp = (const blkptr_t *)&w[PARITY_EW_BP];

			if (w[PARITY_EW_KIND] != PARITY_KIND_FREE ||
			    BP_IS_HOLE(bp) ||
			    BP_GET_TYPE(bp) == DMU_OT_INTENT_LOG)
				continue;
			parity_release_bp(ps, bp, tx, verify);
			rel += parity_bp_asize(spa, bp);
		}
		i += cnt;
		*budget -= cnt;
		p->pr_rel_index = i;
		p->pr_rel_asize += rel;
		lr->lr_dirty = B_TRUE;
		mutex_enter(&ps->ps_lock);
		ps->ps_held_bytes -= MIN(rel, ps->ps_held_bytes);
		ps->ps_held_dspace -= MIN(rel, ps->ps_held_dspace);
		mutex_exit(&ps->ps_lock);
		ps->ps_stat_rel_entries += cnt;
	}
	if (buf != NULL)
		vmem_free(buf, maxchunk * PARITY_ENTRY_BYTES);

	if (error != 0) {
		ps->ps_stat_rel_errors++;
		if (!dropping) {
			/* The next txg tries again. */
			cmn_err(CE_WARN, "parity: pool '%s': record of txg "
			    "%llu is unreadable (error %d); not released",
			    spa_name(spa), (u_longlong_t)lr->lr_txg, error);
			return (B_FALSE);
		}
		/*
		 * A drop must end: the held space of this record that is not
		 * released yet leaks.  Its DVAs are not known, so they are
		 * not freed; the bytes are counted for good.
		 */
		uint64_t leak = p->pr_held_asize > p->pr_rel_asize ?
		    p->pr_held_asize - p->pr_rel_asize : 0;

		parity_add_leaked(spa, leak, tx);
		cmn_err(CE_WARN, "parity: pool '%s': record of txg %llu is "
		    "unreadable (error %d); dropped, %llu bytes of its held "
		    "space are not freed", spa_name(spa),
		    (u_longlong_t)lr->lr_txg, error, (u_longlong_t)leak);
		i = n;
	}
	if (i < n)
		return (B_FALSE);

	/* Done.  Whatever is left in the header is not held any more. */
	mutex_enter(&ps->ps_lock);
	if (p->pr_held_asize > p->pr_rel_asize) {
		uint64_t rem = p->pr_held_asize - p->pr_rel_asize;

		ps->ps_held_bytes -= MIN(rem, ps->ps_held_bytes);
		ps->ps_held_dspace -= MIN(rem, ps->ps_held_dspace);
	}
	ps->ps_record_bytes -= MIN(n * PARITY_ENTRY_BYTES,
	    ps->ps_record_bytes);
	mutex_exit(&ps->ps_lock);
	ps->ps_stat_released++;
	return (B_TRUE);
}

/*
 * State changes.  All in syncing context.
 */

static void parity_dir_update(spa_parity_t *);

/*
 * Discard the records: the outside must resync this pool.  Nothing is held
 * from now on, and the release ignores the applied txg.  Allocations of the
 * open txgs that already captured stay whole (they end at the stop txg).
 */
static void
parity_start_drop(spa_parity_t *ps, dmu_tx_t *tx, uint_t reason)
{
	uint64_t txg = dmu_tx_get_txg(tx);

	ASSERT(dmu_tx_is_syncing(tx));
	if (ps->ps_state == PARITY_DROPPING)
		return;
	if (ps->ps_state == PARITY_OFF && !spa_lag_dir_exists(ps->ps_lag))
		return;

	mutex_enter(&ps->ps_lock);
	if (ps->ps_state == PARITY_ON)
		ps->ps_stop_txg = txg + TXG_CONCURRENT_STATES - 1;
	ps->ps_release_forced = B_TRUE;
	ps->ps_unprotected = (reason == PARITY_DROP_LIMIT);
	membar_producer();
	ps->ps_state = PARITY_DROPPING;
	mutex_exit(&ps->ps_lock);
	ps->ps_stat_drops++;
	parity_dir_update(ps);
	zfs_dbgmsg("parity: pool '%s': dropping the records in txg %llu "
	    "(%s)", spa_name(ps->ps_spa), (u_longlong_t)txg,
	    reason == PARITY_DROP_LIMIT ? "limit" : "request");
	spa_lag_post_unprotected(ps->ps_lag,
	    reason == PARITY_DROP_LIMIT ? "limit" : "drop");
}

/*
 * A drain or a drop is complete: no record is left, and nothing of this txg
 * is captured yet.  A drop starts a new epoch if the property is on; the
 * generation rises either way.  Otherwise the pool is OFF: the directory
 * entry goes and the feature returns to enabled.
 */
static void
parity_finish(spa_parity_t *ps, dmu_tx_t *tx)
{
	spa_t *spa = ps->ps_spa;
	uint64_t txg = dmu_tx_get_txg(tx);
	boolean_t epoch = (ps->ps_state == PARITY_DROPPING && ps->ps_prop_on);
	uint64_t held;

	if (ps->ps_txg[txg & TXG_MASK].pt_verify & PARITY_VERIFY_RELEASE) {
		parity_vrt_t *pv;

		mutex_enter(&ps->ps_verify_lock);
		for (pv = avl_first(&ps->ps_vheld); pv != NULL;
		    pv = AVL_NEXT(&ps->ps_vheld, pv)) {
			if (zfs_range_tree_space(pv->pv_rt) != 0) {
				cmn_err(CE_WARN, "parity verify: txg %llu: "
				    "%llu bytes of held space on vdev %llu "
				    "leak at the end of the records",
				    (u_longlong_t)txg,
				    (u_longlong_t)zfs_range_tree_space(
				    pv->pv_rt), (u_longlong_t)pv->pv_vdev);
				ps->ps_vbad++;
			}
		}
		mutex_exit(&ps->ps_verify_lock);
	}

	mutex_enter(&ps->ps_lock);
	parity_list_free(&ps->ps_late, &ps->ps_nlate);
	ps->ps_fixups_incomplete = B_FALSE;
	ps->ps_unprotected = (ps->ps_unprotected && epoch);
	/* The old store of an import override is gone now. */
	ps->ps_unreadable = B_FALSE;
	held = ps->ps_held_bytes;
	ps->ps_held_bytes = 0;
	ps->ps_held_dspace = 0;
	ps->ps_record_bytes = 0;
	ps->ps_release_forced = B_FALSE;
	ps->ps_set_guid = 0;
	ps->ps_set_guid_dirty = B_FALSE;
	spa_parity_epoch_renew(ps);	/* new epoch id and generation */
	if (epoch) {
		ps->ps_capture_from_txg = txg + TXG_CONCURRENT_STATES;
		ps->ps_start_txg = ps->ps_capture_from_txg;
		ps->ps_stop_txg = 0;
		membar_producer();
		ps->ps_state = PARITY_ON;
	} else {
		ps->ps_capture_from_txg = UINT64_MAX;
		membar_producer();
		ps->ps_state = PARITY_OFF;
	}
	mutex_exit(&ps->ps_lock);

	if (held != 0) {
		zfs_dbgmsg("parity: pool '%s': %llu held bytes were not "
		    "released at the end of the records", spa_name(spa),
		    (u_longlong_t)held);
	}
	if (epoch) {
		parity_dir_update(ps);
		spa_lag_dir_sync(ps->ps_lag, tx);
		zfs_dbgmsg("parity: pool '%s': new epoch from txg %llu, "
		    "generation %llu", spa_name(spa),
		    (u_longlong_t)ps->ps_start_txg,
		    (u_longlong_t)ps->ps_generation);
	} else {
		spa_lag_retire(ps->ps_lag, tx);
		zfs_dbgmsg("parity: pool '%s': off in txg %llu", spa_name(spa),
		    (u_longlong_t)txg);
	}
}

/*
 * The end of spa_sync(), after the last synced uberblock is updated: post the
 * parity_records event of a txg that wrote a record.  The rate limit is for
 * each pool.  A post that the limit held back stays pending, and is made at
 * the end of a later txg (every pool syncs a txg at least each txg timeout),
 * so the last record of a burst always gets its event.
 */
void
spa_parity_txg_done(spa_t *spa)
{
	spa_parity_t *ps = spa->spa_parity;

	spa_parity_repair_barrier(spa);
	if (ps == NULL || ps->ps_event_pending == 0)
		return;
	if (spa_parity_event_records(spa, ps->ps_event_pending))
		ps->ps_event_pending = 0;
}

/*
 * Pass 1: serve a drop request, release the applied records, end a drain or a
 * drop, write the state words.  In a no-op txg it does nothing unless the
 * release is forced, or the pool drains or drops (those must end even on an
 * idle pool): an idle pool stays idle.
 */
static void
parity_sync_pass(void *arg, dmu_tx_t *tx, boolean_t noop)
{
	spa_parity_t *ps = arg;
	spa_t *spa = ps->ps_spa;
	uint64_t txg = dmu_tx_get_txg(tx);
	parity_txg_t *pt = &ps->ps_txg[txg & TXG_MASK];
	spa_lag_rec_t *first;
	uint64_t applied, nrecs, upto;
	uint_t reason;
	boolean_t forced, guid_dirty, empty;
	parity_state_t state;

	ASSERT(!ps->ps_readonly);	/* no release, no state write */
	if (txg > spa_freeze_txg(spa))
		return;

	mutex_enter(&ps->ps_lock);
	applied = ps->ps_applied_txg;
	guid_dirty = ps->ps_set_guid_dirty;
	forced = ps->ps_release_forced || guid_dirty;
	reason = ps->ps_drop_request;
	ps->ps_drop_request = PARITY_DROP_NONE;
	mutex_exit(&ps->ps_lock);

	if (reason != PARITY_DROP_NONE) {
		parity_start_drop(ps, tx, reason);
		forced = B_TRUE;
	}
	state = ps->ps_state;
	if (state == PARITY_OFF && !spa_lag_dir_exists(ps->ps_lag))
		return;
	if (noop && !forced && state != PARITY_DRAINING &&
	    state != PARITY_DROPPING)
		return;

	nrecs = spa_lag_nrecs(ps->ps_lag);
	upto = (state == PARITY_DROPPING) ? UINT64_MAX : applied;
	/*
	 * A reader of records (RECORDS, READ, MAP) holds ps_rec_rwlock: the
	 * sync thread never waits for it, and tries again in the next txg.
	 */
	if (nrecs != 0 && upto != 0 && !zfs_parity_suspend_release &&
	    rw_tryenter(&ps->ps_rec_rwlock, RW_WRITER)) {
		spa_lag_release_sync(ps->ps_lag, upto, zfs_parity_release_limit,
		    tx);
		rw_exit(&ps->ps_rec_rwlock);
	}

	/* Nothing left to release: the next txg need not run the hook. */
	first = spa_lag_rec_first(ps->ps_lag);
	if (first == NULL || first->lr_txg > upto) {
		mutex_enter(&ps->ps_lock);
		ps->ps_release_forced = B_FALSE;
		mutex_exit(&ps->ps_lock);
	}
	spa_lag_dir_set(ps->ps_lag, SLD_APPLIED, applied);

	/*
	 * A drain or a drop ends when no record is left, and nothing is
	 * captured for this txg yet (a free earlier in this pass would
	 * otherwise lose its record).
	 */
	mutex_enter(&pt->pt_lock);
	empty = (avl_numnodes(&pt->pt_tree) == 0 && pt->pt_nvoids == 0 &&
	    pt->pt_nunflushed == 0);
	mutex_exit(&pt->pt_lock);
	if ((state == PARITY_DRAINING || state == PARITY_DROPPING) &&
	    txg > ps->ps_stop_txg && empty && spa_lag_nrecs(ps->ps_lag) == 0) {
		parity_finish(ps, tx);
		return;
	}
	if (guid_dirty) {
		/*
		 * A consumer binds a member before its first record: the
		 * set GUID needs the directory, so make it now.
		 */
		if (state == PARITY_ON)
			spa_lag_dir_create(ps->ps_lag, tx);
		parity_dir_update(ps);
	}
	spa_lag_dir_sync(ps->ps_lag, tx);
}

/*
 * The callbacks of spa_lag.c.
 */

static void
parity_usage(void *arg, spa_lag_usage_t *u)
{
	spa_parity_t *ps = arg;
	parity_state_t state = ps->ps_state;

	membar_consumer();
	u->slu_limited = (state == PARITY_ON || state == PARITY_DRAINING);
	u->slu_held_bytes = ps->ps_held_bytes;
	/* The entries of the syncing txg that are not written yet count. */
	u->slu_record_bytes = ps->ps_record_bytes +
	    ps->ps_txg[spa_syncing_txg(ps->ps_spa) & TXG_MASK].pt_nunflushed *
	    PARITY_ENTRY_BYTES;
}

/* The hard limit with parity_onstall=continue: drop the records. */
static boolean_t
parity_over_limit(void *arg)
{
	spa_parity_t *ps = arg;

	if (ps->ps_state != PARITY_ON && ps->ps_state != PARITY_DRAINING)
		return (B_FALSE);
	if (ps->ps_drop_request != PARITY_DROP_NONE)
		return (B_TRUE);
	mutex_enter(&ps->ps_lock);
	if (ps->ps_drop_request == PARITY_DROP_NONE)
		ps->ps_drop_request = PARITY_DROP_LIMIT;
	ps->ps_release_forced = B_TRUE;
	mutex_exit(&ps->ps_lock);
	return (B_TRUE);
}

static int
parity_drop_op(void *arg)
{
	spa_parity_t *ps = arg;

	if (ps->ps_state == PARITY_OFF && !spa_lag_dir_exists(ps->ps_lag))
		return (SET_ERROR(ENOENT));
	mutex_enter(&ps->ps_lock);
	if (ps->ps_drop_request == PARITY_DROP_NONE)
		ps->ps_drop_request = PARITY_DROP_USER;
	ps->ps_release_forced = B_TRUE;
	mutex_exit(&ps->ps_lock);
	return (0);
}

static boolean_t
parity_drop_pending(void *arg)
{
	spa_parity_t *ps = arg;

	return (ps->ps_drop_request != PARITY_DROP_NONE ||
	    ps->ps_state == PARITY_DROPPING);
}

static void
parity_force(void *arg)
{
	spa_parity_t *ps = arg;

	mutex_enter(&ps->ps_lock);
	ps->ps_release_forced = B_TRUE;
	mutex_exit(&ps->ps_lock);
}

static void
parity_applied(void *arg, uint64_t txg, boolean_t force)
{
	spa_parity_t *ps = arg;

	mutex_enter(&ps->ps_lock);
	ps->ps_applied_txg = MAX(ps->ps_applied_txg, txg);
	if (force || ps->ps_state == PARITY_DRAINING ||
	    ps->ps_state == PARITY_DROPPING)
		ps->ps_release_forced = B_TRUE;
	mutex_exit(&ps->ps_lock);
}

static void
parity_event_payload(void *arg, nvlist_t *nvl)
{
	spa_parity_t *ps = arg;
	spa_lag_summary_t sls;

	spa_lag_summarize(ps->ps_lag, ps->ps_applied_txg, &sls);
	fnvlist_add_uint64(nvl, "generation", ps->ps_generation);
	fnvlist_add_uint64(nvl, "applied_txg", ps->ps_applied_txg);
	fnvlist_add_uint64(nvl, "pending_bytes", sls.sls_pending_bytes);
}

/*
 * The verifiers.
 */

typedef struct parity_held_arg {
	zfs_range_tree_t *pha_held;
	uint64_t	pha_txg;
	uint64_t	pha_vdev;
	uint64_t	pha_bad;
} parity_held_arg_t;

/* An allocated segment that overlaps a held range. */
static void
parity_held_cb(void *arg, uint64_t start, uint64_t size)
{
	parity_held_arg_t *pha = arg;
	uint64_t ostart, osize;

	if (!zfs_range_tree_find_in(pha->pha_held, start, size, &ostart,
	    &osize))
		return;
	pha->pha_bad++;
	cmn_err(CE_WARN, "parity verify: txg %llu: %llu:%llx+%llx is allocated "
	    "while %llx+%llx is held", (u_longlong_t)pha->pha_txg,
	    (u_longlong_t)pha->pha_vdev, (u_longlong_t)start,
	    (u_longlong_t)size, (u_longlong_t)ostart, (u_longlong_t)osize);
}

/* Called by metaslab_sync() for each pass, before it vacates the tree. */
static void
parity_verify_alloc(void *arg, vdev_t *vd, zfs_range_tree_t *rt, uint64_t txg)
{
	spa_parity_t *ps = arg;
	parity_vrt_t *pv;
	uint_t verify = ps->ps_txg[txg & TXG_MASK].pt_verify;

	if (txg > spa_freeze_txg(ps->ps_spa))
		return;

	/* Bit 3: a held range was never freed, so nothing can allocate it. */
	if (verify & PARITY_VERIFY_RELEASE) {
		parity_vrt_t *hv;

		mutex_enter(&ps->ps_verify_lock);
		hv = parity_vrt_find(&ps->ps_vheld, vd->vdev_id);
		if (hv != NULL) {
			parity_held_arg_t pha = {
			    hv->pv_rt, txg, vd->vdev_id, 0 };

			zfs_range_tree_walk(rt, parity_held_cb, &pha);
			ps->ps_vbad += pha.pha_bad;
		}
		mutex_exit(&ps->ps_verify_lock);
	}
	if (!(verify & PARITY_VERIFY_ALLOC))
		return;

	mutex_enter(&ps->ps_verify_lock);
	pv = parity_vrt_get(&ps->ps_verify, vd->vdev_id);
	zfs_range_tree_walk(rt, zfs_range_tree_add, pv->pv_rt);
	mutex_exit(&ps->ps_verify_lock);
}

/*
 * Add a range to the expected set.  The ranges of one txg must not overlap:
 * two entries with the same bytes would be a double allocation.
 */
static boolean_t
parity_want_add(avl_tree_t *want, uint64_t txg, const char *what,
    uint64_t vdev, uint64_t offset, uint64_t size)
{
	parity_vrt_t *pv = parity_vrt_get(want, vdev);
	uint64_t ostart, osize;

	if (zfs_range_tree_find_in(pv->pv_rt, offset, size, &ostart, &osize)) {
		cmn_err(CE_WARN, "parity verify: txg %llu: %s %llu:%llx+%llx "
		    "overlaps %llx+%llx", (u_longlong_t)txg, what,
		    (u_longlong_t)vdev, (u_longlong_t)offset,
		    (u_longlong_t)size, (u_longlong_t)ostart,
		    (u_longlong_t)osize);
		return (B_FALSE);
	}
	zfs_range_tree_add(pv->pv_rt, offset, size);
	return (B_TRUE);
}

typedef struct parity_cmp_arg {
	zfs_range_tree_t *pca_other;
	uint64_t	pca_txg;
	uint64_t	pca_vdev;
	const char	*pca_what;
	uint64_t	pca_bad;
} parity_cmp_arg_t;

/* Report every segment of "this" that "other" does not contain. */
static void
parity_cmp_cb(void *arg, uint64_t start, uint64_t size)
{
	parity_cmp_arg_t *pca = arg;

	if (pca->pca_other != NULL &&
	    zfs_range_tree_contains(pca->pca_other, start, size))
		return;
	pca->pca_bad++;
	if (pca->pca_bad <= 16) {
		cmn_err(CE_WARN, "parity verify: txg %llu: %s %llu:%llx+%llx",
		    (u_longlong_t)pca->pca_txg, pca->pca_what,
		    (u_longlong_t)pca->pca_vdev, (u_longlong_t)start,
		    (u_longlong_t)size);
	}
}

/*
 * Bit 0: the DVA ranges of the captured ALLOC entries, with the cancelled
 * ranges, equal the ranges the metaslabs allocated in this txg.
 */
static boolean_t
parity_verify_txg(spa_parity_t *ps, uint64_t txg)
{
	spa_t *spa = ps->ps_spa;
	parity_txg_t *pt = &ps->ps_txg[txg & TXG_MASK];
	avl_tree_t want;
	parity_pend_t *pp;
	parity_cancel_t *pc;
	parity_vrt_t *pv, *pw;
	boolean_t ok = B_TRUE;

	avl_create(&want, parity_vrt_compare, sizeof (parity_vrt_t),
	    offsetof(parity_vrt_t, pv_node));

	mutex_enter(&pt->pt_lock);
	for (pp = avl_first(&pt->pt_tree); pp != NULL;
	    pp = AVL_NEXT(&pt->pt_tree, pp)) {
		ASSERT3U(pp->pp_kind, ==, PARITY_KIND_ALLOC);
		for (int d = 0; d < BP_GET_NDVAS(&pp->pp_bp); d++) {
			const dva_t *dva = &pp->pp_bp.blk_dva[d];

			ok &= parity_want_add(&want, txg, "alloc",
			    DVA_GET_VDEV(dva), DVA_GET_OFFSET(dva),
			    parity_dva_size(spa, dva));
		}
	}
	for (pc = list_head(&pt->pt_cancel); pc != NULL;
	    pc = list_next(&pt->pt_cancel, pc)) {
		ok &= parity_want_add(&want, txg, "cancel", pc->pc_vdev,
		    pc->pc_offset, pc->pc_size);
	}
	mutex_exit(&pt->pt_lock);

	/*
	 * want == allocated, as sets: same space, and each side contains the
	 * other.  Both trees are merged, so "contains" per segment is exact.
	 */
	mutex_enter(&ps->ps_verify_lock);
	for (pv = avl_first(&ps->ps_verify); pv != NULL;
	    pv = AVL_NEXT(&ps->ps_verify, pv)) {
		parity_cmp_arg_t pca = { NULL, txg, pv->pv_vdev,
		    "allocated by metaslab, not recorded", 0 };

		pw = parity_vrt_find(&want, pv->pv_vdev);
		pca.pca_other = (pw != NULL) ? pw->pv_rt : NULL;
		zfs_range_tree_walk(pv->pv_rt, parity_cmp_cb, &pca);
		ok &= (pca.pca_bad == 0);
	}
	for (pw = avl_first(&want); pw != NULL; pw = AVL_NEXT(&want, pw)) {
		parity_cmp_arg_t pca = { NULL, txg, pw->pv_vdev,
		    "recorded, not allocated by metaslab", 0 };

		pv = parity_vrt_find(&ps->ps_verify, pw->pv_vdev);
		pca.pca_other = (pv != NULL) ? pv->pv_rt : NULL;
		zfs_range_tree_walk(pw->pv_rt, parity_cmp_cb, &pca);
		ok &= (pca.pca_bad == 0);
	}
	mutex_exit(&ps->ps_verify_lock);

	parity_vrt_destroy(&want);
	return (ok);
}

/*
 * Bit 1: the record of this txg, read back, equals the capture.  The header
 * in the index equals the header in memory; every live entry is an ALLOC of
 * this txg in its own slot, with the same BP (or, for a late entry, the same
 * DVAs); nothing is missing; and the live entries add up to pr_alloc_asize.
 */
static boolean_t
parity_verify_rec(spa_parity_t *ps, uint64_t txg)
{
	spa_t *spa = ps->ps_spa;
	parity_txg_t *pt = &ps->ps_txg[txg & TXG_MASK];
	spa_lag_rec_t *lr = spa_lag_rec_find(ps->ps_lag, txg);
	spa_lag_rec_phys_t *phys;
	uint64_t n, nfix, live = 0, bad = 0, asize = 0, *buf, chunk;
	uint64_t nfree = 0, held = 0;
	parity_pend_t *fx = list_head(&ps->ps_fixups);
	int error;

	if (lr == NULL) {
		if (avl_numnodes(&pt->pt_tree) == 0 &&
		    avl_numnodes(&pt->pt_frees) == 0)
			return (B_TRUE);
		cmn_err(CE_WARN, "parity verify: txg %llu: %lu entries, no "
		    "record", (u_longlong_t)txg,
		    (ulong_t)avl_numnodes(&pt->pt_tree));
		return (B_FALSE);
	}

	phys = kmem_alloc(sizeof (*phys), KM_SLEEP);
	error = zap_lookup_uint64(spa->spa_meta_objset,
	    spa_lag_dir_get(ps->ps_lag, SLD_INDEX), &txg, 1,
	    sizeof (uint64_t), SPA_LAG_REC_WORDS, phys);
	if (error != 0 || memcmp(phys, &lr->lr_phys, sizeof (*phys)) != 0) {
		cmn_err(CE_WARN, "parity verify: txg %llu: header on disk "
		    "(error %d) differs from memory", (u_longlong_t)txg, error);
		bad++;
	}
	kmem_free(phys, sizeof (*phys));

	n = lr->lr_phys.pr_nentries;
	nfix = (ps->ps_fixups_txg == txg) ? ps->ps_nfixups : 0;
	if (lr->lr_phys.pr_nfixups != nfix || n <= nfix) {
		cmn_err(CE_WARN, "parity verify: txg %llu: header nentries "
		    "%llu nfixups %llu", (u_longlong_t)txg, (u_longlong_t)n,
		    (u_longlong_t)lr->lr_phys.pr_nfixups);
		return (B_FALSE);
	}

	chunk = MIN(n, PARITY_FLUSH_CHUNK);
	buf = vmem_alloc(chunk * PARITY_ENTRY_WORDS * sizeof (uint64_t),
	    KM_SLEEP);
	for (uint64_t base = 0; base < n; base += chunk) {
		uint64_t cnt = MIN(chunk, n - base);

		error = spa_lag_rec_read(ps->ps_lag, lr, base, cnt, buf);
		if (error != 0) {
			cmn_err(CE_WARN, "parity verify: txg %llu: read "
			    "error %d", (u_longlong_t)txg, error);
			bad++;
			break;
		}

		mutex_enter(&pt->pt_lock);
		for (uint64_t j = 0; j < cnt; j++) {
			const uint64_t *w = &buf[j * PARITY_ENTRY_WORDS];
			const blkptr_t *bp =
			    (const blkptr_t *)&w[PARITY_EW_BP];
			uint64_t i = base + j;
			parity_pend_t *pp;

			if (i < nfix) {
				/* The fix-ups, in written order. */
				if (w[PARITY_EW_KIND] !=
				    PARITY_KIND_FIXUP || w[PARITY_EW_TXG] !=
				    lr->lr_phys.pr_prev_txg || fx == NULL ||
				    memcmp(bp, &fx->pp_bp, sizeof (*bp)) != 0) {
					if (++bad <= 16) {
						cmn_err(CE_WARN, "parity "
						    "verify: txg %llu: fix-up "
						    "%llu differs",
						    (u_longlong_t)txg,
						    (u_longlong_t)i);
					}
				}
				if (fx != NULL)
					fx = list_next(&ps->ps_fixups, fx);
				continue;
			}
			if (w[PARITY_EW_KIND] == PARITY_KIND_VOID)
				continue;
			if (w[PARITY_EW_KIND] == PARITY_KIND_FREE) {
				parity_pend_t key, *pf;

				key.pp_slot = i;
				pf = avl_find(&pt->pt_frees, &key, NULL);
				nfree++;
				if (pf != NULL)
					held += pf->pp_asize;
				if (w[PARITY_EW_TXG] != txg || pf == NULL ||
				    memcmp(bp, &pf->pp_bp, sizeof (*bp)) != 0) {
					if (++bad <= 16) {
						cmn_err(CE_WARN, "parity: "
						    "txg %llu: FREE entry %llu "
						    "differs from capture",
						    (u_longlong_t)txg,
						    (u_longlong_t)i);
					}
				}
				continue;
			}
			live++;
			pp = parity_lookup_dva(pt, &bp->blk_dva[0]);
			if (pp != NULL)
				asize += pp->pp_asize;
			if (w[PARITY_EW_TXG] != txg ||
			    w[PARITY_EW_KIND] != PARITY_KIND_ALLOC ||
			    pp == NULL || pp->pp_slot != i ||
			    (!pp->pp_late &&
			    memcmp(bp, &pp->pp_bp, sizeof (*bp)) != 0) ||
			    (pp->pp_late &&
			    !parity_same_dvas(bp, &pp->pp_bp))) {
				if (++bad <= 16) {
					cmn_err(CE_WARN, "parity verify: txg "
					    "%llu: entry %llu (txg %llu kind "
					    "%llu) does not match the capture "
					    "(%s)", (u_longlong_t)txg,
					    (u_longlong_t)i,
					    (u_longlong_t)w[PARITY_EW_TXG],
					    (u_longlong_t)w[PARITY_EW_KIND],
					    pp == NULL ? "not captured" :
					    "differs");
				}
			}
		}
		mutex_exit(&pt->pt_lock);
	}
	vmem_free(buf, chunk * PARITY_ENTRY_WORDS * sizeof (uint64_t));

	if (bad == 0 && live != avl_numnodes(&pt->pt_tree)) {
		cmn_err(CE_WARN, "parity verify: txg %llu: %llu live entries "
		    "on disk, %lu captured", (u_longlong_t)txg,
		    (u_longlong_t)live, (ulong_t)avl_numnodes(&pt->pt_tree));
		bad++;
	}
	if (bad == 0 && nfree != avl_numnodes(&pt->pt_frees)) {
		cmn_err(CE_WARN, "parity verify: txg %llu: %llu FREE "
		    "entries on disk, %lu captured", (u_longlong_t)txg,
		    (u_longlong_t)nfree, (ulong_t)avl_numnodes(&pt->pt_frees));
		bad++;
	}
	if (bad == 0 && held != lr->lr_phys.pr_held_asize) {
		cmn_err(CE_WARN, "parity verify: txg %llu: FREE entries hold "
		    "%llu bytes, header says %llu", (u_longlong_t)txg,
		    (u_longlong_t)held,
		    (u_longlong_t)lr->lr_phys.pr_held_asize);
		bad++;
	}
	if (bad == 0 && asize != lr->lr_phys.pr_alloc_asize) {
		cmn_err(CE_WARN, "parity verify: txg %llu: live entries hold "
		    "%llu bytes, header says %llu", (u_longlong_t)txg,
		    (u_longlong_t)asize,
		    (u_longlong_t)lr->lr_phys.pr_alloc_asize);
		bad++;
	}
	return (bad == 0);
}

/*
 * A block of the tree that was not recorded under its DVA[0]: print it, and
 * any recorded entry that has one of its DVAs.  Under pt_lock.
 */
static void
parity_verify_final_why(spa_parity_t *ps, parity_txg_t *pt, uint64_t txg,
    const blkptr_t *bp)
{
	char *s = kmem_alloc(BP_SPRINTF_LEN, KM_SLEEP);
	parity_pend_t *pp;

	(void) ps;
	snprintf_blkptr(s, BP_SPRINTF_LEN, bp);
	cmn_err(CE_WARN, "parity verify: txg %llu: block born in this txg "
	    "was not recorded: %s", (u_longlong_t)txg, s);
	for (pp = avl_first(&pt->pt_tree); pp != NULL;
	    pp = AVL_NEXT(&pt->pt_tree, pp)) {
		boolean_t hit = B_FALSE;

		for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
			for (int e = 0; e < BP_GET_NDVAS(&pp->pp_bp); e++) {
				if (DVA_EQUAL(&bp->blk_dva[d],
				    &pp->pp_bp.blk_dva[e]))
					hit = B_TRUE;
			}
		}
		if (!hit)
			continue;
		snprintf_blkptr(s, BP_SPRINTF_LEN, &pp->pp_bp);
		cmn_err(CE_WARN, "parity verify:   shares a DVA with the "
		    "entry (late %d slot %lld): %s", pp->pp_late,
		    (longlong_t)pp->pp_slot, s);
	}
	kmem_free(s, BP_SPRINTF_LEN);
}

/*
 * Bit 2: every recorded allocation of this txg is in the block tree with
 * exactly the BP we hold (the BP the fix-ups carry), and every block born in
 * this txg was recorded.  Expensive (a traversal of the txg); ztest only.
 */
static boolean_t
parity_verify_final(spa_parity_t *ps, uint64_t txg)
{
	parity_txg_t *pt = &ps->ps_txg[txg & TXG_MASK];
	avl_tree_t map;
	parity_pend_t *pp, *f;
	uint64_t bad = 0;
	uint64_t unmapped = 0;
	boolean_t partial;
	int error;

	error = parity_born_map(ps->ps_spa, txg, &map);
	if (error != 0) {
		cmn_err(CE_WARN, "parity verify: txg %llu: traversal error "
		    "%d", (u_longlong_t)txg, error);
		return (B_FALSE);
	}
	partial = parity_map_partial(ps->ps_spa);

	mutex_enter(&pt->pt_lock);
	for (pp = avl_first(&pt->pt_tree); pp != NULL;
	    pp = AVL_NEXT(&pt->pt_tree, pp)) {
		if (!parity_born_wanted(&pp->pp_bp, txg))
			continue;
		f = avl_find(&map, pp, NULL);
		if (f != NULL && parity_map_match(f, &pp->pp_bp))
			continue;
		if (f == NULL && partial) {
			unmapped++;
			continue;
		}
		if (++bad <= 16) {
			char *s = kmem_alloc(BP_SPRINTF_LEN, KM_SLEEP);

			snprintf_blkptr(s, BP_SPRINTF_LEN, &pp->pp_bp);
			cmn_err(CE_WARN, "parity verify: txg %llu (root birth "
			    "%llu, synced %llu, %lu in the tree): recorded "
			    "BP (late %d) %s: %s", (u_longlong_t)txg,
			    (u_longlong_t)BP_GET_LOGICAL_BIRTH(
			    &ps->ps_spa->spa_uberblock.ub_rootbp),
			    (u_longlong_t)ps->ps_spa->spa_ubsync.ub_txg,
			    (ulong_t)avl_numnodes(&map), pp->pp_late,
			    f == NULL ? "is not in the tree" :
			    "differs from the tree", s);
			if (f != NULL) {
				snprintf_blkptr(s, BP_SPRINTF_LEN, &f->pp_bp);
				cmn_err(CE_WARN, "parity verify:   tree BP: "
				    "%s", s);
			}
			kmem_free(s, BP_SPRINTF_LEN);
		}
	}
	for (f = avl_first(&map); f != NULL; f = AVL_NEXT(&map, f)) {
		/* Each allocation of the txg once, by its DVA[0]. */
		if (f->pp_slot != 0 || f->pp_kind == PARITY_MAP_REF)
			continue;
		if (avl_find(&pt->pt_tree, f, NULL) != NULL)
			continue;
		if (++bad <= 16) {
			parity_verify_final_why(ps, pt, txg, &f->pp_bp);
		}
	}
	mutex_exit(&pt->pt_lock);
	parity_map_destroy(&map);
	ps->ps_stat_lost += unmapped;
	return (bad == 0);
}

/*
 * Bit 3, at the end of every txg: nothing was found wrong in the txg (a range
 * that was released twice or not held, one that was freed behind our back, one
 * that leaked, or one that was allocated while held); the shadow set of held
 * ranges is as big as the held-bytes counter; and so are the headers of the
 * records, less what they have released.
 */
static boolean_t
parity_verify_release(spa_parity_t *ps, uint64_t txg)
{
	spa_lag_summary_t sls;
	parity_vrt_t *pv;
	uint64_t shadow = 0, held;
	boolean_t ok = B_TRUE;

	spa_lag_summarize(ps->ps_lag, 0, &sls);
	mutex_enter(&ps->ps_verify_lock);
	for (pv = avl_first(&ps->ps_vheld); pv != NULL;
	    pv = AVL_NEXT(&ps->ps_vheld, pv))
		shadow += zfs_range_tree_space(pv->pv_rt);
	if (ps->ps_vbad != 0) {
		cmn_err(CE_WARN, "parity verify: txg %llu: %llu release "
		    "errors", (u_longlong_t)txg, (u_longlong_t)ps->ps_vbad);
		ok = B_FALSE;
	}
	mutex_exit(&ps->ps_verify_lock);

	mutex_enter(&ps->ps_lock);
	held = ps->ps_held_bytes;
	mutex_exit(&ps->ps_lock);
	if (shadow != held || sls.sls_held_bytes != held) {
		cmn_err(CE_WARN, "parity verify: txg %llu: held space: "
		    "%llu in the shadow set, %llu in the counter, %llu in the "
		    "record headers", (u_longlong_t)txg, (u_longlong_t)shadow,
		    (u_longlong_t)held, (u_longlong_t)sls.sls_held_bytes);
		ok = B_FALSE;
	}
	return (ok);
}

/*
 * End of txg, called at the end of spa_sync() for every txg: check, keep
 * the final BPs of the late entries for the fix-ups, then drop the table.
 */
static void
parity_txg_synced(void *arg, uint64_t txg)
{
	spa_parity_t *ps = arg;
	spa_t *spa = ps->ps_spa;
	parity_txg_t *pt = &ps->ps_txg[txg & TXG_MASK];
	boolean_t frozen = (txg > spa_freeze_txg(spa));

	/* Save the uberblock for lineage when this txg wrote one. */
	if (spa->spa_uberblock.ub_txg == txg)
		spa_parity_lineage_note(ps, &spa->spa_uberblock);

	if (!frozen && parity_rec_window(ps, txg)) {
		/* An allocation or a free after the last flush is a bug. */
		VERIFY0(pt->pt_nunflushed);
		VERIFY0(pt->pt_nvoids);

		/*
		 * A mismatch means parity would miss bytes: stop here.
		 * Bits 0 and 2 expect every allocation of the txg in the
		 * records, so they apply only to the ALLOC window.  After
		 * the stop txg of a drain or a drop the record window stays
		 * open for the held frees, but no allocation is recorded on
		 * purpose: a draining pool leaves the set, and a dropped
		 * one is resynced from a new epoch.  Those blocks are in no
		 * protected image (they land in space that the applied
		 * state has free, which parity counts as zero; the applied
		 * blocks stay held), so there is nothing for bit 2 to find.
		 */
		if ((pt->pt_verify & PARITY_VERIFY_ALLOC) &&
		    parity_alloc_window(ps, txg))
			VERIFY(parity_verify_txg(ps, txg));
		/*
		 * Outside the ALLOC window nothing may be captured: check it
		 * (with bit 0, the one that guards the captured ALLOCs), so
		 * that a later bug is found as "should not exist", not only
		 * as a mismatch between memory and disk.
		 */
		if ((pt->pt_verify & PARITY_VERIFY_ALLOC) &&
		    !parity_alloc_window(ps, txg)) {
			parity_pend_t *pp;

			mutex_enter(&pt->pt_lock);
			for (pp = avl_first(&pt->pt_tree); pp != NULL;
			    pp = AVL_NEXT(&pt->pt_tree, pp))
				VERIFY3U(pp->pp_kind, !=, PARITY_KIND_ALLOC);
			mutex_exit(&pt->pt_lock);
		}
		if (pt->pt_verify & PARITY_VERIFY_RECORD)
			VERIFY(parity_verify_rec(ps, txg));
		if ((pt->pt_verify & PARITY_VERIFY_FINAL) &&
		    parity_alloc_window(ps, txg))
			VERIFY(parity_verify_final(ps, txg));
		if (pt->pt_verify & PARITY_VERIFY_RELEASE)
			VERIFY(parity_verify_release(ps, txg));
	}
	mutex_enter(&ps->ps_lock);
	if (ps->ps_fixups_txg == txg)
		parity_list_free(&ps->ps_fixups, &ps->ps_nfixups);
	mutex_exit(&ps->ps_lock);

	/*
	 * The late entries' BPs are final now.  Any older fix-ups not yet
	 * written are not needed: this txg has a record, so they were written
	 * into it, or their record is gone.
	 */
	if (!frozen && spa_lag_rec_find(ps->ps_lag, txg) != NULL) {
		parity_pend_t *pp, *next;

		mutex_enter(&ps->ps_lock);
		parity_list_free(&ps->ps_late, &ps->ps_nlate);
		mutex_enter(&pt->pt_lock);
		for (pp = avl_first(&pt->pt_tree); pp != NULL; pp = next) {
			next = AVL_NEXT(&pt->pt_tree, pp);
			if (!pp->pp_late)
				continue;
			avl_remove(&pt->pt_tree, pp);
			list_insert_tail(&ps->ps_late, pp);
			ps->ps_nlate++;
			ps->ps_stat_late++;
		}
		mutex_exit(&pt->pt_lock);
		ps->ps_late_txg = txg;
		mutex_exit(&ps->ps_lock);
	}

	if (spa_lag_rec_find(ps->ps_lag, txg) != NULL) {
		uint64_t passes = spa_sync_pass(spa);

		ps->ps_stat_txgs++;
		ps->ps_stat_passes += passes;
		ps->ps_stat_max_passes = MAX(ps->ps_stat_max_passes, passes);
		/*
		 * The event is posted when spa_sync() has moved spa_ubsync on
		 * (spa_parity_txg_done()): a daemon that calls RECORDS at once
		 * must see this txg.
		 */
		if (!frozen)
			ps->ps_event_pending = txg;
	}

	/* The test stand-in for the parity software: it makes progress. */
	if (zfs_parity_test_auto_apply != 0 && !frozen)
		spa_lag_progress(ps->ps_lag);
	if (zfs_parity_test_auto_apply != 0 && !frozen &&
	    txg > zfs_parity_test_auto_apply) {
		mutex_enter(&ps->ps_lock);
		ps->ps_applied_txg = MAX(ps->ps_applied_txg,
		    txg - zfs_parity_test_auto_apply);
		mutex_exit(&ps->ps_lock);
	}

	parity_txg_clear(pt);

	mutex_enter(&ps->ps_verify_lock);
	parity_vrt_destroy(&ps->ps_verify);
	avl_create(&ps->ps_verify, parity_vrt_compare,
	    sizeof (parity_vrt_t), offsetof(parity_vrt_t, pv_node));
	mutex_exit(&ps->ps_verify_lock);
}

static uint64_t
parity_held_dspace(void *arg)
{
	spa_parity_t *ps = arg;

	return (ps->ps_held_dspace);
}

static const spa_lag_ops_t parity_lag_ops = {
	.slo_name = "parity_records",
	.slo_feature = SPA_FEATURE_PARITY_RECORDS,
	.slo_dir_name = PARITY_DIR_NAME,
	.slo_entry_words = PARITY_ENTRY_WORDS,
	.slo_note_alloc = parity_note_alloc,
	.slo_note_unalloc = parity_note_unalloc,
	.slo_write_ready = parity_write_ready,
	.slo_hold_free = parity_hold_free,
	.slo_sync_pass = parity_sync_pass,
	.slo_flush = parity_flush,
	.slo_txg_synced = parity_txg_synced,
	.slo_verify_alloc = parity_verify_alloc,
	.slo_held_dspace = parity_held_dspace,
	.slo_release_rec = parity_release_rec,
	.slo_usage = parity_usage,
	.slo_over_limit = parity_over_limit,
	.slo_drop = parity_drop_op,
	.slo_drop_pending = parity_drop_pending,
	.slo_force = parity_force,
	.slo_applied = parity_applied,
	.slo_event_payload = parity_event_payload,
	.slo_event_throttle = ESC_ZFS_PARITY_THROTTLE,
	.slo_event_unprotected = ESC_ZFS_PARITY_UNPROTECTED,
};

/*
 * The kstat zfs/<pool>/parity_set.  It lives as long as the parity state
 * (parity_create() to spa_parity_unload()), and each read computes it from
 * spa_parity_get_info() and the counters, so no hot path writes it.
 * zpool-parity(8) lists the fields.
 */
typedef struct parity_kstat {
	kstat_named_t	pk_state;
	kstat_named_t	pk_generation;
	kstat_named_t	pk_set_guid;
	kstat_named_t	pk_start_txg;
	kstat_named_t	pk_applied_txg;
	kstat_named_t	pk_synced_txg;
	kstat_named_t	pk_lag_txgs;
	kstat_named_t	pk_first_txg;
	kstat_named_t	pk_last_txg;
	kstat_named_t	pk_records;
	kstat_named_t	pk_pending_bytes;
	kstat_named_t	pk_pending_txgs;
	kstat_named_t	pk_held_bytes;
	kstat_named_t	pk_record_bytes;
	kstat_named_t	pk_max_record_bytes;
	kstat_named_t	pk_max_held_bytes;
	kstat_named_t	pk_leaked_bytes;
	kstat_named_t	pk_throttle;
	kstat_named_t	pk_throttled_ms;
	kstat_named_t	pk_applied_age_ms;
	kstat_named_t	pk_unprotected;
	kstat_named_t	pk_drop_pending;
	kstat_named_t	pk_record_txgs;
	kstat_named_t	pk_entries;
	kstat_named_t	pk_released;
	kstat_named_t	pk_drops;
	kstat_named_t	pk_repair_requests;
	kstat_named_t	pk_repair_supplied;
	kstat_named_t	pk_repair_declined;
	kstat_named_t	pk_repair_rejected;
	kstat_named_t	pk_repair_written;
	kstat_named_t	pk_read_bytes;
} parity_kstat_t;

static const parity_kstat_t parity_kstat_template = {
	{ "state",		KSTAT_DATA_UINT64 },
	{ "generation",		KSTAT_DATA_UINT64 },
	{ "set_guid",		KSTAT_DATA_UINT64 },
	{ "start_txg",		KSTAT_DATA_UINT64 },
	{ "applied_txg",	KSTAT_DATA_UINT64 },
	{ "synced_txg",		KSTAT_DATA_UINT64 },
	{ "lag_txgs",		KSTAT_DATA_UINT64 },
	{ "first_txg",		KSTAT_DATA_UINT64 },
	{ "last_txg",		KSTAT_DATA_UINT64 },
	{ "records",		KSTAT_DATA_UINT64 },
	{ "pending_bytes",	KSTAT_DATA_UINT64 },
	{ "pending_txgs",	KSTAT_DATA_UINT64 },
	{ "held_bytes",		KSTAT_DATA_UINT64 },
	{ "record_bytes",	KSTAT_DATA_UINT64 },
	{ "max_record_bytes",	KSTAT_DATA_UINT64 },
	{ "max_held_bytes",	KSTAT_DATA_UINT64 },
	{ "leaked_bytes",	KSTAT_DATA_UINT64 },
	{ "throttle",		KSTAT_DATA_UINT64 },
	{ "throttled_ms",	KSTAT_DATA_UINT64 },
	{ "applied_age_ms",	KSTAT_DATA_UINT64 },
	{ "unprotected",	KSTAT_DATA_UINT64 },
	{ "drop_pending",	KSTAT_DATA_UINT64 },
	{ "record_txgs",	KSTAT_DATA_UINT64 },
	{ "entries",		KSTAT_DATA_UINT64 },
	{ "released",		KSTAT_DATA_UINT64 },
	{ "drops",		KSTAT_DATA_UINT64 },
	{ "repair_requests",	KSTAT_DATA_UINT64 },
	{ "repair_supplied",	KSTAT_DATA_UINT64 },
	{ "repair_declined",	KSTAT_DATA_UINT64 },
	{ "repair_rejected",	KSTAT_DATA_UINT64 },
	{ "repair_written",	KSTAT_DATA_UINT64 },
	{ "read_bytes",		KSTAT_DATA_UINT64 },
};

static int
parity_kstat_update(kstat_t *ksp, int rw)
{
	spa_t *spa = ksp->ks_private;
	spa_parity_t *ps = spa->spa_parity;
	parity_kstat_t *pk = ksp->ks_data;
	spa_parity_info_t pi;
	uint64_t synced;

	if (rw == KSTAT_WRITE)
		return (SET_ERROR(EACCES));
	if (ps == NULL)
		return (0);

	spa_parity_get_info(spa, &pi);
	synced = spa_last_synced_txg(spa);
	pk->pk_state.value.ui64 = pi.pi_state;
	pk->pk_generation.value.ui64 = pi.pi_generation;
	pk->pk_set_guid.value.ui64 = pi.pi_set_guid;
	pk->pk_start_txg.value.ui64 = pi.pi_start_txg;
	pk->pk_applied_txg.value.ui64 = pi.pi_applied_txg;
	pk->pk_synced_txg.value.ui64 = synced;
	pk->pk_lag_txgs.value.ui64 = (pi.pi_state != PARITY_OFF &&
	    synced > pi.pi_applied_txg) ? synced - pi.pi_applied_txg : 0;
	pk->pk_first_txg.value.ui64 = pi.pi_first_txg;
	pk->pk_last_txg.value.ui64 = pi.pi_last_txg;
	pk->pk_records.value.ui64 = pi.pi_nrecs;
	pk->pk_pending_bytes.value.ui64 = pi.pi_pending_bytes;
	pk->pk_pending_txgs.value.ui64 = pi.pi_pending_txgs;
	pk->pk_held_bytes.value.ui64 = pi.pi_held_bytes;
	pk->pk_record_bytes.value.ui64 = pi.pi_record_bytes;
	pk->pk_max_record_bytes.value.ui64 = pi.pi_max_record_bytes;
	pk->pk_max_held_bytes.value.ui64 = pi.pi_max_held_bytes;
	pk->pk_leaked_bytes.value.ui64 = pi.pi_leaked_bytes;
	pk->pk_throttle.value.ui64 = pi.pi_throttle;
	pk->pk_throttled_ms.value.ui64 = spa_lag_throttled_ms(ps->ps_lag);
	pk->pk_applied_age_ms.value.ui64 = spa_lag_applied_age_ms(ps->ps_lag);
	pk->pk_unprotected.value.ui64 =
	    (pi.pi_flags & SPA_PARITY_F_UNPROTECTED) ? 1 : 0;
	pk->pk_drop_pending.value.ui64 =
	    (pi.pi_flags & SPA_PARITY_F_DROP_PENDING) ? 1 : 0;
	pk->pk_record_txgs.value.ui64 = atomic_load_64(&ps->ps_stat_txgs);
	pk->pk_entries.value.ui64 = atomic_load_64(&ps->ps_stat_entries);
	pk->pk_released.value.ui64 = atomic_load_64(&ps->ps_stat_released);
	pk->pk_drops.value.ui64 = atomic_load_64(&ps->ps_stat_drops);
	pk->pk_repair_requests.value.ui64 =
	    atomic_load_64(&ps->ps_stat_repair_requests);
	pk->pk_repair_supplied.value.ui64 =
	    atomic_load_64(&ps->ps_stat_repair_supplied);
	pk->pk_repair_declined.value.ui64 =
	    atomic_load_64(&ps->ps_stat_repair_declined);
	pk->pk_repair_rejected.value.ui64 =
	    atomic_load_64(&ps->ps_stat_repair_rejected);
	pk->pk_repair_written.value.ui64 =
	    atomic_load_64(&ps->ps_stat_repair_written);
	pk->pk_read_bytes.value.ui64 = atomic_load_64(&ps->ps_stat_read_bytes);
	return (0);
}

static void
parity_kstat_init(spa_t *spa)
{
	spa_history_kstat_t *shk = &spa->spa_stats.parity_set;
	char *name;
	kstat_t *ksp;

	ASSERT0P(shk->kstat);
	mutex_init(&shk->lock, NULL, MUTEX_DEFAULT, NULL);
	name = kmem_asprintf("zfs/%s", spa_name(spa));
	ksp = kstat_create(name, 0, "parity_set", "misc", KSTAT_TYPE_NAMED,
	    sizeof (parity_kstat_t) / sizeof (kstat_named_t),
	    KSTAT_FLAG_VIRTUAL);
	kmem_strfree(name);
	shk->kstat = ksp;
	if (ksp == NULL)
		return;
	ksp->ks_lock = &shk->lock;
	ksp->ks_data = kmem_alloc(sizeof (parity_kstat_t), KM_SLEEP);
	memcpy(ksp->ks_data, &parity_kstat_template, sizeof (parity_kstat_t));
	ksp->ks_private = spa;
	ksp->ks_update = parity_kstat_update;
	kstat_install(ksp);
}

/* First in spa_parity_unload(): kstat_delete() waits for the readers. */
static void
parity_kstat_fini(spa_t *spa)
{
	spa_history_kstat_t *shk = &spa->spa_stats.parity_set;
	kstat_t *ksp = shk->kstat;

	if (ksp != NULL) {
		shk->kstat = NULL;
		kstat_delete(ksp);
		kmem_free(ksp->ks_data, sizeof (parity_kstat_t));
	}
	mutex_destroy(&shk->lock);
}

/*
 * Lifecycle.
 */

static spa_parity_t *
parity_create(spa_t *spa)
{
	spa_parity_t *ps = kmem_zalloc(sizeof (*ps), KM_SLEEP);

	ps->ps_spa = spa;
	ps->ps_state = PARITY_OFF;
	ps->ps_capture_from_txg = UINT64_MAX;
	mutex_init(&ps->ps_lock, NULL, MUTEX_DEFAULT, NULL);
	rw_init(&ps->ps_rec_rwlock, NULL, RW_DEFAULT, NULL);
	mutex_init(&ps->ps_verify_lock, NULL, MUTEX_DEFAULT, NULL);
	avl_create(&ps->ps_verify, parity_vrt_compare,
	    sizeof (parity_vrt_t), offsetof(parity_vrt_t, pv_node));
	avl_create(&ps->ps_vheld, parity_vrt_compare,
	    sizeof (parity_vrt_t), offsetof(parity_vrt_t, pv_node));
	for (int i = 0; i < TXG_SIZE; i++)
		parity_txg_init(&ps->ps_txg[i]);
	list_create(&ps->ps_late, sizeof (parity_pend_t),
	    offsetof(parity_pend_t, pp_lnode));
	list_create(&ps->ps_fixups, sizeof (parity_pend_t),
	    offsetof(parity_pend_t, pp_lnode));

	spa_parity_repair_init(ps);	/* Create the repair request table. */
	ps->ps_lag = spa_lag_register(spa, &parity_lag_ops, ps);
	/*
	 * Publish the pointer last: other threads (INFO, spa_parity_busy())
	 * follow spa->spa_parity to ps_lag without a lock.
	 */
	membar_producer();
	spa->spa_parity = ps;
	parity_kstat_init(spa);
	return (ps);
}

/* Only in spa_unload(): the hot paths read the pointer without a lock. */
void
spa_parity_unload(spa_t *spa)
{
	spa_parity_t *ps = spa->spa_parity;

	spa_parity_reclaim_clear(spa);
	if (ps == NULL)
		return;

	parity_kstat_fini(spa);
	if (ps->ps_stat_txgs != 0) {
		zfs_dbgmsg("parity: pool '%s': %llu record txgs, %llu sync "
		    "passes (max %llu), %llu entries, %llu void, %llu late, "
		    "%llu fix-ups, %llu records released, %llu lost at load, "
		    "%llu held, %llu drops",
		    spa_name(spa),
		    (u_longlong_t)ps->ps_stat_txgs,
		    (u_longlong_t)ps->ps_stat_passes,
		    (u_longlong_t)ps->ps_stat_max_passes,
		    (u_longlong_t)ps->ps_stat_entries,
		    (u_longlong_t)ps->ps_stat_voids,
		    (u_longlong_t)ps->ps_stat_late,
		    (u_longlong_t)ps->ps_stat_fixups,
		    (u_longlong_t)ps->ps_stat_released,
		    (u_longlong_t)ps->ps_stat_lost,
		    (u_longlong_t)ps->ps_stat_held,
		    (u_longlong_t)ps->ps_stat_drops);
	}

	spa_parity_repair_fini(ps);	/* Destroy the repair request table. */
	spa_lag_unregister(ps->ps_lag);
	spa->spa_parity = NULL;

	for (int i = 0; i < TXG_SIZE; i++)
		parity_txg_fini(&ps->ps_txg[i]);
	parity_list_free(&ps->ps_late, &ps->ps_nlate);
	parity_list_free(&ps->ps_fixups, &ps->ps_nfixups);
	list_destroy(&ps->ps_late);
	list_destroy(&ps->ps_fixups);
	parity_vrt_destroy(&ps->ps_verify);
	parity_vrt_destroy(&ps->ps_vheld);
	mutex_destroy(&ps->ps_verify_lock);
	rw_destroy(&ps->ps_rec_rwlock);
	mutex_destroy(&ps->ps_lock);
	kmem_free(ps, sizeof (*ps));
}

static void
parity_dir_update(spa_parity_t *ps)
{
	uint64_t set_guid;

	mutex_enter(&ps->ps_lock);
	set_guid = ps->ps_set_guid;
	mutex_exit(&ps->ps_lock);
	spa_lag_dir_set(ps->ps_lag, SLD_STATE, ps->ps_state |
	    (ps->ps_unprotected ? PARITY_DIR_UNPROTECTED : 0));
	spa_lag_dir_set(ps->ps_lag, SLD_START, ps->ps_start_txg);
	spa_lag_dir_set(ps->ps_lag, SLD_STOP, ps->ps_stop_txg);
	spa_lag_dir_set(ps->ps_lag, SLD_GENERATION, ps->ps_generation);
	spa_lag_dir_set(ps->ps_lag, SLD_EPOCH, ps->ps_epoch);
	spa_lag_dir_set(ps->ps_lag, SLD_SET_GUID, set_guid);
}

/*
 * Property sync, in syncing context of txg S (pass 1, from a sync task).
 * Turning on starts the capture at S + TXG_CONCURRENT_STATES: the open txgs
 * S+1 and S+2 may have allocated before this switch, so they are never
 * recorded.  Turning off stops new ALLOC records after S and moves to
 * DRAINING; the drain ends in a later txg when nothing is left.
 *
 * The open-context check of the property can race with a vdev change, so
 * the eligibility is checked again here; an ineligible pool keeps
 * parityset=off.
 */
void
spa_parity_prop_sync(spa_t *spa, uint64_t on, dmu_tx_t *tx)
{
	spa_parity_t *ps = spa->spa_parity;
	uint64_t S = dmu_tx_get_txg(tx);
	parity_state_t state;

	ASSERT(dmu_tx_is_syncing(tx));
	ASSERT(spa_writeable(spa));
	ASSERT(ps == NULL || !ps->ps_readonly);

	/*
	 * spa_create() applies the properties before syncing starts, and has
	 * checked the vdev tree itself.  Otherwise spa_sync() holds the config
	 * lock as reader.
	 */
	if (on && spa->spa_sync_on &&
	    spa_parity_eligible_root(spa->spa_root_vdev) != 0) {
		uint64_t zero = 0;

		cmn_err(CE_WARN, "parity: pool '%s' no longer fits a pool "
		    "set (single disks or mirrors); parity stays off",
		    spa_name(spa));
		VERIFY0(zap_update(spa->spa_meta_objset,
		    spa->spa_pool_props_object,
		    zpool_prop_to_name(ZPOOL_PROP_PARITYSET), sizeof (uint64_t),
		    1, &zero, tx));
		on = 0;
	}

	/*
	 * A checkpoint and held frees must not exist together: a held free of a
	 * checkpointed block would give its space back to the allocator at the
	 * release.  A checkpoint sync task earlier in this txg passed its check
	 * before parity was on, so test again here.
	 */
	if (on && spa->spa_sync_on && (spa_has_checkpoint(spa) ||
	    spa_feature_is_active(spa, SPA_FEATURE_POOL_CHECKPOINT))) {
		uint64_t zero = 0;

		cmn_err(CE_WARN, "parity: pool '%s' has a checkpoint; parity "
		    "stays off", spa_name(spa));
		VERIFY0(zap_update(spa->spa_meta_objset,
		    spa->spa_pool_props_object,
		    zpool_prop_to_name(ZPOOL_PROP_PARITYSET), sizeof (uint64_t),
		    1, &zero, tx));
		on = 0;
	}

	/*
	 * A healing resilver copies only live blocks, so held frees cannot
	 * start while one runs (see parity_resilver_pending()).  A replace
	 * that started after the open-context check is one, if the pool was
	 * not busy then.  Once parity is on, every replace is a sequential
	 * rebuild.
	 */
	if (on && spa->spa_sync_on && (ps == NULL ||
	    ps->ps_state == PARITY_OFF) && parity_resilver_pending(spa)) {
		uint64_t zero = 0;

		cmn_err(CE_WARN, "parity: pool '%s' is resilvering; parity "
		    "stays off", spa_name(spa));
		VERIFY0(zap_update(spa->spa_meta_objset,
		    spa->spa_pool_props_object,
		    zpool_prop_to_name(ZPOOL_PROP_PARITYSET), sizeof (uint64_t),
		    1, &zero, tx));
		on = 0;
	}

	/*
	 * An import with the override did not load the records on disk,
	 * so records cannot start again: a second store would collide with the
	 * first.  The set must DROP the old records first.
	 */
	if (on && ps != NULL && ps->ps_unreadable) {
		uint64_t zero = 0;

		cmn_err(CE_WARN, "parity: pool '%s' was imported without its "
		    "records; parity stays off", spa_name(spa));
		VERIFY0(zap_update(spa->spa_meta_objset,
		    spa->spa_pool_props_object,
		    zpool_prop_to_name(ZPOOL_PROP_PARITYSET), sizeof (uint64_t),
		    1, &zero, tx));
		on = 0;
	}

	if (ps == NULL) {
		if (!on)
			return;
		ps = parity_create(spa);
	}

	mutex_enter(&ps->ps_lock);
	ps->ps_prop_on = (on != 0);
	if (on && ps->ps_state == PARITY_OFF) {
		ps->ps_capture_from_txg = S + TXG_CONCURRENT_STATES;
		ps->ps_start_txg = ps->ps_capture_from_txg;
		ps->ps_stop_txg = 0;
		spa_parity_epoch_renew(ps);	/* a new epoch id too */
		membar_producer();
		ps->ps_state = PARITY_ON;
	} else if (!on && ps->ps_state == PARITY_ON) {
		/*
		 * The open txgs S+1 and S+2 may already hold entries captured
		 * while ON; they stay whole.  So each txg is captured
		 * completely or not at all, as at the start.
		 */
		ps->ps_stop_txg = S + TXG_CONCURRENT_STATES - 1;
		membar_producer();
		ps->ps_state = PARITY_DRAINING;
	}
	state = ps->ps_state;
	mutex_exit(&ps->ps_lock);
	/*
	 * The feature becomes active with the property.  At pool creation
	 * the property syncs before the features are enabled; then the first
	 * record creates the directory, in the first txgs of the pool.
	 */
	if (on && spa_feature_is_enabled(spa, SPA_FEATURE_PARITY_RECORDS))
		spa_lag_dir_create(ps->ps_lag, tx);

	/*
	 * On during a drain: the frees between the stop and now were not
	 * recorded, so the old records cannot go on.  Drop them; a new epoch
	 * follows (the property is on), and the outside resyncs the pool.
	 */
	if (on && state == PARITY_DRAINING)
		parity_start_drop(ps, tx, PARITY_DROP_USER);
	parity_dir_update(ps);
}

static boolean_t parity_load_bp_verified(spa_t *, const blkptr_t *);

/*
 * An entry of the newest record that is not in the block tree any more was
 * freed after its txg.  If that free was held, the FREE entry has the final BP
 * of the block (a free reads the BP from the tree).  Match the lost entries
 * with the FREE entries of the records from the newest on.  If none matches,
 * the bytes at DVA[0] decide whether the recorded checksum remains usable.
 * An unconfirmed checksum gets an explicit zero-checksum fix-up.
 */
static uint64_t
parity_match_frees(spa_parity_t *ps, spa_lag_rec_t *first, list_t *lostl)
{
	avl_tree_t frees;
	spa_lag_rec_t *lr;
	parity_pend_t *lp, *f;
	uint64_t *buf, unmatched = 0;

	avl_create(&frees, parity_pend_compare, sizeof (parity_pend_t),
	    offsetof(parity_pend_t, pp_node));
	buf = vmem_alloc(PARITY_FLUSH_CHUNK * PARITY_ENTRY_BYTES, KM_SLEEP);
	for (lr = first; lr != NULL; lr = spa_lag_rec_next(ps->ps_lag, lr)) {
		uint64_t n = lr->lr_phys.pr_nentries;

		for (uint64_t base = 0; base < n; base += PARITY_FLUSH_CHUNK) {
			uint64_t cnt = MIN(PARITY_FLUSH_CHUNK, n - base);

			if (spa_lag_rec_read(ps->ps_lag, lr, base, cnt,
			    buf) != 0)
				break;
			for (uint64_t j = 0; j < cnt; j++) {
				const uint64_t *w =
				    &buf[j * PARITY_ENTRY_WORDS];
				const blkptr_t *bp =
				    (const blkptr_t *)&w[PARITY_EW_BP];

				if (w[PARITY_EW_KIND] == PARITY_KIND_FREE &&
				    !BP_IS_HOLE(bp))
					parity_map_add(&frees, bp,
					    PARITY_MAP_BORN);
			}
		}
	}
	vmem_free(buf, PARITY_FLUSH_CHUNK * PARITY_ENTRY_BYTES);

	for (lp = list_head(lostl); lp != NULL; lp = list_next(lostl, lp)) {
		parity_pend_t *pp;

		f = avl_find(&frees, lp, NULL);
		if (f == NULL) {
			/* checksum off or noparity: nothing to verify */
			if (BP_GET_CHECKSUM(&lp->pp_bp) == ZIO_CHECKSUM_OFF ||
			    BP_GET_CHECKSUM(&lp->pp_bp) ==
			    ZIO_CHECKSUM_NOPARITY)
				continue;
			if (parity_load_bp_verified(ps->ps_spa, &lp->pp_bp))
				continue;
			/*
			 * The final BP is gone, and its old checksum cannot be
			 * confirmed.  Keep the ALLOC with an unknown checksum.
			 */
			pp = parity_pend_alloc();
			pp->pp_vdev = lp->pp_vdev;
			pp->pp_offset = lp->pp_offset;
			pp->pp_bp = lp->pp_bp;
			memset(&pp->pp_bp.blk_cksum, 0,
			    sizeof (pp->pp_bp.blk_cksum));
			pp->pp_kind = PARITY_KIND_FIXUP;
			pp->pp_slot = lp->pp_slot;
			list_insert_tail(&ps->ps_late, pp);
			ps->ps_nlate++;
			continue;
		}
		if (ZIO_CHECKSUM_EQUAL(f->pp_bp.blk_cksum, lp->pp_bp.blk_cksum))
			continue;
		pp = parity_pend_alloc();
		pp->pp_vdev = lp->pp_vdev;
		pp->pp_offset = lp->pp_offset;
		/* The final BP, with the DVAs of the entry. */
		pp->pp_bp = f->pp_bp;
		memcpy(pp->pp_bp.blk_dva, lp->pp_bp.blk_dva,
		    sizeof (pp->pp_bp.blk_dva));
		pp->pp_kind = PARITY_KIND_FIXUP;
		pp->pp_slot = lp->pp_slot;
		list_insert_tail(&ps->ps_late, pp);
		ps->ps_nlate++;
	}
	parity_map_destroy(&frees);
	return (unmatched);
}

/*
 * Check the recorded BP against the bytes still at DVA[0].  A block that was
 * freed in a later sync pass is not overwritten until the next txg, so those
 * bytes can tell us whether its recorded checksum is still useful.
 */
static boolean_t
parity_load_bp_verified(spa_t *spa, const blkptr_t *bp)
{
	vdev_t *leaf;
	abd_t *abd;
	void *buf;
	uint64_t size = BP_GET_PSIZE(bp);
	uint64_t off = VDEV_LABEL_START_SIZE +
	    DVA_GET_OFFSET(&bp->blk_dva[0]);
	int error = SET_ERROR(EIO);

	if (size == 0 || size > SPA_MAXBLOCKSIZE + SPA_OLD_GANGBLOCKSIZE ||
	    off + size < off)
		return (B_FALSE);

	buf = vmem_alloc(size, KM_SLEEP);
	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	leaf = spa_parity_data_leaf(spa);
	if (leaf != NULL && vdev_readable(leaf) && off + size <=
	    leaf->vdev_psize) {
		abd = abd_alloc_linear(size, B_FALSE);
		error = zio_wait(zio_read_phys(NULL, leaf, off, size, abd,
		    ZIO_CHECKSUM_OFF, NULL, NULL, ZIO_PRIORITY_SYNC_READ,
		    ZIO_FLAG_CANFAIL | ZIO_FLAG_DONT_PROPAGATE, B_FALSE));
		if (error == 0) {
			abd_copy_to_buf(buf, abd, size);
			error = spa_parity_bp_verify(spa, bp, buf, size);
		}
		abd_free(abd);
	}
	spa_config_exit(spa, SCL_STATE, FTAG);
	vmem_free(buf, size);
	return (error == 0);
}

/*
 * Load: the fix-ups of the newest record N were only in memory (ps_late),
 * and the next record has not been written.  Rebuild them: every entry of N
 * whose BP differs from the BP in the block tree gets a fix-up.  An entry
 * absent from the tree may have been freed in a later sync pass of N.  The
 * deferred-free list keeps only its DVA, so match a later FREE or test the
 * bytes before deciding whether the old checksum is still usable.
 */
static int
parity_load_fixups(spa_parity_t *ps)
{
	spa_t *spa = ps->ps_spa;
	spa_lag_rec_t *lr = spa_lag_rec_last(ps->ps_lag);
	uint64_t txg, n, chunk, *buf, lost = 0, unmatched = 0;
	avl_tree_t map;
	list_t lostl;
	parity_pend_t *lp;
	int error;

	if (lr == NULL)
		return (0);
	txg = lr->lr_txg;
	n = lr->lr_phys.pr_nentries;
	error = parity_born_map(spa, txg, &map);
	if (error != 0)
		return (error);
	list_create(&lostl, sizeof (parity_pend_t),
	    offsetof(parity_pend_t, pp_lnode));

	chunk = MIN(MAX(n, 1), PARITY_FLUSH_CHUNK);
	buf = vmem_alloc(chunk * PARITY_ENTRY_WORDS * sizeof (uint64_t),
	    KM_SLEEP);
	for (uint64_t base = 0; base < n && error == 0; base += chunk) {
		uint64_t cnt = MIN(chunk, n - base);

		error = spa_lag_rec_read(ps->ps_lag, lr, base, cnt, buf);
		for (uint64_t j = 0; j < cnt && error == 0; j++) {
			const uint64_t *w = &buf[j * PARITY_ENTRY_WORDS];
			const blkptr_t *bp =
			    (const blkptr_t *)&w[PARITY_EW_BP];
			parity_pend_t search, *f, *pp;

			if (w[PARITY_EW_KIND] != PARITY_KIND_ALLOC ||
			    w[PARITY_EW_TXG] != txg ||
			    !parity_born_wanted(bp, txg))
				continue;
			search.pp_vdev = DVA_GET_VDEV(&bp->blk_dva[0]);
			search.pp_offset = DVA_GET_OFFSET(&bp->blk_dva[0]);
			f = avl_find(&map, &search, NULL);
			if (f == NULL || f->pp_kind == PARITY_MAP_FREED) {
				/*
				 * Missing entries have no final BP.
				 * A deferred free keeps only DVA.
				 * Try the held FREE, then check bytes.
				 */
				lp = parity_pend_alloc();
				lp->pp_vdev = search.pp_vdev;
				lp->pp_offset = search.pp_offset;
				lp->pp_bp = *bp;
				lp->pp_kind = PARITY_KIND_ALLOC;
				lp->pp_slot = base + j;
				list_insert_tail(&lostl, lp);
				lost++;
				continue;
			}
			/*
			 * A fix-up only from the exact block of txg: not from a
			 * free list (no checksum kept) or a later dedup
			 * reference (another logical birth, maybe more DVAs).
			 */
			if (f->pp_kind != PARITY_MAP_BORN ||
			    BP_GET_LOGICAL_BIRTH(&f->pp_bp) != txg ||
			    !parity_same_dvas(&f->pp_bp, bp) ||
			    memcmp(&f->pp_bp, bp, sizeof (*bp)) == 0)
				continue;
			pp = parity_pend_alloc();
			pp->pp_vdev = search.pp_vdev;
			pp->pp_offset = search.pp_offset;
			pp->pp_bp = f->pp_bp;
			pp->pp_kind = PARITY_KIND_FIXUP;
			pp->pp_slot = base + j;
			list_insert_tail(&ps->ps_late, pp);
			ps->ps_nlate++;
		}
	}
	vmem_free(buf, chunk * PARITY_ENTRY_WORDS * sizeof (uint64_t));
	parity_map_destroy(&map);
	if (error == 0 && lost != 0)
		unmatched = parity_match_frees(ps, lr, &lostl);
	while ((lp = list_remove_head(&lostl)) != NULL)
		parity_pend_free(lp);
	list_destroy(&lostl);
	if (error != 0) {
		parity_list_free(&ps->ps_late, &ps->ps_nlate);
		return (error);
	}
	/* Some recorded checksums may be out of date: say so in the next. */
	if (unmatched != 0)
		ps->ps_fixups_incomplete = B_TRUE;

	ps->ps_late_txg = txg;
	ps->ps_stat_lost += lost;
	if (lost != 0) {
		zfs_dbgmsg("parity: pool '%s': %llu entries of txg %llu are "
		    "not in the tree (last synced txg %llu)", spa_name(spa),
		    (u_longlong_t)lost, (u_longlong_t)txg,
		    (u_longlong_t)spa->spa_ubsync.ub_txg);
		VERIFY(!(zfs_parity_verify & PARITY_VERIFY_FINAL) ||
		    spa->spa_ubsync.ub_txg != txg || parity_map_partial(spa) ||
		    unmatched == 0);
	}
	zfs_dbgmsg("parity: pool '%s': %llu fix-ups of txg %llu rebuilt",
	    spa_name(spa), (u_longlong_t)ps->ps_nlate, (u_longlong_t)txg);
	return (0);
}

/*
 * Verifier bit 3 at load: the held ranges of the unreleased records.
 */
static void
parity_vheld_load(spa_parity_t *ps)
{
	spa_lag_rec_t *lr;
	uint64_t *buf;

	buf = vmem_alloc(PARITY_FLUSH_CHUNK * PARITY_ENTRY_BYTES, KM_SLEEP);
	/*
	 * parity_vheld_add() sizes each DVA with vdev_lookup_top(), which
	 * needs a config lock.  The load holds none.
	 */
	spa_config_enter(ps->ps_spa, SCL_VDEV, FTAG, RW_READER);
	for (lr = spa_lag_rec_first(ps->ps_lag); lr != NULL;
	    lr = spa_lag_rec_next(ps->ps_lag, lr)) {
		uint64_t n = lr->lr_phys.pr_nentries;

		for (uint64_t base = lr->lr_phys.pr_rel_index; base < n;
		    base += PARITY_FLUSH_CHUNK) {
			uint64_t cnt = MIN(PARITY_FLUSH_CHUNK, n - base);

			if (spa_lag_rec_read(ps->ps_lag, lr, base, cnt,
			    buf) != 0)
				break;
			for (uint64_t j = 0; j < cnt; j++) {
				const uint64_t *w =
				    &buf[j * PARITY_ENTRY_WORDS];
				const blkptr_t *bp =
				    (const blkptr_t *)&w[PARITY_EW_BP];

				if (w[PARITY_EW_KIND] != PARITY_KIND_FREE ||
				    BP_IS_HOLE(bp) ||
				    BP_GET_TYPE(bp) == DMU_OT_INTENT_LOG)
					continue;
				for (int d = 0; d < BP_GET_NDVAS(bp); d++)
					parity_vheld_add(ps, &bp->blk_dva[d]);
			}
		}
	}
	spa_config_exit(ps->ps_spa, SCL_VDEV, FTAG);
	vmem_free(buf, PARITY_FLUSH_CHUNK * PARITY_ENTRY_BYTES);
}

/*
 * Read-only import: load the directory entry, the state and the
 * record headers, so that INFO, RECORDS, READ, MAP and LABEL work on a member
 * that was moved to another host, or that the daemon checks before it uses
 * the import override.  Nothing is armed: the capture window stays closed
 * (ps_capture_from_txg = UINT64_MAX), no free is held, nothing is released,
 * and nothing is written (ps_readonly; the write paths assert it).  The
 * epoch is made in memory only.  A pool that has no records, or whose records
 * cannot be read, has no state here, and the import goes on: it must never
 * fail because of the records.
 */
static int
parity_load_readonly(spa_t *spa)
{
	spa_parity_t *ps;
	spa_lag_summary_t sls;
	parity_state_t state = PARITY_OFF;
	boolean_t rewound;
	int error;

	ASSERT(!spa_writeable(spa));
	if (zap_contains(spa->spa_meta_objset, DMU_POOL_DIRECTORY_OBJECT,
	    PARITY_DIR_NAME) != 0)
		return (0);

	rewound = (spa->spa_uberblock.ub_txg < spa->spa_load_latest_ub_txg);
	ps = parity_create(spa);
	ps->ps_readonly = B_TRUE;
	error = spa_lag_load(ps->ps_lag);
	if (error == 0) {
		uint64_t word = spa_lag_dir_get(ps->ps_lag, SLD_STATE);

		state = word & PARITY_DIR_STATE_MASK;
		ps->ps_unprotected = ((word & PARITY_DIR_UNPROTECTED) != 0);
		if (state != PARITY_ON && state != PARITY_DRAINING &&
		    state != PARITY_DROPPING)
			error = SET_ERROR(ENOTSUP);
	}
	if (error != 0) {
		zfs_dbgmsg("parity: pool '%s': read-only import: the records "
		    "cannot be read (error %d)", spa_name(spa), error);
		spa_parity_unload(spa);
		return (0);
	}
	ps->ps_start_txg = spa_lag_dir_get(ps->ps_lag, SLD_START);
	ps->ps_stop_txg = spa_lag_dir_get(ps->ps_lag, SLD_STOP);
	ps->ps_generation = spa_lag_dir_get(ps->ps_lag, SLD_GENERATION);
	ps->ps_set_guid = spa_lag_dir_get(ps->ps_lag, SLD_SET_GUID);
	ps->ps_set_guid_dirty = B_FALSE;
	ps->ps_applied_txg = spa_lag_dir_get(ps->ps_lag, SLD_APPLIED);
	ps->ps_epoch = spa_lag_dir_get(ps->ps_lag, SLD_EPOCH);
	/* The counters come from the headers of the records. */
	spa_lag_summarize(ps->ps_lag, ps->ps_applied_txg, &sls);
	ps->ps_held_bytes = sls.sls_held_bytes;
	ps->ps_held_dspace = sls.sls_held_bytes;
	ps->ps_record_bytes = sls.sls_record_bytes;
	/* RECORDS reads the fix-ups of the newest record from memory. */
	(void) parity_load_fixups(ps);

	ps->ps_load_ub_txg = spa->spa_uberblock.ub_txg;
	ps->ps_load_latest_txg = spa->spa_load_latest_ub_txg;
	ps->ps_rewound = rewound;
	if (ps->ps_epoch == 0 || rewound) {
		if (rewound)
			ps->ps_generation++;
		ps->ps_epoch = spa_parity_new_epoch();
	}
	spa_parity_lineage_note(ps, &spa->spa_uberblock);

	/* The capture window stays closed: no hook records anything. */
	ASSERT3U(ps->ps_capture_from_txg, ==, UINT64_MAX);
	membar_producer();
	ps->ps_state = state;
	return (0);
}

/*
 * Load.  A pool with records loads them (they hold the state of the pool
 * set, and later its held frees); a pool whose property is on, whose records
 * are still draining, or that is imported with parityset=on (a rebuilt
 * pool) arms the capture before the claim txg, so the claimed log blocks are
 * recorded and nothing has been written yet.
 *
 * A read-only import loads the records but arms nothing: see
 * parity_load_readonly().
 *
 * The random epoch id lives in the state of the directory entry.
 * A pool that has none (records of version 1, or records that start now)
 * gets one.  A pool that loaded an uberblock older than the newest one on
 * disk (a rewind import, or a retry after the newest one failed to load) gets
 * a new epoch, because its history changed: records and txgs that the
 * daemon knows may be gone, and the same txg numbers will hold other data.
 *
 * Import override ("zpool import --parity-override", or the module parameter
 * zfs_parity_import_override = 1):
 * records that cannot be read, or a state that this code does not know, do
 * not stop the import.  The pool then imports read-write with the records
 * switched off for this import (INFO shows it as unprotected) and the old
 * records stay on disk, untouched.  The set must resync this pool.  Without
 * the override such a pool does not import read-write: its held frees would
 * be lost.
 */
int
spa_ld_parity_load(spa_t *spa)
{
	objset_t *mos = spa->spa_meta_objset;
	spa_parity_t *ps;
	parity_state_t state = PARITY_OFF;
	uint64_t on = 0, leaked = 0;
	boolean_t dir, rewound;
	int error;

	if (spa_feature_is_enabled(spa, SPA_FEATURE_PARITY_RECORDS))
		(void) zap_lookup(mos, DMU_POOL_DIRECTORY_OBJECT,
		    PARITY_LEAKED_NAME, sizeof (uint64_t), 1, &leaked);
	atomic_store_64(&spa->spa_parity_leaked, leaked);

	if (!spa_writeable(spa))
		return (parity_load_readonly(spa));
	if (spa->spa_pool_props_object != 0) {
		(void) zap_lookup(mos, spa->spa_pool_props_object,
		    zpool_prop_to_name(ZPOOL_PROP_PARITYSET), sizeof (uint64_t),
		    1, &on);
	}
	dir = (zap_contains(mos, DMU_POOL_DIRECTORY_OBJECT,
	    PARITY_DIR_NAME) == 0);
	if (!on && !dir && !spa->spa_parity_import_on)
		return (0);
	if (zfs_parity_test_inject & 16) {
		zfs_parity_test_inject &= ~16;
		return (SET_ERROR(EIO));
	}

	/*
	 * The import asks for a set member that has no records yet.  The
	 * feature must be there, and the pool must fit; else the import fails
	 * (it must not import without records, because parity would then miss
	 * the first writes).
	 */
	if (spa->spa_parity_import_on && !dir) {
		if (!spa_feature_is_enabled(spa, SPA_FEATURE_PARITY_RECORDS)) {
			zfs_dbgmsg("parity: pool '%s': import with "
			    "parityset=on, but the parity_records feature is "
			    "not enabled",
			    spa_name(spa));
			return (SET_ERROR(ENOTSUP));
		}
		error = spa_parity_eligible_root(spa->spa_root_vdev);
		if (error != 0)
			return (error);
	}

	rewound = (spa->spa_uberblock.ub_txg < spa->spa_load_latest_ub_txg);

	ps = parity_create(spa);
	ps->ps_prop_on = (on != 0);
	if (dir) {
		spa_lag_summary_t sls;

		error = spa_lag_load(ps->ps_lag);
		if (error == 0) {
			uint64_t word = spa_lag_dir_get(ps->ps_lag, SLD_STATE);

			state = word & PARITY_DIR_STATE_MASK;
			ps->ps_unprotected =
			    ((word & PARITY_DIR_UNPROTECTED) != 0);
			if (state != PARITY_ON && state != PARITY_DRAINING &&
			    state != PARITY_DROPPING)
				error = SET_ERROR(ENOTSUP);
		}
		if (error != 0) {
			spa_parity_unload(spa);
			if (!zfs_parity_import_override &&
			    !(spa->spa_import_flags &
			    ZFS_IMPORT_PARITY_OVERRIDE)) {
				cmn_err(CE_WARN, "parity: pool '%s': the "
				    "records cannot be used (error %d); "
				    "import it with 'zpool import "
				    "--parity-override' to drop them",
				    spa_name(spa), error);
				return (error);
			}
			cmn_err(CE_WARN, "parity: pool '%s': the records "
			    "cannot be used (error %d); imported with the "
			    "override, so no records are kept: the parity set "
			    "must resync this pool", spa_name(spa), error);
			/*
			 * A fresh state, with the capture off.  Keep the
			 * directory entry and the index object, and end
			 * in a drop: the first sync then runs the normal
			 * retire path, which removes them and returns the
			 * feature to enabled.  Without them each later
			 * import would fail again.
			 */
			ps = parity_create(spa);
			ps->ps_unreadable = B_TRUE;
			/*
			 * The old generation is unknown.  The txg is
			 * not one that a stale daemon can match by
			 * chance, as 1 is.
			 */
			ps->ps_generation = spa->spa_uberblock.ub_txg;
			ps->ps_epoch = spa_parity_new_epoch();
			ps->ps_load_ub_txg = spa->spa_uberblock.ub_txg;
			ps->ps_load_latest_txg = spa->spa_load_latest_ub_txg;
			ps->ps_rewound = rewound;
			spa_parity_lineage_note(ps, &spa->spa_uberblock);
			if (spa_lag_adopt(ps->ps_lag) == 0) {
				membar_producer();
				ps->ps_state = PARITY_DROPPING;
			}
			return (0);
		}
		ps->ps_start_txg = spa_lag_dir_get(ps->ps_lag, SLD_START);
		ps->ps_stop_txg = spa_lag_dir_get(ps->ps_lag, SLD_STOP);
		ps->ps_generation = spa_lag_dir_get(ps->ps_lag, SLD_GENERATION);
		ps->ps_set_guid = spa_lag_dir_get(ps->ps_lag, SLD_SET_GUID);
		ps->ps_set_guid_dirty = B_FALSE;
		ps->ps_applied_txg = spa_lag_dir_get(ps->ps_lag, SLD_APPLIED);
		ps->ps_epoch = spa_lag_dir_get(ps->ps_lag, SLD_EPOCH);
		/* The counters come from the headers of the records. */
		spa_lag_summarize(ps->ps_lag, ps->ps_applied_txg, &sls);
		ps->ps_held_bytes = sls.sls_held_bytes;
		ps->ps_held_dspace = sls.sls_held_bytes;
		ps->ps_record_bytes = sls.sls_record_bytes;
		if (zfs_parity_verify & PARITY_VERIFY_RELEASE)
			parity_vheld_load(ps);
		/*
		 * Without the fix-ups, some recorded checksums of the newest
		 * txg are out of date: a rebuild of such a block then fails
		 * its checksum and is refused, never accepted wrongly.  So a
		 * read error here must not stop the import.
		 */
		error = parity_load_fixups(ps);
		if (error != 0) {
			cmn_err(CE_WARN, "parity: pool '%s': fix-ups of the "
			    "newest record not rebuilt (error %d)",
			    spa_name(spa), error);
			ps->ps_fixups_incomplete = B_TRUE;
		}
	} else {
		state = PARITY_ON;
		ps->ps_start_txg = spa_first_txg(spa);
		ps->ps_generation = 1;
	}

	/*
	 * The vdev tree should still fit (the gates refuse every change that
	 * breaks it).  If it does not, keep recording anyway: silence would
	 * let parity fall out of step without notice.
	 */
	if (spa_parity_eligible_root(spa->spa_root_vdev) != 0) {
		cmn_err(CE_WARN, "parity: pool '%s' is in a parity set but its "
		    "data vdevs are no longer single disks or mirrors",
		    spa_name(spa));
	}

	ps->ps_load_ub_txg = spa->spa_uberblock.ub_txg;
	ps->ps_load_latest_txg = spa->spa_load_latest_ub_txg;
	ps->ps_rewound = rewound;
	if (ps->ps_epoch == 0 || rewound) {
		/* The generation counts a change of history, not a version. */
		if (rewound && dir)
			ps->ps_generation++;
		ps->ps_epoch = spa_parity_new_epoch();
		zfs_dbgmsg("parity: pool '%s': new epoch %llx (generation "
		    "%llu%s)", spa_name(spa), (u_longlong_t)ps->ps_epoch,
		    (u_longlong_t)ps->ps_generation,
		    rewound ? ", loaded an older uberblock" : "");
	}
	spa_parity_lineage_note(ps, &spa->spa_uberblock);

	ps->ps_capture_from_txg = spa_first_txg(spa);
	membar_producer();
	ps->ps_state = state;
	parity_dir_update(ps);
	/*
	 * The held bytes count against the free space.  The import computed
	 * dspace before the records were loaded, so compute it again.
	 */
	spa_update_dspace(spa);
	return (0);
}

/*
 * For zdb: call cb for every MOS object of the records (index, records),
 * read from disk.  Returns ENOENT when there are none.
 */
int
spa_parity_mos_objs(spa_t *spa, void (*cb)(uint64_t))
{
	objset_t *mos = spa->spa_meta_objset;
	uint64_t dir[SPA_LAG_DIR_WORDS];
	zap_cursor_t zc;
	zap_attribute_t *za;
	int error;

	error = spa_lag_dir_read(mos, PARITY_DIR_NAME, dir);
	if (error != 0)
		return (error);
	cb(dir[SLD_INDEX]);

	za = zap_attribute_alloc();
	for (zap_cursor_init(&zc, mos, dir[SLD_INDEX]);
	    (error = zap_cursor_retrieve(&zc, za)) == 0;
	    zap_cursor_advance(&zc)) {
		spa_lag_rec_phys_t phys;

		if (*(uint64_t *)za->za_name == SPA_LAG_STATE_KEY)
			continue;
		error = zap_lookup_uint64(mos, dir[SLD_INDEX],
		    (uint64_t *)za->za_name, 1, sizeof (uint64_t),
		    SPA_LAG_REC_WORDS, &phys);
		if (error != 0)
			break;
		cb(phys.pr_obj);
	}
	zap_cursor_fini(&zc);
	zap_attribute_free(za);
	return (error == ENOENT ? 0 : error);
}

/*
 * The interface for the ioctls and for tests.
 */
spa_lag_t *
spa_parity_lag(spa_t *spa)
{
	spa_parity_t *ps = spa->spa_parity;

	return (ps != NULL ? ps->ps_lag : NULL);
}

/*
 * A snapshot.  The counters are read under ps_lock; the sums over the records
 * under the lock of the record tree.
 */
void
spa_parity_get_info(spa_t *spa, spa_parity_info_t *pi)
{
	spa_parity_t *ps = spa->spa_parity;
	spa_lag_summary_t sls;
	spa_lag_level_t level;

	memset(pi, 0, sizeof (*pi));
	pi->pi_state = PARITY_OFF;
	pi->pi_leaked_bytes = atomic_load_64(&spa->spa_parity_leaked);
	if (ps == NULL)
		return;

	mutex_enter(&ps->ps_lock);
	pi->pi_state = ps->ps_state;
	pi->pi_start_txg = ps->ps_start_txg;
	pi->pi_stop_txg = ps->ps_stop_txg;
	pi->pi_generation = ps->ps_generation;
	pi->pi_set_guid = ps->ps_set_guid;
	pi->pi_applied_txg = ps->ps_applied_txg;
	pi->pi_held_bytes = ps->ps_held_bytes;
	pi->pi_held_dspace = ps->ps_held_dspace;
	pi->pi_record_bytes = ps->ps_record_bytes;
	if (ps->ps_unprotected || ps->ps_unreadable)
		pi->pi_flags |= SPA_PARITY_F_UNPROTECTED;
	if (ps->ps_fixups_incomplete)
		pi->pi_flags |= SPA_PARITY_F_FIXUPS_INCOMPLETE;
	if (ps->ps_drop_request != PARITY_DROP_NONE)
		pi->pi_flags |= SPA_PARITY_F_DROP_PENDING;
	mutex_exit(&ps->ps_lock);

	spa_lag_summarize(ps->ps_lag, pi->pi_applied_txg, &sls);
	pi->pi_nrecs = sls.sls_nrecs;
	pi->pi_last_txg = sls.sls_last_txg;
	pi->pi_pending_bytes = sls.sls_pending_bytes;
	pi->pi_pending_txgs = sls.sls_pending_txgs;
	if (sls.sls_flags & SPA_LAG_REC_FIXUPS_INCOMPLETE)
		pi->pi_flags |= SPA_PARITY_F_FIXUPS_INCOMPLETE;
	if (pi->pi_state == PARITY_DROPPING)
		pi->pi_flags |= SPA_PARITY_F_DROP_PENDING;
	/*
	 * The oldest record, or with no record the larger of the txg after the
	 * applied one and the start.  The daemon's join baseline is the txg
	 * before it.  The history checks use spa_parity_history_txg().
	 */
	pi->pi_first_txg = (sls.sls_nrecs != 0) ? sls.sls_first_txg :
	    MAX(pi->pi_applied_txg + 1, pi->pi_start_txg);

	level = spa_lag_level(ps->ps_lag);
	pi->pi_throttle = (level == SPA_LAG_LEVEL_HARD) ?
	    SPA_PARITY_THROTTLE_HARD : (level == SPA_LAG_LEVEL_SOFT) ?
	    SPA_PARITY_THROTTLE_SOFT : SPA_PARITY_THROTTLE_NONE;
	pi->pi_max_record_bytes = spa_lag_max_records(spa);
	pi->pi_max_held_bytes = spa_lag_max_held(spa);
}

uint64_t
spa_parity_leaked_bytes(spa_t *spa)
{
	return (atomic_load_64(&spa->spa_parity_leaked));
}
ZFS_MODULE_PARAM(zfs_parity, zfs_parity_, verify, UINT, ZMOD_RW,
	"Bit mask of parity set debug checks (1: allocated ranges equal the "
	"captured ranges; 2: records on disk equal the capture; 4: recorded "
	"BPs equal the tree; 8: each held range is freed once), per txg");

ZFS_MODULE_PARAM(zfs_parity, zfs_parity_, allow_expand, INT, ZMOD_RW,
	"Allow online expansion while a parity set holds records");

ZFS_MODULE_PARAM(zfs_parity, zfs_parity_, release_limit, U64, ZMOD_RW,
	"Release at most this many record entries per txg");

ZFS_MODULE_PARAM(zfs_parity, zfs_parity_, test_inject, UINT, ZMOD_RW,
	"Test only: inject a fault so a verifier fires (1: capture, 2: VOID, "
	"4: BP refresh, 8: release, 16: fail one member load, "
	"32: fail record reads at release, 64: hold a repair write 3 s)");

ZFS_MODULE_PARAM(zfs_parity, zfs_parity_, suspend_release, INT, ZMOD_RW,
	"Test only: do not release parity records");

ZFS_MODULE_PARAM(zfs_parity, zfs_parity_, import_override, UINT, ZMOD_RW,
	"Import a parity set member read-write even when its records cannot be "
	"read (the set must resync it)");

ZFS_MODULE_PARAM(zfs_parity, zfs_parity_, test_auto_apply, U64, ZMOD_RW,
	"Test only: treat txgs older than this many txgs as applied (0: off)");

#if defined(_KERNEL)
EXPORT_SYMBOL(spa_parity_busy);
EXPORT_SYMBOL(spa_parity_txg_done);
EXPORT_SYMBOL(spa_parity_expand_blocked);
EXPORT_SYMBOL(spa_parity_eligible);
EXPORT_SYMBOL(spa_parity_eligible_root);
EXPORT_SYMBOL(spa_parity_checksum_refused);
EXPORT_SYMBOL(spa_parity_check_checksums);
EXPORT_SYMBOL(spa_parity_prop_sync);
EXPORT_SYMBOL(spa_ld_parity_load);
EXPORT_SYMBOL(spa_parity_unload);
EXPORT_SYMBOL(spa_parity_mos_objs);
EXPORT_SYMBOL(spa_parity_lag);
EXPORT_SYMBOL(spa_parity_get_info);
#endif
