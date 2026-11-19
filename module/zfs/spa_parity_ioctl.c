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
 * Parity set: the functions behind the ZFS_IOC_PARITY_* ioctls.
 *
 * The user-space parity service reads the records of a pool (RECORDS), reads
 * the recorded blocks (READ, VERIFY), asks for the allocated space (MAP) and
 * the label (LABEL), and tells the pool which records it applied (APPLIED).
 *
 * Every block pointer that comes from the caller is validated in full
 * before the disk is read (parity_ent_parse()): the entry must be a block
 * on the data device, the offset and the length must be inside the device,
 * the length must be the one the pool computes, and the checksum type must
 * be real.  A block with checksum off (or noparity) has no checksum to
 * test: READ hands out its bytes marked unverified, and VERIFY refuses it.
 * READ and VERIFY also check that the txg of the entry still has an
 * unreleased record, and, if the caller gives the index of the entry in its
 * record, that the record holds a block with the same DVAs.
 *
 * Records are protected against release while a call reads them (the
 * "pin": a read hold on ps_rec_rwlock; the release takes it with rw_tryenter
 * and skips the txg when it fails).
 */

#include <sys/spa_impl.h>
#include <sys/spa_lag.h>
#include <sys/spa_parity.h>
#include <sys/spa_parity_impl.h>
#include <sys/vdev_impl.h>
#include <sys/metaslab.h>
#include <sys/metaslab_impl.h>
#include <sys/range_tree.h>
#include <sys/zio.h>
#include <sys/zio_checksum.h>
#include <sys/abd.h>
#include <sys/uberblock_impl.h>
#include <sys/dmu_objset.h>
#include <sys/dmu_traverse.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_dataset.h>
#include <sys/zil.h>
#include <sys/zap.h>
#include <sys/fs/zfs.h>
#include <sys/sysevent/eventdefs.h>

/* RECORDS cursor: txg << 28 | index of the next entry in that record. */
#define	PARITY_CUR_SHIFT	28
#define	PARITY_CUR_MASK		((1ULL << PARITY_CUR_SHIFT) - 1)

#define	PARITY_RECORDS_MAX	16384
#define	PARITY_RANGES_MAX	262144
#define	PARITY_BLOCKS_MAX	16384
#define	PARITY_BLOCKS_STOP	EINTR
#define	PARITY_LABEL_BYTES	(2 * sizeof (vdev_label_t))

/* The READ reply says whether the bytes passed a checksum. */

/* Do not post the event more often than this (milliseconds). */
static uint_t zfs_parity_event_ms = 1000;

/*
 * Pin the records of the pool: the release of applied records is paused while
 * any pin is held.  A pin never waits for I/O in the sync thread: the release
 * takes the lock with rw_tryenter().
 */
static void
parity_pin(spa_parity_t *ps)
{
	rw_enter(&ps->ps_rec_rwlock, RW_READER);
}

static void
parity_unpin(spa_parity_t *ps)
{
	rw_exit(&ps->ps_rec_rwlock);
}

/*
 * The leaves that hold the data device's bytes: the disk itself, or the
 * children of a mirror, a replacing vdev or a spare vdev (these exist while a
 * disk is replaced or a hot spare is in use).
 */
#define	PARITY_MAX_LEAVES	(2 * SPA_PARITY_MAX_SIDES)
/* data columns of one pool (the eligibility check refuses more) */
#define	PARITY_MAX_COLS		SPA_PARITY_MAX_COLS
/* no ZPARITY_VDEV input: the first data column (old callers) */
#define	PARITY_VDEV_FIRST	SPA_PARITY_VDEV_FIRST

static int
parity_leaves(vdev_t *top, vdev_t **leaves)
{
	int n = 0;

	if (top->vdev_ops->vdev_op_leaf) {
		leaves[n++] = top;
	} else if (top->vdev_ops == &vdev_mirror_ops ||
	    top->vdev_ops == &vdev_replacing_ops ||
	    top->vdev_ops == &vdev_spare_ops) {
		for (uint64_t c = 0; c < top->vdev_children &&
		    n < PARITY_MAX_LEAVES; c++) {
			vdev_t *cvd = top->vdev_child[c];

			if (cvd->vdev_ops->vdev_op_leaf)
				leaves[n++] = cvd;
			/*
			 * a mirror side under a replace, or with a hot
			 * spare in use: its disks hold the side's bytes too
			 */
			else if (top->vdev_ops == &vdev_mirror_ops &&
			    (cvd->vdev_ops == &vdev_replacing_ops ||
			    cvd->vdev_ops == &vdev_spare_ops))
				for (uint64_t g = 0; g < cvd->vdev_children &&
				    n < PARITY_MAX_LEAVES; g++)
					if (cvd->vdev_child[g]->
					    vdev_ops->vdev_op_leaf)
						leaves[n++] =
						    cvd->vdev_child[g];
		}
	}
	return (n);
}

typedef struct parity_vdev_info {
	uint64_t	pvi_id;
	uint64_t	pvi_asize;
	uint64_t	pvi_ashift;
	uint64_t	pvi_gang_asize;
	uint64_t	pvi_ms_count;
	uint64_t	pvi_ms_shift;
	uint64_t	pvi_leaf_psize[PARITY_MAX_LEAVES];
	uint_t		pvi_nleaves;
} parity_vdev_info_t;

/*
 * the data column (top-level vdev) vdev, or the first data column for
 * PARITY_VDEV_FIRST (the callers from before columns).  NULL when vdev is
 * not a data column.  The caller holds a config lock.
 */
static vdev_t *
parity_col_tvd(spa_t *spa, uint64_t vdev)
{
	if (vdev == PARITY_VDEV_FIRST)
		return (spa_parity_data_tvd(spa));
	if (!spa_parity_is_data_vdev(spa, vdev))
		return (NULL);
	return (spa->spa_root_vdev->vdev_child[vdev]);
}

/* An optional ZPARITY_VDEV input: the column, or PARITY_VDEV_FIRST. */
uint64_t
spa_parity_ioc_vdev(nvlist_t *innvl)
{
	uint64_t vdev;

	if (innvl == NULL ||
	    nvlist_lookup_uint64(innvl, ZPARITY_VDEV, &vdev) != 0)
		return (PARITY_VDEV_FIRST);
	return (vdev);
}

/* The fields of a column; the caller holds a config lock. */
static void
parity_vdev_fill(vdev_t *top, parity_vdev_info_t *pvi)
{
	vdev_t *leaves[PARITY_MAX_LEAVES];

	pvi->pvi_id = top->vdev_id;
	pvi->pvi_asize = top->vdev_asize;
	pvi->pvi_ashift = top->vdev_ashift;
	pvi->pvi_gang_asize = vdev_gang_header_asize(top);
	pvi->pvi_ms_count = top->vdev_ms_count;
	pvi->pvi_ms_shift = top->vdev_ms_shift;
	pvi->pvi_nleaves = parity_leaves(top, leaves);
	for (uint_t i = 0; i < pvi->pvi_nleaves; i++)
		pvi->pvi_leaf_psize[i] = leaves[i]->vdev_psize;
}

/* Copy fields used after dropping the config lock. */
static int
parity_vdev_info(spa_t *spa, uint64_t vdev, parity_vdev_info_t *pvi)
{
	vdev_t *top;
	int error = SET_ERROR(ENXIO);

	memset(pvi, 0, sizeof (*pvi));
	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	top = parity_col_tvd(spa, vdev);
	if (top != NULL) {
		parity_vdev_fill(top, pvi);
		error = 0;
	}
	spa_config_exit(spa, SCL_STATE, FTAG);

	return (error);
}

/*
 * every data column of the pool, read once under one lock for a call
 * that meets the DVAs of all columns (RECORDS, the reclaim check).  NULL
 * when the pool has no data column.
 */
typedef struct parity_cols {
	uint_t			pc_n;
	parity_vdev_info_t	pc_v[PARITY_MAX_COLS];
} parity_cols_t;

static parity_cols_t *
parity_cols_load(spa_t *spa)
{
	parity_cols_t *pc = kmem_zalloc(sizeof (*pc), KM_SLEEP);
	vdev_t *rvd = spa->spa_root_vdev;

	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	for (uint64_t c = 0; c < rvd->vdev_children &&
	    pc->pc_n < PARITY_MAX_COLS; c++) {
		if (spa_parity_is_data_vdev(spa, c))
			parity_vdev_fill(rvd->vdev_child[c],
			    &pc->pc_v[pc->pc_n++]);
	}
	spa_config_exit(spa, SCL_STATE, FTAG);
	if (pc->pc_n == 0) {
		kmem_free(pc, sizeof (*pc));
		return (NULL);
	}
	return (pc);
}

static void
parity_cols_free(parity_cols_t *pc)
{
	if (pc != NULL)
		kmem_free(pc, sizeof (*pc));
}

/* The column of a DVA's vdev, or NULL (a log or another vdev). */
static const parity_vdev_info_t *
parity_col(const parity_cols_t *pc, uint64_t vdev)
{
	for (uint_t i = 0; i < pc->pc_n; i++)
		if (pc->pc_v[i].pvi_id == vdev)
			return (&pc->pc_v[i]);
	return (NULL);
}

/* The on-disk length of one DVA.  A gang DVA is the gang header. */
static uint64_t
parity_dva_len(const parity_vdev_info_t *pvi, const dva_t *dva)
{
	if (DVA_GET_GANG(dva))
		return (pvi->pvi_gang_asize);
	return (DVA_GET_ASIZE(dva));
}

static boolean_t
parity_same_dvas(const blkptr_t *a, const blkptr_t *b)
{
	if (BP_GET_NDVAS(a) != BP_GET_NDVAS(b))
		return (B_FALSE);
	for (int d = 0; d < BP_GET_NDVAS(a); d++) {
		const dva_t *x = &a->blk_dva[d], *y = &b->blk_dva[d];

		if (DVA_GET_VDEV(x) != DVA_GET_VDEV(y) ||
		    DVA_GET_OFFSET(x) != DVA_GET_OFFSET(y) ||
		    DVA_GET_ASIZE(x) != DVA_GET_ASIZE(y) ||
		    DVA_GET_GANG(x) != DVA_GET_GANG(y))
			return (B_FALSE);
	}
	return (B_TRUE);
}

static const char *
parity_state_name(uint64_t state)
{
	switch (state) {
	case PARITY_ON:
		return ("on");
	case PARITY_DRAINING:
		return ("draining");
	case PARITY_DROPPING:
		return ("dropping");
	default:
		return ("off");
	}
}

static const char *
parity_throttle_name(uint64_t level)
{
	switch (level) {
	case SPA_PARITY_THROTTLE_SOFT:
		return ("soft");
	case SPA_PARITY_THROTTLE_HARD:
		return ("hard");
	default:
		return ("none");
	}
}

/*
 * INFO.
 */
