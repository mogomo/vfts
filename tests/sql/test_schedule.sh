#!/usr/bin/env bash
# The automatic refresh is opt-in (vfts 0.5.0):
#   - create_index without refresh_every makes no Vertica schedule and no trigger, and its notice
#     says how to refresh the index and how to schedule it
#   - refresh_every=N at create_index or alter_index makes them, and the notice names them
#   - alter_index refresh_every=0 removes them; changing the schedule needs a superuser
#   - a non-superuser's create_index with refresh_every says that no schedule was made
#   - import_index makes again the schedule the exported index had, and none when it had none
#   - info says whether the index is refreshed automatically
# Creates schema vf_sd, indexes vf_sd*, user vf_sd_u; drops them at the end.
set -uo pipefail
cd "$(dirname "$0")"; source ./lib.sh
echo "$TEST_NAME"

U=vf_sd_u; PW=Pw_vf_sd_u_9
cleanup() {
    for i in vf_sd vf_sd_a vf_sd_h vf_sd_u vf_sd_i vf_sd_j; do q "CALL vfts.drop_index('$i')" > /dev/null 2>&1; done
    q "DROP SCHEMA IF EXISTS vf_sd CASCADE" > /dev/null 2>&1
    q "DROP SCHEMA IF EXISTS vf_sd_own CASCADE" > /dev/null 2>&1
    q "DROP USER IF EXISTS $U CASCADE" > /dev/null 2>&1
}
cleanup
q "CREATE SCHEMA vf_sd;
   CREATE TABLE vf_sd.docs (doc_id INT NOT NULL, body LONG VARCHAR);
   INSERT INTO vf_sd.docs VALUES (1, 'contract with indemnity');
   INSERT INTO vf_sd.docs VALUES (2, 'holmes and watson');
   COMMIT;" > /dev/null
# qn 'SQL'   like q, with the notices (q runs vsql -q, which leaves them out)
qn() { timeout "$TIMEOUT" vsql -X -A -t -v ON_ERROR_STOP=1 -c "$1" 2>&1; }
as() { timeout "$TIMEOUT" env VSQL_USER="$U" VSQL_PASSWORD="$PW" vsql -X -A -t -v ON_ERROR_STOP=1 -c "$1" 2>&1; }
trig() { q "SELECT COUNT(*) FROM v_catalog.stored_proc_triggers WHERE schema_name = 'vfts' AND trigger_name = 'refresh_$1'"; }
sched() { q "SELECT COALESCE(MAX(date_time_string), '-') FROM v_catalog.user_schedules WHERE schema_name = 'vfts' AND schedule_name = 'refresh_$1_schedule'"; }
every() { q "SELECT refresh_every FROM vfts.indexes WHERE index_name = '$1'"; }
has() { if printf '%s' "$2" | grep -qF -- "$3"; then echo "  ok   $1"; else echo "  FAIL $1"; echo "       got:  $2"; echo "       want: ...$3..."; FAILS=$((FAILS + 1)); fi; }

echo "-- the default: no schedule"
out=$(qn "CALL vfts.create_index('vf_sd', 'vf_sd.docs')")
expect "no option: refresh_every 0, no trigger, no schedule" "$(every vf_sd) $(trig vf_sd) $(sched vf_sd)" "0 0 -"
has "the notice says how to refresh" "$out" "index vf_sd is refreshed when you run CALL vfts.refresh_index('vf_sd')"
has "the notice says how to schedule it" "$out" "a superuser runs CALL vfts.alter_index('vf_sd', 'refresh_every=1') (a Vertica schedule and the trigger vfts.refresh_vf_sd, every minute)"
has "the created notice is still there" "$out" "vfts: index vf_sd created on vf_sd.docs"
has "info: off, and how to turn it on" "$(qn "CALL vfts.info('vf_sd')")" \
    "automatic refresh: off: refreshed by CALL vfts.refresh_index('vf_sd'); scheduled: CALL vfts.alter_index('vf_sd', 'refresh_every=1')"
