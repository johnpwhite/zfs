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
 * Copyright (c) 2026 John White. All rights reserved.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/zfs_ioctl.h>
#include <sys/fm/protocol.h>
#include <sys/nvpair.h>
#include <errno.h>
#include <fcntl.h>
#include <libzfs.h>
#include <libzfs_core.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	PARITY_LABEL_START_SIZE	(4ULL * 1024 * 1024)
#define	PARITY_REPAIR_EVENT	"sysevent.fs.zfs.parity_repair_needed"
#define	PARITY_MAX_BLOCK_SIZE	(16 * 1024 * 1024)

static volatile sig_atomic_t repair_timed_out;

static void
repair_alarm(int signo)
{
	(void) signo;
	repair_timed_out = 1;
}

static int
read_file(const char *path, uint8_t **bufp, size_t *lenp)
{
	struct stat st;
	uint8_t *buf;
	size_t done = 0;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return (-1);
	if (fstat(fd, &st) != 0 || st.st_size <= 0 ||
	    st.st_size > PARITY_MAX_BLOCK_SIZE) {
		(void) close(fd);
		return (-1);
	}
	buf = malloc((size_t)st.st_size);
	if (buf == NULL) {
		(void) close(fd);
		return (-1);
	}
	while (done < (size_t)st.st_size) {
		ssize_t n = read(fd, buf + done, (size_t)st.st_size - done);

		if (n <= 0) {
			free(buf);
			(void) close(fd);
			return (-1);
		}
		done += n;
	}
	(void) close(fd);
	*bufp = buf;
	*lenp = (size_t)st.st_size;
	return (0);
}

static int
corrupt_byte(const char *path, uint64_t dva_offset)
{
	uint64_t offset;
	uint8_t byte;
	int fd, error = -1;

	if (dva_offset > UINT64_MAX - PARITY_LABEL_START_SIZE)
		return (-1);
	offset = dva_offset + PARITY_LABEL_START_SIZE;
	if ((uint64_t)(off_t)offset != offset)
		return (-1);
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return (-1);
	if (pread(fd, &byte, 1, offset) != 1)
		goto out;
	byte ^= 0xff;
	if (pwrite(fd, &byte, 1, offset) != 1 || fsync(fd) != 0)
		goto out;
	error = 0;
out:
	(void) close(fd);
	return (error);
}

static int
write_result(const char *path, uint64_t offset, uint64_t asize)
{
	FILE *file = fopen(path, "w");

	if (file == NULL)
		return (1);
	if (fprintf(file, "%llu %llu\n", (unsigned long long)offset,
	    (unsigned long long)asize) < 0) {
		(void) fclose(file);
		return (1);
	}
	return (fclose(file) == 0 ? 0 : 1);
}

static int
wait_for_repair(libzfs_handle_t *hdl, int event_fd, uint64_t pool_guid,
    boolean_t want_sync, nvlist_t **eventp)
{
	struct sigaction action = { 0 };
	int dropped, error;
	const char *class;

	action.sa_handler = repair_alarm;
	(void) sigemptyset(&action.sa_mask);
	if (sigaction(SIGALRM, &action, NULL) != 0)
		return (-1);
	alarm(30);
	for (;;) {
		error = zpool_events_next(hdl, eventp, &dropped, 0, event_fd);
		if (error != 0 || repair_timed_out || *eventp == NULL)
			break;
		if (nvlist_lookup_string(*eventp, FM_CLASS, &class) == 0 &&
		    strcmp(class, PARITY_REPAIR_EVENT) == 0) {
			uint64_t event_pool_guid;
			boolean_t sw;

			/*
			 * The registration posts the pending requests again
			 * (sync_wait false): a provider that waits for a
			 * waiting read skips them, and the other way round.
			 */
			if (nvlist_lookup_uint64(*eventp, "pool_guid",
			    &event_pool_guid) == 0 &&
			    event_pool_guid == pool_guid &&
			    nvlist_lookup_boolean_value(*eventp, "sync_wait",
			    &sw) == 0 && sw == want_sync)
				break;
		}
		nvlist_free(*eventp);
		*eventp = NULL;
	}
	alarm(0);
	if (error != 0 || repair_timed_out || *eventp == NULL)
		return (-1);
	return (0);
}

