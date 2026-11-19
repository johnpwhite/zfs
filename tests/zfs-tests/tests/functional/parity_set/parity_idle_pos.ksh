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
# An idle parity set member makes no transaction groups. This holds with
# records pending and after the records are applied.
#
# STRATEGY:
# 1. Arm the parity set and write a file.
# 2. Check that the uberblock txg does not move for 15 seconds.
# 3. Apply the records with zfs_parity_test_auto_apply and wait for the
#    release to end.
# 4. Check that the uberblock txg does not move for 30 seconds.
#

verify_runnable "global"
parity_require
parity_require_tunable PARITY_TEST_AUTO_APPLY

function cleanup
{
	parity_cleanup
}
log_onexit cleanup

function ub_txg
{
	zdb -u $TESTPOOL | awk '$1 == "txg" { print $3 }' | sort -n | tail -n 1
}

log_assert "an idle parity set member makes no transaction groups"

parity_create_pool
parity_arm
parity_write /$TESTPOOL/data 16
sleep 5
typeset -i t1=$(ub_txg)
sleep 15
typeset -i t2=$(ub_txg)
log_must test $t1 -eq $t2

log_must set_tunable64 PARITY_TEST_AUTO_APPLY 1
log_must parity_wait 90 'sync_pool $TESTPOOL; test "$(parity_pending)" -lt 1048576'
sleep 5
t1=$(ub_txg)
sleep 30
t2=$(ub_txg)
log_must test $t1 -eq $t2

# An applied, idle member keeps the feature active, so that
# software without parity support can import it read-only only.
log_must test "$(get_pool_prop parityset $TESTPOOL)" == "on"
log_must test "$(get_pool_prop feature@parity_records $TESTPOOL)" == "active"

log_pass "an idle parity set member makes no transaction groups"