int
spa_parity_ioc_info(spa_t *spa, boolean_t have_txg, uint64_t txg, nvlist_t *out)
{
	spa_parity_lineage_t pl;
	spa_parity_info_t pi;
	spa_parity_t *ps = spa->spa_parity;
	vdev_t *top;
	vdev_t *leaves[PARITY_MAX_LEAVES];
	uint64_t asize = 0, leaf_guid = 0, leaf_psize = 0;
	char *path = NULL;
	boolean_t have_top = B_FALSE;
	uint64_t oldest_txg = 0, oldest_age = 0;

	/*
	 * A new epoch records from ps_capture_from_txg, which is the txg that
	 * turned parity on plus TXG_CONCURRENT_STATES.  A consumer starts from
	 * the txg before it (first_txg - 1) and asks for that txg's label.
	 * On an idle pool that txg can still be open or quiescing, and then
	 * it has no uberblock yet: LABEL fails with ENOENT.  Wait until it is
	 * synced, so that INFO never reports a baseline that has no label.
	 */
	if (ps != NULL && !have_txg && spa_writeable(spa)) {
		uint64_t from;

		mutex_enter(&ps->ps_lock);
		from = (ps->ps_state == PARITY_ON) ?
		    ps->ps_capture_from_txg : UINT64_MAX;
		mutex_exit(&ps->ps_lock);
		if (from != UINT64_MAX && from > 1 &&
		    spa_last_synced_txg(spa) + 1 < from) {
			int error = txg_wait_synced_flags(spa_get_dsl(spa),
			    from - 1, TXG_WAIT_SIGNAL | TXG_WAIT_SUSPEND);
			if (error != 0)
				return (error);
		}
	}

	/*
	 * spa_parity_get_info() also reads the lasting leaked count, which a
	 * pool keeps after its records are gone (no spa_parity then).
	 */
	spa_parity_get_info(spa, &pi);

	if (ps != NULL) {
		spa_lag_t *lag = ps->ps_lag;
		spa_lag_rec_t *lr;

		/*
		 * The oldest record that is not applied.  Its header holds
		 * the uberblock that was the newest when it was created: that
		 * time is when the lag of this record began.
		 */
		spa_lag_lock(lag);
		for (lr = spa_lag_rec_first(lag); lr != NULL;
		    lr = spa_lag_rec_next(lag, lr)) {
			if (lr->lr_txg > pi.pi_applied_txg) {
				const uberblock_t *ub = (const uberblock_t *)
				    lr->lr_phys.pr_prev_ub;
				uint64_t now = gethrestime_sec();

				oldest_txg = lr->lr_txg;
				if (ub->ub_timestamp != 0 &&
				    now > ub->ub_timestamp)
					oldest_age = now - ub->ub_timestamp;
				break;
			}
		}
		spa_lag_unlock(lag);
	}

	fnvlist_add_uint64(out, ZPARITY_VERSION, ZPARITY_VERSION_CURRENT);
	fnvlist_add_boolean_value(out, ZPARITY_ENABLED,
	    pi.pi_state == PARITY_ON);
	fnvlist_add_boolean_value(out, ZPARITY_ACTIVE,
	    pi.pi_state != PARITY_OFF || pi.pi_nrecs != 0);
	fnvlist_add_string(out, ZPARITY_STATE, parity_state_name(pi.pi_state));
	fnvlist_add_uint64(out, ZPARITY_START_TXG, pi.pi_start_txg);
	fnvlist_add_uint64(out, ZPARITY_STOP_TXG, pi.pi_stop_txg);
	fnvlist_add_uint64(out, ZPARITY_GENERATION, pi.pi_generation);
	fnvlist_add_uint64(out, ZPARITY_SET_GUID, pi.pi_set_guid);
	fnvlist_add_uint64(out, ZPARITY_FIRST_TXG, pi.pi_first_txg);
	fnvlist_add_uint64(out, ZPARITY_LAST_TXG, pi.pi_last_txg);
	fnvlist_add_uint64(out, ZPARITY_APPLIED_TXG, pi.pi_applied_txg);
	fnvlist_add_uint64(out, ZPARITY_PENDING_BYTES, pi.pi_pending_bytes);
	fnvlist_add_uint64(out, ZPARITY_HELD_BYTES, pi.pi_held_bytes);
	fnvlist_add_uint64(out, ZPARITY_HELD_DSPACE, pi.pi_held_dspace);
	fnvlist_add_uint64(out, ZPARITY_LEAKED_BYTES, pi.pi_leaked_bytes);
	fnvlist_add_uint64(out, ZPARITY_RECORD_BYTES, pi.pi_record_bytes);
	fnvlist_add_uint64(out, ZPARITY_MAX_RECORD_BYTES,
	    pi.pi_max_record_bytes);
	fnvlist_add_uint64(out, ZPARITY_MAX_HELD_BYTES, pi.pi_max_held_bytes);
	fnvlist_add_uint64(out, ZPARITY_NRECS, pi.pi_nrecs);
	fnvlist_add_string(out, ZPARITY_THROTTLE,
	    parity_throttle_name(pi.pi_throttle));
	fnvlist_add_boolean_value(out, ZPARITY_UNPROTECTED,
	    (pi.pi_flags & SPA_PARITY_F_UNPROTECTED) != 0);
	fnvlist_add_uint64(out, ZPARITY_OLDEST_TXG, oldest_txg);
	fnvlist_add_uint64(out, ZPARITY_OLDEST_AGE, oldest_age);
	fnvlist_add_uint64(out, ZPARITY_POOL_GUID, spa_guid(spa));
	fnvlist_add_uint64(out, ZPARITY_BLOCKS_WALKS,
	    atomic_load_64(&spa->spa_parity_blocks_walks));

	/* The history of the pool (epoch and lineage), and the repair table. */
	if (ps != NULL &&
	    spa_parity_lineage(spa, have_txg ? txg : 0, &pl) == 0) {
		fnvlist_add_uint64(out, ZPARITY_EPOCH, pl.pl_epoch);
		fnvlist_add_uint64(out, ZPARITY_LINEAGE_TXG,
		    pl.pl_last_synced_txg);
		fnvlist_add_uint64_array(out, ZPARITY_LINEAGE_CKSUM,
		    pl.pl_last_root_cksum, 4);
		fnvlist_add_boolean_value(out, ZPARITY_REWOUND,
		    pl.pl_rewound);
		fnvlist_add_uint64(out, ZPARITY_DATA_VDEV, pl.pl_data_vdev_id);
		fnvlist_add_uint64(out, ZPARITY_DATA_LEAF_GUID,
		    pl.pl_data_leaf_guid);
		if (have_txg && pl.pl_ub_known) {
			fnvlist_add_uint64_array(out, ZPARITY_TXG_CKSUM,
			    pl.pl_ub_root_cksum, 4);
		}
		fnvlist_add_uint64(out, ZPARITY_REPAIR_PENDING,
		    spa_parity_repair_npending(spa));
	}

	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	/*
	 * every data column with its leaves.  A leaf is in sync when its
	 * DTL is empty: a mirror side under resilver is not, and the service
	 * must not read it.
	 */
	vdev_t *rvd = spa->spa_root_vdev;
	uint64_t ncols_max = rvd->vdev_children;
	nvlist_t **cols = (ncols_max > 0) ?
	    kmem_zalloc(ncols_max * sizeof (nvlist_t *), KM_SLEEP) : NULL;
	uint_t ncols = 0;
	for (uint64_t c = 0; c < ncols_max; c++) {
		vdev_t *tvd = rvd->vdev_child[c];
		vdev_t *lv[PARITY_MAX_LEAVES];
		uint64_t lg[PARITY_MAX_LEAVES], ls[PARITY_MAX_LEAVES];
		boolean_t li[PARITY_MAX_LEAVES];
		const char *lp[PARITY_MAX_LEAVES];
		nvlist_t *cv;
		int nl;

		if (!spa_parity_is_data_vdev(spa, c))
			continue;
		nl = parity_leaves(tvd, lv);
		for (int i = 0; i < nl; i++) {
			lg[i] = lv[i]->vdev_guid;
			ls[i] = lv[i]->vdev_psize;
			li[i] = vdev_dtl_empty(lv[i], DTL_MISSING) &&
			    vdev_readable(lv[i]);
			lp[i] = (lv[i]->vdev_path != NULL) ?
			    lv[i]->vdev_path : "";
		}
		cv = fnvlist_alloc();
		fnvlist_add_uint64(cv, ZPARITY_COL_VDEV, tvd->vdev_id);
		fnvlist_add_uint64(cv, ZPARITY_COL_GUID, tvd->vdev_guid);
		fnvlist_add_string(cv, ZPARITY_COL_TYPE,
		    tvd->vdev_ops->vdev_op_type);
		fnvlist_add_uint64(cv, ZPARITY_ASIZE, tvd->vdev_asize);
		fnvlist_add_uint64(cv, ZPARITY_COL_ASHIFT, tvd->vdev_ashift);
		fnvlist_add_uint64_array(cv, ZPARITY_COL_LEAF_GUIDS, lg, nl);
		fnvlist_add_uint64_array(cv, ZPARITY_COL_LEAF_SIZES, ls, nl);
		fnvlist_add_boolean_array(cv, ZPARITY_COL_LEAF_INSYNC, li, nl);
		fnvlist_add_string_array(cv, ZPARITY_COL_LEAF_PATHS, lp, nl);
		cols[ncols++] = cv;
	}
	top = spa_parity_data_tvd(spa);
	if (top != NULL) {
		int n = parity_leaves(top, leaves);

		have_top = B_TRUE;
		asize = top->vdev_asize;
		if (n > 0) {
			leaf_guid = leaves[0]->vdev_guid;
			leaf_psize = leaves[0]->vdev_psize;
			if (leaves[0]->vdev_path != NULL)
				path = kmem_strdup(leaves[0]->vdev_path);
		}
	}
	spa_config_exit(spa, SCL_STATE, FTAG);
	if (cols != NULL) {
		fnvlist_add_nvlist_array(out, ZPARITY_COLUMNS,
		    (const nvlist_t * const *)cols, ncols);
		for (uint_t i = 0; i < ncols; i++)
			fnvlist_free(cols[i]);
		kmem_free(cols, ncols_max * sizeof (nvlist_t *));
	}
	if (have_top) {
		fnvlist_add_uint64(out, ZPARITY_ASIZE, asize);
		fnvlist_add_uint64(out, ZPARITY_PHYS_OFF,
		    VDEV_LABEL_START_SIZE);
		if (leaf_guid != 0) {
			fnvlist_add_uint64(out, ZPARITY_VDEV_GUID, leaf_guid);
			fnvlist_add_uint64(out, ZPARITY_DEV_SIZE, leaf_psize);
			if (path != NULL)
				fnvlist_add_string(out, ZPARITY_VDEV_PATH,
				    path);
		}
	}
	if (path != NULL)
		kmem_strfree(path);
	return (0);
}

/*
 * Block entries.
 */

typedef struct parity_ent {
	blkptr_t	pe_bp;
	uint64_t	pe_txg;
	uint8_t		pe_kind;
	uint8_t		pe_dva;		/* index of the DVA */
	uint64_t	pe_vdev;	/* the data column of the DVA */
	uint64_t	pe_off;		/* DVA offset on the data vdev */
	uint64_t	pe_len;		/* bytes on disk */
	boolean_t	pe_gang;
	boolean_t	pe_nocksum;	/* the block has no checksum to test */
	boolean_t	pe_zerock;	/* caller BP has zero checksum words */
	boolean_t	pe_fixup_unknown;
} parity_ent_t;

/*
 * A checksum type that cannot tell right bytes from wrong ones: the function
 * of "off" and "noparity" gives a zero checksum for any bytes.
 */
static boolean_t
parity_cksum_none(const blkptr_t *bp)
{
	return (BP_GET_CHECKSUM(bp) == ZIO_CHECKSUM_OFF ||
	    BP_GET_CHECKSUM(bp) == ZIO_CHECKSUM_NOPARITY);
}

/*
 * Parse and validate an entry that comes from the caller (an entry of
 * RECORDS).  Returns EINVAL for anything that is not a block of one of this
 * pool's data columns.  The checks, in short: 18 words; kind ALLOC or FREE;
 * known flags only; the DVA index is one of the BP's DVAs; the DVA is on a
 * data column (any of them); the flags LOG and GANG are the ones of the
 * BP; the length is the pool's own length for the DVA; the whole block is
 * inside the device; the checksum type is a known one; zfs_blkptr_verify()
 * passes.  An entry whose block has checksum off is accepted and marked
 * pe_nocksum.
 */
static int
parity_ent_parse(spa_t *spa, const uint64_t *w, uint_t nwords,
    parity_ent_t *pe)
{
	parity_vdev_info_t pvi;
	uint64_t w1, kind, flags, dvai, sectors, len;
	const dva_t *dva;
	blkptr_t *bp = &pe->pe_bp;

	if (nwords != PARITY_ENTRY_WORDS)
		return (SET_ERROR(EINVAL));

	w1 = w[PARITY_EW_KIND];
	kind = w1 & 0xff;
	flags = (w1 >> 8) & 0xff;
	dvai = (w1 >> 16) & 0xffff;
	sectors = w1 >> 32;
	if ((kind != PARITY_KIND_ALLOC && kind != PARITY_KIND_FREE) ||
	    flags > 3 || dvai >= SPA_DVAS_PER_BP)
		return (SET_ERROR(EINVAL));

	memcpy(bp, &w[PARITY_EW_BP], sizeof (*bp));
	if (BP_IS_EMBEDDED(bp) || BP_IS_HOLE(bp) || BP_IS_REDACTED(bp))
		return (SET_ERROR(EINVAL));
	if (BP_GET_CHECKSUM(bp) >= ZIO_CHECKSUM_FUNCTIONS ||
	    zio_checksum_table[BP_GET_CHECKSUM(bp)].ci_func[0] == NULL)
		return (SET_ERROR(EINVAL));
	if (zfs_blkptr_verify(spa, bp, BLK_CONFIG_NEEDED, BLK_VERIFY_ONLY) != 0)
		return (SET_ERROR(EINVAL));
	if (dvai >= BP_GET_NDVAS(bp))
		return (SET_ERROR(EINVAL));

	dva = &bp->blk_dva[dvai];
	/* the column of this DVA; another vdev (a log) is refused */
	if (parity_vdev_info(spa, DVA_GET_VDEV(dva), &pvi) != 0)
		return (SET_ERROR(EINVAL));
	if ((flags & 1) != (BP_GET_TYPE(bp) == DMU_OT_INTENT_LOG) ||
	    ((flags >> 1) & 1) != (DVA_GET_GANG(dva) ? 1 : 0))
		return (SET_ERROR(EINVAL));

	len = parity_dva_len(&pvi, dva);
	if (len == 0 || len > SPA_MAXBLOCKSIZE + SPA_OLD_GANGBLOCKSIZE ||
	    len != sectors << SPA_MINBLOCKSHIFT)
		return (SET_ERROR(EINVAL));
	if (!DVA_GET_GANG(dva) &&
	    (BP_GET_PSIZE(bp) == 0 || BP_GET_PSIZE(bp) > len))
		return (SET_ERROR(EINVAL));
	if (DVA_GET_GANG(dva) && len < SPA_OLD_GANGBLOCKSIZE)
		return (SET_ERROR(EINVAL));
	if (DVA_GET_OFFSET(dva) + len < DVA_GET_OFFSET(dva) ||
	    DVA_GET_OFFSET(dva) + len > pvi.pvi_asize)
		return (SET_ERROR(EINVAL));

	/*
	 * The physical read must stay inside every leaf that can be read.  A
	 * mirror side whose disk is missing was never opened (psize 0): no
	 * read goes to it, so it must not refuse the entry.  Before this, a
	 * mirror member with one missing side had every entry refused.
	 */
	for (uint_t i = 0; i < pvi.pvi_nleaves; i++) {
		if (pvi.pvi_leaf_psize[i] == 0)
			continue;
		if (VDEV_LABEL_START_SIZE + DVA_GET_OFFSET(dva) + len >
		    pvi.pvi_leaf_psize[i])
			return (SET_ERROR(EINVAL));
	}

	pe->pe_txg = w[PARITY_EW_TXG];
	pe->pe_kind = kind;
	pe->pe_dva = dvai;
	pe->pe_vdev = pvi.pvi_id;
	pe->pe_off = DVA_GET_OFFSET(dva);
	pe->pe_len = len;
	pe->pe_gang = DVA_GET_GANG(dva) ? B_TRUE : B_FALSE;
	/* A gang header has its own checksum, whatever the BP says. */
	pe->pe_nocksum = !pe->pe_gang && parity_cksum_none(bp);
	/* Set only by parity_ent_in_record() for an authoritative FIXUP. */
	pe->pe_fixup_unknown = B_FALSE;
	/*
	 * A caller BP with a zero checksum is only a candidate for an
	 * unverified entry.  parity_ent_in_record() checks a FREE in its record
	 * and an ALLOC against its effective FIXUP before setting pe_nocksum.
	 */
	pe->pe_zerock = ZIO_CHECKSUM_IS_ZERO(&bp->blk_cksum);
	return (0);
}

/*
 * The entry must belong to an unreleased record of this pool.  Without an
 * index only the record (its txg) is checked; with an index the entry at
 * that index must hold the same kind and the same DVAs (its checksum can
 * differ: a fix-up in a later record carries the final one).  The caller
 * holds a pin, so the record cannot be released.
 *
 * A FREE with a zero checksum in the caller's words is unverified only when
 * the indexed record entry also has a zero checksum.  An ALLOC is unverified
 * only when the indexed effective BP is the same zero-checksum FIXUP held in
 * the next record or ps_late.  Without an index, the caller cannot select
 * either exception.
 */
static int
parity_ent_alloc_fixup_unknown(spa_parity_t *ps, spa_lag_rec_t *lr,
    const blkptr_t *record_bp, parity_ent_t *pe);

static int
parity_ent_in_record(spa_parity_t *ps, parity_ent_t *pe,
    const uint64_t *index)
{
	spa_lag_t *lag = ps->ps_lag;
	spa_lag_rec_t *lr;
	uint64_t nent = 0, nfix = 0;
	uint64_t w[PARITY_ENTRY_WORDS];
	const blkptr_t *rbp;

	/* A txg that has not synced has no readable blocks yet. */
	if (pe->pe_txg > spa_last_synced_txg(ps->ps_spa))
		return (SET_ERROR(ENOENT));

	spa_lag_lock(lag);
	lr = spa_lag_rec_find(lag, pe->pe_txg);
	if (lr != NULL) {
		nent = lr->lr_phys.pr_nentries;
		nfix = lr->lr_phys.pr_nfixups;
	}
	spa_lag_unlock(lag);
	if (lr == NULL)
		return (SET_ERROR(ENOENT));
	if (index == NULL)
		return (0);

	if (*index < nfix || *index >= nent)
		return (SET_ERROR(EINVAL));
	if (spa_lag_rec_read(lag, lr, *index, 1, w) != 0)
		return (SET_ERROR(EIO));
	if ((w[PARITY_EW_KIND] & 0xff) != pe->pe_kind)
		return (SET_ERROR(ENOENT));
	rbp = (const blkptr_t *)&w[PARITY_EW_BP];
	if (!parity_same_dvas(rbp, &pe->pe_bp))
		return (SET_ERROR(ENOENT));
	if (pe->pe_zerock && !pe->pe_gang &&
	    ZIO_CHECKSUM_IS_ZERO(&rbp->blk_cksum) &&
	    pe->pe_kind == PARITY_KIND_FREE)
		pe->pe_nocksum = B_TRUE;
	if (pe->pe_zerock && pe->pe_kind == PARITY_KIND_ALLOC &&
	    !parity_cksum_none(&pe->pe_bp)) {
		int error = parity_ent_alloc_fixup_unknown(ps, lr, rbp, pe);

		if (error != 0)
			return (error);
	}
	return (0);
}

/*
 * READ and READV: an entry of an unreleased record, or an ALLOC entry from
 * BLOCKS.  BLOCKS walks the state of an applied txg in [first_txg - 1,
 * applied_txg] and every block of that state is still held, so it may be read
 * after its record is released (the last applied record goes at the pool's
 * next write).  Without this, a set scrub of a member that wrote after its last
 * apply saw every block as missing.  The bytes are
 * still checked against the block pointer checksum.  An entry with no checksum
 * (off or noparity) gets ZFS_ERR_PARITY_UNVERIFIABLE instead: no record and no
 * checksum tie it to the applied state, so a caller-made entry must not read
 * any range of the device through this exception.  VERIFY keeps the strict
 * record check: repair depends on it.
 */