/*
 * "provider": register for the pool, wait for one repair event, and supply
 * the bytes of source.  "decline" (source is "-"): answer that the block
 * cannot be rebuilt, so the waiting read fails at once.  "repost": no read
 * waits; registering posts the pending request again (sync_wait false), and
 * the tool repairs it with REPAIR, as the daemon does.
 * The event stream is opened before the registration, so that the events
 * that the registration posts again are seen.
 */
#define	PROVIDER_SUPPLY		0
#define	PROVIDER_DECLINE	1
#define	PROVIDER_REPOST		2

static int
provider(const char *pool, const char *source, const char *ready,
    const char *result, int how)
{
	boolean_t decline = (how == PROVIDER_DECLINE);
	boolean_t sync_wait, repaired;
	libzfs_handle_t *hdl = NULL;
	nvlist_t *event = NULL;
	nvlist_t *info = NULL;
	uint8_t *data = NULL;
	FILE *file;
	size_t len;
	uint64_t pool_guid, request, offset, asize;
	int event_fd = -1;
	int error = 1;

	if (!decline && read_file(source, &data, &len) != 0) {
		(void) fprintf(stderr, "cannot read source block\n");
		goto out;
	}
	hdl = libzfs_init();
	if (hdl == NULL) {
		(void) fprintf(stderr, "libzfs_init: %s\n", strerror(errno));
		goto out;
	}
	if ((error = lzc_parity_info(pool, &info)) != 0) {
		(void) fprintf(stderr, "parity INFO: %s\n", strerror(error));
		error = 1;
		goto out;
	}
	pool_guid = fnvlist_lookup_uint64(info, ZPARITY_POOL_GUID);
	fnvlist_free(info);
	info = NULL;
	event_fd = open(ZFS_DEV, O_RDWR | O_CLOEXEC);
	if (event_fd < 0) {
		(void) fprintf(stderr, "open %s: %s\n", ZFS_DEV,
		    strerror(errno));
		goto out;
	}
	if (zpool_events_seek(hdl, ZEVENT_SEEK_END, event_fd) != 0)
		goto out;
	if ((error = lzc_parity_provider_register(pool_guid)) != 0) {
		(void) fprintf(stderr, "provider registration: %s\n",
		    strerror(error));
		error = (error == EBUSY) ? 3 : 1;
		goto out;
	}
	file = fopen(ready, "w");
	if (file == NULL)
		goto out;
	(void) fclose(file);
	if (wait_for_repair(hdl, event_fd, pool_guid,
	    how != PROVIDER_REPOST, &event) != 0) {
		(void) fprintf(stderr, "no repair event before timeout\n");
		goto out;
	}
	if (nvlist_lookup_uint64(event, "request", &request) != 0 ||
	    nvlist_lookup_uint64(event, "offset", &offset) != 0 ||
	    nvlist_lookup_uint64(event, "asize", &asize) != 0 ||
	    (!decline && asize != len)) {
		(void) fprintf(stderr,
		    "repair event does not match source block\n");
		goto out;
	}
	if (nvlist_lookup_boolean_value(event, "sync_wait", &sync_wait) != 0 ||
	    sync_wait != (how != PROVIDER_REPOST)) {
		(void) fprintf(stderr, "repair event has a wrong sync_wait\n");
		goto out;
	}
	if (how == PROVIDER_DECLINE) {
		error = lzc_parity_repair_decline(pool_guid, request);
	} else if (how == PROVIDER_REPOST) {
		error = lzc_parity_repair(pool, request, data, len, &repaired);
		if (error == 0 && !repaired)
			error = ECKSUM;
	} else {
		error = lzc_parity_repair_supply(pool_guid, request, data,
		    len);
	}
	if (error != 0) {
		(void) fprintf(stderr, "supply repair answer: %s\n",
		    strerror(error));
		error = 1;
		goto out;
	}
	if (write_result(result, offset, asize) != 0) {
		error = 1;
		goto out;
	}
	error = 0;
out:
	if (event != NULL)
		nvlist_free(event);
	if (info != NULL)
		fnvlist_free(info);
	if (event_fd >= 0)
		(void) close(event_fd);
	if (hdl != NULL)
		libzfs_fini(hdl);
	free(data);
	return (error);
}

