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
#include <sys/sysevent/eventdefs.h>
#include <sys/spa_lag.h>
#include <sys/spa_parity_impl.h>
#include <sys/vdev_impl.h>
#include <sys/vdev.h>
#include <sys/zio.h>
#include <sys/zio_checksum.h>
#include <sys/abd.h>
#include <sys/taskq.h>

/* Requests kept in the table; when it is full, the oldest is dropped. */
static uint_t zfs_parity_repair_max_requests = 1024;
/* Time to wait for the parity service to answer a failed read. */
static uint_t zfs_parity_repair_wait_ms = 30000;
/* A request that no answer has reached in this time is dropped. */
static uint_t zfs_parity_repair_timeout_ms = 10 * 60 * 1000;
/* At most this many zevents each second for the whole pool (0: no events). */
static uint_t zfs_parity_repair_events_per_sec = 100;
/* A posted request whose block fails again after this long is posted again. */
#define	PARITY_REPAIR_REPOST_NS	SEC2NSEC(5)

/* At most this many reads of one pool wait for the parity service at once. */
static uint_t zfs_parity_repair_max_waiters = 8;

static void parity_test_write_delay(void);

/*
 * The repair-provider role is held for each pool: one open
 * /dev/zfs handle owns the role of one pool GUID, so two parity sets on one
 * host each have their own daemon.  The table is small (one slot for each
 * member pool of the host) and only error paths and the daemon use it.
 */
#define	PARITY_PROVIDERS_MAX	256
typedef struct parity_provider {
	uint64_t	pp_pool_guid;
	uint64_t	pp_token;
} parity_provider_t;
static parity_provider_t parity_providers[PARITY_PROVIDERS_MAX];
static kmutex_t parity_providers_lock;

typedef struct parity_req {
	avl_node_t	rq_id_node;
	avl_node_t	rq_dva_node;
	uint64_t	rq_id;
	uint64_t	rq_vdev;	/* DVA: top-level vdev id */
	uint64_t	rq_offset;	/* DVA: offset */
	uint64_t	rq_asize;	/* on-disk bytes of this DVA */
	uint64_t	rq_psize;	/* bytes that the checksum covers */
	uint64_t	rq_birth;	/* physical birth txg */
	uint_t		rq_cksum;	/* checksum type that is checked */
	int		rq_reason;	/* ECKSUM or EIO */
	hrtime_t	rq_created;
	hrtime_t	rq_post_time;	/* last post to a taskq */
	boolean_t	rq_posted;	/* the event was handed to a taskq */
	boolean_t	rq_busy;	/* spa_parity_repair() works on it */
	boolean_t	rq_invalid;	/* the block was freed */
	boolean_t	rq_inval_busy;	/* freed while busy (pr_inval_busy) */
	boolean_t	rq_sync_waiting;	/* wait for repair data */
	boolean_t	rq_sync_answered;
	boolean_t	rq_sync_repaired;
	boolean_t	rq_sync_declined;	/* no rebuild is possible */
	uint8_t		*rq_sync_data;
	uint_t		rq_sync_len;
	zbookmark_phys_t rq_zb;	/* of the failed read: heal its error */
	blkptr_t	rq_bp;
} parity_req_t;

typedef struct parity_repair {
	spa_t		*pr_spa;
	kmutex_t	pr_lock;
	kcondvar_t	pr_cv;		/* rq_busy cleared, pr_tasks changed */
	avl_tree_t	pr_by_id;	/* request id: the oldest is first */
	avl_tree_t	pr_by_dva;	/* (vdev, offset) */
	uint64_t	pr_count;	/* read without the lock by the free */
	uint64_t	pr_next_id;
	hrtime_t	pr_window;	/* start of the event rate window */
	uint_t		pr_window_events;
	uint_t		pr_tasks;	/* event tasks, waiters and writes */
	uint64_t	pr_inval_busy;	/* freed, a write may still run */
} parity_repair_t;

typedef struct parity_post {
	parity_repair_t	*pp_repair;
	uint64_t	pp_id;
} parity_post_t;

static int
parity_req_id_compare(const void *a, const void *b)
{
	const parity_req_t *ra = a, *rb = b;

	return (TREE_CMP(ra->rq_id, rb->rq_id));
}

static int
parity_req_dva_compare(const void *a, const void *b)
{
	const parity_req_t *ra = a, *rb = b;
	int c = TREE_CMP(ra->rq_vdev, rb->rq_vdev);

	if (c != 0)
		return (c);
	return (TREE_CMP(ra->rq_offset, rb->rq_offset));
}

void
spa_parity_repair_global_init(void)
{
	mutex_init(&parity_providers_lock, NULL, MUTEX_DEFAULT, NULL);
}

void
spa_parity_repair_global_fini(void)
{
	mutex_destroy(&parity_providers_lock);
}

/*
 * Take the provider role of one pool for this handle.  A handle may hold
 * the role of many pools.  EBUSY: another handle holds it.  ENOSPC: the
 * table is full.
 */
int
spa_parity_repair_provider_register(void *provider, uint64_t pool_guid)
{
	uint64_t token = (uintptr_t)provider;
	int free_slot = -1;

	if (token == 0 || pool_guid == 0)
		return (SET_ERROR(EINVAL));
	mutex_enter(&parity_providers_lock);
	for (int i = 0; i < PARITY_PROVIDERS_MAX; i++) {
		parity_provider_t *pp = &parity_providers[i];

		if (pp->pp_pool_guid == pool_guid) {
			int error = (pp->pp_token == token) ? 0 :
			    SET_ERROR(EBUSY);
			mutex_exit(&parity_providers_lock);
			return (error);
		}
		if (pp->pp_pool_guid == 0 && free_slot < 0)
			free_slot = i;
	}
	if (free_slot < 0) {
		mutex_exit(&parity_providers_lock);
		return (SET_ERROR(ENOSPC));
	}
	parity_providers[free_slot].pp_pool_guid = pool_guid;
	parity_providers[free_slot].pp_token = token;
	mutex_exit(&parity_providers_lock);
	return (0);
}

/* The handle closes: it gives up the role of every pool it held. */
void
spa_parity_repair_provider_unregister(void *provider)
{
	uint64_t token = (uintptr_t)provider;

	if (token == 0)
		return;
	mutex_enter(&parity_providers_lock);
	for (int i = 0; i < PARITY_PROVIDERS_MAX; i++) {
		if (parity_providers[i].pp_token == token) {
			parity_providers[i].pp_pool_guid = 0;
			parity_providers[i].pp_token = 0;
		}
	}
	mutex_exit(&parity_providers_lock);
}