static int
parity_ent_readable(spa_parity_t *ps, parity_ent_t *pe,
    const uint64_t *index)
{
	uint64_t applied, start, first;
	int error = parity_ent_in_record(ps, pe, index);

	if (error != ENOENT || index != NULL ||
	    pe->pe_kind != PARITY_KIND_ALLOC)
		return (error);
	/*
	 * The first_txg of INFO, read in constant time: READV asks for each
	 * of its entries, so spa_parity_get_info() (a walk over every record
	 * and a MOS lookup) is too costly here.
	 */
	mutex_enter(&ps->ps_lock);
	applied = ps->ps_applied_txg;
	start = ps->ps_start_txg;
	mutex_exit(&ps->ps_lock);
	first = spa_lag_first_txg(ps->ps_lag, MAX(applied + 1, start));
	if (pe->pe_txg + 1 >= first && pe->pe_txg <= applied &&
	    pe->pe_txg <= spa_last_synced_txg(ps->ps_spa))
		return (pe->pe_nocksum ?
		    SET_ERROR(ZFS_ERR_PARITY_UNVERIFIABLE) : 0);
	return (error);
}

/*
 * Check bytes against the entry's block pointer (spa_parity_bp_verify()):
 * a gang header is checked as a gang header.  The bytes are not changed.
 */
static int
parity_ent_cksum(spa_t *spa, const parity_ent_t *pe, void *buf)
{
	return (spa_parity_bp_verify(spa, &pe->pe_bp, buf, pe->pe_len));
}

/* Read bytes from a leaf, without any checksum.  Returns the zio error. */
static int
parity_read_phys(vdev_t *leaf, uint64_t off, void *buf, uint64_t len)
{
	abd_t *abd = abd_alloc_linear(len, B_FALSE);
	int error;

	error = zio_wait(zio_read_phys(NULL, leaf, off, len, abd,
	    ZIO_CHECKSUM_OFF, NULL, NULL, ZIO_PRIORITY_SYNC_READ,
	    ZIO_FLAG_CANFAIL | ZIO_FLAG_DONT_PROPAGATE, B_FALSE));
	if (error == 0)
		abd_copy_to_buf(buf, abd, len);
	abd_free(abd);
	return (error);
}

/*
 * Read the raw on-disk bytes of the entry's DVA and verify them.  A mirror
 * (a disk being replaced) is tried child by child.  Returns EIO when no
 * leaf could read the bytes and ECKSUM when the bytes were read and do not
 * match.
 */
static int
parity_read_verified(spa_t *spa, const parity_ent_t *pe, void *buf)
{
	vdev_t *top;
	vdev_t *leaves[PARITY_MAX_LEAVES];
	int n, error = SET_ERROR(EIO);

	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	top = parity_col_tvd(spa, pe->pe_vdev);
	if (top == NULL) {
		spa_config_exit(spa, SCL_STATE, FTAG);
		return (SET_ERROR(ENXIO));
	}
	n = parity_leaves(top, leaves);
	for (int i = 0; i < n; i++) {
		int rerr;

		if (!vdev_readable(leaves[i]) ||
		    vdev_dtl_contains(leaves[i], DTL_MISSING,
		    BP_GET_PHYSICAL_BIRTH(&pe->pe_bp), 1))
			continue;
		rerr = parity_read_phys(leaves[i],
		    VDEV_LABEL_START_SIZE + pe->pe_off, buf, pe->pe_len);
		if (rerr != 0) {
			if (error != ECKSUM)
				error = rerr;
			continue;
		}
		atomic_add_64(&spa->spa_parity->ps_stat_read_bytes, pe->pe_len);
		if (pe->pe_nocksum || parity_ent_cksum(spa, pe, buf) == 0) {
			error = 0;
			break;
		}
		error = SET_ERROR(ECKSUM);
	}
	spa_config_exit(spa, SCL_STATE, FTAG);
	return (error);
}

/*
 * READ.  Reply: "data", the raw bytes of the DVA, padding included, and
 * "verified": false when the block has no checksum or an ALLOC checksum was
 * lost, so the bytes were not tested.
 */
int
spa_parity_ioc_read(spa_t *spa, const uint64_t *entry, uint_t nwords,
    const uint64_t *index, nvlist_t *out)
{
	spa_parity_t *ps = spa->spa_parity;
	parity_ent_t pe;
	uint8_t *buf;
	int error;

	if (ps == NULL)
		return (SET_ERROR(ENOTSUP));
	error = parity_ent_parse(spa, entry, nwords, &pe);
	if (error != 0)
		return (error);

	parity_pin(ps);
	error = parity_ent_readable(ps, &pe, index);
	if (error == 0) {
		buf = vmem_alloc(pe.pe_len, KM_SLEEP);
		error = parity_read_verified(spa, &pe, buf);
		if (error == 0) {
			fnvlist_add_uint64(out, ZPARITY_VERSION,
			    ZPARITY_VERSION_CURRENT);
			fnvlist_add_boolean_value(out, ZPARITY_VERIFIED,
			    !pe.pe_nocksum);
			fnvlist_add_uint8_array(out, ZPARITY_DATA, buf,
			    pe.pe_len);
		}
		vmem_free(buf, pe.pe_len);
	}
	parity_unpin(ps);
	return (error);
}

/*
 * READV.  Each entry has its own error.  Successful bytes are packed into
 * "data"; the offset and length arrays select each entry's bytes.
 */
int
spa_parity_ioc_readv(spa_t *spa, const uint64_t *entries, uint_t nwords,
    const uint64_t *indexes, uint_t nindexes, nvlist_t *out)
{
	spa_parity_t *ps = spa->spa_parity;
	parity_ent_t *pes;
	int32_t *errors;
	boolean_t *verified;
	uint64_t *offsets, *lengths;
	uint8_t *data, empty_data = 0;
	uint_t nentries, data_len = 0;
	uint64_t data_alloc = 0;
	int error = 0;

	if (ps == NULL)
		return (SET_ERROR(ENOTSUP));
	if (entries == NULL || nwords == 0 ||
	    nwords % PARITY_ENTRY_WORDS != 0)
		return (SET_ERROR(EINVAL));
	nentries = nwords / PARITY_ENTRY_WORDS;
	if (nentries > ZPARITY_READV_MAX_ENTRIES ||
	    (indexes != NULL && nindexes != nentries) ||
	    (indexes == NULL && nindexes != 0))
		return (SET_ERROR(EINVAL));

	pes = kmem_zalloc(nentries * sizeof (*pes), KM_SLEEP);
	errors = kmem_zalloc(nentries * sizeof (*errors), KM_SLEEP);
	verified = kmem_zalloc(nentries * sizeof (*verified), KM_SLEEP);
	offsets = kmem_zalloc(nentries * sizeof (*offsets), KM_SLEEP);
	lengths = kmem_zalloc(nentries * sizeof (*lengths), KM_SLEEP);

	for (uint_t i = 0; i < nentries; i++) {
		errors[i] = parity_ent_parse(spa,
		    &entries[i * PARITY_ENTRY_WORDS], PARITY_ENTRY_WORDS,
		    &pes[i]);
	}

	for (uint_t i = 0; i < nentries; i++) {
		const uint64_t *index = (indexes != NULL) ? &indexes[i] : NULL;

		if (errors[i] != 0)
			continue;
		parity_pin(ps);
		error = parity_ent_readable(ps, &pes[i], index);
		parity_unpin(ps);
		if (error != 0) {
			errors[i] = error;
			continue;
		}
		if (pes[i].pe_len > ZPARITY_READV_MAX_BYTES - data_alloc) {
			errors[i] = E2BIG;
			continue;
		}
		data_alloc += pes[i].pe_len;
	}

	data = (data_alloc != 0) ? vmem_alloc(data_alloc, KM_SLEEP) :
	    &empty_data;
	for (uint_t i = 0; i < nentries; i++) {
		const uint64_t *index = (indexes != NULL) ? &indexes[i] : NULL;

		if (errors[i] != 0)
			continue;
		parity_pin(ps);
		error = parity_ent_readable(ps, &pes[i], index);
		if (error == 0)
			error = parity_read_verified(spa, &pes[i],
			    data + data_len);
		parity_unpin(ps);
		if (error != 0) {
			errors[i] = error;
			continue;
		}
		offsets[i] = data_len;
		lengths[i] = pes[i].pe_len;
		verified[i] = !pes[i].pe_nocksum;
		data_len += pes[i].pe_len;
	}

	fnvlist_add_uint64(out, ZPARITY_VERSION, ZPARITY_VERSION_CURRENT);
	fnvlist_add_int32_array(out, ZPARITY_READ_ERRORS, errors, nentries);
	fnvlist_add_boolean_array(out, ZPARITY_VERIFIED, verified, nentries);
	fnvlist_add_uint64_array(out, ZPARITY_DATA_OFFSETS, offsets, nentries);
	fnvlist_add_uint64_array(out, ZPARITY_DATA_LENGTHS, lengths, nentries);
	fnvlist_add_uint8_array(out, ZPARITY_DATA, data, data_len);

	if (data_alloc != 0)
		vmem_free(data, data_alloc);
	kmem_free(lengths, nentries * sizeof (*lengths));
	kmem_free(offsets, nentries * sizeof (*offsets));
	kmem_free(verified, nentries * sizeof (*verified));
	kmem_free(errors, nentries * sizeof (*errors));
	kmem_free(pes, nentries * sizeof (*pes));
	return (0);
}

/*
 * VERIFY.  Check bytes that the caller rebuilt against the entry's checksum.
 * Writes nothing.
 */
int
spa_parity_ioc_verify(spa_t *spa, const uint64_t *entry, uint_t nwords,
    const uint64_t *index, const uint8_t *data, uint_t len,
    boolean_t *match, boolean_t *verified)
{
	spa_parity_t *ps = spa->spa_parity;
	parity_ent_t pe;
	int error;

	*match = B_FALSE;
	*verified = B_TRUE;
	if (ps == NULL)
		return (SET_ERROR(ENOTSUP));
	error = parity_ent_parse(spa, entry, nwords, &pe);
	if (error != 0)
		return (error);
	if (len != pe.pe_len)
		return (SET_ERROR(EINVAL));

	parity_pin(ps);
	error = parity_ent_in_record(ps, &pe, index);
	parity_unpin(ps);
	if (error != 0)
		return (error);
	if (pe.pe_nocksum && !pe.pe_fixup_unknown)
		return (SET_ERROR(EINVAL));
	if (pe.pe_fixup_unknown) {
		*verified = B_FALSE;
		return (0);
	}

	*match = (spa_parity_bp_verify(spa, &pe.pe_bp, data, len) == 0);
	return (0);
}

/*
 * RECORDS.
 */

typedef struct parity_fix {
	avl_node_t	pf_node;
	uint64_t	pf_vdev;
	uint64_t	pf_offset;
	blkptr_t	pf_bp;
} parity_fix_t;

static int
parity_fix_compare(const void *a, const void *b)
{
	const parity_fix_t *x = a, *y = b;
	int c = TREE_CMP(x->pf_vdev, y->pf_vdev);

	return (c != 0 ? c : TREE_CMP(x->pf_offset, y->pf_offset));
}

static void
parity_fix_add(avl_tree_t *t, const blkptr_t *bp)
{
	parity_fix_t *pf = kmem_alloc(sizeof (*pf), KM_SLEEP);
	avl_index_t where;
	parity_fix_t *old;

	pf->pf_vdev = DVA_GET_VDEV(&bp->blk_dva[0]);
	pf->pf_offset = DVA_GET_OFFSET(&bp->blk_dva[0]);
	pf->pf_bp = *bp;
	if ((old = avl_find(t, pf, &where)) != NULL) {
		old->pf_bp = *bp;
		kmem_free(pf, sizeof (*pf));
		return;
	}
	avl_insert(t, pf, where);
}

static void
parity_fix_destroy(avl_tree_t *t)
{
	parity_fix_t *pf;
	void *cookie = NULL;

	while ((pf = avl_destroy_nodes(t, &cookie)) != NULL)
		kmem_free(pf, sizeof (*pf));
	avl_destroy(t);
}

/*
 * The final BPs of the entries of record lr that changed after they were
 * written.  They are in the first entries of the next record, or,
 * while that record has not written them yet, in the core's in-memory list.
 * See the comment in the code about the order of the checks.
 */
static int
parity_fix_load(spa_parity_t *ps, spa_lag_rec_t *lr, avl_tree_t *t)
{
	spa_lag_t *lag = ps->ps_lag;
	spa_lag_rec_t *succ;
	uint64_t nfix, c;
	uint64_t *buf;
	int error = 0;

	spa_lag_lock(lag);
	succ = spa_lag_rec_next(lag, lr);
	nfix = (succ != NULL) ? succ->lr_phys.pr_nfixups : 0;
	spa_lag_unlock(lag);

	if (nfix == 0) {
		parity_pend_t *pp;

		/*
		 * The next record may have no fix-ups, or it may not have
		 * written them yet; the core keeps them in ps_late until it
		 * has set pr_nfixups, and then moves the list (all under
		 * ps_lock).  Copy ps_late, then look at pr_nfixups again: if
		 * it is still 0, the list had not been moved.
		 */
		mutex_enter(&ps->ps_lock);
		if (ps->ps_late_txg == lr->lr_txg) {
			for (pp = list_head(&ps->ps_late); pp != NULL;
			    pp = list_next(&ps->ps_late, pp))
				parity_fix_add(t, &pp->pp_bp);
		}
		mutex_exit(&ps->ps_lock);

		spa_lag_lock(lag);
		succ = spa_lag_rec_next(lag, lr);
		nfix = (succ != NULL) ? succ->lr_phys.pr_nfixups : 0;
		spa_lag_unlock(lag);
		if (nfix == 0)
			return (0);
		/* Written meanwhile: use the record, drop the copy. */
		parity_fix_destroy(t);
		avl_create(t, parity_fix_compare, sizeof (parity_fix_t),
		    offsetof(parity_fix_t, pf_node));
	}

	buf = vmem_alloc(PARITY_FLUSH_CHUNK * PARITY_ENTRY_WORDS *
	    sizeof (uint64_t), KM_SLEEP);
	for (uint64_t i = 0; i < nfix && error == 0; i += c) {
		c = MIN(nfix - i, PARITY_FLUSH_CHUNK);
		error = spa_lag_rec_read(lag, succ, i, c, buf);
		for (uint64_t j = 0; j < c && error == 0; j++) {
			uint64_t *w = &buf[j * PARITY_ENTRY_WORDS];

			if (w[PARITY_EW_KIND] != PARITY_KIND_FIXUP ||
			    w[PARITY_EW_TXG] != lr->lr_txg)
				continue;
			parity_fix_add(t, (const blkptr_t *)&w[PARITY_EW_BP]);
		}
	}
	vmem_free(buf, PARITY_FLUSH_CHUNK * PARITY_ENTRY_WORDS *
	    sizeof (uint64_t));
	return (error);
}

/*
 * An ALLOC with a zero checksum is unverified only when RECORDS supplied the
 * same BP from a FIXUP in the next record or in ps_late.  The caller's zero
 * checksum alone must not disable checksum verification.
 */
static int
parity_ent_alloc_fixup_unknown(spa_parity_t *ps, spa_lag_rec_t *lr,
    const blkptr_t *record_bp, parity_ent_t *pe)
{
	avl_tree_t fix;
	parity_fix_t key, *pf;
	int error;

	avl_create(&fix, parity_fix_compare, sizeof (parity_fix_t),
	    offsetof(parity_fix_t, pf_node));
	error = parity_fix_load(ps, lr, &fix);
	if (error == 0) {
		key.pf_vdev = DVA_GET_VDEV(&record_bp->blk_dva[0]);
		key.pf_offset = DVA_GET_OFFSET(&record_bp->blk_dva[0]);
		pf = avl_find(&fix, &key, NULL);
		if (pf != NULL && parity_same_dvas(&pf->pf_bp, record_bp) &&
		    memcmp(&pf->pf_bp, &pe->pe_bp, sizeof (pe->pe_bp)) == 0 &&
		    ZIO_CHECKSUM_IS_ZERO(&pf->pf_bp.blk_cksum)) {
			pe->pe_nocksum = B_TRUE;
			pe->pe_fixup_unknown = B_TRUE;
		}
	}
	parity_fix_destroy(&fix);
	return (error);
}

