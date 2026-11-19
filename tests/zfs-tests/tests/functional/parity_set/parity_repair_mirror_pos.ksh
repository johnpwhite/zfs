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
# Parity repair writes correct data to the readable side of a degraded
# mirror, for both a waiting read and an asynchronous repair request.
#
# STRATEGY:
# 1. Make a two-way mirror member and take its first side offline.
# 2. Corrupt a block on the surviving side and repair it through a waiting
#    read. Check the physical block and read it again.
# 3. Corrupt another block and repair its asynchronous request. Check the
#    physical block and read it again.
#

verify_runnable "global"
parity_require

typeset PARITY_TOOL_PID=""
typeset PARITY_SOURCE1=$PARITY_COLS_DIR/source1.$$
typeset PARITY_SOURCE2=$PARITY_COLS_DIR/source2.$$
typeset PARITY_READY1=$PARITY_COLS_DIR/ready1.$$
typeset PARITY_READY2=$PARITY_COLS_DIR/ready2.$$
typeset PARITY_READY3=$PARITY_COLS_DIR/ready3.$$
typeset PARITY_RESULT1=$PARITY_COLS_DIR/result1.$$
typeset PARITY_RESULT2=$PARITY_COLS_DIR/result2.$$
typeset PARITY_READBACK=$PARITY_COLS_DIR/readback.$$
typeset PARITY_RAW=$PARITY_COLS_DIR/raw.$$

function cleanup
{
	if [[ -n $PARITY_TOOL_PID ]]; then
		kill -9 $PARITY_TOOL_PID 2>/dev/null
		wait $PARITY_TOOL_PID 2>/dev/null
	fi
	zinject -c all >/dev/null 2>&1
	parity_set_tunable PARITY_TEST_AUTO_APPLY \
	    $PARITY_TEST_AUTO_APPLY_DEFAULT
	parity_cols_cleanup
}
log_onexit cleanup

log_assert "parity repair writes the readable side of a mirror"
parity_cols_prepare
log_must zpool create -f "$TESTPOOL" mirror "$PARITY_COLS_VDEV0" \
	    "$PARITY_COLS_VDEV1"
log_must zfs set compression=off recordsize=4k primarycache=none \
	    $TESTPOOL
parity_require_tunable PARITY_TEST_AUTO_APPLY
log_must set_tunable64 PARITY_TEST_AUTO_APPLY 1
parity_arm

log_must dd if=/dev/urandom of=$PARITY_SOURCE1 bs=4096 count=1
log_must dd if=/dev/urandom of=$PARITY_SOURCE2 bs=4096 count=1
log_must cp $PARITY_SOURCE1 /$TESTPOOL/data1
log_must cp $PARITY_SOURCE2 /$TESTPOOL/data2
sync_pool $TESTPOOL
parity_write /$TESTPOOL/mark 1
log_must zpool offline "$TESTPOOL" "$PARITY_COLS_VDEV0"

# Exercise the synchronous write while a read waits for its repair data.
parity_repair_wait provider $TESTPOOL $PARITY_SOURCE1 \
	$PARITY_READY1 $PARITY_RESULT1 &
PARITY_TOOL_PID=$!
log_must parity_wait 15 "test -f $PARITY_READY1"
log_must zinject -a -t data -e checksum -f 100 /$TESTPOOL/data1
log_must dd if=/$TESTPOOL/data1 of=$PARITY_READBACK bs=4096 count=1
wait $PARITY_TOOL_PID || log_fail "the sync repair tool failed"
PARITY_TOOL_PID=""
log_must zinject -c all
log_must cmp $PARITY_SOURCE1 $PARITY_READBACK

typeset offset asize
read offset asize < $PARITY_RESULT1
log_must test -n "$offset" -a -n "$asize"
log_must parity_repair_wait corrupt $PARITY_COLS_VDEV1 $offset
parity_repair_wait provider $TESTPOOL $PARITY_SOURCE1 \
	$PARITY_READY2 $PARITY_RESULT1 &
PARITY_TOOL_PID=$!
log_must parity_wait 15 "test -f $PARITY_READY2"
log_must dd if=/$TESTPOOL/data1 of=$PARITY_READBACK bs=4096 count=1
wait $PARITY_TOOL_PID || log_fail "the mirror repair tool failed"
PARITY_TOOL_PID=""
log_must cmp $PARITY_SOURCE1 $PARITY_READBACK
log_must dd if=$PARITY_COLS_VDEV1 of=$PARITY_RAW bs=512 \
	    skip=$(((offset + 4194304) / 512)) count=$((asize / 512)) \
	    2>/dev/null
log_must cmp $PARITY_SOURCE1 $PARITY_RAW

# Exercise the asynchronous repair request after a read fails.
log_must zpool events -c
log_must zinject -a -t data -e checksum -f 100 /$TESTPOOL/data2
log_mustnot dd if=/$TESTPOOL/data2 of=/dev/null bs=4096 count=1
log_must parity_wait_event parity_repair_needed 30
typeset async_offset=$(parity_event_field parity_repair_needed offset)
log_must test -n "$async_offset"
log_must zinject -c all
log_must parity_repair_wait corrupt $PARITY_COLS_VDEV1 $async_offset
parity_repair_wait repost $TESTPOOL $PARITY_SOURCE2 \
	$PARITY_READY3 $PARITY_RESULT2 &
PARITY_TOOL_PID=$!
log_must parity_wait 15 "test -f $PARITY_READY3"
wait $PARITY_TOOL_PID || log_fail "the async repair tool failed"
PARITY_TOOL_PID=""
read offset asize < $PARITY_RESULT2
# the event gives the offset in hex, the tool in decimal: compare numbers
log_must test $((offset)) -eq $((async_offset))
log_must dd if=$PARITY_COLS_VDEV1 of=$PARITY_RAW bs=512 \
	    skip=$(((offset + 4194304) / 512)) count=$((asize / 512)) \
	    2>/dev/null
log_must cmp $PARITY_SOURCE2 $PARITY_RAW
log_must dd if=/$TESTPOOL/data2 of=$PARITY_READBACK bs=4096 count=1
log_must cmp $PARITY_SOURCE2 $PARITY_READBACK

log_pass "parity repair writes the readable side of a mirror"
