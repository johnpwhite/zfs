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
 * Copyright (c) 2018 by Delphix. All rights reserved.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <libzfs_core.h>
#include <libzutil.h>

#include <sys/nvpair.h>
#include <sys/vdev_impl.h>
#include <sys/zfs_ioctl.h>
#include <sys/zfs_bootenv.h>
#include <sys/fs/zfs.h>
#include <sys/spa_parity_impl.h>

/*
 * Test the nvpair inputs for the non-legacy zfs ioctl commands.
 */

static boolean_t unexpected_failures;
static int zfs_fd;
static const char *active_test;

/*
 * Tracks which zfs_ioc_t commands were tested
 */
static boolean_t ioc_tested[ZFS_IOC_LAST - ZFS_IOC_FIRST];

/*
 * Legacy ioctls that are skipped (for now)
 */
static const zfs_ioc_t ioc_skip[] = {
	ZFS_IOC_POOL_CREATE,
	ZFS_IOC_POOL_DESTROY,
	ZFS_IOC_POOL_IMPORT,
	ZFS_IOC_POOL_EXPORT,
	ZFS_IOC_POOL_CONFIGS,
	ZFS_IOC_POOL_STATS,
	ZFS_IOC_POOL_TRYIMPORT,
	ZFS_IOC_POOL_SCAN,
	ZFS_IOC_POOL_FREEZE,
	ZFS_IOC_POOL_UPGRADE,
	ZFS_IOC_POOL_GET_HISTORY,

	ZFS_IOC_VDEV_ADD,
	ZFS_IOC_VDEV_REMOVE,
	ZFS_IOC_VDEV_SET_STATE,
	ZFS_IOC_VDEV_ATTACH,
	ZFS_IOC_VDEV_DETACH,
	ZFS_IOC_VDEV_SETPATH,
	ZFS_IOC_VDEV_SETFRU,

	ZFS_IOC_OBJSET_STATS,
	ZFS_IOC_OBJSET_ZPLPROPS,
	ZFS_IOC_DATASET_LIST_NEXT,
	ZFS_IOC_SNAPSHOT_LIST_NEXT,
	ZFS_IOC_SET_PROP,
	ZFS_IOC_DESTROY,
	ZFS_IOC_RENAME,
	ZFS_IOC_RECV,
	ZFS_IOC_SEND,
	ZFS_IOC_INJECT_FAULT,
	ZFS_IOC_CLEAR_FAULT,
	ZFS_IOC_INJECT_LIST_NEXT,
	ZFS_IOC_ERROR_LOG,
	ZFS_IOC_CLEAR,
	ZFS_IOC_PROMOTE,
	ZFS_IOC_DSOBJ_TO_DSNAME,
	ZFS_IOC_OBJ_TO_PATH,
	ZFS_IOC_POOL_SET_PROPS,
	ZFS_IOC_SET_FSACL,
	ZFS_IOC_GET_FSACL,
	ZFS_IOC_SHARE,
	ZFS_IOC_INHERIT_PROP,
	ZFS_IOC_SMB_ACL,
	ZFS_IOC_USERSPACE_ONE,
	ZFS_IOC_USERSPACE_MANY,
	ZFS_IOC_USERSPACE_UPGRADE,
	ZFS_IOC_OBJSET_RECVD_PROPS,
	ZFS_IOC_VDEV_SPLIT,
	ZFS_IOC_NEXT_OBJ,
	ZFS_IOC_DIFF,
	ZFS_IOC_TMP_SNAPSHOT,
	ZFS_IOC_OBJ_TO_STATS,
	ZFS_IOC_SPACE_WRITTEN,
	ZFS_IOC_POOL_REGUID,
	ZFS_IOC_SEND_PROGRESS,
	ZFS_IOC_EVENTS_NEXT,
	ZFS_IOC_EVENTS_CLEAR,
	ZFS_IOC_EVENTS_SEEK,
	ZFS_IOC_NEXTBOOT,
	ZFS_IOC_JAIL,
	ZFS_IOC_UNJAIL,
};


#define	IOC_INPUT_TEST(ioc, name, req, opt, err)		\
	IOC_INPUT_TEST_IMPL(ioc, name, req, opt, err, B_FALSE)

#define	IOC_INPUT_TEST_WILD(ioc, name, req, opt, err)		\
	IOC_INPUT_TEST_IMPL(ioc, name, req, opt, err, B_TRUE)

#define	IOC_INPUT_TEST_IMPL(ioc, name, req, opt, err, wild)	\
	do {							\
		active_test = __func__ + 5;			\
		ioc_tested[ioc - ZFS_IOC_FIRST] = B_TRUE;	\
		lzc_ioctl_test(ioc, name, req, opt, err, wild);	\
	} while (0)

#define	IOC_INPUT_TEST_INJECT(ioc, name, innvl)			\
	do {							\
		active_test = __func__ + 5;			\
		lzc_ioctl_run_impl(ioc, name, innvl, 0, B_TRUE);	\
	} while (0)

/*
 * Given a zfs_cmd_t containing an already packed nvlist in zc->zc_nvlist_src,
 * and its original innvl, look in innvl for the last string nvpair, or last
 * string array nvpair, and remove the string terminator.  The idea is to
 * corrupt the nvlist string value so that anyone doing a strlen() on it will
 * read past the end of the packed nvlist buffer and trigger a crash.
 */
static void
do_bad_string(zfs_cmd_t *zc, nvlist_t *innvl)
{
	nvpair_t *elem = NULL;
	nvpair_t *lastseen = NULL;
	const char *str = NULL;
	const char **arr;
	uint_t n;
	char *off;
	char *packed;
	uint64_t size, off_size;

	while ((elem = nvlist_next_nvpair(innvl, elem)) != NULL) {
		if ((nvpair_type(elem) == DATA_TYPE_STRING) ||
		    (nvpair_type(elem) == DATA_TYPE_STRING_ARRAY))
			lastseen = elem;
	}

	if (lastseen == NULL)
		return;	/* No strings */

	/*
	 * Lookup either the last string, or the last string in the last
	 * string array in the nvlist.  We will use this to corrupt from the
	 * string to the end of the nvlist buffer.  Any attempts to strlen this
	 * string should run pass the end of the packed buffer.
	 */
	if (nvpair_value_string(lastseen, &str) != 0) {
		if (nvpair_value_string_array(lastseen, &arr, &n) == 0)
			str = arr[n-1];
	}

	/*
	 * We now have the last string.  Corrupt everything from the NULL
	 * terminator byte for the last string to the end of the packed nvlist
	 * buffer.
	 */
	packed = (char *)(uintptr_t)zc->zc_nvlist_src;
	size = zc->zc_nvlist_src_size;

	off = memmem(packed, size, str, strlen(str));
	off_size = strlen(str);

	memset(&off[off_size - 1], '!', (packed + size) -
	    (&off[off_size - 1]));

}

/*
 * For each byte in the packed nvlist list in zc, corrupt a single byte, then
 * try doing the ioctl.  This tests how well the kernel handles fuzzed nvlists.
 *
 * NOTE - make sure you are doing this with a "safe" ioctl!  You don't want to
 * run this on an ioctl that can potentially corrupt data (like a zpool create).
 */
static void
do_fuzz(int zfs_fd, zfs_ioc_t ioc, zfs_cmd_t *zc)
{
	uint64_t size;
	uint64_t i;
	unsigned char old = 0;
	unsigned char *pos;
	zfs_cmd_t orig_zc = *zc;

	pos = (unsigned char *)(uintptr_t)zc->zc_nvlist_src;
	size = zc->zc_nvlist_src_size;

	/*
	 * Fuzz each byte in the packed nvlist, one byte at a time, and do the
	 * ioctl.  If the kernel doesn't crash, then the test passed.
	 */
	for (i = 0; i < size; i++) {
		/* Restore the previously corrupted byte */
		if (i > 0)
			pos[i-1] = old;

		old = pos[i];

		/* Corrupt the new byte */
		pos[i]++;

		/*
		 * Do the ioctl and ignore the return code.  We just want to
		 * see if the kernel panics.
		 */
		lzc_ioctl_fd(zfs_fd, ioc, zc);

		/*
		 * Restore 'zc' with original fields since the ioctl may
		 * have modified them.
		 */
		*zc = orig_zc;
	}
	/* Restore last byte */
	if (i > 0)
		pos[i - 1] = old;

	/*
	 * Try fuzzing the packed nvlist size field.  Test it with one byte
	 * bigger and one byte smaller than the current value.
	 */
	zc->zc_nvlist_src_size--;
	lzc_ioctl_fd(zfs_fd, ioc, zc);

	zc->zc_nvlist_src_size += 2;
	lzc_ioctl_fd(zfs_fd, ioc, zc);

	/* Restore to normal */
	zc->zc_nvlist_src_size -= 1;
}

/*
 * run a zfs ioctl command, verify expected results and log failures
 */
static void
lzc_ioctl_run_impl(zfs_ioc_t ioc, const char *name, nvlist_t *innvl,
    int expected, boolean_t do_corrupt)
{
	zfs_cmd_t zc = {"\0"};
	char *packed = NULL;
	const char *variant;
	size_t size = 0;
	int error = 0;

	switch (expected) {
	case ZFS_ERR_IOC_ARG_UNAVAIL:
		variant = "unsupported input";
		break;
	case ZFS_ERR_IOC_ARG_REQUIRED:
		variant = "missing input";
		break;
	case ZFS_ERR_IOC_ARG_BADTYPE:
		variant = "invalid input type";
		break;
	default:
		variant = "valid input";
		break;
	}

	packed = fnvlist_pack(innvl, &size);
	(void) strlcpy(zc.zc_name, name, sizeof (zc.zc_name));
	zc.zc_name[sizeof (zc.zc_name) - 1] = '\0';
	zc.zc_nvlist_src = (uint64_t)(uintptr_t)packed;
	zc.zc_nvlist_src_size = size;
	zc.zc_nvlist_dst_size = MAX(size * 2, 128 * 1024);
	zc.zc_nvlist_dst = (uint64_t)(uintptr_t)malloc(zc.zc_nvlist_dst_size);

	if (do_corrupt) {
		/*
		 * Try changing bytes in the packed nvlist to see if it will
		 * panic the kernel when you do the ioctl.
		 */
		do_fuzz(zfs_fd, ioc, &zc);

		/*
		 * Corrupt the last string in the packed nvlist so it has no
		 * NULL terminator.
		 */
		do_bad_string(&zc, innvl);

	}

	if (lzc_ioctl_fd(zfs_fd, ioc, &zc) != 0)
		error = errno;

	/*
	 * If we're corrupting the nvlist we don't care about the specific
	 * error code that gets returned, as it could be one of many.  We only
	 * care if it panics the kernel.
	 */
	if (!do_corrupt && error != expected) {
		unexpected_failures = B_TRUE;
		(void) fprintf(stderr, "%s: Unexpected result with %s, "
		    "error %d (expecting %d)\n",
		    active_test, variant, error, expected);
	}

	fnvlist_pack_free(packed, size);
	free((void *)(uintptr_t)zc.zc_nvlist_dst);
}

static void
lzc_ioctl_run(zfs_ioc_t ioc, const char *name, nvlist_t *innvl, int expected)
{
	return (lzc_ioctl_run_impl(ioc, name, innvl, expected, B_FALSE));
}

/*
 * Test each ioc for the following ioctl input errors:
 *   ZFS_ERR_IOC_ARG_UNAVAIL	an input argument is not supported by kernel
 *   ZFS_ERR_IOC_ARG_REQUIRED	a required input argument is missing
 *   ZFS_ERR_IOC_ARG_BADTYPE	an input argument has an invalid type
 */
static int
lzc_ioctl_test(zfs_ioc_t ioc, const char *name, nvlist_t *required,
    nvlist_t *optional, int expected_error, boolean_t wildcard)
{
	nvlist_t *input = fnvlist_alloc();
	nvlist_t *future = fnvlist_alloc();
	int error = 0;

	if (required != NULL) {
		for (nvpair_t *pair = nvlist_next_nvpair(required, NULL);
		    pair != NULL; pair = nvlist_next_nvpair(required, pair)) {
			fnvlist_add_nvpair(input, pair);
		}
	}
	if (optional != NULL) {
		for (nvpair_t *pair = nvlist_next_nvpair(optional, NULL);
		    pair != NULL; pair = nvlist_next_nvpair(optional, pair)) {
			fnvlist_add_nvpair(input, pair);
		}
	}

	/*
	 * Generic input run with 'optional' nvlist pair
	 */
	if (!wildcard)
		fnvlist_add_nvlist(input, "optional", future);
	lzc_ioctl_run(ioc, name, input, expected_error);
	if (!wildcard)
		fnvlist_remove(input, "optional");

	/*
	 * Bogus input value
	 */
	if (!wildcard) {
		fnvlist_add_string(input, "bogus_input", "bogus");
		lzc_ioctl_run(ioc, name, input, ZFS_ERR_IOC_ARG_UNAVAIL);
		fnvlist_remove(input, "bogus_input");
	}

	/*
	 * Missing required inputs
	 */
	if (required != NULL) {
		nvlist_t *empty = fnvlist_alloc();
		lzc_ioctl_run(ioc, name, empty, ZFS_ERR_IOC_ARG_REQUIRED);
		nvlist_free(empty);
	}

	/*
	 * Wrong nvpair type
	 */
	if (required != NULL || optional != NULL) {
		/*
		 * switch the type of one of the input pairs
		 */
		for (nvpair_t *pair = nvlist_next_nvpair(input, NULL);
		    pair != NULL; pair = nvlist_next_nvpair(input, pair)) {
			char pname[MAXNAMELEN];
			data_type_t ptype;

			strlcpy(pname, nvpair_name(pair), sizeof (pname));
			pname[sizeof (pname) - 1] = '\0';
			ptype = nvpair_type(pair);
			fnvlist_remove_nvpair(input, pair);

			switch (ptype) {
			case DATA_TYPE_STRING:
				fnvlist_add_uint64(input, pname, 42);
				break;
			default:
				fnvlist_add_string(input, pname, "bogus");
				break;
			}
		}
		lzc_ioctl_run(ioc, name, input, ZFS_ERR_IOC_ARG_BADTYPE);
	}

	nvlist_free(future);
	nvlist_free(input);

	return (error);
}

static void
test_pool_sync(const char *pool)
{
	nvlist_t *required = fnvlist_alloc();

	fnvlist_add_boolean_value(required, "force", B_TRUE);

	IOC_INPUT_TEST(ZFS_IOC_POOL_SYNC, pool, required, NULL, 0);

	nvlist_free(required);
}

static void
test_pool_reopen(const char *pool)
{
	nvlist_t *optional = fnvlist_alloc();

	fnvlist_add_boolean_value(optional, "scrub_restart", B_FALSE);

	IOC_INPUT_TEST(ZFS_IOC_POOL_REOPEN, pool, NULL, optional, 0);

	nvlist_free(optional);
}

static void
test_pool_checkpoint(const char *pool)
{
	IOC_INPUT_TEST(ZFS_IOC_POOL_CHECKPOINT, pool, NULL, NULL, 0);
}

static void
test_pool_discard_checkpoint(const char *pool)
{
	int err = lzc_pool_checkpoint(pool);
	if (err == 0 || err == ZFS_ERR_CHECKPOINT_EXISTS)
		IOC_INPUT_TEST(ZFS_IOC_POOL_DISCARD_CHECKPOINT, pool, NULL,
		    NULL, 0);
}

static void
test_log_history(const char *pool)
{
	nvlist_t *required = fnvlist_alloc();

	fnvlist_add_string(required, "message", "input check");

	IOC_INPUT_TEST(ZFS_IOC_LOG_HISTORY, pool, required, NULL, 0);
	IOC_INPUT_TEST_INJECT(ZFS_IOC_LOG_HISTORY, pool, required);

	nvlist_free(required);
}

static void
test_create(const char *pool)
{
	char dataset[MAXNAMELEN + 32];

	(void) snprintf(dataset, sizeof (dataset), "%s/create-fs", pool);

	nvlist_t *required = fnvlist_alloc();
	nvlist_t *optional = fnvlist_alloc();
	nvlist_t *props = fnvlist_alloc();

	fnvlist_add_int32(required, "type", DMU_OST_ZFS);
	fnvlist_add_uint64(props, "recordsize", 8192);
	fnvlist_add_nvlist(optional, "props", props);

	IOC_INPUT_TEST(ZFS_IOC_CREATE, dataset, required, optional, 0);

	nvlist_free(required);
	nvlist_free(optional);
}

static void
test_snapshot(const char *pool, const char *snapshot)
{
	nvlist_t *required = fnvlist_alloc();
	nvlist_t *optional = fnvlist_alloc();
	nvlist_t *snaps = fnvlist_alloc();
	nvlist_t *props = fnvlist_alloc();

	fnvlist_add_boolean(snaps, snapshot);
	fnvlist_add_nvlist(required, "snaps", snaps);

	fnvlist_add_string(props, "org.openzfs:launch", "September 17th, 2013");
	fnvlist_add_nvlist(optional, "props", props);

	IOC_INPUT_TEST(ZFS_IOC_SNAPSHOT, pool, required, optional, 0);

	nvlist_free(props);
	nvlist_free(snaps);
	nvlist_free(optional);
	nvlist_free(required);
}

static void
test_space_snaps(const char *snapshot)
{
	nvlist_t *required = fnvlist_alloc();
	fnvlist_add_string(required, "firstsnap", snapshot);

	IOC_INPUT_TEST(ZFS_IOC_SPACE_SNAPS, snapshot, required, NULL, 0);

	nvlist_free(required);
}

static void
test_destroy_snaps(const char *pool, const char *snapshot)
{
	nvlist_t *required = fnvlist_alloc();
	nvlist_t *snaps = fnvlist_alloc();

	fnvlist_add_boolean(snaps, snapshot);
	fnvlist_add_nvlist(required, "snaps", snaps);

	IOC_INPUT_TEST(ZFS_IOC_DESTROY_SNAPS, pool, required, NULL, 0);

	nvlist_free(snaps);
	nvlist_free(required);
}