/* The first record with a txg of at least "txg", or NULL. */
static spa_lag_rec_t *
parity_rec_ge(spa_lag_t *lag, uint64_t txg)
{
	spa_lag_rec_t *lr;

	spa_lag_lock(lag);
	for (lr = spa_lag_rec_first(lag); lr != NULL;
	    lr = spa_lag_rec_next(lag, lr)) {
		if (lr->lr_txg >= txg)
			break;
	}
	spa_lag_unlock(lag);
	return (lr);
}

/* Is a record entry (read from disk) valid enough to hand to the caller? */
static boolean_t
parity_rec_entry_ok(spa_t *spa, const parity_cols_t *pc, const blkptr_t *bp)
{
	if (BP_IS_EMBEDDED(bp) || BP_IS_HOLE(bp))
		return (B_FALSE);
	if (zfs_blkptr_verify(spa, bp, BLK_CONFIG_NEEDED, BLK_VERIFY_ONLY) != 0)
		return (B_FALSE);
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		const dva_t *dva = &bp->blk_dva[d];
		uint64_t len;

		/* A log device holds only log blocks: they have no data. */
		const parity_vdev_info_t *pv = parity_col(pc,
		    DVA_GET_VDEV(dva));
		if (pv == NULL)
			continue;
		len = parity_dva_len(pv, dva);
		if (len == 0 || DVA_GET_OFFSET(dva) + len > pv->pvi_asize)
			return (B_FALSE);
	}
	return (B_TRUE);
}

int
spa_parity_ioc_records(spa_t *spa, uint64_t from_txg, uint64_t cursor,
    uint64_t max_entries, nvlist_t *out)
{
	spa_parity_t *ps = spa->spa_parity;
	spa_parity_info_t pi;
	spa_lag_t *lag;
	parity_cols_t *pc;
	uint64_t *ebuf, *idxbuf, *rbuf;
	uint64_t max, nout = 0, last, start_txg, start_idx = 0;
	uint64_t next_cursor = 0;
	boolean_t done = B_TRUE;
	spa_lag_rec_t *lr;
	int error = 0;

	if (ps == NULL || (pc = parity_cols_load(spa)) == NULL)
		return (SET_ERROR(ENOTSUP));
	lag = ps->ps_lag;
	max = MIN(MAX(max_entries, 3), PARITY_RECORDS_MAX);

	start_txg = from_txg;
	if (cursor != 0 && (cursor >> PARITY_CUR_SHIFT) >= from_txg) {
		start_txg = cursor >> PARITY_CUR_SHIFT;
		start_idx = cursor & PARITY_CUR_MASK;
	}

	ebuf = vmem_alloc(max * PARITY_ENTRY_WORDS * sizeof (uint64_t),
	    KM_SLEEP);
	idxbuf = vmem_alloc(max * sizeof (uint64_t), KM_SLEEP);
	rbuf = vmem_alloc(PARITY_FLUSH_CHUNK * PARITY_ENTRY_WORDS *
	    sizeof (uint64_t), KM_SLEEP);

	parity_pin(ps);
	spa_parity_get_info(spa, &pi);
	last = spa_last_synced_txg(spa);

	/* Find the start once, then step on (the pin keeps the record). */
	lr = parity_rec_ge(lag, start_txg);
	while (lr != NULL && error == 0 && done) {
		avl_tree_t fix;
		uint64_t nent, nfix, i, c = 0;

		if (lr->lr_txg > last)
			break;
		if (lr->lr_txg != start_txg)
			start_idx = 0;
		nent = lr->lr_phys.pr_nentries;
		nfix = lr->lr_phys.pr_nfixups;
		if (start_idx > nent) {
			error = SET_ERROR(EINVAL);
			break;
		}

		avl_create(&fix, parity_fix_compare, sizeof (parity_fix_t),
		    offsetof(parity_fix_t, pf_node));
		error = parity_fix_load(ps, lr, &fix);

		for (i = MAX(start_idx, nfix); i < nent && error == 0 && done;
		    i += c) {
			c = MIN(nent - i, PARITY_FLUSH_CHUNK);
			error = spa_lag_rec_read(lag, lr, i, c, rbuf);
			if (error != 0)
				break;
			for (uint64_t j = 0; j < c; j++) {
				uint64_t *w = &rbuf[j * PARITY_ENTRY_WORDS];
				uint64_t kind = w[PARITY_EW_KIND] & 0xff;
				blkptr_t bp;
				int nd;

				if (kind != PARITY_KIND_ALLOC &&
				    kind != PARITY_KIND_FREE)
					continue;
				bp = *(const blkptr_t *)&w[PARITY_EW_BP];
				if (kind == PARITY_KIND_ALLOC) {
					parity_fix_t key, *pf;

					key.pf_vdev =
					    DVA_GET_VDEV(&bp.blk_dva[0]);
					key.pf_offset =
					    DVA_GET_OFFSET(&bp.blk_dva[0]);
					pf = avl_find(&fix, &key, NULL);
					if (pf != NULL &&
					    parity_same_dvas(&pf->pf_bp, &bp))
						bp = pf->pf_bp;
				}
				if (!parity_rec_entry_ok(spa, pc, &bp)) {
					zfs_dbgmsg("parity: pool '%s': bad "
					    "block pointer in record %llu "
					    "entry %llu", spa_name(spa),
					    (u_longlong_t)lr->lr_txg,
					    (u_longlong_t)(i + j));
					error = SET_ERROR(ECKSUM);
					break;
				}
				nd = BP_GET_NDVAS(&bp);
				if (nout + nd > max) {
					/* The cursor holds 28 bits of index. */
					if (i + j > PARITY_CUR_MASK ||
					    (lr->lr_txg >>
					    (64 - PARITY_CUR_SHIFT)) != 0) {
						error = SET_ERROR(EOVERFLOW);
						break;
					}
					next_cursor = (lr->lr_txg <<
					    PARITY_CUR_SHIFT) | (i + j);
					done = B_FALSE;
					break;
				}
				for (int d = 0; d < nd; d++) {
					uint64_t *o = &ebuf[nout *
					    PARITY_ENTRY_WORDS];
					const dva_t *dva = &bp.blk_dva[d];
					const parity_vdev_info_t *pv;
					uint64_t flags = 0;
					uint64_t len;

					if (BP_GET_TYPE(&bp) ==
					    DMU_OT_INTENT_LOG)
						flags |= 1;
					if (DVA_GET_GANG(dva))
						flags |= 2;
					/* the length on its own column */
					pv = parity_col(pc, DVA_GET_VDEV(dva));
					len = (pv != NULL) ?
					    parity_dva_len(pv, dva) :
					    DVA_GET_ASIZE(dva);
					o[PARITY_EW_TXG] = lr->lr_txg;
					o[PARITY_EW_KIND] = kind |
					    (flags << 8) |
					    ((uint64_t)d << 16) |
					    ((len >> SPA_MINBLOCKSHIFT) << 32);
					memcpy(&o[PARITY_EW_BP], &bp,
					    sizeof (bp));
					idxbuf[nout] = i + j;
					nout++;
				}
			}
		}
		parity_fix_destroy(&fix);
		spa_lag_lock(lag);
		lr = spa_lag_rec_next(lag, lr);
		spa_lag_unlock(lag);
	}
	parity_unpin(ps);

	if (error == 0) {
		fnvlist_add_uint64(out, ZPARITY_VERSION,
		    ZPARITY_VERSION_CURRENT);
		fnvlist_add_uint64_array(out, ZPARITY_ENTRIES, ebuf,
		    nout * PARITY_ENTRY_WORDS);
		fnvlist_add_uint64_array(out, ZPARITY_INDEXES, idxbuf, nout);
		fnvlist_add_uint64(out, ZPARITY_CURSOR, next_cursor);
		fnvlist_add_boolean_value(out, ZPARITY_DONE, done);
		fnvlist_add_uint64(out, ZPARITY_FIRST_TXG, pi.pi_first_txg);
	}
	vmem_free(rbuf, PARITY_FLUSH_CHUNK * PARITY_ENTRY_WORDS *
	    sizeof (uint64_t));
	vmem_free(idxbuf, max * sizeof (uint64_t));
	vmem_free(ebuf, max * PARITY_ENTRY_WORDS * sizeof (uint64_t));
	parity_cols_free(pc);
	return (error);
}

/*
 * MAP.
 *
 * Allocated space as of txg T, as the data device holds it:
 *
 *   the allocated ranges of the metaslabs (their synced state)
 *   - the ALLOC entries of records after T, and of the capture tables
 *     (blocks that were allocated after T and are already in a metaslab)
 *   - the FREE entries of unreleased records up to T (a held free stays
 *     held in the space map after the parity service has applied the free)
 *   - the log blocks: ALLOC entries flagged LOG, and the blocks of the live
 *     intent log chains.
 *
 * The release is paused while the call runs (the pin).  The order of the
 * steps matters: the metaslabs first, then the capture tables, then the
 * records.  An entry is in a capture table from its capture until the txg is
 * synced, and in its record from its flush; so an entry that reached a
 * metaslab before the first step ended is in the table at the second step,
 * or in its record at the third.
 */

typedef struct parity_map_arg {
	zfs_range_tree_t	*pm_rt;
	uint64_t		pm_from;
	uint64_t		pm_max;
	uint64_t		*pm_out;
	uint64_t		pm_n;
	uint64_t		pm_next;
	boolean_t		pm_trunc;
} parity_map_arg_t;

static void
parity_map_emit(void *arg, uint64_t start, uint64_t size)
{
	parity_map_arg_t *pm = arg;
	uint64_t end = start + size;

	if (end <= pm->pm_from || pm->pm_trunc)
		return;
	if (start < pm->pm_from)
		start = pm->pm_from;
	if (pm->pm_n == pm->pm_max) {
		pm->pm_trunc = B_TRUE;
		return;
	}
	pm->pm_out[2 * pm->pm_n] = start;
	pm->pm_out[2 * pm->pm_n + 1] = end - start;
	pm->pm_n++;
	pm->pm_next = end;
}

/* Remove the data-device ranges of every DVA of a BP. */
static void
parity_map_clear_bp(zfs_range_tree_t *rt,
    const parity_vdev_info_t *pvi, const blkptr_t *bp)
{
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		const dva_t *dva = &bp->blk_dva[d];
		uint64_t len;

		if (DVA_GET_VDEV(dva) != pvi->pvi_id)
			continue;
		len = parity_dva_len(pvi, dva);
		if (len != 0)
			zfs_range_tree_clear(rt, DVA_GET_OFFSET(dva), len);
	}
}

typedef struct parity_log_arg {
	zfs_range_tree_t	*pl_rt;
	parity_vdev_info_t	pl_vdev;
} parity_log_arg_t;

static int
parity_log_blk(zilog_t *zilog, const blkptr_t *bp, void *arg, uint64_t txg)
{
	parity_log_arg_t *pl = arg;

	(void) zilog, (void) txg;
	if (!BP_IS_HOLE(bp) && !BP_IS_EMBEDDED(bp))
		parity_map_clear_bp(pl->pl_rt, &pl->pl_vdev, bp);
	return (0);
}

/*
 * The blocks of the live intent log chain of one filesystem or volume.  A
 * dataset whose key is not loaded cannot be opened: its log blocks stay in
 * the map (they cannot change while the key is not loaded).  The walk can see
 * a block twice: clearing a range twice does no harm.
 */
static int
parity_log_ds(dsl_pool_t *dp, dsl_dataset_t *ds, void *arg)
{
	objset_t *os;
	int error;

	(void) dp;
	if ((error = dmu_objset_from_ds(ds, &os)) != 0)
		return (error);
	/* This walk leaves the shared state of the live log alone. */
	error = zil_walk_blocks(dmu_objset_zil(os), parity_log_blk, arg);
	return ((error == ECKSUM || error == ENOENT) ? 0 : error);
}

/*
 * The first txg whose history is whole (released watermark + 1): the oldest
 * record, but not above the txg after the applied one (or the start).  Only
 * records up to the applied txg are released, so a txg with no record at
 * all is not a released one: with applied txg 76 and the oldest record at
 * 78, MAP and BLOCKS still serve txg 76.  A rebuild asks for the applied
 * txg of a surviving pool.
 */
static uint64_t
parity_history_txg(const spa_parity_info_t *pi)
{
	uint64_t released = MAX(pi->pi_applied_txg + 1, pi->pi_start_txg);

	return (MIN(pi->pi_first_txg, released));
}