static uint64_t
parity_provider_of(uint64_t pool_guid)
{
	uint64_t token = 0;

	mutex_enter(&parity_providers_lock);
	for (int i = 0; i < PARITY_PROVIDERS_MAX; i++) {
		if (parity_providers[i].pp_pool_guid == pool_guid) {
			token = parity_providers[i].pp_token;
			break;
		}
	}
	mutex_exit(&parity_providers_lock);
	return (token);
}

boolean_t
spa_parity_repair_provider_available(spa_t *spa)
{
	return (parity_provider_of(spa_guid(spa)) != 0);
}

boolean_t
spa_parity_repair_provider_authorized(void *provider, uint64_t pool_guid)
{
	return (provider != NULL &&
	    parity_provider_of(pool_guid) == (uintptr_t)provider);
}

void
spa_parity_repair_init(spa_parity_t *ps)
{
	parity_repair_t *pr = kmem_zalloc(sizeof (*pr), KM_SLEEP);

	pr->pr_spa = ps->ps_spa;
	mutex_init(&pr->pr_lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&pr->pr_cv, NULL, CV_DEFAULT, NULL);
	avl_create(&pr->pr_by_id, parity_req_id_compare, sizeof (parity_req_t),
	    offsetof(parity_req_t, rq_id_node));
	avl_create(&pr->pr_by_dva, parity_req_dva_compare,
	    sizeof (parity_req_t), offsetof(parity_req_t, rq_dva_node));
	pr->pr_next_id = 1;
	ps->ps_repair = pr;
}

/* Only in spa_unload(): nobody calls a repair on a pool that is going. */
void
spa_parity_repair_fini(spa_parity_t *ps)
{
	parity_repair_t *pr = ps->ps_repair;
	parity_req_t *rq;
	void *cookie = NULL, *cookie2 = NULL;

	if (pr == NULL)
		return;
	ps->ps_repair = NULL;

	mutex_enter(&pr->pr_lock);
	while (pr->pr_tasks != 0)
		cv_wait(&pr->pr_cv, &pr->pr_lock);
	mutex_exit(&pr->pr_lock);

	/*
	 * Both trees hold each request: empty the DVA tree first (nothing is
	 * freed there), then free each request from the id tree.
	 */
	while (avl_destroy_nodes(&pr->pr_by_dva, &cookie2) != NULL)
		;
	while ((rq = avl_destroy_nodes(&pr->pr_by_id, &cookie)) != NULL) {
		if (rq->rq_sync_data != NULL)
			kmem_free(rq->rq_sync_data, rq->rq_sync_len);
		kmem_free(rq, sizeof (*rq));
	}
	avl_destroy(&pr->pr_by_id);
	avl_destroy(&pr->pr_by_dva);
	cv_destroy(&pr->pr_cv);
	mutex_destroy(&pr->pr_lock);
	kmem_free(pr, sizeof (*pr));
}

/* The caller holds pr_lock for these table helpers. */
static void
parity_req_idle(parity_repair_t *pr, parity_req_t *rq)
{
	ASSERT(MUTEX_HELD(&pr->pr_lock));
	rq->rq_busy = B_FALSE;
	if (rq->rq_inval_busy) {
		rq->rq_inval_busy = B_FALSE;
		ASSERT3U(pr->pr_inval_busy, >, 0);
		pr->pr_inval_busy--;
	}
}

static void
parity_req_remove(parity_repair_t *pr, parity_req_t *rq)
{
	ASSERT(MUTEX_HELD(&pr->pr_lock));
	ASSERT(!rq->rq_busy);
	ASSERT(!rq->rq_sync_waiting);
	avl_remove(&pr->pr_by_id, rq);
	avl_remove(&pr->pr_by_dva, rq);
	pr->pr_count--;
	if (rq->rq_sync_data != NULL)
		kmem_free(rq->rq_sync_data, rq->rq_sync_len);
	kmem_free(rq, sizeof (*rq));
}

static boolean_t
parity_req_expired(const parity_req_t *rq, hrtime_t now)
{
	return (now - rq->rq_created >
	    MSEC2NSEC((hrtime_t)zfs_parity_repair_timeout_ms));
}

/* Drop the requests that no answer reached in time (the oldest first). */
static void
parity_req_expire(parity_repair_t *pr, hrtime_t now)
{
	parity_req_t *rq;

	while ((rq = avl_first(&pr->pr_by_id)) != NULL &&
	    !rq->rq_busy && !rq->rq_sync_waiting &&
	    parity_req_expired(rq, now))
		parity_req_remove(pr, rq);
}

static parity_req_t *
parity_req_find_dva(parity_repair_t *pr, uint64_t vdev, uint64_t offset)
{
	parity_req_t search;

	search.rq_vdev = vdev;
	search.rq_offset = offset;
	return (avl_find(&pr->pr_by_dva, &search, NULL));
}

/*
 * The zevent.  taskq context: the event code allocates with KM_SLEEP.
 */

static void
parity_repair_post_task(void *arg)
{
	parity_post_t *pp = arg;
	parity_repair_t *pr = pp->pp_repair;
	spa_t *spa = pr->pr_spa;
	parity_req_t rq, search, *found;
	boolean_t have = B_FALSE;
	vdev_t *tvd = NULL;

	memset(&rq, 0, sizeof (rq));
	search.rq_id = pp->pp_id;
	mutex_enter(&pr->pr_lock);
	found = avl_find(&pr->pr_by_id, &search, NULL);
	if (found != NULL && !found->rq_invalid) {
		rq = *found;
		have = B_TRUE;
	}
	mutex_exit(&pr->pr_lock);

	/*
	 * The vdev tree is read here, under SCL_STATE: the note runs in
	 * zio_done() and holds no config lock.  A request of a vdev that is
	 * not the data disk (for example a log vdev) is dropped.  A gang
	 * request gets its size now.
	 */
	if (have) {
		spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
		tvd = vdev_lookup_top(spa, rq.rq_vdev);
		if (!spa_parity_is_data_vdev(spa, rq.rq_vdev) || tvd == NULL) {
			have = B_FALSE;
		} else if (rq.rq_asize == 0) {
			rq.rq_asize = vdev_gang_header_asize(tvd);
		}
		mutex_enter(&pr->pr_lock);
		found = avl_find(&pr->pr_by_id, &search, NULL);
		if (found != NULL && !found->rq_busy) {
			if (!have) {
				if (found->rq_sync_waiting) {
					found->rq_invalid = B_TRUE;
					cv_broadcast(&pr->pr_cv);
				} else {
					parity_req_remove(pr, found);
				}
			} else {
				found->rq_asize = rq.rq_asize;
			}
		}
		mutex_exit(&pr->pr_lock);
		if (!have)
			spa_config_exit(spa, SCL_STATE, FTAG);
	}

	if (have) {
		nvlist_t *nvl = fnvlist_alloc();
		vdev_t *leaf;

		fnvlist_add_uint64(nvl, "request", rq.rq_id);
		/* the top-level vdev of the DVA: the parity column */
		fnvlist_add_uint64(nvl, "vdev", rq.rq_vdev);
		fnvlist_add_uint64(nvl, "offset", rq.rq_offset);
		fnvlist_add_uint64(nvl, "asize", rq.rq_asize);
		fnvlist_add_uint64(nvl, "psize", rq.rq_psize);
		fnvlist_add_uint64(nvl, "birth_txg", rq.rq_birth);
		fnvlist_add_uint64(nvl, "cksum_type", rq.rq_cksum);
		fnvlist_add_string(nvl, "reason",
		    rq.rq_reason == ECKSUM ? "checksum" : "io");
		/* A read waits for SUPPLY; any other request takes REPAIR. */
		fnvlist_add_boolean_value(nvl, "sync_wait",
		    rq.rq_sync_waiting);

		leaf = spa_parity_tvd_leaf(tvd);
		spa_event_notify(spa, leaf, nvl, ESC_ZFS_PARITY_REPAIR_NEEDED);
		spa_config_exit(spa, SCL_STATE, FTAG);
		fnvlist_free(nvl);
	}

	kmem_free(pp, sizeof (*pp));
	mutex_enter(&pr->pr_lock);
	pr->pr_tasks--;
	cv_broadcast(&pr->pr_cv);
	mutex_exit(&pr->pr_lock);
}

