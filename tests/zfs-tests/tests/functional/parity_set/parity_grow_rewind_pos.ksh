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
# A rewind import that retries after the pool properties loaded, but before
# the parity records did, must not grow a busy member with autoexpand=on
# beyond its grant.
#
# STRATEGY:
# 1. Arm a pool with autoexpand=on, keep its records pending, grow its disk
#    by 512 MB and export it.
# 2. Make the parity load fail once (test_inject bit 16): a plain import
#    fails cleanly. Again, and import with -F: the load retries with a rewind.
# 3. Check that the retry ran (the bit cleared) and the top-level vdev
#    asize did not change, also after the autoexpand that follows an import.
# 4. Grant 128 MB more and expand: the asize grows, but not above the grant.
#

verify_runnable "global"
parity_require_tunable PARITY_TEST_INJECT

function cleanup
{
	parity_set_tunable PARITY_TEST_INJECT 0
	parity_cleanup
}
log_onexit cleanup

function top_asize
{
	zdb -C $TESTPOOL | awk '$1 == "asize:" {print $2; exit}'
}

log_assert "a rewind import retry does not grow a busy member"

parity_create_pool -o autoexpand=on
parity_arm
log_must parity_write /$TESTPOOL/f0 4
log_must parity_write /$TESTPOOL/f1 1
log_must test "$(parity_state)" == "on"

typeset asize0=$(top_asize)
log_must test -n "$asize0"
log_must zpool export $TESTPOOL
log_must truncate -s +512M $PARITY_VDEV

# A plain import whose parity load fails must fail cleanly (it once returned
# without the namespace lock and panicked in spa_unload).
log_must set_tunable64 PARITY_TEST_INJECT 16
log_mustnot zpool import -d $PARITY_DIR $TESTPOOL
log_must test "$(get_tunable PARITY_TEST_INJECT)" == "0"
log_must set_tunable64 PARITY_TEST_INJECT 16
log_must zpool import -F -d $PARITY_DIR $TESTPOOL
log_must test "$(get_tunable PARITY_TEST_INJECT)" == "0"
log_must test "$(parity_state)" == "on"
log_must test "$(top_asize)" == "$asize0"

# The import asks for an autoexpand; the busy member must still refuse it.
sleep 3
log_must sync_pool $TESTPOOL
log_must test "$(top_asize)" == "$asize0"

typeset cap=$((asize0 + 128 * 1024 * 1024))
log_must zpool set parity_grow_max=$cap $TESTPOOL
log_must zpool online -e $TESTPOOL $PARITY_VDEV
log_must sync_pool $TESTPOOL
typeset asize1=$(top_asize)
log_note "asize $asize0 -> $asize1, cap $cap"
log_must test $asize1 -gt $asize0
log_must test $asize1 -le $cap
log_must zpool status -x $TESTPOOL

log_pass "a rewind import retry does not grow a busy member"