int
spa_parity_ioc_map(spa_t *spa, uint64_t vdev, uint64_t txg,
    uint64_t from_offset, uint64_t max_ranges, nvlist_t *out)
{
	spa_parity_t *ps = spa->spa_parity;
	spa_parity_info_t pi;
	vdev_t *top;
	zfs_range_tree_t *rt;
	parity_map_arg_t pm;
	parity_log_arg_t pl;
	uint64_t max, window_end = from_offset, last, nrecs, k;
	uint64_t asize, ms_count, ms_shift;
	parity_vdev_info_t pvi = { 0 };
	spa_lag_rec_t **recs;
	spa_lag_rec_t *lr;
	uint64_t *rbuf;
	int error = 0;

	if (ps == NULL)
		return (SET_ERROR(ENOTSUP));
	max = MIN(MAX(max_ranges, 1), PARITY_RANGES_MAX);

	spa_parity_get_info(spa, &pi);
	if (pi.pi_state == PARITY_OFF && pi.pi_nrecs == 0)
		return (SET_ERROR(ENOTSUP));
	last = spa_last_synced_txg(spa);
	if (txg + 1 < parity_history_txg(&pi) || txg > last)
		return (SET_ERROR(EINVAL));

	/*
	 * Look up the top vdev under the lock that protects its metaslab array.
	 * A replace can change the top vdev while parity records exist.
	 */
	spa_config_enter(spa, SCL_CONFIG, FTAG, RW_READER);
	top = parity_col_tvd(spa, vdev);
	if (top == NULL) {
		spa_config_exit(spa, SCL_CONFIG, FTAG);
		return (SET_ERROR(ENXIO));
	}
	asize = top->vdev_asize;
	ms_count = top->vdev_ms_count;
	ms_shift = top->vdev_ms_shift;
	pvi.pvi_id = top->vdev_id;
	pvi.pvi_asize = asize;
	pvi.pvi_ashift = top->vdev_ashift;
	pvi.pvi_gang_asize = vdev_gang_header_asize(top);
	if (ms_count == 0) {
		spa_config_exit(spa, SCL_CONFIG, FTAG);
		return (SET_ERROR(EAGAIN));
	}

	if (from_offset >= asize) {
		uint64_t none = 0;

		spa_config_exit(spa, SCL_CONFIG, FTAG);
		fnvlist_add_uint64(out, ZPARITY_VERSION,
		    ZPARITY_VERSION_CURRENT);
		fnvlist_add_uint64_array(out, ZPARITY_RANGES, &none, 0);
		fnvlist_add_uint64(out, ZPARITY_NEXT_OFFSET, asize);
		fnvlist_add_boolean_value(out, ZPARITY_DONE, B_TRUE);
		return (0);
	}

	rt = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64, NULL, 0, 0);
	parity_pin(ps);

	/* 1. The metaslabs, from the one that holds from_offset. */
	for (uint64_t id = from_offset >> ms_shift; id < ms_count; id++) {
		metaslab_t *msp = top->vdev_ms[id];

		if (msp == NULL)
			continue;
		error = metaslab_allocated_ranges(msp, rt);
		if (error != 0)
			break;
		window_end = msp->ms_start + msp->ms_size;
		if (zfs_range_tree_numsegs(rt) >= max)
			break;
	}

	/*
	 * The space after the last metaslab is never allocated: the asize is
	 * rarely a whole number of metaslabs.  So a window that reaches the
	 * last metaslab reaches the end.  Without this the reply said "not
	 * done, next = the end of the last metaslab", and the next call, which
	 * finds no metaslab there, said the same again: a caller that pages
	 * until DONE never got there.
	 */
	if (error == 0 && window_end >= (ms_count << ms_shift))
		window_end = asize;
	spa_config_exit(spa, SCL_CONFIG, FTAG);

	/* 2. The capture tables: blocks of the txgs that are not synced. */
	for (int i = 0; i < TXG_SIZE && error == 0; i++) {
		parity_txg_t *pt = &ps->ps_txg[i];
		parity_pend_t *pp;

		mutex_enter(&pt->pt_lock);
		for (pp = avl_first(&pt->pt_tree); pp != NULL;
		    pp = AVL_NEXT(&pt->pt_tree, pp)) {
			if (pp->pp_kind == PARITY_KIND_ALLOC)
				parity_map_clear_bp(rt, &pvi, &pp->pp_bp);
		}
		mutex_exit(&pt->pt_lock);
	}

	/* 3. The records. */
	if (error == 0) {
		spa_lag_t *lag = ps->ps_lag;

		spa_lag_lock(lag);
		nrecs = spa_lag_nrecs(lag);
		recs = vmem_alloc((nrecs + 1) * sizeof (*recs), KM_SLEEP);
		k = 0;
		for (lr = spa_lag_rec_first(lag); lr != NULL && k < nrecs;
		    lr = spa_lag_rec_next(lag, lr))
			recs[k++] = lr;
		spa_lag_unlock(lag);
		nrecs = k;

		rbuf = vmem_alloc(PARITY_FLUSH_CHUNK * PARITY_ENTRY_WORDS *
		    sizeof (uint64_t), KM_SLEEP);
		for (k = 0; k < nrecs && error == 0; k++) {
			uint64_t nent, nfix, i, c = 0;

			lr = recs[k];
			nent = lr->lr_phys.pr_nentries;
			nfix = lr->lr_phys.pr_nfixups;
			for (i = nfix; i < nent && error == 0; i += c) {
				c = MIN(nent - i, PARITY_FLUSH_CHUNK);
				error = spa_lag_rec_read(lag, lr, i, c, rbuf);
				for (uint64_t j = 0; j < c && error == 0;
				    j++) {
					uint64_t *w = &rbuf[j *
					    PARITY_ENTRY_WORDS];
					const blkptr_t *bp = (const blkptr_t *)
					    &w[PARITY_EW_BP];
					uint64_t kind =
					    w[PARITY_EW_KIND] & 0xff;
					boolean_t log;

					if (kind != PARITY_KIND_ALLOC &&
					    kind != PARITY_KIND_FREE)
						continue;
					if (BP_IS_EMBEDDED(bp) ||
					    BP_IS_HOLE(bp))
						continue;
					log = (BP_GET_TYPE(bp) ==
					    DMU_OT_INTENT_LOG);
					if ((kind == PARITY_KIND_ALLOC &&
					    (log || lr->lr_txg > txg)) ||
					    (kind == PARITY_KIND_FREE &&
					    lr->lr_txg <= txg))
						parity_map_clear_bp(rt, &pvi,
						    bp);
				}
			}
		}
		vmem_free(rbuf, PARITY_FLUSH_CHUNK * PARITY_ENTRY_WORDS *
		    sizeof (uint64_t));
		vmem_free(recs, (nrecs + 1) * sizeof (*recs));
	}

	/* 4. The live intent log chains. */
	if (error == 0) {
		dsl_pool_t *dp = spa_get_dsl(spa);

		pl.pl_rt = rt;
		pl.pl_vdev = pvi;
		dsl_pool_config_enter(dp, FTAG);
		error = dmu_objset_find_dp(dp, dp->dp_root_dir_obj,
		    parity_log_ds, &pl, DS_FIND_CHILDREN | DS_FIND_SERIALIZE);
		dsl_pool_config_exit(dp, FTAG);
	}
	parity_unpin(ps);

	if (error == 0) {
		pm.pm_rt = rt;
		pm.pm_from = from_offset;
		pm.pm_max = max;
		pm.pm_out = vmem_alloc(max * 2 * sizeof (uint64_t), KM_SLEEP);
		pm.pm_n = 0;
		pm.pm_next = from_offset;
		pm.pm_trunc = B_FALSE;
		zfs_range_tree_walk(rt, parity_map_emit, &pm);

		fnvlist_add_uint64(out, ZPARITY_VERSION,
		    ZPARITY_VERSION_CURRENT);
		fnvlist_add_uint64_array(out, ZPARITY_RANGES, pm.pm_out,
		    pm.pm_n * 2);
		if (pm.pm_trunc) {
			fnvlist_add_uint64(out, ZPARITY_NEXT_OFFSET,
			    pm.pm_next);
			fnvlist_add_boolean_value(out, ZPARITY_DONE, B_FALSE);
		} else {
			fnvlist_add_uint64(out, ZPARITY_NEXT_OFFSET,
			    MAX(window_end, from_offset));
			fnvlist_add_boolean_value(out, ZPARITY_DONE,
			    window_end >= asize);
		}
		vmem_free(pm.pm_out, max * 2 * sizeof (uint64_t));
	}
	zfs_range_tree_vacate(rt, NULL, NULL);
	zfs_range_tree_destroy(rt);
	return (error);
}

typedef struct parity_block {
	avl_node_t	pb_node;
	uint64_t	pb_offset;
	uint64_t	pb_len;
	uint64_t	pb_entry[PARITY_ENTRY_WORDS];
} parity_block_t;

typedef struct parity_blocks_arg {
	spa_t		*pba_spa;
	parity_vdev_info_t pba_vdev;
	uint64_t	pba_txg;
	uint64_t	pba_from;
	uint64_t	pba_max;
	uint64_t	pba_count;
	boolean_t	pba_windowed;
	const uint64_t	*pba_windows;
	uint_t		pba_nwin;
	boolean_t	pba_more;
	boolean_t	pba_resume_valid;
	uint64_t	pba_resume[PARITY_BLOCKS_RESUME_WORDS];
	int		pba_error;
	avl_tree_t	pba_blocks;
} parity_blocks_arg_t;

static int
parity_block_compare(const void *a, const void *b)
{
	const parity_block_t *x = a, *y = b;

	return (TREE_CMP(x->pb_offset, y->pb_offset));
}

/* Keep the first pba_max distinct data offsets for this page. */
static int
parity_blocks_add_bp(parity_blocks_arg_t *pba, const blkptr_t *bp)
{
	boolean_t on_top = B_FALSE;

	if (BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp) || BP_IS_REDACTED(bp) ||
	    BP_GET_BIRTH(bp) > pba->pba_txg)
		return (0);
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		const dva_t *dva = &bp->blk_dva[d];

		if (DVA_GET_VDEV(dva) == pba->pba_vdev.pvi_id) {
			on_top = B_TRUE;
			break;
		}
	}
	if (!on_top)
		return (0);
	if (zfs_blkptr_verify(pba->pba_spa, bp, BLK_CONFIG_NEEDED,
	    BLK_VERIFY_ONLY) != 0)
		return (SET_ERROR(ECKSUM));

	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		const dva_t *dva = &bp->blk_dva[d];
		parity_block_t key, *block, *last;
		avl_index_t where;
		uint64_t len, end, flags;

		if (DVA_GET_VDEV(dva) != pba->pba_vdev.pvi_id)
			continue;
		len = parity_dva_len(&pba->pba_vdev, dva);
		end = DVA_GET_OFFSET(dva) + len;
		if (len == 0 || end < DVA_GET_OFFSET(dva) ||
		    end > pba->pba_vdev.pvi_asize)
			return (SET_ERROR(ECKSUM));
		if (DVA_GET_OFFSET(dva) < pba->pba_from)
			continue;

		memset(&key, 0, sizeof (key));
		key.pb_offset = DVA_GET_OFFSET(dva);
		if (avl_find(&pba->pba_blocks, &key, NULL) != NULL)
			continue;

		block = kmem_zalloc(sizeof (*block), KM_SLEEP);
		block->pb_offset = key.pb_offset;
		block->pb_len = len;
		flags = (BP_GET_TYPE(bp) == DMU_OT_INTENT_LOG ? 1 : 0) |
		    (DVA_GET_GANG(dva) ? 2 : 0);
		block->pb_entry[PARITY_EW_TXG] = pba->pba_txg;
		block->pb_entry[PARITY_EW_KIND] = PARITY_KIND_ALLOC |
		    (flags << 8) | ((uint64_t)d << 16) |
		    ((len >> SPA_MINBLOCKSHIFT) << 32);
		memcpy(&block->pb_entry[PARITY_EW_BP], bp, sizeof (*bp));

		if (pba->pba_count < pba->pba_max) {
			avl_find(&pba->pba_blocks, block, &where);
			avl_insert(&pba->pba_blocks, block, where);
			pba->pba_count++;
			continue;
		}

		last = avl_last(&pba->pba_blocks);
		if (block->pb_offset < last->pb_offset) {
			avl_remove(&pba->pba_blocks, last);
			kmem_free(last, sizeof (*last));
			avl_find(&pba->pba_blocks, block, &where);
			avl_insert(&pba->pba_blocks, block, where);
		} else {
			kmem_free(block, sizeof (*block));
		}
		pba->pba_more = B_TRUE;
	}
	return (0);
}

static boolean_t
parity_blocks_window_overlap(const parity_blocks_arg_t *pba, uint64_t offset,
    uint64_t end)
{
	uint_t lo = 0, hi = pba->pba_nwin;

	while (lo < hi) {
		uint_t mid = lo + (hi - lo) / 2;

		if (pba->pba_windows[mid * 2 + 1] <= offset)
			lo = mid + 1;
		else
			hi = mid;
	}
	return (lo < pba->pba_nwin &&
	    pba->pba_windows[lo * 2] < end);
}

/* Add one whole block, or stop before it so the next page starts here. */
static int
parity_blocks_add_bp_windows(parity_blocks_arg_t *pba, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const dnode_phys_t *dnp)
{
	parity_block_t candidates[SPA_DVAS_PER_BP] = { 0 };
	uint_t ncandidates = 0;
	boolean_t on_top = B_FALSE;
	uint64_t flags;

	if (BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp) || BP_IS_REDACTED(bp) ||
	    BP_GET_BIRTH(bp) > pba->pba_txg)
		return (0);
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		if (DVA_GET_VDEV(&bp->blk_dva[d]) == pba->pba_vdev.pvi_id) {
			on_top = B_TRUE;
			break;
		}
	}
	if (!on_top)
		return (0);
	if (zfs_blkptr_verify(pba->pba_spa, bp, BLK_CONFIG_NEEDED,
	    BLK_VERIFY_ONLY) != 0)
		return (SET_ERROR(ECKSUM));

	flags = (BP_GET_TYPE(bp) == DMU_OT_INTENT_LOG ? 1 : 0);
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		const dva_t *dva = &bp->blk_dva[d];
		parity_block_t *candidate;
		uint64_t len, end, offset;
		boolean_t duplicate = B_FALSE;

		if (DVA_GET_VDEV(dva) != pba->pba_vdev.pvi_id)
			continue;
		len = parity_dva_len(&pba->pba_vdev, dva);
		offset = DVA_GET_OFFSET(dva);
		end = offset + len;
		if (len == 0 || end < offset || end > pba->pba_vdev.pvi_asize)
			return (SET_ERROR(ECKSUM));
		if (!parity_blocks_window_overlap(pba, offset, end))
			continue;

		candidate = &candidates[ncandidates];
		candidate->pb_offset = offset;
		if (avl_find(&pba->pba_blocks, candidate, NULL) != NULL)
			continue;
		for (uint_t i = 0; i < ncandidates; i++) {
			if (candidates[i].pb_offset == offset) {
				duplicate = B_TRUE;
				break;
			}
		}
		if (duplicate)
			continue;

		candidate->pb_len = len;
		candidate->pb_entry[PARITY_EW_TXG] = pba->pba_txg;
		candidate->pb_entry[PARITY_EW_KIND] = PARITY_KIND_ALLOC |
		    (flags << 8) | ((DVA_GET_GANG(dva) ? 2ULL : 0) << 8) |
		    ((uint64_t)d << 16) |
		    ((len >> SPA_MINBLOCKSHIFT) << 32);
		memcpy(&candidate->pb_entry[PARITY_EW_BP], bp, sizeof (*bp));
		ncandidates++;
	}
	if (pba->pba_count + ncandidates > pba->pba_max) {
		/* Resume at this block, normalized for traverse_impl(). */
		pba->pba_resume[0] = zb->zb_objset;
		pba->pba_resume[1] = zb->zb_objset;
		pba->pba_resume[2] = zb->zb_object;
		pba->pba_resume[3] = 0;
		pba->pba_resume[4] = zb->zb_blkid;
		if (zb->zb_level > 0) {
			ASSERT(dnp != NULL);
			pba->pba_resume[4] <<= zb->zb_level *
			    (dnp->dn_indblkshift - SPA_BLKPTRSHIFT);
		}
		pba->pba_resume_valid = B_TRUE;
		return (SET_ERROR(PARITY_BLOCKS_STOP));
	}

	for (uint_t i = 0; i < ncandidates; i++) {
		parity_block_t *block;
		avl_index_t where;

		block = kmem_alloc(sizeof (*block), KM_SLEEP);
		memcpy(block, &candidates[i], sizeof (*block));
		avl_find(&pba->pba_blocks, block, &where);
		avl_insert(&pba->pba_blocks, block, where);
		pba->pba_count++;
	}
	return (0);
}

static int
parity_blocks_cb(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const dnode_phys_t *dnp, void *arg)
{
	parity_blocks_arg_t *pba = arg;
	int error;

	(void) spa;
	if (pba->pba_error != 0)
		return (pba->pba_error);
	if (pba->pba_windowed && (zilog != NULL ||
	    BP_GET_TYPE(bp) == DMU_OT_INTENT_LOG))
		return (0);
	if (pba->pba_windowed)
		error = parity_blocks_add_bp_windows(pba, bp, zb, dnp);
	else
		error = parity_blocks_add_bp(pba, bp);
	if (error != 0 && !pba->pba_resume_valid)
		pba->pba_error = error;
	return (error);
}

static void
parity_blocks_walk_start(spa_t *spa, uint_t nwin)
{
	atomic_add_64(&spa->spa_parity_blocks_walks, 1);
	zfs_dbgmsg("parity: pool '%s': BLOCKS walk with %u windows",
	    spa_name(spa), nwin);
}

static void
parity_blocks_destroy(parity_blocks_arg_t *pba)
{
	parity_block_t *block;
	void *cookie = NULL;

	while ((block = avl_destroy_nodes(&pba->pba_blocks, &cookie)) != NULL)
		kmem_free(block, sizeof (*block));
	avl_destroy(&pba->pba_blocks);
}

/* Find the saved state before the first record after txg. */
static int
parity_blocks_uberblock(spa_parity_t *ps, uint64_t txg, uberblock_t *ub)
{
	spa_lag_t *lag = ps->ps_lag;
	spa_lag_rec_t *lr;
	boolean_t found = B_FALSE;

	spa_lag_lock(lag);
	for (lr = spa_lag_rec_first(lag); lr != NULL;
	    lr = spa_lag_rec_next(lag, lr)) {
		if (lr->lr_txg > txg) {
			memcpy(ub, lr->lr_phys.pr_prev_ub, sizeof (*ub));
			found = B_TRUE;
			break;
		}
	}
	spa_lag_unlock(lag);

	if (!found) {
		/* Use two equal copies of the last synced uberblock. */
		for (int i = 0; i < 4; i++) {
			uberblock_t a = ps->ps_spa->spa_ubsync;
			uberblock_t b = ps->ps_spa->spa_ubsync;

			if (memcmp(&a, &b, sizeof (a)) == 0) {
				*ub = a;
				found = B_TRUE;
				break;
			}
		}
	}
	if (!found || ub->ub_magic != UBERBLOCK_MAGIC || ub->ub_txg < txg)
		return (SET_ERROR(ESTALE));
	return (0);
}

