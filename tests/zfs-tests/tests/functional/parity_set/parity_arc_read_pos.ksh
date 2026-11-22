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
# READ takes a new block from the ARC when it is still cached in its
# on-disk form (#148), and the bytes are the same as from the disk.
#
# STRATEGY:
# 1. Arm the pool. Write random data (compression off) and repeated text
#    (compression lz4), then sync.
# 2. READV every ALLOC entry with the ARC path on: arc_hits grows, also for
#    the compressed entries.
# 3. READV them again with the ARC path off: arc_hits stays, read_bytes grows
#    by all the bytes, and the hash of the bytes is the same.
#

verify_runnable "global"
parity_require
parity_require_tunable PARITY_ARC_READ

function cleanup
{
	parity_cleanup
}

log_onexit cleanup

function hits
{
	kstat_pool $TESTPOOL parity_set.arc_hits
}

log_assert "READ takes cached new blocks from the ARC, with the disk's bytes"

log_must set_tunable64 PARITY_ARC_READ 1
parity_create_pool
parity_arm
log_must zfs create -o compression=off $TESTPOOL/plain
log_must zfs create -o compression=lz4 -o recordsize=128k $TESTPOOL/lz4
log_must file_write -o create -f /$TESTPOOL/plain/rand -b 131072 -c 16 -d R
typeset -i i
for i in {1..4096}; do
	echo "parity set arc read test line $i of a compressible file"
done > /$TESTPOOL/lz4/text
sync_pool $TESTPOOL

typeset h0=$(hits)
typeset c1=$(libzfs_input_check --parity-read-hash $TESTPOOL --compressed)
typeset h1=$(hits)
log_note "compressed, ARC on: $c1 (arc_hits $h0 -> $h1)"
[[ -n "$c1" ]] || log_fail "no compressed ALLOC entry was read"
(( h1 > h0 )) || log_fail "no compressed entry came from the ARC"

typeset a1=$(libzfs_input_check --parity-read-hash $TESTPOOL)
typeset h2=$(hits)
log_note "all, ARC on: $a1 (arc_hits $h1 -> $h2)"
(( h2 > h1 )) || log_fail "no entry came from the ARC"

log_must set_tunable64 PARITY_ARC_READ 0
typeset r0=$(kstat_pool $TESTPOOL parity_set.read_bytes)
typeset a2=$(libzfs_input_check --parity-read-hash $TESTPOOL)
typeset c2=$(libzfs_input_check --parity-read-hash $TESTPOOL --compressed)
typeset r1=$(kstat_pool $TESTPOOL parity_set.read_bytes)
typeset h3=$(hits)
log_note "ARC off: $a2 / $c2 (read_bytes $r0 -> $r1)"
(( h3 == h2 )) || log_fail "arc_hits grew with the ARC path off"

[[ "$a1" == "$a2" ]] || log_fail "bytes differ: ARC '$a1', disk '$a2'"
[[ "$c1" == "$c2" ]] || log_fail "compressed bytes differ: '$c1', '$c2'"
typeset -i b2=$(echo "$a2" | awk '{ print $3 }')
typeset -i bc=$(echo "$c2" | awk '{ print $3 }')
(( r1 - r0 == b2 + bc )) || \
    log_fail "disk reads $((r1 - r0)) bytes, expected $((b2 + bc))"

log_pass "READ takes cached new blocks from the ARC, with the disk's bytes"
