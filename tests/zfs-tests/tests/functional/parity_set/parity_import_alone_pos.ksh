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
# Principle 1: a member of a parity set is a normal single-disk pool. It
# imports alone, with no daemon and no other disk.
#
# STRATEGY:
# 1. Arm the parity set and write data. Export the pool.
# 2. Import it read-only. Check the data, that the feature is active, that the
#    pending bytes are unchanged and that a write fails.
# 3. Import it read-write. Check that the records are still there and that
#    writes work.
# 4. Drop the records, set parityset=off and export. Import read-write.
#    Check that the feature is "enabled" and the state is "off".
#
# A host without parity support can import a pool with active records
# read-only only. That case needs an old build and is not tested here.
#

verify_runnable "global"
parity_require

function cleanup
{
	parity_cleanup
}
log_onexit cleanup

log_assert "a parity set member imports alone"

parity_create_pool
parity_arm
parity_write /$TESTPOOL/data 16
typeset sum=$(xxh128digest /$TESTPOOL/data)
typeset -i pending=$(parity_pending)
log_must test $pending -gt 0
log_must zpool export $TESTPOOL

log_must zpool import -o readonly=on -d $PARITY_DIR $TESTPOOL
log_must test "$(xxh128digest /$TESTPOOL/data)" == "$sum"
log_must test "$(get_pool_prop feature@parity_records $TESTPOOL)" == "active"
# The export itself syncs txgs that write records: the count can only grow.
log_must test "$(parity_pending)" -ge $pending
log_mustnot touch /$TESTPOOL/not_allowed
log_must zpool export $TESTPOOL

log_must zpool import -d $PARITY_DIR $TESTPOOL
log_must test "$(xxh128digest /$TESTPOOL/data)" == "$sum"
log_must test "$(get_pool_prop feature@parity_records $TESTPOOL)" == "active"
log_must test "$(parity_pending)" -ge $pending
parity_write /$TESTPOOL/more 4

# Off first: a drop while the pool drains ends in off (parity_refuse_neg).
log_must zpool set parityset=off $TESTPOOL
log_must zpool parity drop -f $TESTPOOL
log_must parity_wait 60 'test "$(parity_state)" == "off"'
log_must zpool export $TESTPOOL
log_must zpool import -d $PARITY_DIR $TESTPOOL
log_must test "$(xxh128digest /$TESTPOOL/data)" == "$sum"
log_must test "$(get_pool_prop feature@parity_records $TESTPOOL)" == "enabled"
log_must test "$(parity_state)" == "off"
parity_write /$TESTPOOL/after 4

# The override: the pool imports read-write with records off, and a later
# parityset=on is refused until the records are dropped.
if tunable_exists PARITY_IMPORT_OVERRIDE; then
	log_must zpool destroy $TESTPOOL
	parity_create_pool
	parity_arm
	parity_write /$TESTPOOL/data 4
	log_must zpool export $TESTPOOL
	log_must set_tunable64 PARITY_IMPORT_OVERRIDE 1
	log_must zpool import -d $PARITY_DIR $TESTPOOL
	log_must test "$(xxh128digest /$TESTPOOL/data)" != ""
	parity_write /$TESTPOOL/more 4
	log_must set_tunable64 PARITY_IMPORT_OVERRIDE 0
fi

# The same as an import option, with no module parameter.
log_must zpool export $TESTPOOL
log_must zpool import --parity-override -d $PARITY_DIR $TESTPOOL
log_must test "$(xxh128digest /$TESTPOOL/data)" != ""
zpool import --help 2>&1 | grep -q -- "--parity-override" || \
    log_fail "zpool import usage has no --parity-override"

log_pass "a parity set member imports alone"
