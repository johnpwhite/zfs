#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# https://opensource.org/license/CDDL-1.0.
#

#
# Copyright (c) 2026 John White. All rights reserved.
#

. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
# A pool exports cleanly while a scrub runs.  Export writes the txg time
# database into the MOS directory; a running scrub writes its state into
# the same object in every txg.  The export write must be made in syncing
# context, or a debug build trips the txg-order assertion in dbuf_dirty().
#
# STRATEGY:
# 1. Create a pool with data, and make the scrub slow.
# 2. Many times: start a scrub, let it run for a moment, export the pool
#    while the scrub runs, and import it again.
# 3. The pool must import and be healthy.  (The race is a timing window:
#    each round makes a hit likely on unfixed code, but cannot force it.)
#

verify_runnable "global"

DEVICE_DIR=$TEST_BASE_DIR/dev_export_scrub
POOLNAME=export_scrub_pool
ROUNDS=20

function cleanup
{
	restore_tunable SCAN_VDEV_LIMIT
	poolexists $POOLNAME && destroy_pool $POOLNAME
	[[ -d $DEVICE_DIR ]] && log_must rm -rf $DEVICE_DIR
}

log_assert "A pool exports cleanly while a scrub runs"
log_onexit cleanup

log_must save_tunable SCAN_VDEV_LIMIT
log_must mkdir -p $DEVICE_DIR
# 1200 files of 128 KiB (150 MiB) do not fit in $MINVDEVSIZE (256 MiB): ZFS
# keeps at least 128 MiB of slop space.  The file is sparse.
log_must truncate -s 1G $DEVICE_DIR/disk0
log_must zpool create -O compression=off $POOLNAME $DEVICE_DIR/disk0
log_must fill_fs /$POOLNAME 1 1200 131072 1 R
sync_pool $POOLNAME
log_must set_tunable64 SCAN_VDEV_LIMIT 4096

typeset -i i=0
while (( i < ROUNDS )); do
	log_must zpool scrub $POOLNAME
	sleep 1
	is_pool_scrubbing $POOLNAME || log_note "round $i: scrub ended early"
	log_must zpool export $POOLNAME
	log_must zpool import -d $DEVICE_DIR $POOLNAME
	(( i += 1 ))
done

log_must zpool scrub -w $POOLNAME
log_must check_pool_status $POOLNAME "errors" "No known data errors"
log_pass "A pool exports cleanly while a scrub runs"