/*
 * Hand the event of a request to a taskq, if the rate allows it.  Never
 * blocks.  Under pr_lock.  A request that was not posted (the rate was
 * reached, or no memory) is tried again when its block fails again.
 */
static void
parity_req_post(parity_repair_t *pr, parity_req_t *rq, hrtime_t now)
{
	parity_post_t *pp;

	ASSERT(MUTEX_HELD(&pr->pr_lock));
	if (zfs_parity_repair_events_per_sec == 0)
		return;
	/*
	 * A dropped zevent must not silence the request for its whole life:
	 * post again when the block fails again some time after the last post.
	 */
	if (rq->rq_posted && now - rq->rq_post_time < PARITY_REPAIR_REPOST_NS)
		return;
	if (now - pr->pr_window >= NANOSEC) {
		pr->pr_window = now;
		pr->pr_window_events = 0;
	}
	if (pr->pr_window_events >= zfs_parity_repair_events_per_sec)
		return;
	pp = kmem_alloc(sizeof (*pp), KM_NOSLEEP);
	if (pp == NULL)
		return;
	pp->pp_repair = pr;
	pp->pp_id = rq->rq_id;
	pr->pr_tasks++;
	if (taskq_dispatch(system_taskq, parity_repair_post_task, pp,
	    TQ_NOSLEEP) == TASKQID_INVALID) {
		pr->pr_tasks--;
		kmem_free(pp, sizeof (*pp));
		return;
	}
	pr->pr_window_events++;
	rq->rq_posted = B_TRUE;
	rq->rq_post_time = now;
}

/*
 * A block with checksum=off (or noparity) has a checksum function that accepts
 * any bytes, so a match proves nothing.  Such a block is never repaired.
 */
static boolean_t
parity_cksum_unchecked(enum zio_checksum cksum)
{
	return (cksum == ZIO_CHECKSUM_OFF || cksum == ZIO_CHECKSUM_NOPARITY);
}

/*
 * The checksum of a block pointer over bytes that are not in a zio.
 *
 * zio_checksum_verify_bp() (zio_checksum.c) does the check without a zio.  The
 * BP is not changed.  A gang header
 * has its own checksum type and size (as zio_checksum_error() does), and an
 * old header is shorter than a dynamic one.
 */
static int
parity_cksum_check(spa_t *spa, const blkptr_t *bp, abd_t *abd, size_t len)
{
	enum zio_checksum cksum;
	uint64_t size, old_size = SPA_OLD_GANGBLOCKSIZE;
	int error;

	if (BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp))
		return (SET_ERROR(EINVAL));
	if (BP_IS_GANG(bp)) {
		cksum = ZIO_CHECKSUM_GANG_HEADER;
		size = old_size;
		if (spa_dynamic_gang_header(spa)) {
			size = UINT64_MAX;
			for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
				vdev_t *vd = vdev_lookup_top(spa,
				    DVA_GET_VDEV(&bp->blk_dva[d]));

				if (vd == NULL)
					return (SET_ERROR(EINVAL));
				size = MIN(size, vdev_gang_header_psize(vd));
			}
		}
	} else {
		cksum = BP_GET_CHECKSUM(bp);
		size = BP_GET_PSIZE(bp);
		if (parity_cksum_unchecked(cksum))
			return (SET_ERROR(EINVAL));
	}
	if (cksum >= ZIO_CHECKSUM_FUNCTIONS ||
	    zio_checksum_table[cksum].ci_func[0] == NULL)
		return (SET_ERROR(EINVAL));
	if (len < size)
		return (SET_ERROR(EINVAL));

	error = zio_checksum_verify_bp(spa, bp, cksum, abd, size, 0);
	if (error != 0 && BP_IS_GANG(bp) && size > old_size)
		error = zio_checksum_verify_bp(spa, bp, cksum, abd, old_size,
		    0);
	return (error);
}

int
spa_parity_bp_verify(spa_t *spa, const blkptr_t *bp, const void *buf,
    size_t len)
{
	abd_t *abd;
	int error;

	if (len == 0)
		return (SET_ERROR(EINVAL));
	/* The embedded checksum of a gang header is computed in place. */
	abd = abd_alloc_for_io(len, B_FALSE);
	abd_copy_from_buf(abd, buf, len);
	error = parity_cksum_check(spa, bp, abd, len);
	abd_free(abd);
	return (error);
}

/*
 * A read failed.  Runs in zio_done(), possibly in an interrupt taskq: it
 * takes only pr_lock (a leaf lock, never held over I/O), allocates with
 * KM_NOSLEEP and takes no config lock.
 *
 * Only a zio that reads a block from the device counts.  The logical zio of a
 * block that is a gang block is not one: it inherits its error from its gang
 * children, and the gang child (or the gang header) that failed has its own
 * zio and notes its own block.  Scrub and resilver reads can post events, but
 * they do not wait for the daemon.  A prefetch (speculative) read does not
 * post an event.
 */
