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
#include <sys/dmu.h>
#include <sys/dmu_tx.h>
#include <sys/zap.h>
#include <sys/zfeature.h>
#include <sys/uberblock_impl.h>
#include <sys/dsl_pool.h>
#include <sys/metaslab.h>
#include <sys/txg.h>
#include <sys/nvpair.h>

struct spa_lag {
	spa_t			*sl_spa;
	const spa_lag_ops_t	*sl_ops;
	void			*sl_arg;
	/* The record store. */
	kmutex_t		sl_lock;
	avl_tree_t		sl_recs;	/* spa_lag_rec_t, by txg */
	uint64_t		sl_dir[SPA_LAG_DIR_WORDS];
	boolean_t		sl_dir_exists;
	boolean_t		sl_dir_dirty;
	/* Directory entry kept without its index (spa_lag_adopt()). */
	boolean_t		sl_dir_adopted;
	/* Readers of records keep the release away (rw_tryenter(WRITER)). */
	krwlock_t		sl_recs_rwlock;
	uint_t			sl_pause;	/* under sl_lock */
	/* Writers that wait at the hard limit. */
	kmutex_t		sl_wait_lock;
	kcondvar_t		sl_wait_cv;
	/* The throttle level last posted (syncing context only). */
	spa_lag_level_t		sl_level;
	hrtime_t		sl_level_time;
	/* The last APPLIED call: the outside makes progress (pacing). */
	hrtime_t		sl_applied_time;
	/* Time at the soft or hard level, for the kstat (under sl_lock). */
	boolean_t		sl_thr_on;
	hrtime_t		sl_thr_since;
	hrtime_t		sl_thr_total;
};

#define	SPA_LAG_FOREACH(spa, lag, i)					\
	for (i = 0; i < SPA_LAG_MAX; i++)				\
		if (((lag) = (spa)->spa_lag[i]) != NULL)

/* Defaults of the limits (pool properties parity_max_* override them). */
static uint64_t zfs_parity_max_record_bytes = 4ULL << 30;
/*
 * The records live in the MOS (three copies), and a background free writes
 * them in syncing context: on a small pool a fixed byte limit alone could
 * fill the pool there.  The default is also a share of the pool.
 */
uint_t zfs_parity_max_record_pct = 5;
static uint_t zfs_parity_max_held_pct = 25;
/* At most one level event in this time. */
static uint_t zfs_parity_throttle_event_ms = 1000;
/*
 * Pacing (pool property parity_pace=on): the delay starts at this share of
 * the limits, in percent, and grows on a steeper curve, so that writers
 * settle at the apply rate well below the limit.  It applies only while the
 * outside has called APPLIED within zfs_parity_pace_idle_ms: when the
 * daemon is gone, writers are not slowed for nothing.
 */
static uint_t zfs_parity_pace_min_pct = 10;
static uint64_t zfs_parity_pace_scale = 20000000;
static uint_t zfs_parity_pace_idle_ms = 10000;

static int spa_lag_rec_compare(const void *, const void *);

spa_lag_t *
spa_lag_register(spa_t *spa, const spa_lag_ops_t *ops, void *arg)
{
	spa_lag_t *lag = kmem_zalloc(sizeof (*lag), KM_SLEEP);

	lag->sl_spa = spa;
	lag->sl_ops = ops;
	lag->sl_arg = arg;
	mutex_init(&lag->sl_lock, NULL, MUTEX_DEFAULT, NULL);
	rw_init(&lag->sl_recs_rwlock, NULL, RW_DEFAULT, NULL);
	mutex_init(&lag->sl_wait_lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&lag->sl_wait_cv, NULL, CV_DEFAULT, NULL);
	avl_create(&lag->sl_recs, spa_lag_rec_compare, sizeof (spa_lag_rec_t),
	    offsetof(spa_lag_rec_t, lr_node));

	for (int i = 0; i < SPA_LAG_MAX; i++) {
		if (spa->spa_lag[i] == NULL) {
			/* Publish after the consumer is complete. */
			membar_producer();
			spa->spa_lag[i] = lag;
			return (lag);
		}
	}
	panic("spa_lag_register(): no free slot");
}

static void
spa_lag_recs_free(spa_lag_t *lag)
{
	spa_lag_rec_t *lr;

	/* The tree stays usable (a failed load leaves it empty). */
	mutex_enter(&lag->sl_lock);
	while ((lr = avl_first(&lag->sl_recs)) != NULL) {
		avl_remove(&lag->sl_recs, lr);
		kmem_free(lr, sizeof (*lr));
	}
	mutex_exit(&lag->sl_lock);
}

void
spa_lag_unregister(spa_lag_t *lag)
{
	spa_t *spa = lag->sl_spa;

	/*
	 * Keep the slots packed: the hooks test only slot 0 for "nothing
	 * registered".  Unregister runs only in spa_unload(), when no hook
	 * can run.
	 */
	for (int i = 0; i < SPA_LAG_MAX; i++) {
		if (spa->spa_lag[i] == lag) {
			for (; i + 1 < SPA_LAG_MAX; i++)
				spa->spa_lag[i] = spa->spa_lag[i + 1];
			spa->spa_lag[SPA_LAG_MAX - 1] = NULL;
			break;
		}
	}
	spa_lag_recs_free(lag);
	avl_destroy(&lag->sl_recs);
	mutex_destroy(&lag->sl_lock);
	rw_destroy(&lag->sl_recs_rwlock);
	cv_destroy(&lag->sl_wait_cv);
	mutex_destroy(&lag->sl_wait_lock);
	kmem_free(lag, sizeof (*lag));
}

/*
 * The record store.
 *
 * The records AVL changes only in syncing context and at load, under
 * sl_lock.  The syncing thread reads it without the lock; any other reader
 * (the ioctls) takes sl_lock.
 */

_Static_assert(sizeof (uberblock_t) <= SPA_LAG_UB_WORDS * sizeof (uint64_t),
	"spa_lag_rec_phys_t: pr_prev_ub is too small for an uberblock");

/* Index ZAP block sizes (leaf and indirect), as small MOS ZAPs use. */
#define	SPA_LAG_ZAP_SHIFT	14

static int
spa_lag_rec_compare(const void *a, const void *b)
{
	const spa_lag_rec_t *ra = a, *rb = b;

	return (TREE_CMP(ra->lr_txg, rb->lr_txg));
}