static void
test_bookmark(const char *pool, const char *snapshot, const char *bookmark)
{
	nvlist_t *required = fnvlist_alloc();

	fnvlist_add_string(required, bookmark, snapshot);

	IOC_INPUT_TEST_WILD(ZFS_IOC_BOOKMARK, pool, required, NULL, 0);

	nvlist_free(required);
}

static void
test_get_bookmarks(const char *dataset)
{
	nvlist_t *optional = fnvlist_alloc();

	fnvlist_add_boolean(optional, "guid");
	fnvlist_add_boolean(optional, "createtxg");
	fnvlist_add_boolean(optional, "creation");

	IOC_INPUT_TEST_WILD(ZFS_IOC_GET_BOOKMARKS, dataset, NULL, optional, 0);

	nvlist_free(optional);
}

static void
test_destroy_bookmarks(const char *pool, const char *bookmark)
{
	nvlist_t *required = fnvlist_alloc();

	fnvlist_add_boolean(required, bookmark);

	IOC_INPUT_TEST_WILD(ZFS_IOC_DESTROY_BOOKMARKS, pool, required, NULL, 0);

	nvlist_free(required);
}

static void
test_clone(const char *snapshot, const char *clone)
{
	nvlist_t *required = fnvlist_alloc();
	nvlist_t *optional = fnvlist_alloc();
	nvlist_t *props = fnvlist_alloc();

	fnvlist_add_string(required, "origin", snapshot);

	IOC_INPUT_TEST(ZFS_IOC_CLONE, clone, required, NULL, 0);

	nvlist_free(props);
	nvlist_free(optional);
	nvlist_free(required);
}

static void
test_rollback(const char *dataset, const char *snapshot)
{
	nvlist_t *optional = fnvlist_alloc();

	fnvlist_add_string(optional, "target", snapshot);

	IOC_INPUT_TEST(ZFS_IOC_ROLLBACK, dataset, NULL, optional, B_FALSE);

	nvlist_free(optional);
}

static void
test_hold(const char *pool, const char *snapshot)
{
	nvlist_t *required = fnvlist_alloc();
	nvlist_t *optional = fnvlist_alloc();
	nvlist_t *holds = fnvlist_alloc();

	fnvlist_add_string(holds, snapshot, "libzfs_check_hold");
	fnvlist_add_nvlist(required, "holds", holds);
	fnvlist_add_int32(optional, "cleanup_fd", zfs_fd);

	IOC_INPUT_TEST(ZFS_IOC_HOLD, pool, required, optional, 0);

	nvlist_free(holds);
	nvlist_free(optional);
	nvlist_free(required);
}

static void
test_get_holds(const char *snapshot)
{
	IOC_INPUT_TEST(ZFS_IOC_GET_HOLDS, snapshot, NULL, NULL, 0);
}

static void
test_release(const char *pool, const char *snapshot)
{
	nvlist_t *required = fnvlist_alloc();
	nvlist_t *release = fnvlist_alloc();

	fnvlist_add_boolean(release, "libzfs_check_hold");
	fnvlist_add_nvlist(required, snapshot, release);

	IOC_INPUT_TEST_WILD(ZFS_IOC_RELEASE, pool, required, NULL, 0);

	nvlist_free(release);
	nvlist_free(required);
}


static void
test_send_new(const char *snapshot, int fd)
{
	nvlist_t *required = fnvlist_alloc();
	nvlist_t *optional = fnvlist_alloc();

	fnvlist_add_int32(required, "fd", fd);

	fnvlist_add_boolean(optional, "largeblockok");
	fnvlist_add_boolean(optional, "embedok");
	fnvlist_add_boolean(optional, "compressok");
	fnvlist_add_boolean(optional, "rawok");

	/*
	 * TODO - Resumable send is harder to set up. So we currently
	 * ignore testing for that variant.
	 */
#if 0
	fnvlist_add_string(optional, "fromsnap", from);
	fnvlist_add_uint64(optional, "resume_object", resumeobj);
	fnvlist_add_uint64(optional, "resume_offset", offset);
	fnvlist_add_boolean(optional, "savedok");
#endif
	IOC_INPUT_TEST(ZFS_IOC_SEND_NEW, snapshot, required, optional, 0);

	nvlist_free(optional);
	nvlist_free(required);
}

static void
test_recv_new(const char *dataset, int fd)
{
	dmu_replay_record_t drr;
	nvlist_t *required = fnvlist_alloc();
	nvlist_t *optional = fnvlist_alloc();
	nvlist_t *props = fnvlist_alloc();
	char snapshot[MAXNAMELEN + 32];
	ssize_t count;

	memset(&drr, 0, sizeof (dmu_replay_record_t));

	int cleanup_fd = open(ZFS_DEV, O_RDWR);
	if (cleanup_fd == -1) {
		(void) fprintf(stderr, "open(%s) failed: %s\n", ZFS_DEV,
		    strerror(errno));
		exit(EXIT_FAILURE);
	}
	(void) snprintf(snapshot, sizeof (snapshot), "%s@replicant", dataset);

	count = pread(fd, &drr, sizeof (drr), 0);
	if (count != sizeof (drr)) {
		(void) fprintf(stderr, "could not read stream: %s\n",
		    strerror(errno));
	}

	fnvlist_add_string(required, "snapname", snapshot);
	fnvlist_add_byte_array(required, "begin_record", (uchar_t *)&drr,
	    sizeof (drr));
	fnvlist_add_int32(required, "input_fd", fd);

	fnvlist_add_string(props, "org.openzfs:launch", "September 17th, 2013");
	fnvlist_add_nvlist(optional, "localprops", props);
	fnvlist_add_boolean(optional, "force");
	fnvlist_add_boolean(optional, "heal");
	fnvlist_add_int32(optional, "cleanup_fd", cleanup_fd);

	/*
	 * TODO - Resumable receive is harder to set up. So we currently
	 * ignore testing for one.
	 */
#if 0
	fnvlist_add_nvlist(optional, "props", recvdprops);
	fnvlist_add_string(optional, "origin", origin);
	fnvlist_add_boolean(optional, "resumable");
	fnvlist_add_uint64(optional, "action_handle", *action_handle);
#endif
	IOC_INPUT_TEST(ZFS_IOC_RECV_NEW, dataset, required, optional,
	    ENOTSUP);

	nvlist_free(props);
	nvlist_free(optional);
	nvlist_free(required);

	(void) close(cleanup_fd);
}

static void
test_send_space(const char *snapshot1, const char *snapshot2)
{
	nvlist_t *optional = fnvlist_alloc();

	fnvlist_add_string(optional, "from", snapshot1);
	fnvlist_add_boolean(optional, "largeblockok");
	fnvlist_add_boolean(optional, "embedok");
	fnvlist_add_boolean(optional, "compressok");
	fnvlist_add_boolean(optional, "rawok");

	IOC_INPUT_TEST(ZFS_IOC_SEND_SPACE, snapshot2, NULL, optional, 0);

	nvlist_free(optional);
}

static void
test_remap(const char *dataset)
{
	IOC_INPUT_TEST(ZFS_IOC_REMAP, dataset, NULL, NULL, 0);
}

static void
test_channel_program(const char *pool)
{
	const char *program =
	    "arg = ...\n"
	    "argv = arg[\"argv\"]\n"
	    "return argv[1]";
	const char *const argv[1] = { "Hello World!" };
	nvlist_t *required = fnvlist_alloc();
	nvlist_t *optional = fnvlist_alloc();
	nvlist_t *args = fnvlist_alloc();

	fnvlist_add_string(required, "program", program);
	fnvlist_add_string_array(args, "argv", argv, 1);
	fnvlist_add_nvlist(required, "arg", args);

	fnvlist_add_boolean_value(optional, "sync", B_TRUE);
	fnvlist_add_uint64(optional, "instrlimit", 1000 * 1000);
	fnvlist_add_uint64(optional, "memlimit", 8192 * 1024);

	IOC_INPUT_TEST(ZFS_IOC_CHANNEL_PROGRAM, pool, required, optional, 0);

	nvlist_free(args);
	nvlist_free(optional);
	nvlist_free(required);
}

#define	WRAPPING_KEY_LEN	32

static void
test_load_key(const char *dataset)
{
	nvlist_t *required = fnvlist_alloc();
	nvlist_t *optional = fnvlist_alloc();
	nvlist_t *hidden = fnvlist_alloc();
	uint8_t keydata[WRAPPING_KEY_LEN] = {0};

	fnvlist_add_uint8_array(hidden, "wkeydata", keydata, sizeof (keydata));
	fnvlist_add_nvlist(required, "hidden_args", hidden);
	fnvlist_add_boolean(optional, "noop");

	IOC_INPUT_TEST(ZFS_IOC_LOAD_KEY, dataset, required, optional, EINVAL);
	nvlist_free(hidden);
	nvlist_free(optional);
	nvlist_free(required);
}

static void
test_change_key(const char *dataset)
{
	IOC_INPUT_TEST(ZFS_IOC_CHANGE_KEY, dataset, NULL, NULL, EINVAL);
}

static void
test_unload_key(const char *dataset)
{
	IOC_INPUT_TEST(ZFS_IOC_UNLOAD_KEY, dataset, NULL, NULL, EACCES);
}

static void
test_vdev_initialize(const char *pool)
{
	nvlist_t *required = fnvlist_alloc();
	nvlist_t *optional = fnvlist_alloc();
	nvlist_t *vdev_guids = fnvlist_alloc();

	fnvlist_add_uint64(vdev_guids, "path", 0xdeadbeefdeadbeef);
	fnvlist_add_uint64(required, ZPOOL_INITIALIZE_COMMAND,
	    POOL_INITIALIZE_START);
	fnvlist_add_nvlist(required, ZPOOL_INITIALIZE_VDEVS, vdev_guids);
	fnvlist_add_uint64(optional, ZPOOL_INITIALIZE_VALUE, 0);

	IOC_INPUT_TEST(ZFS_IOC_POOL_INITIALIZE, pool, required, optional,
	    EINVAL);
	nvlist_free(vdev_guids);
	nvlist_free(optional);
	nvlist_free(required);
}

static void
test_vdev_trim(const char *pool)
{
	nvlist_t *required = fnvlist_alloc();
	nvlist_t *optional = fnvlist_alloc();
	nvlist_t *vdev_guids = fnvlist_alloc();

	fnvlist_add_uint64(vdev_guids, "path", 0xdeadbeefdeadbeef);
	fnvlist_add_uint64(required, ZPOOL_TRIM_COMMAND, POOL_TRIM_START);
	fnvlist_add_nvlist(required, ZPOOL_TRIM_VDEVS, vdev_guids);
	fnvlist_add_uint64(optional, ZPOOL_TRIM_RATE, 1ULL << 30);
	fnvlist_add_boolean_value(optional, ZPOOL_TRIM_SECURE, B_TRUE);

	IOC_INPUT_TEST(ZFS_IOC_POOL_TRIM, pool, required, optional, EINVAL);
	nvlist_free(vdev_guids);
	nvlist_free(optional);
	nvlist_free(required);
}

/* Test with invalid values */
static void
test_scrub(const char *pool)
{
	nvlist_t *required = fnvlist_alloc();
	nvlist_t *optional = fnvlist_alloc();

	fnvlist_add_uint64(required, "scan_type", POOL_SCAN_FUNCS + 1);
	fnvlist_add_uint64(required, "scan_command", POOL_SCRUB_FLAGS_END + 1);
	IOC_INPUT_TEST(ZFS_IOC_POOL_SCRUB, pool, required, NULL, EINVAL);
	nvlist_free(required);

	required = fnvlist_alloc();
	fnvlist_add_uint64(required, "scan_type", POOL_SCAN_SCRUB);
	fnvlist_add_uint64(required, "scan_command", POOL_SCRUB_NORMAL);
	fnvlist_add_uint64(optional, "scan_flags", POOL_SCRUB_THOROUGH << 1);
	IOC_INPUT_TEST(ZFS_IOC_POOL_SCRUB, pool, required, optional, EINVAL);
	nvlist_free(optional);
	nvlist_free(required);
}

static int
zfs_destroy(const char *dataset)
{
	zfs_cmd_t zc = {"\0"};
	int err;

	(void) strlcpy(zc.zc_name, dataset, sizeof (zc.zc_name));
	zc.zc_name[sizeof (zc.zc_name) - 1] = '\0';
	err = lzc_ioctl_fd(zfs_fd, ZFS_IOC_DESTROY, &zc);

	return (err == 0 ? 0 : errno);
}

static void
test_redact(const char *snapshot1, const char *snapshot2)
{
	nvlist_t *required = fnvlist_alloc();
	nvlist_t *snapnv = fnvlist_alloc();
	char bookmark[MAXNAMELEN + 32];

	fnvlist_add_string(required, "bookname", "testbookmark");
	fnvlist_add_boolean(snapnv, snapshot2);
	fnvlist_add_nvlist(required, "snapnv", snapnv);

	IOC_INPUT_TEST(ZFS_IOC_REDACT, snapshot1, required, NULL, 0);

	nvlist_free(snapnv);
	nvlist_free(required);

	strlcpy(bookmark, snapshot1, sizeof (bookmark));
	*strchr(bookmark, '@') = '\0';
	strlcat(bookmark, "#testbookmark", sizeof (bookmark) -
	    strlen(bookmark));
	zfs_destroy(bookmark);
}

static void
test_get_bookmark_props(const char *bookmark)
{
	IOC_INPUT_TEST(ZFS_IOC_GET_BOOKMARK_PROPS, bookmark, NULL, NULL, 0);
}

static void
test_wait(const char *pool)
{
	nvlist_t *required = fnvlist_alloc();
	nvlist_t *optional = fnvlist_alloc();

	fnvlist_add_int32(required, "wait_activity", 2);
	fnvlist_add_uint64(optional, "wait_tag", 0xdeadbeefdeadbeef);

	IOC_INPUT_TEST(ZFS_IOC_WAIT, pool, required, optional, EINVAL);

	nvlist_free(required);
	nvlist_free(optional);
}

static void
test_wait_fs(const char *dataset)
{
	nvlist_t *required = fnvlist_alloc();

	fnvlist_add_int32(required, "wait_activity", 2);

	IOC_INPUT_TEST(ZFS_IOC_WAIT_FS, dataset, required, NULL, EINVAL);

	nvlist_free(required);
}

static void
test_get_bootenv(const char *pool)
{
	IOC_INPUT_TEST(ZFS_IOC_GET_BOOTENV, pool, NULL, NULL, 0);
}

static void
test_set_bootenv(const char *pool)
{
	nvlist_t *required = fnvlist_alloc();

	fnvlist_add_uint64(required, "version", VB_RAW);
	fnvlist_add_string(required, GRUB_ENVMAP, "test");

	IOC_INPUT_TEST_WILD(ZFS_IOC_SET_BOOTENV, pool, required, NULL, 0);

	nvlist_free(required);
}


/*
 * The parity set ioctls. The test pool is not a member of a parity set, so
 * INFO succeeds and pool operations return ENOTSUP. Provider registration
 * and supply use the same file descriptor in this test.
 */
static void
test_parity_info(const char *pool)
{
	nvlist_t *optional = fnvlist_alloc();

	fnvlist_add_uint64(optional, "txg", 0);

	IOC_INPUT_TEST(ZFS_IOC_PARITY_INFO, pool, NULL, optional, 0);
	IOC_INPUT_TEST_INJECT(ZFS_IOC_PARITY_INFO, pool, optional);

	nvlist_free(optional);
}

/*
 * Check the data columns in INFO.  The test supplies the expected number of
 * data columns and every expected data leaf path.  A log device is therefore
 * excluded when all data leaves are present and the column count matches.
 */
static int
test_parity_columns(const char *pool, uint_t expected_columns,
    const char *const *expected_paths, uint_t nexpected_paths)
{
	nvlist_t *info = NULL;
	nvlist_t **columns;
	boolean_t *found = NULL;
	uint_t ncolumns = 0;
	int error;
	int result = 1;

	if (nexpected_paths == 0) {
		(void) fprintf(stderr, "no expected parity leaf paths\n");
		return (1);
	}

	error = lzc_parity_info(pool, &info);
	if (error != 0) {
		(void) fprintf(stderr, "parity INFO failed: %s\n",
		    strerror(error));
		return (1);
	}

	if (nvlist_lookup_nvlist_array(info, ZPARITY_COLUMNS, &columns,
	    &ncolumns) != 0 || ncolumns != expected_columns) {
		(void) fprintf(stderr,
		    "parity INFO returned %u columns, expected %u\n",
		    ncolumns, expected_columns);
		goto out;
	}
	found = calloc(nexpected_paths, sizeof (boolean_t));
	if (found == NULL) {
		(void) fprintf(stderr,
		    "cannot allocate parity INFO check state\n");
		goto out;
	}

	for (uint_t c = 0; c < ncolumns; c++) {
		uint64_t vdev;
		uint64_t guid;
		uint64_t asize;
		uint64_t ashift;
		uint64_t *leaf_guids;
		uint64_t *leaf_sizes;
		boolean_t *leaf_insync;
		char **leaf_paths;
		const char *type;
		uint_t nguids;
		uint_t nsizes;
		uint_t ninsync;
		uint_t npaths;

		if (nvlist_lookup_uint64(columns[c], ZPARITY_COL_VDEV,
		    &vdev) != 0 ||
		    nvlist_lookup_uint64(columns[c], ZPARITY_COL_GUID,
		    &guid) != 0 || guid == 0 ||
		    nvlist_lookup_string(columns[c], ZPARITY_COL_TYPE,
		    &type) != 0 || type == NULL || type[0] == '\0' ||
		    nvlist_lookup_uint64(columns[c], ZPARITY_ASIZE,
		    &asize) != 0 || asize == 0 ||
		    nvlist_lookup_uint64(columns[c], ZPARITY_COL_ASHIFT,
		    &ashift) != 0 ||
		    nvlist_lookup_uint64_array(columns[c],
		    ZPARITY_COL_LEAF_GUIDS, &leaf_guids, &nguids) != 0 ||
		    nvlist_lookup_uint64_array(columns[c],
		    ZPARITY_COL_LEAF_SIZES, &leaf_sizes, &nsizes) != 0 ||
		    nvlist_lookup_string_array(columns[c],
		    ZPARITY_COL_LEAF_PATHS, &leaf_paths, &npaths) != 0 ||
		    nvlist_lookup_boolean_array(columns[c],
		    ZPARITY_COL_LEAF_INSYNC, &leaf_insync, &ninsync) != 0 ||
		    nguids == 0 || nguids != nsizes || nguids != npaths ||
		    nguids != ninsync) {
			(void) fprintf(stderr,
			    "parity INFO column %u is incomplete\n", c);
			goto out;
		}
		(void) vdev;
		(void) ashift;
		for (uint_t i = 0; i < nguids; i++) {
			if (leaf_guids[i] == 0 || leaf_sizes[i] == 0 ||
			    leaf_paths[i] == NULL || leaf_paths[i][0] == '\0' ||
			    !leaf_insync[i]) {
				(void) fprintf(stderr, "parity INFO column %u "
				    "leaf %u is incomplete or out of sync\n",
				    c, i);
				goto out;
			}
			for (uint_t p = 0; p < nexpected_paths; p++) {
				if (strcmp(leaf_paths[i],
				    expected_paths[p]) == 0)
					found[p] = B_TRUE;
			}
		}
	}

	for (uint_t p = 0; p < nexpected_paths; p++) {
		if (!found[p]) {
			(void) fprintf(stderr,
			    "parity INFO does not list leaf '%s'\n",
			    expected_paths[p]);
			goto out;
		}
	}
	result = 0;
out:
	free(found);
	fnvlist_free(info);
	return (result);
}