static const char *
parity_repair_wait_skip_reason(spa_t *spa, zio_t *zio)
{
	spa_parity_t *ps = spa->spa_parity;
	dsl_pool_t *dp;
	uint64_t applied_txg;

	if (zfs_parity_repair_wait_ms == 0)
		return ("disabled");
	if (spa_is_initializing(spa))
		return ("pool initialization");
	if ((zio->io_flags & (ZIO_FLAG_TXG_SYNC | ZIO_FLAG_SCAN_THREAD |
	    ZIO_FLAG_SCRUB | ZIO_FLAG_RESILVER)) != 0)
		return ("txg sync, scrub, or resilver I/O");
	dp = spa_get_dsl(spa);
	if (dp == NULL)
		return ("no DSL pool");
	if (dsl_pool_sync_context(dp))
		return ("txg sync context");
	if (!spa_parity_repair_provider_available(spa))
		return ("no repair provider");
	if (ps == NULL)
		return ("no parity set");

	mutex_enter(&ps->ps_lock);
	applied_txg = ps->ps_applied_txg;
	mutex_exit(&ps->ps_lock);
	if (BP_GET_PHYSICAL_BIRTH(zio->io_bp) > applied_txg)
		return ("block is newer than the applied txg");
	return (NULL);
}

static void
parity_repair_wait_skipped(spa_t *spa, const char *reason)
{
	zfs_dbgmsg("parity repair wait skipped for %s: %s", spa->spa_name,
	    reason);
}

uint64_t
spa_parity_repair_note(spa_t *spa, zio_t *zio)
{
	spa_parity_t *ps = spa->spa_parity;
	parity_repair_t *pr;
	const blkptr_t *bp = zio->io_bp;
	hrtime_t now;
	uint_t cksum;
	uint64_t wait_request = 0;
	boolean_t wait = B_FALSE;
	const char *skip_reason;

	if (ps == NULL || (pr = ps->ps_repair) == NULL)
		return (0);
	if (ps->ps_state == PARITY_OFF || ps->ps_readonly)
		return (0);
	if (zio->io_type != ZIO_TYPE_READ || bp == NULL)
		return (0);
	if (zio->io_error != ECKSUM && zio->io_error != EIO)
		return (0);
	if (zio->io_child_type != ZIO_CHILD_LOGICAL &&
	    zio->io_child_type != ZIO_CHILD_GANG)
		return (0);
	if (zio->io_child_type == ZIO_CHILD_LOGICAL && BP_IS_GANG(bp))
		return (0);
	if ((zio->io_flags & ZIO_FLAG_SPECULATIVE) ||
	    (zio->io_post & ZIO_POST_DIO_CHKSUM_ERR))
		return (0);
	if (BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp) || BP_IS_REDACTED(bp) ||
	    BP_GET_TYPE(bp) == DMU_OT_INTENT_LOG)
		return (0);
	/*
	 * An error that did not come from a device makes no repair:
	 * zio_decrypt() turns an authentication failure into EIO, and rebuilt
	 * bytes that match the checksum would be written over the same bytes.
	 * For an encrypted block, only a device EIO or ECKSUM counts.
	 */
	if (BP_USES_CRYPT(bp) && zio->io_error != ECKSUM &&
	    !(zio->io_error == EIO &&
	    (zio->io_child_error[ZIO_CHILD_VDEV] == EIO ||
	    zio->io_child_error[ZIO_CHILD_GANG] == EIO)))
		return (0);

	/* A block with no checksum cannot be checked, so not repaired. */
	cksum = BP_IS_GANG(bp) ? ZIO_CHECKSUM_GANG_HEADER : BP_GET_CHECKSUM(bp);
	if (parity_cksum_unchecked(cksum) || cksum >= ZIO_CHECKSUM_FUNCTIONS ||
	    zio_checksum_table[cksum].ci_func[0] == NULL)
		return (0);

	/* Do not hold sync or scan work on a daemon. Post the event. */
	skip_reason = parity_repair_wait_skip_reason(spa, zio);
	if (skip_reason == NULL)
		wait = B_TRUE;
	else
		parity_repair_wait_skipped(spa, skip_reason);

	now = gethrtime();
	mutex_enter(&pr->pr_lock);
	parity_req_expire(pr, now);

	/*
	 * A waiting read holds a read-interrupt thread of the pool.  Limit the
	 * waiters, so that reads of good blocks keep
	 * their threads.  A read over the limit fails now; the event still
	 * goes out.
	 */
	if (wait) {
		uint_t waiters = 0;

		for (parity_req_t *w = avl_first(&pr->pr_by_id); w != NULL;
		    w = AVL_NEXT(&pr->pr_by_id, w)) {
			if (w->rq_sync_waiting)
				waiters++;
		}
		if (waiters >= zfs_parity_repair_max_waiters) {
			wait = B_FALSE;
			parity_repair_wait_skipped(spa,
			    "too many waiting reads");
		}
	}

	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		const dva_t *dva = &bp->blk_dva[d];
		uint64_t vdev = DVA_GET_VDEV(dva);
		uint64_t offset = DVA_GET_OFFSET(dva);
		parity_req_t *rq;
		avl_index_t where;

		/*
		 * No config lock here, so no vdev lookup: the post task drops a
		 * request of a vdev that is not the data disk.
		 */
		rq = parity_req_find_dva(pr, vdev, offset);
		if (rq != NULL) {
			/* Pending already; the answer may have been lost. */
			if (wait && wait_request == 0 && !rq->rq_busy &&
			    !rq->rq_sync_waiting && !rq->rq_sync_answered) {
				rq->rq_sync_waiting = B_TRUE;
				wait_request = rq->rq_id;
				/*
				 * The event changes (a read waits now): post
				 * it even if it went out a moment ago, e.g.
				 * by the provider's registration (sync_wait
				 * false), or the provider never learns that
				 * a read waits.  The rate limit still holds.
				 */
				rq->rq_posted = B_FALSE;
			}
			parity_req_post(pr, rq, now);
			continue;
		}

		/* A full table drops its oldest request. */
		if (pr->pr_count >= MAX(zfs_parity_repair_max_requests, 1U)) {
			parity_req_t *old = avl_first(&pr->pr_by_id);

			while (old != NULL &&
			    (old->rq_busy || old->rq_sync_waiting))
				old = AVL_NEXT(&pr->pr_by_id, old);
			if (old == NULL)
				break;
			parity_req_remove(pr, old);
		}

		rq = kmem_zalloc(sizeof (*rq), KM_NOSLEEP);
		if (rq == NULL)
			break;
		rq->rq_id = pr->pr_next_id++;
		rq->rq_vdev = vdev;
		rq->rq_offset = offset;
		rq->rq_asize = DVA_GET_GANG(dva) ? 0 : DVA_GET_ASIZE(dva);
		rq->rq_psize = BP_IS_GANG(bp) ? SPA_OLD_GANGBLOCKSIZE :
		    BP_GET_PSIZE(bp);
		rq->rq_birth = BP_GET_PHYSICAL_BIRTH(bp);
		rq->rq_cksum = cksum;
		rq->rq_reason = zio->io_error;
		rq->rq_created = now;
		rq->rq_bp = *bp;
		rq->rq_zb = zio->io_bookmark;
		if (wait && wait_request == 0) {
			rq->rq_sync_waiting = B_TRUE;
			wait_request = rq->rq_id;
		}
		VERIFY3P(avl_find(&pr->pr_by_dva, rq, &where), ==, NULL);
		avl_insert(&pr->pr_by_dva, rq, where);
		avl_add(&pr->pr_by_id, rq);
		pr->pr_count++;
		atomic_inc_64(&ps->ps_stat_repair_requests);
		parity_req_post(pr, rq, now);
	}
	mutex_exit(&pr->pr_lock);
	return (wait_request);
}

