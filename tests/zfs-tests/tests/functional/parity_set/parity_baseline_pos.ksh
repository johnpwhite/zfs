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
# On an idle member that was just armed, the baseline txg (first_txg - 1
# from INFO) has a label at once, and APPLIED of that txg binds the member
# to a set.
#
# STRATEGY:
# 1. Create an empty pool and turn parityset on. Write nothing.
# 2. INFO, then LABEL of first_txg - 1: it must succeed and hold that txg.
#    APPLIED of that txg with a new set GUID binds the member.
# 3. Turn parityset off and let zfs_parity_test_auto_apply apply the records
#    in place of the daemon (even an idle pool records its own property and
#    history writes). Wait until the drain ends (the state is "off";
#    the end of the records clears the set GUID). Turn it on again (a new
#    epoch) and check again: the new epoch has no set GUID.
#

verify_runnable "global"
parity_require
parity_require_tunable PARITY_TEST_AUTO_APPLY

function cleanup
{
	parity_cleanup
}
log_onexit cleanup

log_assert "the baseline of a new epoch has a label on an idle pool"

parity_create_pool
log_must zpool set parityset=on $TESTPOOL
log_must libzfs_input_check --parity-baseline $TESTPOOL

log_must zpool set parityset=off $TESTPOOL
log_must set_tunable64 PARITY_TEST_AUTO_APPLY 1
log_must parity_wait 60 'sync_pool $TESTPOOL; test "$(parity_state)" == "off"'
log_must set_tunable64 PARITY_TEST_AUTO_APPLY 0
log_must zpool set parityset=on $TESTPOOL
log_must libzfs_input_check --parity-baseline $TESTPOOL

log_pass "the baseline of a new epoch has a label on an idle pool"
