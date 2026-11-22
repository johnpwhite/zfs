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
# When records are applied, ZFS releases them and the held space returns.
# The test uses zfs_parity_test_auto_apply in place of the daemon.
#
# STRATEGY:
# 1. Arm the parity set, write a file and remove it.
# 2. With zfs_parity_suspend_release=1 check that nothing is released.
# 3. Set zfs_parity_test_auto_apply=1. Check that the space returns and that
#    the pending and held bytes go to 0.
# 4. Repeat with a small zfs_parity_release_limit.
# 5. Set parityset=off. Check that the state goes to "off" and the feature
#    goes back to "enabled".
#

verify_runnable "global"
parity_require
parity_require_tunable PARITY_TEST_AUTO_APPLY

function cleanup
{
	parity_cleanup
}
log_onexit cleanup

typeset -i mb=$PARITY_DATA_MB
typeset -i slack=$(( 16 * 1048576 ))

function churn
{
	parity_write /$TESTPOOL/data $mb
	log_must rm /$TESTPOOL/data
	sync_pool $TESTPOOL
}

# A release runs only in a txg that writes: an idle pool stays idle, also when
# records are applied.  So each wait below writes one small block first; that
# block frees the one before it, so a little stays held (about 2 MiB with a
# release limit of 64 entries, in the test VM).
function kick
{
	file_write -o create -f /$TESTPOOL/kick -b 4096 -c 1 -d R >/dev/null
	sync_pool $TESTPOOL
}

function free_back # bytes
{
	kick
	parity_free_at_least $1
}

function held_small
{
	kick
	(( $(parity_held) < slack ))
}

log_assert "applied records are released and held space returns"

parity_create_pool
parity_arm
typeset -i free0=$(parity_free)

if tunable_exists PARITY_SUSPEND_RELEASE; then
	log_must set_tunable64 PARITY_SUSPEND_RELEASE 1
	churn
	log_must set_tunable64 PARITY_TEST_AUTO_APPLY 1
	sync_pool $TESTPOOL
	sync_pool $TESTPOOL
	sync_pool $TESTPOOL
	sleep 5
	log_must test "$(parity_held)" -gt 0
	log_must set_tunable64 PARITY_SUSPEND_RELEASE 0
else
	log_must set_tunable64 PARITY_TEST_AUTO_APPLY 1
	churn
fi
log_must parity_wait 90 free_back $(( free0 - slack ))
log_must parity_wait 60 held_small
log_must test "$(parity_state)" == "on"

if tunable_exists PARITY_RELEASE_LIMIT; then
	log_must set_tunable64 PARITY_RELEASE_LIMIT 64
	churn
	log_must parity_wait 120 free_back $(( free0 - slack ))
	log_must parity_wait 120 held_small
fi

log_must zpool set parityset=off $TESTPOOL
log_must parity_wait 90 'sync_pool $TESTPOOL; test "$(parity_state)" == "off"'
log_must parity_wait 60 \
    'test "$(get_pool_prop feature@parity_records $TESTPOOL)" == "enabled"'

log_pass "applied records are released and held space returns"
