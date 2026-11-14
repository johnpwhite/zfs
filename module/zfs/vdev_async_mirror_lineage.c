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
 * async mirror: the lineage.
 *
 * This file holds the lineage and its fork, the load check for divergent
 * copies, the keep point (--async-mirror-keep), and the erase of uberblocks
 * from the copy that the operator discards.  The lineage rules are described
 * in the comment below.
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_async_mirror.h>
#include <sys/vdev_async_mirror_impl.h>
#include <sys/uberblock_impl.h>
#include <sys/zap.h>
#include <sys/zfeature.h>
#include <sys/dmu.h>
#include <sys/dmu_tx.h>
#include <sys/dsl_pool.h>
#include <sys/fs/zfs.h>

/*
 * The lineage.
 *
 * The pool selects the uberblock with the highest txg on any child.  If the
 * children have divergent histories, a writable import can select one and
 * silently lose the writes from the other.  The lagging child is a complete
 * older copy that an operator can import on its own, so the load checks the
 * lineage before it accepts both children.
 *
 * A lineage is a random number that names one line of history.  The MOS
 * holds it in the lineage entry, async_mirror_lineage_phys_t.  A lineage
 * starts with the async mirror: in the txg that sets the property to
 * lagging, the last txg that both children get, or at an import that turns
 * the async mirror on.
 * A writable import with a child of the mirror missing starts a new lineage
 * and records the fork: the parent lineage, the last txg that it shares
 * with the parent, and the guid of the missing child.  More imports with
 * the same child missing add no fork.  When both children are present and
 * need no resilver, the fork is resolved.  When the async mirror is off and
 * its records are gone, the lineage is cleared.
 *
 * The labels of each child of the mirror hold the lineage of the pool that
 * wrote them (ZPOOL_CONFIG_ASYNC_MIRROR_LINEAGE), and the leaf ZAP of the
 * child holds the same value.  The pool changes both in one txg; see
 * async_mirror_label_sync().  The labels of the lagging child are not
 * written while the async mirror is on, so its value can be an older
 * lineage: the leaf ZAP says which one.  A child imported alone, read-write,
 * writes its new lineage to its labels and its own MOS only.  A child whose
 * labels hold a different lineage than its leaf ZAP in the selected MOS was
 * written by an import whose history is not in the selected MOS.
 *
 * Every load compares each child C of the mirror with the selected MOS
 * (async_mirror_child_compatible()).  C diverges from the selected history if
 *
 *   - its labels hold a lineage other than its leaf ZAP, or
 *   - the open fork names C as the missing child, C has a txg above the
 *     fork txg, and C's labels are not of the current lineage (which this
 *     history wrote after the fork): C holds a txg that this history does
 *     not.  A fork that a rewind made accepts such txgs up to the newest
 *     txg that the rewind discarded, by the operator's choice; the import
 *     erases them, or resilvers C in full.
 *
 * A child with no uberblock is accepted.  A child with no lineage in its
 * labels passes the first test: its labels were last written before the
 * lineage existed, or by software without this feature.  A pair of copies
 * with divergent histories is refused for a writable import.  A
 * read-only import is allowed, so that the operator can inspect either
 * copy with the other one hidden, and then keep one of them.
 *
 * Software without this feature writes labels with no lineage, so once it
 * has written the pool, no check can find two copies.  So the lineage
 * entry holds a reference on the feature while the copies have not met
 * again: while a fork is open, or a child of the mirror is missing or
 * needs a resilver.  Such software cannot import the pool then.
 */

/*
 * A new lineage: random and not zero.  spa_generate_guid() needs the
 * namespace lock, which the load does not hold here.
 */
static uint64_t
async_mirror_new_lineage(void)
{
	uint64_t v;

	do {
		(void) random_get_pseudo_bytes((uint8_t *)&v, sizeof (v));
	} while (v == 0);
	return (v);
}

