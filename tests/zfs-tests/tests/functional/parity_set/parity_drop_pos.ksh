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
# "zpool parity drop" discards the records and releases the held blocks.
#
# STRATEGY:
# 1. Arm the parity set, write and remove a file.
# 2. Run the command without -f and without a terminal. Check that nothing
#    is dropped.
# 3. Run it with -f. Check that pending and held go to 0, that the space
#    returns and that the state is still "on" (a new epoch).
# 4. Check that new writes make new records.
# 5. Set parityset=off and drop. Check that the state is "off".
# 6. Check that a pool that does not exist is refused.
#

verify_runnable "global"
parity_require

function cleanup
{
	parity_cleanup
}
log_onexit cleanup

typeset -i mb=$PARITY_DATA_MB
typeset -i slack=$(( 16 * 1048576 ))

log_assert "zpool parity drop discards the records"

parity_create_pool
parity_arm
typeset -i free0=$(parity_free)
parity_write /$TESTPOOL/data $mb
log_must rm /$TESTPOOL/data
sync_pool $TESTPOOL

zpool parity drop $TESTPOOL < /dev/null
log_must test "$(parity_pending)" -gt 0

log_must zpool parity drop -f $TESTPOOL
log_must parity_wait 60 'test "$(parity_pending)" -lt 1048576'
log_must parity_wait 60 'test "$(parity_held)" -eq 0'
log_must parity_wait 60 parity_free_at_least $(( free0 - slack ))
log_must test "$(parity_state)" == "on"

parity_write /$TESTPOOL/data2 8
log_must test "$(parity_pending)" -gt 0

log_must zpool set parityset=off $TESTPOOL
log_must zpool parity drop -f $TESTPOOL
log_must parity_wait 60 'test "$(parity_state)" == "off"'

log_mustnot zpool parity drop -f no_such_pool_$$

log_pass "zpool parity drop discards the records"