/*
 * Return every data-device DVA in the block tree at txg.  A recordless gap
 * leaves the member state unchanged.  The next record's saved uberblock is
 * therefore the tree root for that state, and its held frees keep the blocks
 * readable until this pinned walk ends.
 */
int
spa_parity_ioc_blocks(spa_t *spa, uint64_t vdev, uint64_t txg,
    uint64_t from_offset, uint64_t max_entries, nvlist_t *out)
{
	spa_parity_t *ps = spa->spa_parity;
	spa_parity_info_t pi;
	parity_vdev_info_t pvi;
	uberblock_t ub;
	parity_blocks_arg_t pba = { 0 };
	uint64_t last, *entries, next;
	uint64_t nout = 0;
	int error = 0;

	if (ps == NULL || parity_vdev_info(spa, vdev, &pvi) != 0)
		return (SET_ERROR(ENOTSUP));
	max_entries = MIN(MAX(max_entries, 1), PARITY_BLOCKS_MAX);
	parity_pin(ps);
	spa_parity_get_info(spa, &pi);
	if (pi.pi_state == PARITY_OFF && pi.pi_nrecs == 0) {
		parity_unpin(ps);
		return (SET_ERROR(ENOTSUP));
	}
	last = spa_last_synced_txg(spa);
	if (txg + 1 < parity_history_txg(&pi) || txg > pi.pi_applied_txg ||
	    txg > last) {
		parity_unpin(ps);
		return (SET_ERROR(EINVAL));
	}

	error = parity_blocks_uberblock(ps, txg, &ub);
	if (error != 0) {
		parity_unpin(ps);
		return (error);
	}
	pba.pba_spa = spa;
	pba.pba_vdev = pvi;
	pba.pba_txg = txg;
	pba.pba_from = from_offset;
	pba.pba_max = max_entries;
	avl_create(&pba.pba_blocks, parity_block_compare,
	    sizeof (parity_block_t), offsetof(parity_block_t, pb_node));

	if (from_offset < pvi.pvi_asize) {
		parity_blocks_walk_start(spa, 0);
		error = traverse_pool_at_uberblock(spa, &ub,
		    TRAVERSE_PRE | TRAVERSE_NO_DECRYPT,
		    parity_blocks_cb, &pba);
		if (error == 0)
			error = pba.pba_error;
	}
	parity_unpin(ps);

	if (error == 0) {
		entries = vmem_alloc(max_entries * PARITY_ENTRY_WORDS *
		    sizeof (uint64_t), KM_SLEEP);
		next = from_offset;
		for (parity_block_t *block = avl_first(&pba.pba_blocks);
		    block != NULL; block = AVL_NEXT(&pba.pba_blocks, block)) {
			memcpy(&entries[nout * PARITY_ENTRY_WORDS],
			    block->pb_entry, sizeof (block->pb_entry));
			next = block->pb_offset + block->pb_len;
			nout++;
		}
		fnvlist_add_uint64(out, ZPARITY_VERSION,
		    ZPARITY_VERSION_CURRENT);
		fnvlist_add_uint64_array(out, ZPARITY_ENTRIES, entries,
		    nout * PARITY_ENTRY_WORDS);
		fnvlist_add_uint64(out, ZPARITY_NEXT_OFFSET,
		    pba.pba_more ? next : pvi.pvi_asize);
		fnvlist_add_boolean_value(out, ZPARITY_DONE, !pba.pba_more);
		vmem_free(entries, max_entries * PARITY_ENTRY_WORDS *
		    sizeof (uint64_t));
	}
	parity_blocks_destroy(&pba);
	return (error);
}

/*
 * Return the data-device DVAs that overlap a list of windows at txg.  Each
 * page contains whole blocks and resumes at the first block that did not fit.
 */
int
spa_parity_ioc_blocks_windows(spa_t *spa, uint64_t vdev, uint64_t txg,
    const uint64_t *windows, uint_t nwin,
    const uint64_t resume[PARITY_BLOCKS_RESUME_WORDS],
    uint64_t max_entries, nvlist_t *out)
{
	spa_parity_t *ps = spa->spa_parity;
	spa_parity_info_t pi;
	parity_vdev_info_t pvi;
	uberblock_t ub;
	parity_blocks_arg_t pba = { 0 };
	zbookmark_phys_t resume_zb;
	zbookmark_phys_t *resume_ptr = NULL;
	uint64_t resume_ds_obj = 0;
	uint64_t last, *entries;
	uint64_t nout = 0;
	int error;

	if (nwin > PARITY_BLOCKS_WINDOWS_MAX ||
	    (nwin != 0 && windows == NULL))
		return (SET_ERROR(EINVAL));
	for (uint_t i = 0; i < nwin; i++) {
		uint64_t lo = windows[i * 2];
		uint64_t hi = windows[i * 2 + 1];

		if (lo >= hi || (i != 0 && lo < windows[i * 2 - 1]))
			return (SET_ERROR(EINVAL));
	}
	if (resume != NULL) {
		boolean_t zero = B_TRUE;

		for (uint_t i = 0; i < PARITY_BLOCKS_RESUME_WORDS; i++)
			zero &= (resume[i] == 0);
		if (!zero) {
			if (resume[0] != resume[1] || resume[3] != 0)
				return (SET_ERROR(EINVAL));
			resume_ds_obj = resume[0];
			resume_zb.zb_objset = resume[1];
			resume_zb.zb_object = resume[2];
			resume_zb.zb_level = (int64_t)resume[3];
			resume_zb.zb_blkid = resume[4];
			resume_ptr = &resume_zb;
		}
	}
	if (ps == NULL || parity_vdev_info(spa, vdev, &pvi) != 0)
		return (SET_ERROR(ENOTSUP));
	for (uint_t i = 0; i < nwin; i++) {
		if (windows[i * 2 + 1] > pvi.pvi_asize)
			return (SET_ERROR(EINVAL));
	}
	max_entries = MIN(MAX(max_entries, SPA_DVAS_PER_BP),
	    PARITY_BLOCKS_MAX);
	parity_pin(ps);
	spa_parity_get_info(spa, &pi);
	if (pi.pi_state == PARITY_OFF && pi.pi_nrecs == 0) {
		parity_unpin(ps);
		return (SET_ERROR(ENOTSUP));
	}
	last = spa_last_synced_txg(spa);
	if (txg + 1 < parity_history_txg(&pi) || txg > pi.pi_applied_txg ||
	    txg > last) {
		parity_unpin(ps);
		return (SET_ERROR(EINVAL));
	}
	error = parity_blocks_uberblock(ps, txg, &ub);
	if (error != 0) {
		parity_unpin(ps);
		return (error);
	}
	pba.pba_spa = spa;
	pba.pba_vdev = pvi;
	pba.pba_txg = txg;
	pba.pba_max = max_entries;
	pba.pba_windowed = B_TRUE;
	pba.pba_windows = windows;
	pba.pba_nwin = nwin;
	avl_create(&pba.pba_blocks, parity_block_compare,
	    sizeof (parity_block_t), offsetof(parity_block_t, pb_node));

	parity_blocks_walk_start(spa, nwin);
	error = traverse_pool_at_uberblock_resume(spa, &ub, resume_ds_obj,
	    resume_ptr, TRAVERSE_PRE | TRAVERSE_NO_DECRYPT,
	    parity_blocks_cb, &pba);
	if (error == PARITY_BLOCKS_STOP && pba.pba_resume_valid)
		error = 0;
	else if (error == 0)
		error = pba.pba_error;
	parity_unpin(ps);

	if (error == 0) {
		entries = vmem_alloc(max_entries * PARITY_ENTRY_WORDS *
		    sizeof (uint64_t), KM_SLEEP);
		for (parity_block_t *block = avl_first(&pba.pba_blocks);
		    block != NULL; block = AVL_NEXT(&pba.pba_blocks, block)) {
			memcpy(&entries[nout * PARITY_ENTRY_WORDS],
			    block->pb_entry, sizeof (block->pb_entry));
			nout++;
		}
		fnvlist_add_uint64(out, ZPARITY_VERSION,
		    ZPARITY_VERSION_CURRENT);
		fnvlist_add_uint64_array(out, ZPARITY_ENTRIES, entries,
		    nout * PARITY_ENTRY_WORDS);
		fnvlist_add_boolean_value(out, ZPARITY_DONE,
		    !pba.pba_resume_valid);
		if (pba.pba_resume_valid) {
			fnvlist_add_uint64_array(out, ZPARITY_RESUME,
			    pba.pba_resume, PARITY_BLOCKS_RESUME_WORDS);
		}
		vmem_free(entries, max_entries * PARITY_ENTRY_WORDS *
		    sizeof (uint64_t));
	}
	parity_blocks_destroy(&pba);
	return (error);
}

typedef struct parity_current_arg {
	spa_t		*pca_spa;
	parity_vdev_info_t pca_vdev;
	uint64_t	pca_offset;
	uint64_t	pca_txg;
	uint64_t	pca_entry[PARITY_ENTRY_WORDS];
	boolean_t	pca_found;
} parity_current_arg_t;

static int
parity_current_cb(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const dnode_phys_t *dnp, void *arg)
{
	parity_current_arg_t *pca = arg;

	(void) spa, (void) zilog, (void) zb, (void) dnp;
	if (BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp) || BP_IS_REDACTED(bp) ||
	    BP_GET_BIRTH(bp) > pca->pca_txg)
		return (0);
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		const dva_t *dva = &bp->blk_dva[d];
		uint64_t len, sectors, flags;

		if (DVA_GET_VDEV(dva) != pca->pca_vdev.pvi_id ||
		    DVA_GET_OFFSET(dva) != pca->pca_offset)
			continue;
		if (zfs_blkptr_verify(pca->pca_spa, bp, BLK_CONFIG_NEEDED,
		    BLK_VERIFY_ONLY) != 0)
			return (SET_ERROR(ECKSUM));

		len = parity_dva_len(&pca->pca_vdev, dva);
		sectors = len >> SPA_MINBLOCKSHIFT;
		if (len == 0 ||
		    len > SPA_MAXBLOCKSIZE + SPA_OLD_GANGBLOCKSIZE ||
		    (len & (SPA_MINBLOCKSIZE - 1)) != 0 ||
		    sectors > UINT32_MAX ||
		    DVA_GET_OFFSET(dva) + len < DVA_GET_OFFSET(dva) ||
		    DVA_GET_OFFSET(dva) + len > pca->pca_vdev.pvi_asize)
			return (SET_ERROR(ECKSUM));

		flags = (BP_GET_TYPE(bp) == DMU_OT_INTENT_LOG ? 1 : 0) |
		    (DVA_GET_GANG(dva) ? 2 : 0);
		pca->pca_entry[PARITY_EW_TXG] = pca->pca_txg;
		pca->pca_entry[PARITY_EW_KIND] = PARITY_KIND_ALLOC |
		    (flags << 8) | ((uint64_t)d << 16) | (sectors << 32);
		memcpy(&pca->pca_entry[PARITY_EW_BP], bp, sizeof (*bp));
		pca->pca_found = B_TRUE;
		return (SET_ERROR(EEXIST));
	}
	return (0);
}

/*
 * Return the live block pointer for one data-device DVA offset.
 */
int
spa_parity_ioc_current(spa_t *spa, uint64_t vdev, uint64_t offset,
    nvlist_t *out)
{
	spa_parity_t *ps = spa->spa_parity;
	spa_parity_info_t pi;
	parity_current_arg_t pca = { 0 };
	parity_vdev_info_t pvi;
	uberblock_t ub;
	uint64_t none = 0;
	int error;

	if (ps == NULL || parity_vdev_info(spa, vdev, &pvi) != 0)
		return (SET_ERROR(ENOTSUP));
	if (offset >= pvi.pvi_asize ||
	    (offset & (SPA_MINBLOCKSIZE - 1)) != 0)
		return (SET_ERROR(EINVAL));

	parity_pin(ps);
	spa_parity_get_info(spa, &pi);
	if (pi.pi_state == PARITY_OFF && pi.pi_nrecs == 0) {
		parity_unpin(ps);
		return (SET_ERROR(ENOTSUP));
	}
	spa_config_enter(spa, SCL_CONFIG, FTAG, RW_READER);
	ub = spa->spa_ubsync;
	spa_config_exit(spa, SCL_CONFIG, FTAG);
	if (ub.ub_magic != UBERBLOCK_MAGIC ||
	    ub.ub_txg > spa_last_synced_txg(spa)) {
		parity_unpin(ps);
		return (SET_ERROR(ESTALE));
	}

	pca.pca_spa = spa;
	pca.pca_vdev = pvi;
	pca.pca_offset = offset;
	pca.pca_txg = ub.ub_txg;
	error = traverse_pool_at_uberblock(spa, &ub,
	    TRAVERSE_PRE | TRAVERSE_NO_DECRYPT, parity_current_cb, &pca);
	parity_unpin(ps);
	if (error == EEXIST && pca.pca_found)
		error = 0;
	if (error != 0)
		return (error);

	fnvlist_add_uint64(out, ZPARITY_VERSION, ZPARITY_VERSION_CURRENT);
	fnvlist_add_uint64(out, ZPARITY_TXG, ub.ub_txg);
	fnvlist_add_boolean_value(out, ZPARITY_FOUND, pca.pca_found);
	fnvlist_add_uint64_array(out, ZPARITY_ENTRY,
	    pca.pca_found ? pca.pca_entry : &none,
	    pca.pca_found ? PARITY_ENTRY_WORDS : 0);
	return (0);
}

/*
 * LABEL.
 */

static boolean_t
parity_ub_magic(const uberblock_t *ub, uint64_t *txg)
{
	if (ub->ub_magic == UBERBLOCK_MAGIC) {
		*txg = ub->ub_txg;
		return (B_TRUE);
	}
	if (ub->ub_magic == BSWAP_64((uint64_t)UBERBLOCK_MAGIC)) {
		*txg = BSWAP_64(ub->ub_txg);
		return (B_TRUE);
	}
	return (B_FALSE);
}

/* Check the embedded label checksum of a piece of a label buffer. */
static boolean_t
parity_label_cksum_ok(spa_t *spa, const uint8_t *p, uint64_t size,
    uint64_t phys_off)
{
	uint8_t *copy = vmem_alloc(size, KM_SLEEP);
	abd_t *abd;
	int error;

	memcpy(copy, p, size);
	abd = abd_get_from_buf(copy, size);
	error = zio_checksum_verify_bp(spa, NULL, ZIO_CHECKSUM_LABEL, abd,
	    size, phys_off);
	abd_free(abd);
	vmem_free(copy, size);
	return (error == 0);
}

/*
 * The uberblock of a synced txg that changed nothing.  vdev_config_sync()
 * writes no uberblock for such a txg (the root block pointer did not
 * change), so its state is the state of the newest uberblock before it, and
 * the uberblock that ZFS keeps in memory for it (spa_ubsync) is that one with
 * ub_txg set to the txg.  Make the same: take the newest valid native-order
 * uberblock X < txg from the ring and set its ub_txg to txg.
 *
 * This is safe only when no uberblock of a txg in (X, txg] can be missing
 * from the ring: the ring keeps the "slots" newest uberblocks by txg, so the
 * newest uberblock in the ring must be less than X + slots.  Else B_FALSE.
 */
