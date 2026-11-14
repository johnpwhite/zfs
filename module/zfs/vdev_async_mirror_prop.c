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
 * async mirror: the live property.
 *
 * This file holds the vdev property async_mirror: the check and the sync
 * task that store it, the state that "zpool get" reports, and the switch
 * on and the two-step switch off at a txg boundary.  The sync thread calls
 * vdev_async_mirror_sync_start() at the start of each txg.  See "The
 * property" in vdev_async_mirror.c.
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
#include <sys/dsl_synctask.h>
#include <sys/dsl_scan.h>
#include <sys/dsl_dataset.h>
#include <sys/dmu_traverse.h>
#include <sys/dmu_objset.h>
#include <sys/dnode.h>
#include <sys/arc.h>
#include <sys/zio_checksum.h>
#include <sys/zil.h>

/*
 * True if the state has no async mirror or anything from an earlier one:
 * no lagging child, switch-on request, record, freeze, or worker.  Only
 * then can the property turn the async mirror on.
 */
static boolean_t
async_mirror_idle(vdev_async_mirror_t *am)
{
	ASSERT(MUTEX_HELD(&am->am_lock));

	return ((am->am_state == AMS_OFF || am->am_state == AMS_DRAINING) &&
	    list_is_empty(&am->am_txgs) && am->am_zap_obj == 0 &&
	    am->am_defer_zap == 0 && !am->am_frozen_entry &&
	    !am->am_off_entry && am->am_thread == NULL);
}

/* Clear state from an earlier async mirror before a switch on. */
static void
async_mirror_reset(vdev_async_mirror_t *am)
{
	ASSERT(MUTEX_HELD(&am->am_lock));
	ASSERT(async_mirror_idle(am));
	ASSERT0(am->am_held_bytes);
	ASSERT0(am->am_held_dspace);
	ASSERT0(am->am_held_log_bytes);
	ASSERT(!am->am_off);
	ASSERT(!am->am_defer_io);
	ASSERT3U(am->am_persist, ==, AMP_NONE);

	am->am_stop = B_FALSE;
	am->am_pass_txg = 0;
	am->am_fail_from = am->am_fail_open_txg = 0;
	am->am_halt_txg = am->am_dtl_txg = 0;
	am->am_release_more = B_FALSE;
	am->am_defer_txg = 0;
	am->am_defer_failed = am->am_defer_quick = B_FALSE;
	am->am_pending_bytes = am->am_pending_txgs = 0;
}

/*
 * Return 0 if the async_mirror property of the leaf vd can be set to value,
 * or the error that says why not.  The caller holds SCL_CONFIG, or is the
 * sync thread.
 *
 * Off is always allowed; on a pool with no state it changes nothing but the
 * property.  Lagging is allowed on the lagging child, unless a switch off
 * is under way, and on a leaf that can start the async mirror when the pool
 * has no async mirror and nothing left from an earlier one.
 */
static int
async_mirror_set_allowed(spa_t *spa, vdev_t *vd, uint64_t value)
{
	vdev_async_mirror_t *am = spa->spa_async_mirror;
	vdev_t *lead = NULL;

	if (!vd->vdev_ops->vdev_op_leaf)
		return (SET_ERROR(ENOTSUP));
	if (value == VDEV_ASYNC_MIRROR_OFF)
		return (0);
	if (am == NULL)
		return (async_mirror_can_start(spa, vd, &lead));

	mutex_enter(&am->am_lock);
	boolean_t same = (am->am_lagging == vd ||
	    (am->am_state == AMS_STARTING && am->am_on_guid == vd->vdev_guid));
	boolean_t busy = same ? am->am_off : !async_mirror_idle(am);
	mutex_exit(&am->am_lock);

	if (busy)
		return (SET_ERROR(EBUSY));
	if (same)
		return (0);
	return (async_mirror_can_start(spa, vd, &lead));
}

/* Check a change of the property in open context, before the sync task. */
int
vdev_async_mirror_set_check(spa_t *spa, vdev_t *vd, uint64_t value)
{
	ASSERT(spa_config_held(spa, SCL_CONFIG, RW_READER));

	if (!spa_writeable(spa))
		return (SET_ERROR(EROFS));
	return (async_mirror_set_allowed(spa, vd, value));
}

typedef struct async_mirror_set_arg {
	uint64_t	asa_guid;
	uint64_t	asa_value;
	uint64_t	asa_txg;
} async_mirror_set_arg_t;

/*
 * The check of the sync task.  The open context check came before it, in
 * vdev_async_mirror_set_check(), with the config lock that a lookup needs.
 */
