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
. $STF_SUITE/tests/functional/mmp/mmp.kshlib

#
# DESCRIPTION:
#	The async mirror cannot be used with multihost.  The multihost thread
#	writes current uberblocks to the labels of the lagging child.
#
# STRATEGY:
#	1. Create a pool with a two-way mirror and set multihost=on.
#	2. Verify that "zpool set async_mirror=lagging" is refused, and
#	   that the async mirror is not on.
#	3. Set multihost=off, and turn the async mirror on.  Export and import.
#	   Verify the async mirror is on.
#	4. Verify that setting multihost=on is now refused.
#

verify_runnable "global"

function cleanup
{
	cleanup_async_mirror
	log_must mmp_clear_hostid
}

log_onexit cleanup

log_must mmp_set_hostid $HOSTID1
async_mirror_create_pool
log_must zpool set multihost=on $TESTPOOL

log_mustnot zpool set async_mirror=lagging $TESTPOOL $ASYNC_MIRROR_LAGGING
log_must test "$(get_async_mirror_stat active)" -eq 0

log_must zpool set multihost=off $TESTPOOL
async_mirror_enable
log_must zpool export $TESTPOOL
async_mirror_import
log_must test "$(get_async_mirror_stat active)" -eq 1

log_mustnot zpool set multihost=on $TESTPOOL
log_must test "$(get_pool_prop multihost $TESTPOOL)" == "off"

log_pass "The async mirror cannot be used with multihost."
