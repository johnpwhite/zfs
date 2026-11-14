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
 * async mirror: the copy to the lagging child.
 *
 * This file holds the replay of the txgs: the copy of the blocks of each txg
 * from its block tree, and the commit (the flush and the uberblock write)
 * that ends a run of copied txgs.  It also holds
 * the deferred blocks (a data block that no copy could supply at first,
 * which is tried again later), and the error episode that ends in a failure
 * or a freeze of the lagging child.  The worker thread that calls it is in
 * vdev_async_mirror.c.
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
#include <sys/bptree.h>
#include <sys/dmu_objset.h>
#include <sys/dnode.h>
#include <sys/arc.h>
#include <sys/zio_checksum.h>
#include <sys/zil.h>
#include <sys/zil_impl.h>

/* Flush the write cache of the lagging child. */
static int
async_mirror_flush(vdev_async_mirror_t *am)
{
	spa_t *spa = am->am_spa;

	ASSERT(!MUTEX_HELD(&am->am_lock));

	spa_config_enter(spa, SCL_STATE_ALL, am, RW_READER);
	zio_t *pio = zio_root(spa, NULL, NULL, ZIO_FLAG_CANFAIL);
	zio_flush_leaf(pio, am->am_lagging);
	int err = zio_wait(pio);
	spa_config_exit(spa, SCL_STATE_ALL, am);
	return (err);
}

/* Write one uberblock to the labels of the lagging child. */
static int
async_mirror_write_ub(vdev_async_mirror_t *am, uberblock_t *ub)
{
	spa_t *spa = am->am_spa;
	uint64_t good = 0;
	int flags = ZIO_FLAG_CONFIG_WRITER | ZIO_FLAG_CANFAIL;

	ASSERT(!MUTEX_HELD(&am->am_lock));

	spa_config_enter(spa, SCL_STATE_ALL, am, RW_READER);
	zio_t *pio = zio_root(spa, NULL, NULL, flags);
	vdev_uberblock_write(pio, &good, ub, am->am_lagging, flags);
	(void) zio_wait(pio);
	spa_config_exit(spa, SCL_STATE_ALL, am);

	/*
	 * One good label is what vdev_uberblock_sync_list() asks of a whole
	 * pool: an import reads every label and takes the newest uberblock.
	 */
	return (good >= 1 ? 0 : SET_ERROR(EIO));
}

/*
 * The copy of a txg.
 *
 * The record of a txg lists ranges, with no block boundaries and no
 * checksums.  So the worker walks the tree of the txg from the txg's own
 * uberblock, and visits each block born in that txg.  None of these blocks
 * can change on the leader before the lagging child has passed the txg,
 * because their frees are held.  For each block, the worker gets the bytes
 * in their on-disk form, checked against the block pointer: from the ARC
 * when it holds them, else with a read of the leader through the block
 * pointer.  That read is the normal read path: it checks the checksum,
 * tries each copy of the block, repairs a bad copy from a good one, and
 * logs what it cannot repair.  The worker then writes the checked bytes to
 * each copy of the block on the lagging child.
 *
 * The walk reaches every block that the txg allocated on the mirror and
 * that is still allocated at the end of the txg:
 *
 * - A gang block: the tree points to its gang header, which is checked
 *   against the block pointer and copied, and the header points to the
 *   blocks that hold the data, which are copied as any other block.
 * - A block with no checksum is copied from the leader as it is.
 * - A log that the import claimed in this txg is part of the pool from
 *   this txg on, but the tree reaches it only through the log header of its
 *   dataset.  Its log blocks are copied from the leader as they are: the
 *   last one was allocated but never written, so it does not match its
 *   checksum.  The data blocks of its write records are checked.
 * - A block that the txg freed in a late sync pass is listed in the bpobj
 *   of deferred frees, and the root block of a dataset that it destroyed
 *   in the background in the bptree of the pool; see
 *   async_mirror_walk_bpobj() and async_mirror_walk_bptree().
 * - A hole or an embedded block allocates nothing.  A dedup or clone
 *   reference to an older block has an older physical birth.
 *
 * A block that the txg allocated and also freed is in no tree, and needs
 * no copy.  A debug build checks that the walk covers the record; see
 * async_mirror_walk_verify().
 *
 * A read error is first tried again; see async_mirror_thread().  On the
 * last try, a data block that no copy can supply is lost: the ARC does not
 * hold it, the leader returns EIO or a checksum error while the leader can
 * still be read, and the lagging child cannot have it, because it is newer
 * than the child's txg.  (For an older block, the read of the leader
 * already falls back to the lagging child; see vdev_mirror_child_select().)
 * The block is skipped, and the copy goes on.  The block is deferred: it
 * is tried again later, and reported as lost only if it still cannot be
 * read then; see "Deferred blocks".  Until then the lagging child keeps
 * older bytes there, which fail the checksum.  Any other read error that
 * stays after the tries, such as a metadata block, a gang header or a
 * leader that cannot be read, stops the copy of the txg: the lagging child
 * must not get a txg that it cannot read, so it freezes.
 */

/*
 * The I/O of the walk.  The walk only collects the blocks of the txg, and
 * their I/O runs in the background.  The walk sorts the blocks by their
 * offset on the mirror, up to ASYNC_MIRROR_SORT_BLOCKS at a time, and issues
 * them in that order: a read of the leader, whose done callback starts the
 * write of each copy on the lagging child, or a write from the ARC.  The
 * two children have the same offsets, so both see mostly sequential I/O,
 * and the queue of the lagging child can merge the writes.  At most
 * zfs_vdev_async_mirror_copy_limit bytes of blocks are in flight, as
 * vdev_rebuild_range() limits a rebuild, and the reads of some blocks
 * overlap the writes of others.  The copied txgs do not overwrite each
 * other (see async_mirror_commit()), so the order of the writes does not
 * matter.
 *
 * A block from the ARC is checked against its block pointer, because the
 * ARC can hold it in another form than on disk.  It is written from the ARC
 * buffer, with no copy, when it fills the allocated size of its largest
 * copy.  A block read from the leader is checked once, by the read.
 */

/* The most blocks that the walk sorts before it issues them. */
#define	ASYNC_MIRROR_SORT_BLOCKS	16384

typedef struct async_mirror_walk async_mirror_walk_t;

/* One block of the walk, from its read to the end of its writes. */
typedef struct async_mirror_blk {
	async_mirror_walk_t *ab_aw;
	blkptr_t	ab_bp;
	zbookmark_phys_t ab_zb;
	uint64_t	ab_asize;	/* of the largest copy to write */
	uint64_t	ab_raw;		/* read here from the leader, or -1 */
	uint64_t	ab_off[SPA_DVAS_PER_BP]; /* the copies to write */
	uint64_t	ab_size[SPA_DVAS_PER_BP]; /* and their sizes */
	int		ab_ncopies;
	abd_t		*ab_abd;	/* ab_asize: the block, then zeros */
	arc_buf_t	*ab_buf;	/* the ARC buffer of ab_abd, or NULL */
	uint32_t	ab_writes;	/* writes in flight */
} async_mirror_blk_t;

/* A dataset whose root block was born in the txg of the walk. */
typedef struct async_mirror_root {
	list_node_t	ar_node;
	uint64_t	ar_objset;
	blkptr_t	ar_bp;
} async_mirror_root_t;

/*
 * The state of a walk.  The walk thread alone uses the fields up to
 * aw_nsort.  aw_rio is constant.  aw_lock protects the rest, which the done
 * callbacks change.
 */
struct async_mirror_walk {
	vdev_async_mirror_t *aw_am;
	uint64_t	aw_txg;
	zfs_range_tree_t *aw_done;	/* ranges written, or skipped */
	list_t		aw_roots;
	uint64_t	aw_objset;	/* of the log being walked */
	int		aw_zil_error;	/* an error of a log callback */
	boolean_t	aw_last;	/* the last try: skip a lost block */
	async_mirror_blk_t **aw_sort;	/* the blocks to sort and issue */
	uint_t		aw_nsort;
	zio_t		*aw_rio;	/* the parent of each I/O */
	kmutex_t	aw_lock;
	kcondvar_t	aw_cv;		/* a block ended */
	uint64_t	aw_inflight;	/* bytes of the blocks in flight */
	hrtime_t	aw_progress;	/* when the last block ended */
	uint64_t	aw_copied;	/* bytes written */
	uint64_t	aw_data_errors;	/* lost data blocks of a log */
	int		aw_write_error;
	int		aw_meta_error;	/* a read error, not a lost block */
	list_t		aw_defer;	/* the skipped blocks */
	uint_t		aw_ndefer;
};

static int async_mirror_walk_add(async_mirror_walk_t *, const blkptr_t *,
    const zbookmark_phys_t *);

/* True if the block is metadata: its loss can hide other blocks. */
static boolean_t
async_mirror_bp_is_metadata(const blkptr_t *bp)
{
	return (BP_GET_LEVEL(bp) > 0 || DMU_OT_IS_METADATA(BP_GET_TYPE(bp)));
}

/* True if the walk has written, or skipped, a part of this range. */
static boolean_t
async_mirror_walk_done(async_mirror_walk_t *aw, uint64_t offset,
    uint64_t size)
{
	uint64_t start, len;

	return (zfs_range_tree_find_in(aw->aw_done, offset, size, &start,
	    &len));
}

/*
 * The allocated size of the largest copy of a block on the mirror, and at
 * least the allocated size of its data.  The copies of one block can differ
 * in size: an allocation of a range gives each copy what it found, and the
 * block uses the smallest one.  See metaslab_alloc_range().
 */
static uint64_t
async_mirror_bp_asize(vdev_t *tvd, const blkptr_t *bp)
{
	uint64_t asize = vdev_psize_to_asize(tvd, BP_GET_PSIZE(bp));

	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		const dva_t *dva = &bp->blk_dva[d];

		if (DVA_GET_VDEV(dva) == tvd->vdev_id && !DVA_GET_GANG(dva))
			asize = MAX(asize, DVA_GET_ASIZE(dva));
	}
	return (asize);
}

/*
 * True if the walk writes this copy of a block: it is on the mirror, it is
 * not larger than the asize bytes read, and no part of it is written yet.
 * The copy gets its own allocated size of those bytes.
 */
static boolean_t
async_mirror_walk_dva_ok(async_mirror_walk_t *aw, const dva_t *dva,
    uint64_t asize)
{
	vdev_t *tvd = aw->aw_am->am_lagging->vdev_top;

	return (DVA_GET_VDEV(dva) == tvd->vdev_id && !DVA_GET_GANG(dva) &&
	    DVA_GET_ASIZE(dva) <= asize &&
	    !async_mirror_walk_done(aw, DVA_GET_OFFSET(dva),
	    DVA_GET_ASIZE(dva)));
}

/*
 * Copy the block from the ARC into abd, if the ARC holds it in its on-disk
 * form and the bytes match the block pointer.  Any other result is a miss:
 * the caller then reads the leader, which decides whether the block is bad.
 */