static int
async_mirror_set_check_sync(void *arg, dmu_tx_t *tx)
{
	async_mirror_set_arg_t *asa = arg;
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;

	if (!dmu_tx_is_syncing(tx))
		return (0);

	vdev_t *vd = spa_lookup_by_guid(spa, asa->asa_guid, B_FALSE);

	if (vd == NULL)
		return (SET_ERROR(ENODEV));
	if (vd->vdev_leaf_zap == 0)
		return (SET_ERROR(ENOTSUP));
	/* vdev_async_mirror_set() created the state for lagging. */
	if (asa->asa_value == VDEV_ASYNC_MIRROR_LAGGING &&
	    spa->spa_async_mirror == NULL)
		return (SET_ERROR(ENOTSUP));
	return (async_mirror_set_allowed(spa, vd, asa->asa_value));
}

/* Store the property of one leaf in its ZAP. */
static void
async_mirror_prop_store(spa_t *spa, vdev_t *vd, uint64_t value, dmu_tx_t *tx)
{
	VERIFY0(zap_update(spa_meta_objset(spa), vd->vdev_leaf_zap,
	    vdev_prop_to_name(VDEV_PROP_ASYNC_MIRROR), sizeof (uint64_t), 1,
	    &value, tx));
	vd->vdev_async_mirror = value;
	spa_history_log_internal(spa, "vdev set", tx,
	    "vdev_guid=%llu: async_mirror=%s", (u_longlong_t)vd->vdev_guid,
	    value == VDEV_ASYNC_MIRROR_LAGGING ? "lagging" : "off");
}

/*
 * The sync task, in txg P.  Store the property.  For lagging, the switch on
 * comes at the start of txg P + 1; for off, the switch off starts at the
 * start of txg P + 1.  See "The property" at the top of this file.
 */
static void
async_mirror_set_sync(void *arg, dmu_tx_t *tx)
{
	async_mirror_set_arg_t *asa = arg;
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	vdev_async_mirror_t *am = spa->spa_async_mirror;
	vdev_t *rvd = spa->spa_root_vdev;
	vdev_t *vd = spa_lookup_by_guid(spa, asa->asa_guid, B_FALSE);
	uint64_t txg = dmu_tx_get_txg(tx);

	VERIFY3P(vd, !=, NULL);
	asa->asa_txg = txg;

	/* One leaf at most names itself as the lagging child. */
	for (uint64_t c = 0; asa->asa_value == VDEV_ASYNC_MIRROR_LAGGING &&
	    c < rvd->vdev_children; c++) {
		vdev_t *tvd = rvd->vdev_child[c];

		if (tvd->vdev_ops != &vdev_mirror_ops)
			continue;
		for (uint64_t i = 0; i < tvd->vdev_children; i++) {
			vdev_t *leaf = tvd->vdev_child[i];

			if (leaf != vd && leaf->vdev_leaf_zap != 0 &&
			    leaf->vdev_async_mirror ==
			    VDEV_ASYNC_MIRROR_LAGGING) {
				async_mirror_prop_store(spa, leaf,
				    VDEV_ASYNC_MIRROR_OFF, tx);
			}
		}
	}
	async_mirror_prop_store(spa, vd, asa->asa_value, tx);

	/* Off on a pool with no state changes nothing else. */
	if (am == NULL) {
		ASSERT3U(asa->asa_value, ==, VDEV_ASYNC_MIRROR_OFF);
		return;
	}

	boolean_t start = B_FALSE;

	mutex_enter(&am->am_lock);
	if (asa->asa_value == VDEV_ASYNC_MIRROR_LAGGING) {
		if (am->am_state == AMS_OFF || am->am_state == AMS_DRAINING) {
			am->am_on_guid = vd->vdev_guid;
			am->am_on_txg = txg + 1;
			async_mirror_set_state(am, AMS_STARTING);
			start = B_TRUE;
		}
	} else if (am->am_state == AMS_STARTING &&
	    am->am_on_guid == vd->vdev_guid) {
		am->am_on_txg = 0;
		async_mirror_set_state(am, AMS_OFF);
	} else if (am->am_lagging == vd) {
		am->am_off = B_TRUE;
	}
	mutex_exit(&am->am_lock);

	/*
	 * The lagging child keeps this txg, the last one that both children
	 * get.  Start the lineage here, so that this txg has it, in the MOS
	 * and in the labels of both children.  A child imported alone at this
	 * txg then starts a fork, and the copies are found to be written
	 * separately when they meet; see "The lineage".
	 */
	if (start && spa_async_mirror_lineage(spa) == 0)
		async_mirror_lineage_start(spa);
}