static void
test_parity_records(const char *pool)
{
	nvlist_t *required = fnvlist_alloc();

	fnvlist_add_uint64(required, "from_txg", 0);
	fnvlist_add_uint64(required, "cursor", 0);
	fnvlist_add_uint64(required, "max_entries", 16);

	IOC_INPUT_TEST(ZFS_IOC_PARITY_RECORDS, pool, required, NULL, ENOTSUP);
	IOC_INPUT_TEST_INJECT(ZFS_IOC_PARITY_RECORDS, pool, required);

	nvlist_free(required);
}

static void
test_parity_read(const char *pool)
{
	nvlist_t *required = fnvlist_alloc();
	nvlist_t *optional = fnvlist_alloc();
	uint64_t entry[18] = { 0 };

	fnvlist_add_uint64_array(required, "entry", entry, 18);
	fnvlist_add_uint64(optional, "index", 0);

	IOC_INPUT_TEST(ZFS_IOC_PARITY_READ, pool, required, optional,
	    ENOTSUP);
	IOC_INPUT_TEST_INJECT(ZFS_IOC_PARITY_READ, pool, required);

	nvlist_free(required);
	nvlist_free(optional);
}

static void
test_parity_verify(const char *pool)
{
	nvlist_t *required = fnvlist_alloc();
	nvlist_t *optional = fnvlist_alloc();
	uint64_t entry[18] = { 0 };
	uchar_t data[512] = { 0 };

	fnvlist_add_uint64_array(required, "entry", entry, 18);
	fnvlist_add_uint8_array(required, "data", data, sizeof (data));
	fnvlist_add_uint64(optional, "index", 0);

	IOC_INPUT_TEST(ZFS_IOC_PARITY_VERIFY, pool, required, optional,
	    ENOTSUP);
	IOC_INPUT_TEST_INJECT(ZFS_IOC_PARITY_VERIFY, pool, required);

	nvlist_free(required);
	nvlist_free(optional);
}

static void
test_parity_map(const char *pool)
{
	nvlist_t *required = fnvlist_alloc();

	fnvlist_add_uint64(required, "txg", 0);
	fnvlist_add_uint64(required, "from_offset", 0);
	fnvlist_add_uint64(required, "max_ranges", 16);

	IOC_INPUT_TEST(ZFS_IOC_PARITY_MAP, pool, required, NULL, ENOTSUP);
	IOC_INPUT_TEST_INJECT(ZFS_IOC_PARITY_MAP, pool, required);

	nvlist_free(required);
}

static void
test_parity_blocks(const char *pool)
{
	nvlist_t *required = fnvlist_alloc();
	nvlist_t *optional = fnvlist_alloc();
	uint64_t windows[4] = { 0, 4096, 8192, 12288 };
	uint64_t resume[PARITY_BLOCKS_RESUME_WORDS] = { 0 };

	fnvlist_add_uint64(required, "txg", 0);
	fnvlist_add_uint64(required, "from_offset", 0);
	fnvlist_add_uint64(required, "max_entries", 16);
	fnvlist_add_uint64_array(optional, ZPARITY_WINDOWS, windows,
	    ARRAY_SIZE(windows));
	fnvlist_add_uint64_array(optional, ZPARITY_RESUME, resume,
	    ARRAY_SIZE(resume));

	IOC_INPUT_TEST(ZFS_IOC_PARITY_BLOCKS, pool, required, optional,
	    ENOTSUP);
	IOC_INPUT_TEST_INJECT(ZFS_IOC_PARITY_BLOCKS, pool, required);

	nvlist_free(required);
	nvlist_free(optional);
}

static void
test_parity_label(const char *pool)
{
	nvlist_t *required = fnvlist_alloc();

	fnvlist_add_uint64(required, "txg", 0);

	IOC_INPUT_TEST(ZFS_IOC_PARITY_LABEL, pool, required, NULL, ENOTSUP);
	IOC_INPUT_TEST_INJECT(ZFS_IOC_PARITY_LABEL, pool, required);

	nvlist_free(required);
}

static void
test_parity_applied(const char *pool)
{
	nvlist_t *required = fnvlist_alloc();

	fnvlist_add_uint64(required, "set_guid", 1);
	fnvlist_add_uint64(required, "generation", 1);
	fnvlist_add_uint64(required, "txg", 0);

	IOC_INPUT_TEST(ZFS_IOC_PARITY_APPLIED, pool, required, NULL, ENOTSUP);
	IOC_INPUT_TEST_INJECT(ZFS_IOC_PARITY_APPLIED, pool, required);

	nvlist_free(required);
}

static void
test_parity_repair(const char *pool)
{
	nvlist_t *required = fnvlist_alloc();
	uchar_t data[512] = { 0 };

	fnvlist_add_uint64(required, "request", 1);
	fnvlist_add_uint8_array(required, "data", data, sizeof (data));

	IOC_INPUT_TEST(ZFS_IOC_PARITY_REPAIR, pool, required, NULL, ENOTSUP);
	IOC_INPUT_TEST_INJECT(ZFS_IOC_PARITY_REPAIR, pool, required);

	nvlist_free(required);
}

static void
test_parity_provider(void)
{
	nvlist_t *empty = fnvlist_alloc();

	IOC_INPUT_TEST(ZFS_IOC_PARITY_PROVIDER, "", NULL, NULL, 0);
	IOC_INPUT_TEST_INJECT(ZFS_IOC_PARITY_PROVIDER, "", empty);
	nvlist_free(empty);
}

static void
test_parity_supply(void)
{
	nvlist_t *required = fnvlist_alloc();
	uint8_t data = 0;

	fnvlist_add_uint64(required, ZPARITY_POOL_GUID, 0);
	fnvlist_add_uint64(required, ZPARITY_REQUEST, 1);
	fnvlist_add_uint8_array(required, ZPARITY_DATA, &data, 1);
	IOC_INPUT_TEST(ZFS_IOC_PARITY_SUPPLY, "", required, NULL, ENOENT);
	IOC_INPUT_TEST_INJECT(ZFS_IOC_PARITY_SUPPLY, "", required);
	nvlist_free(required);
}

static void
test_parity_drop(const char *pool)
{
	nvlist_t *required = fnvlist_alloc();

	fnvlist_add_uint64(required, "set_guid", 1);
	fnvlist_add_uint64(required, "generation", 1);

	IOC_INPUT_TEST(ZFS_IOC_PARITY_DROP, pool, required, NULL, ENOTSUP);
	IOC_INPUT_TEST_INJECT(ZFS_IOC_PARITY_DROP, pool, required);

	nvlist_free(required);
}

static void
test_parity_reclaim(const char *pool)
{
	nvlist_t *required = fnvlist_alloc();
	nvlist_t *optional = fnvlist_alloc();
	uint64_t ranges[3] = { 0, 0, 4096 };

	fnvlist_add_uint64(required, ZPARITY_POOL_GUID, 0);
	fnvlist_add_uint64(required, ZPARITY_TXG, 1);
	fnvlist_add_uint64(required, ZPARITY_RECLAIM_FLAGS,
	    ZPARITY_RECLAIM_STAGE);
	fnvlist_add_uint64_array(optional, "ranges", ranges, 3);

	IOC_INPUT_TEST(ZFS_IOC_PARITY_RECLAIM, pool, required, optional,
	    EINVAL);
	IOC_INPUT_TEST_INJECT(ZFS_IOC_PARITY_RECLAIM, pool, required);

	nvlist_free(optional);
	nvlist_free(required);
}

static void
test_parity_readv(const char *pool)
{
	nvlist_t *required = fnvlist_alloc();
	nvlist_t *optional = fnvlist_alloc();
	uint64_t entries[ZPARITY_ENTRY_WORDS] = { 0 };
	uint64_t indexes[1] = { 0 };

	fnvlist_add_uint64_array(required, ZPARITY_ENTRIES, entries,
	    ARRAY_SIZE(entries));
	fnvlist_add_uint64_array(optional, ZPARITY_INDEXES, indexes,
	    ARRAY_SIZE(indexes));
	IOC_INPUT_TEST(ZFS_IOC_PARITY_READV, pool, required, optional, ENOTSUP);
	IOC_INPUT_TEST_INJECT(ZFS_IOC_PARITY_READV, pool, required);
	nvlist_free(optional);
	nvlist_free(required);
}

static void
test_parity_current(const char *pool)
{
	nvlist_t *required = fnvlist_alloc();

	fnvlist_add_uint64(required, ZPARITY_PHYS_OFF, 0);
	IOC_INPUT_TEST(ZFS_IOC_PARITY_CURRENT, pool, required, NULL, ENOTSUP);
	IOC_INPUT_TEST_INJECT(ZFS_IOC_PARITY_CURRENT, pool, required);
	nvlist_free(required);
}

static void
parity_malformed_input(const char *what, zfs_ioc_t ioc, const char *pool,
    nvlist_t *input, int expected)
{
	active_test = what;
	lzc_ioctl_run(ioc, pool, input, expected);
}

static int
test_parity_indexed_asize(const char *pool)
{
	nvlist_t *out = NULL, *rd = NULL;
	uint64_t *entries, *indexes;
	uint_t nwords = 0, nindexes = 0;
	int error, result = 1;

	error = lzc_parity_records(pool, 0, 0, 64, &out);
	if (error != 0) {
		(void) fprintf(stderr, "parity RECORDS failed: %s\n",
		    strerror(error));
		goto out;
	}
	entries = fnvlist_lookup_uint64_array(out, ZPARITY_ENTRIES, &nwords);
	indexes = fnvlist_lookup_uint64_array(out, ZPARITY_INDEXES, &nindexes);
	if (entries == NULL || indexes == NULL ||
	    nwords % PARITY_ENTRY_WORDS != 0 ||
	    nindexes != nwords / PARITY_ENTRY_WORDS) {
		(void) fprintf(stderr,
		    "parity RECORDS returned invalid entries\n");
		goto out;
	}

	for (uint_t i = 0; i < nindexes; i++) {
		uint64_t *entry = &entries[i * PARITY_ENTRY_WORDS];
		uint64_t kind = entry[PARITY_EW_KIND];
		uint64_t copy[PARITY_ENTRY_WORDS];
		uint64_t index = indexes[i];
		uint64_t dvai = (kind >> 16) & 0xffff;
		boolean_t got_data;
		blkptr_t *bp;
		dva_t *dva;

		if ((kind & 0xff) != PARITY_KIND_ALLOC ||
		    ((kind >> 8) & 3) != 0)
			continue;
		memcpy(copy, entry, sizeof (copy));
		bp = (blkptr_t *)&copy[PARITY_EW_BP];
		/* not BP_GET_NDVAS: it reads the kernel's dmu_ot table */
		if (dvai >= SPA_DVAS_PER_BP ||
		    DVA_GET_ASIZE(&bp->blk_dva[dvai]) == 0)
			continue;
		dva = &bp->blk_dva[dvai];
		if (DVA_GET_ASIZE(dva) > UINT64_MAX - SPA_MINBLOCKSIZE)
			continue;
		DVA_SET_ASIZE(dva, DVA_GET_ASIZE(dva) + SPA_MINBLOCKSIZE);
		copy[PARITY_EW_KIND] += 1ULL << 32;

		error = lzc_parity_read_index(pool, copy, &index, &rd);
		got_data = (rd != NULL);
		if (rd != NULL) {
			fnvlist_free(rd);
			rd = NULL;
		}
		if (error != ENOENT || got_data) {
			(void) fprintf(stderr, "indexed READ with a changed "
			    "DVA asize returned %d, not ENOENT\n", error);
			goto out;
		}
		result = 0;
		break;
	}
	if (result != 0)
		(void) fprintf(stderr,
		    "parity RECORDS had no ordinary ALLOC\n");

out:
	if (rd != NULL)
		fnvlist_free(rd);
	if (out != NULL)
		fnvlist_free(out);
	return (result);
}