static boolean_t
async_mirror_arc_copy(spa_t *spa, const blkptr_t *bp,
    const zbookmark_phys_t *zb, abd_t *abd)
{
	arc_flags_t aflags = ARC_FLAG_WAIT | ARC_FLAG_CACHED_ONLY;
	arc_buf_t *buf = NULL;
	uint64_t psize = BP_GET_PSIZE(bp);
	boolean_t ok = B_FALSE;

	if (BP_GET_CHECKSUM(bp) == ZIO_CHECKSUM_OFF)
		return (B_FALSE);
	if (arc_read(NULL, spa, bp, arc_getbuf_func, &buf,
	    ZIO_PRIORITY_ASYNC_READ, ZIO_FLAG_CANFAIL | ZIO_FLAG_RAW |
	    ZIO_FLAG_SPECULATIVE, &aflags, zb) != 0 || buf == NULL)
		return (B_FALSE);

	if (arc_buf_size(buf) == psize) {
		abd_copy_from_buf(abd, buf->b_data, psize);
		ok = (zio_checksum_verify_bp(spa, bp, abd, psize) == 0);
	}
	arc_buf_destroy(buf, &buf);
	return (ok);
}

/*
 * Take the bytes of ab from the ARC, as async_mirror_arc_copy() does.  When
 * the block fills ab_asize, ab_abd shows the ARC buffer, which ab keeps
 * until its writes end.
 */
static boolean_t
async_mirror_arc_take(spa_t *spa, async_mirror_blk_t *ab)
{
	arc_flags_t aflags = ARC_FLAG_WAIT | ARC_FLAG_CACHED_ONLY;
	const blkptr_t *bp = &ab->ab_bp;
	uint64_t psize = BP_GET_PSIZE(bp);
	uint64_t asize = ab->ab_asize;

	if (arc_read(NULL, spa, bp, arc_getbuf_func, &ab->ab_buf,
	    ZIO_PRIORITY_ASYNC_READ, ZIO_FLAG_CANFAIL | ZIO_FLAG_RAW |
	    ZIO_FLAG_SPECULATIVE, &aflags, &ab->ab_zb) == 0 &&
	    ab->ab_buf != NULL && arc_buf_size(ab->ab_buf) == psize) {
		abd_t *abd;

		if (psize == asize) {
			abd = abd_get_from_buf(ab->ab_buf->b_data, psize);
		} else {
			abd = abd_alloc_for_io(asize,
			    async_mirror_bp_is_metadata(bp));
			abd_copy_from_buf(abd, ab->ab_buf->b_data, psize);
			abd_zero_off(abd, psize, asize - psize);
		}
		if (zio_checksum_verify_bp(spa, bp, abd, psize) == 0) {
			ab->ab_abd = abd;
			if (psize != asize) {
				arc_buf_destroy(ab->ab_buf, &ab->ab_buf);
				ab->ab_buf = NULL;
			}
			return (B_TRUE);
		}
		abd_free(abd);
	}
	if (ab->ab_buf != NULL) {
		arc_buf_destroy(ab->ab_buf, &ab->ab_buf);
		ab->ab_buf = NULL;
	}
	return (B_FALSE);
}

/* Free the view of a buffer that a write used. */
static void
async_mirror_abd_free_done(zio_t *zio)
{
	abd_free(zio->io_abd);
}

void
async_mirror_defer_list_create(list_t *list)
{
	list_create(list, sizeof (async_mirror_defer_t),
	    offsetof(async_mirror_defer_t, ad_node));
}

/* Free each deferred block of the list.  The list stays. */
void
async_mirror_defer_list_free(list_t *list)
{
	async_mirror_defer_t *ad;

	while ((ad = list_remove_head(list)) != NULL)
		kmem_free(ad, sizeof (*ad));
}

/*
 * Keep a skipped data block, to try it again later; see "Deferred blocks".
 * The caller holds aw_lock.
 */
static void
async_mirror_walk_defer(async_mirror_walk_t *aw, const blkptr_t *bp,
    const zbookmark_phys_t *zb)
{
	async_mirror_defer_t *ad = kmem_zalloc(sizeof (*ad), KM_SLEEP);

	ASSERT(MUTEX_HELD(&aw->aw_lock));

	ad->ad_bp = *bp;
	ad->ad_zb = *zb;
	list_insert_tail(&aw->aw_defer, ad);
	aw->aw_ndefer++;
}

/* The first error of the I/O of the walk: a write error first, or 0. */
static int
async_mirror_walk_error(async_mirror_walk_t *aw)
{
	mutex_enter(&aw->aw_lock);
	int err = (aw->aw_write_error != 0) ? aw->aw_write_error :
	    aw->aw_meta_error;
	mutex_exit(&aw->aw_lock);
	return (err);
}

static async_mirror_blk_t *
async_mirror_blk_alloc(async_mirror_walk_t *aw, const blkptr_t *bp,
    const zbookmark_phys_t *zb, uint64_t asize)
{
	async_mirror_blk_t *ab = kmem_zalloc(sizeof (*ab), KM_SLEEP);

	ab->ab_aw = aw;
	ab->ab_bp = *bp;
	ab->ab_zb = *zb;
	ab->ab_asize = asize;
	ab->ab_raw = UINT64_MAX;
	return (ab);
}

static void
async_mirror_blk_free(async_mirror_blk_t *ab)
{
	if (ab->ab_abd != NULL)
		abd_free(ab->ab_abd);
	if (ab->ab_buf != NULL)
		arc_buf_destroy(ab->ab_buf, &ab->ab_buf);
	kmem_free(ab, sizeof (*ab));
}

/* The I/O of ab has ended: free it, and let the walk issue more. */
static void
async_mirror_blk_end(async_mirror_blk_t *ab)
{
	async_mirror_walk_t *aw = ab->ab_aw;
	uint64_t asize = ab->ab_asize;

	async_mirror_blk_free(ab);
	spa_config_exit(aw->aw_am->am_spa, SCL_STATE_ALL, aw);

	mutex_enter(&aw->aw_lock);
	ASSERT3U(aw->aw_inflight, >=, asize);
	aw->aw_inflight -= asize;
	aw->aw_progress = gethrtime();
	cv_broadcast(&aw->aw_cv);
	mutex_exit(&aw->aw_lock);
}

static void
async_mirror_blk_write_done(zio_t *zio)
{
	async_mirror_blk_t *ab = zio->io_private;
	async_mirror_walk_t *aw = ab->ab_aw;

	abd_free(zio->io_abd);
	mutex_enter(&aw->aw_lock);
	if (zio->io_error == 0)
		aw->aw_copied += zio->io_size;
	else if (aw->aw_write_error == 0)
		aw->aw_write_error = zio->io_error;
	mutex_exit(&aw->aw_lock);

	if (atomic_dec_32_nv(&ab->ab_writes) == 0)
		async_mirror_blk_end(ab);
}

/*
 * Write the bytes of ab to each of its copies on the lagging child, each
 * with its own allocated size: a prefix of the bytes.  The last write to
 * end frees ab, so ab is not used after the last write is issued.
 */
static void
async_mirror_blk_write(async_mirror_blk_t *ab)
{
	async_mirror_walk_t *aw = ab->ab_aw;
	vdev_t *lag = aw->aw_am->am_lagging;
	int n = ab->ab_ncopies;

	ASSERT3S(n, >, 0);
	ab->ab_writes = n;
	for (int c = 0; c < n; c++) {
		zio_nowait(zio_write_phys(aw->aw_rio, lag,
		    ab->ab_off[c] + VDEV_LABEL_START_SIZE, ab->ab_size[c],
		    abd_get_offset_size(ab->ab_abd, 0, ab->ab_size[c]),
		    ZIO_CHECKSUM_OFF, async_mirror_blk_write_done,
		    ab, ZIO_PRIORITY_ASYNC_WRITE, ZIO_FLAG_CANFAIL, B_FALSE));
	}
}

/*
 * The read of ab has ended.  Write the block, or note why it could not be
 * read.  On the last try a data block that no copy can supply is lost: it
 * is skipped, and deferred; see "Deferred blocks".  Any other read error
 * stops the copy of the txg.
 */
static void
async_mirror_blk_read_done(zio_t *zio)
{
	async_mirror_blk_t *ab = zio->io_private;
	async_mirror_walk_t *aw = ab->ab_aw;
	vdev_async_mirror_t *am = aw->aw_am;
	const blkptr_t *bp = &ab->ab_bp;
	const zbookmark_phys_t *zb = &ab->ab_zb;
	int err = zio->io_error;

	if (zio->io_abd != ab->ab_abd)
		abd_free(zio->io_abd);
	if (err == 0) {
		async_mirror_blk_write(ab);
		return;
	}

	boolean_t meta = async_mirror_bp_is_metadata(bp);
	boolean_t lost = !meta && aw->aw_last &&
	    (err == EIO || err == ECKSUM) && vdev_readable(am->am_leader);

	zfs_dbgmsg("async mirror: txg %llu: %s block "
	    "<%llu, %llu, %lld, %llx> cannot be read, error %d%s",
	    (u_longlong_t)aw->aw_txg, meta ? "metadata" : "data",
	    (u_longlong_t)zb->zb_objset, (u_longlong_t)zb->zb_object,
	    (longlong_t)zb->zb_level, (u_longlong_t)zb->zb_blkid,
	    err, lost ? "; skipped" : "");

	mutex_enter(&aw->aw_lock);
	if (!lost) {
		if (aw->aw_meta_error == 0)
			aw->aw_meta_error = err;
	} else if (zb->zb_level < 0) {
		aw->aw_data_errors++;
	} else {
		async_mirror_walk_defer(aw, bp, zb);
	}
	mutex_exit(&aw->aw_lock);
	async_mirror_blk_end(ab);
}

/*
 * Wait until size more bytes can be in flight, and count them.  When no
 * block ends for the time of the deadman, report the I/O of the walk, as
 * zio_wait() does.
 */
static void
async_mirror_walk_reserve(async_mirror_walk_t *aw, uint64_t size)
{
	spa_t *spa = aw->aw_am->am_spa;
	clock_t timeout = MSEC_TO_TICK(zfs_deadman_ziotime_ms);

	mutex_enter(&aw->aw_lock);
	while (aw->aw_inflight > 0 &&
	    aw->aw_inflight + size > zfs_vdev_async_mirror_copy_limit) {
		if (cv_timedwait_io(&aw->aw_cv, &aw->aw_lock,
		    ddi_get_lbolt() + timeout) == -1 && zfs_deadman_enabled &&
		    gethrtime() - aw->aw_progress > spa_deadman_ziotime(spa)) {
			mutex_exit(&aw->aw_lock);
			timeout = MSEC_TO_TICK(zfs_deadman_checktime_ms);
			zio_deadman(aw->aw_rio, FTAG);
			mutex_enter(&aw->aw_lock);
		}
	}
	aw->aw_inflight += size;
	mutex_exit(&aw->aw_lock);
}

/*
 * Start the I/O of ab: write it at once if the walk read it (a gang header)
 * or the ARC holds it, else read it from the leader: as it is for a block
 * with no checksum and for a log block, else through its block pointer.
 */
