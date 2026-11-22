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

# Compare windowed pages with the old BLOCKS answer on a large block tree.

verify_runnable "global"
parity_require

function cleanup
{
	parity_cleanup
}
log_onexit cleanup

log_assert "parity BLOCKS pages a list of allocated windows in one walk"

parity_create_pool
parity_arm
log_must zfs set recordsize=4K $TESTPOOL
log_must file_write -o create -f /$TESTPOOL/windowed.blocks \
	-b 4096 -c 20000 -d R
sync_pool $TESTPOOL
log_must libzfs_input_check --parity-blocks-windows $TESTPOOL

log_pass "parity BLOCKS pages a list of allocated windows in one walk"
