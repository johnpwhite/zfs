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
# When the unapplied records reach the limit, the pool throttles writers and
# posts parity_throttle events.
#
# STRATEGY:
# 1. Set parity_onstall=wait and a small parity_max_records. Arm the set.
# 2. Start a large write in the background.
# 3. Check that a parity_throttle event with level "hard" arrives and has
#    the pending_bytes and held_bytes fields, and that the status shows
#    the record and held use with their limits.
#    The kstat parity_set shows throttle 2 and a positive throttled_ms.
# 4. Check that the writer is still blocked.
# 5. Raise the limit. Check that the writer finishes.
#

verify_runnable "global"
parity_require

function cleanup
{
	parity_cleanup
}
log_onexit cleanup

log_assert "the hard limit throttles writers and posts parity_throttle"

parity_create_pool
log_must zpool set parity_onstall=wait $TESTPOOL
log_must zpool set parity_max_records=$PARITY_SMALL_LIMIT $TESTPOOL
log_must zpool events -c
parity_arm
parity_write_bg /$TESTPOOL/big $PARITY_DATA_MB

log_must parity_wait_event parity_throttle 60
log_must parity_wait 60 \
    'test "$(parity_event_field parity_throttle level)" == "hard"'
log_must test -n "$(parity_event_field parity_throttle pending_bytes)"
log_must test -n "$(parity_event_field parity_throttle held_bytes)"
log_must test -n "$(parity_event_field parity_throttle pool_guid)"
# The status shows the usage and the limits that the throttle compares.
log_must test "$(parity_status_value max_record_bytes)" -eq $PARITY_SMALL_LIMIT
log_must test -n "$(parity_status_value record_bytes)"
log_must test "$(parity_status_value max_held_bytes)" -gt 0
log_must kill -0 $PARITY_WRITER_PID
# The kstat counts the time at the hard level.
log_must test "$(kstat_pool $TESTPOOL parity_set.throttle)" -eq 2
log_must test "$(kstat_pool $TESTPOOL parity_set.throttled_ms)" -gt 0

log_must zpool set parity_max_records=0 $TESTPOOL
log_must parity_wait 120 '! kill -0 $PARITY_WRITER_PID 2>/dev/null'
log_must wait $PARITY_WRITER_PID
PARITY_WRITER_PID=""

log_pass "the hard limit throttles writers and posts parity_throttle"
