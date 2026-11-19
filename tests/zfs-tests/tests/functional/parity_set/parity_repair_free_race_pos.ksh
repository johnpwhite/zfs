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
# A block that is freed while the daemon's repair write for it is held is not
# handed out again before the write ends: the txg sync waits for the write.
#
# The daemon path is the one where this happens: the read has failed, so its
# file can be closed and removed while the repair is written.  (While a read
# waits for its repair, the reader holds the range lock or the dbuf.)
#
# STRATEGY:
# 1. Write one uncompressed data block to a parity member.
# 2. Read it with a checksum error and no provider: the read fails and a
#    repair request waits for the daemon.
# 3. Send the bytes with ZFS_IOC_PARITY_REPAIR, as the daemon does, with each
#    repair write held for 3 s (zfs_parity_test_inject bit 64).
# 4. Remove the file and sync the pool in that wait: the sync takes at least
#    2 s and logs the barrier.
# 5. Reuse the space; the pool is clean after a scrub.
#

verify_runnable "global"
parity_require

typeset PARITY_TOOL_PID=""
typeset PARITY_SOURCE=$PARITY_DIR/source.$$
typeset PARITY_READY=$PARITY_DIR/ready.$$
typeset PARITY_GO=$PARITY_DIR/go.$$
typeset PARITY_STARTED=$PARITY_DIR/started.$$
typeset PARITY_OUT=$PARITY_DIR/out.$$

function cleanup
{
	set_tunable32 PARITY_TEST_INJECT 0
	if [[ -n $PARITY_TOOL_PID ]]; then
		kill -9 $PARITY_TOOL_PID 2>/dev/null
		wait $PARITY_TOOL_PID 2>/dev/null
	fi
	zinject -c all >/dev/null 2>&1
	rm -f $PARITY_SOURCE $PARITY_READY $PARITY_GO $PARITY_STARTED \
	    $PARITY_OUT
	parity_cleanup
}
log_onexit cleanup

log_assert "the txg sync waits for a repair write whose block was freed"

parity_create_pool
parity_require_tunable PARITY_TEST_AUTO_APPLY
log_must set_tunable64 PARITY_TEST_AUTO_APPLY 1
parity_require_tunable PARITY_TEST_INJECT
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
log_mustnot dd if=/$TESTPOOL/data of=/dev/null bs=4096 count=1
log_must zinject -c all

# The tool sends the bytes now; the write is held 3 s.  Free and sync.
log_must set_tunable32 PARITY_TEST_INJECT 64
log_must touch $PARITY_GO
log_must parity_wait 15 "test -f $PARITY_STARTED"
sleep 1
log_must rm /$TESTPOOL/data
typeset -i t0=$SECONDS
log_must zpool sync $TESTPOOL
typeset -i took=$((SECONDS - t0))
wait $PARITY_TOOL_PID || log_fail "the tool failed: $(<$PARITY_OUT)"
PARITY_TOOL_PID=""
log_must set_tunable32 PARITY_TEST_INJECT 0
log_note "the sync took $took s; the tool: $(<$PARITY_OUT)"
log_must test $took -ge 1
log_must eval "grep -q 'parity repair barrier: $TESTPOOL waits' \
    /proc/spl/kstat/zfs/dbgmsg"

# Reuse the space and check the pool.
parity_write /$TESTPOOL/after 8
sync_pool $TESTPOOL
log_must zpool scrub -w $TESTPOOL
log_must check_pool_status $TESTPOOL "errors" "No known data errors"

log_pass "the txg sync waits for a repair write whose block was freed"