static objset_t *
spa_lag_mos(spa_lag_t *lag)
{
	return (lag->sl_spa->spa_meta_objset);
}

/*
 * Read the directory entry and the index into memory.  Returns ENOENT when
 * the consumer has nothing on disk.
 */
int
spa_lag_dir_read(objset_t *mos, const char *name, uint64_t *dir)
{
	uint64_t dirent[SPA_LAG_DIRENT_WORDS];
	uint64_t key = SPA_LAG_STATE_KEY;
	int error;

	error = zap_lookup(mos, DMU_POOL_DIRECTORY_OBJECT, name,
	    sizeof (uint64_t), SPA_LAG_DIRENT_WORDS, dirent);
	if (error != 0)
		return (error);
	if (dirent[SLD_VERSION] != SPA_LAG_VERSION) {
		cmn_err(CE_WARN,
		    "spa_lag: %s state version %llu is unsupported "
		    "(expected %llu)", name,
		    (u_longlong_t)dirent[SLD_VERSION],
		    (u_longlong_t)SPA_LAG_VERSION);
		return (SET_ERROR(ENOTSUP));
	}
	if (dirent[SLD_INDEX] == 0)
		return (SET_ERROR(ENOTSUP));
	error = zap_lookup_uint64(mos, dirent[SLD_INDEX], &key, 1,
	    sizeof (uint64_t), SPA_LAG_DIR_WORDS, dir);
	if (error != 0)
		return (error);
	if (dir[SLD_VERSION] != dirent[SLD_VERSION] ||
	    dir[SLD_INDEX] != dirent[SLD_INDEX])
		return (SET_ERROR(ECKSUM));
	return (0);
}

int
spa_lag_load(spa_lag_t *lag)
{
	objset_t *mos = spa_lag_mos(lag);
	zap_cursor_t zc;
	zap_attribute_t *za;
	uint64_t dir[SPA_LAG_DIR_WORDS];
	int error;

	ASSERT0(avl_numnodes(&lag->sl_recs));

	error = spa_lag_dir_read(mos, lag->sl_ops->slo_dir_name, dir);
	if (error != 0)
		return (error);

	za = zap_attribute_alloc();
	for (zap_cursor_init(&zc, mos, dir[SLD_INDEX]);
	    (error = zap_cursor_retrieve(&zc, za)) == 0;
	    zap_cursor_advance(&zc)) {
		spa_lag_rec_t *lr;
		uint64_t txg = *(uint64_t *)za->za_name;

		if (txg == SPA_LAG_STATE_KEY)
			continue;
		if (za->za_integer_length != sizeof (uint64_t)) {
			error = SET_ERROR(ECKSUM);
			break;
		}
		if (za->za_num_integers != SPA_LAG_REC_WORDS) {
			cmn_err(CE_WARN, "spa_lag: record txg %llu has an "
			    "unsupported header", (u_longlong_t)txg);
			error = SET_ERROR(ENOTSUP);
			break;
		}
		lr = kmem_zalloc(sizeof (*lr), KM_SLEEP);
		lr->lr_txg = txg;
		error = zap_lookup_uint64(mos, dir[SLD_INDEX], &txg, 1,
		    sizeof (uint64_t), SPA_LAG_REC_WORDS, &lr->lr_phys);
		if (error == 0 && lr->lr_phys.pr_version != SPA_LAG_VERSION) {
			cmn_err(CE_WARN, "spa_lag: record txg %llu has version "
			    "%llu; expected %llu", (u_longlong_t)txg,
			    (u_longlong_t)lr->lr_phys.pr_version,
			    (u_longlong_t)SPA_LAG_VERSION);
			error = SET_ERROR(ENOTSUP);
		}
		if (error != 0) {
			kmem_free(lr, sizeof (*lr));
			break;
		}
		mutex_enter(&lag->sl_lock);
		avl_add(&lag->sl_recs, lr);
		mutex_exit(&lag->sl_lock);
	}
	zap_cursor_fini(&zc);
	zap_attribute_free(za);
	if (error == ENOENT)
		error = 0;
	if (error != 0) {
		spa_lag_recs_free(lag);
		return (error);
	}

	memcpy(lag->sl_dir, dir, sizeof (dir));
	lag->sl_dir_exists = B_TRUE;
	lag->sl_dir_dirty = B_FALSE;
	return (0);
}

/*
 * The records cannot be used (the import override), but their directory entry
 * must go one day.  Keep the entry and the index object, and nothing else: the
 * consumer then ends in a drop, and spa_lag_retire() removes them.  The index
 * itself is not read or written again, because it may be the reason that the
 * load failed.
 */
int
spa_lag_adopt(spa_lag_t *lag)
{
	uint64_t dirent[SPA_LAG_DIRENT_WORDS];
	int error;

	error = zap_lookup(spa_lag_mos(lag), DMU_POOL_DIRECTORY_OBJECT,
	    lag->sl_ops->slo_dir_name, sizeof (uint64_t), SPA_LAG_DIRENT_WORDS,
	    dirent);
	if (error != 0)
		return (error);
	if (dirent[SLD_INDEX] == 0)
		return (SET_ERROR(ENOTSUP));
	lag->sl_dir[SLD_VERSION] = dirent[SLD_VERSION];
	lag->sl_dir[SLD_INDEX] = dirent[SLD_INDEX];
	lag->sl_dir_exists = B_TRUE;
	lag->sl_dir_adopted = B_TRUE;
	lag->sl_dir_dirty = B_FALSE;
	return (0);
}

boolean_t
spa_lag_dir_exists(spa_lag_t *lag)
{
	return (lag->sl_dir_exists);
}

uint64_t
spa_lag_dir_get(spa_lag_t *lag, int word)
{
	ASSERT3S(word, <, SPA_LAG_DIR_WORDS);
	return (lag->sl_dir[word]);
}

/* Syncing context.  Written at the next spa_lag_dir_sync(). */
void
spa_lag_dir_set(spa_lag_t *lag, int word, uint64_t value)
{
	ASSERT3S(word, >, SLD_INDEX);
	ASSERT3S(word, <, SPA_LAG_DIR_WORDS);
	if (lag->sl_dir[word] != value) {
		lag->sl_dir[word] = value;
		lag->sl_dir_dirty = B_TRUE;
	}
}

