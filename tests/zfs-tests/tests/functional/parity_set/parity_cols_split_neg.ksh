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

verify_runnable "global"
parity_require

typeset SPLITPOOL=${TESTPOOL}_split

function cleanup
{
	parity_cols_cleanup "$SPLITPOOL"
}
log_onexit cleanup

log_assert "a mirror member of a parity set cannot be split"
parity_cols_prepare

log_must zpool create -f "$TESTPOOL" mirror "$PARITY_COLS_VDEV0" \
	    "$PARITY_COLS_VDEV1"
log_must zpool set parityset=on "$TESTPOOL"
parity_cols_check_columns "$TESTPOOL" 1 "$PARITY_COLS_VDEV0" \
	    "$PARITY_COLS_VDEV1"

log_mustnot zpool split "$TESTPOOL" "$SPLITPOOL"
log_mustnot poolexists "$SPLITPOOL"
parity_cols_check_columns "$TESTPOOL" 1 "$PARITY_COLS_VDEV0" \
	    "$PARITY_COLS_VDEV1"

log_pass "a mirror member of a parity set cannot be split"