out=$(qn "CALL vfts.create_index('vf_sd_a', 'vf_sd.docs', 'refresh_every=0')")
expect "refresh_every=0 given: the same" "$(every vf_sd_a) $(trig vf_sd_a)" "0 0"
has "refresh_every=0 given: the same notice" "$out" "is refreshed when you run CALL vfts.refresh_index('vf_sd_a')"

echo "-- on request"
out=$(qn "CALL vfts.alter_index('vf_sd', 'refresh_every=1')")
expect "alter_index refresh_every=1: a trigger and a schedule" "$(every vf_sd) $(trig vf_sd) $(sched vf_sd)" "1 1 */1 * * * *"
has "the notice names the schedule and the trigger" "$out" \
    "index vf_sd refreshes itself every minute (Vertica schedule vfts.refresh_vf_sd_schedule, trigger vfts.refresh_vf_sd)"
has "info: every minute" "$(qn "CALL vfts.info('vf_sd')")" "automatic refresh: every minute"
out=$(qn "CALL vfts.alter_index('vf_sd', 'refresh_every=0')")
expect "alter_index refresh_every=0: neither" "$(every vf_sd) $(trig vf_sd) $(sched vf_sd)" "0 0 -"
has "the notice says it is off" "$out" "automatic refresh of index vf_sd off (schedule and trigger removed)"
out=$(qn "CALL vfts.alter_index('vf_sd', 'merge_factor=8')")
expect "another option leaves the schedule alone" "$(every vf_sd) $(trig vf_sd)" "0 0"
expect "and says nothing about it" "$(printf '%s' "$out" | grep -c 'refreshes itself\|automatic refresh')" "0"
out=$(qn "CALL vfts.create_index('vf_sd_h', 'vf_sd.docs', 'refresh_every=120')")
expect "create_index refresh_every=120: every two hours" "$(every vf_sd_h) $(trig vf_sd_h) $(sched vf_sd_h)" "120 1 0 */2 * * *"
has "the notice: every 2 hours" "$out" "index vf_sd_h refreshes itself every 2 hours (Vertica schedule vfts.refresh_vf_sd_h_schedule, trigger vfts.refresh_vf_sd_h); off: CALL vfts.alter_index('vf_sd_h', 'refresh_every=0')"
has "alter_index refresh_every=5: every 5 minutes" "$(qn "CALL vfts.alter_index('vf_sd_h', 'refresh_every=5')")" "refreshes itself every 5 minutes"
expect "the schedule follows" "$(sched vf_sd_h)" "*/5 * * * *"
has "alter_index refresh_every=60: every hour" "$(qn "CALL vfts.alter_index('vf_sd_h', 'refresh_every=60')")" "refreshes itself every hour"
expect_error "a malformed interval is refused" "CALL vfts.alter_index('vf_sd_h', 'refresh_every=90')" "refresh_every is in minutes"
expect "and changes nothing" "$(every vf_sd_h) $(sched vf_sd_h)" "60 0 */1 * * *"

echo "-- a user who is not a superuser"
q "CREATE USER $U IDENTIFIED BY '$PW'; GRANT USAGE ON SCHEMA vfts TO $U; GRANT SELECT ON vfts.indexes TO $U;
   GRANT EXECUTE ON PROCEDURE vfts.alter_index(VARCHAR, VARCHAR) TO $U;" > /dev/null
out=$(as "CALL vfts.alter_index('vf_sd_h', 'refresh_every=0')")
has "alter_index refresh_every by a non-superuser: refused" "$out" "refresh_every creates or removes a Vertica schedule and trigger: run it as a superuser"
expect "and the schedule stays" "$(every vf_sd_h) $(trig vf_sd_h)" "60 1"
# create_index by the owner of a table who is not a superuser (the privileges README "create_index" lists)
q "CREATE SCHEMA vf_sd_own; GRANT USAGE, CREATE ON SCHEMA vf_sd_own TO $U;
   GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA vfts TO $U;
   GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA vfts TO $U;" > /dev/null 2>&1
