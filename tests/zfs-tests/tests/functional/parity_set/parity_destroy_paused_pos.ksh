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
# A large destroy on a small member with a stopped consumer pauses its
# frees at the held-bytes limit and does not grow the records without end;
# the pool stays healthy, and the destroy goes on when the limit allows it.
#
# STRATEGY:
# 1. Arm the parity set (nobody calls APPLIED: the consumer is stopped).
#    Set parity_onstall=wait and parity_max_held=2 (percent of the pool).
# 2. Write data into three file systems and destroy all three.
# 3. Check that the frees stop: most of the space is still "freeing", the
#    held bytes stay near the limit, and the pool is healthy.  The
#    parity_throttle event shows a default record limit of at most 5% of
#    the pool (zfs_parity_max_record_pct), not the 4 GiB byte limit.
# 4. Raise the limit to 100%. Check that the destroys end ("freeing" is 0) and
#    that the held bytes then cover the destroyed data.
#

verify_runnable "global"
parity_require

function cleanup
{
	zpool set parity_max_held=0 $TESTPOOL 2>/dev/null
	parity_cleanup
}
log_onexit cleanup

typeset -i mb=40
typeset -i pct=2
typeset -i total=$(( 3 * mb * 1048576 ))

log_assert "a destroy pauses its frees at the held limit of a stopped consumer"

parity_create_pool
parity_arm
log_must zpool set parity_onstall=wait $TESTPOOL

for i in 1 2 3; do
	log_must zfs create -o compression=off -o recordsize=128k \
	    $TESTPOOL/fs$i
	parity_write /$TESTPOOL/fs$i/data $mb
done

log_must zpool events -c
log_must zpool set parity_max_held=$pct $TESTPOOL
for i in 1 2 3; do
	log_must zfs destroy $TESTPOOL/fs$i
done

# Let the background frees run as far as they may.
for i in 1 2 3 4 5; do
	sync_pool $TESTPOOL
	sleep 1
done

typeset -i freeing=$(get_pool_prop freeing $TESTPOOL)
typeset -i held=$(parity_held)
log_note "paused: freeing $freeing, held $held, limit $pct% of the pool"
log_must test $freeing -ge $(( total / 2 ))
log_must test $held -lt $(( total / 2 ))
log_must check_pool_status $TESTPOOL "state" "ONLINE"
log_must test "$(zpool list -Ho health $TESTPOOL)" == "ONLINE"

# The default record limit is a share of this small pool, not 4 GiB.
log_must parity_wait_event parity_throttle 30
typeset -i maxrec=$(parity_event_field parity_throttle max_record_bytes)
typeset -i size=$(zpool list -Hp -o size $TESTPOOL)
log_note "record limit in force: $maxrec of a pool of $size"
log_must test $maxrec -gt 0
log_must test $maxrec -le $(( size / 100 * 5 ))

log_must zpool set parity_max_held=100 $TESTPOOL
log_must parity_wait 120 'sync_pool $TESTPOOL; \
    test "$(get_pool_prop freeing $TESTPOOL)" == "0"'
log_must test "$(parity_held)" -ge $(( total - 16 * 1048576 ))

log_pass "a destroy pauses its frees at the held limit of a stopped consumer"
