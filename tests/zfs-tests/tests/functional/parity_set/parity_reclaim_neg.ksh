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
# zpool parity reclaim frees nothing when a check fails.
#
# STRATEGY:
# 1. Make leaked held space and save zdb's reclaim output.
# 2. Stale txg, free-space, duplicate, oversized, empty, error, and other-pool
#    inputs fail and leave leaked_bytes unchanged.
# 3. Reclaim the valid list. Test the explicit reset flag.
#

verify_runnable "global"
parity_require
parity_require_tunable PARITY_TEST_INJECT

typeset LEAKS=$PARITY_DIR/leaks.$$
typeset BAD=$PARITY_DIR/bad.$$
typeset OTHER_FILE=$PARITY_DIR/other.$$
typeset OTHER=${TESTPOOL}_other

function cleanup
{
	parity_set_tunable PARITY_TEST_INJECT 0
	if ! poolexists $OTHER; then
		zpool import -d $PARITY_DIR $OTHER >/dev/null 2>&1
	fi
	poolexists $OTHER && destroy_pool $OTHER
	rm -f $LEAKS $BAD $OTHER_FILE
	parity_cleanup
}
log_onexit cleanup

function unchanged
{
	log_must test "$(parity_status_value leaked_bytes)" == "$leaked"
}

log_assert "zpool parity reclaim frees nothing when a check fails"

parity_make_leak
typeset -i leaked=$(parity_status_value leaked_bytes)
parity_zdb_leaks $LEAKS
typeset txg=$(awk '$1 == "parity" && $2 == "reclaim:" { \
	sub(/,$/, "", $6); print $6 }' $LEAKS)
log_must grep -q "leaked space" $LEAKS

# A stale txg: the import loaded a different state.
awk -v t=$((txg + 1)) '{ if ($1 == "parity" && $2 == "reclaim:") sub(/txg [0-9]+,/, "txg " t ","); print }' $LEAKS > $BAD
log_mustnot zpool parity reclaim $TESTPOOL $BAD
unchanged

# A range in free space, near the end of the data device.
typeset -i size=$(zpool list -Hp -o size $TESTPOOL)
typeset -i off=$(( (size - 1048576) / 4096 * 4096 ))
awk -v off=$off '
    /^parity reclaim:/ { print; next }
    /^leaked space:/ && !changed {
        sub(/offset 0x[0-9a-f]+/, "offset 0x" sprintf("%x", off))
        changed = 1
    }
    { print }
    END { if (!changed) exit 1 }
' $LEAKS > $BAD
log_mustnot zpool parity reclaim $TESTPOOL $BAD
unchanged

# The same range twice: the ranges overlap.
grep '^parity reclaim:' $LEAKS > $BAD
grep "leaked space" $LEAKS | head -1 >> $BAD
grep "leaked space" $LEAKS | head -1 >> $BAD
log_mustnot zpool parity reclaim $TESTPOOL $BAD
unchanged

# Every size doubled: more than the leaked count (or past the allocation).
awk '/^parity reclaim:/ { print; next } /leaked space/ { \
	sub(/size [0-9]+/, "size " ($NF * 2)); print }' $LEAKS > $BAD
log_mustnot zpool parity reclaim $TESTPOOL $BAD
unchanged

# An empty list with a valid header must not reset the count.
grep '^parity reclaim:' $LEAKS > $BAD
log_mustnot zpool parity reclaim $TESTPOOL $BAD
unchanged

# Missing header also must not reset the count.
: > $BAD
log_mustnot zpool parity reclaim $TESTPOOL $BAD
unchanged

# zdb errors and another pool's output are refused.
awk '{ if ($1 == "parity" && $2 == "reclaim:") sub(/errors [0-9]+/, "errors 1"); print }' $LEAKS > $BAD
log_mustnot zpool parity reclaim $TESTPOOL $BAD
unchanged

log_must truncate -s $PARITY_VDEV_SIZE $PARITY_VDEV2
log_must zpool create -f $OTHER $PARITY_VDEV2
log_must zpool export $OTHER
zdb -e -p $PARITY_DIR -b $OTHER > $OTHER_FILE 2>&1
log_must zpool import -d $PARITY_DIR $OTHER
log_mustnot zpool parity reclaim $TESTPOOL $OTHER_FILE
unchanged
log_must destroy_pool $OTHER

# The right input works.
log_must zpool parity reclaim $TESTPOOL $LEAKS
log_must test "$(parity_status_value leaked_bytes)" == "0"

# A pool with nothing leaked.
log_mustnot zpool parity reclaim $TESTPOOL $LEAKS

# Resetting the counter is explicit and frees no range.
log_must destroy_pool $TESTPOOL
parity_make_leak
typeset -i leaked_reset=$(parity_status_value leaked_bytes)
typeset -i free_reset=$(parity_free)
log_must zpool parity reclaim -r $TESTPOOL
log_must test "$(parity_status_value leaked_bytes)" == "0"
log_must test "$(parity_free)" -lt $(( free_reset + leaked_reset - 1048576 ))

log_pass "zpool parity reclaim frees nothing when a check fails"
