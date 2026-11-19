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
# A parity set member that holds records grows only up to the size that its
# pool property parity_grow_max grants.
#
# STRATEGY:
# 1. Arm a pool and keep its records pending; grow its disk by 512 MB.
# 2. Check that online expansion fails without a grant.
# 3. Grant 128 MB more than the top-level vdev's asize; expand.
# 4. Check that the pool grew, and that the new asize is not above the grant
#    although the disk has more space.
# 5. Check that the grant cannot shrink the pool and that 0 blocks again.
#

verify_runnable "global"

function cleanup
{
	parity_cleanup
}
log_onexit cleanup

function top_asize
{
	zdb -C $TESTPOOL | awk '$1 == "asize:" {print $2; exit}'
}

log_assert "a busy member grows only up to parity_grow_max"

parity_create_pool
parity_arm
log_must parity_write /$TESTPOOL/f0 4
log_must test "$(parity_state)" == "on"

typeset size0=$(get_pool_prop size $TESTPOOL)
typeset asize0=$(top_asize)
log_must test -n "$asize0"
log_must truncate -s +512M $PARITY_VDEV

log_mustnot zpool online -e $TESTPOOL $PARITY_VDEV
log_must test "$(get_pool_prop size $TESTPOOL)" == "$size0"

typeset cap=$((asize0 + 128 * 1024 * 1024))
log_must zpool set parity_grow_max=$cap $TESTPOOL
log_must test "$(get_pool_prop parity_grow_max $TESTPOOL)" == "$cap"
log_must zpool online -e $TESTPOOL $PARITY_VDEV
log_must sync_pool $TESTPOOL

typeset size1=$(get_pool_prop size $TESTPOOL)
typeset asize1=$(top_asize)
log_note "size $size0 -> $size1, asize $asize0 -> $asize1, cap $cap"
log_must test $size1 -gt $size0
log_must test $asize1 -gt $asize0
log_must test $asize1 -le $cap

# A second expansion stays at the cap.
log_must zpool online -e $TESTPOOL $PARITY_VDEV
log_must test "$(top_asize)" -le $cap

# A grant of 0 blocks a busy member again.
log_must zpool set parity_grow_max=0 $TESTPOOL
log_mustnot zpool online -e $TESTPOOL $PARITY_VDEV
log_must test "$(parity_state)" == "on"
log_must zpool status -x $TESTPOOL

log_pass "a busy member grows only up to parity_grow_max"