for s in $(q "SELECT sequence_name FROM v_catalog.sequences WHERE sequence_schema = 'vfts'"); do q "GRANT ALL ON SEQUENCE vfts.$s TO $U" > /dev/null 2>&1; done
as "CREATE TABLE vf_sd_own.docs (doc_id INT NOT NULL, body LONG VARCHAR); INSERT INTO vf_sd_own.docs VALUES (1, 'contract'); COMMIT;" > /dev/null
out=$(as "CALL vfts.create_index('vf_sd_u', 'vf_sd_own.docs', 'refresh_every=1')")
if printf '%s' "$out" | grep -q "created on vf_sd_own.docs"; then
    has "create_index refresh_every=1 by a non-superuser: no schedule, said so" "$out" \
        "index vf_sd_u is not refreshed automatically: scheduling needs a superuser (CALL vfts.alter_index('vf_sd_u', 'refresh_every=1') as one); until then CALL vfts.refresh_index('vf_sd_u')"
    expect "no trigger was made" "$(trig vf_sd_u)" "0"
    has "info: no trigger" "$(qn "CALL vfts.info('vf_sd_u')")" "automatic refresh: every minute (no trigger: run vfts.alter_index as a superuser)"
    has "a superuser makes it" "$(qn "CALL vfts.alter_index('vf_sd_u', 'refresh_every=1')")" "index vf_sd_u refreshes itself every minute"
    expect "then the trigger is there" "$(trig vf_sd_u)" "1"
else
    echo "  FAIL create_index by a non-superuser did not run"; echo "       got:  $(printf '%s' "$out" | tail -3)"; FAILS=$((FAILS + 1))
fi

echo "-- the short forms print their own notices (a nested CALL's notices do not reach the client)"
q "INSERT INTO vf_sd.docs VALUES (3, 'another contract'); COMMIT;" > /dev/null
q "CALL vfts.refresh_index('vf_sd')" > /dev/null 2>&1
q "CALL vfts.alter_index('vf_sd', 'merge_factor=2')" > /dev/null 2>&1
out=$(qn "CALL vfts.merge('vf_sd')")
has "merge(index): the tiered policy merged, and says so" "$out" "vfts: index vf_sd merged ("
has "with the segment counts" "$out" "1 segments now, 2 before"
expect "merge(index) with nothing to merge: no notice" "$(qn "CALL vfts.merge('vf_sd')" | grep -c 'merged')" "0"

echo "-- import_index: the schedule the exported index had"
q "CALL vfts.export_index('vf_sd_h')" > /dev/null 2>&1
out=$(qn "CALL vfts.import_index('vf_sd_i', 'vf_sd.vf_sd_h_vfts_export')")
expect "an index exported with refresh_every=60: imported with its schedule" "$(every vf_sd_i) $(trig vf_sd_i) $(sched vf_sd_i)" "60 1 0 */1 * * *"
has "the notice says so" "$out" "index vf_sd_i refreshes itself every hour, as when it was exported (trigger vfts.refresh_vf_sd_i; off: CALL vfts.alter_index('vf_sd_i', 'refresh_every=0'))"
q "CALL vfts.export_index('vf_sd')" > /dev/null 2>&1
out=$(qn "CALL vfts.import_index('vf_sd_j', 'vf_sd.vf_sd_vfts_export')")
expect "an index exported without one: imported without one" "$(every vf_sd_j) $(trig vf_sd_j)" "0 0"
expect "and no schedule notice" "$(printf '%s' "$out" | grep -c 'refreshes itself')" "0"

echo "-- drop_index removes what was scheduled"
for i in vf_sd_h vf_sd_i vf_sd_u; do q "CALL vfts.drop_index('$i')" > /dev/null 2>&1; done
expect "no trigger or schedule left" \
       "$(q "SELECT COUNT(*) FROM v_catalog.stored_proc_triggers WHERE schema_name = 'vfts' AND trigger_name LIKE 'refresh_vf_sd%'") $(q "SELECT COUNT(*) FROM v_catalog.user_schedules WHERE schema_name = 'vfts' AND schedule_name LIKE 'refresh_vf_sd%'")" "0 0"

cleanup
done_test