static void
async_mirror_blk_issue(async_mirror_blk_t *ab)
{
	async_mirror_walk_t *aw = ab->ab_aw;
	vdev_async_mirror_t *am = aw->aw_am;
	spa_t *spa = am->am_spa;
	const blkptr_t *bp = &ab->ab_bp;
	uint64_t psize = BP_GET_PSIZE(bp);

	async_mirror_walk_reserve(aw, ab->ab_asize);
	/* async_mirror_blk_end() exits. */
	spa_config_enter(spa, SCL_STATE_ALL, aw, RW_READER);

	if (ab->ab_abd != NULL ||
	    (ab->ab_raw == UINT64_MAX && async_mirror_arc_take(spa, ab))) {
		async_mirror_blk_write(ab);
		return;
	}

	ab->ab_abd = abd_alloc_for_io(ab->ab_asize,
	    async_mirror_bp_is_metadata(bp));
	if (ab->ab_raw != UINT64_MAX) {
		zio_nowait(zio_read_phys(aw->aw_rio, am->am_leader,
		    ab->ab_raw + VDEV_LABEL_START_SIZE, ab->ab_asize,
		    ab->ab_abd, ZIO_CHECKSUM_OFF, async_mirror_blk_read_done,
		    ab, ZIO_PRIORITY_ASYNC_READ, ZIO_FLAG_CANFAIL, B_FALSE));
		return;
	}
	if (ab->ab_asize > psize)
		abd_zero_off(ab->ab_abd, psize, ab->ab_asize - psize);
	zio_nowait(zio_read(aw->aw_rio, spa, bp,
	    abd_get_offset_size(ab->ab_abd, 0, psize), psize,
	    async_mirror_blk_read_done, ab, ZIO_PRIORITY_ASYNC_READ,
	    ZIO_FLAG_CANFAIL | ZIO_FLAG_RAW, &ab->ab_zb));
}

static int
async_mirror_blk_compare(const void *x1, const void *x2)
{
	const async_mirror_blk_t *a = *(async_mirror_blk_t * const *)x1;
	const async_mirror_blk_t *b = *(async_mirror_blk_t * const *)x2;

	return (TREE_CMP(a->ab_off[0], b->ab_off[0]));
}

/*
 * Issue the blocks that the walk has collected, by offset.  After an error
 * of the walk (stop) or of its I/O, free them instead: the copy of the txg
 * stops.
 */
static void
async_mirror_walk_issue(async_mirror_walk_t *aw, boolean_t stop)
{
	stop = stop || async_mirror_walk_error(aw) != 0;

	if (!stop) {
		qsort(aw->aw_sort, aw->aw_nsort, sizeof (async_mirror_blk_t *),
		    async_mirror_blk_compare);
	}
	for (uint_t i = 0; i < aw->aw_nsort; i++) {
		if (stop)
			async_mirror_blk_free(aw->aw_sort[i]);
		else
			async_mirror_blk_issue(aw->aw_sort[i]);
	}
	aw->aw_nsort = 0;
}

/* Add ab to the blocks that the walk sorts and issues. */
static void
async_mirror_walk_queue(async_mirror_walk_t *aw, async_mirror_blk_t *ab)
{
	aw->aw_sort[aw->aw_nsort++] = ab;
	if (aw->aw_nsort == ASYNC_MIRROR_SORT_BLOCKS)
		async_mirror_walk_issue(aw, B_FALSE);
}

/*
 * A gang block.  Read its gang header from the leader, check it against bp,
 * and add each copy of the header and each block that the header points to.
 * A dynamic gang header can also be an old one; see zio_checksum_error().
 */
static int
async_mirror_walk_gang(async_mirror_walk_t *aw, const blkptr_t *bp,
    const zbookmark_phys_t *zb)
{
	vdev_async_mirror_t *am = aw->aw_am;
	spa_t *spa = am->am_spa;
	vdev_t *tvd = am->am_lagging->vdev_top;
	uint64_t asize = vdev_gang_header_asize(tvd);
	uint64_t gsize = spa_dynamic_gang_header(spa) ?
	    vdev_gang_header_psize(tvd) : SPA_OLD_GANGBLOCKSIZE;
	abd_t *abd = abd_alloc_linear(asize, B_TRUE);
	int err = SET_ERROR(ENXIO);

	ASSERT3U(gsize, <=, asize);

	for (int d = 0; d < BP_GET_NDVAS(bp) && err != 0; d++) {
		const dva_t *dva = &bp->blk_dva[d];

		if (DVA_GET_VDEV(dva) != tvd->vdev_id)
			continue;
		spa_config_enter(spa, SCL_STATE_ALL, am, RW_READER);
		err = zio_wait(zio_read_phys(NULL, am->am_leader,
		    DVA_GET_OFFSET(dva) + VDEV_LABEL_START_SIZE, asize, abd,
		    ZIO_CHECKSUM_OFF, NULL, NULL, ZIO_PRIORITY_ASYNC_READ,
		    ZIO_FLAG_CANFAIL, B_FALSE));
		spa_config_exit(spa, SCL_STATE_ALL, am);
		if (err != 0)
			continue;
		err = zio_checksum_verify_bp(spa, bp, abd, gsize);
		if (err != 0 && gsize > SPA_OLD_GANGBLOCKSIZE &&
		    zio_checksum_verify_bp(spa, bp, abd,
		    SPA_OLD_GANGBLOCKSIZE) == 0) {
			gsize = SPA_OLD_GANGBLOCKSIZE;
			err = 0;
		}
	}
	if (err != 0) {
		zfs_dbgmsg("async mirror: txg %llu: gang header of block "
		    "<%llu, %llu, %lld, %llx> cannot be read, error %d",
		    (u_longlong_t)aw->aw_txg, (u_longlong_t)zb->zb_objset,
		    (u_longlong_t)zb->zb_object, (longlong_t)zb->zb_level,
		    (u_longlong_t)zb->zb_blkid, err);
		abd_free(abd);
		return (err);
	}

	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		const dva_t *dva = &bp->blk_dva[d];
		uint64_t offset = DVA_GET_OFFSET(dva);

		if (DVA_GET_VDEV(dva) != tvd->vdev_id ||
		    async_mirror_walk_done(aw, offset, asize))
			continue;

		async_mirror_blk_t *ab = async_mirror_blk_alloc(aw, bp, zb,
		    asize);

		ab->ab_off[ab->ab_ncopies] = offset;
		ab->ab_size[ab->ab_ncopies++] = asize;
		ab->ab_abd = abd_alloc_for_io(asize, B_TRUE);
		abd_copy(ab->ab_abd, abd, asize);
		zfs_range_tree_add(aw->aw_done, offset, asize);
		async_mirror_walk_queue(aw, ab);
	}

	zio_gbh_phys_t *gbh = abd_to_buf(abd);

	if (BP_SHOULD_BYTESWAP(bp))
		byteswap_uint64_array(gbh, gsize);
	for (int g = 0; g < gbh_nblkptrs(gsize) && err == 0; g++) {
		const blkptr_t *gbp = gbh_bp(gbh, g);

		if (!BP_IS_HOLE(gbp))
			err = async_mirror_walk_add(aw, gbp, zb);
	}
	abd_free(abd);
	return (err);
}

/*
 * Add one block born in the txg of the walk: each of its copies on the
 * mirror that the walk has not written yet, with its own allocated size.
 * The walk gets the bytes of the largest of these copies, so that each copy
 * is a prefix of them: from the ARC or through the block pointer, padded
 * with zeros after the physical size, or as it is from that copy on the
 * leader.  Stop the walk after an error of its I/O.
 */
static int
async_mirror_walk_add(async_mirror_walk_t *aw, const blkptr_t *bp,
    const zbookmark_phys_t *zb)
{
	vdev_t *tvd = aw->aw_am->am_lagging->vdev_top;
	uint64_t asize = async_mirror_bp_asize(tvd, bp);
	async_mirror_blk_t *ab = NULL;
	int err = async_mirror_walk_error(aw);
	int big = 0;

	if (err != 0)
		return (err);
	if (BP_IS_GANG(bp))
		return (async_mirror_walk_gang(aw, bp, zb));

	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		const dva_t *dva = &bp->blk_dva[d];
		uint64_t size = DVA_GET_ASIZE(dva);

		if (!async_mirror_walk_dva_ok(aw, dva, asize))
			continue;
		if (ab == NULL)
			ab = async_mirror_blk_alloc(aw, bp, zb, 0);
		if (size > ab->ab_asize) {
			ab->ab_asize = size;
			big = ab->ab_ncopies;
		}
		ab->ab_off[ab->ab_ncopies] = DVA_GET_OFFSET(dva);
		ab->ab_size[ab->ab_ncopies++] = size;
		zfs_range_tree_add(aw->aw_done, DVA_GET_OFFSET(dva), size);
	}
	if (ab == NULL)
		return (0);
	if (BP_GET_CHECKSUM(bp) == ZIO_CHECKSUM_OFF ||
	    BP_GET_TYPE(bp) == DMU_OT_INTENT_LOG)
		ab->ab_raw = ab->ab_off[big];
	async_mirror_walk_queue(aw, ab);
	return (0);
}

/* Keep a dataset root born in the txg of the walk, once. */
static void
async_mirror_walk_root(async_mirror_walk_t *aw, uint64_t objset,
    const blkptr_t *bp)
{
	if (BP_IS_HOLE(bp) || BP_GET_PHYSICAL_BIRTH(bp) != aw->aw_txg)
		return;
	for (async_mirror_root_t *ar = list_head(&aw->aw_roots); ar != NULL;
	    ar = list_next(&aw->aw_roots, ar)) {
		if (BP_EQUAL(&ar->ar_bp, bp))
			return;
	}

	async_mirror_root_t *ar = kmem_alloc(sizeof (*ar), KM_SLEEP);

	ar->ar_objset = objset;
	ar->ar_bp = *bp;
	list_insert_tail(&aw->aw_roots, ar);
}

/*
 * A block of a bpobj in the MOS lists blocks that no tree of the txg needs
 * to point to, such as the frees that the txg deferred to the next txg.
 * The later txgs read them to free them.  Add each one born in the txg of
 * the walk.
 */
static int
async_mirror_walk_bpobj(async_mirror_walk_t *aw, const blkptr_t *bp,
    const zbookmark_phys_t *zb)
{
	arc_flags_t aflags = ARC_FLAG_WAIT;
	arc_buf_t *buf = NULL;
	int err = arc_read(NULL, aw->aw_am->am_spa, bp, arc_getbuf_func, &buf,
	    ZIO_PRIORITY_ASYNC_READ, ZIO_FLAG_CANFAIL, &aflags, zb);

	if (err != 0)
		return (err);

	const blkptr_t *bps = buf->b_data;
	uint64_t n = arc_buf_size(buf) / sizeof (blkptr_t);

	for (uint64_t i = 0; i < n && err == 0; i++) {
		if (!BP_IS_HOLE(&bps[i]) && !BP_IS_EMBEDDED(&bps[i]) &&
		    BP_GET_PHYSICAL_BIRTH(&bps[i]) == aw->aw_txg)
			err = async_mirror_walk_add(aw, &bps[i], zb);
	}
	arc_buf_destroy(buf, &buf);
	return (err);
}

/*
 * The bptree of the pool lists the root blocks of the datasets that are
 * destroyed in the background, and no tree of the txg points to them.  The
 * later txgs read their blocks to free them.  Keep each root born in the
 * txg of the walk.  The bptree is found through the MOS directory of the
 * txg, so open the MOS of the txg from rootbp.
 */