/*
 * Create the state and the release thread of a pool that has none, when the
 * property asks for the async mirror.  Like vdev_initialize() and
 * vdev_trim(), this runs in open context, from the ioctl.  spa_async_lock
 * orders it with spa_async_suspend() and
 * spa_async_resume(): a suspend that came first makes it fail with EBUSY,
 * and a suspend that comes later finds the state and cancels the thread.
 * The pointer is published once, when the state is complete, so a reader in
 * the I/O path sees NULL or the whole state; an idle state changes no I/O.
 * The state and the thread stay until the export frees them.
 */
static int
async_mirror_state_get(spa_t *spa)
{
	int error = 0;

	mutex_enter(&spa->spa_async_lock);
	if (spa->spa_async_mirror == NULL) {
		if (spa->spa_async_suspended != 0) {
			error = SET_ERROR(EBUSY);
		} else {
			vdev_async_mirror_t *am = async_mirror_create(spa);

			spa_async_mirror_stats_init(spa);
			am->am_release_zthr = zthr_create(
			    "z_async_mirror_release",
			    async_mirror_release_thread_check,
			    async_mirror_release_thread, am, minclsyspri);
			/*
			 * Each reader loads the fields through the pointer,
			 * so its loads depend on it and come after it.
			 */
			membar_producer();
			spa->spa_async_mirror = am;
		}
	}
	mutex_exit(&spa->spa_async_lock);
	return (error);
}

/*
 * Set the async_mirror property of a leaf: the ioctl of "zpool set".  The
 * caller has checked the change with vdev_async_mirror_set_check(), and
 * holds no config lock.  For lagging, wait for the txg of the switch on,
 * so that the async mirror is on when this returns, and fail if the switch
 * found that the leaf could not start after all.  For off, return at once.
 * The switch off waits for the replay; async_mirror_state shows its progress.
 */
int
vdev_async_mirror_set(spa_t *spa, uint64_t guid, uint64_t value)
{
	async_mirror_set_arg_t asa = {
		.asa_guid = guid,
		.asa_value = value
	};
	int error = 0;

	if (value == VDEV_ASYNC_MIRROR_LAGGING)
		error = async_mirror_state_get(spa);
	if (error == 0) {
		error = dsl_sync_task(spa_name(spa),
		    async_mirror_set_check_sync, async_mirror_set_sync, &asa, 1,
		    ZFS_SPACE_CHECK_EXTRA_RESERVED);
	}

	if (error != 0 || value != VDEV_ASYNC_MIRROR_LAGGING)
		return (error);

	txg_wait_synced(spa_get_dsl(spa), asa.asa_txg + 1);

	spa_config_enter(spa, SCL_CONFIG, FTAG, RW_READER);
	vdev_t *vd = spa_lookup_by_guid(spa, guid, B_FALSE);

	if (vd == NULL)
		error = SET_ERROR(ENODEV);
	else if (vd->vdev_async_mirror != VDEV_ASYNC_MIRROR_LAGGING)
		error = SET_ERROR(EBUSY);
	spa_config_exit(spa, SCL_CONFIG, FTAG);
	return (error);
}

/*
 * The async_mirror_state property of a leaf: what the async mirror does with it
 * now.  The async_mirror property says what the operator asked for.
 */
const char *
vdev_async_mirror_state(vdev_t *vd)
{
	vdev_async_mirror_t *am = vd->vdev_spa->spa_async_mirror;
	const char *state = "off";

	if (am == NULL)
		return (state);

	mutex_enter(&am->am_lock);
	if (am->am_lagging == vd) {
		if (am->am_state == AMS_FROZEN)
			state = "frozen";
		else if (am->am_state == AMS_FAILED)
			state = "failed";
		else if (am->am_off)
			state = "stopping";
		else if (am->am_pending_txgs > 0)
			state = "behind";
		else
			state = "current";
	} else if (am->am_lagging != NULL && am->am_leader == vd) {
		state = "leader";
	} else if (am->am_state == AMS_STARTING &&
	    am->am_on_guid == vd->vdev_guid) {
		state = "starting";
	}
	mutex_exit(&am->am_lock);
	return (state);
}

/*
 * The switch on, at the start of txg T.  See "The property" at the top of
 * this file for why this point is safe.  If the leaf cannot start now, the
 * property goes back to off, and the history says why.
 */