/* Give the pool a new lineage, as the async mirror turns on. */
void
async_mirror_lineage_start(spa_t *spa)
{
	async_mirror_lineage_phys_t *lin = &spa->spa_async_mirror_lin;

	memset(lin, 0, sizeof (*lin));
	lin->aml_version = ASYNC_MIRROR_LINEAGE_VERSION;
	lin->aml_lineage = async_mirror_new_lineage();
}

uint64_t
spa_async_mirror_lineage(spa_t *spa)
{
	return (spa->spa_async_mirror_lin.aml_lineage);
}

/* True if tvd is a top-level mirror that can hold an asynchronous child. */
static boolean_t
async_mirror_lineage_tvd(const vdev_t *tvd)
{
	return (tvd->vdev_ops == &vdev_mirror_ops &&
	    tvd->vdev_children == 2 && !tvd->vdev_islog);
}

/* The lineage in the newest labels of leaf vd at or below txg, or 0. */
static uint64_t
async_mirror_label_lineage(spa_t *spa, vdev_t *vd, uint64_t txg)
{
	nvlist_t *label;
	uint64_t lineage = 0;

	spa_config_enter(spa, SCL_STATE_ALL, FTAG, RW_WRITER);
	label = vdev_label_read_config(vd, txg, VDEV_TRUSTED_LABELS(vd));
	spa_config_exit(spa, SCL_STATE_ALL, FTAG);
	if (label != NULL) {
		(void) nvlist_lookup_uint64(label,
		    ZPOOL_CONFIG_ASYNC_MIRROR_LINEAGE, &lineage);
		nvlist_free(label);
	}
	return (lineage);
}

/*
 * True if child cvd, whose newest uberblock is of txg newest, can meet the
 * history of the selected MOS; see "The lineage".
 */
static boolean_t
async_mirror_child_compatible(spa_t *spa, vdev_t *cvd, uint64_t newest)
{
	const async_mirror_lineage_phys_t *lin = &spa->spa_async_mirror_lin;
	uint64_t label = 0;

	if (newest == 0)
		return (B_TRUE);
	if (cvd->vdev_ops->vdev_op_leaf)
		label = async_mirror_label_lineage(spa, cvd, newest);
	if (label != 0 && label != cvd->vdev_async_label_lineage)
		return (B_FALSE);
	/* Labels of this lineage: this history wrote the child since. */
	if (lin->aml_parent == 0 || lin->aml_absent != cvd->vdev_guid ||
	    newest <= lin->aml_fork_txg || label == lin->aml_lineage)
		return (B_TRUE);
	return ((lin->aml_flags & ASYNC_MIRROR_FORK_REWIND) != 0 &&
	    newest <= lin->aml_seen);
}

/*
 * True if each child of each mirror that can hold an asynchronous child is
 * present and needs no resilver.  *missingp, if not NULL, gets the first
 * child that cannot be read or written, or NULL.
 */
static boolean_t
async_mirror_lineage_clean(spa_t *spa, vdev_t **missingp)
{
	vdev_t *rvd = spa->spa_root_vdev;
	vdev_t *missing = NULL;
	boolean_t clean = B_TRUE;

	for (uint64_t c = 0; c < rvd->vdev_children; c++) {
		vdev_t *tvd = rvd->vdev_child[c];

		if (!async_mirror_lineage_tvd(tvd))
			continue;
		for (uint64_t i = 0; i < 2; i++) {
			vdev_t *cvd = tvd->vdev_child[i];

			if (!vdev_readable(cvd) || !vdev_writeable(cvd)) {
				if (missing == NULL)
					missing = cvd;
				clean = B_FALSE;
			} else if (async_mirror_child_needs_resilver(spa,
			    cvd)) {
				clean = B_FALSE;
			}
		}
	}
	if (missingp != NULL)
		*missingp = missing;
	return (clean);
}

/*
 * Read the lineage entry, and the lineage that the leaf ZAP of each child
 * of the mirror holds for its labels, at load.
 */
