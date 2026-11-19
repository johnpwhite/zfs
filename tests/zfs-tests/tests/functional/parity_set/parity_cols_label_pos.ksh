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
# LABEL reads a mirror member through a readable side when child zero is
# unavailable.
#
# STRATEGY:
# 1. Create a two-way mirror parity member.
# 2. Take child zero offline.
# 3. Request the current label and verify its uberblock and label image.
#

verify_runnable "global"
parity_require

function cleanup
{
	parity_cols_cleanup
}
log_onexit cleanup

log_assert "LABEL reads from an available mirror side"
parity_cols_prepare
log_must zpool create -f "$TESTPOOL" mirror "$PARITY_COLS_VDEV0" \
	    "$PARITY_COLS_VDEV1"
parity_arm

log_must zpool offline "$TESTPOOL" "$PARITY_COLS_VDEV0"
log_must check_pool_status "$TESTPOOL" "state" "DEGRADED"
parity_write /$TESTPOOL/labelmark 1
log_must libzfs_input_check --parity-label-current "$TESTPOOL"

log_pass "LABEL reads from an available mirror side"