static void
async_mirror_switch_on(vdev_async_mirror_t *am, dmu_tx_t *tx)
{
	spa_t *spa = am->am_spa;
	uint64_t txg = dmu_tx_get_txg(tx);
	uint64_t guid = am->am_on_guid;
	vdev_t *lag = spa_lookup_by_guid(spa, guid, B_FALSE);
	vdev_t *lead = NULL;
	int error;

	mutex_enter(&am->am_lock);
	ASSERT3U(am->am_state, ==, AMS_STARTING);
	am->am_on_txg = 0;
	async_mirror_set_state(am, AMS_OFF);
	boolean_t idle = async_mirror_idle(am);
	mutex_exit(&am->am_lock);

	if (lag == NULL || !lag->vdev_ops->vdev_op_leaf)
		error = SET_ERROR(ENODEV);
	else if (!idle)
		error = SET_ERROR(EBUSY);
	else
		error = async_mirror_can_start(spa, lag, &lead);
	if (error != 0) {
		if (lag != NULL && lag->vdev_ops->vdev_op_leaf &&
		    lag->vdev_leaf_zap != 0) {
			async_mirror_prop_store(spa, lag,
			    VDEV_ASYNC_MIRROR_OFF, tx);
		}
		spa_history_log_internal(spa, "async mirror", tx,
		    "leaf guid %llu cannot become the lagging child at txg "
		    "%llu, error %d", (u_longlong_t)guid, (u_longlong_t)txg,
		    error);
		return;
	}

	/*
	 * The newest uberblock is of txg T - 1, the txg of the sync task,
	 * which wrote the MOS.  Both children have it, and every block of
	 * it.  The records start there.
	 */
	ASSERT3U(spa->spa_uberblock.ub_txg, <, txg);
	mutex_enter(&am->am_lock);
	async_mirror_reset(am);
	am->am_last_ub = spa->spa_uberblock;
	am->am_applied_txg = spa->spa_uberblock.ub_txg;
	mutex_exit(&am->am_lock);

	/* async_mirror_set_sync() started the lineage in txg T - 1. */
	ASSERT3U(spa_async_mirror_lineage(spa), !=, 0);

	/*
	 * The mark first: until the async mirror is on, it only fences reads of
	 * blocks born after T - 1, and there are none on the child yet.
	 */
	async_mirror_mark(spa, lag, am->am_applied_txg);

	mutex_enter(&am->am_lock);
	am->am_leader = lead;
	am->am_lagging = lag;
	async_mirror_set_state(am, AMS_ACTIVE);
	async_mirror_update_stats(am);
	am->am_thread = thread_create(NULL, 0, async_mirror_thread, am, 0, &p0,
	    TS_RUN, minclsyspri);
	mutex_exit(&am->am_lock);
	spa_history_log_internal(spa, "async mirror", tx,
	    "leaf guid %llu is the lagging child from txg %llu",
	    (u_longlong_t)guid, (u_longlong_t)txg);
	zfs_dbgmsg("async mirror: on, lagging child guid %llu at txg %llu",
	    (u_longlong_t)guid, (u_longlong_t)am->am_applied_txg);
}

/*
 * The switch off, at the start of a txg; called in each txg until it is
 * done.  See "The property" at the top of this file for the two steps of a
 * child that copies normally.
 *
 * A frozen child gets normal writes from this txg on, and then a resilver
 * from its frozen txg, as after a failed copy; see async_mirror_fail_sync().
 * The DTLs, the cleared mark and the end of the freeze entry are in one txg.
 * A crash before it leaves the freeze entry with the property off, so the
 * import ends the freeze with the same resilver.  The freeze entry must be
 * on disk first, and the records released, or a crash could leave the child
 * as a valid copy of a txg that it does not have in full.
 *
 * The state lets go of a child whose copy failed when its DTLs are on disk
 * and the records are gone.
 */
