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
# TRIM does not touch a held block. The daemon needs the bytes of a held
# block, so the vdev must keep them until the records are applied.
#
# STRATEGY:
# 1. Arm the parity set, write a file, remove it.
# 2. Run a manual TRIM. Check that the vdev file does not shrink.
# 3. Drop the records and run TRIM again. Check that the vdev file shrinks.
#

verify_runnable "global"
parity_require

function cleanup
{
	parity_cleanup
}
log_onexit cleanup

function vdev_kb
{
	du -k $PARITY_VDEV | cut -f1
}

typeset -i mb=$PARITY_DATA_MB

log_assert "TRIM leaves held blocks alone"

parity_create_pool
zpool trim -w $TESTPOOL || log_unsupported "TRIM of a file vdev fails here"
parity_arm
parity_write /$TESTPOOL/data $mb
typeset -i kb1=$(vdev_kb)
log_must rm /$TESTPOOL/data
sync_pool $TESTPOOL
sync_pool $TESTPOOL
sync_pool $TESTPOOL
log_must zpool trim -w $TESTPOOL
typeset -i kb2=$(vdev_kb)
log_must test $kb2 -ge $(( kb1 - 8192 ))
log_must test "$(parity_held)" -gt 0

log_must zpool parity drop -f $TESTPOOL
log_must parity_wait 60 parity_free_at_least $(( 200 * 1048576 ))
log_must zpool trim -w $TESTPOOL
typeset -i kb3=$(vdev_kb)
# A file vdev keeps about 30 MiB after a TRIM even without a parity set
# (measured): ask for half of the data back.
log_must test $kb3 -lt $(( kb2 - (mb / 2) * 1024 ))

log_pass "TRIM leaves held blocks alone"
