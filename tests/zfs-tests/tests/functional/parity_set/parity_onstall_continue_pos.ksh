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
# With parity_onstall=continue, a pool at the hard limit drops its records and
# keeps writing. It posts parity_unprotected and shows that it is not
# protected.
#
# STRATEGY:
# 1. Set a small parity_max_records. Keep parity_onstall at its default.
# 2. Write more than the limit. Check that the write does not block.
# 3. Check the event and the status text.
# 4. Check that the data is intact after an export and an import, and that
#    the pool takes more writes.
#

verify_runnable "global"
parity_require

function cleanup
{
	parity_cleanup
}
log_onexit cleanup

log_assert "parity_onstall=continue keeps writing and reports the loss"

parity_create_pool
log_must test "$(get_pool_prop parity_onstall $TESTPOOL)" == "continue"
log_must zpool set parity_max_records=$PARITY_SMALL_LIMIT $TESTPOOL
log_must zpool events -c
parity_arm

log_must timeout 120 file_write -o create -f /$TESTPOOL/big \
    -b 1048576 -c $PARITY_DATA_MB -d R
sync_pool $TESTPOOL
typeset sum=$(xxh128digest /$TESTPOOL/big)

log_must parity_wait_event parity_unprotected 60
log_must test -n "$(parity_event_field parity_unprotected pool_guid)"
log_must eval "zpool parity status $TESTPOOL | grep -q \
    'not protected, resync needed'"

log_must zpool export $TESTPOOL
log_must zpool import -d $PARITY_DIR $TESTPOOL
log_must test "$(xxh128digest /$TESTPOOL/big)" == "$sum"
parity_write /$TESTPOOL/more 8

log_pass "parity_onstall=continue keeps writing and reports the loss"
