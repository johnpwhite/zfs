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
# Operations that the parity records cannot follow are refused while records
# are pending, and the property is refused on a pool that cannot be a member.
#
# STRATEGY:
# 1. With records pending, check that checkpoint, reguid and online
#    expansion fail with a message that names the parity set.  A member
#    allows attach and add (parity_cols_attach_pos, parity_cols_add_pos).
#    A member also refuses checksum=off.
# 2. If zfs_parity_allow_expand exists, check that it permits the expansion.
# 3. Check that parityset=on fails on a pool with a checkpoint, on a pool
#    and with the feature disabled.  A stripe is allowed and raidz is refused
#    (parity_cols_eligible_pos, parity_cols_eligible_neg).
# 4. After the records are dropped and parityset is off, check that
#    checkpoint and reguid work.
# 5. Check that parityset=on fails when a dataset has checksum=off or
#    noparity, and that a member refuses to set, create or receive one.
#

verify_runnable "global"
parity_require

function cleanup
{
	parity_cleanup
}
log_onexit cleanup

log_assert "unsafe operations are refused while records are pending"

parity_create_pool
log_must truncate -s $PARITY_VDEV_SIZE $PARITY_VDEV2
log_must zpool add $TESTPOOL $PARITY_VDEV2
parity_arm

typeset out
out=$(zpool remove $TESTPOOL $PARITY_VDEV2 2>&1)
typeset remove_status=$?
log_must test $remove_status -ne 0
log_must eval 'echo "$out" | grep -qi "parity set"'
log_must libzfs_input_check --parity-columns $TESTPOOL 2 \
	    $PARITY_VDEV $PARITY_VDEV2

log_mustnot zpool checkpoint $TESTPOOL
out=$(zpool checkpoint $TESTPOOL 2>&1)
log_must eval 'echo "$out" | grep -qi "in a parity set"'
log_mustnot zpool reguid $TESTPOOL
log_mustnot zfs set checksum=off $TESTPOOL
log_must truncate -s +256M $PARITY_VDEV
log_mustnot zpool online -e $TESTPOOL $PARITY_VDEV
log_must test "$(get_pool_prop feature@parity_records $TESTPOOL)" == "active"

if tunable_exists PARITY_ALLOW_EXPAND; then
	log_must set_tunable64 PARITY_ALLOW_EXPAND 1
	log_must zpool online -e $TESTPOOL $PARITY_VDEV
	log_must set_tunable64 PARITY_ALLOW_EXPAND 0
fi

# Off first: a drop while the pool drains ends in off.  A drop while it is
# on starts a new epoch, whose records would then wait for a daemon.
log_must zpool set parityset=off $TESTPOOL
log_must zpool parity drop -f $TESTPOOL
log_must parity_wait 60 'test "$(parity_state)" == "off"'
# Off and drained, but still a member on disk: no zpool remove
if [[ "$(get_pool_prop feature@parity_records $TESTPOOL)" == "active" ]]; then
	log_mustnot zpool remove $TESTPOOL $PARITY_VDEV2
fi
log_must zpool checkpoint $TESTPOOL
log_must zpool checkpoint -d $TESTPOOL
log_must zpool reguid $TESTPOOL
log_must zpool destroy $TESTPOOL

# The property is refused on a pool that cannot be a member.
parity_create_pool
log_must zpool checkpoint $TESTPOOL
log_mustnot zpool set parityset=on $TESTPOOL
log_must zpool checkpoint -d $TESTPOOL
log_must zpool destroy $TESTPOOL

rm -f $PARITY_VDEV
log_must truncate -s $PARITY_VDEV_SIZE $PARITY_VDEV
log_must zpool create -f -d $TESTPOOL $PARITY_VDEV
log_mustnot zpool set parityset=on $TESTPOOL
log_must zpool destroy $TESTPOOL

# 3a. A dataset with checksum=off or noparity, set or inherited, refuses
# parityset=on, and the message names the reason.
rm -f $PARITY_VDEV
log_must truncate -s $PARITY_VDEV_SIZE $PARITY_VDEV
log_must zpool create -f $TESTPOOL $PARITY_VDEV
log_must zfs create $TESTPOOL/fs
log_must zfs create $TESTPOOL/fs/child
log_must zfs set checksum=off $TESTPOOL/fs
typeset out=$(zpool set parityset=on $TESTPOOL 2>&1)
log_mustnot zpool set parityset=on $TESTPOOL
log_must eval 'echo "$out" | grep -qi "checksum"'
log_must test "$(get_pool_prop parityset $TESTPOOL)" == "off"
# The child inherits the value: it is refused on its own too.
log_must zfs set checksum=sha256 $TESTPOOL/fs
log_must zfs set checksum=noparity $TESTPOOL/fs/child
log_mustnot zpool set parityset=on $TESTPOOL
log_must zfs inherit checksum $TESTPOOL/fs/child
log_must zfs create -V 8M -o checksum=off $TESTPOOL/vol
log_mustnot zpool set parityset=on $TESTPOOL
log_must zfs destroy $TESTPOOL/vol

# 3b. Once the datasets are clean the pool joins, and then a new off or
# noparity checksum is refused: set, create and receive.  An inherit of the
# parent value (not off) still works.
log_must zfs snapshot $TESTPOOL/fs@snap
log_must zpool set parityset=on $TESTPOOL
log_mustnot zfs set checksum=off $TESTPOOL/fs
log_mustnot zfs set checksum=noparity $TESTPOOL/fs/child
log_mustnot zfs create -o checksum=off $TESTPOOL/fs2
log_mustnot zfs create -o checksum=noparity $TESTPOOL/fs2
log_mustnot eval "zfs send $TESTPOOL/fs@snap | zfs recv -o checksum=off $TESTPOOL/fs3"
log_mustnot datasetexists $TESTPOOL/fs3
log_must zfs set checksum=sha256 $TESTPOOL/fs/child
log_must zfs inherit checksum $TESTPOOL/fs/child
log_must zfs inherit -S checksum $TESTPOOL/fs/child
log_must zpool set parityset=off $TESTPOOL
log_must zpool parity drop -f $TESTPOOL
log_must zpool destroy $TESTPOOL


log_pass "unsafe operations are refused while records are pending"