/* Write the state words into the index (key SPA_LAG_STATE_KEY). */
void
spa_lag_dir_sync(spa_lag_t *lag, dmu_tx_t *tx)
{
	uint64_t key = SPA_LAG_STATE_KEY;

	ASSERT(dmu_tx_is_syncing(tx));
	ASSERT(spa_writeable(lag->sl_spa));
	/* An adopted index is never written. */
	if (!lag->sl_dir_exists || !lag->sl_dir_dirty || lag->sl_dir_adopted)
		return;
	VERIFY0(zap_update_uint64(spa_lag_mos(lag), lag->sl_dir[SLD_INDEX],
	    &key, 1, sizeof (uint64_t), SPA_LAG_DIR_WORDS, lag->sl_dir, tx));
	lag->sl_dir_dirty = B_FALSE;
}

/*
 * No record is left and the consumer is done: remove the directory entry and
 * the index, and the feature goes back to enabled.
 */
void
spa_lag_retire(spa_lag_t *lag, dmu_tx_t *tx)
{
	objset_t *mos = spa_lag_mos(lag);

	ASSERT(dmu_tx_is_syncing(tx));
	ASSERT0(avl_numnodes(&lag->sl_recs));
	ASSERT(spa_writeable(lag->sl_spa));
	if (!lag->sl_dir_exists)
		return;

	if (lag->sl_dir_adopted) {
		/*
		 * Best effort: free the record objects that the index names,
		 * then the index.  Their held frees are lost with them (the
		 * set resyncs this pool).  A read error leaks an object and
		 * must not stop the import or the sync.
		 */
		zap_cursor_t zc;
		zap_attribute_t *za = zap_attribute_alloc();

		uint64_t index = lag->sl_dir[SLD_INDEX];

		for (zap_cursor_init(&zc, mos, index);
		    zap_cursor_retrieve(&zc, za) == 0;
		    zap_cursor_advance(&zc)) {
			uint64_t txg = *(uint64_t *)za->za_name;
			spa_lag_rec_phys_t phys;

			if (txg == SPA_LAG_STATE_KEY ||
			    za->za_integer_length != sizeof (uint64_t) ||
			    za->za_num_integers != SPA_LAG_REC_WORDS)
				continue;
			if (zap_lookup_uint64(mos, index, &txg, 1,
			    sizeof (uint64_t), SPA_LAG_REC_WORDS, &phys) != 0)
				continue;
			(void) dmu_object_free(mos, phys.pr_obj, tx);
		}
		zap_cursor_fini(&zc);
		zap_attribute_free(za);
		(void) zap_destroy(mos, lag->sl_dir[SLD_INDEX], tx);
	} else {
		VERIFY0(zap_destroy(mos, lag->sl_dir[SLD_INDEX], tx));
	}
	VERIFY0(zap_remove(mos, DMU_POOL_DIRECTORY_OBJECT,
	    lag->sl_ops->slo_dir_name, tx));
	spa_feature_decr(lag->sl_spa, lag->sl_ops->slo_feature, tx);
	lag->sl_dir[SLD_INDEX] = 0;
	lag->sl_dir_exists = B_FALSE;
	lag->sl_dir_adopted = B_FALSE;
	lag->sl_dir_dirty = B_FALSE;
}

uint64_t
spa_lag_nrecs(spa_lag_t *lag)
{
	return (avl_numnodes(&lag->sl_recs));
}

spa_lag_rec_t *
spa_lag_rec_find(spa_lag_t *lag, uint64_t txg)
{
	spa_lag_rec_t search;

	search.lr_txg = txg;
	return (avl_find(&lag->sl_recs, &search, NULL));
}

spa_lag_rec_t *
spa_lag_rec_first(spa_lag_t *lag)
{
	return (avl_first(&lag->sl_recs));
}

/*
 * The released watermark + 1: the txg of the oldest record that is not
 * released, but never above released, the first txg that can still have a
 * record (the txg after the applied one; the caller passes it).  A txg
 * with no record at all (nothing allocated or freed) is not a released one:
 * with applied txg 76 and the oldest record at 78, the history from 77 on
 * is whole (the same rule as parity_history_txg() of MAP and BLOCKS).
 */
uint64_t
spa_lag_first_txg(spa_lag_t *lag, uint64_t released)
{
	spa_lag_rec_t *lr;
	uint64_t txg;

	mutex_enter(&lag->sl_lock);
	lr = avl_first(&lag->sl_recs);
	txg = (lr != NULL) ? MIN(lr->lr_txg, released) : released;
	mutex_exit(&lag->sl_lock);
	return (txg);
}

spa_lag_rec_t *
spa_lag_rec_last(spa_lag_t *lag)
{
	return (avl_last(&lag->sl_recs));
}

spa_lag_rec_t *
spa_lag_rec_next(spa_lag_t *lag, spa_lag_rec_t *lr)
{
	return (AVL_NEXT(&lag->sl_recs, lr));
}

/*
 * Create the directory entry and the index if they do not exist yet; the
 * feature becomes active.  The first record does this, and so does the
 * consumer when it must store state (a set GUID) before any record exists.
 * Syncing context.
 */
void
spa_lag_dir_create(spa_lag_t *lag, dmu_tx_t *tx)
{
	objset_t *mos = spa_lag_mos(lag);
	uint64_t key = SPA_LAG_STATE_KEY;

	ASSERT(dmu_tx_is_syncing(tx));
	ASSERT(spa_writeable(lag->sl_spa));
	if (lag->sl_dir_exists)
		return;

	lag->sl_dir[SLD_VERSION] = SPA_LAG_VERSION;
	lag->sl_dir[SLD_INDEX] = zap_create_flags(mos, 0,
	    ZAP_FLAG_HASH64 | ZAP_FLAG_UINT64_KEY,
	    DMU_OTN_ZAP_METADATA, SPA_LAG_ZAP_SHIFT, SPA_LAG_ZAP_SHIFT,
	    DMU_OT_NONE, 0, tx);
	/* The first SPA_LAG_DIRENT_WORDS state words are the entry. */
	VERIFY0(zap_add(mos, DMU_POOL_DIRECTORY_OBJECT,
	    lag->sl_ops->slo_dir_name, sizeof (uint64_t),
	    SPA_LAG_DIRENT_WORDS, lag->sl_dir, tx));
	VERIFY0(zap_add_uint64(mos, lag->sl_dir[SLD_INDEX], &key, 1,
	    sizeof (uint64_t), SPA_LAG_DIR_WORDS, lag->sl_dir, tx));
	spa_feature_incr(lag->sl_spa, lag->sl_ops->slo_feature, tx);
	lag->sl_dir_exists = B_TRUE;
	lag->sl_dir_dirty = B_FALSE;
}