static int
test_parity_malformed(const char *pool)
{
	nvlist_t *args, *out = NULL;
	uint64_t words[ZPARITY_ENTRY_WORDS] = { 0 };
	uint8_t byte = 0;
	uint64_t generation, txg, *entries, cursor;
	uint64_t resume[PARITY_BLOCKS_RESUME_WORDS] = { 0 };
	uint64_t unsorted_windows[4] = { 1024, 2048, 0, 512 };
	uint64_t overlapping_windows[4] = { 0, 2048, 1024, 4096 };
	uint64_t offset_window[2] = { 0, 4096 };
	uint64_t *windows;
	uint_t nwords;
	uint_t nwin = PARITY_BLOCKS_WINDOWS_MAX + 1;
	int error;

	if ((error = lzc_parity_info(pool, &out)) != 0) {
		(void) fprintf(stderr, "parity INFO failed: %s\n",
		    strerror(error));
		return (1);
	}
	generation = fnvlist_lookup_uint64(out, ZPARITY_GENERATION);
	fnvlist_free(out);

	/* READ and VERIFY must reject an entry with fewer than 18 words. */
	args = fnvlist_alloc();
	fnvlist_add_uint64_array(args, ZPARITY_ENTRY, words,
	    ZPARITY_ENTRY_WORDS - 1);
	parity_malformed_input("parity READ malformed entry",
	    ZFS_IOC_PARITY_READ, pool, args, EINVAL);
	fnvlist_free(args);

	args = fnvlist_alloc();
	fnvlist_add_uint64_array(args, ZPARITY_ENTRY, words,
	    ZPARITY_ENTRY_WORDS - 1);
	fnvlist_add_uint8_array(args, ZPARITY_DATA, &byte, 1);
	parity_malformed_input("parity VERIFY malformed entry",
	    ZFS_IOC_PARITY_VERIFY, pool, args, EINVAL);
	fnvlist_free(args);

	/* READV rejects a flat vector whose length is not a whole entry. */
	args = fnvlist_alloc();
	fnvlist_add_uint64_array(args, ZPARITY_ENTRIES, words, 1);
	parity_malformed_input("parity READV malformed vector",
	    ZFS_IOC_PARITY_READV, pool, args, EINVAL);
	fnvlist_free(args);

	args = fnvlist_alloc();
	fnvlist_add_uint64_array(args, ZPARITY_ENTRIES, words,
	    ZPARITY_ENTRY_WORDS);
	fnvlist_add_uint64_array(args, ZPARITY_INDEXES, words, 2);
	parity_malformed_input("parity READV index count mismatch",
	    ZFS_IOC_PARITY_READV, pool, args, EINVAL);
	fnvlist_free(args);

	/* CURRENT accepts only a sector-aligned data-device offset. */
	args = fnvlist_alloc();
	fnvlist_add_uint64(args, ZPARITY_PHYS_OFF, 1);
	parity_malformed_input("parity CURRENT unaligned offset",
	    ZFS_IOC_PARITY_CURRENT, pool, args, EINVAL);
	fnvlist_free(args);

	/* MAP and BLOCKS reject a txg beyond the synced pool state. */
	args = fnvlist_alloc();
	fnvlist_add_uint64(args, ZPARITY_TXG, UINT64_MAX);
	fnvlist_add_uint64(args, ZPARITY_FROM_OFFSET, 0);
	fnvlist_add_uint64(args, ZPARITY_MAX_RANGES, 1);
	parity_malformed_input("parity MAP future txg", ZFS_IOC_PARITY_MAP,
	    pool, args, EINVAL);
	fnvlist_free(args);

	args = fnvlist_alloc();
	fnvlist_add_uint64(args, ZPARITY_TXG, UINT64_MAX);
	fnvlist_add_uint64(args, ZPARITY_FROM_OFFSET, 0);
	fnvlist_add_uint64(args, ZPARITY_MAX_ENTRIES, 1);
	parity_malformed_input("parity BLOCKS future txg",
	    ZFS_IOC_PARITY_BLOCKS, pool, args, EINVAL);
	fnvlist_free(args);

	args = fnvlist_alloc();
	fnvlist_add_uint64(args, ZPARITY_TXG, 0);
	fnvlist_add_uint64(args, ZPARITY_FROM_OFFSET, 0);
	fnvlist_add_uint64(args, ZPARITY_MAX_ENTRIES, 16);
	fnvlist_add_uint64_array(args, ZPARITY_WINDOWS, unsorted_windows,
	    ARRAY_SIZE(unsorted_windows));
	parity_malformed_input("parity BLOCKS unsorted windows",
	    ZFS_IOC_PARITY_BLOCKS, pool, args, EINVAL);
	fnvlist_free(args);

	args = fnvlist_alloc();
	fnvlist_add_uint64(args, ZPARITY_TXG, 0);
	fnvlist_add_uint64(args, ZPARITY_FROM_OFFSET, 0);
	fnvlist_add_uint64(args, ZPARITY_MAX_ENTRIES, 16);
	fnvlist_add_uint64_array(args, ZPARITY_WINDOWS, overlapping_windows,
	    ARRAY_SIZE(overlapping_windows));
	parity_malformed_input("parity BLOCKS overlapping windows",
	    ZFS_IOC_PARITY_BLOCKS, pool, args, EINVAL);
	fnvlist_free(args);

	args = fnvlist_alloc();
	fnvlist_add_uint64(args, ZPARITY_TXG, 0);
	fnvlist_add_uint64(args, ZPARITY_FROM_OFFSET, 1);
	fnvlist_add_uint64(args, ZPARITY_MAX_ENTRIES, 16);
	fnvlist_add_uint64_array(args, ZPARITY_WINDOWS, offset_window,
	    ARRAY_SIZE(offset_window));
	parity_malformed_input("parity BLOCKS windows with offset",
	    ZFS_IOC_PARITY_BLOCKS, pool, args, EINVAL);
	fnvlist_free(args);

	args = fnvlist_alloc();
	fnvlist_add_uint64(args, ZPARITY_TXG, 0);
	fnvlist_add_uint64(args, ZPARITY_FROM_OFFSET, 0);
	fnvlist_add_uint64(args, ZPARITY_MAX_ENTRIES, 16);
	windows = calloc(nwin * 2, sizeof (*windows));
	if (windows == NULL) {
		(void) fprintf(stderr, "cannot allocate test windows\n");
		fnvlist_free(args);
		return (1);
	}
	for (uint_t i = 0; i < nwin; i++) {
		windows[i * 2] = i * 2;
		windows[i * 2 + 1] = i * 2 + 1;
	}
	fnvlist_add_uint64_array(args, ZPARITY_WINDOWS, windows, nwin * 2);
	parity_malformed_input("parity BLOCKS too many windows",
	    ZFS_IOC_PARITY_BLOCKS, pool, args, EINVAL);
	free(windows);
	fnvlist_free(args);

	args = fnvlist_alloc();
	fnvlist_add_uint64(args, ZPARITY_TXG, 0);
	fnvlist_add_uint64(args, ZPARITY_FROM_OFFSET, 0);
	fnvlist_add_uint64(args, ZPARITY_MAX_ENTRIES, 16);
	fnvlist_add_uint64_array(args, ZPARITY_RESUME, resume,
	    ARRAY_SIZE(resume));
	parity_malformed_input("parity BLOCKS resume without windows",
	    ZFS_IOC_PARITY_BLOCKS, pool, args, EINVAL);
	fnvlist_free(args);

	resume[3] = 1;
	args = fnvlist_alloc();
	fnvlist_add_uint64(args, ZPARITY_TXG, 0);
	fnvlist_add_uint64(args, ZPARITY_FROM_OFFSET, 0);
	fnvlist_add_uint64(args, ZPARITY_MAX_ENTRIES, 16);
	fnvlist_add_uint64_array(args, ZPARITY_WINDOWS, offset_window,
	    ARRAY_SIZE(offset_window));
	fnvlist_add_uint64_array(args, ZPARITY_RESUME, resume,
	    ARRAY_SIZE(resume));
	parity_malformed_input("parity BLOCKS invalid resume level",
	    ZFS_IOC_PARITY_BLOCKS, pool, args, EINVAL);
	fnvlist_free(args);

	/* The label query rejects a txg that has no saved uberblock. */
	args = fnvlist_alloc();
	fnvlist_add_uint64(args, ZPARITY_TXG, UINT64_MAX);
	parity_malformed_input("parity LABEL unknown txg", ZFS_IOC_PARITY_LABEL,
	    pool, args, ENOENT);
	fnvlist_free(args);

	/* A zero set GUID and a stale generation must not change state. */
	args = fnvlist_alloc();
	fnvlist_add_uint64(args, ZPARITY_SET_GUID, 0);
	fnvlist_add_uint64(args, ZPARITY_GENERATION, generation);
	fnvlist_add_uint64(args, ZPARITY_TXG, 0);
	parity_malformed_input("parity APPLIED zero set GUID",
	    ZFS_IOC_PARITY_APPLIED, pool, args, EINVAL);
	fnvlist_free(args);

	args = fnvlist_alloc();
	fnvlist_add_uint64(args, ZPARITY_REQUEST, 1);
	fnvlist_add_uint8_array(args, ZPARITY_DATA, &byte, 0);
	parity_malformed_input("parity REPAIR empty data",
	    ZFS_IOC_PARITY_REPAIR, pool, args, EINVAL);
	fnvlist_free(args);

	args = fnvlist_alloc();
	fnvlist_add_uint64(args, ZPARITY_SET_GUID, 0);
	fnvlist_add_uint64(args, ZPARITY_GENERATION, generation + 1);
	parity_malformed_input("parity DROP stale generation",
	    ZFS_IOC_PARITY_DROP, pool, args, ESTALE);
	fnvlist_free(args);

	/* RECORDS rejects an index beyond the first retained record. */
	out = NULL;
	error = lzc_parity_records(pool, 0, 0, 1, &out);
	if (error != 0) {
		(void) fprintf(stderr, "parity RECORDS failed: %s\n",
		    strerror(error));
		fnvlist_free(out);
		return (1);
	}
	entries = fnvlist_lookup_uint64_array(out, ZPARITY_ENTRIES, &nwords);
	if (nwords < ZPARITY_ENTRY_WORDS) {
		(void) fprintf(stderr, "parity RECORDS returned no entries\n");
		fnvlist_free(out);
		return (1);
	}
	txg = entries[PARITY_EW_TXG];
	fnvlist_free(out);
	if (txg > (UINT64_MAX >> 28)) {
		(void) fprintf(stderr,
		    "parity record txg does not fit its cursor\n");
		return (1);
	}
	cursor = (txg << 28) | ((1ULL << 28) - 1);
	args = fnvlist_alloc();
	fnvlist_add_uint64(args, ZPARITY_FROM_TXG, txg);
	fnvlist_add_uint64(args, ZPARITY_CURSOR, cursor);
	fnvlist_add_uint64(args, ZPARITY_MAX_ENTRIES, 1);
	parity_malformed_input("parity RECORDS invalid cursor",
	    ZFS_IOC_PARITY_RECORDS, pool, args, EINVAL);
	fnvlist_free(args);
	if (test_parity_indexed_asize(pool) != 0)
		unexpected_failures = B_TRUE;

	return (unexpected_failures ? 1 : 0);
}

/*
 * --parity-dtl-read: a checksum-off indexed READ must not read from a
 * replacement leaf whose DTL says that it does not have the block yet.
 */
static int
test_parity_dtl_read(const char *pool)
{
	nvlist_t *out = NULL, *rd = NULL;
	uint64_t *entries, *indexes;
	uint_t nwords = 0, nindexes = 0;
	int error, result = 1;

	error = lzc_parity_records(pool, 0, 0, 64, &out);
	if (error != 0) {
		(void) fprintf(stderr, "parity RECORDS failed: %s\n",
		    strerror(error));
		goto out;
	}
	entries = fnvlist_lookup_uint64_array(out, ZPARITY_ENTRIES, &nwords);
	indexes = fnvlist_lookup_uint64_array(out, ZPARITY_INDEXES, &nindexes);
	if (entries == NULL || indexes == NULL ||
	    nwords % PARITY_ENTRY_WORDS != 0 ||
	    nindexes != nwords / PARITY_ENTRY_WORDS) {
		(void) fprintf(stderr,
		    "parity RECORDS returned invalid entries\n");
		goto out;
	}

	for (uint_t i = 0; i < nindexes; i++) {
		uint64_t *entry = &entries[i * PARITY_ENTRY_WORDS];
		uint64_t kind = entry[PARITY_EW_KIND];
		uint64_t copy[PARITY_ENTRY_WORDS];
		uint64_t index = indexes[i];

		if ((kind & 0xff) != PARITY_KIND_ALLOC ||
		    ((kind >> 8) & 3) != 0)
			continue;

		memcpy(copy, entry, sizeof (copy));
		BP_SET_CHECKSUM((blkptr_t *)&copy[PARITY_EW_BP],
		    ZIO_CHECKSUM_OFF);
		error = lzc_parity_read_index(pool, copy, &index, &rd);
		if (rd != NULL) {
			fnvlist_free(rd);
			rd = NULL;
		}
		if (error == 0)
			continue;
		if (error != EIO) {
			(void) fprintf(stderr, "parity READ of a recorded "
			    "block returned unexpected error %d\n", error);
			goto out;
		}
		result = 0;
		break;
	}
	if (result != 0)
		(void) fprintf(stderr,
		    "parity READ found no DTL-missing block\n");

out:
	if (rd != NULL)
		fnvlist_free(rd);
	if (out != NULL)
		fnvlist_free(out);
	return (result);
}

static int
test_parity_policy(const char *pool)
{
	zfs_cmd_t zc = { 0 };
	int error;

	(void) strlcpy(zc.zc_name, pool, sizeof (zc.zc_name));
	if (lzc_ioctl_fd(zfs_fd, ZFS_IOC_PARITY_INFO, &zc) == 0)
		error = 0;
	else
		error = errno;
	if (error != EPERM) {
		(void) fprintf(stderr,
		    "parity INFO policy returned %d, not EPERM\n",
		    error);
		return (1);
	}
	return (0);
}

/*
 * A member that was just armed has no record yet. INFO reports first_txg,
 * and the consumer starts from first_txg - 1: the label of that txg must
 * exist at once, also on an idle pool, and APPLIED of that txg must bind
 * the member to a set.
 */
static int
test_parity_baseline(const char *pool)
{
	nvlist_t *out = NULL;
	uint64_t first, ub_txg, generation, pool_guid, set_guid;
	int error;

	error = lzc_parity_info(pool, &out);
	if (error != 0) {
		(void) fprintf(stderr, "parity INFO failed: %s\n",
		    strerror(error));
		return (1);
	}
	first = fnvlist_lookup_uint64(out, ZPARITY_FIRST_TXG);
	generation = fnvlist_lookup_uint64(out, ZPARITY_GENERATION);
	pool_guid = fnvlist_lookup_uint64(out, ZPARITY_POOL_GUID);
	set_guid = fnvlist_lookup_uint64(out, ZPARITY_SET_GUID);
	fnvlist_free(out);
	if (first < 2) {
		(void) fprintf(stderr, "parity INFO first_txg is %llu\n",
		    (u_longlong_t)first);
		return (1);
	}

	out = NULL;
	error = lzc_parity_label(pool, first - 1, &out);
	if (error != 0) {
		(void) fprintf(stderr, "parity LABEL of the baseline txg %llu "
		    "failed: %s\n", (u_longlong_t)(first - 1), strerror(error));
		fnvlist_free(out);
		return (1);
	}
	ub_txg = fnvlist_lookup_uint64(out, ZPARITY_UB_TXG);
	fnvlist_free(out);
	if (ub_txg != first - 1) {
		(void) fprintf(stderr, "parity LABEL of txg %llu holds the "
		    "uberblock of txg %llu\n", (u_longlong_t)(first - 1),
		    (u_longlong_t)ub_txg);
		return (1);
	}

	/* A new epoch has no set yet: bind it with APPLIED of the baseline. */
	if (set_guid != 0) {
		(void) fprintf(stderr, "a new epoch has set GUID %llx\n",
		    (u_longlong_t)set_guid);
		return (1);
	}
	set_guid = pool_guid ^ 0xa5a5a5a5a5a5a5a5ULL;
	if (set_guid == 0)
		set_guid = 1;
	error = lzc_parity_applied_set(pool, set_guid, generation, first - 1);
	if (error != 0) {
		(void) fprintf(stderr, "parity APPLIED of the baseline txg "
		    "failed: %s\n", strerror(error));
		return (1);
	}
	out = NULL;
	error = lzc_parity_info(pool, &out);
	if (error != 0 ||
	    fnvlist_lookup_uint64(out, ZPARITY_SET_GUID) != set_guid) {
		(void) fprintf(stderr, "APPLIED did not bind the set GUID\n");
		fnvlist_free(out);
		return (1);
	}
	fnvlist_free(out);
	return (0);
}

/* Check LABEL on the first data column at its current synced txg. */
static int
test_parity_label_current(const char *pool)
{
	nvlist_t *info = NULL, *out = NULL;
	uint64_t txg, ub_txg;
	uint8_t *labels;
	uint_t nlabels;
	int error, result = 1;

	error = lzc_parity_info(pool, &info);
	if (error != 0) {
		(void) fprintf(stderr, "parity INFO failed: %s\n",
		    strerror(error));
		goto out;
	}
	txg = fnvlist_lookup_uint64(info, ZPARITY_LAST_TXG);
	fnvlist_free(info);
	info = NULL;
	if (txg == 0) {
		(void) fprintf(stderr, "parity INFO has no synced txg\n");
		goto out;
	}
	error = lzc_parity_label(pool, txg, &out);
	if (error != 0) {
		(void) fprintf(stderr, "parity LABEL failed: %s\n",
		    strerror(error));
		goto out;
	}
	ub_txg = fnvlist_lookup_uint64(out, ZPARITY_UB_TXG);
	if (ub_txg != txg || nvlist_lookup_uint8_array(out, ZPARITY_LABEL,
	    &labels, &nlabels) != 0 || nlabels != 2 * sizeof (vdev_label_t)) {
		(void) fprintf(stderr,
		    "parity LABEL returned an invalid image\n");
		goto out;
	}
	result = 0;
out:
	if (out != NULL)
		fnvlist_free(out);
	if (info != NULL)
		fnvlist_free(info);
	return (result);
}

/*
 * #151: LABEL must answer for every txg that a consumer can still reach:
 * from the applied txg up to the last synced txg, also after the label ring
 * moved on.
 */
static int
test_parity_label_all(const char *pool)
{
	nvlist_t *info = NULL;
	uint64_t from, last;
	int error, failed = 0;

	error = lzc_parity_info(pool, &info);
	if (error != 0) {
		(void) fprintf(stderr, "parity INFO failed: %s\n",
		    strerror(error));
		return (1);
	}
	/* nothing applied yet: from the baseline before the first record */
	from = fnvlist_lookup_uint64(info, ZPARITY_APPLIED_TXG);
	if (from == 0 && fnvlist_lookup_uint64(info, ZPARITY_FIRST_TXG) > 0)
		from = fnvlist_lookup_uint64(info, ZPARITY_FIRST_TXG) - 1;
	last = fnvlist_lookup_uint64(info, ZPARITY_LAST_TXG);
	fnvlist_free(info);
	if (from == 0 || last < from) {
		(void) fprintf(stderr, "parity INFO: from %llu, last %llu\n",
		    (u_longlong_t)from, (u_longlong_t)last);
		return (1);
	}
	for (uint64_t txg = from; txg <= last; txg++) {
		nvlist_t *out = NULL;
		error = lzc_parity_label(pool, txg, &out);
		if (error != 0 && error != ESTALE) {
			(void) fprintf(stderr, "parity LABEL of txg %llu "
			    "failed: %s\n", (u_longlong_t)txg,
			    strerror(error));
			failed++;
		} else if (out == NULL || fnvlist_lookup_uint64(out,
		    ZPARITY_UB_TXG) != txg) {
			(void) fprintf(stderr, "parity LABEL of txg %llu "
			    "returned another txg\n", (u_longlong_t)txg);
			failed++;
		}
		if (out != NULL)
			fnvlist_free(out);
	}
	(void) printf("LABEL checked for txg %llu to %llu: %d failed\n",
	    (u_longlong_t)from, (u_longlong_t)last, failed);
	return (failed != 0);
}

static int
test_parity_identity(const char *pool)
{
	nvlist_t *out = NULL;
	uint64_t generation, pool_guid, set_guid, wrong_guid;
	int error;

	error = lzc_parity_info(pool, &out);
	if (error != 0) {
		(void) fprintf(stderr, "parity INFO failed: %s\n",
		    strerror(error));
		return (1);
	}
	generation = fnvlist_lookup_uint64(out, ZPARITY_GENERATION);
	pool_guid = fnvlist_lookup_uint64(out, ZPARITY_POOL_GUID);
	set_guid = fnvlist_lookup_uint64(out, ZPARITY_SET_GUID);
	fnvlist_free(out);

	if (set_guid == 0) {
		set_guid = pool_guid ^ 0x5a5a5a5a5a5a5a5aULL;
		if (set_guid == 0)
			set_guid = 1;
		error = lzc_parity_applied_set(pool, set_guid, generation, 0);
		if (error != 0) {
			(void) fprintf(stderr,
			    "parity APPLIED could not bind: %s\n",
			    strerror(error));
			return (1);
		}
	}

	wrong_guid = set_guid + 1;
	if (wrong_guid == 0 || wrong_guid == set_guid)
		wrong_guid = set_guid ^ 1;
	error = lzc_parity_applied_set(pool, wrong_guid, generation, 0);
	if (error != ZFS_ERR_PARITY_SET_MISMATCH) {
		(void) fprintf(stderr,
		    "wrong set APPLIED returned %d\n", error);
		return (1);
	}
	error = lzc_parity_drop_set(pool, wrong_guid, generation);
	if (error != ZFS_ERR_PARITY_SET_MISMATCH) {
		(void) fprintf(stderr, "wrong set DROP returned %d\n", error);
		return (1);
	}

	error = lzc_parity_info(pool, &out);
	if (error != 0 ||
	    fnvlist_lookup_uint64(out, ZPARITY_SET_GUID) != set_guid) {
		(void) fprintf(stderr,
		    "wrong set call changed member identity\n");
		fnvlist_free(out);
		return (1);
	}
	fnvlist_free(out);
	return (0);
}