/*
 * "late": the daemon path.  No provider is registered, so the read fails at
 * once and its request waits for the daemon.  After the repair event, wait
 * for the file "go", touch "started" and send the bytes with
 * lzc_parity_repair() (ZFS_IOC_PARITY_REPAIR), as the daemon does.  The
 * test frees the block while that write is held (zfs_parity_test_inject 64).
 * Prints "repaired", "not repaired" or the error.
 */
static int
late(const char *pool, const char *source, const char *ready,
    const char *go, const char *started)
{
	libzfs_handle_t *hdl = NULL;
	nvlist_t *event = NULL;
	nvlist_t *info = NULL;
	uint8_t *data = NULL;
	FILE *file;
	size_t len;
	uint64_t pool_guid, request, asize;
	boolean_t repaired = B_FALSE;
	int event_fd = -1;
	int error = 1;

	if (read_file(source, &data, &len) != 0) {
		(void) fprintf(stderr, "cannot read source block\n");
		goto out;
	}
	hdl = libzfs_init();
	if (hdl == NULL)
		goto out;
	if (lzc_parity_info(pool, &info) != 0)
		goto out;
	pool_guid = fnvlist_lookup_uint64(info, ZPARITY_POOL_GUID);
	event_fd = open(ZFS_DEV, O_RDWR | O_CLOEXEC);
	if (event_fd < 0 ||
	    zpool_events_seek(hdl, ZEVENT_SEEK_END, event_fd) != 0)
		goto out;
	if ((file = fopen(ready, "w")) == NULL)
		goto out;
	(void) fclose(file);
	if (wait_for_repair(hdl, event_fd, pool_guid, B_FALSE, &event) != 0) {
		(void) fprintf(stderr, "no repair event before timeout\n");
		goto out;
	}
	if (nvlist_lookup_uint64(event, "request", &request) != 0 ||
	    nvlist_lookup_uint64(event, "asize", &asize) != 0 ||
	    asize != len) {
		(void) fprintf(stderr,
		    "repair event does not match source block\n");
		goto out;
	}
	for (int i = 0; access(go, F_OK) != 0; i++) {
		if (i == 300)
			goto out;
		(void) usleep(100000);
	}
	if ((file = fopen(started, "w")) == NULL)
		goto out;
	(void) fclose(file);
	error = lzc_parity_repair(pool, request, data, len, &repaired);
	if (error != 0)
		(void) printf("repair: %s\n", strerror(error));
	else
		(void) printf("%s\n", repaired ? "repaired" : "not repaired");
	error = 0;
out:
	if (event != NULL)
		nvlist_free(event);
	if (info != NULL)
		fnvlist_free(info);
	if (event_fd >= 0)
		(void) close(event_fd);
	if (hdl != NULL)
		libzfs_fini(hdl);
	free(data);
	return (error);
}

int
main(int argc, char *argv[])
{
	uint64_t offset;

	if (argc == 6 && strcmp(argv[1], "provider") == 0)
		return (provider(argv[2], argv[3], argv[4], argv[5],
		    PROVIDER_SUPPLY));
	if (argc == 5 && strcmp(argv[1], "decline") == 0)
		return (provider(argv[2], "-", argv[3], argv[4],
		    PROVIDER_DECLINE));
	if (argc == 6 && strcmp(argv[1], "repost") == 0)
		return (provider(argv[2], argv[3], argv[4], argv[5],
		    PROVIDER_REPOST));
	if (argc == 7 && strcmp(argv[1], "late") == 0)
		return (late(argv[2], argv[3], argv[4], argv[5], argv[6]));
	if (argc == 4 && strcmp(argv[1], "corrupt") == 0) {
		offset = strtoull(argv[3], NULL, 10);
		return (corrupt_byte(argv[2], offset) == 0 ? 0 : 1);
	}
	(void) fprintf(stderr, "usage: %s provider pool source ready result\n"
	    "       %s decline pool ready result\n"
	    "       %s repost pool source ready result\n"
	    "       %s late pool source ready go started\n"
	    "       %s corrupt vdev dva_offset\n", argv[0], argv[0], argv[0],
	    argv[0], argv[0]);
	return (2);
}
