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
# A mirror member with one missing side is a normal degraded pool: ZFS
# reads from the other side.  READ and READV must accept its entries.
# Before the fix every entry failed with EINVAL, because the size check
# also counted the missing side (never opened, size 0), and the daemon
# stopped applying the pool.
#
# STRATEGY:
# 1. Create a 2-way mirror member, turn the parity set on, write data.
# 2. Export, take one side away, import: the pool is DEGRADED.
# 3. Write, report the newest txg as applied, write again.
# 4. READ and READV every BLOCKS entry of the applied txg.
#

verify_runnable "global"
parity_require

function cleanup
{
	parity_cols_cleanup
	[[ -f "$PARITY_COLS_DIR.vdev1.away" ]] && \
	    mv "$PARITY_COLS_DIR.vdev1.away" "$PARITY_COLS_VDEV1"
}
log_onexit cleanup

log_assert "READ accepts the entries of a mirror member with a missing side"
parity_cols_prepare

log_must zpool create -f "$TESTPOOL" mirror "$PARITY_COLS_VDEV0" \
	    "$PARITY_COLS_VDEV1"
parity_arm
parity_write /$TESTPOOL/data 4

log_must zpool export "$TESTPOOL"
# outside the folder: import -d scans every file in it
log_must mv "$PARITY_COLS_VDEV1" "$PARITY_COLS_DIR.vdev1.away"
log_must zpool import -d "$PARITY_COLS_DIR" "$TESTPOOL"
log_must check_pool_status "$TESTPOOL" "state" "DEGRADED"

parity_write /$TESTPOOL/more 4
log_must libzfs_input_check --parity-apply $TESTPOOL
parity_write /$TESTPOOL/more2 2
parity_write /$TESTPOOL/more3 1
log_must libzfs_input_check --parity-blocks-read $TESTPOOL

log_pass "READ accepts the entries of a mirror member with a missing side"
