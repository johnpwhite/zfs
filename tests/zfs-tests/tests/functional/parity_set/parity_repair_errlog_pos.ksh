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
# A block that a scrub found bad, and that the parity service then repaired,
# leaves the error log: "zpool status -v" does not name the file any more.
#
# STRATEGY:
# 1. Write one data block to an applied parity member.
# 2. With a checksum error injected, scrub the pool: the scrub posts a repair
#    event and logs a permanent error for the file.
# 3. The test tool sends the bytes as the daemon does (REPAIR).
# 4. After a sync, "zpool status -v" does not name the file, and the file
#    reads back right.
#

verify_runnable "global"
parity_require

typeset PARITY_TOOL_PID=""
typeset PARITY_SOURCE=$PARITY_DIR/source.$$
typeset PARITY_READY=$PARITY_DIR/ready.$$
typeset PARITY_GO=$PARITY_DIR/go.$$
typeset PARITY_STARTED=$PARITY_DIR/started.$$
typeset PARITY_OUT=$PARITY_DIR/out.$$
typeset PARITY_READBACK=$PARITY_DIR/readback.$$

function cleanup
{
	if [[ -n $PARITY_TOOL_PID ]]; then
		kill -9 $PARITY_TOOL_PID 2>/dev/null
		wait $PARITY_TOOL_PID 2>/dev/null
	fi
	zinject -c all >/dev/null 2>&1
	parity_cleanup
}
log_onexit cleanup

log_assert "a parity repair takes the block out of the error log"

parity_create_pool
parity_require_tunable PARITY_TEST_AUTO_APPLY
log_must set_tunable64 PARITY_TEST_AUTO_APPLY 1
log_must zfs set compression=off recordsize=4k primarycache=none $TESTPOOL
parity_arm
log_must dd if=/dev/urandom of=$PARITY_SOURCE bs=4096 count=1
log_must cp $PARITY_SOURCE /$TESTPOOL/data
sync_pool $TESTPOOL
parity_write /$TESTPOOL/mark 1

parity_repair_wait late $TESTPOOL $PARITY_SOURCE $PARITY_READY \
	$PARITY_GO $PARITY_STARTED > $PARITY_OUT 2>&1 &
PARITY_TOOL_PID=$!
log_must parity_wait 15 "test -f $PARITY_READY"
log_must zinject -a -t data -e checksum -f 100 /$TESTPOOL/data
log_must zpool scrub -w $TESTPOOL
log_must zinject -c all
log_must zpool sync $TESTPOOL
log_note "$(zpool status -v $TESTPOOL)"
zpool status -v $TESTPOOL | grep -q "/$TESTPOOL/data" || \
    log_unsupported "the scrub logged no error for the file"

# The daemon path: REPAIR with the right bytes.
log_must touch $PARITY_GO
wait $PARITY_TOOL_PID || log_fail "the tool failed: $(<$PARITY_OUT)"
PARITY_TOOL_PID=""
log_must grep -q "^repaired" $PARITY_OUT
log_must test "$(kstat_pool $TESTPOOL parity_set.repair_written)" -ge 1
log_must zpool sync $TESTPOOL
log_must zpool sync $TESTPOOL
log_note "$(zpool status -v $TESTPOOL)"
log_mustnot eval "zpool status -v $TESTPOOL | grep -q '/$TESTPOOL/data'"
log_must dd if=/$TESTPOOL/data of=$PARITY_READBACK bs=4096 count=1
log_must cmp $PARITY_SOURCE $PARITY_READBACK

log_pass "a parity repair takes the block out of the error log"