static boolean_t
parity_blocks_offset_ordered(uint64_t offset, uint64_t cursor,
    boolean_t have_previous, uint64_t previous)
{
	return (offset >= cursor &&
	    (!have_previous || offset > previous));
}

typedef struct parity_block_ref {
	uint64_t pbr_offset;
	uint64_t pbr_asize;
	uint64_t pbr_dva;
} parity_block_ref_t;

static int
parity_block_ref_compare(const void *a, const void *b)
{
	const parity_block_ref_t *x = a;
	const parity_block_ref_t *y = b;

	if (x->pbr_offset != y->pbr_offset)
		return (x->pbr_offset < y->pbr_offset ? -1 : 1);
	if (x->pbr_asize != y->pbr_asize)
		return (x->pbr_asize < y->pbr_asize ? -1 : 1);
	if (x->pbr_dva != y->pbr_dva)
		return (x->pbr_dva < y->pbr_dva ? -1 : 1);
	return (0);
}

static int
parity_block_refs_append(parity_block_ref_t **refs, size_t *count,
    size_t *capacity, const uint64_t *entry)
{
	const blkptr_t *bp = (const blkptr_t *)&entry[PARITY_EW_BP];
	uint64_t kind = entry[PARITY_EW_KIND];
	uint64_t dva = (kind >> 16) & 0xffff;
	uint64_t sectors = kind >> 32;
	uint64_t asize, offset;
	parity_block_ref_t *tmp;

	if (((kind >> 8) & 1) != 0)
		return (0);
	if ((kind & 0xff) != PARITY_KIND_ALLOC ||
	    dva >= SPA_DVAS_PER_BP || sectors == 0 ||
	    sectors > (UINT64_MAX >> SPA_MINBLOCKSHIFT))
		return (EINVAL);
	asize = sectors << SPA_MINBLOCKSHIFT;
	offset = DVA_GET_OFFSET(&bp->blk_dva[dva]);
	if (offset + asize < offset)
		return (EINVAL);
	if (*count == *capacity) {
		size_t new_capacity = (*capacity == 0 ? 1024 : *capacity * 2);

		if (new_capacity < *capacity ||
		    new_capacity > SIZE_MAX / sizeof (**refs))
			return (EOVERFLOW);
		tmp = realloc(*refs, new_capacity * sizeof (**refs));
		if (tmp == NULL)
			return (ENOMEM);
		*refs = tmp;
		*capacity = new_capacity;
	}
	(*refs)[*count].pbr_offset = offset;
	(*refs)[*count].pbr_asize = asize;
	(*refs)[*count].pbr_dva = dva;
	(*count)++;
	return (0);
}

static int
parity_block_refs_collect(parity_block_ref_t **refs, size_t *count,
    size_t *capacity, const uint64_t *entries, uint_t nwords)
{
	if (nwords % PARITY_ENTRY_WORDS != 0)
		return (EINVAL);
	for (uint_t i = 0; i < nwords / PARITY_ENTRY_WORDS; i++) {
		int error = parity_block_refs_append(refs, count, capacity,
		    &entries[i * PARITY_ENTRY_WORDS]);

		if (error != 0)
			return (error);
	}
	return (0);
}

static size_t
parity_block_refs_unique(parity_block_ref_t *refs, size_t count)
{
	size_t unique = 0;

	if (count < 2)
		return (count);
	qsort(refs, count, sizeof (*refs), parity_block_ref_compare);
	for (size_t i = 0; i < count; i++) {
		if (unique == 0 || parity_block_ref_compare(&refs[i],
		    &refs[unique - 1]) != 0)
			refs[unique++] = refs[i];
	}
	return (unique);
}

static boolean_t
parity_block_ref_overlaps(const parity_block_ref_t *ref,
    const uint64_t *windows, uint_t nwin)
{
	uint64_t end = ref->pbr_offset + ref->pbr_asize;

	for (uint_t i = 0; i < nwin; i++) {
		if (ref->pbr_offset < windows[i * 2 + 1] &&
		    windows[i * 2] < end)
			return (B_TRUE);
	}
	return (B_FALSE);
}

static int
test_parity_blocks_offset_ordering(void)
{
	/* Offset zero is valid for the first block, but not for a repeat. */
	if (!parity_blocks_offset_ordered(0, 0, B_FALSE, 0) ||
	    parity_blocks_offset_ordered(0, 0, B_TRUE, 0) ||
	    !parity_blocks_offset_ordered(512, 512, B_TRUE, 0)) {
		(void) fprintf(stderr,
		    "parity BLOCKS offset check is invalid\n");
		return (1);
	}
	return (0);
}

static int
test_parity_blocks_page_boundary(const char *pool, uint64_t applied)
{
	uint64_t cursor = 0, previous = 0;
	boolean_t have_previous = B_FALSE;

	for (uint_t page = 0; page < 2; page++) {
		nvlist_t *out = NULL;
		uint64_t *entries, next, kind, dva, offset;
		const uint64_t *w;
		const blkptr_t *bp;
		uint_t nwords = 0;
		boolean_t done;
		int error;

		error = lzc_parity_blocks(pool, applied, cursor, 1, &out);
		if (error != 0) {
			(void) fprintf(stderr,
			    "parity BLOCKS page probe failed: %s\n",
			    strerror(error));
			return (1);
		}
		entries = fnvlist_lookup_uint64_array(out, ZPARITY_ENTRIES,
		    &nwords);
		if (nwords != PARITY_ENTRY_WORDS) {
			(void) fprintf(stderr,
			    "parity BLOCKS page probe returned %u "
			    "words\n", nwords);
			fnvlist_free(out);
			return (1);
		}
		w = entries;
		bp = (const blkptr_t *)&w[PARITY_EW_BP];
		kind = w[PARITY_EW_KIND];
		dva = (kind >> 16) & 0xffff;
		if ((kind & 0xff) != PARITY_KIND_ALLOC ||
		    dva >= SPA_DVAS_PER_BP ||
		    DVA_GET_ASIZE(&bp->blk_dva[dva]) == 0) {
			(void) fprintf(stderr,
			    "parity BLOCKS page probe entry is "
			    "invalid\n");
			fnvlist_free(out);
			return (1);
		}
		offset = DVA_GET_OFFSET(&bp->blk_dva[dva]);
		if (!parity_blocks_offset_ordered(offset, cursor,
		    have_previous, previous)) {
			(void) fprintf(stderr,
			    "parity BLOCKS page probe is not "
			    "ordered\n");
			fnvlist_free(out);
			return (1);
		}
		next = fnvlist_lookup_uint64(out, ZPARITY_NEXT_OFFSET);
		done = fnvlist_lookup_boolean_value(out, ZPARITY_DONE);
		if (next <= cursor || (page == 0 && done)) {
			(void) fprintf(stderr,
			    "parity BLOCKS page probe did not "
			    "reach a second page\n");
			fnvlist_free(out);
			return (1);
		}
		cursor = next;
		previous = offset;
		have_previous = B_TRUE;
		fnvlist_free(out);
	}
	return (0);
}

static int
test_parity_blocks_pages(const char *pool)
{
	nvlist_t *out = NULL;
	uint64_t applied, first, generation, guid, pool_guid, cursor = 0;
	uint64_t previous = 0;
	boolean_t blocks_seen = B_FALSE;
	boolean_t have_previous = B_FALSE;
	boolean_t done = B_FALSE;
	int error;

	if (test_parity_blocks_offset_ordering() != 0)
		return (1);

	error = lzc_parity_info(pool, &out);
	if (error != 0) {
		(void) fprintf(stderr, "parity INFO failed: %s\n",
		    strerror(error));
		return (1);
	}
	generation = fnvlist_lookup_uint64(out, ZPARITY_GENERATION);
	guid = fnvlist_lookup_uint64(out, ZPARITY_SET_GUID);
	pool_guid = fnvlist_lookup_uint64(out, ZPARITY_POOL_GUID);
	applied = fnvlist_lookup_uint64(out, ZPARITY_LINEAGE_TXG);
	fnvlist_free(out);

	if (guid == 0) {
		guid = pool_guid ^ 0x6b6b6b6b6b6b6b6bULL;
		if (guid == 0)
			guid = 1;
	}
	error = lzc_parity_applied_set(pool, guid, generation, applied);
	if (error != 0) {
		(void) fprintf(stderr, "parity APPLIED failed: %s\n",
		    strerror(error));
		return (1);
	}

	error = lzc_parity_info(pool, &out);
	if (error != 0) {
		(void) fprintf(stderr, "parity INFO failed: %s\n",
		    strerror(error));
		return (1);
	}
	applied = fnvlist_lookup_uint64(out, ZPARITY_APPLIED_TXG);
	first = fnvlist_lookup_uint64(out, ZPARITY_FIRST_TXG);
	fnvlist_free(out);
	if (applied + 1 < first) {
		(void) fprintf(stderr, "parity applied state is outside the "
		    "retained block range\n");
		return (1);
	}
	if (test_parity_blocks_page_boundary(pool, applied) != 0)
		return (1);

	for (uint64_t pages = 0; !done && pages < 65536; pages++) {
		uint64_t next;
		uint64_t *entries;
		uint_t nwords = 0;

		out = NULL;
		error = lzc_parity_blocks(pool, applied, cursor, 64, &out);
		if (error != 0) {
			(void) fprintf(stderr, "parity BLOCKS failed: %s\n",
			    strerror(error));
			return (1);
		}
		if (fnvlist_lookup_uint64(out, ZPARITY_VERSION) !=
		    ZPARITY_VERSION_CURRENT) {
			(void) fprintf(stderr,
			    "parity BLOCKS version mismatch\n");
			fnvlist_free(out);
			return (1);
		}
		entries = fnvlist_lookup_uint64_array(out, ZPARITY_ENTRIES,
		    &nwords);
		if (nwords % PARITY_ENTRY_WORDS != 0) {
			(void) fprintf(stderr,
			    "parity BLOCKS returned %u words\n",
			    nwords);
			fnvlist_free(out);
			return (1);
		}
			blocks_seen |= (nwords != 0);
		next = fnvlist_lookup_uint64(out, ZPARITY_NEXT_OFFSET);
		done = fnvlist_lookup_boolean_value(out, ZPARITY_DONE);
		for (uint_t i = 0; i < nwords / PARITY_ENTRY_WORDS; i++) {
			const uint64_t *w = &entries[i * PARITY_ENTRY_WORDS];
			const blkptr_t *bp =
			    (const blkptr_t *)&w[PARITY_EW_BP];
			uint64_t kind = w[PARITY_EW_KIND];
			uint64_t dva = (kind >> 16) & 0xffff;
			uint64_t offset;

			if ((kind & 0xff) != PARITY_KIND_ALLOC ||
			    w[PARITY_EW_TXG] != applied ||
			    dva >= SPA_DVAS_PER_BP ||
			    DVA_GET_ASIZE(&bp->blk_dva[dva]) == 0 ||
			    BP_GET_BIRTH(bp) > applied) {
				(void) fprintf(stderr,
				    "parity BLOCKS entry is invalid\n");
				fnvlist_free(out);
				return (1);
			}
			offset = DVA_GET_OFFSET(&bp->blk_dva[dva]);
			if (!parity_blocks_offset_ordered(offset, cursor,
			    have_previous, previous)) {
				(void) fprintf(stderr,
				    "parity BLOCKS entries are not ordered\n");
				fnvlist_free(out);
				return (1);
			}
			previous = offset;
			have_previous = B_TRUE;
		}
		if (!done && next <= cursor) {
			(void) fprintf(stderr,
			    "parity BLOCKS page did not advance\n");
			fnvlist_free(out);
			return (1);
		}
		cursor = next;
		fnvlist_free(out);
	}
	if (!done) {
		(void) fprintf(stderr, "parity BLOCKS did not finish paging\n");
		return (1);
	}
	if (!blocks_seen) {
		(void) fprintf(stderr,
		    "parity BLOCKS returned no allocated blocks\n");
		return (1);
	}
	return (0);
}

