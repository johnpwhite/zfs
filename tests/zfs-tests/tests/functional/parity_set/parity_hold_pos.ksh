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
# A freed block is held, so that its space does not come back, until the
# records are applied or dropped.
#
# STRATEGY:
# 1. Arm the parity set and note the free space.
# 2. Write a file and remove it. Check that the free space does not return
#    and that the held bytes cover the file.
# 3. Drop the records. Check that the space returns.
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

log_assert "freed blocks are held while the records are pending"

parity_create_pool
parity_arm
typeset -i free0=$(parity_free)

parity_write /$TESTPOOL/data $mb
typeset -i free1=$(parity_free)
log_must test $free1 -lt $(( free0 - mb * 1048576 + slack ))

log_must rm /$TESTPOOL/data
sync_pool $TESTPOOL
sync_pool $TESTPOOL
sync_pool $TESTPOOL
typeset -i free2=$(parity_free)
log_must test $free2 -lt $(( free1 + slack ))
log_must test "$(parity_held)" -ge $(( (mb - 6) * 1048576 ))

log_must zpool parity drop -f $TESTPOOL
log_must parity_wait 60 parity_free_at_least $(( free0 - slack ))
# The drop starts a new epoch (parityset stays on): the frees of the txgs
# after it are held again, so only a little may be held now.
log_must test "$(parity_held)" -lt $slack

log_pass "freed blocks are held while the records are pending"
