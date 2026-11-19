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
# A forced drop whose records cannot be read ends, frees no unknown block,
# and keeps the held space that it could not free visible as leaked_bytes,
# also after the records are gone and after an export and import.
#
# STRATEGY:
# 1. Arm a pool, write and remove a file, so the pool holds freed space.
#    Check that leaked_bytes is 0.
# 2. Make every record read at release fail (test_inject bit 32), turn
#    parityset off and force a drop. Wait for the state "off".
# 3. Check that held_bytes is 0, that leaked_bytes is at least the held
#    space before the drop, and that the free space did not come back.
# 4. Export and import: leaked_bytes stays. The pool is healthy.
#

verify_runnable "global"
parity_require
parity_require_tunable PARITY_TEST_INJECT

function cleanup
{
	parity_set_tunable PARITY_TEST_INJECT 0
	parity_cleanup
}
log_onexit cleanup

typeset -i mb=$PARITY_DATA_MB

log_assert "a drop of unreadable records keeps the lost space visible"

parity_create_pool
parity_arm
log_must test "$(parity_status_value leaked_bytes)" == "0"
parity_write /$TESTPOOL/data $mb
log_must rm /$TESTPOOL/data
sync_pool $TESTPOOL
typeset -i held0=$(parity_held)
log_note "held before the drop: $held0"
log_must test $held0 -gt 0
typeset -i free0=$(parity_free)

log_must set_tunable64 PARITY_TEST_INJECT 32
log_must zpool set parityset=off $TESTPOOL
log_must zpool parity drop -f $TESTPOOL
log_must parity_wait 60 'sync_pool $TESTPOOL; test "$(parity_state)" == "off"'
log_must set_tunable64 PARITY_TEST_INJECT 0

typeset -i leaked=$(parity_status_value leaked_bytes)
log_note "leaked $leaked, held $(parity_held), free $free0 -> $(parity_free)"
log_must test "$(parity_held)" -eq 0
log_must test $leaked -ge $held0
# The held blocks were not freed: the free space did not grow by them.
log_must test "$(parity_free)" -lt $(( free0 + held0 ))

log_must zpool export $TESTPOOL
log_must zpool import -d $PARITY_DIR $TESTPOOL
log_must test "$(parity_status_value leaked_bytes)" == "$leaked"
log_must zpool status -x $TESTPOOL

log_pass "a drop of unreadable records keeps the lost space visible"