/*
 * This txg's record: find it, or create it (and, on first use, the directory
 * entry and the index; the feature becomes active).  Syncing context.
 */
spa_lag_rec_t *
spa_lag_rec_get(spa_lag_t *lag, dmu_tx_t *tx)
{
	objset_t *mos = spa_lag_mos(lag);
	uint64_t txg = dmu_tx_get_txg(tx);
	spa_lag_rec_t *lr, *prev;

	ASSERT(dmu_tx_is_syncing(tx));
	ASSERT(spa_writeable(lag->sl_spa));
	lr = spa_lag_rec_find(lag, txg);
	if (lr != NULL)
		return (lr);

	spa_lag_dir_create(lag, tx);

	lr = kmem_zalloc(sizeof (*lr), KM_SLEEP);
	lr->lr_txg = txg;
	lr->lr_phys.pr_version = SPA_LAG_VERSION;
	lr->lr_phys.pr_time = gethrestime_sec();
	lr->lr_phys.pr_obj = dmu_object_alloc(mos, DMU_OTN_UINT64_METADATA,
	    SPA_LAG_REC_BLOCKSIZE, DMU_OT_NONE, 0, tx);
	prev = avl_last(&lag->sl_recs);
	ASSERT(prev == NULL || prev->lr_txg < txg);
	lr->lr_phys.pr_prev_txg = (prev != NULL) ? prev->lr_txg : 0;
	/* The uberblock of the last synced txg (LABEL needs it). */
	memcpy(lr->lr_phys.pr_prev_ub, &lag->sl_spa->spa_ubsync,
	    sizeof (uberblock_t));
	VERIFY0(zap_add_uint64(mos, lag->sl_dir[SLD_INDEX], &txg, 1,
	    sizeof (uint64_t), SPA_LAG_REC_WORDS, &lr->lr_phys, tx));

	mutex_enter(&lag->sl_lock);
	avl_add(&lag->sl_recs, lr);
	mutex_exit(&lag->sl_lock);
	return (lr);
}

/* Append entries; returns the index of the first one.  Syncing context. */
uint64_t
spa_lag_rec_append(spa_lag_t *lag, spa_lag_rec_t *lr, const uint64_t *words,
    uint64_t nentries, dmu_tx_t *tx)
{
	uint64_t ew = lag->sl_ops->slo_entry_words * sizeof (uint64_t);
	uint64_t first = lr->lr_phys.pr_nentries;

	ASSERT(dmu_tx_is_syncing(tx));
	ASSERT3U(lr->lr_txg, ==, dmu_tx_get_txg(tx));
	if (nentries == 0)
		return (first);
	dmu_write(spa_lag_mos(lag), lr->lr_phys.pr_obj, first * ew,
	    nentries * ew, words, tx, 0);
	lr->lr_phys.pr_nentries += nentries;
	lr->lr_dirty = B_TRUE;
	return (first);
}

/* Overwrite one word of one entry (e.g. its kind, to VOID it). */
void
spa_lag_rec_write_word(spa_lag_t *lag, spa_lag_rec_t *lr, uint64_t entry,
    uint64_t word, uint64_t value, dmu_tx_t *tx)
{
	uint64_t ew = lag->sl_ops->slo_entry_words;

	ASSERT(dmu_tx_is_syncing(tx));
	ASSERT3U(entry, <, lr->lr_phys.pr_nentries);
	ASSERT3U(word, <, ew);
	dmu_write(spa_lag_mos(lag), lr->lr_phys.pr_obj,
	    (entry * ew + word) * sizeof (uint64_t), sizeof (uint64_t),
	    &value, tx, 0);
}

int
spa_lag_rec_read(spa_lag_t *lag, spa_lag_rec_t *lr, uint64_t first,
    uint64_t nentries, uint64_t *buf)
{
	uint64_t ew = lag->sl_ops->slo_entry_words * sizeof (uint64_t);

	if (first + nentries > lr->lr_phys.pr_nentries)
		return (SET_ERROR(EINVAL));
	return (dmu_read(spa_lag_mos(lag), lr->lr_phys.pr_obj, first * ew,
	    nentries * ew, buf, DMU_READ_NO_PREFETCH));
}

/* Write the header if it changed.  Syncing context. */
void
spa_lag_rec_sync(spa_lag_t *lag, spa_lag_rec_t *lr, dmu_tx_t *tx)
{
	ASSERT(dmu_tx_is_syncing(tx));
	if (!lr->lr_dirty)
		return;
	VERIFY0(zap_update_uint64(spa_lag_mos(lag), lag->sl_dir[SLD_INDEX],
	    &lr->lr_txg, 1, sizeof (uint64_t), SPA_LAG_REC_WORDS,
	    &lr->lr_phys, tx));
	lr->lr_dirty = B_FALSE;
}

/*
 * Release the records up to and including applied_txg, oldest first, within
 * a budget of entries (each record counts at least one).  The consumer
 * releases the entries (its held frees); a record that is done is destroyed.
 * Syncing context, pass 1.
 *
 * A reader of records (spa_lag_recs_hold()) or a long walk
 * (spa_lag_pause_release()) keeps the release away: the sync thread only
 * tries the lock and leaves the records to a later txg.
 */
