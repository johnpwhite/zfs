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
# Copyright (c) 2026, John White.
#

. $STF_SUITE/tests/functional/async_mirror/async_mirror.kshlib

#
# DESCRIPTION:
#	When the lagging child was imported alone and written while it was
#	behind, a read-write import of both copies is refused, and the
#	message names the choice to keep one copy.  With that choice, the
#	import keeps the lagging child's copy, erases the uberblocks of the
#	leader, and resilvers the leader in full.  The txgs that only the
#	leader had are lost, by the operator's choice.  After the resilver,
#	the next import turns the async mirror on again with the same leader.
#
# STRATEGY:
#	1. Create a pool with a two-way mirror and turn the async mirror on.
#	2. Write files a, and wait until the lagging child has caught up.
#	3. Stop the copy, write files b, and export the pool.
#	4. Hide the leader.  Import the lagging child alone, write files c,
#	   and export the pool.
#	5. Bring the leader back.  Verify that the import is refused, and
#	   that the message names --async-mirror-keep.
#	6. Import with --async-mirror-keep and the guid of the lagging
#	   child.  Verify files a and c, and that files b are gone.  Verify
#	   that the leader holds no uberblock from before the import above
#	   the fork.  Wait for the resilver.
#	7. Import the leader alone, read-only.  Verify files a and c, with no
#	   errors.
#	8. Import the pool.  Verify that the async mirror is on.
#

verify_runnable "global"

function cleanup
{
	[[ -e $ASYNC_MIRROR_HIDDEN ]] && async_mirror_unhide_leader
	[[ -e $ASYNC_MIRROR_LAGGING_HIDDEN ]] && async_mirror_unhide_lagging
	cleanup_async_mirror
}

log_onexit cleanup

async_mirror_create_pool
async_mirror_enable

async_mirror_write_files a 2
log_must wait_async_mirror_idle
async_mirror_export_behind b 2
async_mirror_suspend_copy 0
typeset -i fork=$(get_ring_max_txg $ASYNC_MIRROR_LAGGING)
typeset -i leader=$(get_ring_max_txg $ASYNC_MIRROR_LEADER)
log_must test $leader -gt $fork

async_mirror_import_lagging
async_mirror_write_files c 2
typeset digests="$(async_mirror_digests a 2)
$(async_mirror_digests c 2)"
log_must zpool export $TESTPOOL
async_mirror_unhide_leader

#
# The new txgs of the next import are above the txg of the lagging child,
# so below it every uberblock above the fork is one of the leader's own.
#
typeset -i kept=$(get_ring_max_txg $ASYNC_MIRROR_LAGGING)
(( kept < leader )) && leader=$kept

typeset out
out=$(zpool import -d $ASYNC_MIRROR_DIR $TESTPOOL 2>&1) &&
    log_fail "The import of copies written separately succeeded"
log_note "$out"
echo "$out" | grep -q "written separately" ||
    log_fail "The import failed with another error"
echo "$out" | grep -q -- "--async-mirror-keep" ||
    log_fail "The message does not name the choice"
log_mustnot poolexists $TESTPOOL

typeset guid=$(get_leaf_guid $ASYNC_MIRROR_LAGGING)
log_must zpool import -d $ASYNC_MIRROR_DIR --async-mirror-keep=$guid \
    $TESTPOOL
log_must test "$(get_async_mirror_stat active)" == 0
log_must test "$digests" == "$(async_mirror_digests a 2)
$(async_mirror_digests c 2)"
log_mustnot test -e /$TESTPOOL/b.0
log_must test $(count_ring_txgs $ASYNC_MIRROR_LEADER $fork $leader) -eq 0
log_must wait_async_mirror_resilvered
log_must zpool export $TESTPOOL

log_must mv $ASYNC_MIRROR_LAGGING $ASYNC_MIRROR_LAGGING_HIDDEN
log_must zpool import -o readonly=on -d $ASYNC_MIRROR_DIR $TESTPOOL
log_must test "$digests" == "$(async_mirror_digests a 2)
$(async_mirror_digests c 2)"
log_must check_pool_status $TESTPOOL errors "No known data errors"
log_must zpool export $TESTPOOL
async_mirror_unhide_lagging

async_mirror_import
log_must test "$(get_async_mirror_stat active)" == 1
log_must verify_pool $TESTPOOL

log_pass "The operator can keep the lagging child's copy."