/*
 * The block is good again: take it out of the error log, so that
 * "zpool status -v" does not keep naming a file that the parity set healed:
 * a scrub read posts an event and does not wait; the daemon repairs it a
 * moment later.  Both the pending and the on-disk logs.
 */
static void
parity_heal_errlog(spa_t *spa, const parity_req_t *rq)
{
	zbookmark_phys_t zb = rq->rq_zb;

	spa_remove_pending_error(spa, &zb);
	spa_remove_error(spa, &zb, rq->rq_birth);
}

/* A mirror side can be a replacing or spare vdev with two disk leaves. */
#define	PARITY_REPAIR_MAX_LEAVES	(2 * SPA_PARITY_MAX_SIDES)

static uint_t
parity_repair_leaves(vdev_t *vd, vdev_t **leaves, uint_t max)
{
	uint_t n = 0;

	if (vd == NULL || max == 0)
		return (0);
	if (vd->vdev_ops->vdev_op_leaf) {
		leaves[0] = vd;
		return (1);
	}
	if (vd->vdev_ops != &vdev_mirror_ops &&
	    vd->vdev_ops != &vdev_replacing_ops &&
	    vd->vdev_ops != &vdev_spare_ops)
		return (0);

	for (uint64_t c = 0; c < vd->vdev_children && n < max; c++)
		n += parity_repair_leaves(vd->vdev_child[c], &leaves[n],
		    max - n);

	return (n);
}

/*
 * Wait for one answer without holding a pool or parity lock.  The reply is
 * checked again here before it enters the read pipeline.  Wait for the
 * physical self-heal write before the read returns.
 */
