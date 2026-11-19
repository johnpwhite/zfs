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
# The daemon reports the txg of the newest record as applied.  Txgs after
# it in which the pool synced only its own state have no record.  When the
# pool writes again, the oldest record is newer than the txg after the
# applied one.  MAP and BLOCKS must still serve the applied txg: a rebuild
# reads each surviving pool there.  Before the fix both refused it with
# EINVAL, and "zparity rebuild" failed.
#
# STRATEGY:
# 1. Turn the parity set on and write data.
# 2. Report the txg of the newest record as applied (the APPLIED call
#    syncs txgs that have no record).
# 3. Write again twice, so that the applied records are released.
# 4. Check that the oldest record is newer than the applied txg + 1, and
#    that MAP and BLOCKS serve the applied txg.
#

verify_runnable "global"
parity_require

function cleanup
{
	parity_cleanup
}
log_onexit cleanup

log_assert "MAP and BLOCKS serve the applied txg across txgs with no record"

parity_create_pool
parity_arm
parity_write /$TESTPOOL/data 8
log_must libzfs_input_check --parity-apply-last $TESTPOOL
log_must zpool parity status $TESTPOOL
parity_write /$TESTPOOL/more 2
parity_write /$TESTPOOL/more2 1
log_must zpool parity status $TESTPOOL
log_must libzfs_input_check --parity-history-gap $TESTPOOL

log_pass "MAP and BLOCKS serve the applied txg across txgs with no record"
