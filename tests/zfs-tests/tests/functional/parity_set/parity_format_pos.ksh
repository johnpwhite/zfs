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

# Check that zdb reports the widened parity record format.

verify_runnable "global"
parity_require

function cleanup
{
	parity_cleanup
}
log_onexit cleanup

log_assert "zdb reports the parity record format"

parity_create_pool
parity_arm

typeset dump=$(zdb --parity -v $TESTPOOL)
typeset record_re='record txg [0-9]+: time [1-9][0-9]*'
log_must eval "print -r -- \"\$dump\" | grep -q 'parity set: version 3'"
log_must eval "print -r -- \"\$dump\" | grep -Eq '$record_re'"
log_must eval "print -r -- \"\$dump\" | grep -q 'reserved 0 0 0 0'"

log_pass "zdb reports the parity record format"
