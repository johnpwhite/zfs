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
# A replace of the disk of a parity set member is always a sequential
# rebuild, so the new disk holds every range that parity counts as
# allocated, and not only the live blocks. The ranges of blocks that were
# freed (and are held, or wait to be freed after a snapshot destroy) are
# the ones that a healing resilver would miss.
#
# STRATEGY:
# 1. Arm the parity set. Write a file, take a snapshot, remove the file
#    and destroy the snapshot: its blocks are freed, so they are held.
#    Note the DVAs of the file's blocks before the remove.
# 2. Run "zpool replace" without -s. Check that the history shows a
#    rebuild, and wait until the replace is done.
# 3. Check that the old and the new disk hold the same bytes at each DVA
#    of the freed file.
# 4. Wait for the scrub that follows the rebuild and check that the pool
#    has no errors.
#

verify_runnable "global"
parity_require

function cleanup
{
	parity_cleanup
}
log_onexit cleanup

log_assert "a replace on a parity set member is a sequential rebuild"

parity_create_pool
log_must zfs create -o recordsize=1M $TESTPOOL/fs
parity_arm
log_must truncate -s $PARITY_VDEV_SIZE $PARITY_VDEV2

parity_write /$TESTPOOL/fs/data 16
typeset -i obj=$(ls -i /$TESTPOOL/fs/data | awk '{print $1}')
log_must zfs snapshot $TESTPOOL/fs@s1
sync_pool $TESTPOOL
typeset dvas=$(zdb -ddddd $TESTPOOL/fs $obj |
    awk '$2 == "L0" && $3 ~ /^[0-9]+:[0-9a-f]+:[0-9a-f]+$/ { print $3 }')
log_must test -n "$dvas"

log_must rm /$TESTPOOL/fs/data
log_must zfs destroy $TESTPOOL/fs@s1
sync_pool $TESTPOOL
sync_pool $TESTPOOL
log_must test "$(parity_held)" -ge $(( 8 * 1048576 ))

log_must zpool replace $TESTPOOL $PARITY_VDEV $PARITY_VDEV2
log_must parity_wait 120 'zpool history -i $TESTPOOL | grep -q "rebuild.*started"'
wait_replacing $TESTPOOL 120
log_mustnot is_pool_replacing $TESTPOOL

# The label area is 4 MiB: the DVA offset starts after it.
typeset dva off len
for dva in $dvas; do
	off=$(( 0x$(echo $dva | cut -d: -f2) + 4194304 ))
	len=$(( 0x$(echo $dva | cut -d: -f3) ))
	log_must cmp -i $off:$off -n $len $PARITY_VDEV $PARITY_VDEV2
done

wait_scrubbed $TESTPOOL 120
log_must test "$(zpool status -x $TESTPOOL)" == \
    "pool '$TESTPOOL' is healthy"

log_pass "a replace on a parity set member is a sequential rebuild"
