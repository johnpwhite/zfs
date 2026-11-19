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
# A failed read without a repair provider fails at once. Scrub and sync do
# not wait for the provider.
#
# STRATEGY:
# 1. Write data and inject a checksum error.
# 2. Check that a normal read fails without a provider.
# 3. Check that scrub and zpool sync finish within five seconds each.
#

verify_runnable "global"
parity_require

function cleanup
{
	zinject -c all
	parity_cleanup
}
log_onexit cleanup

log_assert "repair waits do not block scrub or txg sync"

parity_create_pool
parity_require_tunable PARITY_REPAIR_WAIT_MS
log_must set_tunable64 PARITY_REPAIR_WAIT_MS 30000
parity_require_tunable PARITY_TEST_AUTO_APPLY
log_must set_tunable64 PARITY_TEST_AUTO_APPLY 1
parity_arm
parity_write /$TESTPOOL/data 8
parity_write /$TESTPOOL/mark 1

log_must zinject -a -t data -e checksum -f 100 /$TESTPOOL/data
log_mustnot dd if=/$TESTPOOL/data of=/dev/null bs=1M
log_must zpool scrub $TESTPOOL
log_must timeout 5 zpool sync $TESTPOOL
log_must timeout 5 zpool scrub -w $TESTPOOL

log_must zinject -c all
log_pass "repair waits do not block scrub or txg sync"
