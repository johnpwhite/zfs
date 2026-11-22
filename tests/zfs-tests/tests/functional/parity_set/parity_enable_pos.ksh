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
# The parityset property and the parity_records feature work as documented.
#
# STRATEGY:
# 1. Create a pool and check the defaults of the four properties.
# 2. Check that bad property values are refused.
# 3. Set parityset=on and write. Check that the feature is active, that the
#    state is "on", that records are pending and that parity_records fires.
# 4. Set parityset=off. Check that the state is "draining".
# 5. Drop the records. Check that the state is "off" and the feature is
#    "enabled" again.
#

verify_runnable "global"
parity_require

function cleanup
{
	parity_cleanup
}
log_onexit cleanup

log_assert "parityset and the parity_records feature follow the documentation"

parity_create_pool
log_must test "$(get_pool_prop parityset $TESTPOOL)" == "off"
log_must test "$(get_pool_prop parity_onstall $TESTPOOL)" == "continue"
log_must test "$(get_pool_prop parity_max_records $TESTPOOL)" == "0"
log_must test "$(get_pool_prop parity_max_held $TESTPOOL)" == "0"
log_must test "$(get_pool_prop feature@parity_records $TESTPOOL)" == "enabled"

log_mustnot zpool set parityset=maybe $TESTPOOL
log_mustnot zpool set parity_onstall=bogus $TESTPOOL
log_must zpool set parity_onstall=wait $TESTPOOL
log_must test "$(get_pool_prop parity_onstall $TESTPOOL)" == "wait"
log_must zpool set parity_onstall=continue $TESTPOOL

log_must zpool events -c
log_must zpool set parityset=on $TESTPOOL
sync_pool $TESTPOOL
log_must test "$(get_pool_prop feature@parity_records $TESTPOOL)" == "active"
log_must test "$(parity_state)" == "on"
log_must file_write -o create -f /$TESTPOOL/arm -b 4096 -c 1 -d R
sync_pool $TESTPOOL
parity_write /$TESTPOOL/data 8
log_must test "$(get_pool_prop parityset $TESTPOOL)" == "on"
log_must test "$(get_pool_prop feature@parity_records $TESTPOOL)" == "active"
log_must test "$(parity_state)" == "on"
log_must test "$(parity_pending)" -gt 0
log_must parity_wait_event parity_records 30
log_must test -n "$(parity_event_field parity_records txg)"

log_must zpool set parityset=off $TESTPOOL
log_must test "$(parity_state)" == "draining"
log_must test "$(get_pool_prop feature@parity_records $TESTPOOL)" == "active"

log_must zpool parity drop -f $TESTPOOL
log_must parity_wait 60 'test "$(parity_state)" == "off"'
log_must parity_wait 60 \
    'test "$(get_pool_prop feature@parity_records $TESTPOOL)" == "enabled"'

log_pass "parityset and the parity_records feature follow the documentation"
