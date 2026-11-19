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
# zpool parity reclaim frees the held space that a drop of unreadable records
# leaked, from the ranges that zdb finds.
#
# STRATEGY:
# 1. Make leaked held space (a forced drop with failing record reads).
# 2. Export, run zdb -u and zdb -b, import.  zdb reports the leak.
# 3. Reclaim with those lines: leaked_bytes is 0 and the free space grows by
#    the leaked bytes (less 1 MiB for new metadata).
# 4. Export: zdb -b finds no leak.  Import: the pool is healthy, and a new
#    file can use the space.
# 5. A second reclaim reports that nothing is leaked.
#

verify_runnable "global"
parity_require
parity_require_tunable PARITY_TEST_INJECT

typeset LEAKS=$PARITY_DIR/leaks.$$

function cleanup
{
	parity_set_tunable PARITY_TEST_INJECT 0
	rm -f $LEAKS
	parity_cleanup
}
log_onexit cleanup

log_assert "zpool parity reclaim frees the space that a drop leaked"

parity_make_leak
typeset -i leaked=$(parity_status_value leaked_bytes)
typeset -i free0=$(parity_free)
parity_zdb_leaks $LEAKS
log_note "leaked $leaked"
log_must grep -q "leaked space" $LEAKS

log_must zpool parity reclaim $TESTPOOL $LEAKS
log_must test "$(parity_status_value leaked_bytes)" == "0"
log_must test "$(parity_leaked_zap)" == "0"
log_mustnot zpool parity reclaim $TESTPOOL $LEAKS
sync_pool $TESTPOOL
# The export, the import and the history use a little new metadata.
log_must parity_wait 30 \
    "sync_pool $TESTPOOL; test \$(parity_free) -ge $((free0 + leaked - 1048576))"

log_must zpool export $TESTPOOL
log_must zdb -e -p $PARITY_DIR -b $TESTPOOL
log_must zpool import -d $PARITY_DIR $TESTPOOL
log_must zpool status -x $TESTPOOL
parity_write /$TESTPOOL/after $PARITY_DATA_MB
sync_pool $TESTPOOL
log_must zpool scrub -w $TESTPOOL
log_must check_pool_status $TESTPOOL "errors" "No known data errors"

log_pass "zpool parity reclaim frees the space that a drop leaked"
