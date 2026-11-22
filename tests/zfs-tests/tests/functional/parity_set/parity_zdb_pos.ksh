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
# "zdb --parity" dumps the records of a pool, also when the pool is exported.
#
# STRATEGY:
# 1. Arm the parity set and write a file.
# 2. Run zdb --parity. Check that it prints something and that -v prints
#    more lines.
# 3. Export the pool and run zdb -e --parity. Check that it prints the same
#    number of lines.
# 4. Import the pool, drop the records and check that the dump shrinks.
#

verify_runnable "global"
parity_require

function cleanup
{
	parity_cleanup
}
log_onexit cleanup

log_assert "zdb --parity dumps the records"

parity_create_pool
parity_arm
parity_write /$TESTPOOL/data 16

log_must eval "zdb --parity $TESTPOOL >/dev/null"
typeset -i n=$(zdb --parity $TESTPOOL | wc -l)
typeset -i nv=$(zdb --parity -v $TESTPOOL | wc -l)
log_must test $n -gt 0
log_must test $nv -gt $n

log_must zpool export $TESTPOOL
typeset -i ne=$(zdb -e -p $PARITY_DIR --parity $TESTPOOL | wc -l)
# The export syncs txgs that write records too.
log_must test $ne -ge $n
log_must zpool import -d $PARITY_DIR $TESTPOOL

log_must zpool parity drop -f $TESTPOOL
log_must parity_wait 60 'test "$(parity_pending)" -lt 1048576'
sync_pool $TESTPOOL
typeset -i nd=$(zdb --parity -v $TESTPOOL | wc -l)
log_must test $nd -lt $nv

log_pass "zdb --parity dumps the records"
