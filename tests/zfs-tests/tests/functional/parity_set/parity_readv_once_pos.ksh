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
# One READV call reads each block from disk once.  The kernel reads every
# block before it packs the reply; when the reply did not fit the first
# buffer of libzfs_core, the library called again with a larger buffer and
# the kernel read every block a second time.
#
# STRATEGY:
# 1. Create a pool, arm it and write 8 MiB with nothing applied.
# 2. READV the pending ALLOC records in one call (more than 1 MiB).
# 3. The kstat read_bytes grew by exactly the bytes of the reply.
#

verify_runnable "global"
parity_require

function cleanup
{
	parity_cleanup
}
log_onexit cleanup

log_assert "One READV call reads each block from disk once"

parity_create_pool
parity_arm
parity_write /$TESTPOOL/data 8

typeset before=$(kstat_pool $TESTPOOL parity_set.read_bytes)
typeset out=$(libzfs_input_check --parity-readv $TESTPOOL)
log_note "$out"
typeset after=$(kstat_pool $TESTPOOL parity_set.read_bytes)
typeset bytes=$(echo "$out" | awk '$1 == "readv" { print $3 }')

[[ -n "$before" && -n "$after" && -n "$bytes" ]] || \
    log_fail "no kstat read_bytes or no READV reply: '$before' '$after' '$out'"
(( bytes >= 1048576 )) || \
    log_fail "READV read only $bytes bytes: too few to need a large reply"
(( after - before == bytes )) || \
    log_fail "READV of $bytes bytes read $((after - before)) bytes from disk"

log_pass "One READV call reads each block from disk once"