boolean_t
spa_parity_repair_wait(spa_t *spa, zio_t *zio, uint64_t request)
{
	spa_parity_t *ps = spa->spa_parity;
	parity_repair_t *pr;
	parity_req_t search, *rq;
	hrtime_t deadline;
	uint_t wait_ms = zfs_parity_repair_wait_ms;
	boolean_t repaired = B_FALSE;
	const char *skip_reason;

	if (ps == NULL || (pr = ps->ps_repair) == NULL)
		return (B_FALSE);

	search.rq_id = request;
	skip_reason = parity_repair_wait_skip_reason(spa, zio);
	if (skip_reason != NULL) {
		parity_repair_wait_skipped(spa, skip_reason);
		mutex_enter(&pr->pr_lock);
		rq = avl_find(&pr->pr_by_id, &search, NULL);
		if (rq != NULL && rq->rq_sync_waiting) {
			rq->rq_sync_waiting = B_FALSE;
			if (rq->rq_invalid && !rq->rq_busy)
				parity_req_remove(pr, rq);
			cv_broadcast(&pr->pr_cv);
		}
		mutex_exit(&pr->pr_lock);
		return (B_FALSE);
	}
	deadline = gethrtime() + MSEC2NSEC((hrtime_t)wait_ms);
	mutex_enter(&pr->pr_lock);
	rq = avl_find(&pr->pr_by_id, &search, NULL);
	if (rq == NULL || !rq->rq_sync_waiting) {
		mutex_exit(&pr->pr_lock);
		return (B_FALSE);
	}
	pr->pr_tasks++;

	for (;;) {
		parity_req_t snap;
		uint8_t *data;
		uint_t len;
		int error;
		abd_t *write_abd = NULL;
		vdev_t *tvd, *leaves[PARITY_REPAIR_MAX_LEAVES];
		uint_t nleaves;
		uint64_t write_offset;
		boolean_t write_succeeded = B_FALSE;

		while (!rq->rq_sync_answered && !rq->rq_invalid) {
			if (cv_timedwait_hires(&pr->pr_cv, &pr->pr_lock,
			    deadline, NANOSEC, CALLOUT_FLAG_ABSOLUTE) == -1)
				break;
		}
		if (rq->rq_sync_answered && rq->rq_sync_declined) {
			/* The service cannot rebuild it: fail the read now. */
			rq->rq_sync_answered = B_FALSE;
			rq->rq_sync_declined = B_FALSE;
			rq->rq_sync_waiting = B_FALSE;
			if (!rq->rq_busy)
				parity_req_remove(pr, rq);
			pr->pr_tasks--;
			cv_broadcast(&pr->pr_cv);
			mutex_exit(&pr->pr_lock);
			return (B_FALSE);
		}
		if (!rq->rq_sync_answered || rq->rq_invalid) {
			rq->rq_sync_waiting = B_FALSE;
			if (rq->rq_invalid && !rq->rq_busy)
				parity_req_remove(pr, rq);
			pr->pr_tasks--;
			cv_broadcast(&pr->pr_cv);
			mutex_exit(&pr->pr_lock);
			return (B_FALSE);
		}

		rq->rq_busy = B_TRUE;
		snap = *rq;
		data = rq->rq_sync_data;
		len = rq->rq_sync_len;
		mutex_exit(&pr->pr_lock);

		spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
		error = spa_parity_bp_verify(spa, &snap.rq_bp, data, len);
		spa_config_exit(spa, SCL_STATE, FTAG);

		mutex_enter(&pr->pr_lock);
		if (error != 0 || rq->rq_invalid) {
			parity_req_idle(pr, rq);
			if (rq->rq_sync_data != NULL) {
				kmem_free(rq->rq_sync_data, rq->rq_sync_len);
				rq->rq_sync_data = NULL;
				rq->rq_sync_len = 0;
			}
			rq->rq_sync_answered = B_FALSE;
			cv_broadcast(&pr->pr_cv);
			if (rq->rq_invalid) {
				rq->rq_sync_waiting = B_FALSE;
				parity_req_remove(pr, rq);
				pr->pr_tasks--;
				mutex_exit(&pr->pr_lock);
				return (B_FALSE);
			}
			continue;
		}

		if (snap.rq_psize > zio->io_size) {
			parity_req_idle(pr, rq);
			rq->rq_sync_waiting = B_FALSE;
			rq->rq_sync_answered = B_FALSE;
			if (rq->rq_sync_data != NULL) {
				kmem_free(rq->rq_sync_data, rq->rq_sync_len);
				rq->rq_sync_data = NULL;
				rq->rq_sync_len = 0;
			}
			cv_broadcast(&pr->pr_cv);
			pr->pr_tasks--;
			mutex_exit(&pr->pr_lock);
			return (B_FALSE);
		}
		mutex_exit(&pr->pr_lock);
		if (zio->io_abd == NULL) {
			mutex_enter(&pr->pr_lock);
			parity_req_idle(pr, rq);
			rq->rq_sync_waiting = B_FALSE;
			rq->rq_sync_answered = B_FALSE;
			if (rq->rq_sync_data != NULL) {
				kmem_free(rq->rq_sync_data, rq->rq_sync_len);
				rq->rq_sync_data = NULL;
				rq->rq_sync_len = 0;
			}
			pr->pr_tasks--;
			cv_broadcast(&pr->pr_cv);
			mutex_exit(&pr->pr_lock);
			return (B_FALSE);
		}
		abd_copy_from_buf_off(zio->io_abd, data, 0, snap.rq_psize);

		/* Keep a copy until the self-heal zio ends. */
		write_abd = abd_alloc_for_io(len, B_FALSE);
		if (write_abd != NULL)
			abd_copy_from_buf(write_abd, data, len);
		if (write_abd != NULL) {
			spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
			tvd = vdev_lookup_top(spa, snap.rq_vdev);
			nleaves = parity_repair_leaves(tvd, leaves,
			    PARITY_REPAIR_MAX_LEAVES);
			if (snap.rq_offset <= UINT64_MAX -
			    VDEV_LABEL_START_SIZE) {
				write_offset = snap.rq_offset +
				    VDEV_LABEL_START_SIZE;
			} else {
				write_offset = UINT64_MAX;
			}
			if (spa_writeable(spa) && write_offset != UINT64_MAX) {
				boolean_t delayed = B_FALSE;

				for (uint_t i = 0; i < nleaves; i++) {
					vdev_t *leaf = leaves[i];

					if (!vdev_readable(leaf) ||
					    !vdev_writeable(leaf) ||
					    write_offset > leaf->vdev_psize ||
					    len > leaf->vdev_psize -
					    write_offset)
						continue;
					if (!delayed) {
						parity_test_write_delay();
						delayed = B_TRUE;
					}
					mutex_enter(&pr->pr_lock);
					if (rq->rq_invalid) {
						mutex_exit(&pr->pr_lock);
						break;
					}
					mutex_exit(&pr->pr_lock);

					zio_t *write_zio = zio_write_phys(NULL,
					    leaf, write_offset, len, write_abd,
					    ZIO_CHECKSUM_OFF, NULL, NULL,
					    ZIO_PRIORITY_ASYNC_WRITE,
					    ZIO_FLAG_IO_REPAIR |
					    ZIO_FLAG_SELF_HEAL |
					    ZIO_FLAG_CANFAIL, B_FALSE);
					if (zio_wait(write_zio) == 0)
						write_succeeded = B_TRUE;
				}
			}
			spa_config_exit(spa, SCL_STATE, FTAG);
		}
		if (write_abd != NULL)
			abd_free(write_abd);

		mutex_enter(&pr->pr_lock);
		if (write_succeeded) {
			rq->rq_sync_repaired = B_TRUE;
			mutex_exit(&pr->pr_lock);
			parity_heal_errlog(spa, &snap);
			mutex_enter(&pr->pr_lock);
		}
		rq->rq_sync_waiting = B_FALSE;
		parity_req_idle(pr, rq);
		if (!rq->rq_sync_repaired && rq->rq_sync_data != NULL) {
			kmem_free(rq->rq_sync_data, rq->rq_sync_len);
			rq->rq_sync_data = NULL;
			rq->rq_sync_len = 0;
			rq->rq_sync_answered = B_FALSE;
		}
		if ((rq->rq_sync_repaired || rq->rq_invalid) && !rq->rq_busy)
			parity_req_remove(pr, rq);
		pr->pr_tasks--;
		cv_broadcast(&pr->pr_cv);
		mutex_exit(&pr->pr_lock);
		repaired = B_TRUE;
		break;
	}
	return (repaired);
}

/*
 * A provider registered for this pool: post the event of every pending
 * request again, so that a daemon that starts late (or lost a zevent) finds
 * the requests that wait.  The event rate limit holds.
 */
void
spa_parity_repair_repost(spa_t *spa)
{
	spa_parity_t *ps = spa->spa_parity;
	parity_repair_t *pr;
	hrtime_t now = gethrtime();

	if (ps == NULL || (pr = ps->ps_repair) == NULL)
		return;
	mutex_enter(&pr->pr_lock);
	parity_req_expire(pr, now);
	for (parity_req_t *rq = avl_first(&pr->pr_by_id); rq != NULL;
	    rq = AVL_NEXT(&pr->pr_by_id, rq)) {
		if (rq->rq_busy || rq->rq_invalid)
			continue;
		rq->rq_posted = B_FALSE;
		parity_req_post(pr, rq, now);
	}
	mutex_exit(&pr->pr_lock);
}

/*
 * Store bytes for a failed read.  The supplier must own the provider role of
 * this pool.  The waiting read verifies the bytes again before use.  With
 * decline set (no data), the service says it cannot rebuild the block: the
 * read stops its wait at once and fails.
 */
