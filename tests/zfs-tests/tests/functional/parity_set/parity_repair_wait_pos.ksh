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
# A registered parity provider repairs a corrupt block before the read
# returns. Without a provider, the read fails as before.
#
# STRATEGY:
# 1. Write one uncompressed data block to an applied parity member.
# 2. Inject a checksum error with a provider and record the block offset.
# 3. Corrupt that block, then read it with a provider and check the repair.
# 4. Corrupt it again without a provider and check that the read fails.
#

verify_runnable "global"
parity_require

typeset PARITY_PROVIDER_PID=""
typeset PARITY_READ_PID=""
typeset PARITY_SOURCE=$PARITY_DIR/source.$$
typeset PARITY_PROVIDER_READY1=$PARITY_DIR/provider-ready1.$$
typeset PARITY_PROVIDER_READY2=$PARITY_DIR/provider-ready2.$$
typeset PARITY_RESULT=$PARITY_DIR/provider-result.$$
typeset PARITY_READBACK=$PARITY_DIR/readback.$$
typeset PARITY_RAW=$PARITY_DIR/raw-block.$$
typeset PARITY_READ_STATUS=$PARITY_DIR/read-status.$$

function cleanup
{
	if [[ -n $PARITY_PROVIDER_PID ]]; then
		kill -9 $PARITY_PROVIDER_PID 2>/dev/null
		wait $PARITY_PROVIDER_PID 2>/dev/null
	fi
	if [[ -n $PARITY_READ_PID ]]; then
		kill -9 $PARITY_READ_PID 2>/dev/null
		wait $PARITY_READ_PID 2>/dev/null
	fi
	parity_cleanup
}
log_onexit cleanup

log_assert "a parity provider repairs a block before read completion"

parity_create_pool
parity_require_tunable PARITY_REPAIR_WAIT_MS
log_must set_tunable64 PARITY_REPAIR_WAIT_MS 30000
parity_require_tunable PARITY_TEST_AUTO_APPLY
log_must set_tunable64 PARITY_TEST_AUTO_APPLY 1
log_must zfs set compression=off recordsize=4k primarycache=none $TESTPOOL
parity_arm
log_must dd if=/dev/urandom of=$PARITY_SOURCE bs=4096 count=1
log_must cp $PARITY_SOURCE /$TESTPOOL/data
sync_pool $TESTPOOL
parity_write /$TESTPOOL/mark 1
log_must cmp $PARITY_SOURCE /$TESTPOOL/data

parity_repair_wait provider $TESTPOOL $PARITY_SOURCE \
	$PARITY_PROVIDER_READY1 $PARITY_RESULT &
PARITY_PROVIDER_PID=$!
log_must parity_wait 15 "test -f $PARITY_PROVIDER_READY1"
log_must zinject -a -t data -e checksum -f 100 /$TESTPOOL/data
log_must dd if=/$TESTPOOL/data of=$PARITY_READBACK bs=4096 count=1
wait $PARITY_PROVIDER_PID || log_fail "provider did not supply repair data"
PARITY_PROVIDER_PID=""
log_must zinject -c all
log_must cmp $PARITY_SOURCE $PARITY_READBACK

typeset offset asize
read offset asize < $PARITY_RESULT
log_must test -n "$offset" -a -n "$asize"
log_must parity_repair_wait corrupt $PARITY_VDEV $offset

parity_repair_wait provider $TESTPOOL $PARITY_SOURCE \
	$PARITY_PROVIDER_READY2 $PARITY_RESULT &
PARITY_PROVIDER_PID=$!
log_must parity_wait 15 "test -f $PARITY_PROVIDER_READY2"
log_must dd if=/$TESTPOOL/data of=$PARITY_READBACK bs=4096 count=1
wait $PARITY_PROVIDER_PID || log_fail "provider did not supply repair data"
PARITY_PROVIDER_PID=""
log_must cmp $PARITY_SOURCE $PARITY_READBACK
read offset asize < $PARITY_RESULT
log_must test -n "$offset" -a -n "$asize"

log_must dd if=$PARITY_VDEV of=$PARITY_RAW bs=512 \
    skip=$(((offset + 4194304) / 512)) count=$((asize / 512)) 2>/dev/null
log_must cmp $PARITY_SOURCE $PARITY_RAW

log_must parity_repair_wait corrupt $PARITY_VDEV $offset
(
	dd if=/$TESTPOOL/data of=$PARITY_READBACK bs=4096 count=1 2>/dev/null
	echo $? > $PARITY_READ_STATUS
) &
PARITY_READ_PID=$!
log_must parity_wait 5 "test -f $PARITY_READ_STATUS"
wait $PARITY_READ_PID
PARITY_READ_PID=""
typeset read_error
read read_error < $PARITY_READ_STATUS
log_must test $read_error -ne 0

log_pass "a parity provider repairs a block before read completion"
