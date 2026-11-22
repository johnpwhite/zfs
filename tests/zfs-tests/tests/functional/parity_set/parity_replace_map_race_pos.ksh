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

# MAP and BLOCKS remain consistent while a member vdev is replaced.

verify_runnable "global"
parity_require

typeset MAP_LOG=$PARITY_DIR/map.$$
typeset MAP_PID=""

function cleanup
{
	if [[ -n $MAP_PID ]]; then
		kill $MAP_PID 2>/dev/null
		wait $MAP_PID 2>/dev/null
	fi
	rm -f $MAP_LOG
	parity_cleanup
}
log_onexit cleanup

log_assert "MAP stays sane while replacing a parity member"

parity_create_pool
log_must truncate -s $PARITY_VDEV_SIZE $PARITY_VDEV2
parity_arm
parity_write /$TESTPOOL/data 48
log_must libzfs_input_check --parity-blocks $TESTPOOL

libzfs_input_check --parity-map-stress $TESTPOOL > $MAP_LOG 2>&1 &
MAP_PID=$!

log_must zpool replace $TESTPOOL $PARITY_VDEV $PARITY_VDEV2
wait_replacing $TESTPOOL 120
log_must zpool replace -f $TESTPOOL $PARITY_VDEV2 $PARITY_VDEV
wait_replacing $TESTPOOL 120
log_must zpool replace -f $TESTPOOL $PARITY_VDEV $PARITY_VDEV2
wait_replacing $TESTPOOL 120

wait $MAP_PID || log_fail "MAP stress failed: $(<$MAP_LOG)"
MAP_PID=""
log_must grep -q "parity MAP stayed within" $MAP_LOG
log_mustnot is_pool_replacing $TESTPOOL

log_pass "MAP stays sane while replacing a parity member"
