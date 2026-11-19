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
# parity_pace is on by default: a member whose writers outrun the parity
# software is slowed to its rate, and does not drop its records.  The kernel
# uses the default also after an import of a pool that never set it.
#
# STRATEGY:
# 1. Create a pool: parity_pace is "on" from the default source.
# 2. Set a held limit of 10% of the pool and a very large record limit. Arm.
#    Write a file and overwrite it, so that the held frees use about a third
#    of the held limit.  Check that the throttle level is "none" (no
#    progress yet).
# 3. Export and import the pool: parity_pace is still "on", from the default.
# 4. Make progress while a ticker syncs a txg each second, without setting
#    parity_pace.  Check that the level becomes "soft".
#

verify_runnable "global"
parity_require
parity_require_tunable PARITY_TEST_AUTO_APPLY
parity_require_tunable PARITY_PACE_IDLE_MS

typeset TICKER_PID=""

function stop_ticker
{
	if [[ -n $TICKER_PID ]]; then
		kill $TICKER_PID 2>/dev/null
		wait $TICKER_PID 2>/dev/null
		TICKER_PID=""
	fi
}

function cleanup
{
	stop_ticker
	parity_cleanup
}
log_onexit cleanup

function throttle
{
	parity_status_value throttle
}

log_assert "parity_pace is on by default, also after an import"

parity_create_pool
log_must test "$(zpool get -Hpo value,source parity_pace $TESTPOOL)" == \
    "on	default"
log_must zpool set parity_max_held=10 $TESTPOOL
log_must zpool set parity_max_records=64G $TESTPOOL
parity_arm
parity_write /$TESTPOOL/f 16
parity_write /$TESTPOOL/f 16
held=$(parity_status_value held_bytes)
log_note "held_bytes $held"
log_must test $held -ge $(( 12 * 1024 * 1024 ))
log_must test "$(throttle)" == "none"

log_must zpool export $TESTPOOL
log_must zpool import -d $PARITY_DIR $TESTPOOL
log_must test "$(zpool get -Hpo value,source parity_pace $TESTPOOL)" == \
    "on	default"

log_must parity_set_tunable PARITY_PACE_IDLE_MS 3000
log_must parity_set_tunable PARITY_TEST_AUTO_APPLY $(( 1 << 62 ))
( while true; do
	echo tick > /$TESTPOOL/tick
	zpool sync $TESTPOOL
	sleep 1
done ) &
TICKER_PID=$!
log_must parity_wait 30 'test "$(throttle)" == "soft"'
stop_ticker

log_pass "parity_pace is on by default, also after an import"