static int
async_mirror_walk_bptree(async_mirror_walk_t *aw, blkptr_t *rootbp)
{
	objset_t *os;
	dmu_buf_t *db;
	uint64_t obj;
	int err = dmu_objset_open_impl(aw->aw_am->am_spa, NULL, rootbp, &os);

	if (err != 0)
		return (err);
	err = zap_lookup(os, DMU_POOL_DIRECTORY_OBJECT, DMU_POOL_BPTREE_OBJ,
	    sizeof (uint64_t), 1, &obj);
	if (err == ENOENT) {
		dmu_objset_evict(os);
		return (0);
	}
	if (err == 0)
		err = dmu_bonus_hold(os, obj, FTAG, &db);
	if (err == 0) {
		const bptree_phys_t *bt = db->db_data;
		uint64_t begin = bt->bt_begin, end = bt->bt_end;

		dmu_buf_rele(db, FTAG);
		for (uint64_t i = begin; i < end && err == 0; i++) {
			bptree_entry_phys_t bte;

			err = dmu_read(os, obj, i * sizeof (bte), sizeof (bte),
			    &bte, DMU_READ_NO_PREFETCH);
			if (err == 0) {
				async_mirror_walk_root(aw, bte.be_zb.zb_objset,
				    &bte.be_bp);
			}
		}
	}
	dmu_objset_evict(os);
	return (err);
}

/*
 * The traverse callback.  In the MOS, a dnode of a dataset leads to the
 * dataset's root block, which the MOS tree does not point to.
 */
static int
async_mirror_walk_cb(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const dnode_phys_t *dnp, void *arg)
{
	(void) spa, (void) zilog;
	async_mirror_walk_t *aw = arg;

	if (zb->zb_level == ZB_DNODE_LEVEL) {
		if (zb->zb_objset == DMU_META_OBJSET &&
		    dnp->dn_bonustype == DMU_OT_DSL_DATASET &&
		    dnp->dn_bonuslen >= sizeof (dsl_dataset_phys_t)) {
			const dsl_dataset_phys_t *dsp = (const void *)
			    (dnp->dn_bonus +
			    (dnp->dn_nblkptr - 1) * sizeof (blkptr_t));

			async_mirror_walk_root(aw, zb->zb_object, &dsp->ds_bp);
		}
		return (0);
	}
	if (BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp) || BP_IS_REDACTED(bp) ||
	    BP_GET_PHYSICAL_BIRTH(bp) != aw->aw_txg)
		return (0);
	if (zb->zb_objset == DMU_META_OBJSET && zb->zb_level == 0 &&
	    BP_GET_TYPE(bp) == DMU_OT_BPOBJ) {
		int err = async_mirror_walk_bpobj(aw, bp, zb);

		if (err != 0)
			return (err);
	}
	return (async_mirror_walk_add(aw, bp, zb));
}

/* A log block of a log claimed in the txg of the walk. */
static int
async_mirror_walk_zil_blk(zilog_t *zilog, const blkptr_t *bp, void *arg,
    uint64_t claim_txg)
{
	(void) zilog, (void) claim_txg;
	async_mirror_walk_t *aw = arg;
	zbookmark_phys_t zb;

	if (BP_IS_HOLE(bp))
		return (0);
	SET_BOOKMARK(&zb, aw->aw_objset, ZB_ZIL_OBJECT, ZB_ZIL_LEVEL,
	    bp->blk_cksum.zc_word[ZIL_ZC_SEQ]);
	aw->aw_zil_error = async_mirror_walk_add(aw, bp, &zb);
	return (aw->aw_zil_error);
}

/*
 * A write record of a log claimed in the txg of the walk.  The claim
 * allocated the data blocks born from the claim txg on; see zil_claim_write().
 */
static int
async_mirror_walk_zil_lr(zilog_t *zilog, const lr_t *lrc, void *arg,
    uint64_t claim_txg)
{
	(void) zilog;
	async_mirror_walk_t *aw = arg;
	zbookmark_phys_t zb;

	if (lrc->lrc_txtype != TX_WRITE ||
	    lrc->lrc_reclen < sizeof (lr_write_t))
		return (0);

	const lr_write_t *lr = (const lr_write_t *)lrc;
	const blkptr_t *bp = &lr->lr_blkptr;

	if (BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp) ||
	    BP_GET_BIRTH(bp) < claim_txg)
		return (0);
	SET_BOOKMARK(&zb, aw->aw_objset, lr->lr_foid, ZB_ZIL_LEVEL,
	    lr->lr_offset / BP_GET_LSIZE(bp));
	aw->aw_zil_error = async_mirror_walk_add(aw, bp, &zb);
	return (aw->aw_zil_error);
}

/*
 * Add the blocks of the log of the dataset whose root is ar, if the import
 * claimed that log in the txg of the walk.  The claim visits the same blocks,
 * up to the last block and record that it claimed.  A read error before the
 * last claimed block is an error of the walk; the last one need not read.
 */
static int
async_mirror_walk_zil(async_mirror_walk_t *aw, const async_mirror_root_t *ar)
{
	spa_t *spa = aw->aw_am->am_spa;
	arc_flags_t aflags = ARC_FLAG_WAIT;
	zio_flag_t flags = ZIO_FLAG_CANFAIL;
	arc_buf_t *buf = NULL;
	zbookmark_phys_t zb;
	zil_header_t zh;

	SET_BOOKMARK(&zb, ar->ar_objset, ZB_ROOT_OBJECT, ZB_ROOT_LEVEL,
	    ZB_ROOT_BLKID);
	if (BP_IS_PROTECTED(&ar->ar_bp))
		flags |= ZIO_FLAG_RAW;

	int err = arc_read(NULL, spa, &ar->ar_bp, arc_getbuf_func, &buf,
	    ZIO_PRIORITY_ASYNC_READ, flags, &aflags, &zb);

	if (err != 0)
		return (err);
	zh = ((objset_phys_t *)buf->b_data)->os_zil_header;
	arc_buf_destroy(buf, &buf);
	if (zh.zh_claim_txg != aw->aw_txg || BP_IS_HOLE(&zh.zh_log))
		return (0);

	zilog_t *zilog = zil_alloc(spa_meta_objset(spa), &zh);

	aw->aw_objset = ar->ar_objset;
	aw->aw_zil_error = 0;
	err = zil_parse(zilog, async_mirror_walk_zil_blk,
	    async_mirror_walk_zil_lr, aw, zh.zh_claim_txg, B_FALSE);
	if (err != 0 && aw->aw_zil_error == 0 &&
	    zilog->zl_parse_blk_seq == zh.zh_claim_blk_seq)
		err = 0;
	zil_free(zilog);
	return (err);
}

#ifdef ZFS_DEBUG
typedef struct async_mirror_verify_arg {
	uint64_t	ava_vdev;
	zfs_range_tree_t *ava_alloc;
	zfs_range_tree_t *ava_free;
} async_mirror_verify_arg_t;

static int
async_mirror_verify_cb(space_map_entry_t *sme, void *arg)
{
	async_mirror_verify_arg_t *ava = arg;
	zfs_range_tree_t *rt = (sme->sme_type == SM_ALLOC) ?
	    ava->ava_alloc : ava->ava_free;

	if (sme->sme_vdev == ava->ava_vdev) {
		zfs_range_tree_clear(rt, sme->sme_offset, sme->sme_run);
		zfs_range_tree_add(rt, sme->sme_offset, sme->sme_run);
	}
	return (0);
}

static void
async_mirror_rt_clear_cb(void *arg, uint64_t start, uint64_t size)
{
	zfs_range_tree_clear(arg, start, size);
}

static void
async_mirror_verify_note_cb(void *arg, uint64_t start, uint64_t size)
{
	zfs_dbgmsg("async mirror: txg %llu: range %llu, %llu bytes",
	    (u_longlong_t)*(uint64_t *)arg, (u_longlong_t)start,
	    (u_longlong_t)size);
}

/* rt minus each range of the other trees, which can be NULL. */
static zfs_range_tree_t *
async_mirror_rt_minus(zfs_range_tree_t *rt, zfs_range_tree_t *a,
    zfs_range_tree_t *b)
{
	zfs_range_tree_t *res = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
	    NULL, 0, 0);

	zfs_range_tree_walk(rt, zfs_range_tree_add, res);
	zfs_range_tree_walk(a, async_mirror_rt_clear_cb, res);
	if (b != NULL)
		zfs_range_tree_walk(b, async_mirror_rt_clear_cb, res);
	return (res);
}

static void
async_mirror_rt_free(zfs_range_tree_t *rt)
{
	zfs_range_tree_vacate(rt, NULL, NULL);
	zfs_range_tree_destroy(rt);
}

/*
 * Check that the walk of the txg of at covered its record: each range that
 * the txg allocated on the mirror was written or skipped by the walk, or
 * freed in the same txg, and the walk wrote nothing else.
 */
static void
async_mirror_walk_verify(vdev_async_mirror_t *am, async_mirror_txg_t *at,
    zfs_range_tree_t *done)
{
	async_mirror_verify_arg_t ava = {
		.ava_vdev = am->am_lagging->vdev_top->vdev_id,
		.ava_alloc = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
		    NULL, 0, 0),
		.ava_free = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
		    NULL, 0, 0),
	};
	space_map_t *sm = NULL;
	int err = space_map_open(&sm, spa_meta_objset(am->am_spa), at->at_obj,
	    0, UINT64_MAX, SPA_MINBLOCKSHIFT);

	if (err == 0) {
		err = space_map_iterate(sm, space_map_length(sm),
		    async_mirror_verify_cb, &ava);
		space_map_close(sm);
	}
	if (err == 0) {
		zfs_range_tree_t *missed = async_mirror_rt_minus(ava.ava_alloc,
		    done, ava.ava_free);
		zfs_range_tree_t *extra = async_mirror_rt_minus(done,
		    ava.ava_alloc, NULL);
		uint64_t m = zfs_range_tree_space(missed);
		uint64_t x = zfs_range_tree_space(extra);

		zfs_range_tree_walk(missed, async_mirror_verify_note_cb,
		    &at->at_txg);
		zfs_range_tree_walk(extra, async_mirror_verify_note_cb,
		    &at->at_txg);
		if (m != 0 || x != 0) {
			zfs_panic_recover("async mirror: txg %llu: the walk "
			    "missed %llu bytes of the record, and wrote %llu "
			    "bytes outside it", (u_longlong_t)at->at_txg,
			    (u_longlong_t)m, (u_longlong_t)x);
		}
		async_mirror_rt_free(missed);
		async_mirror_rt_free(extra);
	}
	async_mirror_rt_free(ava.ava_alloc);
	async_mirror_rt_free(ava.ava_free);
}
#endif

/*
 * Walk the tree of the txg of at: the MOS, then each dataset whose root was
 * born in that txg, with its log if the import claimed it in that txg.
 * Write each block to the lagging child, and add its ranges to done; see
 * "The I/O of the walk".  On the last try, skip a lost data block, add its
 * ranges to done too, and move it to the list defer (*ndefp blocks) on
 * success.  On an error that is not a write error, set *metap: a block
 * cannot be read.
 */