static int
async_mirror_lineage_load(spa_t *spa)
{
	objset_t *mos = spa_meta_objset(spa);
	async_mirror_lineage_phys_t *lin = &spa->spa_async_mirror_lin;
	vdev_t *rvd = spa->spa_root_vdev;
	uint64_t ilen, num;
	int error;

	memset(lin, 0, sizeof (*lin));
	error = zap_length(mos, DMU_POOL_DIRECTORY_OBJECT,
	    DMU_POOL_ASYNC_MIRROR_LINEAGE, &ilen, &num);
	if (error == 0 && (ilen != sizeof (uint64_t) ||
	    num != ASYNC_MIRROR_LINEAGE_WORDS))
		error = SET_ERROR(EINVAL);
	if (error == 0) {
		error = zap_lookup(mos, DMU_POOL_DIRECTORY_OBJECT,
		    DMU_POOL_ASYNC_MIRROR_LINEAGE, sizeof (uint64_t),
		    ASYNC_MIRROR_LINEAGE_WORDS, lin);
	}
	if (error == 0 && (lin->aml_version != ASYNC_MIRROR_LINEAGE_VERSION ||
	    lin->aml_lineage == 0))
		error = SET_ERROR(EINVAL);
	if (error == ENOENT) {
		memset(lin, 0, sizeof (*lin));
		error = 0;
	}
	if (error != 0) {
		spa_load_note(spa, "async mirror: cannot read the lineage "
		    "entry: error %d", error);
		return (error);
	}
	spa->spa_async_mirror_lin_disk = *lin;
	lin->aml_flags &= ~ASYNC_MIRROR_LINEAGE_ACTIVE;

	/* Without a lineage nothing reads them, and the pool pays nothing. */
	spa->spa_async_mirror_label_keys = B_FALSE;
	if (lin->aml_lineage == 0)
		return (0);
	for (uint64_t c = 0; c < rvd->vdev_children; c++) {
		vdev_t *tvd = rvd->vdev_child[c];

		if (!async_mirror_lineage_tvd(tvd))
			continue;
		for (uint64_t i = 0; i < 2; i++) {
			vdev_t *cvd = tvd->vdev_child[i];
			uint64_t v = 0;

			if (cvd->vdev_leaf_zap != 0) {
				error = zap_lookup(mos, cvd->vdev_leaf_zap,
				    VDEV_LEAF_ZAP_ASYNC_MIRROR_LINEAGE,
				    sizeof (uint64_t), 1, &v);
			}
			if (error == ENOENT) {
				v = 0;
				error = 0;
			}
			if (error != 0) {
				spa_load_note(spa, "async mirror: cannot read "
				    "the lineage of child guid %llu: error %d",
				    (u_longlong_t)cvd->vdev_guid, error);
				return (error);
			}
			cvd->vdev_async_label_lineage = v;
			if (v != 0)
				spa->spa_async_mirror_label_keys = B_TRUE;
		}
	}
	return (0);
}

/*
 * Make the lineage in the labels of each child of the mirror follow the
 * lineage of the pool.  A child whose labels the pool writes gets the
 * lineage in its leaf ZAP, and in its labels in this txg: the config is
 * dirty, and vdev_label_sync() writes vdev_async_label_lineage.  The
 * labels of a child that is missing or lagging keep their value, and so
 * does its leaf ZAP.
 */
static void
async_mirror_label_sync(spa_t *spa, dmu_tx_t *tx)
{
	objset_t *mos = spa_meta_objset(spa);
	vdev_t *rvd = spa->spa_root_vdev;
	uint64_t lineage = spa->spa_async_mirror_lin.aml_lineage;

	for (uint64_t c = 0; c < rvd->vdev_children; c++) {
		vdev_t *tvd = rvd->vdev_child[c];

		if (!async_mirror_lineage_tvd(tvd))
			continue;
		for (uint64_t i = 0; i < 2; i++) {
			vdev_t *cvd = tvd->vdev_child[i];

			if (!cvd->vdev_ops->vdev_op_leaf ||
			    cvd->vdev_leaf_zap == 0 ||
			    cvd->vdev_async_label_lineage == lineage ||
			    !vdev_writeable(cvd) ||
			    vdev_async_mirror_skip_labels(cvd))
				continue;
			if (lineage != 0) {
				VERIFY0(zap_update(mos, cvd->vdev_leaf_zap,
				    VDEV_LEAF_ZAP_ASYNC_MIRROR_LINEAGE,
				    sizeof (uint64_t), 1, &lineage, tx));
				spa->spa_async_mirror_label_keys = B_TRUE;
			} else {
				VERIFY0(zap_remove(mos, cvd->vdev_leaf_zap,
				    VDEV_LEAF_ZAP_ASYNC_MIRROR_LINEAGE, tx));
			}
			cvd->vdev_async_label_lineage = lineage;
			vdev_config_dirty(tvd);
		}
	}
}