static boolean_t
parity_idle_ub(spa_t *spa, const uint8_t *lbuf, uint64_t len, uint64_t slots,
    uint64_t txg, uberblock_t *ubp)
{
	const uberblock_t *best = NULL;
	uint64_t best_txg = 0, newest = 0;

	for (int l = 0; l < 2; l++) {
		for (uint64_t s = 0; s < slots; s++) {
			uint64_t off = l * sizeof (vdev_label_t) +
			    offsetof(vdev_label_t, vl_uberblock) + s * len;
			const uberblock_t *ub =
			    (const uberblock_t *)(lbuf + off);
			uint64_t ubtxg;

			if (!parity_ub_magic(ub, &ubtxg) ||
			    !parity_label_cksum_ok(spa, lbuf + off, len, off))
				continue;
			newest = MAX(newest, ubtxg);
			if (ubtxg < txg && (best == NULL || ubtxg > best_txg)) {
				best = ub;
				best_txg = ubtxg;
			}
		}
	}
	/* Only a native-order copy is rewritten as it is. */
	if (best == NULL || best->ub_magic != UBERBLOCK_MAGIC ||
	    newest >= best_txg + slots)
		return (B_FALSE);
	memcpy(ubp, best, sizeof (*ubp));
	ubp->ub_txg = txg;
	return (B_TRUE);
}

/*
 * The uberblock of "txg": from the label rings if a valid copy is still
 * there, else the one a record keeps (pr_prev_ub), else the pool's current
 * one, else (for a synced txg that changed nothing) the newest one before it
 * with its txg set.  Returns B_FALSE when none is found.
 */
static boolean_t
parity_find_ub(spa_parity_t *ps, const uint8_t *lbuf, uint64_t len,
    uint64_t count, uint64_t txg, uberblock_t *ubp)
{
	spa_t *spa = ps->ps_spa;
	uint64_t m = spa_multihost(spa) ? MMP_BLOCKS_PER_LABEL : 0;
	spa_lag_t *lag = ps->ps_lag;
	spa_lag_rec_t *lr;
	boolean_t found = B_FALSE;

	for (int l = 0; l < 2 && !found; l++) {
		for (uint64_t s = 0; s < count - m && !found; s++) {
			uint64_t off = l * sizeof (vdev_label_t) +
			    offsetof(vdev_label_t, vl_uberblock) + s * len;
			const uberblock_t *ub =
			    (const uberblock_t *)(lbuf + off);
			uint64_t ubtxg;

			if (!parity_ub_magic(ub, &ubtxg) || ubtxg != txg)
				continue;
			if (!parity_label_cksum_ok(spa, lbuf + off, len, off))
				continue;
			/* Only a native-order copy is rewritten as it is. */
			if (ub->ub_magic != UBERBLOCK_MAGIC)
				continue;
			memcpy(ubp, ub, sizeof (*ubp));
			found = B_TRUE;
		}
	}

	if (!found) {
		spa_lag_lock(lag);
		for (lr = spa_lag_rec_first(lag); lr != NULL;
		    lr = spa_lag_rec_next(lag, lr)) {
			const uberblock_t *ub =
			    (const uberblock_t *)lr->lr_phys.pr_prev_ub;

			if (ub->ub_magic == UBERBLOCK_MAGIC &&
			    ub->ub_txg == txg) {
				memcpy(ubp, ub, sizeof (*ubp));
				found = B_TRUE;
				break;
			}
		}
		spa_lag_unlock(lag);
	}

	if (!found) {
		/* spa_ubsync changes without a lock: take two equal copies. */
		for (int t = 0; t < 4 && !found; t++) {
			uberblock_t a = spa->spa_ubsync;
			uberblock_t b = spa->spa_ubsync;

			if (memcmp(&a, &b, sizeof (a)) == 0 &&
			    a.ub_magic == UBERBLOCK_MAGIC && a.ub_txg == txg) {
				*ubp = a;
				found = B_TRUE;
			}
		}
	}

	if (!found && txg <= spa_last_synced_txg(spa))
		found = parity_idle_ub(spa, lbuf, len, count - m, txg, ubp);
	return (found);
}

/*
 * The txg of the newest label config in a label buffer, or 0 when no label
 * has a valid vdev_phys block.
 */
static boolean_t
parity_label_config_txg(spa_t *spa, const uint8_t *lbuf, uint64_t *txgp)
{
	boolean_t found = B_FALSE;

	*txgp = 0;
	for (int l = 0; l < 2; l++) {
		uint64_t off = l * sizeof (vdev_label_t) +
		    offsetof(vdev_label_t, vl_vdev_phys);
		const vdev_phys_t *vp = (const vdev_phys_t *)(lbuf + off);
		nvlist_t *config = NULL;
		uint64_t t;

		if (!parity_label_cksum_ok(spa, lbuf + off,
		    sizeof (vdev_phys_t), off))
			continue;
		if (nvlist_unpack((char *)vp->vp_nvlist,
		    sizeof (vp->vp_nvlist), &config, KM_SLEEP) != 0)
			continue;
		if (nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_TXG,
		    &t) == 0) {
			*txgp = MAX(*txgp, t);
			found = B_TRUE;
		}
		nvlist_free(config);
	}
	return (found);
}

int
spa_parity_ioc_label(spa_t *spa, uint64_t vdev, uint64_t txg, nvlist_t *out)
{
	spa_parity_t *ps = spa->spa_parity;
	vdev_t *top;
	vdev_t *leaf, *leaves[PARITY_MAX_LEAVES];
	uint8_t *lbuf, *slot;
	uberblock_t ub;
	uint64_t offsets[2], cfg_txg = 0, len = 0, count = 0, m, n, ub_offset;
	uint_t nleaves;
	int error = 0;
	boolean_t have_cfg = B_FALSE;

	if (ps == NULL)
		return (SET_ERROR(ENOTSUP));

	lbuf = vmem_alloc(PARITY_LABEL_BYTES, KM_SLEEP);

	/* A torn read of a label that is being written: try twice. */
	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	top = parity_col_tvd(spa, vdev);
	if (top == NULL) {
		spa_config_exit(spa, SCL_STATE, FTAG);
		vmem_free(lbuf, PARITY_LABEL_BYTES);
		return (SET_ERROR(ENXIO));
	}
	nleaves = parity_leaves(top, leaves);
	if (nleaves == 0) {
		spa_config_exit(spa, SCL_STATE, FTAG);
		vmem_free(lbuf, PARITY_LABEL_BYTES);
		return (SET_ERROR(ENXIO));
	}
	error = SET_ERROR(EIO);
	for (uint_t i = 0; i < nleaves && !have_cfg; i++) {
		leaf = leaves[i];
		if (!vdev_readable(leaf) ||
		    leaf->vdev_psize < PARITY_LABEL_BYTES)
			continue;

		len = VDEV_UBERBLOCK_SIZE(leaf);
		count = VDEV_UBERBLOCK_COUNT(leaf);
		for (int tries = 0; tries < 2 && !have_cfg; tries++) {
			abd_t *abd;

			abd = abd_alloc_linear(PARITY_LABEL_BYTES, B_FALSE);
			error = zio_wait(zio_read_phys(NULL, leaf, 0,
			    PARITY_LABEL_BYTES, abd, ZIO_CHECKSUM_OFF, NULL,
			    NULL, ZIO_PRIORITY_SYNC_READ, ZIO_FLAG_CANFAIL |
			    ZIO_FLAG_DONT_PROPAGATE, B_TRUE));
			if (error == 0)
				abd_copy_to_buf(lbuf, abd, PARITY_LABEL_BYTES);
			abd_free(abd);
			if (error != 0)
				break;
			have_cfg = parity_label_config_txg(spa, lbuf,
			    &cfg_txg);
		}
	}
	spa_config_exit(spa, SCL_STATE, FTAG);
	m = spa_multihost(spa) ? MMP_BLOCKS_PER_LABEL : 0;
	/* have_cfg: len and count come from the side that was read */
	if (error == 0 && (!have_cfg || len == 0 || count <= m))
		error = SET_ERROR(EIO);
	if (error != 0) {
		vmem_free(lbuf, PARITY_LABEL_BYTES);
		return (error);
	}

	/* The slot for this txg's uberblock, in each of the two labels. */
	n = txg % (count - m);
	ub_offset = offsetof(vdev_label_t, vl_uberblock) + n * len;
	for (int l = 0; l < 2; l++) {
		offsets[l] = l * sizeof (vdev_label_t) + ub_offset;
	}

	parity_pin(ps);
	if (!parity_find_ub(ps, lbuf, len, count, txg, &ub)) {
		parity_unpin(ps);
		vmem_free(lbuf, PARITY_LABEL_BYTES);
		return (SET_ERROR(ENOENT));
	}
	parity_unpin(ps);

	/* One image per slot: the checksum depends on the offset. */
	slot = vmem_zalloc(2 * len, KM_SLEEP);
	for (int l = 0; l < 2; l++) {
		abd_t *abd;

		memcpy(slot + l * len, &ub, sizeof (ub));
		abd = abd_get_from_buf(slot + l * len, len);
		zio_checksum_compute_label(spa, abd, len, offsets[l]);
		abd_free(abd);
	}

	fnvlist_add_uint64(out, ZPARITY_VERSION, ZPARITY_VERSION_CURRENT);
	fnvlist_add_uint64(out, ZPARITY_UB_TXG, txg);
	fnvlist_add_uint8_array(out, ZPARITY_UB_SLOT, slot, 2 * len);
	fnvlist_add_uint64_array(out, ZPARITY_UB_OFFSETS, offsets, 2);

	if (cfg_txg > txg) {
		/*
		 * The config on disk describes a newer pool: ZFS cannot make
		 * up the older one.  The caller keeps its saved label and
		 * writes the slot into it.
		 */
		error = SET_ERROR(ESTALE);
	} else {
		/* Blank the uberblocks that are newer than txg. */
		for (int l = 0; l < 2; l++) {
			for (uint64_t s = 0; s < count; s++) {
				uint8_t *p = lbuf + l * sizeof (vdev_label_t) +
				    offsetof(vdev_label_t, vl_uberblock) +
				    s * len;
				uint64_t ubtxg;

				if (parity_ub_magic((const uberblock_t *)p,
				    &ubtxg) && ubtxg > txg)
					memset(p, 0, len);
			}
		}
		/* The uberblock of txg is in its slot. */
		for (int l = 0; l < 2; l++)
			memcpy(lbuf + offsets[l], slot + l * len, len);
		fnvlist_add_uint8_array(out, ZPARITY_LABEL, lbuf,
		    PARITY_LABEL_BYTES);
	}

	vmem_free(slot, 2 * len);
	vmem_free(lbuf, PARITY_LABEL_BYTES);
	return (error);
}

/*
 * APPLIED and DROP.  A stale generation gets ESTALE.  A different set GUID
 * gets ZFS_ERR_PARITY_SET_MISMATCH.  APPLIED also refuses a txg that has not
 * synced yet.  The release itself is the core's.
 */
int
spa_parity_ioc_applied(spa_t *spa, uint64_t set_guid, uint64_t generation,
    uint64_t txg)
{
	spa_parity_t *ps = spa->spa_parity;
	spa_parity_info_t pi;
	boolean_t force;

	if (ps == NULL)
		return (SET_ERROR(ENOTSUP));
	spa_parity_get_info(spa, &pi);
	if (pi.pi_state == PARITY_OFF && pi.pi_nrecs == 0)
		return (SET_ERROR(ENOTSUP));
	if (pi.pi_set_guid != 0 && pi.pi_set_guid != set_guid)
		return (SET_ERROR(ZFS_ERR_PARITY_SET_MISMATCH));
	if (generation != pi.pi_generation)
		return (SET_ERROR(ESTALE));
	if (set_guid == 0)
		return (SET_ERROR(EINVAL));
	if (txg > spa_last_synced_txg(spa))
		return (SET_ERROR(EINVAL));

	mutex_enter(&ps->ps_lock);
	if (ps->ps_set_guid != 0 && ps->ps_set_guid != set_guid) {
		mutex_exit(&ps->ps_lock);
		return (SET_ERROR(ZFS_ERR_PARITY_SET_MISMATCH));
	}
	if (ps->ps_generation != generation) {
		mutex_exit(&ps->ps_lock);
		return (SET_ERROR(ESTALE));
	}
	if (ps->ps_set_guid == 0) {
		/*
		 * With no record yet, the sync pass makes the directory that
		 * holds the GUID, but only while parity is on.
		 */
		if (!spa_lag_dir_exists(ps->ps_lag) &&
		    ps->ps_state != PARITY_ON) {
			mutex_exit(&ps->ps_lock);
			return (SET_ERROR(ENOTSUP));
		}
		ps->ps_set_guid = set_guid;
		ps->ps_set_guid_dirty = B_TRUE;
	}
	force = ps->ps_set_guid_dirty;
	mutex_exit(&ps->ps_lock);

	if (!force)
		return (spa_lag_applied(ps->ps_lag, txg, 0));
	do {
		int error = spa_lag_applied(ps->ps_lag, txg, SPA_LAG_FORCE);

		if (error != 0)
			return (error);
		mutex_enter(&ps->ps_lock);
		if (ps->ps_set_guid == set_guid)
			ps->ps_set_guid_dirty = B_FALSE;
		force = ps->ps_set_guid_dirty;
		mutex_exit(&ps->ps_lock);
	} while (force);
	return (0);
}

int
spa_parity_ioc_drop(spa_t *spa, uint64_t set_guid, uint64_t generation)
{
	spa_parity_t *ps = spa->spa_parity;
	spa_parity_info_t pi;

	if (ps == NULL)
		return (SET_ERROR(ENOTSUP));
	spa_parity_get_info(spa, &pi);
	if (pi.pi_state == PARITY_OFF && pi.pi_nrecs == 0)
		return (SET_ERROR(ENOTSUP));
	if (pi.pi_set_guid != set_guid &&
	    (pi.pi_set_guid != 0 || set_guid != 0))
		return (SET_ERROR(ZFS_ERR_PARITY_SET_MISMATCH));
	if (generation != pi.pi_generation)
		return (SET_ERROR(ESTALE));
	mutex_enter(&ps->ps_lock);
	if (ps->ps_set_guid != set_guid &&
	    (ps->ps_set_guid != 0 || set_guid != 0)) {
		mutex_exit(&ps->ps_lock);
		return (SET_ERROR(ZFS_ERR_PARITY_SET_MISMATCH));
	}
	if (ps->ps_generation != generation) {
		mutex_exit(&ps->ps_lock);
		return (SET_ERROR(ESTALE));
	}
	mutex_exit(&ps->ps_lock);
	return (spa_lag_drop(spa));
}

/*
 * RECLAIM frees leaked held ranges in one transaction.  The CLI uploads
 * bounded batches, then commits the complete list with its pool GUID and the
 * txg from zdb.  The temporary list is not persisted and ends on export,
 * abort, or commit.
 */
typedef struct parity_reclaim {
	spa_t		*prc_spa;
	const uint64_t	*prc_r;		/* (vdev, offset, size) */
	uint_t		prc_n;
	uint64_t	prc_total;
	uint64_t	prc_left;
	boolean_t	prc_reset;
} parity_reclaim_t;

static uint64_t
parity_leaked_get(spa_t *spa)
{
	return (spa_parity_leaked_bytes(spa));
}

void
spa_parity_reclaim_clear(spa_t *spa)
{
	mutex_enter(&spa->spa_parity_reclaim_lock);
	if (spa->spa_parity_reclaim_ranges != NULL) {
		kmem_free(spa->spa_parity_reclaim_ranges,
		    spa->spa_parity_reclaim_n * 3 * sizeof (uint64_t));
		spa->spa_parity_reclaim_ranges = NULL;
	}
	spa->spa_parity_reclaim_guid = 0;
	spa->spa_parity_reclaim_txg = 0;
	spa->spa_parity_reclaim_total = 0;
	spa->spa_parity_reclaim_n = 0;
	spa->spa_parity_reclaim_active = B_FALSE;
	mutex_exit(&spa->spa_parity_reclaim_lock);
}