static int
async_mirror_walk(vdev_async_mirror_t *am, async_mirror_txg_t *at,
    zfs_range_tree_t *done, boolean_t last, boolean_t *metap, list_t *defer,
    uint_t *ndefp)
{
	spa_t *spa = am->am_spa;
	int flags = TRAVERSE_POST | TRAVERSE_PREFETCH_METADATA |
	    TRAVERSE_NO_DECRYPT;
	blkptr_t rootbp = at->at_ub.ub_rootbp;
	async_mirror_walk_t aw = {
		.aw_am = am,
		.aw_txg = at->at_txg,
		.aw_done = done,
		.aw_last = last,
	};
	async_mirror_root_t *ar;

	ASSERT(!MUTEX_HELD(&am->am_lock));

	mutex_init(&aw.aw_lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&aw.aw_cv, NULL, CV_DEFAULT, NULL);
	aw.aw_sort = vmem_alloc(ASYNC_MIRROR_SORT_BLOCKS *
	    sizeof (async_mirror_blk_t *), KM_SLEEP);
	aw.aw_rio = zio_root(spa, NULL, NULL, ZIO_FLAG_CANFAIL);
	aw.aw_progress = gethrtime();
	list_create(&aw.aw_roots, sizeof (async_mirror_root_t),
	    offsetof(async_mirror_root_t, ar_node));
	async_mirror_defer_list_create(&aw.aw_defer);

	int err = traverse_root(spa, DMU_META_OBJSET, &rootbp, at->at_txg - 1,
	    flags, async_mirror_walk_cb, &aw);
	if (err == 0)
		err = async_mirror_walk_bptree(&aw, &rootbp);
	while (err == 0 && (ar = list_remove_head(&aw.aw_roots)) != NULL) {
		err = traverse_root(spa, ar->ar_objset, &ar->ar_bp,
		    at->at_txg - 1, flags, async_mirror_walk_cb, &aw);
		if (err == 0)
			err = async_mirror_walk_zil(&aw, ar);
		kmem_free(ar, sizeof (*ar));
	}
	async_mirror_walk_issue(&aw, err != 0);
	(void) zio_wait(aw.aw_rio);
	ASSERT0(aw.aw_inflight);
	if (err == 0)
		err = async_mirror_walk_error(&aw);

	mutex_enter(&am->am_lock);
	spa->spa_async_mirror_stats.sams_data_errors += aw.aw_data_errors;
	spa->spa_async_mirror_stats.sams_copied_bytes += aw.aw_copied;
	async_mirror_update_stats(am);
	mutex_exit(&am->am_lock);

	while ((ar = list_remove_head(&aw.aw_roots)) != NULL)
		kmem_free(ar, sizeof (*ar));
	list_destroy(&aw.aw_roots);
	vmem_free(aw.aw_sort, ASYNC_MIRROR_SORT_BLOCKS *
	    sizeof (async_mirror_blk_t *));
	if (err != 0) {
		async_mirror_defer_list_free(&aw.aw_defer);
		aw.aw_ndefer = 0;
	}
	list_move_tail(defer, &aw.aw_defer);
	list_destroy(&aw.aw_defer);
	*ndefp = aw.aw_ndefer;
	cv_destroy(&aw.aw_cv);
	mutex_destroy(&aw.aw_lock);

	*metap = (err != 0 && aw.aw_write_error == 0);
	if (*metap) {
		zfs_dbgmsg("async mirror: txg %llu: a metadata block cannot "
		    "be read, error %d", (u_longlong_t)at->at_txg, err);
	}
	return (err);
}

/*
 * Deferred blocks.
 *
 * A read error can last longer than the tries of the copy of a txg: a
 * sector that fails for a while, or a path that comes back later.  A data
 * block that no copy can supply on the last try is skipped, so that the copy
 * goes on.  That is not yet proof that the block is lost, so the block is
 * deferred: it is tried again before the record of its txg is released.
 * Until then the holds keep its range from reuse: the block is live, or its
 * free is held by the record of a later txg, and records are released
 * oldest first.
 *
 * A txg with deferred blocks ends its commit, so the uberblock that the
 * commit writes is of that txg.  Before the worker writes it to the lagging
 * child, it adds the deferred blocks to a ZAP in the MOS, and waits until
 * that txg is on disk.  So when the lagging child holds the txg, the MOS
 * holds its deferred blocks, also after a crash.  The import attaches them
 * to the records, and release waits for them.  If the
 * pool stops first, the worker does not write the uberblock, and the next
 * import copies the txg again.
 *
 * The release thread tries the deferred blocks of each record that the
 * lagging child has passed: at once, and then up to
 * zfs_vdev_async_mirror_read_retries more times, with ASYNC_MIRROR_RETRY_MS
 * between two tries.  One try takes the blocks of many records together, so
 * the records do not wait for each other's tries; see
 * async_mirror_defer_retry().  The bytes come from the ARC, or from a read
 * through the block pointer, the normal checked read path.  A block that
 * reads with a good checksum is written to the lagging child and flushed;
 * its entry in the error log is removed, the data_recovered counter counts
 * it, and an async_mirror_recovered event names it.  A block that still
 * fails on the last try is lost: the last read is not speculative, so it
 * logs the block and posts an ereport, and the data_errors counter counts
 * it.  The record is then released.
 * The replay does not wait for these tries.  A write error on the lagging
 * child fails the copy, as a write error of the replay does.
 *
 * The error episode.  A file that cannot be read often spans many txgs.  If
 * the copy of each of those txgs ran its own burst of tries, the copy would
 * stop for zfs_vdev_async_mirror_read_retries times
 * ASYNC_MIRROR_RETRY_MS per txg, 30 s by default, while
 * the deferred blocks get the same tries in the release thread anyway.  So
 * after a copy defers blocks, an episode starts (am_defer_quick).  During
 * the episode, the first try of the copy of a txg is also its last try for
 * data blocks: a data block that no copy can supply is deferred at once, and
 * the copy goes on.  The episode ends when a deferred block is read after
 * all (the error has passed, so a new error gets its burst again), or when
 * no deferred block waits any more (the release thread has made all of its
 * tries).  A metadata block, a gang header, or a leader that cannot be read
 * is not deferred: such an error still gets the full burst of tries, and
 * then freezes the lagging child.  An import starts with no
 * episode.
 *
 * When the copy fails, the DTLs of the lagging child start at the oldest
 * txg with a deferred block, so the resilver copies it.  An import that does
 * not resume the copy does the same.  A freeze reports the deferred blocks
 * as lost: the frozen child gets no I/O.
 */

/*
 * The name of the MOS entry of a deferred block: its txg and the offset of
 * its first copy.  The blocks of a gang block share one bookmark.
 */
static void
async_mirror_defer_name(char *name, size_t len, uint64_t txg,
    const blkptr_t *bp)
{
	(void) snprintf(name, len, "%llx-%llx", (u_longlong_t)txg,
	    (u_longlong_t)DVA_GET_OFFSET(&bp->blk_dva[0]));
}

typedef struct async_mirror_defer_arg {
	vdev_async_mirror_t	*ada_am;
	uint64_t		ada_txg;
	list_t			*ada_def;
} async_mirror_defer_arg_t;

/* Add the deferred blocks of one txg to the MOS.  A sync task. */
static void
async_mirror_defer_sync(void *arg, dmu_tx_t *tx)
{
	async_mirror_defer_arg_t *ada = arg;
	vdev_async_mirror_t *am = ada->ada_am;
	objset_t *mos = spa_meta_objset(am->am_spa);

	ASSERT(dsl_pool_sync_context(dmu_objset_pool(mos)));
	ASSERT3U(am->am_zap_obj, !=, 0);

	if (am->am_defer_zap == 0) {
		uint64_t zap = zap_create(mos, DMU_OTN_ZAP_METADATA,
		    DMU_OT_NONE, 0, tx);

		VERIFY0(zap_add(mos, DMU_POOL_DIRECTORY_OBJECT,
		    DMU_POOL_ASYNC_MIRROR_DEFERRED, sizeof (uint64_t), 1,
		    &zap, tx));
		mutex_enter(&am->am_lock);
		am->am_defer_zap = zap;
		mutex_exit(&am->am_lock);
	}
	for (async_mirror_defer_t *ad = list_head(ada->ada_def); ad != NULL;
	    ad = list_next(ada->ada_def, ad)) {
		uint64_t val[ASYNC_MIRROR_DEFER_WORDS];
		char name[96];

		val[0] = ada->ada_txg;
		memcpy(&val[1], &ad->ad_bp, sizeof (blkptr_t));
		memcpy(&val[1 + sizeof (blkptr_t) / sizeof (uint64_t)],
		    &ad->ad_zb, sizeof (zbookmark_phys_t));
		async_mirror_defer_name(name, sizeof (name), ada->ada_txg,
		    &ad->ad_bp);
		VERIFY0(zap_update(mos, am->am_defer_zap, name,
		    sizeof (uint64_t), ASYNC_MIRROR_DEFER_WORDS, val, tx));
	}
}

/*
 * Make the deferred blocks of the txg of at durable, and attach them to at.
 * The worker calls this before it writes the uberblock of the txg.  It takes
 * the n blocks of the list def in all cases.  Return
 * ECANCELED if the pool stops first, is frozen, or cannot assign a txg:
 * the uberblock must then not be written.  Wait while the pool threads are
 * suspended: the export suspends them before the sync thread stops, and so
 * waits for a txg in progress here.
 */
static int
async_mirror_defer_persist(vdev_async_mirror_t *am, async_mirror_txg_t *at,
    list_t *def, uint_t n)
{
	spa_t *spa = am->am_spa;
	dsl_pool_t *dp = spa_get_dsl(spa);
	async_mirror_defer_arg_t ada = {
		.ada_am = am,
		.ada_txg = at->at_txg,
		.ada_def = def
	};
	int err = 0;

	ASSERT(!MUTEX_HELD(&am->am_lock));
	ASSERT3U(n, >, 0);

	mutex_enter(&am->am_lock);
	am->am_persist = AMP_WAIT;
	cv_broadcast(&am->am_drain_cv);
	while ((am->am_quiesce > 0 || !spa->spa_sync_on) && !am->am_stop) {
		(void) cv_timedwait_idle(&am->am_cv, &am->am_lock,
		    ddi_get_lbolt() + hz / 10 + 1);
	}
	if (am->am_stop)
		err = SET_ERROR(ECANCELED);
	else
		am->am_persist = AMP_SYNC;
	mutex_exit(&am->am_lock);

	if (err == 0) {
		dmu_tx_t *tx = dmu_tx_create_dd(dp->dp_mos_dir);

		/* A pool that suspends waits here, as the release does. */
		err = dmu_tx_assign(tx, DMU_TX_WAIT | DMU_TX_SUSPEND);
		if (err == 0) {
			uint64_t txg = dmu_tx_get_txg(tx);

			dsl_sync_task_nowait(dp, async_mirror_defer_sync, &ada,
			    tx);
			dmu_tx_commit(tx);
			txg_wait_synced(dp, txg);
			/* A txg after spa_freeze() never reaches the disk. */
			if (txg > spa_freeze_txg(spa))
				err = SET_ERROR(ECANCELED);
		} else {
			/* The txg is copied again at the next import. */
			dmu_tx_abort(tx);
			err = SET_ERROR(ECANCELED);
		}
	}

	mutex_enter(&am->am_lock);
	am->am_persist = AMP_NONE;
	cv_broadcast(&am->am_drain_cv);
	if (err == 0) {
		ASSERT(list_is_empty(&at->at_defer));
		list_move_tail(&at->at_defer, def);
		at->at_ndefer_count = n;
		at->at_ndefer = n;
		at->at_defer_tries = 0;
		spa->spa_async_mirror_stats.sams_data_deferred += n;
		async_mirror_defer_floor(am);
		am->am_defer_quick = B_TRUE;
		async_mirror_update_stats(am);
	}
	mutex_exit(&am->am_lock);

	if (err != 0) {
		async_mirror_defer_list_free(def);
		zfs_dbgmsg("async mirror: txg %llu: the deferred blocks are "
		    "not durable, error %d", (u_longlong_t)ada.ada_txg, err);
	} else {
		zfs_dbgmsg("async mirror: txg %llu: %u data blocks deferred",
		    (u_longlong_t)ada.ada_txg, n);
	}
	return (err);
}