/*
 * Write the lineage to the MOS and to the labels; see "The lineage".  The
 * entry holds a reference on the feature while the copies have not met
 * again.  A fork is resolved only at an import, so while one is open the
 * reference stays until the next import.  The sync thread calls this in
 * pass 1 of each txg.
 */
void
vdev_async_mirror_sync_lineage(spa_t *spa, dmu_tx_t *tx)
{
	async_mirror_lineage_phys_t want = spa->spa_async_mirror_lin;
	async_mirror_lineage_phys_t *disk = &spa->spa_async_mirror_lin_disk;
	objset_t *mos = spa_meta_objset(spa);

	ASSERT(dsl_pool_sync_context(spa_get_dsl(spa)));

	if (want.aml_lineage == 0 && disk->aml_version == 0 &&
	    !spa->spa_async_mirror_label_keys)
		return;

	async_mirror_label_sync(spa, tx);

	if (want.aml_lineage == 0)
		memset(&want, 0, sizeof (want));
	else if (want.aml_parent != 0 || !async_mirror_lineage_clean(spa, NULL))
		want.aml_flags |= ASYNC_MIRROR_LINEAGE_ACTIVE;
	if (memcmp(&want, disk, sizeof (want)) == 0)
		return;

	boolean_t was = (disk->aml_flags & ASYNC_MIRROR_LINEAGE_ACTIVE) != 0;
	boolean_t is = (want.aml_flags & ASYNC_MIRROR_LINEAGE_ACTIVE) != 0;

	if (want.aml_lineage != 0) {
		ASSERT(spa_feature_is_enabled(spa, SPA_FEATURE_ASYNC_MIRROR));
		VERIFY0(zap_update(mos, DMU_POOL_DIRECTORY_OBJECT,
		    DMU_POOL_ASYNC_MIRROR_LINEAGE, sizeof (uint64_t),
		    ASYNC_MIRROR_LINEAGE_WORDS, &want, tx));
	} else {
		VERIFY0(zap_remove(mos, DMU_POOL_DIRECTORY_OBJECT,
		    DMU_POOL_ASYNC_MIRROR_LINEAGE, tx));
	}
	if (is && !was) {
		spa_feature_incr(spa, SPA_FEATURE_ASYNC_MIRROR, tx);
		spa_history_log_internal(spa, "async mirror", tx,
		    "the copies of lineage %llx have not met; the feature "
		    "stays active", (u_longlong_t)want.aml_lineage);
	} else if (was && !is) {
		spa_feature_decr(spa, SPA_FEATURE_ASYNC_MIRROR, tx);
		spa_history_log_internal(spa, "async mirror", tx,
		    "the copies of lineage %llx have met",
		    (u_longlong_t)disk->aml_lineage);
	}
	*disk = want;
}

/*
 * The copies have divergent histories, and the operator chooses one to keep
 * at import: ami_keep is the guid of that child.  Return ERESTART with
 * spa_async_mirror_skip set to the other child.  The load then starts again
 * and selects the kept copy's uberblock, because the other child is no source
 * of uberblocks, nor of reads or repairs during the load
 * (see vdev_async_mirror_load_marked()).
 * Its uberblocks are erased only at activation, once the kept copy has
 * loaded and before the pool writes anything; see
 * async_mirror_erase_skipped().  So a kept copy that fails to load, for
 * example with a log device missing, leaves the other copy unchanged.  The
 * child then has no uberblock, and gets a full resilver (see
 * async_mirror_mark_erased()).  The writes that only the other copy holds
 * are lost, by that choice.
 *
 * Return another error if the guid names no child of tvd, or the other
 * child cannot be written; the load then refuses the import.
 */
