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
# The kstat parity_set publishes the parity state of a member pool without
# any service: it appears when the pool gets parity state, follows writes and
# applies, and goes away with the state.
#
# STRATEGY:
# 1. Create a pool: it has no parity_set kstat.
# 2. Arm it: the kstat has every field, state is 1, and the generation and
#    the applied txg equal those of zpool parity status.
# 3. Write 8 MiB with nothing applied: pending_bytes, records, entries and
#    record_txgs grow, lag_txgs is synced_txg minus applied_txg, and
#    applied_age_ms stays 0.
#    With a record limit below the records (a module parameter: no sync),
#    throttle reads 2 and throttled_ms grows on an idle pool.
# 4. Export and import: the kstat goes and comes back with the same applied
#    txg and pending bytes.
# 5. Apply with zfs_parity_test_auto_apply: the applied txg moves on, records
#    are released and the pending bytes go down.
#    applied_age_ms becomes positive.
# 6. Set parityset=off: state goes to 0 and lag_txgs to 0.  After an export
#    and import of the drained pool there is no kstat.
#

verify_runnable "global"
parity_require
parity_require_tunable PARITY_TEST_AUTO_APPLY
parity_require_tunable PARITY_MAX_RECORD_BYTES

function cleanup
{
	[[ -n "$PK_MAX_RECORDS" ]] && \
	    set_tunable64 PARITY_MAX_RECORD_BYTES $PK_MAX_RECORDS >/dev/null 2>&1
	parity_cleanup
}
log_onexit cleanup

typeset FIELDS="state generation set_guid start_txg applied_txg synced_txg \
    lag_txgs first_txg last_txg records pending_bytes pending_txgs held_bytes \
    record_bytes max_record_bytes max_held_bytes leaked_bytes throttle \
    throttled_ms applied_age_ms unprotected drop_pending record_txgs entries \
    released drops repair_requests repair_supplied repair_declined \
    repair_rejected repair_written read_bytes arc_hits arc_bytes"

function pk_present
{
	if is_linux; then
		test -e /proc/spl/kstat/zfs/$TESTPOOL/parity_set
	else
		sysctl -q kstat.zfs.$TESTPOOL.misc.parity_set >/dev/null 2>&1
	fi
}

function pk # field
{
	kstat_pool $TESTPOOL parity_set.$1
}

# One read of every field, as "name value" lines.
function pk_all
{
	kstat_pool -g $TESTPOOL parity_set
}

function pk_of # snapshot field
{
	echo "$1" | awk -v f="$2" '$1 == f { print $2 }'
}

# A release runs only in a txg that writes.
function kick
{
	file_write -o create -f /$TESTPOOL/kick -b 4096 -c 1 -d R >/dev/null
	sync_pool $TESTPOOL
}

# A string: ksh typeset -i holds 32 bits, and the default is 4 GiB.
typeset PK_MAX_RECORDS=$(get_tunable PARITY_MAX_RECORD_BYTES)
log_must test -n "$PK_MAX_RECORDS"

log_assert "the kstat parity_set publishes the parity state"

parity_create_pool
# A record limit below the records must not block the writes or drop them.
log_must zpool set parity_onstall=wait $TESTPOOL
log_mustnot pk_present

parity_arm
log_must pk_present
typeset all=$(pk_all)
typeset f
for f in $FIELDS; do
	[[ -n "$(pk_of "$all" $f)" ]] || log_fail "no field $f in the kstat"
done
log_must test "$(echo "$all" | wc -l)" -eq "$(echo $FIELDS | wc -w)"
log_must test "$(pk state)" -eq 1
log_must test "$(pk generation)" -eq "$(parity_status_value generation)"
log_must test "$(pk applied_txg)" -eq "$(parity_status_value applied_txg)"
log_must test "$(pk applied_age_ms)" -eq 0

typeset -i a0=$(pk applied_txg)
typeset -i e0=$(pk entries)
parity_write /$TESTPOOL/data 8
all=$(pk_all)
typeset -i synced=$(pk_of "$all" synced_txg)
typeset -i applied=$(pk_of "$all" applied_txg)
typeset -i lag=$(pk_of "$all" lag_txgs)
log_must test $applied -eq $a0
log_must test $lag -gt 0
log_must test $((synced - applied)) -eq $lag
log_must test "$(pk_of "$all" pending_bytes)" -ge $((8 * 1048576))
log_must test "$(pk_of "$all" records)" -gt 0
log_must test "$(pk_of "$all" entries)" -gt $e0
log_must test "$(pk_of "$all" record_txgs)" -gt 0
log_must test "$(pk_of "$all" applied_age_ms)" -eq 0
log_must test "$(pk pending_bytes)" -eq "$(parity_pending)"

# The throttle time follows the live level, also with no sync after it moved.
log_must test "$(pk throttle)" -eq 0
log_must test "$(pk throttled_ms)" -eq 0
log_must set_tunable64 PARITY_MAX_RECORD_BYTES 4096
log_must test "$(pk throttle)" -eq 2
sleep 2
log_must test "$(pk throttled_ms)" -ge 1000
log_must set_tunable64 PARITY_MAX_RECORD_BYTES $PK_MAX_RECORDS
log_must test "$(get_tunable PARITY_MAX_RECORD_BYTES)" == "$PK_MAX_RECORDS"
log_must test "$(pk throttle)" -eq 0
typeset -i thr=$(pk throttled_ms)
sleep 1
log_must test "$(pk throttled_ms)" -eq $thr

log_must zpool export $TESTPOOL
log_mustnot pk_present
log_must zpool import -d $PARITY_DIR $TESTPOOL
log_must pk_present
log_must test "$(pk state)" -eq 1
log_must test "$(pk applied_txg)" -eq $a0
log_must test "$(pk pending_bytes)" -ge $((8 * 1048576))

log_must set_tunable64 PARITY_TEST_AUTO_APPLY 1
log_must parity_wait 90 'kick; test "$(pk applied_txg)" -gt $a0 -a \
    "$(pk released)" -gt 0 -a "$(pk pending_bytes)" -lt 1048576'
sleep 1
log_must test "$(pk applied_age_ms)" -gt 0

log_must zpool set parityset=off $TESTPOOL
log_must parity_wait 90 'kick; test "$(parity_state)" == "off"'
log_must test "$(pk state)" -eq 0
log_must test "$(pk lag_txgs)" -eq 0
log_must zpool export $TESTPOOL
log_mustnot pk_present
log_must zpool import -d $PARITY_DIR $TESTPOOL
log_must test "$(get_pool_prop feature@parity_records $TESTPOOL)" == "enabled"
log_mustnot pk_present

log_pass "the kstat parity_set publishes the parity state"
