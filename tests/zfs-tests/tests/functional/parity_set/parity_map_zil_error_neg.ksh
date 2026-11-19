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

# MAP returns an error when it cannot walk a live intent log.

verify_runnable "global"
parity_require

function cleanup
{
	parity_cleanup
}
log_onexit cleanup

log_assert "MAP reports an error from a live intent-log walk"

parity_create_pool
parity_arm
log_must zfs create -o sync=always $TESTPOOL/fs
log_must file_write -w -o create -f /$TESTPOOL/fs/data \
    -b 4096 -c 1 -d R
# Read the live log before a pool sync can retire its chain.
log_must libzfs_input_check --parity-map $TESTPOOL

log_must zinject -d $PARITY_VDEV -e io -T read -f 100 $TESTPOOL
log_mustnot libzfs_input_check --parity-map $TESTPOOL
log_must zinject -c all

log_pass "MAP reports an error from a live intent-log walk"