/*
 * Copy the blocks of the txg of this record to the lagging child; see
 * async_mirror_walk().  Nothing is flushed: async_mirror_commit() does
 * that.  On an error, *kindp tells what failed: a write to the lagging
 * child, or a read of the walk.  last is set on the last try.  The data
 * blocks that the copy skipped are moved to def (*ndefp blocks).
 */
int
async_mirror_copy(vdev_async_mirror_t *am, async_mirror_txg_t *at,
    boolean_t last, async_mirror_err_t *kindp, list_t *def, uint_t *ndefp)
{
	int err = 0;

	ASSERT(!MUTEX_HELD(&am->am_lock));
	ASSERT(at->at_ready);
	ASSERT3U(at->at_txg, >, am->am_applied_txg);
	ASSERT(list_is_empty(def));

	*ndefp = 0;
	*kindp = ASYNC_MIRROR_ERR_WRITE;
	if (at->at_bytes > 0) {
		zfs_range_tree_t *done = zfs_range_tree_create(NULL,
		    ZFS_RANGE_SEG64, NULL, 0, 0);

		/*
		 * The tree of the txg starts at the root block of the
		 * record's uberblock.  The uberblock of the newest record at
		 * import can be of a later txg that did not change the MOS,
		 * so test the birth of its root block, not its txg.
		 */
		if (BP_GET_LOGICAL_BIRTH(&at->at_ub.ub_rootbp) == at->at_txg) {
			boolean_t meta;

			err = async_mirror_walk(am, at, done, last, &meta,
			    def, ndefp);
			if (err != 0) {
				*kindp = meta ? ASYNC_MIRROR_ERR_META :
				    ASYNC_MIRROR_ERR_WRITE;
			}
		}
#ifdef ZFS_DEBUG
		if (err == 0)
			async_mirror_walk_verify(am, at, done);
#endif
		zfs_range_tree_vacate(done, NULL, NULL);
		zfs_range_tree_destroy(done);
	}
	return (err);
}

/*
 * Bring the lagging child up to the txg of the record at, after
 * async_mirror_copy() has copied the blocks of that txg and of every txg
 * before it that the child lacks: flush, make the deferred blocks of the
 * txg of at durable (def, ndef blocks), write the uberblock of at, and
 * flush again.  wrote tells whether the copies wrote a block.  On an error,
 * *kindp tells what failed: a write to the lagging child, or the stop of
 * the pool before the deferred blocks were durable.  def is empty on
 * return.
 *
 * One commit covers several txgs, so a backlog of small txgs costs two
 * flushes and one uberblock per commit, not per txg.  This keeps the order
 * of the copy (the blocks of a txg are on disk before its uberblock):
 *
 * - The child holds the txg of its newest uberblock in full, and every
 *   block of that txg stays where it is until the child passes its free:
 *   the free is held, and a record is released only after its commit.  So
 *   each write of the copied txgs lands on a range that the newest
 *   uberblock of the child does not use, and a crash before the commit
 *   leaves the child at its newest committed txg.
 *
 * - For the same reason the copied txgs do not overwrite each other: a
 *   range that one of them uses is not reused by a later one before the
 *   commit.  So their writes can be issued in any order.
 *
 * - The tree of the txg of at points to blocks born in that txg or before.
 *   The copy wrote those born after the child's txg, each in the walk of
 *   its own txg, which visits every block of that txg that is still
 *   allocated at its end; the child already holds the older ones.  The
 *   first flush makes all of them durable before the uberblock is written.
 *   So the uberblock of at is the newest txg whose blocks are all on the
 *   child, and the uberblocks of the txgs before it are not needed.
 *
 * - A txg whose copy deferred blocks ends the commit, so its uberblock is
 *   the one written, after its deferred blocks are durable in the MOS; see
 *   "Deferred blocks".  A read error ends the commit before the txg that
 *   failed, which is then the first txg of the next commit, with its own
 *   tries.  The release, the freeze and the failure see only committed
 *   txgs.
 *
 * vdev_uberblock_write() puts the uberblock in the slot of its txg, as the
 * pool does, so an import of the child finds the newest one in its ring.
 */
int
async_mirror_commit(vdev_async_mirror_t *am, async_mirror_txg_t *at,
    boolean_t wrote, list_t *def, uint_t ndef, async_mirror_err_t *kindp)
{
	int err = 0;

	ASSERT(!MUTEX_HELD(&am->am_lock));
	ASSERT(at->at_ready);

	*kindp = ASYNC_MIRROR_ERR_WRITE;
	if (wrote)
		err = async_mirror_flush(am);

	/* Make the skipped blocks durable before the txg is passed. */
	if (ndef > 0 && err == 0) {
		err = async_mirror_defer_persist(am, at, def, ndef);
		if (err == ECANCELED)
			*kindp = ASYNC_MIRROR_ERR_STOP;
	}
	async_mirror_defer_list_free(def);

	if (err == 0) {
		*kindp = ASYNC_MIRROR_ERR_WRITE;
		err = async_mirror_write_ub(am, &at->at_ub);
		if (err == 0)
			err = async_mirror_flush(am);
	}
	return (err);
}

/*
 * Post one event for a burst of tries of the copy of a txg, at its start:
 * the copy of txg failed with a read error, err.
 */
void
async_mirror_retry_event(vdev_async_mirror_t *am, uint64_t txg, int err)
{
	nvlist_t *nvl = fnvlist_alloc();

	zfs_dbgmsg("async mirror: copy of txg %llu: read error %d; trying "
	    "again up to %u times, %u ms apart", (u_longlong_t)txg, err,
	    zfs_vdev_async_mirror_read_retries, ASYNC_MIRROR_RETRY_MS);
	fnvlist_add_uint64(nvl, ZFS_EV_ASYNC_MIRROR_TXG, txg);
	fnvlist_add_int32(nvl, ZFS_EV_ASYNC_MIRROR_ERRNO, err);
	fnvlist_add_uint64(nvl, ZFS_EV_ASYNC_MIRROR_RETRIES,
	    zfs_vdev_async_mirror_read_retries);
	fnvlist_add_uint64(nvl, ZFS_EV_ASYNC_MIRROR_DELAY_MS,
	    ASYNC_MIRROR_RETRY_MS);
	spa_event_notify(am->am_spa, am->am_leader, nvl,
	    ESC_ZFS_ASYNC_MIRROR_RETRY);
	fnvlist_free(nvl);
}

/*
 * Wait before the next try of a copy, and count it.  Return false if the
 * pool stops in the meantime.
 */
boolean_t
async_mirror_retry_wait(vdev_async_mirror_t *am)
{
	clock_t end = ddi_get_lbolt() +
	    MSEC_TO_TICK(ASYNC_MIRROR_RETRY_MS);

	mutex_enter(&am->am_lock);
	am->am_spa->spa_async_mirror_stats.sams_read_retries++;
	async_mirror_update_stats(am);
	while (!am->am_stop &&
	    cv_timedwait_idle(&am->am_cv, &am->am_lock, end) > 0)
		;
	boolean_t go = !am->am_stop;
	mutex_exit(&am->am_lock);
	return (go);
}

/*
 * Stop the copy after an error that stays: freeze the lagging child after a
 * read error, or fail the copy after a write error.  Then release the
 * records.  txg is the txg whose copy failed.  The caller holds am_lock.
 */
void
async_mirror_halt(vdev_async_mirror_t *am, uint64_t txg, int err,
    async_mirror_err_t kind)
{
	ASSERT(MUTEX_HELD(&am->am_lock));
	ASSERT3S(err, !=, 0);
	ASSERT3U(kind, !=, ASYNC_MIRROR_ERR_STOP);
	ASSERT(async_mirror_copying(am->am_state));

	boolean_t freeze = (kind != ASYNC_MIRROR_ERR_WRITE);

	/*
	 * A read error that stays after the tries freezes the child: a block
	 * other than a lost data block cannot be read, and the walk cannot see
	 * below it.  A write error is a failure.
	 */
	if (freeze) {
		zfs_dbgmsg("async mirror: copy of txg %llu: a block cannot be "
		    "read, error %d; the lagging child guid %llu is frozen at "
		    "txg %llu", (u_longlong_t)txg, err,
		    (u_longlong_t)am->am_lagging->vdev_guid,
		    (u_longlong_t)am->am_applied_txg);
	} else {
		zfs_dbgmsg("async mirror: copy of txg %llu failed, error %d",
		    (u_longlong_t)txg, err);
	}
	/*
	 * A write issued before this point was dropped for the child.
	 * dmu_sync() and Direct I/O write in open context, with the birth txg
	 * of an open txg.  No txg above am_fail_open_txg was open before here.
	 * The child lacks the txgs from the first one that the copy did not
	 * finish, or from the oldest txg with a deferred block, so that the
	 * resilver copies the deferred blocks too.
	 */
	am->am_fail_open_txg = spa_last_synced_txg(am->am_spa) +
	    TXG_CONCURRENT_STATES;
	am->am_fail_from = am->am_applied_txg + 1;
	if (am->am_defer_txg != 0)
		am->am_fail_from = MIN(am->am_fail_from, am->am_defer_txg);
	async_mirror_set_state(am, freeze ? AMS_FROZEN : AMS_FAILED);
	async_mirror_update_stats(am);
	if (am->am_release_zthr != NULL) {
		mutex_exit(&am->am_lock);
		zthr_wakeup(am->am_release_zthr);
		mutex_enter(&am->am_lock);
	}
}

/*
 * Set am_defer_txg to the oldest txg with deferred blocks that are not done.
 * When none is left, the error episode ends; see "Deferred blocks".  The
 * caller holds am_lock.
 */
void
async_mirror_defer_floor(vdev_async_mirror_t *am)
{
	ASSERT(MUTEX_HELD(&am->am_lock));

	am->am_defer_txg = 0;
	for (async_mirror_txg_t *at = list_head(&am->am_txgs); at != NULL;
	    at = list_next(&am->am_txgs, at)) {
		if (at->at_ndefer > 0) {
			am->am_defer_txg = at->at_txg;
			break;
		}
	}
	if (am->am_defer_txg == 0)
		am->am_defer_quick = B_FALSE;
	async_mirror_publish_txg(am);
}

/*
 * A record with deferred blocks that are not done is released without their
 * tries: the copy failed or froze, so every record is released.  After a
 * failure the DTLs of the lagging child cover them, and the resilver copies
 * them.  A frozen child gets no I/O, so they are lost.  The caller holds
 * am_lock.
 */
