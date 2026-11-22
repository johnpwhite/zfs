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

#include <sys/spa_impl.h>
#include <sys/spa_lag.h>
#include <sys/spa_parity_impl.h>
#include <sys/vdev_impl.h>
#include <sys/random.h>

/*
 * The data device.  A hole holds no data; a log device holds log blocks only
 * (they are not in parity); special and dedup vdevs are refused for a member.
 * The caller holds a config lock, except for spa_parity_is_data_vdev().
 */
static boolean_t
parity_tvd_is_data(vdev_t *tvd)
{
	return (tvd != NULL && !tvd->vdev_ishole && !tvd->vdev_islog &&
	    tvd->vdev_ops != &vdev_indirect_ops &&
	    tvd->vdev_alloc_bias == VDEV_BIAS_NONE);
}

vdev_t *
spa_parity_data_tvd(spa_t *spa)
{
	vdev_t *rvd = spa->spa_root_vdev;

	for (uint64_t c = 0; c < rvd->vdev_children; c++) {
		if (parity_tvd_is_data(rvd->vdev_child[c]))
			return (rvd->vdev_child[c]);
	}
	return (NULL);
}

/*
 * The data leaf of a top-level vdev: the vdev itself for a plain disk, and
 * the first child of a replacing or spare vdev (the device that holds the
 * data until the replace ends).
 */
vdev_t *
spa_parity_tvd_leaf(vdev_t *tvd)
{
	vdev_t *vd = tvd;

	while (vd != NULL && !vd->vdev_ops->vdev_op_leaf) {
		if (vd->vdev_children == 0)
			return (NULL);
		vd = vd->vdev_child[0];
	}
	return (vd);
}

vdev_t *
spa_parity_data_leaf(spa_t *spa)
{
	return (spa_parity_tvd_leaf(spa_parity_data_tvd(spa)));
}

boolean_t
spa_parity_is_data_vdev(spa_t *spa, uint64_t vdev_id)
{
	vdev_t *rvd = spa->spa_root_vdev;

	if (rvd == NULL || vdev_id >= rvd->vdev_children)
		return (B_FALSE);
	return (parity_tvd_is_data(rvd->vdev_child[vdev_id]));
}

/*
 * Epoch.
 */

/* A random id that is not zero (zero means "no epoch yet"). */
uint64_t
spa_parity_new_epoch(void)
{
	uint64_t epoch;

	do {
		(void) random_get_pseudo_bytes((uint8_t *)&epoch,
		    sizeof (epoch));
	} while (epoch == 0);
	return (epoch);
}

/*
 * A new history starts (a DROP, a rewind): a new epoch id, and the generation
 * counts it.  Syncing context or load only; the words reach the disk with the
 * next write of the state.
 */
void
spa_parity_epoch_renew(spa_parity_t *ps)
{
	ps->ps_epoch = spa_parity_new_epoch();
	ps->ps_generation++;
	spa_lag_dir_set(ps->ps_lag, SLD_EPOCH, ps->ps_epoch);
	spa_lag_dir_set(ps->ps_lag, SLD_GENERATION, ps->ps_generation);
}

/*
 * Lineage.
 */

/*
 * Keep the uberblock of a txg that just synced (or the one that the import
 * loaded).  A txg that wrote no uberblock has no entry.  The ring keeps the
 * newest PARITY_UB_RING of them.
 */
void
spa_parity_lineage_note(spa_parity_t *ps, const uberblock_t *ub)
{
	parity_ub_t *pu;

	if (ub->ub_txg == 0)
		return;
	mutex_enter(&ps->ps_lock);
	/* The same txg twice (load, then the first sync): keep one. */
	for (uint_t i = 0; i < PARITY_UB_RING; i++) {
		if (ps->ps_ub[i].pu_txg == ub->ub_txg) {
			mutex_exit(&ps->ps_lock);
			return;
		}
	}
	pu = &ps->ps_ub[ps->ps_ub_next];
	ps->ps_ub_next = (ps->ps_ub_next + 1) % PARITY_UB_RING;
	pu->pu_txg = ub->ub_txg;
	pu->pu_timestamp = ub->ub_timestamp;
	for (int w = 0; w < 4; w++)
		pu->pu_cksum[w] = ub->ub_rootbp.blk_cksum.zc_word[w];
	mutex_exit(&ps->ps_lock);
}

int
spa_parity_lineage(spa_t *spa, uint64_t txg, spa_parity_lineage_t *out)
{
	spa_parity_t *ps = spa->spa_parity;
	parity_ub_t *newest = NULL;
	vdev_t *tvd, *leaf;

	memset(out, 0, sizeof (*out));
	if (ps == NULL)
		return (SET_ERROR(ENOENT));

	out->pl_start_txg = ps->ps_start_txg;
	out->pl_load_ub_txg = ps->ps_load_ub_txg;
	out->pl_load_latest_txg = ps->ps_load_latest_txg;
	out->pl_rewound = ps->ps_rewound;
	out->pl_unprotected = ps->ps_unreadable;

	mutex_enter(&ps->ps_lock);
	out->pl_epoch = ps->ps_epoch;
	out->pl_generation = ps->ps_generation;
	for (uint_t i = 0; i < PARITY_UB_RING; i++) {
		parity_ub_t *pu = &ps->ps_ub[i];

		if (pu->pu_txg == 0)
			continue;
		if (newest == NULL || pu->pu_txg > newest->pu_txg)
			newest = pu;
		if (pu->pu_txg == txg) {
			out->pl_ub_known = B_TRUE;
			out->pl_ub_txg = pu->pu_txg;
			out->pl_ub_timestamp = pu->pu_timestamp;
			memcpy(out->pl_ub_root_cksum, pu->pu_cksum,
			    sizeof (out->pl_ub_root_cksum));
		}
	}
	if (newest != NULL) {
		out->pl_last_synced_txg = newest->pu_txg;
		memcpy(out->pl_last_root_cksum, newest->pu_cksum,
		    sizeof (out->pl_last_root_cksum));
	}
	mutex_exit(&ps->ps_lock);

	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	tvd = spa_parity_data_tvd(spa);
	leaf = spa_parity_tvd_leaf(tvd);
	if (tvd != NULL)
		out->pl_data_vdev_id = tvd->vdev_id;
	if (leaf != NULL)
		out->pl_data_leaf_guid = leaf->vdev_guid;
	spa_config_exit(spa, SCL_STATE, FTAG);
	return (0);
}

#if defined(_KERNEL)
EXPORT_SYMBOL(spa_parity_data_tvd);
EXPORT_SYMBOL(spa_parity_data_leaf);
EXPORT_SYMBOL(spa_parity_tvd_leaf);
EXPORT_SYMBOL(spa_parity_is_data_vdev);
EXPORT_SYMBOL(spa_parity_new_epoch);
EXPORT_SYMBOL(spa_parity_epoch_renew);
EXPORT_SYMBOL(spa_parity_lineage_note);
EXPORT_SYMBOL(spa_parity_lineage);
#endif