void
spa_lag_release_sync(spa_lag_t *lag, uint64_t applied_txg, uint64_t budget,
    dmu_tx_t *tx)
{
	objset_t *mos = spa_lag_mos(lag);
	spa_lag_rec_t *lr;
	uint_t paused;

	ASSERT(dmu_tx_is_syncing(tx));
	ASSERT(spa_writeable(lag->sl_spa));
	mutex_enter(&lag->sl_lock);
	paused = lag->sl_pause;
	mutex_exit(&lag->sl_lock);
	if (paused != 0 || !rw_tryenter(&lag->sl_recs_rwlock, RW_WRITER))
		return;

	while (budget > 0 && (lr = avl_first(&lag->sl_recs)) != NULL &&
	    lr->lr_txg <= applied_txg && lr->lr_txg < dmu_tx_get_txg(tx)) {
		boolean_t done = B_TRUE;

		if (lag->sl_ops->slo_release_rec != NULL) {
			done = lag->sl_ops->slo_release_rec(lag->sl_arg, lr,
			    &budget, tx);
		}
		if (!done) {
			spa_lag_rec_sync(lag, lr, tx);
			break;
		}

		VERIFY0(dmu_object_free(mos, lr->lr_phys.pr_obj, tx));
		VERIFY0(zap_remove_uint64(mos, lag->sl_dir[SLD_INDEX],
		    &lr->lr_txg, 1, tx));
		mutex_enter(&lag->sl_lock);
		avl_remove(&lag->sl_recs, lr);
		mutex_exit(&lag->sl_lock);
		kmem_free(lr, sizeof (*lr));
		if (budget > 0)
			budget--;
	}
	rw_exit(&lag->sl_recs_rwlock);
	spa_lag_wake(lag);
}

void
spa_lag_recs_hold(spa_lag_t *lag)
{
	rw_enter(&lag->sl_recs_rwlock, RW_READER);
}

void
spa_lag_recs_rele(spa_lag_t *lag)
{
	rw_exit(&lag->sl_recs_rwlock);
}

void
spa_lag_pause_release(spa_lag_t *lag)
{
	mutex_enter(&lag->sl_lock);
	lag->sl_pause++;
	mutex_exit(&lag->sl_lock);
}

void
spa_lag_resume_release(spa_lag_t *lag)
{
	mutex_enter(&lag->sl_lock);
	ASSERT3U(lag->sl_pause, >, 0);
	lag->sl_pause--;
	mutex_exit(&lag->sl_lock);
}

/*
 * Sum the unreleased records.  Any context; the numbers are a snapshot, and
 * the sync thread may change a header while it is read.
 */
void
spa_lag_summarize(spa_lag_t *lag, uint64_t applied_txg, spa_lag_summary_t *sls)
{
	spa_lag_rec_t *lr;

	memset(sls, 0, sizeof (*sls));
	mutex_enter(&lag->sl_lock);
	for (lr = avl_first(&lag->sl_recs); lr != NULL;
	    lr = AVL_NEXT(&lag->sl_recs, lr)) {
		const spa_lag_rec_phys_t *p = &lr->lr_phys;

		if (sls->sls_nrecs++ == 0)
			sls->sls_first_txg = lr->lr_txg;
		sls->sls_last_txg = lr->lr_txg;
		if (lr->lr_txg > applied_txg) {
			sls->sls_pending_bytes += p->pr_alloc_asize;
			sls->sls_pending_txgs++;
		}
		if (p->pr_held_asize > p->pr_rel_asize) {
			sls->sls_held_bytes +=
			    p->pr_held_asize - p->pr_rel_asize;
		}
		sls->sls_record_bytes += p->pr_nentries *
		    lag->sl_ops->slo_entry_words * sizeof (uint64_t);
		sls->sls_flags |= p->pr_flags;
	}
	mutex_exit(&lag->sl_lock);
}

/*
 * Free a block that was held.  Each DVA is freed as a normal free, so it
 * goes through the metaslab's two-txg defer; the hold hook is not asked.
 */
void
spa_lag_free_held(spa_t *spa, const blkptr_t *bp, dmu_tx_t *tx)
{
	ASSERT(dmu_tx_is_syncing(tx));
	metaslab_free_held(spa, bp);
}

/*
 * The outside has applied every record up to txg.
 */
int
spa_lag_applied(spa_lag_t *lag, uint64_t txg, int flags)
{
	spa_t *spa = lag->sl_spa;
	boolean_t force = (flags & SPA_LAG_FORCE) != 0;

	if (!spa_writeable(spa))
		return (SET_ERROR(EROFS));
	if (txg > spa_last_synced_txg(spa))
		return (SET_ERROR(EINVAL));
	spa_lag_progress(lag);

	/* Back-pressure also forces the release (no wait for a write). */
	if (spa_lag_level(lag) != SPA_LAG_LEVEL_NONE)
		force = B_TRUE;
	if (lag->sl_ops->slo_applied != NULL)
		lag->sl_ops->slo_applied(lag->sl_arg, txg, force);

	if (flags & SPA_LAG_FORCE) {
		return (txg_wait_synced_flags(spa_get_dsl(spa), 0,
		    TXG_WAIT_SUSPEND));
	}
	return (0);
}

void
spa_lag_progress(spa_lag_t *lag)
{
	lag->sl_applied_time = gethrtime();
}

/*
 * Note the throttle level at "now": start or stop the throttle timer when the
 * level moves between none and soft or hard.  Each sync and each read of the
 * time sample the level, so the time agrees with the level that INFO and the
 * kstat show, also on a pool that has not synced since it was loaded.
 */
static void
spa_lag_throttle_note(spa_lag_t *lag, spa_lag_level_t level, hrtime_t now)
{
	boolean_t on = (level != SPA_LAG_LEVEL_NONE);

	ASSERT(MUTEX_HELD(&lag->sl_lock));
	if (on == lag->sl_thr_on)
		return;
	if (lag->sl_thr_on)
		lag->sl_thr_total += now - lag->sl_thr_since;
	else
		lag->sl_thr_since = now;
	lag->sl_thr_on = on;
}

static spa_lag_level_t spa_lag_calc(spa_lag_t *, uint64_t *);

/* The time the writers spent at the soft or hard level, in ms. */
uint64_t
spa_lag_throttled_ms(spa_lag_t *lag)
{
	uint64_t pm;
	spa_lag_level_t level = spa_lag_calc(lag, &pm);
	hrtime_t now = gethrtime();
	hrtime_t t;

	mutex_enter(&lag->sl_lock);
	spa_lag_throttle_note(lag, level, now);
	t = lag->sl_thr_total;
	if (lag->sl_thr_on)
		t += now - lag->sl_thr_since;
	mutex_exit(&lag->sl_lock);
	return (NSEC2MSEC(t));
}

/*
 * The time since the consumer last applied a txg or reported progress, in
 * ms; 0 when it has not done so since the pool was loaded.
 */
uint64_t
spa_lag_applied_age_ms(spa_lag_t *lag)
{
	hrtime_t t = lag->sl_applied_time;

	return (t == 0 ? 0 : NSEC2MSEC(gethrtime() - t));
}