/* Compare one windowed walk with the old offset-paged BLOCKS answer. */
static int
test_parity_blocks_windows(const char *pool)
{
	nvlist_t *out = NULL;
	parity_block_ref_t *old = NULL, *expected = NULL, *actual = NULL;
	size_t old_count = 0, old_capacity = 0;
	size_t expected_count = 0, actual_count = 0, actual_capacity = 0;
	uint64_t applied, first, generation, guid, pool_guid, cursor = 0;
	uint64_t windows[4], gap[2], resume[PARITY_BLOCKS_RESUME_WORDS] = { 0 };
	uint64_t walks_before, walks_after, empty_before, empty_after;
	uint64_t pages = 0;
	uint_t nwin = 2;
	boolean_t old_done = B_FALSE, done = B_FALSE;
	boolean_t have_resume = B_FALSE, have_gap = B_FALSE;
	int error, result = 1;

	error = lzc_parity_info(pool, &out);
	if (error != 0) {
		(void) fprintf(stderr, "parity INFO failed: %s\n",
		    strerror(error));
		goto out;
	}
	generation = fnvlist_lookup_uint64(out, ZPARITY_GENERATION);
	guid = fnvlist_lookup_uint64(out, ZPARITY_SET_GUID);
	pool_guid = fnvlist_lookup_uint64(out, ZPARITY_POOL_GUID);
	applied = fnvlist_lookup_uint64(out, ZPARITY_LINEAGE_TXG);
	fnvlist_free(out);
	out = NULL;
	if (guid == 0) {
		guid = pool_guid ^ 0x6b6b6b6b6b6b6b6bULL;
		if (guid == 0)
			guid = 1;
	}
	error = lzc_parity_applied_set(pool, guid, generation, applied);
	if (error != 0) {
		(void) fprintf(stderr, "parity APPLIED failed: %s\n",
		    strerror(error));
		goto out;
	}
	error = lzc_parity_info(pool, &out);
	if (error != 0) {
		(void) fprintf(stderr, "parity INFO failed: %s\n",
		    strerror(error));
		goto out;
	}
	applied = fnvlist_lookup_uint64(out, ZPARITY_APPLIED_TXG);
	first = fnvlist_lookup_uint64(out, ZPARITY_FIRST_TXG);
	fnvlist_free(out);
	out = NULL;
	if (applied == 0 || applied + 1 < first) {
		(void) fprintf(stderr,
		    "parity applied txg is outside the retained range\n");
		goto out;
	}

	for (uint_t page = 0; !old_done && page < 65536; page++) {
		uint64_t *entries, next;
		uint_t nwords = 0;

		error = lzc_parity_blocks(pool, applied, cursor, 16384, &out);
		if (error != 0) {
			(void) fprintf(stderr, "parity BLOCKS failed: %s\n",
			    strerror(error));
			goto out;
		}
		entries = fnvlist_lookup_uint64_array(out, ZPARITY_ENTRIES,
		    &nwords);
		if ((entries == NULL && nwords != 0) ||
		    parity_block_refs_collect(&old, &old_count, &old_capacity,
		    entries, nwords) != 0) {
			(void) fprintf(stderr,
			    "old BLOCKS returned an invalid entry array\n");
			fnvlist_free(out);
			out = NULL;
			goto out;
		}
		next = fnvlist_lookup_uint64(out, ZPARITY_NEXT_OFFSET);
		old_done = fnvlist_lookup_boolean_value(out, ZPARITY_DONE);
		fnvlist_free(out);
		out = NULL;
		if (!old_done && next <= cursor) {
			(void) fprintf(stderr,
			    "old BLOCKS page did not advance\n");
			goto out;
		}
		cursor = next;
	}
	if (!old_done) {
		(void) fprintf(stderr, "old BLOCKS did not finish paging\n");
		goto out;
	}
	old_count = parity_block_refs_unique(old, old_count);
	if (old_count < 16000) {
		(void) fprintf(stderr,
		    "old BLOCKS returned only %llu distinct entries\n",
		    (u_longlong_t)old_count);
		goto out;
	}

	windows[0] = old[0].pbr_offset;
	windows[1] = old[4999].pbr_offset + old[4999].pbr_asize;
	windows[2] = old[10000].pbr_offset;
	windows[3] = old[14999].pbr_offset + old[14999].pbr_asize;
	if (windows[0] >= windows[1] || windows[2] >= windows[3]) {
		(void) fprintf(stderr, "test windows are empty\n");
		goto out;
	}
	if (windows[2] < windows[1]) {
		windows[1] = MAX(windows[1], windows[3]);
		nwin = 1;
	}
	expected = malloc(old_count * sizeof (*expected));
	if (expected == NULL) {
		(void) fprintf(stderr, "cannot allocate expected entries\n");
		goto out;
	}
	for (size_t i = 0; i < old_count; i++) {
		if (parity_block_ref_overlaps(&old[i], windows, nwin))
			expected[expected_count++] = old[i];
	}
	if (expected_count <= 500 || expected_count == old_count) {
		(void) fprintf(stderr,
		    "test windows did not select a paged subset\n");
		goto out;
	}

	error = lzc_parity_info(pool, &out);
	if (error != 0) {
		(void) fprintf(stderr, "parity INFO failed: %s\n",
		    strerror(error));
		goto out;
	}
	walks_before = fnvlist_lookup_uint64(out,
	    ZPARITY_BLOCKS_WALKS);
	fnvlist_free(out);
	out = NULL;

	while (!done && pages < 65536) {
		const uint64_t *resume_out;
		const uint64_t *resume_in = have_resume ? resume : NULL;
		uint64_t *entries;
		uint_t nwords = 0, nresume = 0;
		uint64_t nentries;

		error = lzc_parity_blocks_windows(pool, applied, windows, nwin,
		    resume_in, 500, &out);
		if (error != 0) {
			(void) fprintf(stderr,
			    "windowed BLOCKS failed: %s\n", strerror(error));
			goto out;
		}
		entries = fnvlist_lookup_uint64_array(out, ZPARITY_ENTRIES,
		    &nwords);
		if ((entries == NULL && nwords != 0) ||
		    nwords % PARITY_ENTRY_WORDS != 0) {
			(void) fprintf(stderr,
			    "windowed BLOCKS returned an invalid entry "
			    "array\n");
			fnvlist_free(out);
			out = NULL;
			goto out;
		}
		done = fnvlist_lookup_boolean_value(out, ZPARITY_DONE);
		nentries = nwords / PARITY_ENTRY_WORDS;
		if (nentries > 500 || (!done && nentries + 2 < 500)) {
			(void) fprintf(stderr,
			    "windowed BLOCKS page exceeded its strict cap\n");
			fnvlist_free(out);
			out = NULL;
			goto out;
		}
		if (parity_block_refs_collect(&actual, &actual_count,
		    &actual_capacity, entries, nwords) != 0) {
			(void) fprintf(stderr,
			    "windowed BLOCKS returned an invalid entry\n");
			fnvlist_free(out);
			out = NULL;
			goto out;
		}
		if (!done) {
			resume_out = fnvlist_lookup_uint64_array(out,
			    ZPARITY_RESUME, &nresume);
			if (resume_out == NULL ||
			    nresume != PARITY_BLOCKS_RESUME_WORDS ||
			    resume_out[3] != 0 ||
			    (have_resume && memcmp(resume, resume_out,
			    sizeof (resume)) == 0)) {
				(void) fprintf(stderr,
				    "windowed BLOCKS returned an invalid or "
				    "repeated resume bookmark\n");
				fnvlist_free(out);
				out = NULL;
				goto out;
			}
			memcpy(resume, resume_out, sizeof (resume));
			have_resume = B_TRUE;
		} else if (nvlist_exists(out, ZPARITY_RESUME)) {
			(void) fprintf(stderr,
			    "completed BLOCKS walk returned a resume "
			    "bookmark\n");
			fnvlist_free(out);
			out = NULL;
			goto out;
		}
		pages++;
		fnvlist_free(out);
		out = NULL;
	}
	if (!done) {
		(void) fprintf(stderr,
		    "windowed BLOCKS did not finish paging\n");
		goto out;
	}
	error = lzc_parity_info(pool, &out);
	if (error != 0) {
		(void) fprintf(stderr, "parity INFO failed: %s\n",
		    strerror(error));
		goto out;
	}
	walks_after = fnvlist_lookup_uint64(out, ZPARITY_BLOCKS_WALKS);
	fnvlist_free(out);
	out = NULL;
	if (walks_after < walks_before || walks_after - walks_before != pages) {
		(void) fprintf(stderr,
		    "BLOCKS walk counter did not match its page count\n");
		goto out;
	}
	actual_count = parity_block_refs_unique(actual, actual_count);
	if (actual_count != expected_count) {
		(void) fprintf(stderr,
		    "windowed BLOCKS returned %llu entries; expected %llu\n",
		    (u_longlong_t)actual_count,
		    (u_longlong_t)expected_count);
		goto out;
	}
	for (size_t i = 0; i < expected_count; i++) {
		if (parity_block_ref_compare(&actual[i], &expected[i]) != 0) {
			(void) fprintf(stderr,
			    "windowed BLOCKS differs from old BLOCKS\n");
			goto out;
		}
	}

	for (size_t i = 1; i < old_count; i++) {
		uint64_t end = old[i - 1].pbr_offset + old[i - 1].pbr_asize;

		if (end < old[i].pbr_offset) {
			gap[0] = end;
			gap[1] = old[i].pbr_offset;
			have_gap = B_TRUE;
			break;
		}
	}
	if (!have_gap) {
		(void) fprintf(stderr,
		    "could not find an unallocated window for the test\n");
		goto out;
	}
	error = lzc_parity_info(pool, &out);
	if (error != 0) {
		(void) fprintf(stderr, "parity INFO failed: %s\n",
		    strerror(error));
		goto out;
	}
	empty_before = fnvlist_lookup_uint64(out, ZPARITY_BLOCKS_WALKS);
	fnvlist_free(out);
	out = NULL;
	error = lzc_parity_blocks_windows(pool, applied, gap, 1, NULL, 500,
	    &out);
	if (error != 0) {
		(void) fprintf(stderr, "empty windowed BLOCKS failed: %s\n",
		    strerror(error));
		goto out;
	}
	{
		uint_t nwords = 0;
		(void) fnvlist_lookup_uint64_array(out, ZPARITY_ENTRIES,
		    &nwords);
		if (nwords != 0 ||
		    !fnvlist_lookup_boolean_value(out, ZPARITY_DONE) ||
		    nvlist_exists(out, ZPARITY_RESUME)) {
			(void) fprintf(stderr,
			    "empty windowed BLOCKS was not done in one page\n");
			goto out;
		}
	}
	fnvlist_free(out);
	out = NULL;
	error = lzc_parity_info(pool, &out);
	if (error != 0) {
		(void) fprintf(stderr, "parity INFO failed: %s\n",
		    strerror(error));
		goto out;
	}
	empty_after = fnvlist_lookup_uint64(out, ZPARITY_BLOCKS_WALKS);
	if (empty_after != empty_before + 1) {
		(void) fprintf(stderr,
		    "empty window did not use exactly one walk\n");
		goto out;
	}
	result = 0;
out:
	fnvlist_free(out);
	free(old);
	free(expected);
	free(actual);
	return (result);
}

/* --parity-map: ask MAP for the baseline of the current parity epoch. */
static int
test_parity_map_epoch(const char *pool)
{
	nvlist_t *info = NULL, *map = NULL;
	uint64_t first, txg;
	int error;

	error = lzc_parity_info(pool, &info);
	if (error != 0) {
		(void) fprintf(stderr, "parity INFO failed: %s\n",
		    strerror(error));
		return (1);
	}
	first = fnvlist_lookup_uint64(info, ZPARITY_FIRST_TXG);
	fnvlist_free(info);
	if (first == 0) {
		(void) fprintf(stderr, "parity INFO returned no first txg\n");
		return (1);
	}
	txg = first - 1;
	error = lzc_parity_map(pool, txg, 0, 64, &map);
	if (error != 0) {
		(void) fprintf(stderr, "parity MAP failed: %s\n",
		    strerror(error));
		return (1);
	}
	fnvlist_free(map);
	return (0);
}

static int
test_parity_map_total(const char *pool, uint64_t txg, uint64_t *total)
{
	uint64_t cursor = 0;

	*total = 0;
	for (uint64_t page = 0; page < 4096; page++) {
		nvlist_t *out = NULL;
		uint64_t *ranges, next;
		uint_t nwords = 0;
		boolean_t done;
		int error;

		error = lzc_parity_map(pool, txg, cursor, 4096, &out);
		if (error != 0)
			return (error);
		ranges = fnvlist_lookup_uint64_array(out, ZPARITY_RANGES,
		    &nwords);
		if (nwords % 2 != 0) {
			fnvlist_free(out);
			return (EINVAL);
		}
		for (uint_t i = 0; i < nwords; i += 2) {
			if (*total + ranges[i + 1] < *total) {
				fnvlist_free(out);
				return (EOVERFLOW);
			}
			*total += ranges[i + 1];
		}
		next = fnvlist_lookup_uint64(out, ZPARITY_NEXT_OFFSET);
		done = fnvlist_lookup_boolean_value(out, ZPARITY_DONE);
		fnvlist_free(out);
		if (done)
			return (0);
		if (next <= cursor)
			return (EINVAL);
		cursor = next;
	}
	return (EOVERFLOW);
}

/* --parity-map-stress: keep MAP and BLOCKS active during a member replace. */
static int
test_parity_map_stress(const char *pool)
{
	uint64_t baseline = 0;
	uint64_t samples = 0;
	int error;

	for (uint64_t iter = 0; iter < 256; iter++) {
		nvlist_t *info = NULL, *blocks = NULL;
		uint64_t applied, first, asize, txg, total = 0, tolerance;
		uint64_t *entries;
		uint_t nwords = 0;

		error = lzc_parity_info(pool, &info);
		if (error != 0) {
			(void) fprintf(stderr, "parity INFO failed: %s\n",
			    strerror(error));
			return (1);
		}
		applied = fnvlist_lookup_uint64(info, ZPARITY_APPLIED_TXG);
		first = fnvlist_lookup_uint64(info, ZPARITY_FIRST_TXG);
		asize = fnvlist_lookup_uint64(info, ZPARITY_ASIZE);
		fnvlist_free(info);
		if (first == 0 || applied + 1 < first) {
			(void) fprintf(stderr, "parity state has no applied "
			    "txg\n");
			return (1);
		}
		txg = applied;
		error = test_parity_map_total(pool, txg, &total);
		if (error == EAGAIN)
			continue;
		if (error != 0 || total == 0 || total > asize) {
			(void) fprintf(stderr, "parity MAP total is invalid "
			    "(%s, "
			    "%llu of %llu bytes)\n",
			    strerror(error), (u_longlong_t)total,
			    (u_longlong_t)asize);
			return (1);
		}
		if (samples == 0)
			baseline = total;
		samples++;
		tolerance = MAX(16ULL * 1024 * 1024, baseline / 8);
		if (total + tolerance < baseline ||
		    baseline + tolerance < total) {
			(void) fprintf(stderr, "parity MAP total changed from "
			    "%llu "
			    "to %llu bytes\n", (u_longlong_t)baseline,
			    (u_longlong_t)total);
			return (1);
		}

		error = lzc_parity_blocks(pool, applied, 0, 1, &blocks);
		if (error != 0) {
			(void) fprintf(stderr, "parity BLOCKS failed: %s\n",
			    strerror(error));
			return (1);
		}
		entries = fnvlist_lookup_uint64_array(blocks,
		    ZPARITY_ENTRIES, &nwords);
		if (entries == NULL || nwords % PARITY_ENTRY_WORDS != 0) {
			fnvlist_free(blocks);
			(void) fprintf(stderr, "parity BLOCKS returned an "
			    "invalid entry array\n");
			return (1);
		}
		fnvlist_free(blocks);
	}
	if (samples == 0) {
		(void) fprintf(stderr, "parity MAP had no stable sample\n");
		return (1);
	}
	(void) fprintf(stderr, "parity MAP stayed within %llu bytes of "
	    "its initial total\n", (u_longlong_t)MAX(16ULL * 1024 * 1024,
	    baseline / 8));
	return (0);
}

/*
 * --parity-apply: report the lineage txg as applied, so the next write of
 * the pool releases that record.
 */
static int
test_parity_apply(const char *pool)
{
	nvlist_t *out = NULL;
	uint64_t applied, generation, guid, pool_guid;
	int error;

	error = lzc_parity_info(pool, &out);
	if (error != 0) {
		(void) fprintf(stderr, "parity INFO failed: %s\n",
		    strerror(error));
		return (1);
	}
	generation = fnvlist_lookup_uint64(out, ZPARITY_GENERATION);
	guid = fnvlist_lookup_uint64(out, ZPARITY_SET_GUID);
	pool_guid = fnvlist_lookup_uint64(out, ZPARITY_POOL_GUID);
	applied = fnvlist_lookup_uint64(out, ZPARITY_LINEAGE_TXG);
	fnvlist_free(out);
	if (guid == 0) {
		guid = pool_guid ^ 0x6b6b6b6b6b6b6b6bULL;
		if (guid == 0)
			guid = 1;
	}
	error = lzc_parity_applied_set(pool, guid, generation, applied);
	if (error != 0) {
		(void) fprintf(stderr, "parity APPLIED failed: %s\n",
		    strerror(error));
		return (1);
	}
	(void) printf("applied %llu\n", (u_longlong_t)applied);
	return (0);
}

/*
 * --parity-apply-last: apply the txg of the newest record, as the daemon
 * does, not the last synced txg.  The txgs after it that have no record
 * (the pool synced only its own state) are then not applied.
 */
static int
test_parity_apply_last(const char *pool)
{
	nvlist_t *out = NULL;
	uint64_t last, generation, guid, pool_guid;
	int error;

	error = lzc_parity_info(pool, &out);
	if (error != 0) {
		(void) fprintf(stderr, "parity INFO failed: %s\n",
		    strerror(error));
		return (1);
	}
	generation = fnvlist_lookup_uint64(out, ZPARITY_GENERATION);
	guid = fnvlist_lookup_uint64(out, ZPARITY_SET_GUID);
	pool_guid = fnvlist_lookup_uint64(out, ZPARITY_POOL_GUID);
	last = fnvlist_lookup_uint64(out, ZPARITY_LAST_TXG);
	fnvlist_free(out);
	if (guid == 0) {
		guid = pool_guid ^ 0x6b6b6b6b6b6b6b6bULL;
		if (guid == 0)
			guid = 1;
	}
	if (last == 0) {
		(void) fprintf(stderr, "the pool has no record\n");
		return (1);
	}
	error = lzc_parity_applied_set(pool, guid, generation, last);
	if (error != 0) {
		(void) fprintf(stderr, "parity APPLIED failed: %s\n",
		    strerror(error));
		return (1);
	}
	(void) printf("applied %llu\n", (u_longlong_t)last);
	return (0);
}

/*
 * --parity-history-gap: the oldest record is newer than the txg after the
 * applied one (the txgs between had no record).  MAP and BLOCKS must still
 * serve the applied txg: a rebuild reads each surviving pool there.  Before
 * the fix, both refused it with EINVAL.
 */
static int
test_parity_history_gap(const char *pool)
{
	nvlist_t *out = NULL;
	uint64_t applied, first, total = 0;
	int error;

	error = lzc_parity_info(pool, &out);
	if (error != 0) {
		(void) fprintf(stderr, "parity INFO failed: %s\n",
		    strerror(error));
		return (1);
	}
	applied = fnvlist_lookup_uint64(out, ZPARITY_APPLIED_TXG);
	first = fnvlist_lookup_uint64(out, ZPARITY_FIRST_TXG);
	fnvlist_free(out);
	if (applied == 0 || first <= applied + 1) {
		(void) fprintf(stderr, "no gap: applied txg %llu, oldest "
		    "record %llu\n", (u_longlong_t)applied,
		    (u_longlong_t)first);
		return (1);
	}
	error = test_parity_map_total(pool, applied, &total);
	if (error != 0 || total == 0) {
		(void) fprintf(stderr, "parity MAP at the applied txg %llu "
		    "(oldest record %llu): %s\n", (u_longlong_t)applied,
		    (u_longlong_t)first, strerror(error));
		return (1);
	}
	error = lzc_parity_blocks(pool, applied, 0, 16, &out);
	if (error != 0) {
		(void) fprintf(stderr, "parity BLOCKS at the applied txg %llu "
		    "(oldest record %llu): %s\n", (u_longlong_t)applied,
		    (u_longlong_t)first, strerror(error));
		return (1);
	}
	fnvlist_free(out);
	(void) printf("parity MAP and BLOCKS served txg %llu below the "
	    "oldest record %llu\n", (u_longlong_t)applied,
	    (u_longlong_t)first);
	return (0);
}

/*
 * --parity-readv: READV the pending ALLOC records of 64 KiB or more (at most
 * ZPARITY_READV_MAX_ENTRIES; the small metadata blocks of the arm come
 * first) in one call and print "readv <entries> <bytes>".  The test
 * compares <bytes> with the growth of the kstat read_bytes: a reply that
 * did not fit the first buffer made the kernel read every block twice.
 */
static int
test_parity_readv_once(const char *pool)
{
	nvlist_t *out = NULL, *rd = NULL;
	uint64_t *entries, *indexes, *lengths;
	uint64_t words[ZPARITY_READV_MAX_ENTRIES * PARITY_ENTRY_WORDS];
	uint64_t idx[ZPARITY_READV_MAX_ENTRIES], bytes = 0;
	uint_t nwords = 0, nindexes = 0, nlengths = 0, n = 0, nerr = 0;
	int32_t *errors;
	int error, result = 1;

	error = lzc_parity_records(pool, 0, 0, 4096, &out);
	if (error != 0) {
		(void) fprintf(stderr, "parity RECORDS failed: %s\n",
		    strerror(error));
		goto out;
	}
	entries = fnvlist_lookup_uint64_array(out, ZPARITY_ENTRIES, &nwords);
	indexes = fnvlist_lookup_uint64_array(out, ZPARITY_INDEXES, &nindexes);
	if (entries == NULL || indexes == NULL ||
	    nwords % PARITY_ENTRY_WORDS != 0 ||
	    nindexes != nwords / PARITY_ENTRY_WORDS) {
		(void) fprintf(stderr,
		    "parity RECORDS returned invalid entries\n");
		goto out;
	}
	for (uint_t i = 0; i < nindexes && n < ZPARITY_READV_MAX_ENTRIES;
	    i++) {
		uint64_t *entry = &entries[i * PARITY_ENTRY_WORDS];
		uint64_t kind = entry[PARITY_EW_KIND];
		const blkptr_t *bp = (const blkptr_t *)&entry[PARITY_EW_BP];

		if ((kind & 0xff) != PARITY_KIND_ALLOC ||
		    ((kind >> 8) & 3) != 0 ||
		    DVA_GET_ASIZE(&bp->blk_dva[0]) < 65536)
			continue;
		memcpy(&words[n * PARITY_ENTRY_WORDS], entry,
		    PARITY_ENTRY_BYTES);
		idx[n++] = indexes[i];
	}
	if (n == 0) {
		(void) fprintf(stderr,
		    "parity RECORDS had no large ALLOC entry\n");
		goto out;
	}
	error = lzc_parity_readv(pool, words, n, idx, &rd);
	if (error != 0) {
		(void) fprintf(stderr, "parity READV failed: %s\n",
		    strerror(error));
		goto out;
	}
	errors = fnvlist_lookup_int32_array(rd, ZPARITY_READ_ERRORS, &nerr);
	lengths = fnvlist_lookup_uint64_array(rd, ZPARITY_DATA_LENGTHS,
	    &nlengths);
	if (nerr != n || nlengths != n) {
		(void) fprintf(stderr, "parity READV returned %u errors and "
		    "%u lengths for %u entries\n", nerr, nlengths, n);
		goto out;
	}
	for (uint_t i = 0; i < n; i++) {
		if (errors[i] != 0) {
			(void) fprintf(stderr, "parity READV of entry %u "
			    "failed: %d\n", i, errors[i]);
			goto out;
		}
		bytes += lengths[i];
	}
	(void) printf("readv %u %llu\n", n, (u_longlong_t)bytes);
	result = 0;
out:
	if (rd != NULL)
		fnvlist_free(rd);
	if (out != NULL)
		fnvlist_free(out);
	return (result);
}

