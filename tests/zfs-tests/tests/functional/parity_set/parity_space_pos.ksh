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
# The space numbers of a parity set member add up.
#
# STRATEGY:
# 1. Arm the parity set and write a file. Check that the pending bytes
#    cover the file.
# 2. Remove the file. Check that the held bytes cover it, that the held
#    bytes are part of the allocated space and that allocated minus held is
#    close to the space that the dataset uses.
#

verify_runnable "global"
parity_require

function cleanup
{
	parity_cleanup
}
log_onexit cleanup

typeset -i mb=$PARITY_DATA_MB
typeset -i slack=$(( 16 * 1048576 ))

log_assert "pending, held and allocated space agree"

parity_create_pool
parity_arm
log_must test "$(parity_held)" -lt $slack
parity_write /$TESTPOOL/data $mb
log_must test "$(parity_pending)" -ge $(( (mb - 6) * 1048576 ))

log_must rm /$TESTPOOL/data
sync_pool $TESTPOOL
sync_pool $TESTPOOL
sync_pool $TESTPOOL
typeset -i held=$(parity_held)
typeset -i alloc=$(zpool list -Hp -o allocated $TESTPOOL)
typeset -i used=$(zfs get -Hp -o value used $TESTPOOL)
log_must test $held -ge $(( (mb - 6) * 1048576 ))
log_must test $held -le $alloc
log_must test $(( alloc - held )) -lt $(( used + slack ))
log_must test $(( alloc - held )) -gt $(( used - slack ))

log_pass "pending, held and allocated space agree"
