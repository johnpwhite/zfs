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
# LABEL returns the uberblock of every txg from the applied txg to the last
# synced txg, also when parity lags more txgs than the label ring holds and
# the txg after a record txg changed nothing (#151).
#
# STRATEGY:
# 1. Create a pool with ashift 12 (a ring of 32 uberblocks) and arm it.
# 2. 48 times: write one small file and force a sync.  A forced sync also
#    syncs empty txgs, so most record txgs are followed by a txg with no
#    record.
# 3. Ask LABEL for each txg from the applied txg to the last synced txg.
#

verify_runnable "global"
parity_require

function cleanup
{
	parity_cleanup
}

log_onexit cleanup

log_assert "LABEL finds the uberblock of each unapplied txg"

parity_create_pool -o ashift=12
parity_arm

typeset -i i
for i in {1..48}; do
	log_must file_write -o create -f /$TESTPOOL/lag.$i -b 4096 -c 1 -d R
	sync_pool $TESTPOOL
done

log_must libzfs_input_check --parity-label-all $TESTPOOL

log_pass "LABEL finds the uberblock of each unapplied txg"
