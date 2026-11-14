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
#	The vdev config marks the lagging child while the async mirror is on.  The
#	mark is in the labels of the leader, and never in the labels of the
#	lagging child.  Turning the async mirror off clears the mark.  A failed
#	replay clears it too, because the DTLs then say what the child is
#	missing.
#
# STRATEGY:
#	1. Create a pool with a two-way mirror and turn the async mirror on.
#	2. Write data, and verify the mark in the labels of the leader only.
#	3. Turn the async mirror off, and verify that the mark is gone.
#	4. Turn the async mirror on again, and verify the mark.
#	5. Inject write errors on the lagging child until the replay fails.
#	6. Wait for the release of the records, and verify that the mark
#	   is gone.
#

verify_runnable "global"

log_onexit cleanup_async_mirror

function has_mark # vdev file
{
	zdb -l $1 | grep -q async_mirror_lagging
}

async_mirror_create_pool
log_mustnot has_mark $ASYNC_MIRROR_LEADER
async_mirror_enable

async_mirror_write_files file 2
log_must wait_async_mirror_idle
log_must has_mark $ASYNC_MIRROR_LEADER
log_mustnot has_mark $ASYNC_MIRROR_LAGGING

async_mirror_disable
sync_pool $TESTPOOL
log_mustnot has_mark $ASYNC_MIRROR_LEADER
log_mustnot has_mark $ASYNC_MIRROR_LAGGING

async_mirror_enable
async_mirror_write_files more 2
log_must wait_async_mirror_idle
log_must has_mark $ASYNC_MIRROR_LEADER

typeset guid=$(get_leaf_guid $ASYNC_MIRROR_LAGGING)
log_must zinject -d $guid -e io -T write $TESTPOOL
async_mirror_write_files fail 2
log_must wait_async_mirror_failed
log_must zinject -c all
log_must wait_async_mirror_feature enabled
sync_pool $TESTPOOL
log_mustnot has_mark $ASYNC_MIRROR_LEADER
log_must wait_async_mirror_resilvered
sync_pool $TESTPOOL
log_mustnot has_mark $ASYNC_MIRROR_LAGGING

log_pass "The vdev config marks the lagging child while the async mirror is on."
