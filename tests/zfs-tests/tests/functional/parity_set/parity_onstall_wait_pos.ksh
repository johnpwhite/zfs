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
# With parity_onstall=wait, a writer at the hard limit waits. The pool does not
# drop its records and does not post parity_unprotected.
#
# STRATEGY:
# 1. Set parity_onstall=wait and a small parity_max_records.
# 2. Start a large write in the background.
# 3. Check that the writer is still blocked after 15 seconds and that no
#    parity_unprotected event exists.
# 4. Raise the limit. Check that the writer finishes with the whole file.
#

verify_runnable "global"
parity_require

function cleanup
{
	parity_cleanup
}
log_onexit cleanup

log_assert "parity_onstall=wait blocks writers and keeps the records"

parity_create_pool
log_must zpool set parity_onstall=wait $TESTPOOL
log_must zpool set parity_max_records=$PARITY_SMALL_LIMIT $TESTPOOL
log_must zpool events -c
parity_arm
parity_write_bg /$TESTPOOL/big $PARITY_DATA_MB

sleep 15
log_must kill -0 $PARITY_WRITER_PID
log_mustnot parity_event_seen parity_unprotected
log_mustnot eval "zpool parity status $TESTPOOL | grep -q 'not protected'"
log_must test "$(parity_pending)" -gt 0

log_must zpool set parity_max_records=0 $TESTPOOL
log_must parity_wait 120 '! kill -0 $PARITY_WRITER_PID 2>/dev/null'
log_must wait $PARITY_WRITER_PID
PARITY_WRITER_PID=""
sync_pool $TESTPOOL
log_must test "$(wc -c < /$TESTPOOL/big)" -eq $(( PARITY_DATA_MB * 1048576 ))
log_mustnot parity_event_seen parity_unprotected

log_pass "parity_onstall=wait blocks writers and keeps the records"