static int
async_mirror_keep_copy(spa_t *spa, vdev_t *tvd, const uberblock_t *newest)
{
	uint64_t keep = spa->spa_async_mirror_import.ami_keep;
	int k;

	if (tvd->vdev_child[0]->vdev_guid == keep)
		k = 0;
	else if (tvd->vdev_child[1]->vdev_guid == keep)
		k = 1;
	else {
		spa_load_note(spa, "async mirror: the copy to keep, guid "
		    "%llu, is not a child of this mirror", (u_longlong_t)keep);
		return (SET_ERROR(ENOENT));
	}

	vdev_t *other = tvd->vdev_child[1 - k];

	if (newest[k].ub_txg == 0) {
		spa_load_note(spa, "async mirror: the copy to keep, guid "
		    "%llu, has no uberblock", (u_longlong_t)keep);
		return (SET_ERROR(ENOENT));
	}

	if (!other->vdev_ops->vdev_op_leaf || !vdev_writeable(other)) {
		spa_load_note(spa, "async mirror: cannot erase the uberblocks "
		    "of child guid %llu", (u_longlong_t)other->vdev_guid);
		return (SET_ERROR(ENXIO));
	}
	spa_load_note(spa, "async mirror: the operator keeps the copy of "
	    "child guid %llu at txg %llu; every uberblock of child guid %llu "
	    "is erased once that copy has loaded", (u_longlong_t)keep,
	    (u_longlong_t)newest[k].ub_txg, (u_longlong_t)other->vdev_guid);
	spa->spa_async_mirror_skip = other->vdev_guid;
	return (SET_ERROR(ERESTART));
}

/*
 * Compare each child of each mirror that can hold an asynchronous child
 * with the selected MOS.  The load calls this once the vdev tree is
 * trusted, before anything is written.  On a divergence, put both
 * children's guids and newest txgs in the load info.  Refuse a load that
 * may write, so that no copy wins silently.  Allow a read-only load, so
 * that the operator can inspect a copy.  A load that may write and names
 * the copy to keep returns ERESTART; see async_mirror_keep_copy().
 *
 * Use whether the load can write, not spa_writeable(): trust in the config
 * changes during the load.
 */
int
vdev_async_mirror_check_lineage(spa_t *spa)
{
	vdev_t *rvd = spa->spa_root_vdev;
	int error = async_mirror_lineage_load(spa);

	if (error != 0)
		return (error);
	if (spa_async_mirror_lineage(spa) == 0)
		return (0);

	for (uint64_t c = 0; c < rvd->vdev_children; c++) {
		vdev_t *tvd = rvd->vdev_child[c];
		uberblock_t best, newest[2];
		boolean_t diverged = B_FALSE;

		if (!async_mirror_lineage_tvd(tvd))
			continue;
		for (uint64_t i = 0; i < 2; i++) {
			vdev_t *cvd = tvd->vdev_child[i];

			memset(&newest[i], 0, sizeof (newest[i]));
			if (!vdev_readable(cvd))
				continue;
			async_mirror_read_leaf_ub(spa, cvd, &best,
			    &newest[i]);
			if (!async_mirror_child_compatible(spa, cvd,
			    newest[i].ub_txg))
				diverged = B_TRUE;
		}
		if (!diverged)
			continue;

		uint64_t info[4] = {
			tvd->vdev_child[0]->vdev_guid, newest[0].ub_txg,
			tvd->vdev_child[1]->vdev_guid, newest[1].ub_txg
		};

		spa_load_note(spa, "async mirror: the copies were written "
		    "separately: child guid %llu at txg %llu, child guid %llu "
		    "at txg %llu", (u_longlong_t)info[0], (u_longlong_t)info[1],
		    (u_longlong_t)info[2], (u_longlong_t)info[3]);
		if ((spa->spa_mode & SPA_MODE_WRITE) &&
		    spa->spa_async_mirror_import.ami_keep != 0) {
			error = async_mirror_keep_copy(spa, tvd, newest);
			if (error == ERESTART)
				return (error);
		}
		fnvlist_add_uint64_array(spa->spa_load_info,
		    ZPOOL_CONFIG_ASYNC_MIRROR_DIVERGED, info, 4);
		if (spa->spa_mode & SPA_MODE_WRITE)
			return (SET_ERROR(ZFS_ERR_ASYNC_MIRROR_DIVERGED));
	}
	return (0);
}

