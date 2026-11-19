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

function cleanup
{
	parity_cols_cleanup
}
log_onexit cleanup

log_assert "raidz, special, dedup and over-wide mirror pools cannot join a parity set"
parity_cols_prepare

function check_ineligible
{
	log_must zpool create -f "$TESTPOOL" "$@"
	log_mustnot zpool set parityset=on "$TESTPOOL"
	log_must test "$(get_pool_prop parityset $TESTPOOL)" == "off"
	log_must zpool destroy -f "$TESTPOOL"
}

check_ineligible raidz "$PARITY_COLS_VDEV0" "$PARITY_COLS_VDEV1" \
	    "$PARITY_COLS_VDEV2"
check_ineligible "$PARITY_COLS_VDEV0" special "$PARITY_COLS_VDEV1"
check_ineligible "$PARITY_COLS_VDEV0" dedup "$PARITY_COLS_VDEV1"

# a mirror column takes at most 8 sides (SPA_PARITY_MAX_SIDES): a 9-way
# mirror cannot join, and a member cannot attach a ninth side
set -A wide
for i in 1 2 3 4 5 6 7 8 9; do
	wide[$i]=$PARITY_COLS_DIR/wide$i
	log_must truncate -s 128M "${wide[$i]}"
done
check_ineligible mirror "${wide[1]}" "${wide[2]}" "${wide[3]}" "${wide[4]}" \
	    "${wide[5]}" "${wide[6]}" "${wide[7]}" "${wide[8]}" "${wide[9]}"
log_must zpool create -f "$TESTPOOL" mirror "${wide[1]}" "${wide[2]}" \
	    "${wide[3]}" "${wide[4]}" "${wide[5]}" "${wide[6]}" "${wide[7]}" \
	    "${wide[8]}"
log_must zpool set parityset=on "$TESTPOOL"
log_mustnot zpool attach -f "$TESTPOOL" "${wide[1]}" "${wide[9]}"
sides=$(zpool list -Hv "$TESTPOOL" | grep -c "$PARITY_COLS_DIR/wide")
log_must test "$sides" -eq 8
log_must zpool destroy -f "$TESTPOOL"

log_pass "raidz, special, dedup and over-wide mirror pools cannot join a parity set"
