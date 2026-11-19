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
# A block that fails its checksum, or fails to read, makes the pool post
# parity_repair_needed. No daemon runs, so no repair is written, and the pool
# reads the block again when the fault is gone.
#
# STRATEGY:
# 1. Arm the parity set and write a file.
# 2. Inject a checksum error on the file with zinject. Read the file. The
#    read must fail.
# 3. Check the event: the reason is "checksum" and the fields are present.
# 4. Repeat with an I/O error. The reason is "io".
#    The kstat parity_set counts the requests of both reads.
# 5. Clear the faults. Check that the file reads with its original sum.
#

verify_runnable "global"
parity_require

function cleanup
{
	parity_cleanup
}
log_onexit cleanup

function check_fields
{
	typeset f
	for f in pool_guid vdev_guid request offset asize psize birth_txg \
	    cksum_type; do
		log_must test -n "$(parity_event_field parity_repair_needed $f)"
	done
}

log_assert "a failed read posts parity_repair_needed"

parity_create_pool
parity_arm
parity_write /$TESTPOOL/data 8
typeset sum=$(xxh128digest /$TESTPOOL/data)
# A second file for the io case: a block keeps its pending request (and is not
# posted again for 5 s), so the io case must fail other blocks.
parity_write /$TESTPOOL/data2 8

log_must zpool events -c
log_must zinject -a -t data -e checksum -f 100 /$TESTPOOL/data
log_mustnot dd if=/$TESTPOOL/data of=/dev/null bs=1M
log_must parity_wait_event parity_repair_needed 30
log_must test "$(parity_event_field parity_repair_needed reason)" == "checksum"
check_fields
log_must zinject -c all
typeset -i nreq=$(kstat_pool $TESTPOOL parity_set.repair_requests)
log_must test $nreq -ge 1

log_must zpool events -c
log_must zinject -a -t data -e io -f 100 /$TESTPOOL/data2
log_mustnot dd if=/$TESTPOOL/data2 of=/dev/null bs=1M
log_must parity_wait_event parity_repair_needed 30
log_must test "$(parity_event_field parity_repair_needed reason)" == "io"
check_fields
log_must zinject -c all
log_must test "$(kstat_pool $TESTPOOL parity_set.repair_requests)" -gt $nreq

log_must test "$(xxh128digest /$TESTPOOL/data)" == "$sum"

log_pass "a failed read posts parity_repair_needed"