/*
 * Update the lineage at a writable import, before the sync thread starts;
 * the first txg writes it.  keep is true while the async mirror is on or has
 * records.
 */
void
async_mirror_lineage_update(spa_t *spa, boolean_t keep)
{
	async_mirror_lineage_phys_t *lin = &spa->spa_async_mirror_lin;
	vdev_t *missing = NULL;

	ASSERT(spa_writeable(spa));
	ASSERT(!spa->spa_sync_on);

	if (lin->aml_lineage == 0)
		return;

	boolean_t clean = async_mirror_lineage_clean(spa, &missing);

	if (missing != NULL) {
		if (lin->aml_absent == missing->vdev_guid)
			return;
		uint64_t parent = lin->aml_lineage;

		lin->aml_lineage = async_mirror_new_lineage();
		lin->aml_parent = parent;
		lin->aml_fork_txg = spa->spa_uberblock.ub_txg;
		lin->aml_absent = missing->vdev_guid;
		lin->aml_seen = 0;
		lin->aml_flags = 0;
		/*
		 * A rewind import has a limit on the txg to select.  It
		 * discards the txgs above the fork that the children it has
		 * hold, up to the newest one, which it can read.
		 */
		if (spa->spa_load_max_txg != UINT64_MAX) {
			vdev_t *tvd = missing->vdev_parent;

			lin->aml_flags = ASYNC_MIRROR_FORK_REWIND;
			for (uint64_t c = 0; c < tvd->vdev_children; c++) {
				vdev_t *cvd = tvd->vdev_child[c];
				uberblock_t cub, newest;

				if (cvd == missing || !vdev_readable(cvd))
					continue;
				async_mirror_read_leaf_ub(spa, cvd, &cub,
				    &newest);
				lin->aml_seen = MAX(lin->aml_seen,
				    newest.ub_txg);
			}
			spa_history_log_internal(spa, "async mirror", NULL,
			    "child guid %llu is missing; new lineage %llx "
			    "forks from %llx at txg %llu by a rewind, which "
			    "discards the txgs up to %llu",
			    (u_longlong_t)missing->vdev_guid,
			    (u_longlong_t)lin->aml_lineage,
			    (u_longlong_t)parent,
			    (u_longlong_t)lin->aml_fork_txg,
			    (u_longlong_t)lin->aml_seen);
		} else {
			spa_history_log_internal(spa, "async mirror", NULL,
			    "child guid %llu is missing; new lineage %llx "
			    "forks from %llx at txg %llu",
			    (u_longlong_t)missing->vdev_guid,
			    (u_longlong_t)lin->aml_lineage,
			    (u_longlong_t)parent,
			    (u_longlong_t)lin->aml_fork_txg);
		}
	} else if (clean) {
		if (lin->aml_parent != 0) {
			spa_history_log_internal(spa, "async mirror", NULL,
			    "both children present; the fork of lineage %llx "
			    "from %llx is resolved",
			    (u_longlong_t)lin->aml_lineage,
			    (u_longlong_t)lin->aml_parent);
		}
		lin->aml_parent = 0;
		lin->aml_fork_txg = 0;
		lin->aml_absent = 0;
		lin->aml_seen = 0;
		lin->aml_flags = 0;
		if (!keep)
			memset(lin, 0, sizeof (*lin));
	}
}