int
spa_parity_repair_supply(spa_t *spa, uint64_t request, const uint8_t *data,
    uint_t len, boolean_t decline, void *provider)
{
	spa_parity_t *ps = spa->spa_parity;
	parity_repair_t *pr;
	parity_req_t search, *rq;
	blkptr_t bp;
	uint64_t asize;
	uint8_t *copy;
	int error;

	if (!spa_parity_repair_provider_authorized(provider, spa_guid(spa)))
		return (SET_ERROR(EPERM));
	if (ps == NULL || (pr = ps->ps_repair) == NULL)
		return (SET_ERROR(ENOTSUP));
	if (decline ? (data != NULL || len != 0) :
	    (data == NULL || len == 0 || len > SPA_MAXBLOCKSIZE))
		return (SET_ERROR(EINVAL));

	search.rq_id = request;
	mutex_enter(&pr->pr_lock);
	rq = avl_find(&pr->pr_by_id, &search, NULL);
	if (rq == NULL || !rq->rq_sync_waiting || rq->rq_invalid) {
		mutex_exit(&pr->pr_lock);
		return (SET_ERROR(ENOENT));
	}
	if (rq->rq_busy || rq->rq_sync_answered) {
		mutex_exit(&pr->pr_lock);
		return (SET_ERROR(EBUSY));
	}
	if (decline) {
		rq->rq_sync_declined = B_TRUE;
		rq->rq_sync_answered = B_TRUE;
		atomic_inc_64(&ps->ps_stat_repair_declined);
		cv_broadcast(&pr->pr_cv);
		mutex_exit(&pr->pr_lock);
		return (0);
	}
	asize = rq->rq_asize;
	bp = rq->rq_bp;
	if (len != asize) {
		mutex_exit(&pr->pr_lock);
		return (SET_ERROR(EINVAL));
	}
	rq->rq_busy = B_TRUE;
	mutex_exit(&pr->pr_lock);

	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	error = spa_parity_bp_verify(spa, &bp, data, len);
	spa_config_exit(spa, SCL_STATE, FTAG);
	if (error != 0) {
		atomic_inc_64(&ps->ps_stat_repair_rejected);
		mutex_enter(&pr->pr_lock);
		parity_req_idle(pr, rq);
		if (rq->rq_invalid && !rq->rq_sync_waiting)
			parity_req_remove(pr, rq);
		cv_broadcast(&pr->pr_cv);
		mutex_exit(&pr->pr_lock);
		return (error);
	}

	copy = kmem_alloc(len, KM_SLEEP);
	memcpy(copy, data, len);
	mutex_enter(&pr->pr_lock);
	if (rq->rq_invalid || !rq->rq_sync_waiting) {
		parity_req_idle(pr, rq);
		if (rq->rq_invalid && !rq->rq_sync_waiting)
			parity_req_remove(pr, rq);
		cv_broadcast(&pr->pr_cv);
		mutex_exit(&pr->pr_lock);
		kmem_free(copy, len);
		return (SET_ERROR(ENOENT));
	}
	rq->rq_sync_data = copy;
	rq->rq_sync_len = len;
	rq->rq_sync_answered = B_TRUE;
	atomic_inc_64(&ps->ps_stat_repair_supplied);
	parity_req_idle(pr, rq);
	cv_broadcast(&pr->pr_cv);
	mutex_exit(&pr->pr_lock);
	return (0);
}

/*
 * A block is freed (metaslab_free(), before the free or the hold): its
 * requests are void.  The caller is the syncing thread or a zio free taskq,
 * with SCL_FREE held, so it never waits.  A request that a repair works on
 * is only marked: the repair has tested the mark before its write, and it
 * removes the request when it ends.
 * The mark alone does not stop a write that passed its last test:
 * spa_parity_repair_barrier() makes the txg sync wait for
 * such a write, so the freed space is not handed out again before it ends.
 */
void
spa_parity_repair_invalidate(spa_parity_t *ps, const blkptr_t *bp)
{
	parity_repair_t *pr = ps->ps_repair;

	if (pr == NULL || atomic_load_64(&pr->pr_count) == 0)
		return;
	if (BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp))
		return;

	mutex_enter(&pr->pr_lock);
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		uint64_t vdev = DVA_GET_VDEV(&bp->blk_dva[d]);
		uint64_t offset = DVA_GET_OFFSET(&bp->blk_dva[d]);
		parity_req_t *rq;

		rq = parity_req_find_dva(pr, vdev, offset);
		if (rq == NULL)
			continue;
		if (rq->rq_busy || rq->rq_sync_waiting) {
			rq->rq_invalid = B_TRUE;
			/*
			 * A write may already be in flight: the sync waits
			 * for it before this space can be allocated again.
			 */
			if (rq->rq_busy && !rq->rq_inval_busy) {
				rq->rq_inval_busy = B_TRUE;
				pr->pr_inval_busy++;
			}
			cv_broadcast(&pr->pr_cv);
		} else {
			parity_req_remove(pr, rq);
		}
	}
	mutex_exit(&pr->pr_lock);
}

/* Test hook used by parity_repair_free_race_pos. */
static void
parity_test_write_delay(void)
{
	if (zfs_parity_test_inject & 64)
		delay(SEC_TO_TICK(3));
}

/* Wait for invalidated repair writes before the txg can reuse their space. */
void
spa_parity_repair_barrier(spa_t *spa)
{
	spa_parity_t *ps = spa->spa_parity;
	parity_repair_t *pr;

	if (ps == NULL || (pr = ps->ps_repair) == NULL ||
	    atomic_load_64(&pr->pr_inval_busy) == 0)
		return;
	zfs_dbgmsg("parity repair barrier: %s waits for a voided write",
	    spa_name(spa));
	mutex_enter(&pr->pr_lock);
	while (pr->pr_inval_busy > 0)
		cv_wait(&pr->pr_cv, &pr->pr_lock);
	mutex_exit(&pr->pr_lock);
}

uint64_t
spa_parity_repair_npending(spa_t *spa)
{
	spa_parity_t *ps = spa->spa_parity;

	if (ps == NULL || ps->ps_repair == NULL)
		return (0);
	return (atomic_load_64(&ps->ps_repair->pr_count));
}

/*
 * Handle an asynchronous repair answer.  The caller holds SCL_STATE (see the
 * note on locks at the top).
 *
 * The request is marked busy while the bytes are checked and written, so a
 * second call gets EBUSY and a free waits for the write.  The check of
 * rq_invalid and the start of the write are one step under pr_lock.
 */