static void
async_mirror_switch_off(vdev_async_mirror_t *am, dmu_tx_t *tx)
{
	spa_t *spa = am->am_spa;
	uint64_t txg = dmu_tx_get_txg(tx);
	uint64_t last = spa_last_synced_txg(spa) + TXG_CONCURRENT_STATES;
	vdev_t *lag = am->am_lagging;
	uint64_t guid = lag->vdev_guid;

	mutex_enter(&am->am_lock);
	if (am->am_state == AMS_FROZEN) {
		uint64_t frozen_txg = am->am_applied_txg;

		if (!am->am_frozen_entry ||
		    !async_mirror_durable(am, am->am_halt_txg) ||
		    !list_is_empty(&am->am_txgs)) {
			mutex_exit(&am->am_lock);
			return;
		}
		am->am_fail_from = frozen_txg + 1;
		am->am_fail_open_txg = last;
		async_mirror_set_state(am, AMS_FAILED);
		async_mirror_update_stats(am);
		mutex_exit(&am->am_lock);
		spa_history_log_internal(spa, "async mirror", tx,
		    "the freeze of the lagging child guid %llu at txg %llu "
		    "ends; it gets writes again from txg %llu",
		    (u_longlong_t)guid, (u_longlong_t)frozen_txg,
		    (u_longlong_t)txg);
		return;
	}

	if (am->am_state == AMS_FAILED) {
		boolean_t done = async_mirror_durable(am, am->am_dtl_txg) &&
		    list_is_empty(&am->am_txgs) && am->am_zap_obj == 0 &&
		    am->am_defer_zap == 0 && am->am_thread == NULL;

		if (done) {
			am->am_off = B_FALSE;
			async_mirror_set_state(am, AMS_DRAINING);
			async_mirror_update_stats(am);
		}
		mutex_exit(&am->am_lock);
		if (done) {
			spa_history_log_internal(spa, "async mirror", tx,
			    "the failed lagging child guid %llu leaves the "
			    "mode", (u_longlong_t)guid);
		}
		return;
	}

	/* Step 1: writes reach the child again. */
	if (am->am_state == AMS_ACTIVE) {
		am->am_pass_txg = last;
		async_mirror_set_state(am, AMS_PASSTHROUGH);
		mutex_exit(&am->am_lock);
		spa_history_log_internal(spa, "async mirror", tx,
		    "the lagging child guid %llu gets writes again from txg "
		    "%llu", (u_longlong_t)guid, (u_longlong_t)txg);
		return;
	}

	/*
	 * Step 2: when the replay has copied every txg up to am_pass_txg.
	 * The child got the later txgs from the pool.  Every txg before this
	 * one has synced, so each record up to am_pass_txg is ready.
	 */
	async_mirror_txg_t *at = list_head(&am->am_txgs);

	while (at != NULL && at->at_txg <= am->am_applied_txg)
		at = list_next(&am->am_txgs, at);
	ASSERT3U(am->am_state, ==, AMS_PASSTHROUGH);
	if (txg <= am->am_pass_txg || am->am_defer_txg != 0 ||
	    am->am_defer_io || am->am_persist != AMP_NONE ||
	    (at != NULL && at->at_txg <= am->am_pass_txg)) {
		mutex_exit(&am->am_lock);
		return;
	}
	ASSERT0(am->am_pending_txgs);
	am->am_off = B_FALSE;
	async_mirror_set_state(am, AMS_DRAINING);
	am->am_applied_txg = UINT64_MAX;
	boolean_t records = (am->am_zap_obj != 0);
	async_mirror_update_stats(am);
	cv_broadcast(&am->am_cv);
	mutex_exit(&am->am_lock);

	async_mirror_mark(spa, NULL, 0);
	if (records) {
		VERIFY0(zap_add(spa_meta_objset(spa),
		    DMU_POOL_DIRECTORY_OBJECT, DMU_POOL_ASYNC_MIRROR_OFF,
		    sizeof (uint64_t), 1, &txg, tx));
		mutex_enter(&am->am_lock);
		am->am_off_entry = B_TRUE;
		mutex_exit(&am->am_lock);
	}
	spa_history_log_internal(spa, "async mirror", tx,
	    "the lagging child guid %llu is a normal child of its mirror "
	    "from txg %llu", (u_longlong_t)guid, (u_longlong_t)txg);
	if (am->am_release_zthr != NULL)
		zthr_wakeup(am->am_release_zthr);
}

/*
 * Called by spa_sync() at the start of each txg, before the txg issues any
 * write or free: the switch points of the property, and the DTLs of a child
 * that left the async mirror.  The sync thread holds SCL_CONFIG as a reader.
 */
void
vdev_async_mirror_sync_start(spa_t *spa, dmu_tx_t *tx)
{
	vdev_async_mirror_t *am = spa->spa_async_mirror;

	ASSERT(dsl_pool_sync_context(spa_get_dsl(spa)));
	ASSERT(spa_config_held(spa, SCL_CONFIG, RW_READER));

	if (am == NULL)
		return;
	if (am->am_on_txg != 0 && dmu_tx_get_txg(tx) >= am->am_on_txg)
		async_mirror_switch_on(am, tx);
	if (am->am_off && am->am_lagging != NULL)
		async_mirror_switch_off(am, tx);

	mutex_enter(&am->am_lock);
	boolean_t fail = (am->am_state == AMS_FAILED && am->am_dtl_txg == 0);
	mutex_exit(&am->am_lock);
	if (fail)
		async_mirror_fail_sync(am, tx);
}