/*
 * Erase the uberblocks of the copy that the operator discards; see
 * async_mirror_keep_copy().  The load has verified the kept copy, and the
 * sync thread has not started, so nothing is written yet.
 *
 * A crash during the erase must not leave that copy looking like an older
 * point of the kept history.  Its labels hold the lineage that made the
 * load refuse it, unless the open fork of the kept copy names it as the
 * missing child.  Then its uberblocks at or below the fork txg are such
 * points, so vdev_uberblock_erase() erases them first, and a crash in
 * between leaves uberblocks above the fork txg, which the next import
 * refuses again.  A child with no uberblock left is resilvered in full.
 * An error fails the import; the operator repeats it.
 */
int
async_mirror_erase_skipped(spa_t *spa)
{
	const async_mirror_lineage_phys_t *lin = &spa->spa_async_mirror_lin;
	uint64_t guid = spa->spa_async_mirror_skip;

	ASSERT(!spa->spa_sync_on);

	if (guid == 0)
		return (0);

	vdev_t *vd = vdev_lookup_by_guid(spa->spa_root_vdev, guid);
	uint64_t max_txg = (lin->aml_parent != 0 && lin->aml_absent == guid) ?
	    lin->aml_fork_txg : 0;
	int error;

	/* The erase reads the ring back, so the ring must be visible. */
	spa->spa_async_mirror_skip = 0;
	if (vd == NULL || !vd->vdev_ops->vdev_op_leaf || !vdev_writeable(vd))
		error = SET_ERROR(ENXIO);
	else
		error = vdev_uberblock_erase(vd, max_txg);
	if (error != 0) {
		spa_load_note(spa, "async mirror: cannot erase the uberblocks "
		    "of child guid %llu, error %d", (u_longlong_t)guid, error);
		spa->spa_async_mirror_skip = guid;
		return (error);
	}
	spa->spa_async_mirror_import.ami_erased = guid;
	return (0);
}

/*
 * A child of the mirror that holds no valid uberblock, while the other
 * child can be read, was erased: by this import, which keeps the other
 * copy at the operator's choice, or by an earlier import that crashed
 * before its first txg.  Its blocks belong to no txg of this pool, so it
 * gets a full resilver.  This does not depend on the records or on the
 * operator's choice, so a crash between the erase and the txg that writes
 * the DTLs cannot leave the child looking like a valid copy.  Only a pool
 * with a lineage is checked.  Runs at a writable import, before the sync
 * thread starts.
 */
void
async_mirror_mark_erased(spa_t *spa)
{
	vdev_t *rvd = spa->spa_root_vdev;
	const async_mirror_import_t *ami = &spa->spa_async_mirror_import;
	uint64_t import_txg = spa->spa_uberblock.ub_txg;

	if (spa_async_mirror_lineage(spa) == 0)
		return;

	for (uint64_t c = 0; c < rvd->vdev_children; c++) {
		vdev_t *tvd = rvd->vdev_child[c];

		if (!async_mirror_lineage_tvd(tvd))
			continue;
		for (uint64_t i = 0; i < 2; i++) {
			vdev_t *cvd = tvd->vdev_child[i];
			uberblock_t ub, newest;

			if (!async_mirror_child_usable(cvd) ||
			    !vdev_readable(tvd->vdev_child[1 - i]))
				continue;
			async_mirror_read_leaf_ub(spa, cvd, &ub, &newest);
			if (newest.ub_txg != 0)
				continue;
			if (ami->ami_erased == cvd->vdev_guid) {
				spa_history_log_internal(spa, "async mirror",
				    NULL, "the operator kept the copy of child "
				    "guid %llu at txg %llu; the copy of child "
				    "guid %llu is discarded and resilvered in "
				    "full",
				    (u_longlong_t)ami->ami_keep,
				    (u_longlong_t)import_txg,
				    (u_longlong_t)cvd->vdev_guid);
			}
			spa_load_note(spa, "async mirror: child guid %llu has "
			    "no uberblock; it is resilvered in full",
			    (u_longlong_t)cvd->vdev_guid);
			async_mirror_mark_dtl(spa, cvd, TXG_INITIAL,
			    import_txg - TXG_INITIAL + 1);
			async_mirror_note_resync(spa, cvd);
		}
	}
}