/*
 * #148: --parity-read-hash [--compressed]: READV every ALLOC entry of the
 * pending records (not a log or gang block; with --compressed only the
 * compressed ones) and print "readhash <entries> <bytes> <fnv1a-64>".  The
 * test runs it with the ARC path on and off: the bytes must be the same.
 */
static int
test_parity_read_hash(const char *pool, boolean_t compressed)
{
	nvlist_t *out = NULL;
	uint64_t *entries, *indexes;
	uint64_t words[64 * PARITY_ENTRY_WORDS], idx[64];
	uint64_t hash = 0xcbf29ce484222325ULL, bytes = 0, total = 0;
	uint_t nwords = 0, nindexes = 0, n = 0;
	int error, result = 1;

	error = lzc_parity_records(pool, 0, 0, 4096, &out);
	if (error != 0) {
		(void) fprintf(stderr, "parity RECORDS failed: %s\n",
		    strerror(error));
		return (1);
	}
	entries = fnvlist_lookup_uint64_array(out, ZPARITY_ENTRIES, &nwords);
	indexes = fnvlist_lookup_uint64_array(out, ZPARITY_INDEXES, &nindexes);
	if (entries == NULL || indexes == NULL ||
	    nindexes != nwords / PARITY_ENTRY_WORDS) {
		(void) fprintf(stderr, "parity RECORDS: invalid entries\n");
		goto out;
	}
	for (uint_t i = 0; i <= nindexes; i++) {
		if (i < nindexes) {
			uint64_t *entry = &entries[i * PARITY_ENTRY_WORDS];
			uint64_t kind = entry[PARITY_EW_KIND];
			const blkptr_t *bp =
			    (const blkptr_t *)&entry[PARITY_EW_BP];

			if ((kind & 0xff) != PARITY_KIND_ALLOC ||
			    ((kind >> 8) & 3) != 0 || (compressed &&
			    BP_GET_COMPRESS(bp) == ZIO_COMPRESS_OFF))
				continue;
			memcpy(&words[n * PARITY_ENTRY_WORDS], entry,
			    PARITY_ENTRY_BYTES);
			idx[n++] = indexes[i];
			if (n < 64)
				continue;
		}
		if (n == 0)
			break;
		nvlist_t *rd = NULL;
		uint8_t *data;
		uint64_t *offs, *lens;
		int32_t *errs;
		uint_t nd = 0, no = 0, nl = 0, ne = 0;

		error = lzc_parity_readv(pool, words, n, idx, &rd);
		if (error != 0) {
			(void) fprintf(stderr, "parity READV failed: %s\n",
			    strerror(error));
			goto out;
		}
		errs = fnvlist_lookup_int32_array(rd, ZPARITY_READ_ERRORS, &ne);
		offs = fnvlist_lookup_uint64_array(rd, ZPARITY_DATA_OFFSETS,
		    &no);
		lens = fnvlist_lookup_uint64_array(rd, ZPARITY_DATA_LENGTHS,
		    &nl);
		data = fnvlist_lookup_uint8_array(rd, ZPARITY_DATA, &nd);
		for (uint_t j = 0; j < n; j++) {
			if (ne != n || no != n || nl != n || errs[j] != 0 ||
			    offs[j] + lens[j] > nd) {
				(void) fprintf(stderr, "parity READV entry "
				    "%u failed\n", j);
				fnvlist_free(rd);
				goto out;
			}
			for (uint64_t k = 0; k < lens[j]; k++) {
				hash ^= data[offs[j] + k];
				hash *= 0x100000001b3ULL;
			}
			bytes += lens[j];
		}
		fnvlist_free(rd);
		total += n;
		n = 0;
	}
	if (total == 0) {
		(void) fprintf(stderr, "parity RECORDS had no ALLOC entry\n");
		goto out;
	}
	(void) printf("readhash %llu %llu %016llx\n", (u_longlong_t)total,
	    (u_longlong_t)bytes, (u_longlong_t)hash);
	result = 0;
out:
	fnvlist_free(out);
	return (result);
}

/*
 * --parity-blocks-read: READ every BLOCKS entry of the applied state, after
 * the pool wrote again and released the record of that txg.  READV must also
 * accept them.  A set scrub depends on this.
 */
static int
test_parity_blocks_read(const char *pool)
{
	nvlist_t *out = NULL;
	uint64_t applied, first, cursor = 0, nread = 0;
	boolean_t done = B_FALSE, nocksum_tried = B_FALSE;
	int error;

	error = lzc_parity_info(pool, &out);
	if (error != 0) {
		(void) fprintf(stderr, "parity INFO failed: %s\n",
		    strerror(error));
		return (1);
	}
	applied = fnvlist_lookup_uint64(out, ZPARITY_APPLIED_TXG);
	first = fnvlist_lookup_uint64(out, ZPARITY_FIRST_TXG);
	fnvlist_free(out);
	if (applied == 0 || applied + 1 < first) {
		(void) fprintf(stderr, "parity applied txg %llu is outside "
		    "the retained range (first %llu)\n",
		    (u_longlong_t)applied, (u_longlong_t)first);
		return (1);
	}
	if (applied >= first) {
		(void) fprintf(stderr, "the record of txg %llu is not "
		    "released (first %llu): the test did not write\n",
		    (u_longlong_t)applied, (u_longlong_t)first);
		return (1);
	}
	for (uint64_t pages = 0; !done && pages < 65536; pages++) {
		uint64_t *entries;
		uint_t nwords = 0;

		out = NULL;
		error = lzc_parity_blocks(pool, applied, cursor, 64, &out);
		if (error != 0) {
			(void) fprintf(stderr, "parity BLOCKS failed: %s\n",
			    strerror(error));
			return (1);
		}
		entries = fnvlist_lookup_uint64_array(out, ZPARITY_ENTRIES,
		    &nwords);
		for (uint_t i = 0; i < nwords / PARITY_ENTRY_WORDS; i++) {
			nvlist_t *rd = NULL;

			error = lzc_parity_read(pool,
			    &entries[i * PARITY_ENTRY_WORDS], &rd);
			if (rd != NULL)
				fnvlist_free(rd);
			if (error != 0) {
				(void) fprintf(stderr, "parity READ of a "
				    "BLOCKS entry of txg %llu failed: %s\n",
				    (u_longlong_t)applied, strerror(error));
				fnvlist_free(out);
				return (1);
			}
			nread++;
			/*
			 * The same entry with checksum off must not read: no
			 * record and no checksum tie it to the applied state.
			 */
			uint64_t *e = &entries[i * PARITY_ENTRY_WORDS];
			uint64_t flags = e[PARITY_EW_KIND] >> 8;

			if (nocksum_tried || (flags & 3) != 0)
				continue;
			uint64_t copy[PARITY_ENTRY_WORDS];

			memcpy(copy, e, sizeof (copy));
			BP_SET_CHECKSUM((blkptr_t *)&copy[PARITY_EW_BP],
			    ZIO_CHECKSUM_OFF);
			rd = NULL;
			error = lzc_parity_read(pool, copy, &rd);
			if (rd != NULL)
				fnvlist_free(rd);
			if (error != ZFS_ERR_PARITY_UNVERIFIABLE) {
				(void) fprintf(stderr, "parity READ of a "
				    "checksum-off BLOCKS entry gave %d, not "
				    "ZFS_ERR_PARITY_UNVERIFIABLE\n", error);
				fnvlist_free(out);
				return (1);
			}
			nocksum_tried = B_TRUE;
		}
		if (nwords != 0) {
			nvlist_t *rd = NULL;

			error = lzc_parity_readv(pool, entries,
			    nwords / PARITY_ENTRY_WORDS, NULL, &rd);
			if (rd != NULL)
				fnvlist_free(rd);
			if (error != 0) {
				(void) fprintf(stderr, "parity READV of "
				    "BLOCKS entries failed: %s\n",
				    strerror(error));
				fnvlist_free(out);
				return (1);
			}
		}
		done = fnvlist_lookup_boolean_value(out, ZPARITY_DONE);
		cursor = fnvlist_lookup_uint64(out, ZPARITY_NEXT_OFFSET);
		fnvlist_free(out);
	}
	if (!done || nread == 0 || !nocksum_tried) {
		(void) fprintf(stderr, "parity BLOCKS gave %llu entries, "
		    "done %d, checksum-off case run %d\n", (u_longlong_t)nread,
		    done, nocksum_tried);
		return (1);
	}
	(void) printf("read %llu blocks of txg %llu\n",
	    (u_longlong_t)nread, (u_longlong_t)applied);
	return (0);
}

static void
test_zpool_get(const char *pool)
{
	const char *strs[] = {ZPOOL_DEDUPCACHED_PROP_NAME};
	nvlist_t *optional = fnvlist_alloc();

	fnvlist_add_string_array(optional, ZPOOL_GET_PROPS_NAMES, strs, 1);

	IOC_INPUT_TEST(ZFS_IOC_POOL_GET_PROPS, pool, NULL, optional, 0);
	IOC_INPUT_TEST_INJECT(ZFS_IOC_POOL_GET_PROPS, pool, optional);

	nvlist_free(optional);
}

static void
zfs_ioc_input_tests(const char *pool)
{
	char filepath[] = "/tmp/ioc_test_file_XXXXXX";
	char dataset[ZFS_MAX_DATASET_NAME_LEN];
	char snapbase[ZFS_MAX_DATASET_NAME_LEN + 32];
	char snapshot[ZFS_MAX_DATASET_NAME_LEN + 32];
	char bookmark[ZFS_MAX_DATASET_NAME_LEN + 32];
	char backup[ZFS_MAX_DATASET_NAME_LEN];
	char clone[ZFS_MAX_DATASET_NAME_LEN];
	char clonesnap[ZFS_MAX_DATASET_NAME_LEN + 32];
	int tmpfd, err;

	/*
	 * Setup names and create a working dataset
	 */
	(void) snprintf(dataset, sizeof (dataset), "%s/test-fs", pool);
	(void) snprintf(snapbase, sizeof (snapbase), "%s@snapbase", dataset);
	(void) snprintf(snapshot, sizeof (snapshot), "%s@snapshot", dataset);
	(void) snprintf(bookmark, sizeof (bookmark), "%s#bookmark", dataset);
	(void) snprintf(clone, sizeof (clone), "%s/test-fs-clone", pool);
	(void) snprintf(clonesnap, sizeof (clonesnap), "%s@snap", clone);
	(void) snprintf(backup, sizeof (backup), "%s/backup", pool);

	err = lzc_create(dataset, LZC_DATSET_TYPE_ZFS, NULL, NULL, -1);
	if (err) {
		(void) fprintf(stderr, "could not create '%s': %s\n",
		    dataset, strerror(errno));
		exit(2);
	}

	tmpfd = mkstemp(filepath);
	if (tmpfd < 0) {
		(void) fprintf(stderr, "could not create '%s': %s\n",
		    filepath, strerror(errno));
		exit(2);
	}

	/*
	 * run a test for each ioctl
	 * Note that some test build on previous test operations
	 */
	test_pool_sync(pool);
	test_pool_reopen(pool);
	test_pool_checkpoint(pool);
	test_pool_discard_checkpoint(pool);
	test_log_history(pool);

	test_create(dataset);
	test_snapshot(pool, snapbase);
	test_snapshot(pool, snapshot);

	test_space_snaps(snapshot);
	test_send_space(snapbase, snapshot);
	test_send_new(snapshot, tmpfd);
	test_recv_new(backup, tmpfd);

	test_bookmark(pool, snapshot, bookmark);
	test_get_bookmarks(dataset);
	test_get_bookmark_props(bookmark);
	test_destroy_bookmarks(pool, bookmark);

	test_hold(pool, snapshot);
	test_get_holds(snapshot);
	test_release(pool, snapshot);

	test_clone(snapshot, clone);
	test_snapshot(pool, clonesnap);
	test_redact(snapshot, clonesnap);
	zfs_destroy(clonesnap);
	zfs_destroy(clone);

	test_rollback(dataset, snapshot);
	test_destroy_snaps(pool, snapshot);
	test_destroy_snaps(pool, snapbase);

	test_remap(dataset);
	test_channel_program(pool);

	test_load_key(dataset);
	test_change_key(dataset);
	test_unload_key(dataset);

	test_vdev_initialize(pool);
	test_vdev_trim(pool);

	test_wait(pool);
	test_wait_fs(dataset);

	test_set_bootenv(pool);
	test_get_bootenv(pool);

	test_parity_info(pool);
	test_parity_records(pool);
	test_parity_read(pool);
	test_parity_readv(pool);
	test_parity_verify(pool);
	test_parity_map(pool);
	test_parity_blocks(pool);
	test_parity_current(pool);
	test_parity_label(pool);
	test_parity_applied(pool);
	test_parity_repair(pool);
	test_parity_provider();
	test_parity_supply();
	test_parity_drop(pool);
	test_parity_reclaim(pool);

	test_scrub(pool);

	test_zpool_get(pool);
	/*
	 * cleanup
	 */
	zfs_cmd_t zc = {"\0"};

	nvlist_t *snaps = fnvlist_alloc();
	fnvlist_add_boolean(snaps, snapshot);
	(void) lzc_destroy_snaps(snaps, B_FALSE, NULL);
	nvlist_free(snaps);

	(void) zfs_destroy(dataset);
	(void) zfs_destroy(backup);

	(void) close(tmpfd);
	(void) unlink(filepath);

	/*
	 * All the unused slots should yield ZFS_ERR_IOC_CMD_UNAVAIL
	 */
	for (int i = 0; i < ARRAY_SIZE(ioc_skip); i++) {
		if (ioc_tested[ioc_skip[i] - ZFS_IOC_FIRST])
			(void) fprintf(stderr, "cmd %d tested, not skipped!\n",
			    (int)(ioc_skip[i] - ZFS_IOC_FIRST));

		ioc_tested[ioc_skip[i] - ZFS_IOC_FIRST] = B_TRUE;
	}

	(void) strlcpy(zc.zc_name, pool, sizeof (zc.zc_name));
	zc.zc_name[sizeof (zc.zc_name) - 1] = '\0';

	for (unsigned ioc = ZFS_IOC_FIRST; ioc < ZFS_IOC_LAST; ioc++) {
		unsigned cmd = ioc - ZFS_IOC_FIRST;

		if (ioc_tested[cmd])
			continue;

		if (lzc_ioctl_fd(zfs_fd, ioc, &zc) != 0 &&
		    errno != ZFS_ERR_IOC_CMD_UNAVAIL) {
			(void) fprintf(stderr, "cmd %d is missing a test case "
			    "(%d)\n", cmd, errno);
		}
	}
}

enum zfs_ioc_ref {
#ifdef __FreeBSD__
	ZFS_IOC_BASE = 0,
#else
	ZFS_IOC_BASE = ('Z' << 8),
#endif
	ZFS_IOC_PLATFORM_BASE = ZFS_IOC_BASE + 0x80,
};

/*
 * Canonical reference check of /dev/zfs ioctl numbers.
 * These cannot change and new ioctl numbers must be appended.
 */
