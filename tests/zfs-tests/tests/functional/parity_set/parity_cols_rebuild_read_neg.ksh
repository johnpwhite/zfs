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

. $STF_SUITE/tests/functional/parity_set/parity_set.kshlib

#
# DESCRIPTION:
# READ must not use a readable replacement side that has not rebuilt the
# requested block.
#
# STRATEGY:
# 1. Create a parity member and write blocks with pending records.
# 2. Start a sequential rebuild and pause it before it copies any data.
# 3. Make every read of the original disk fail (zinject).
# 4. Require an indexed checksum-off READ to fail with EIO.
#

verify_runnable "global"
parity_require

typeset ORIG_SCAN_SUSPEND_PROGRESS=$(get_tunable SCAN_SUSPEND_PROGRESS)
typeset ORIG_REBUILD_SCRUB_ENABLED=$(get_tunable REBUILD_SCRUB_ENABLED)

function cleanup
{
	zinject -c all >/dev/null 2>&1
	log_must set_tunable32 SCAN_SUSPEND_PROGRESS \
	    $ORIG_SCAN_SUSPEND_PROGRESS
	log_must set_tunable32 REBUILD_SCRUB_ENABLED \
	    $ORIG_REBUILD_SCRUB_ENABLED
	parity_cols_cleanup
}
log_onexit cleanup

log_assert "READ skips a replacement side with missing data"
parity_cols_prepare

log_must zpool create -f "$TESTPOOL" "$PARITY_COLS_VDEV0"
parity_arm
log_must zfs set compression=off recordsize=4k $TESTPOOL
parity_write /$TESTPOOL/data 4

log_must set_tunable32 REBUILD_SCRUB_ENABLED 0
log_must set_tunable32 SCAN_SUSPEND_PROGRESS 1
log_must zpool attach -s "$TESTPOOL" "$PARITY_COLS_VDEV0" \
	    "$PARITY_COLS_VDEV1"
log_must parity_wait 30 \
	    'zpool history -i $TESTPOOL | grep -q "rebuild.*started"'
# ZFS refuses to offline the only complete side during the rebuild: make
# every read of it fail instead, so READ must not fall back to the new side
log_must zinject -d "$PARITY_COLS_VDEV0" -e io -T read -f 100 "$TESTPOOL"
log_must libzfs_input_check --parity-dtl-read "$TESTPOOL"

log_pass "READ skips a replacement side with missing data"