/*
 * A user drop.  The consumer starts it in syncing context (a request flag
 * that its pass-1 hook turns into the DROPPING state, so the change is made
 * with the txg's own tables in view); this waits until it is complete.
 * A signal ends the wait, not the drop.
 */
int
spa_lag_drop(spa_t *spa)
{
	spa_lag_t *lag;
	int i, error;

	if (!spa_writeable(spa))
		return (SET_ERROR(EROFS));

	SPA_LAG_FOREACH(spa, lag, i) {
		if (lag->sl_ops->slo_drop == NULL)
			continue;
		error = lag->sl_ops->slo_drop(lag->sl_arg);
		if (error == ENOENT)
			continue;
		if (error != 0)
			return (error);
		while (lag->sl_ops->slo_drop_pending != NULL &&
		    lag->sl_ops->slo_drop_pending(lag->sl_arg)) {
			error = txg_wait_synced_flags(spa_get_dsl(spa), 0,
			    TXG_WAIT_SIGNAL | TXG_WAIT_SUSPEND);
			if (error != 0)
				return (error);
		}
	}
	return (0);
}

/*
 * The write throttle and the limits.
 *
 * Two limits, both per pool: the record space (bytes of record objects) and
 * the held space (bytes of frees not released).  They are measured on the
 * consumer's counters and are not the amount of pending data: that costs
 * nothing in the pool; only the records and the held frees do.  The limits
 * come from the pool properties parity_max_records (bytes) and
 * parity_max_held (percent of the pool); 0 means the module default.
 *
 * "used / limit", in per mille, drives three levels: below the start of the
 * dirty-data delay curve (zfs_delay_min_dirty_percent) there is none; from
 * there to the limit, writers are delayed on the same curve as for dirty data;
 * at the limit (the hard level) the property parity_onstall decides.  With
 * "continue" (the default) the consumer drops its records and the pool goes
 * on, unprotected; with "wait" writers wait until the outside catches up.
 */
uint64_t
spa_lag_max_records(spa_t *spa)
{
	uint64_t max = spa->spa_parity_max_records;

	if (max == 0) {
		uint64_t space =
		    metaslab_class_get_space(spa_normal_class(spa));
		uint_t pct = MIN(zfs_parity_max_record_pct, 100);

		max = zfs_parity_max_record_bytes;
		/* 0: the byte limit alone. */
		if (pct != 0)
			max = MIN(max, space / 100 * pct);
	}
	return (MAX(max, 1));
}

uint64_t
spa_lag_max_held(spa_t *spa)
{
	uint64_t pct = spa->spa_parity_max_held;
	uint64_t space = metaslab_class_get_space(spa_normal_class(spa));

	if (pct == 0)
		pct = zfs_parity_max_held_pct;
	pct = MIN(pct, 100);
	return (MAX(space / 100 * pct, 1));
}

static uint64_t
spa_lag_permille(uint64_t used, uint64_t max)
{
	if (used >= max)
		return (1000);
	if (used < UINT64_MAX / 1000)
		return (used * 1000 / max);
	return (used / (max / 1000 + 1));
}

/*
 * Pacing is on for this consumer: the pool asks for it, and the outside
 * called APPLIED a short time ago.  Read without a lock: a stale value only
 * moves one delay between the two curves.
 */
static boolean_t
spa_lag_pacing(spa_lag_t *lag)
{
	hrtime_t t = lag->sl_applied_time;

	return (lag->sl_spa->spa_parity_pace != 0 && t != 0 &&
	    gethrtime() - t < MSEC2NSEC((hrtime_t)zfs_parity_pace_idle_ms));
}

/*
 * The level, the per-mille use of the limits (*pmp), and the delay curve: the
 * per-mille point where it starts (*minp) and its scale (*scalep).  Pacing is
 * read once here, so that the level and the curve agree.
 */
static spa_lag_level_t
spa_lag_calc_curve(spa_lag_t *lag, uint64_t *pmp, uint64_t *minp,
    uint64_t *scalep)
{
	spa_t *spa = lag->sl_spa;
	spa_lag_usage_t u = { 0 };
	uint64_t pm;

	*pmp = 0;
	if (spa_lag_pacing(lag)) {
		*minp = (uint64_t)MIN(zfs_parity_pace_min_pct, 99) * 10;
		*scalep = zfs_parity_pace_scale;
	} else {
		*minp = (uint64_t)zfs_delay_min_dirty_percent * 10;
		*scalep = zfs_delay_scale;
	}
	if (lag->sl_ops->slo_usage == NULL)
		return (SPA_LAG_LEVEL_NONE);
	lag->sl_ops->slo_usage(lag->sl_arg, &u);
	if (!u.slu_limited)
		return (SPA_LAG_LEVEL_NONE);

	pm = MAX(spa_lag_permille(u.slu_record_bytes,
	    spa_lag_max_records(spa)),
	    spa_lag_permille(u.slu_held_bytes, spa_lag_max_held(spa)));
	*pmp = pm;
	if (pm >= 1000)
		return (SPA_LAG_LEVEL_HARD);
	if (pm >= *minp)
		return (SPA_LAG_LEVEL_SOFT);
	return (SPA_LAG_LEVEL_NONE);
}

static spa_lag_level_t
spa_lag_calc(spa_lag_t *lag, uint64_t *pmp)
{
	uint64_t min, scale;

	return (spa_lag_calc_curve(lag, pmp, &min, &scale));
}

spa_lag_level_t
spa_lag_level(spa_lag_t *lag)
{
	uint64_t pm;

	return (spa_lag_calc(lag, &pm));
}

/*
 * The delay of one write, in ns (dmu_tx_delay() caps it).  At the hard limit
 * with "continue" this is also where the consumer is told to drop.
 */
hrtime_t
spa_lag_delay_impl(spa_t *spa)
{
	spa_lag_t *lag;
	hrtime_t delay = 0;
	int i;

	if (spa->spa_lag[0] == NULL)
		return (0);
	SPA_LAG_FOREACH(spa, lag, i) {
		uint64_t pm, min, scale;
		hrtime_t d;

		switch (spa_lag_calc_curve(lag, &pm, &min, &scale)) {
		case SPA_LAG_LEVEL_NONE:
			continue;
		case SPA_LAG_LEVEL_HARD:
			if (spa->spa_parity_onstall ==
			    ZPOOL_PARITY_ONSTALL_CONTINUE &&
			    lag->sl_ops->slo_over_limit != NULL)
				(void) lag->sl_ops->slo_over_limit(lag->sl_arg);
			d = scale * 1000;
			break;
		default:
			d = scale * (pm - min) / (1000 - pm);
			break;
		}
		delay = MAX(delay, d);
	}
	return (delay);
}

