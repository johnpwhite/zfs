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
# A set scrub reads the blocks of a member's applied state through BLOCKS,
# then READ and READV.  The record of the last applied txg is released at
# the pool's next write, but its blocks are still held.  READ and READV must
# still accept those BLOCKS entries.  Before the fix every READ failed
# with ENOENT, and the set scrub reported every block of the member as bad.
#
# STRATEGY:
# 1. Turn the parity set on and write data.
# 2. Report the newest txg as applied.
# 3. Write again, so that the record of the applied txg is released.
# 4. READ and READV every BLOCKS entry of the applied txg.
#

verify_runnable "global"
parity_require

function cleanup
{
	parity_cleanup
}
log_onexit cleanup

log_assert "READ accepts BLOCKS entries after the applied record is released"

parity_create_pool
parity_arm
parity_write /$TESTPOOL/data 8
log_must libzfs_input_check --parity-apply $TESTPOOL
parity_write /$TESTPOOL/more 2
parity_write /$TESTPOOL/more2 1
log_must libzfs_input_check --parity-blocks-read $TESTPOOL

log_pass "READ accepts BLOCKS entries after the applied record is released"