static void
parity_reclaim_clear_locked(spa_t *spa)
{
	if (spa->spa_parity_reclaim_ranges != NULL) {
		kmem_free(spa->spa_parity_reclaim_ranges,
		    spa->spa_parity_reclaim_n * 3 * sizeof (uint64_t));
		spa->spa_parity_reclaim_ranges = NULL;
	}
	spa->spa_parity_reclaim_guid = 0;
	spa->spa_parity_reclaim_txg = 0;
	spa->spa_parity_reclaim_total = 0;
	spa->spa_parity_reclaim_n = 0;
	spa->spa_parity_reclaim_active = B_FALSE;
}

/*
 * Does [off, off + len) on vdev touch a range?  The ranges are sorted by
 * (vdev, offset).
 */
static boolean_t
parity_reclaim_hits(const uint64_t *r, uint_t n, uint64_t vdev, uint64_t off,
    uint64_t len)
{
	uint_t lo = 0, hi = n;

	while (lo < hi) {
		uint_t mid = lo + (hi - lo) / 2;

		if (r[mid * 3] < vdev || (r[mid * 3] == vdev &&
		    r[mid * 3 + 1] + r[mid * 3 + 2] <= off))
			lo = mid + 1;
		else
			hi = mid;
	}
	return (lo < n && r[lo * 3] == vdev && r[lo * 3 + 1] < off + len);
}

/*
 * Does a held free of a live record touch a range?  0, EBUSY, or the error of
 * a record read (a record that cannot be read proves nothing).
 */
static int
parity_reclaim_held(spa_parity_t *ps, const parity_cols_t *pc,
    const uint64_t *r, uint_t n)
{
	spa_lag_t *lag = ps->ps_lag;
	uint64_t *buf;
	int error = 0;

	buf = vmem_alloc(PARITY_FLUSH_CHUNK * PARITY_ENTRY_BYTES, KM_SLEEP);
	parity_pin(ps);
	for (spa_lag_rec_t *lr = spa_lag_rec_first(lag);
	    lr != NULL && error == 0; lr = spa_lag_rec_next(lag, lr)) {
		uint64_t i = lr->lr_phys.pr_rel_index;
		uint64_t nent = lr->lr_phys.pr_nentries;

		while (i < nent && error == 0) {
			uint64_t c = MIN(nent - i, PARITY_FLUSH_CHUNK);

			error = spa_lag_rec_read(lag, lr, i, c, buf);
			for (uint64_t j = 0; j < c && error == 0; j++) {
				const uint64_t *w =
				    &buf[j * PARITY_ENTRY_WORDS];
				const blkptr_t *bp =
				    (const blkptr_t *)&w[PARITY_EW_BP];

				if (w[PARITY_EW_KIND] != PARITY_KIND_FREE ||
				    BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp))
					continue;
				for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
					const dva_t *dva = &bp->blk_dva[d];
					const parity_vdev_info_t *pv =
					    parity_col(pc, DVA_GET_VDEV(dva));

					if (pv == NULL)
						continue;
					uint64_t off = DVA_GET_OFFSET(dva);
					uint64_t len = MAX(DVA_GET_ASIZE(dva),
					    parity_dva_len(pv, dva));

					if (off + len < off ||
					    off + len > pv->pvi_asize) {
						error = SET_ERROR(ECKSUM);
						break;
					}
					if (parity_reclaim_hits(r, n,
					    pv->pvi_id, off, len))
						error = SET_ERROR(EBUSY);
				}
			}
			i += c;
		}
	}
	parity_unpin(ps);
	vmem_free(buf, PARITY_FLUSH_CHUNK * PARITY_ENTRY_BYTES);
	return (error);
}

static int
parity_reclaim_check(void *arg, dmu_tx_t *tx)
{
	parity_reclaim_t *prc = arg;
	spa_t *spa = prc->prc_spa;
	uint64_t leaked = parity_leaked_get(spa);
	int error = 0;

	(void) tx;
	if (leaked == 0)
		return (SET_ERROR(ENOENT));
	if (prc->prc_reset)
		return (0);
	if (prc->prc_n == 0)
		return (SET_ERROR(EINVAL));
	if (prc->prc_total < leaked)
		return (SET_ERROR(EINVAL));
	if (prc->prc_total > leaked)
		return (SET_ERROR(EOVERFLOW));
	if (spa_has_checkpoint(spa))
		return (SET_ERROR(ZFS_ERR_CHECKPOINT_EXISTS));
	for (uint_t i = 0; i < prc->prc_n && error == 0; i++) {
		error = metaslab_range_allocated(spa, prc->prc_r[i * 3],
		    prc->prc_r[i * 3 + 1], prc->prc_r[i * 3 + 2]);
	}
	return (error);
}

static void
parity_reclaim_sync(void *arg, dmu_tx_t *tx)
{
	parity_reclaim_t *prc = arg;
	spa_t *spa = prc->prc_spa;

	if (!prc->prc_reset) {
		for (uint_t i = 0; i < prc->prc_n; i++) {
			metaslab_free_range(spa, prc->prc_r[i * 3],
			    prc->prc_r[i * 3 + 1], prc->prc_r[i * 3 + 2]);
		}
	}
	prc->prc_left = 0;
	VERIFY0(zap_update(spa->spa_meta_objset, DMU_POOL_DIRECTORY_OBJECT,
	    PARITY_LEAKED_NAME, sizeof (uint64_t), 1, &prc->prc_left, tx));
	atomic_store_64(&spa->spa_parity_leaked, prc->prc_left);
	spa_history_log_internal(spa, "parity reclaim", tx,
	    "%llu bytes in %u ranges freed, %llu leaked bytes left",
	    (u_longlong_t)prc->prc_total, prc->prc_n,
	    (u_longlong_t)prc->prc_left);
}

static int
parity_reclaim_ranges_check(const parity_cols_t *pc,
    const uint64_t *old_ranges, const uint64_t *r, uint_t nwords,
    uint_t old_n, uint64_t *total)
{
	uint_t n;

	if (nwords == 0 || nwords % 3 != 0 ||
	    nwords / 3 > PARITY_RECLAIM_BATCH_MAX)
		return (SET_ERROR(EINVAL));
	n = nwords / 3;
	if ((uint64_t)old_n + n > PARITY_RECLAIM_RANGES_MAX)
		return (SET_ERROR(E2BIG));
	for (uint_t i = 0; i < n; i++) {
		uint64_t vdev = r[i * 3], off = r[i * 3 + 1];
		uint64_t len = r[i * 3 + 2];
		uint64_t prev_vdev, prev_off, prev_len;
		/* each range is on a data column, checked on that one */
		const parity_vdev_info_t *pv = parity_col(pc, vdev);

		if (pv == NULL || len == 0 ||
		    off + len < off || off + len > pv->pvi_asize ||
		    *total + len < *total)
			return (SET_ERROR(EINVAL));
		if (i > 0) {
			prev_vdev = r[(i - 1) * 3];
			prev_off = r[(i - 1) * 3 + 1];
			prev_len = r[(i - 1) * 3 + 2];
		} else if (old_n != 0) {
			prev_vdev = old_ranges[(old_n - 1) * 3];
			prev_off = old_ranges[(old_n - 1) * 3 + 1];
			prev_len = old_ranges[(old_n - 1) * 3 + 2];
		} else {
			prev_vdev = 0;
			prev_off = 0;
			prev_len = 0;
		}
		/* sorted by (vdev, offset), not overlapping */
		if ((i != 0 || old_n != 0) && (prev_vdev > vdev ||
		    (prev_vdev == vdev && prev_off + prev_len > off)))
			return (SET_ERROR(EINVAL));
		*total += len;
	}
	return (0);
}

int
spa_parity_ioc_reclaim(spa_t *spa, uint64_t txg, uint64_t pool_guid,
    uint64_t flags, const uint64_t *ranges, uint_t nwords, nvlist_t *out)
{
	spa_parity_t *ps = spa->spa_parity;
	parity_cols_t *pc = NULL;
	parity_reclaim_t prc = { .prc_spa = spa };
	uint64_t total, leaked;
	uint_t n;
	int error = 0;

	if (!spa_writeable(spa))
		return (SET_ERROR(EROFS));
	if (pool_guid != spa_guid(spa))
		return (SET_ERROR(EINVAL));
	if (flags != ZPARITY_RECLAIM_STAGE &&
	    flags != ZPARITY_RECLAIM_COMMIT &&
	    flags != ZPARITY_RECLAIM_ABORT &&
	    flags != ZPARITY_RECLAIM_RESET)
		return (SET_ERROR(EINVAL));
	if (flags != ZPARITY_RECLAIM_RESET &&
	    flags != ZPARITY_RECLAIM_ABORT) {
		if (txg == UINT64_MAX || txg + 1 != spa_first_txg(spa))
			return (SET_ERROR(ESTALE));
		if ((pc = parity_cols_load(spa)) == NULL)
			return (SET_ERROR(ENXIO));
	}
	if (flags == ZPARITY_RECLAIM_RESET) {
		if (nwords != 0)
			return (SET_ERROR(EINVAL));
		prc.prc_reset = B_TRUE;
	} else if (flags == ZPARITY_RECLAIM_ABORT) {
		if (nwords != 0)
			return (SET_ERROR(EINVAL));
		spa_parity_reclaim_clear(spa);
		return (0);
	} else if (flags == ZPARITY_RECLAIM_STAGE) {
		mutex_enter(&spa->spa_parity_reclaim_lock);
		if (spa->spa_parity_reclaim_active &&
		    (spa->spa_parity_reclaim_guid != pool_guid ||
		    spa->spa_parity_reclaim_txg != txg)) {
			error = SET_ERROR(EBUSY);
			goto stage_out;
		}
		n = nwords / 3;
		total = spa->spa_parity_reclaim_total;
		if (nwords == 0 || nwords % 3 != 0) {
			error = SET_ERROR(EINVAL);
			goto stage_out;
		}
		if (n > PARITY_RECLAIM_BATCH_MAX) {
			error = SET_ERROR(E2BIG);
			goto stage_out;
		}
		error = parity_reclaim_ranges_check(pc,
		    spa->spa_parity_reclaim_ranges, ranges, nwords,
		    spa->spa_parity_reclaim_n, &total);
		if (error != 0)
			goto stage_out;
		leaked = parity_leaked_get(spa);
		if (leaked == 0) {
			error = SET_ERROR(ENOENT);
			goto stage_out;
		}
		if (total > leaked) {
			error = SET_ERROR(EOVERFLOW);
			goto stage_out;
		}
		uint_t new_n = spa->spa_parity_reclaim_n + n;
		uint64_t *new_ranges = kmem_alloc(new_n * 3 *
		    sizeof (uint64_t), KM_SLEEP);
		if (spa->spa_parity_reclaim_n != 0)
			memcpy(new_ranges, spa->spa_parity_reclaim_ranges,
			    spa->spa_parity_reclaim_n * 3 * sizeof (uint64_t));
		memcpy(&new_ranges[spa->spa_parity_reclaim_n * 3], ranges,
		    nwords * sizeof (uint64_t));
		if (spa->spa_parity_reclaim_ranges != NULL)
			kmem_free(spa->spa_parity_reclaim_ranges,
			    spa->spa_parity_reclaim_n * 3 * sizeof (uint64_t));
		spa->spa_parity_reclaim_ranges = new_ranges;
		spa->spa_parity_reclaim_n = new_n;
		spa->spa_parity_reclaim_total = total;
		spa->spa_parity_reclaim_guid = pool_guid;
		spa->spa_parity_reclaim_txg = txg;
		spa->spa_parity_reclaim_active = B_TRUE;
		fnvlist_add_uint64(out, ZPARITY_RECLAIM_STAGED, new_n);
stage_out:
		mutex_exit(&spa->spa_parity_reclaim_lock);
		parity_cols_free(pc);
		return (error);
	}

	/*
	 * Commit or explicit reset.  Do not release the staged array until the
	 * syncing task has finished reading it.
	 */
	mutex_enter(&spa->spa_parity_reclaim_lock);
	if (flags == ZPARITY_RECLAIM_COMMIT) {
		if (nwords != 0 || !spa->spa_parity_reclaim_active ||
		    spa->spa_parity_reclaim_guid != pool_guid ||
		    spa->spa_parity_reclaim_txg != txg) {
			error = SET_ERROR(EINVAL);
			goto commit_out;
		}
		prc.prc_r = spa->spa_parity_reclaim_ranges;
		prc.prc_n = spa->spa_parity_reclaim_n;
		prc.prc_total = spa->spa_parity_reclaim_total;
		if (prc.prc_n == 0) {
			error = SET_ERROR(EINVAL);
			goto commit_out;
		}
		if (ps != NULL && (error = parity_reclaim_held(ps, pc,
		    prc.prc_r, prc.prc_n)) != 0)
			goto commit_out;
	} else if (nwords != 0) {
		error = SET_ERROR(EINVAL);
		goto commit_out;
	}
	error = dsl_sync_task(spa_name(spa), parity_reclaim_check,
	    parity_reclaim_sync, &prc, 0, ZFS_SPACE_CHECK_NONE);
	if (error == 0) {
		fnvlist_add_uint64(out, ZPARITY_RECLAIMED, prc.prc_total);
		fnvlist_add_uint64(out, ZPARITY_LEAKED_BYTES, prc.prc_left);
	}
commit_out:
	parity_reclaim_clear_locked(spa);
	mutex_exit(&spa->spa_parity_reclaim_lock);
	parity_cols_free(pc);
	return (error);
}

/*
 * REPAIR: the repair code checks the bytes against the request and writes.
 */
int
spa_parity_ioc_repair(spa_t *spa, uint64_t request, const uint8_t *data,
    uint_t len, boolean_t *repaired)
{
	*repaired = B_FALSE;
	if (spa->spa_parity == NULL)
		return (SET_ERROR(ENOTSUP));
	if (len == 0 || len > SPA_MAXBLOCKSIZE)
		return (SET_ERROR(EINVAL));
	return (spa_parity_repair(spa, request, data, len, repaired));
}

/*
 * The event "parity_records": a txg with records has synced.  It wakes the
 * parity service; events may be combined, so it is rate limited.
 */
boolean_t
spa_parity_event_records(spa_t *spa, uint64_t txg)
{
	spa_parity_t *ps = spa->spa_parity;
	nvlist_t *aux;
	hrtime_t now = gethrtime();

	if (ps == NULL)
		return (B_TRUE);
	/* The stamp is for this pool: the caller keeps a held back event. */
	if (ps->ps_event_last != 0 &&
	    now - ps->ps_event_last < MSEC2NSEC((hrtime_t)zfs_parity_event_ms))
		return (B_FALSE);
	ps->ps_event_last = now;

	aux = fnvlist_alloc();
	fnvlist_add_uint64(aux, ZPARITY_TXG, txg);
	spa_event_notify(spa, NULL, aux, ESC_ZFS_PARITY_RECORDS);
	fnvlist_free(aux);
	return (B_TRUE);
}

#if defined(_KERNEL)
EXPORT_SYMBOL(spa_parity_ioc_info);
EXPORT_SYMBOL(spa_parity_ioc_records);
EXPORT_SYMBOL(spa_parity_ioc_read);
EXPORT_SYMBOL(spa_parity_ioc_verify);
EXPORT_SYMBOL(spa_parity_ioc_map);
EXPORT_SYMBOL(spa_parity_ioc_blocks);
EXPORT_SYMBOL(spa_parity_ioc_label);
EXPORT_SYMBOL(spa_parity_ioc_applied);
EXPORT_SYMBOL(spa_parity_ioc_drop);
EXPORT_SYMBOL(spa_parity_ioc_repair);
EXPORT_SYMBOL(spa_parity_event_records);
#endif

ZFS_MODULE_PARAM(zfs_parity, zfs_parity_, event_ms, UINT, ZMOD_RW,
	"Minimum milliseconds between parity_records events");