static int
parity_repair_locked(spa_t *spa, parity_repair_t *pr, uint64_t request,
    const void *buf, size_t len, boolean_t *repaired)
{
	parity_req_t *rq, search, snap;
	abd_t *abd = NULL;
	vdev_t *tvd, *leaves[PARITY_REPAIR_MAX_LEAVES];
	uint_t nleaves;
	uint64_t offset;
	int error = 0;

	mutex_enter(&pr->pr_lock);
	search.rq_id = request;
	rq = avl_find(&pr->pr_by_id, &search, NULL);
	if (rq != NULL && !rq->rq_busy &&
	    (rq->rq_invalid || parity_req_expired(rq, gethrtime()))) {
		parity_req_remove(pr, rq);
		rq = NULL;
	}
	if (rq == NULL) {
		mutex_exit(&pr->pr_lock);
		return (SET_ERROR(ENOENT));
	}
	if (rq->rq_busy || rq->rq_sync_waiting) {
		mutex_exit(&pr->pr_lock);
		return (SET_ERROR(EBUSY));
	}
	rq->rq_busy = B_TRUE;
	snap = *rq;
	mutex_exit(&pr->pr_lock);

	if (!spa_writeable(spa)) {
		error = SET_ERROR(EROFS);
		goto out;
	}
	if (len != snap.rq_asize || buf == NULL) {
		error = SET_ERROR(EINVAL);
		goto out;
	}

	abd = abd_alloc_for_io(len, B_FALSE);
	abd_copy_from_buf(abd, buf, len);
	error = parity_cksum_check(spa, &snap.rq_bp, abd, len);
	if (error == ECKSUM) {
		/* Wrong bytes: write nothing, keep the request. */
		atomic_inc_64(&spa->spa_parity->ps_stat_repair_rejected);
		error = 0;
		goto out;
	}
	if (error != 0)
		goto out;

	/*
	 * The checksum and the free race here: from now on a free waits for
	 * the write.  The checksum code may have changed the embedded checksum
	 * of a gang header and put it back; the buffer is whole again, so it
	 * is what the daemon sent.
	 */
	mutex_enter(&pr->pr_lock);
	if (rq->rq_invalid) {
		error = SET_ERROR(ENOENT);
		mutex_exit(&pr->pr_lock);
		goto out;
	}
	mutex_exit(&pr->pr_lock);

	tvd = vdev_lookup_top(spa, snap.rq_vdev);
	nleaves = parity_repair_leaves(tvd, leaves,
	    PARITY_REPAIR_MAX_LEAVES);
	if (nleaves == 0) {
		error = SET_ERROR(EIO);
	} else if (snap.rq_offset > UINT64_MAX - VDEV_LABEL_START_SIZE) {
		error = SET_ERROR(EINVAL);
	} else {
		boolean_t attempted = B_FALSE;
		boolean_t delayed = B_FALSE;
		boolean_t write_succeeded = B_FALSE;

		offset = snap.rq_offset + VDEV_LABEL_START_SIZE;
		error = SET_ERROR(EIO);
		for (uint_t i = 0; i < nleaves; i++) {
			vdev_t *leaf = leaves[i];
			int write_error;

			if (!vdev_readable(leaf) || !vdev_writeable(leaf) ||
			    offset > leaf->vdev_psize ||
			    len > leaf->vdev_psize - offset)
				continue;
			attempted = B_TRUE;
			if (!delayed) {
				parity_test_write_delay();
				delayed = B_TRUE;
			}
			mutex_enter(&pr->pr_lock);
			if (rq->rq_invalid) {
				error = SET_ERROR(ENOENT);
				mutex_exit(&pr->pr_lock);
				break;
			}
			mutex_exit(&pr->pr_lock);

			/* A self-heal write is not a device error. */
			zio_t *zio = zio_write_phys(NULL, leaf, offset, len,
			    abd, ZIO_CHECKSUM_OFF, NULL, NULL,
			    ZIO_PRIORITY_ASYNC_WRITE,
			    ZIO_FLAG_IO_REPAIR | ZIO_FLAG_SELF_HEAL |
			    ZIO_FLAG_CANFAIL, B_FALSE);
			write_error = zio_wait(zio);
			if (write_error == 0)
				write_succeeded = B_TRUE;
			else
				error = write_error;
		}
		if (write_succeeded) {
			*repaired = B_TRUE;
			parity_heal_errlog(spa, &snap);
			error = 0;
		} else if (!attempted) {
			error = SET_ERROR(EIO);
		}
	}

out:
	if (abd != NULL)
		abd_free(abd);
	mutex_enter(&pr->pr_lock);
	parity_req_idle(pr, rq);
	if (*repaired || error == ENOENT || rq->rq_invalid)
		parity_req_remove(pr, rq);
	cv_broadcast(&pr->pr_cv);
	mutex_exit(&pr->pr_lock);
	return (error);
}

int
spa_parity_repair(spa_t *spa, uint64_t request, const void *buf, size_t len,
    boolean_t *repaired)
{
	spa_parity_t *ps = spa->spa_parity;
	int error;

	*repaired = B_FALSE;
	if (ps == NULL || ps->ps_repair == NULL)
		return (SET_ERROR(ENOENT));

	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	error = parity_repair_locked(spa, ps->ps_repair, request, buf, len,
	    repaired);
	spa_config_exit(spa, SCL_STATE, FTAG);
	if (*repaired)
		atomic_inc_64(&ps->ps_stat_repair_written);
	return (error);
}

ZFS_MODULE_PARAM(zfs_parity, zfs_parity_, repair_max_requests, UINT, ZMOD_RW,
	"Repair requests kept for each pool; the oldest is dropped when full");

ZFS_MODULE_PARAM(zfs_parity, zfs_parity_, repair_wait_ms, UINT, ZMOD_RW,
	"Time to wait for a parity repair answer; zero disables read repair");

ZFS_MODULE_PARAM(zfs_parity, zfs_parity_, repair_max_waiters, UINT, ZMOD_RW,
	"Reads of one pool that wait for a parity repair at once");

ZFS_MODULE_PARAM(zfs_parity, zfs_parity_, repair_timeout_ms, UINT, ZMOD_RW,
	"Drop a repair request after this many milliseconds");

ZFS_MODULE_PARAM(zfs_parity, zfs_parity_, repair_events_per_sec, UINT, ZMOD_RW,
	"Post at most this many repair events each second for a pool");

#if defined(_KERNEL)
EXPORT_SYMBOL(spa_parity_repair_note);
EXPORT_SYMBOL(spa_parity_repair_wait);
EXPORT_SYMBOL(spa_parity_repair);
EXPORT_SYMBOL(spa_parity_repair_supply);
EXPORT_SYMBOL(spa_parity_repair_provider_register);
EXPORT_SYMBOL(spa_parity_repair_provider_unregister);
EXPORT_SYMBOL(spa_parity_repair_provider_available);
EXPORT_SYMBOL(spa_parity_repair_provider_authorized);
EXPORT_SYMBOL(spa_parity_repair_npending);
EXPORT_SYMBOL(spa_parity_repair_invalidate);
EXPORT_SYMBOL(spa_parity_repair_init);
EXPORT_SYMBOL(spa_parity_repair_fini);
EXPORT_SYMBOL(spa_parity_bp_verify);
#endif