void
async_mirror_defer_drop(vdev_async_mirror_t *am, async_mirror_txg_t *at)
{
	spa_async_mirror_stats_t *sams = &am->am_spa->spa_async_mirror_stats;
	boolean_t frozen = (am->am_state == AMS_FROZEN);

	ASSERT(MUTEX_HELD(&am->am_lock));
	ASSERT(async_mirror_draining(am->am_state));

	zfs_dbgmsg("async mirror: txg %llu: %u deferred data blocks %s",
	    (u_longlong_t)at->at_txg, at->at_ndefer, frozen ?
	    "are lost: the lagging child is frozen" :
	    "are left to the resilver");
	if (frozen)
		sams->sams_data_errors += at->at_ndefer;
	ASSERT3U(sams->sams_data_deferred, >=, at->at_ndefer);
	sams->sams_data_deferred -= at->at_ndefer;
	at->at_ndefer = 0;
	async_mirror_defer_floor(am);
	async_mirror_update_stats(am);
}

/*
 * Remove the MOS entries of the deferred blocks of txg and of older txgs,
 * with the record of txg.  Destroy the ZAP when it is empty.  The ZAP is
 * small: it holds only the blocks that the copy could not read.
 */
void
async_mirror_defer_clear(vdev_async_mirror_t *am, uint64_t txg, dmu_tx_t *tx)
{
	objset_t *mos = spa_meta_objset(am->am_spa);
	zap_attribute_t *za = zap_attribute_alloc();
	nvlist_t *names = fnvlist_alloc();
	zap_cursor_t zc;
	uint64_t count = 0;

	ASSERT(dsl_pool_sync_context(dmu_objset_pool(mos)));
	ASSERT3U(am->am_defer_zap, !=, 0);

	/* Collect the names, then remove them: one walk of the ZAP. */
	for (zap_cursor_init(&zc, mos, am->am_defer_zap);
	    zap_cursor_retrieve(&zc, za) == 0; zap_cursor_advance(&zc)) {
		if (zfs_strtonum(za->za_name, NULL) <= txg)
			fnvlist_add_boolean(names, za->za_name);
	}
	zap_cursor_fini(&zc);
	zap_attribute_free(za);

	for (nvpair_t *nvp = nvlist_next_nvpair(names, NULL); nvp != NULL;
	    nvp = nvlist_next_nvpair(names, nvp)) {
		VERIFY0(zap_remove(mos, am->am_defer_zap, nvpair_name(nvp),
		    tx));
	}
	fnvlist_free(names);

	VERIFY0(zap_count(mos, am->am_defer_zap, &count));
	if (count == 0) {
		VERIFY0(zap_destroy(mos, am->am_defer_zap, tx));
		VERIFY0(zap_remove(mos, DMU_POOL_DIRECTORY_OBJECT,
		    DMU_POOL_ASYNC_MIRROR_DEFERRED, tx));
		mutex_enter(&am->am_lock);
		am->am_defer_zap = 0;
		mutex_exit(&am->am_lock);
	}
}

/*
 * The oldest record that the lagging child has passed and that has deferred
 * blocks to try, or NULL.  None while every record is released.
 */
async_mirror_txg_t *
async_mirror_defer_next(vdev_async_mirror_t *am)
{
	ASSERT(MUTEX_HELD(&am->am_lock));

	if (!async_mirror_copying(am->am_state) || am->am_defer_failed)
		return (NULL);
	for (async_mirror_txg_t *at = list_head(&am->am_txgs); at != NULL &&
	    at->at_ready && at->at_txg <= am->am_applied_txg;
	    at = list_next(&am->am_txgs, at)) {
		if (at->at_ndefer > 0)
			return (at);
	}
	return (NULL);
}

/*
 * Read one deferred block into abd (its allocated size; zeros after the
 * physical size): from the ARC, else through its block pointer.  The read
 * on the last try is not speculative, so a block that still fails is logged
 * and an ereport is posted.
 */
static int
async_mirror_defer_read(vdev_async_mirror_t *am, const async_mirror_defer_t *ad,
    abd_t *abd, boolean_t last)
{
	spa_t *spa = am->am_spa;
	const blkptr_t *bp = &ad->ad_bp;
	uint64_t psize = BP_GET_PSIZE(bp);
	zio_flag_t flags = ZIO_FLAG_CANFAIL | ZIO_FLAG_RAW;

	if (async_mirror_arc_copy(spa, bp, &ad->ad_zb, abd))
		return (0);
	if (!last)
		flags |= ZIO_FLAG_SPECULATIVE;

	abd_t *pabd = abd_get_offset_size(abd, 0, psize);
	int err = zio_wait(zio_read(NULL, spa, bp, pabd, psize, NULL, NULL,
	    ZIO_PRIORITY_ASYNC_READ, flags, &ad->ad_zb));

	abd_free(pabd);
	return (err);
}

/* Write a deferred block that was read to each of its copies. */
static void
async_mirror_defer_write(vdev_async_mirror_t *am, zio_t *pio,
    const async_mirror_defer_t *ad, abd_t *abd, uint64_t asize)
{
	const blkptr_t *bp = &ad->ad_bp;
	vdev_t *tvd = am->am_lagging->vdev_top;

	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		const dva_t *dva = &bp->blk_dva[d];
		uint64_t size = DVA_GET_ASIZE(dva);

		if (DVA_GET_VDEV(dva) != tvd->vdev_id || DVA_GET_GANG(dva) ||
		    size > asize)
			continue;
		zio_nowait(zio_write_phys(pio, am->am_lagging,
		    DVA_GET_OFFSET(dva) + VDEV_LABEL_START_SIZE, size,
		    abd_get_offset_size(abd, 0, size), ZIO_CHECKSUM_OFF,
		    async_mirror_abd_free_done, NULL, ZIO_PRIORITY_ASYNC_WRITE,
		    ZIO_FLAG_CANFAIL, B_FALSE));
	}
}

/* Post the event for a deferred block that was copied after all. */
static void
async_mirror_recovered_event(vdev_async_mirror_t *am, uint64_t txg,
    const async_mirror_defer_t *ad)
{
	nvlist_t *nvl = fnvlist_alloc();
	const zbookmark_phys_t *zb = &ad->ad_zb;

	zfs_dbgmsg("async mirror: txg %llu: deferred data block <%llu, %llu, "
	    "%lld, %llx> is copied", (u_longlong_t)txg,
	    (u_longlong_t)zb->zb_objset, (u_longlong_t)zb->zb_object,
	    (longlong_t)zb->zb_level, (u_longlong_t)zb->zb_blkid);
	fnvlist_add_uint64(nvl, ZFS_EV_ASYNC_MIRROR_TXG, txg);
	fnvlist_add_uint64(nvl, ZFS_EV_ASYNC_MIRROR_OBJSET, zb->zb_objset);
	fnvlist_add_uint64(nvl, ZFS_EV_ASYNC_MIRROR_OBJECT, zb->zb_object);
	fnvlist_add_int64(nvl, ZFS_EV_ASYNC_MIRROR_LEVEL, zb->zb_level);
	fnvlist_add_uint64(nvl, ZFS_EV_ASYNC_MIRROR_BLKID, zb->zb_blkid);
	spa_event_notify(am->am_spa, am->am_leader, nvl,
	    ESC_ZFS_ASYNC_MIRROR_RECOVERED);
	fnvlist_free(nvl);
}

/* What one try did with a deferred block. */
typedef enum async_mirror_defer_result {
	ASYNC_MIRROR_DEFER_NONE = 0,
	ASYNC_MIRROR_DEFER_READ,
	ASYNC_MIRROR_DEFER_RECOVERED,
	ASYNC_MIRROR_DEFER_LOST,
} async_mirror_defer_result_t;

/*
 * The most records, and about the most deferred blocks, that one try takes.
 * The first record is always taken whole, so the copy of the blocks can
 * still be large: it uses vmem_alloc().
 */
#define	ASYNC_MIRROR_DEFER_RECS		64
#define	ASYNC_MIRROR_DEFER_BATCH	256

/* One record in a try: where its blocks are in the copy of the lists. */
typedef struct async_mirror_defer_rec {
	uint64_t	dr_txg;
	uint_t		dr_tries;	/* its tries before this one */
	uint_t		dr_first;	/* its first block in the copy */
	uint_t		dr_count;	/* its blocks in the copy */
} async_mirror_defer_rec_t;

/* One try: a copy of the deferred blocks of up to DEFER_RECS records. */
typedef struct async_mirror_defer_try {
	async_mirror_defer_rec_t *dt_recs;
	uint_t		dt_nrecs;
	async_mirror_defer_t *dt_def;	/* the copy of their blocks */
	uint8_t		*dt_got;	/* async_mirror_defer_result_t */
	abd_t		**dt_abds;	/* the bytes read of each block */
	uint_t		dt_n;		/* blocks in dt_def */
	vdev_t		*dt_tvd;	/* the mirror of the lagging child */
} async_mirror_defer_try_t;

/*
 * Copy the deferred blocks of the records from first on into dt.  The
 * caller holds am_lock.
 */
static void
async_mirror_defer_try_init(vdev_async_mirror_t *am, async_mirror_txg_t *first,
    async_mirror_defer_try_t *dt)
{
	uint_t nrec = 0, n = 0;

	ASSERT(MUTEX_HELD(&am->am_lock));

	for (async_mirror_txg_t *at = first; at != NULL && at->at_ready &&
	    at->at_txg <= am->am_applied_txg;
	    at = list_next(&am->am_txgs, at)) {
		if (at->at_ndefer == 0)
			continue;
		if (nrec > 0 && (nrec == ASYNC_MIRROR_DEFER_RECS ||
		    n + at->at_ndefer_count > ASYNC_MIRROR_DEFER_BATCH))
			break;
		nrec++;
		n += at->at_ndefer_count;
	}

	dt->dt_recs = kmem_alloc(nrec * sizeof (*dt->dt_recs), KM_SLEEP);
	dt->dt_nrecs = nrec;
	dt->dt_def = vmem_alloc(n * sizeof (*dt->dt_def), KM_SLEEP);
	dt->dt_got = kmem_zalloc(n * sizeof (uint8_t), KM_SLEEP);
	dt->dt_abds = kmem_zalloc(n * sizeof (abd_t *), KM_SLEEP);
	dt->dt_n = n;
	dt->dt_tvd = am->am_lagging->vdev_top;

	uint_t r = 0, k = 0;

	for (async_mirror_txg_t *at = first; r < nrec;
	    at = list_next(&am->am_txgs, at)) {
		if (at->at_ndefer == 0)
			continue;
		dt->dt_recs[r].dr_txg = at->at_txg;
		dt->dt_recs[r].dr_tries = at->at_defer_tries;
		dt->dt_recs[r].dr_first = k;
		dt->dt_recs[r].dr_count = at->at_ndefer_count;
		for (async_mirror_defer_t *ad = list_head(&at->at_defer);
		    ad != NULL; ad = list_next(&at->at_defer, ad)) {
			dt->dt_def[k].ad_bp = ad->ad_bp;
			dt->dt_def[k].ad_zb = ad->ad_zb;
			dt->dt_def[k].ad_done = ad->ad_done;
			k++;
		}
		r++;
	}
	ASSERT3U(k, ==, n);
}

