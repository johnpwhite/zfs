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
# The repair provider role is held for each pool, a "cannot" answer ends the
# wait of a read at once, and a read over the waiter limit does not wait.
#
# STRATEGY:
# 1. Write one data block to an applied parity member.
# 2. Register a provider for the pool; a second provider for the same pool
#    gets EBUSY (exit 3).
# 3. With a checksum error injected, a provider that declines makes the read
#    fail in less than 10 seconds, though the wait time is 30 seconds.
# 4. With the waiter limit at 0, a read with a provider registered fails at
#    once and the debug log names the limit.
# 5. With the limit back, a provider that supplies the bytes repairs the read.
# 6. With no provider, a read fails and its request stays pending.  A provider
#    that registers later gets the event again (sync_wait false).  REPAIR with
#    wrong bytes writes nothing and keeps the request; REPAIR with the right
#    bytes repairs the block.
# 7. The kstat parity_set counts the requests and each kind of answer.
#

verify_runnable "global"
parity_require

typeset PARITY_PROVIDER_PID=""
typeset PARITY_SOURCE=$PARITY_DIR/source.$$
typeset PARITY_READY1=$PARITY_DIR/ready1.$$
typeset PARITY_READY2=$PARITY_DIR/ready2.$$
typeset PARITY_READY3=$PARITY_DIR/ready3.$$
typeset PARITY_READY4=$PARITY_DIR/ready4.$$
typeset PARITY_READY5=$PARITY_DIR/ready5.$$
typeset PARITY_READY6=$PARITY_DIR/ready6.$$
typeset PARITY_WRONG=$PARITY_DIR/wrong.$$
typeset PARITY_RESULT=$PARITY_DIR/result.$$
typeset PARITY_READBACK=$PARITY_DIR/readback.$$

function cleanup
{
	if [[ -n $PARITY_PROVIDER_PID ]]; then
		kill -9 $PARITY_PROVIDER_PID 2>/dev/null
		wait $PARITY_PROVIDER_PID 2>/dev/null
	fi
	zinject -c all >/dev/null 2>&1
	set_tunable32 PARITY_REPAIR_MAX_WAITERS 8
	parity_cleanup
}
log_onexit cleanup

log_assert "per-pool provider, decline answer and waiter limit"

parity_create_pool
parity_require_tunable PARITY_REPAIR_WAIT_MS
parity_require_tunable PARITY_REPAIR_MAX_WAITERS
log_must set_tunable64 PARITY_REPAIR_WAIT_MS 30000
parity_require_tunable PARITY_TEST_AUTO_APPLY
log_must set_tunable64 PARITY_TEST_AUTO_APPLY 1
log_must zfs set compression=off recordsize=4k primarycache=none $TESTPOOL
parity_arm
log_must dd if=/dev/urandom of=$PARITY_SOURCE bs=4096 count=1
log_must cp $PARITY_SOURCE /$TESTPOOL/data
sync_pool $TESTPOOL
parity_write /$TESTPOOL/mark 1

# One provider for each pool: a second handle gets EBUSY.
parity_repair_wait decline $TESTPOOL $PARITY_READY1 $PARITY_RESULT &
PARITY_PROVIDER_PID=$!
log_must parity_wait 15 "test -f $PARITY_READY1"
parity_repair_wait decline $TESTPOOL $PARITY_READY2 $PARITY_RESULT
log_must test $? -eq 3
log_mustnot test -f $PARITY_READY2

# The decline ends the wait at once.
log_must zinject -a -t data -e checksum -f 100 /$TESTPOOL/data
typeset -i t0=$SECONDS
log_mustnot dd if=/$TESTPOOL/data of=$PARITY_READBACK bs=4096 count=1
typeset -i took=$((SECONDS - t0))
wait $PARITY_PROVIDER_PID || log_fail "the provider did not decline"
PARITY_PROVIDER_PID=""
log_note "the declined read failed after $took s"
log_must test $took -lt 10
log_must test "$(kstat_pool $TESTPOOL parity_set.repair_declined)" -ge 1

# The waiter limit: no read waits, the read fails at once.
log_must set_tunable32 PARITY_REPAIR_MAX_WAITERS 0
parity_repair_wait decline $TESTPOOL $PARITY_READY3 $PARITY_RESULT &
PARITY_PROVIDER_PID=$!
log_must parity_wait 15 "test -f $PARITY_READY3"
t0=$SECONDS
log_mustnot dd if=/$TESTPOOL/data of=$PARITY_READBACK bs=4096 count=1
took=$((SECONDS - t0))
log_must test $took -lt 10
log_must eval "grep -q 'parity repair wait skipped for $TESTPOOL: too many' \
    /proc/spl/kstat/zfs/dbgmsg"
kill -9 $PARITY_PROVIDER_PID 2>/dev/null
wait $PARITY_PROVIDER_PID 2>/dev/null
PARITY_PROVIDER_PID=""
log_must set_tunable32 PARITY_REPAIR_MAX_WAITERS 8

# The role ended with the handle: a new provider repairs the read.
parity_repair_wait provider $TESTPOOL $PARITY_SOURCE $PARITY_READY4 \
    $PARITY_RESULT &
PARITY_PROVIDER_PID=$!
log_must parity_wait 15 "test -f $PARITY_READY4"
log_must dd if=/$TESTPOOL/data of=$PARITY_READBACK bs=4096 count=1
wait $PARITY_PROVIDER_PID || log_fail "the provider did not supply the bytes"
PARITY_PROVIDER_PID=""
log_must zinject -c all
log_must cmp $PARITY_SOURCE $PARITY_READBACK
log_must test "$(kstat_pool $TESTPOOL parity_set.repair_supplied)" -ge 1

# A late provider gets the pending request again.
log_must zinject -a -t data -e checksum -f 100 /$TESTPOOL/data
log_mustnot dd if=/$TESTPOOL/data of=$PARITY_READBACK bs=4096 count=1
log_must zinject -c all
# Wrong bytes: REPAIR writes nothing and the request stays.
log_must dd if=/dev/urandom of=$PARITY_WRONG bs=4096 count=1
log_mustnot parity_repair_wait repost $TESTPOOL $PARITY_WRONG $PARITY_READY6 \
    $PARITY_RESULT
log_must test "$(kstat_pool $TESTPOOL parity_set.repair_rejected)" -ge 1
parity_repair_wait repost $TESTPOOL $PARITY_SOURCE $PARITY_READY5 \
    $PARITY_RESULT
log_must test $? -eq 0
log_must dd if=/$TESTPOOL/data of=$PARITY_READBACK bs=4096 count=1
log_must cmp $PARITY_SOURCE $PARITY_READBACK

log_must test "$(kstat_pool $TESTPOOL parity_set.repair_requests)" -ge 1
log_must test "$(kstat_pool $TESTPOOL parity_set.repair_written)" -ge 1

log_pass "per-pool provider, decline answer and waiter limit"