/*
 * parity_onstall=wait, hard limit: the writer waits.  The release is forced
 * each second, and a txg is kicked so that the forced release runs at once
 * (an idle pool would wait for the txg timeout), so the wait ends soon after
 * the outside has applied records.  A signal ends the wait: the daemon may be
 * down, and the writer must be killable.  Returns EINTR then, and the caller
 * returns it to the writer.
 * Called from dmu_tx_wait(), never from a sync task or the sync thread.
 */
int
spa_lag_wait_impl(spa_t *spa)
{
	spa_lag_t *lag;
	int i;

	if (spa->spa_lag[0] == NULL ||
	    spa->spa_parity_onstall != ZPOOL_PARITY_ONSTALL_WAIT)
		return (0);
	SPA_LAG_FOREACH(spa, lag, i) {
		uint64_t pm;

		while (spa_lag_calc(lag, &pm) == SPA_LAG_LEVEL_HARD &&
		    !spa_suspended(spa) && !spa_shutting_down(spa)) {
			int r;

			if (lag->sl_ops->slo_force != NULL)
				lag->sl_ops->slo_force(lag->sl_arg);
			txg_kick(spa_get_dsl(spa), spa_syncing_txg(spa) + 1);
			mutex_enter(&lag->sl_wait_lock);
			r = cv_timedwait_sig(&lag->sl_wait_cv,
			    &lag->sl_wait_lock, ddi_get_lbolt() + hz);
			mutex_exit(&lag->sl_wait_lock);
			if (r == 0)	/* a signal */
				return (SET_ERROR(EINTR));
		}
	}
	return (0);
}

void
spa_lag_lock(spa_lag_t *lag)
{
	mutex_enter(&lag->sl_lock);
}

void
spa_lag_unlock(spa_lag_t *lag)
{
	mutex_exit(&lag->sl_lock);
}

void
spa_lag_wake(spa_lag_t *lag)
{
	mutex_enter(&lag->sl_wait_lock);
	cv_broadcast(&lag->sl_wait_cv);
	mutex_exit(&lag->sl_wait_lock);
}

/*
 * Pacing of frees that run in syncing context (the async destroy of a large
 * dataset): each free adds a record entry and returns no space until the
 * outside has applied it, so such a destroy stops for this txg when the
 * counters pass the start of the delay curve, and goes on in a later txg.  It
 * only reads counters, so it never stalls the sync thread.
 */
boolean_t
spa_lag_free_pause_impl(spa_t *spa)
{
	spa_lag_t *lag;
	int i;

	if (spa->spa_lag[0] == NULL)
		return (B_FALSE);
	SPA_LAG_FOREACH(spa, lag, i) {
		if (spa_lag_level(lag) != SPA_LAG_LEVEL_NONE)
			return (B_TRUE);
	}
	return (B_FALSE);
}

/*
 * Zevents.  The class names come from the consumer; the common keys are the
 * consumer name, the level and the numbers the limits are measured on.
 */
static void
spa_lag_throttle_update(spa_lag_t *lag)
{
	spa_t *spa = lag->sl_spa;
	spa_lag_usage_t u = { 0 };
	spa_lag_level_t level;
	const char *level_name;
	hrtime_t now = gethrtime();
	nvlist_t *nvl;
	uint64_t pm;

	level = spa_lag_calc(lag, &pm);
	switch (level) {
	case SPA_LAG_LEVEL_SOFT:
		level_name = "soft";
		break;
	case SPA_LAG_LEVEL_HARD:
		level_name = "hard";
		break;
	default:
		level_name = "none";
		break;
	}
	/*
	 * The time at the soft or hard level counts in every txg, not only
	 * when an event goes out (the events are rate limited).
	 */
	mutex_enter(&lag->sl_lock);
	spa_lag_throttle_note(lag, level, now);
	mutex_exit(&lag->sl_lock);

	if (level == lag->sl_level || lag->sl_ops->slo_event_throttle == NULL)
		return;
	if (now - lag->sl_level_time <
	    MSEC2NSEC((hrtime_t)zfs_parity_throttle_event_ms))
		return;

	lag->sl_ops->slo_usage(lag->sl_arg, &u);
	nvl = fnvlist_alloc();
	fnvlist_add_string(nvl, "consumer", lag->sl_ops->slo_name);
	fnvlist_add_string(nvl, "level", level_name);
	fnvlist_add_uint64(nvl, "record_bytes", u.slu_record_bytes);
	fnvlist_add_uint64(nvl, "held_bytes", u.slu_held_bytes);
	fnvlist_add_uint64(nvl, "max_record_bytes", spa_lag_max_records(spa));
	fnvlist_add_uint64(nvl, "max_held_bytes", spa_lag_max_held(spa));
	if (lag->sl_ops->slo_event_payload != NULL)
		lag->sl_ops->slo_event_payload(lag->sl_arg, nvl);
	spa_event_notify(spa, NULL, nvl, lag->sl_ops->slo_event_throttle);
	fnvlist_free(nvl);
	lag->sl_level = level;
	lag->sl_level_time = now;
}

/* "Not protected, resync needed."  Syncing context. */
void
spa_lag_post_unprotected(spa_lag_t *lag, const char *reason)
{
	nvlist_t *nvl;

	if (lag->sl_ops->slo_event_unprotected == NULL)
		return;
	nvl = fnvlist_alloc();
	fnvlist_add_string(nvl, "consumer", lag->sl_ops->slo_name);
	fnvlist_add_string(nvl, "reason", reason);
	if (lag->sl_ops->slo_event_payload != NULL)
		lag->sl_ops->slo_event_payload(lag->sl_arg, nvl);
	spa_event_notify(lag->sl_spa, NULL, nvl,
	    lag->sl_ops->slo_event_unprotected);
	fnvlist_free(nvl);
}