static void
async_mirror_defer_try_fini(async_mirror_defer_try_t *dt)
{
	for (uint_t i = 0; i < dt->dt_n; i++) {
		if (dt->dt_abds[i] != NULL)
			abd_free(dt->dt_abds[i]);
	}
	kmem_free(dt->dt_abds, dt->dt_n * sizeof (abd_t *));
	kmem_free(dt->dt_got, dt->dt_n * sizeof (uint8_t));
	vmem_free(dt->dt_def, dt->dt_n * sizeof (*dt->dt_def));
	kmem_free(dt->dt_recs, dt->dt_nrecs * sizeof (*dt->dt_recs));
}

/* Read each block of the try that is not done.  Return the blocks read. */
static uint_t
async_mirror_defer_try_read(vdev_async_mirror_t *am,
    async_mirror_defer_try_t *dt)
{
	uint_t nread = 0;

	for (uint_t r = 0; r < dt->dt_nrecs; r++) {
		const async_mirror_defer_rec_t *dr = &dt->dt_recs[r];
		boolean_t last = (dr->dr_tries >=
		    zfs_vdev_async_mirror_read_retries);

		for (uint_t i = dr->dr_first; i < dr->dr_first + dr->dr_count;
		    i++) {
			const async_mirror_defer_t *ad = &dt->dt_def[i];

			if (ad->ad_done)
				continue;

			uint64_t psize = BP_GET_PSIZE(&ad->ad_bp);
			uint64_t asize = async_mirror_bp_asize(dt->dt_tvd,
			    &ad->ad_bp);

			dt->dt_abds[i] = abd_alloc_for_io(asize, B_FALSE);
			if (asize > psize) {
				abd_zero_off(dt->dt_abds[i], psize,
				    asize - psize);
			}
			if (async_mirror_defer_read(am, ad, dt->dt_abds[i],
			    last) == 0) {
				dt->dt_got[i] = ASYNC_MIRROR_DEFER_READ;
				nread++;
			}
		}
	}
	return (nread);
}

/*
 * Write the blocks that were read, and flush the lagging child.  Not once
 * every record is released: the ranges can then be reused.  The release
 * waits for these writes.  *wrotep tells whether they were issued.
 */
static int
async_mirror_defer_try_write(vdev_async_mirror_t *am,
    async_mirror_defer_try_t *dt, boolean_t *wrotep)
{
	spa_t *spa = am->am_spa;
	int err = 0;

	spa_config_enter(spa, SCL_STATE_ALL, am, RW_READER);
	mutex_enter(&am->am_lock);
	*wrotep = !async_mirror_draining(am->am_state);
	if (*wrotep)
		am->am_defer_io = B_TRUE;
	mutex_exit(&am->am_lock);

	if (*wrotep) {
		zio_t *pio = zio_root(spa, NULL, NULL, ZIO_FLAG_CANFAIL);

		for (uint_t i = 0; i < dt->dt_n; i++) {
			if (dt->dt_got[i] != ASYNC_MIRROR_DEFER_READ)
				continue;
			async_mirror_defer_write(am, pio, &dt->dt_def[i],
			    dt->dt_abds[i], abd_get_size(dt->dt_abds[i]));
		}
		err = zio_wait(pio);
		if (err == 0) {
			pio = zio_root(spa, NULL, NULL, ZIO_FLAG_CANFAIL);
			zio_flush_leaf(pio, am->am_lagging);
			err = zio_wait(pio);
		}
	}
	spa_config_exit(spa, SCL_STATE_ALL, am);
	return (err);
}

/*
 * Mark the blocks of the try as done in their records, unless every record
 * was released meanwhile.  dt_got then says what happened to each block:
 * recovered or lost.  Return the blocks that wait for another try.
 */
static uint_t
async_mirror_defer_try_account(vdev_async_mirror_t *am,
    async_mirror_defer_try_t *dt, boolean_t wrote, int err)
{
	spa_async_mirror_stats_t *sams = &am->am_spa->spa_async_mirror_stats;
	uint_t recovered = 0, lost = 0, left = 0;

	mutex_enter(&am->am_lock);
	if (wrote) {
		am->am_defer_io = B_FALSE;
		cv_broadcast(&am->am_drain_cv);
	}
	if (err != 0) {
		/* The worker fails the copy; see async_mirror_thread(). */
		zfs_dbgmsg("async mirror: txg %llu: write of deferred data "
		    "blocks failed, error %d",
		    (u_longlong_t)dt->dt_recs[0].dr_txg, err);
		am->am_defer_failed = B_TRUE;
		cv_broadcast(&am->am_cv);
		wrote = B_FALSE;
	}

	async_mirror_txg_t *at = async_mirror_defer_next(am);

	for (uint_t r = 0; r < dt->dt_nrecs; r++) {
		const async_mirror_defer_rec_t *dr = &dt->dt_recs[r];
		boolean_t last = (dr->dr_tries >=
		    zfs_vdev_async_mirror_read_retries);
		uint_t rrecovered = 0, rlost = 0, rleft = 0;

		while (at != NULL && at->at_txg < dr->dr_txg)
			at = list_next(&am->am_txgs, at);
		if (at == NULL || at->at_txg != dr->dr_txg ||
		    at->at_ndefer == 0) {
			memset(&dt->dt_got[dr->dr_first], 0, dr->dr_count);
			continue;
		}
		ASSERT3U(at->at_ndefer_count, ==, dr->dr_count);

		uint_t i = dr->dr_first;

		for (async_mirror_defer_t *ad = list_head(&at->at_defer);
		    ad != NULL; ad = list_next(&at->at_defer, ad), i++) {
			if (ad->ad_done) {
				dt->dt_got[i] = ASYNC_MIRROR_DEFER_NONE;
			} else if (wrote &&
			    dt->dt_got[i] == ASYNC_MIRROR_DEFER_READ) {
				ad->ad_done = B_TRUE;
				dt->dt_got[i] = ASYNC_MIRROR_DEFER_RECOVERED;
				rrecovered++;
			} else if (last) {
				ad->ad_done = B_TRUE;
				dt->dt_got[i] = ASYNC_MIRROR_DEFER_LOST;
				rlost++;
			} else {
				dt->dt_got[i] = ASYNC_MIRROR_DEFER_NONE;
				rleft++;
			}
		}
		ASSERT3U(at->at_ndefer, ==, rrecovered + rlost + rleft);
		at->at_ndefer = rleft;
		at->at_defer_tries = dr->dr_tries + 1;
		recovered += rrecovered;
		lost += rlost;
		left += rleft;
	}
	if (recovered + lost + left > 0) {
		ASSERT3U(sams->sams_data_deferred, >=, recovered + lost);
		sams->sams_data_deferred -= recovered + lost;
		sams->sams_data_recovered += recovered;
		sams->sams_data_errors += lost;
		/* The error has passed: a new one gets its burst again. */
		if (recovered > 0)
			am->am_defer_quick = B_FALSE;
		async_mirror_defer_floor(am);
		async_mirror_update_stats(am);
	}
	mutex_exit(&am->am_lock);
	return (left);
}

/* Report the blocks that the try recovered or found lost. */
static void
async_mirror_defer_try_report(vdev_async_mirror_t *am,
    const async_mirror_defer_try_t *dt)
{
	for (uint_t r = 0; r < dt->dt_nrecs; r++) {
		const async_mirror_defer_rec_t *dr = &dt->dt_recs[r];

		for (uint_t i = dr->dr_first; i < dr->dr_first + dr->dr_count;
		    i++) {
			const async_mirror_defer_t *ad = &dt->dt_def[i];
			const zbookmark_phys_t *zb = &ad->ad_zb;

			if (dt->dt_got[i] == ASYNC_MIRROR_DEFER_RECOVERED) {
				/* The read that failed before logged it. */
				zbookmark_phys_t zbc = *zb;

				spa_remove_error(am->am_spa, &zbc,
				    BP_GET_PHYSICAL_BIRTH(&ad->ad_bp));
				async_mirror_recovered_event(am, dr->dr_txg,
				    ad);
			} else if (dt->dt_got[i] == ASYNC_MIRROR_DEFER_LOST) {
				zfs_dbgmsg("async mirror: txg %llu: deferred "
				    "data block <%llu, %llu, %lld, %llx> is "
				    "lost", (u_longlong_t)dr->dr_txg,
				    (u_longlong_t)zb->zb_objset,
				    (u_longlong_t)zb->zb_object,
				    (longlong_t)zb->zb_level,
				    (u_longlong_t)zb->zb_blkid);
			}
		}
	}
}

/*
 * Wait for the delay of the next try, unless the pool stops or every record
 * is released.
 */
static void
async_mirror_defer_wait(vdev_async_mirror_t *am, zthr_t *zthr)
{
	clock_t end = ddi_get_lbolt() +
	    MSEC_TO_TICK(ASYNC_MIRROR_RETRY_MS);

	while (!zthr_iscancelled(zthr)) {
		mutex_enter(&am->am_lock);
		clock_t now = ddi_get_lbolt();
		boolean_t wait = !async_mirror_draining(am->am_state) &&
		    now < end;

		if (wait) {
			(void) cv_timedwait_idle(&am->am_cv, &am->am_lock,
			    MIN(end, now + hz / 10 + 1));
		}
		mutex_exit(&am->am_lock);
		if (!wait)
			break;
	}
}

/*
 * One try of the deferred blocks of the records that the lagging child has
 * passed; see "Deferred blocks".  The release thread calls this.  A file
 * that cannot be read often defers blocks in many txgs, and the tries of
 * each record take zfs_vdev_async_mirror_read_retries times
 * ASYNC_MIRROR_RETRY_MS.  If the records took their tries one
 * after the other, the last record would wait for the tries of all of the
 * records before it, and the holds of all of them would stay that long.  So
 * one try takes the blocks of up to ASYNC_MIRROR_DEFER_RECS records, oldest
 * first, and each record counts its own tries.
 *
 * The holds keep the ranges of all of these blocks: a release stops at the
 * oldest record with blocks that are not done, and only this thread marks
 * blocks as done.  The try works on a copy of the lists, because a failure
 * can release every record meanwhile.  It writes the blocks that it read,
 * flushes the lagging child once, and only then marks them as done.  After a
 * try that leaves blocks, it waits for the delay of the next try.
 */
void
async_mirror_defer_retry(vdev_async_mirror_t *am, zthr_t *zthr)
{
	async_mirror_defer_try_t dt;
	boolean_t wrote = B_FALSE;
	int err = 0;

	mutex_enter(&am->am_lock);
	async_mirror_txg_t *first = async_mirror_defer_next(am);

	if (first == NULL) {
		mutex_exit(&am->am_lock);
		return;
	}
	async_mirror_defer_try_init(am, first, &dt);
	mutex_exit(&am->am_lock);

	if (async_mirror_defer_try_read(am, &dt) > 0)
		err = async_mirror_defer_try_write(am, &dt, &wrote);
	uint_t left = async_mirror_defer_try_account(am, &dt, wrote, err);
	async_mirror_defer_try_report(am, &dt);
	async_mirror_defer_try_fini(&dt);

	if (left > 0)
		async_mirror_defer_wait(am, zthr);
}
