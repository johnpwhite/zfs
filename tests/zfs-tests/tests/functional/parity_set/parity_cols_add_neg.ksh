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

verify_runnable "global"
parity_require

function cleanup
{
	parity_cols_cleanup
}
log_onexit cleanup

log_assert "a parity set member refuses a raidz vdev and one larger than parity"
parity_cols_prepare

log_must zpool create -f "$TESTPOOL" "$PARITY_COLS_VDEV0"
log_must zpool set parityset=on "$TESTPOOL"
parity_cols_check_columns "$TESTPOOL" 1 "$PARITY_COLS_VDEV0"

# -f overrides the replication mismatch, so only the parity set can refuse
log_mustnot_expect "parity set" zpool add -f "$TESTPOOL" raidz \
	    "$PARITY_COLS_VDEV1" "$PARITY_COLS_VDEV2" "$PARITY_COLS_VDEV3"
parity_cols_check_columns "$TESTPOOL" 1 "$PARITY_COLS_VDEV0"

# A data vdev larger than the largest one (twice the size) is refused,
# until parity_grow_max grants that size
big=$PARITY_COLS_DIR/big
log_must truncate -s 0 "$big"
log_must truncate -s $PARITY_VDEV_SIZE "$big"
log_must truncate -s +$PARITY_VDEV_SIZE "$big"
log_mustnot_expect "parity set covers" zpool add -f "$TESTPOOL" "$big"
parity_cols_check_columns "$TESTPOOL" 1 "$PARITY_COLS_VDEV0"
log_must zpool set parity_grow_max=$(stat -c %s "$big") "$TESTPOOL"
log_must zpool add -f "$TESTPOOL" "$big"
parity_cols_check_columns "$TESTPOOL" 2 "$PARITY_COLS_VDEV0" "$big"

log_pass "a parity set member refuses a raidz vdev and one larger than parity"