static boolean_t
validate_ioc_values(void)
{
	boolean_t result = B_TRUE;

#define	CHECK(expr) do { \
	if (!(expr)) { \
		result = B_FALSE; \
		fprintf(stderr, "(%s) === FALSE\n", #expr); \
	} \
} while (0)

	CHECK(ZFS_IOC_BASE + 0 == ZFS_IOC_POOL_CREATE);
	CHECK(ZFS_IOC_BASE + 1 == ZFS_IOC_POOL_DESTROY);
	CHECK(ZFS_IOC_BASE + 2 == ZFS_IOC_POOL_IMPORT);
	CHECK(ZFS_IOC_BASE + 3 == ZFS_IOC_POOL_EXPORT);
	CHECK(ZFS_IOC_BASE + 4 == ZFS_IOC_POOL_CONFIGS);
	CHECK(ZFS_IOC_BASE + 5 == ZFS_IOC_POOL_STATS);
	CHECK(ZFS_IOC_BASE + 6 == ZFS_IOC_POOL_TRYIMPORT);
	CHECK(ZFS_IOC_BASE + 7 == ZFS_IOC_POOL_SCAN);
	CHECK(ZFS_IOC_BASE + 8 == ZFS_IOC_POOL_FREEZE);
	CHECK(ZFS_IOC_BASE + 9 == ZFS_IOC_POOL_UPGRADE);
	CHECK(ZFS_IOC_BASE + 10 == ZFS_IOC_POOL_GET_HISTORY);
	CHECK(ZFS_IOC_BASE + 11 == ZFS_IOC_VDEV_ADD);
	CHECK(ZFS_IOC_BASE + 12 == ZFS_IOC_VDEV_REMOVE);
	CHECK(ZFS_IOC_BASE + 13 == ZFS_IOC_VDEV_SET_STATE);
	CHECK(ZFS_IOC_BASE + 14 == ZFS_IOC_VDEV_ATTACH);
	CHECK(ZFS_IOC_BASE + 15 == ZFS_IOC_VDEV_DETACH);
	CHECK(ZFS_IOC_BASE + 16 == ZFS_IOC_VDEV_SETPATH);
	CHECK(ZFS_IOC_BASE + 17 == ZFS_IOC_VDEV_SETFRU);
	CHECK(ZFS_IOC_BASE + 18 == ZFS_IOC_OBJSET_STATS);
	CHECK(ZFS_IOC_BASE + 19 == ZFS_IOC_OBJSET_ZPLPROPS);
	CHECK(ZFS_IOC_BASE + 20 == ZFS_IOC_DATASET_LIST_NEXT);
	CHECK(ZFS_IOC_BASE + 21 == ZFS_IOC_SNAPSHOT_LIST_NEXT);
	CHECK(ZFS_IOC_BASE + 22 == ZFS_IOC_SET_PROP);
	CHECK(ZFS_IOC_BASE + 23 == ZFS_IOC_CREATE);
	CHECK(ZFS_IOC_BASE + 24 == ZFS_IOC_DESTROY);
	CHECK(ZFS_IOC_BASE + 25 == ZFS_IOC_ROLLBACK);
	CHECK(ZFS_IOC_BASE + 26 == ZFS_IOC_RENAME);
	CHECK(ZFS_IOC_BASE + 27 == ZFS_IOC_RECV);
	CHECK(ZFS_IOC_BASE + 28 == ZFS_IOC_SEND);
	CHECK(ZFS_IOC_BASE + 29 == ZFS_IOC_INJECT_FAULT);
	CHECK(ZFS_IOC_BASE + 30 == ZFS_IOC_CLEAR_FAULT);
	CHECK(ZFS_IOC_BASE + 31 == ZFS_IOC_INJECT_LIST_NEXT);
	CHECK(ZFS_IOC_BASE + 32 == ZFS_IOC_ERROR_LOG);
	CHECK(ZFS_IOC_BASE + 33 == ZFS_IOC_CLEAR);
	CHECK(ZFS_IOC_BASE + 34 == ZFS_IOC_PROMOTE);
	CHECK(ZFS_IOC_BASE + 35 == ZFS_IOC_SNAPSHOT);
	CHECK(ZFS_IOC_BASE + 36 == ZFS_IOC_DSOBJ_TO_DSNAME);
	CHECK(ZFS_IOC_BASE + 37 == ZFS_IOC_OBJ_TO_PATH);
	CHECK(ZFS_IOC_BASE + 38 == ZFS_IOC_POOL_SET_PROPS);
	CHECK(ZFS_IOC_BASE + 39 == ZFS_IOC_POOL_GET_PROPS);
	CHECK(ZFS_IOC_BASE + 40 == ZFS_IOC_SET_FSACL);
	CHECK(ZFS_IOC_BASE + 41 == ZFS_IOC_GET_FSACL);
	CHECK(ZFS_IOC_BASE + 42 == ZFS_IOC_SHARE);
	CHECK(ZFS_IOC_BASE + 43 == ZFS_IOC_INHERIT_PROP);
	CHECK(ZFS_IOC_BASE + 44 == ZFS_IOC_SMB_ACL);
	CHECK(ZFS_IOC_BASE + 45 == ZFS_IOC_USERSPACE_ONE);
	CHECK(ZFS_IOC_BASE + 46 == ZFS_IOC_USERSPACE_MANY);
	CHECK(ZFS_IOC_BASE + 47 == ZFS_IOC_USERSPACE_UPGRADE);
	CHECK(ZFS_IOC_BASE + 48 == ZFS_IOC_HOLD);
	CHECK(ZFS_IOC_BASE + 49 == ZFS_IOC_RELEASE);
	CHECK(ZFS_IOC_BASE + 50 == ZFS_IOC_GET_HOLDS);
	CHECK(ZFS_IOC_BASE + 51 == ZFS_IOC_OBJSET_RECVD_PROPS);
	CHECK(ZFS_IOC_BASE + 52 == ZFS_IOC_VDEV_SPLIT);
	CHECK(ZFS_IOC_BASE + 53 == ZFS_IOC_NEXT_OBJ);
	CHECK(ZFS_IOC_BASE + 54 == ZFS_IOC_DIFF);
	CHECK(ZFS_IOC_BASE + 55 == ZFS_IOC_TMP_SNAPSHOT);
	CHECK(ZFS_IOC_BASE + 56 == ZFS_IOC_OBJ_TO_STATS);
	CHECK(ZFS_IOC_BASE + 57 == ZFS_IOC_SPACE_WRITTEN);
	CHECK(ZFS_IOC_BASE + 58 == ZFS_IOC_SPACE_SNAPS);
	CHECK(ZFS_IOC_BASE + 59 == ZFS_IOC_DESTROY_SNAPS);
	CHECK(ZFS_IOC_BASE + 60 == ZFS_IOC_POOL_REGUID);
	CHECK(ZFS_IOC_BASE + 61 == ZFS_IOC_POOL_REOPEN);
	CHECK(ZFS_IOC_BASE + 62 == ZFS_IOC_SEND_PROGRESS);
	CHECK(ZFS_IOC_BASE + 63 == ZFS_IOC_LOG_HISTORY);
	CHECK(ZFS_IOC_BASE + 64 == ZFS_IOC_SEND_NEW);
	CHECK(ZFS_IOC_BASE + 65 == ZFS_IOC_SEND_SPACE);
	CHECK(ZFS_IOC_BASE + 66 == ZFS_IOC_CLONE);
	CHECK(ZFS_IOC_BASE + 67 == ZFS_IOC_BOOKMARK);
	CHECK(ZFS_IOC_BASE + 68 == ZFS_IOC_GET_BOOKMARKS);
	CHECK(ZFS_IOC_BASE + 69 == ZFS_IOC_DESTROY_BOOKMARKS);
	CHECK(ZFS_IOC_BASE + 70 == ZFS_IOC_RECV_NEW);
	CHECK(ZFS_IOC_BASE + 71 == ZFS_IOC_POOL_SYNC);
	CHECK(ZFS_IOC_BASE + 72 == ZFS_IOC_CHANNEL_PROGRAM);
	CHECK(ZFS_IOC_BASE + 73 == ZFS_IOC_LOAD_KEY);
	CHECK(ZFS_IOC_BASE + 74 == ZFS_IOC_UNLOAD_KEY);
	CHECK(ZFS_IOC_BASE + 75 == ZFS_IOC_CHANGE_KEY);
	CHECK(ZFS_IOC_BASE + 76 == ZFS_IOC_REMAP);
	CHECK(ZFS_IOC_BASE + 77 == ZFS_IOC_POOL_CHECKPOINT);
	CHECK(ZFS_IOC_BASE + 78 == ZFS_IOC_POOL_DISCARD_CHECKPOINT);
	CHECK(ZFS_IOC_BASE + 79 == ZFS_IOC_POOL_INITIALIZE);
	CHECK(ZFS_IOC_BASE + 80 == ZFS_IOC_POOL_TRIM);
	CHECK(ZFS_IOC_BASE + 81 == ZFS_IOC_REDACT);
	CHECK(ZFS_IOC_BASE + 82 == ZFS_IOC_GET_BOOKMARK_PROPS);
	CHECK(ZFS_IOC_BASE + 83 == ZFS_IOC_WAIT);
	CHECK(ZFS_IOC_BASE + 84 == ZFS_IOC_WAIT_FS);
	CHECK(ZFS_IOC_BASE + 87 == ZFS_IOC_POOL_SCRUB);
	CHECK(ZFS_IOC_BASE + 91 == ZFS_IOC_PARITY_INFO);
	CHECK(ZFS_IOC_BASE + 92 == ZFS_IOC_PARITY_RECORDS);
	CHECK(ZFS_IOC_BASE + 93 == ZFS_IOC_PARITY_READ);
	CHECK(ZFS_IOC_BASE + 94 == ZFS_IOC_PARITY_VERIFY);
	CHECK(ZFS_IOC_BASE + 95 == ZFS_IOC_PARITY_MAP);
	CHECK(ZFS_IOC_BASE + 96 == ZFS_IOC_PARITY_BLOCKS);
	CHECK(ZFS_IOC_BASE + 97 == ZFS_IOC_PARITY_LABEL);
	CHECK(ZFS_IOC_BASE + 98 == ZFS_IOC_PARITY_APPLIED);
	CHECK(ZFS_IOC_BASE + 99 == ZFS_IOC_PARITY_REPAIR);
	CHECK(ZFS_IOC_BASE + 100 == ZFS_IOC_PARITY_DROP);
	CHECK(ZFS_IOC_BASE + 101 == ZFS_IOC_PARITY_READV);
	CHECK(ZFS_IOC_BASE + 102 == ZFS_IOC_PARITY_CURRENT);
	CHECK(ZFS_IOC_BASE + 103 == ZFS_IOC_PARITY_PROVIDER);
	CHECK(ZFS_IOC_BASE + 104 == ZFS_IOC_PARITY_SUPPLY);
	CHECK(ZFS_IOC_BASE + 105 == ZFS_IOC_PARITY_RECLAIM);
	CHECK(ZFS_IOC_PLATFORM_BASE + 1 == ZFS_IOC_EVENTS_NEXT);
	CHECK(ZFS_IOC_PLATFORM_BASE + 2 == ZFS_IOC_EVENTS_CLEAR);
	CHECK(ZFS_IOC_PLATFORM_BASE + 3 == ZFS_IOC_EVENTS_SEEK);
	CHECK(ZFS_IOC_PLATFORM_BASE + 4 == ZFS_IOC_NEXTBOOT);
	CHECK(ZFS_IOC_PLATFORM_BASE + 5 == ZFS_IOC_JAIL);
	CHECK(ZFS_IOC_PLATFORM_BASE + 6 == ZFS_IOC_UNJAIL);
	CHECK(ZFS_IOC_PLATFORM_BASE + 7 == ZFS_IOC_SET_BOOTENV);
	CHECK(ZFS_IOC_PLATFORM_BASE + 8 == ZFS_IOC_GET_BOOTENV);

#undef CHECK

	return (result);
}

int
main(int argc, const char *argv[])
{
	boolean_t parity_identity = B_FALSE;
	boolean_t parity_blocks = B_FALSE;
	boolean_t parity_malformed = B_FALSE;
	boolean_t parity_policy = B_FALSE;
	boolean_t parity_baseline = B_FALSE;
	boolean_t parity_label_current = B_FALSE;
	boolean_t parity_read_hash = B_FALSE, read_hash_compressed = B_FALSE;
	boolean_t parity_label_all = B_FALSE;
	boolean_t parity_apply = B_FALSE;
	boolean_t parity_apply_last = B_FALSE;
	boolean_t parity_history_gap = B_FALSE;
	boolean_t parity_blocks_read = B_FALSE;
	boolean_t parity_readv = B_FALSE;
	boolean_t parity_dtl_read = B_FALSE;
	boolean_t parity_blocks_windows = B_FALSE;
	boolean_t parity_map = B_FALSE;
	boolean_t parity_map_stress = B_FALSE;
	boolean_t parity_columns = B_FALSE;
	uint_t expected_columns = 0;
	const char *const *expected_paths = NULL;
	uint_t nexpected_paths = 0;
	const char *pool;

	if (argc == 2) {
		pool = argv[1];
	} else if (argc == 3 && strcmp(argv[1], "--parity-identity") == 0) {
		pool = argv[2];
		parity_identity = B_TRUE;
	} else if (argc == 3 && strcmp(argv[1], "--parity-blocks") == 0) {
		pool = argv[2];
		parity_blocks = B_TRUE;
	} else if (argc == 3 &&
	    strcmp(argv[1], "--parity-blocks-windows") == 0) {
		pool = argv[2];
		parity_blocks_windows = B_TRUE;
	} else if (argc == 3 &&
	    strcmp(argv[1], "--parity-malformed") == 0) {
		pool = argv[2];
		parity_malformed = B_TRUE;
	} else if (argc == 3 && strcmp(argv[1], "--parity-policy") == 0) {
		pool = argv[2];
		parity_policy = B_TRUE;
	} else if (argc == 3 &&
	    strcmp(argv[1], "--parity-baseline") == 0) {
		pool = argv[2];
		parity_baseline = B_TRUE;
	} else if (argc == 3 &&
	    strcmp(argv[1], "--parity-label-current") == 0) {
		pool = argv[2];
		parity_label_current = B_TRUE;
	} else if (argc == 3 &&
	    strcmp(argv[1], "--parity-label-all") == 0) {
		pool = argv[2];
		parity_label_all = B_TRUE;
	} else if ((argc == 3 || (argc == 4 &&
	    strcmp(argv[3], "--compressed") == 0)) &&
	    strcmp(argv[1], "--parity-read-hash") == 0) {
		pool = argv[2];
		parity_read_hash = B_TRUE;
		read_hash_compressed = (argc == 4);
	} else if (argc == 3 && strcmp(argv[1], "--parity-apply") == 0) {
		pool = argv[2];
		parity_apply = B_TRUE;
	} else if (argc == 3 &&
	    strcmp(argv[1], "--parity-apply-last") == 0) {
		pool = argv[2];
		parity_apply_last = B_TRUE;
	} else if (argc == 3 &&
	    strcmp(argv[1], "--parity-history-gap") == 0) {
		pool = argv[2];
		parity_history_gap = B_TRUE;
	} else if (argc == 3 &&
	    strcmp(argv[1], "--parity-blocks-read") == 0) {
		pool = argv[2];
		parity_blocks_read = B_TRUE;
	} else if (argc == 3 && strcmp(argv[1], "--parity-readv") == 0) {
		pool = argv[2];
		parity_readv = B_TRUE;
	} else if (argc == 3 &&
	    strcmp(argv[1], "--parity-dtl-read") == 0) {
		pool = argv[2];
		parity_dtl_read = B_TRUE;
	} else if (argc == 3 && strcmp(argv[1], "--parity-map") == 0) {
		pool = argv[2];
		parity_map = B_TRUE;
	} else if (argc == 3 &&
	    strcmp(argv[1], "--parity-map-stress") == 0) {
		pool = argv[2];
		parity_map_stress = B_TRUE;
	} else if (argc >= 4 &&
	    strcmp(argv[1], "--parity-columns") == 0) {
		char *end;
		unsigned long count;

		pool = argv[2];
		errno = 0;
		count = strtoul(argv[3], &end, 10);
		if (errno != 0 || end == argv[3] || *end != '\0' ||
		    count > UINT_MAX || argc == 4) {
			(void) fprintf(stderr,
			    "invalid parity column arguments\n");
			exit(2);
		}
		expected_columns = (uint_t)count;
		expected_paths = &argv[4];
		nexpected_paths = (uint_t)(argc - 4);
		parity_columns = B_TRUE;
	} else {
		(void) fprintf(stderr, "usage: %s [--parity-identity | "
		    "--parity-blocks | --parity-blocks-windows | "
		    "--parity-malformed | "
		    "--parity-policy | --parity-baseline | "
		    "--parity-label-current | --parity-label-all | "
		    "--parity-read-hash | "
		    "--parity-apply | "
		    "--parity-apply-last | --parity-history-gap | "
		    "--parity-blocks-read | --parity-readv | "
		    "--parity-dtl-read | --parity-map | "
		    "--parity-map-stress] <pool>\n"
		    "       %s --parity-columns <pool> <count> <leaf>...\n",
		    argv[0], argv[0]);
		exit(2);
	}

	if (!validate_ioc_values()) {
		(void) fprintf(stderr, "WARNING: zfs_ioc_t has binary "
		    "incompatible command values\n");
		exit(3);
	}

	(void) libzfs_core_init();
	zfs_fd = open(ZFS_DEV, O_RDWR);
	if (zfs_fd < 0) {
		(void) fprintf(stderr, "open: %s\n", strerror(errno));
		libzfs_core_fini();
		exit(2);
	}

	if (parity_identity)
		unexpected_failures = test_parity_identity(pool) != 0;
	else if (parity_blocks)
		unexpected_failures = test_parity_blocks_pages(pool) != 0;
	else if (parity_blocks_windows)
		unexpected_failures = test_parity_blocks_windows(pool) != 0;
	else if (parity_malformed)
		unexpected_failures = test_parity_malformed(pool) != 0;
	else if (parity_policy)
		unexpected_failures = test_parity_policy(pool) != 0;
	else if (parity_baseline)
		unexpected_failures = test_parity_baseline(pool) != 0;
	else if (parity_read_hash)
		unexpected_failures = test_parity_read_hash(pool,
		    read_hash_compressed) != 0;
	else if (parity_label_current)
		unexpected_failures = test_parity_label_current(pool) != 0;
	else if (parity_label_all)
		unexpected_failures = test_parity_label_all(pool) != 0;
	else if (parity_apply)
		unexpected_failures = test_parity_apply(pool) != 0;
	else if (parity_apply_last)
		unexpected_failures = test_parity_apply_last(pool) != 0;
	else if (parity_history_gap)
		unexpected_failures = test_parity_history_gap(pool) != 0;
	else if (parity_blocks_read)
		unexpected_failures = test_parity_blocks_read(pool) != 0;
	else if (parity_readv)
		unexpected_failures = test_parity_readv_once(pool) != 0;
	else if (parity_dtl_read)
		unexpected_failures = test_parity_dtl_read(pool) != 0;
	else if (parity_map)
		unexpected_failures = test_parity_map_epoch(pool) != 0;
	else if (parity_map_stress)
		unexpected_failures = test_parity_map_stress(pool) != 0;
	else if (parity_columns)
		unexpected_failures = test_parity_columns(pool,
		    expected_columns, expected_paths, nexpected_paths) != 0;
	else
		zfs_ioc_input_tests(pool);

	(void) close(zfs_fd);
	libzfs_core_fini();

	return (unexpected_failures);
}
