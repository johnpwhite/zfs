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

# A leaked range stays out of the space that the DSL gives to writers.

verify_runnable "global"
parity_require
parity_require_tunable PARITY_TEST_INJECT

function cleanup
{
	parity_set_tunable PARITY_TEST_INJECT 0
	parity_cleanup
}
log_onexit cleanup

log_assert "leaked parity space does not suspend a full pool"

log_must truncate -s 128M $PARITY_VDEV
log_must zpool create -f $TESTPOOL $PARITY_VDEV
log_must zfs set compression=off $TESTPOOL
parity_arm
parity_write /$TESTPOOL/data 24
log_must rm /$TESTPOOL/data
sync_pool $TESTPOOL
log_must test "$(parity_held)" -gt 0

log_must set_tunable64 PARITY_TEST_INJECT 32
log_must zpool set parityset=off $TESTPOOL
log_must zpool parity drop -f $TESTPOOL
log_must parity_wait 60 \
    'sync_pool $TESTPOOL; test "$(parity_state)" == "off"'
log_must set_tunable64 PARITY_TEST_INJECT 0

typeset -i leaked=$(parity_status_value leaked_bytes)
typeset -i zap_leaked=$(parity_leaked_zap)
typeset -i slop=$((128 * 1024 * 1024 * 3 / 100))
log_must test $leaked -gt $slop
log_must test $leaked -eq $zap_leaked
log_mustnot dd if=/dev/zero of=/$TESTPOOL/fill bs=1M count=256
log_must test "$(zpool status -x $TESTPOOL)" == \
    "pool '$TESTPOOL' is healthy"

log_pass "leaked parity space does not suspend a full pool"