void
spa_lag_note_alloc_impl(spa_t *spa, const blkptr_t *bp, uint64_t txg)
{
	spa_lag_t *lag;
	int i;

	if (spa->spa_lag[0] == NULL)
		return;
	SPA_LAG_FOREACH(spa, lag, i) {
		if (lag->sl_ops->slo_note_alloc != NULL)
			lag->sl_ops->slo_note_alloc(lag->sl_arg, bp, txg);
	}
}

void
spa_lag_note_unalloc_impl(spa_t *spa, const blkptr_t *bp, uint64_t txg)
{
	spa_lag_t *lag;
	int i;

	if (spa->spa_lag[0] == NULL)
		return;
	SPA_LAG_FOREACH(spa, lag, i) {
		if (lag->sl_ops->slo_note_unalloc != NULL)
			lag->sl_ops->slo_note_unalloc(lag->sl_arg, bp, txg);
	}
}

/*
 * Returns the mask of the DVAs that a consumer holds: the caller must not
 * free those.  A consumer may also cancel its own pending entry here.
 */
uint_t
spa_lag_hold_free_impl(spa_t *spa, const blkptr_t *bp, uint64_t txg)
{
	spa_lag_t *lag;
	uint_t held = 0;
	int i;

	if (spa->spa_lag[0] == NULL)
		return (0);
	SPA_LAG_FOREACH(spa, lag, i) {
		if (lag->sl_ops->slo_hold_free != NULL) {
			held |= lag->sl_ops->slo_hold_free(lag->sl_arg, bp,
			    txg);
		}
	}
	return (held);
}

void
spa_lag_write_ready_impl(spa_t *spa, const blkptr_t *bp, uint64_t txg)
{
	spa_lag_t *lag;
	int i;

	if (spa->spa_lag[0] == NULL)
		return;
	SPA_LAG_FOREACH(spa, lag, i) {
		if (lag->sl_ops->slo_write_ready != NULL)
			lag->sl_ops->slo_write_ready(lag->sl_arg, bp, txg);
	}
}

/*
 * spa_sync_iterate_to_convergence(), pass 1, before the no-op test: the
 * release and the fix-ups.  A consumer must write nothing when noop is set,
 * unless it must force a txg.
 */
void
spa_lag_sync_pass_impl(spa_t *spa, dmu_tx_t *tx, boolean_t noop)
{
	spa_lag_t *lag;
	int i;

	if (spa->spa_lag[0] == NULL)
		return;
	/*
	 * The pool is shutting down (export, destroy): the txgs after the
	 * final dirty txg only clear out the deferred frees, and must dirty
	 * nothing (see ddt_sync_flush_log()).  A release would free a record
	 * object and held space, and a drain or a drop would write the
	 * directory.  They go on at the next import, from the state on disk.
	 */
	if (dmu_tx_get_txg(tx) > spa_final_dirty_txg(spa))
		return;
	SPA_LAG_FOREACH(spa, lag, i) {
		if (lag->sl_ops->slo_sync_pass != NULL)
			lag->sl_ops->slo_sync_pass(lag->sl_arg, tx, noop);
	}
}

/* The end of every pass: write what the pass captured. */
void
spa_lag_flush_impl(spa_t *spa, dmu_tx_t *tx)
{
	spa_lag_t *lag;
	int i;

	if (spa->spa_lag[0] == NULL)
		return;
	SPA_LAG_FOREACH(spa, lag, i) {
		if (lag->sl_ops->slo_flush != NULL)
			lag->sl_ops->slo_flush(lag->sl_arg, tx);
	}
}

void
spa_lag_txg_synced_impl(spa_t *spa, uint64_t txg)
{
	spa_lag_t *lag;
	int i;

	if (spa->spa_lag[0] == NULL)
		return;
	SPA_LAG_FOREACH(spa, lag, i) {
		if (lag->sl_ops->slo_txg_synced != NULL)
			lag->sl_ops->slo_txg_synced(lag->sl_arg, txg);
		spa_lag_throttle_update(lag);
		spa_lag_wake(lag);
	}
}

void
spa_lag_verify_alloc_impl(spa_t *spa, vdev_t *vd, zfs_range_tree_t *rt,
    uint64_t txg)
{
	spa_lag_t *lag;
	int i;

	if (spa->spa_lag[0] == NULL)
		return;
	SPA_LAG_FOREACH(spa, lag, i) {
		if (lag->sl_ops->slo_verify_alloc != NULL)
			lag->sl_ops->slo_verify_alloc(lag->sl_arg, vd, rt, txg);
	}
}

uint64_t
spa_lag_held_dspace_impl(spa_t *spa)
{
	spa_lag_t *lag;
	uint64_t held = 0;
	int i;

	if (spa->spa_lag[0] == NULL)
		return (0);
	SPA_LAG_FOREACH(spa, lag, i) {
		if (lag->sl_ops->slo_held_dspace != NULL)
			held += lag->sl_ops->slo_held_dspace(lag->sl_arg);
	}
	return (held);
}

ZFS_MODULE_PARAM(zfs_parity, zfs_parity_, max_record_bytes, U64, ZMOD_RW,
	"Default limit of the record space of a parity set member, in bytes "
	"(pool property parity_max_records overrides it)");

ZFS_MODULE_PARAM(zfs_parity, zfs_parity_, max_record_pct, UINT, ZMOD_RW,
	"Default limit of the record space of a parity set member, in percent "
	"of the pool; the smaller of this and max_record_bytes applies");

ZFS_MODULE_PARAM(zfs_parity, zfs_parity_, max_held_pct, UINT, ZMOD_RW,
	"Default limit of the held space of a parity set member, in percent of "
	"the pool (pool property parity_max_held overrides it)");

ZFS_MODULE_PARAM(zfs_parity, zfs_parity_, throttle_event_ms, UINT, ZMOD_RW,
	"At most one parity_throttle event in this time");

ZFS_MODULE_PARAM(zfs_parity, zfs_parity_, pace_min_pct, UINT, ZMOD_RW,
	"With parity_pace=on: the delay of writers starts at this percent of "
	"the record and held limits");

ZFS_MODULE_PARAM(zfs_parity, zfs_parity_, pace_scale, U64, ZMOD_RW,
	"With parity_pace=on: the scale of the delay curve, in ns (as "
	"zfs_delay_scale)");

ZFS_MODULE_PARAM(zfs_parity, zfs_parity_, pace_idle_ms, UINT, ZMOD_RW,
	"With parity_pace=on: pace writers only while APPLIED was called "
	"within this time");
