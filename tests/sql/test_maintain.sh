#!/usr/bin/env bash
# Node caches and the automatic refresh (vfts 0.5.0):
#   - the default cache directory is <catalog>/vfts (persistent), caches of /tmp/vfts are taken over
#   - a lost cache (a reboot with a cache on tmpfs, a new node): load_all writes the missing
#     segments only; a refresh, or vfts.maintain, repairs a node that lacks segments
#   - refresh_every makes a trigger that refreshes the index; a failure is noted, not lost
#   - a long live tail gives a warning
#   - drop_index of a missing index, caches of indexes that no longer exist, cleanup() of indexes
#     whose table is gone
# Creates schema vf_mt and indexes vf_mt*; drops them at the end. Takes about two minutes (it waits
# for the scheduler).
set -uo pipefail
cd "$(dirname "$0")"; source ./lib.sh
echo "$TEST_NAME"

cleanup() {
    for i in vf_mt vf_mt_a vf_mt_l vf_mt_o vf_mt_g; do q "CALL vfts.drop_index('$i')" > /dev/null 2>&1; done
    q "DROP SCHEMA IF EXISTS vf_mt CASCADE" > /dev/null 2>&1
}
cleanup
q "CREATE SCHEMA vf_mt;
   CREATE TABLE vf_mt.docs (doc_id INT NOT NULL, body LONG VARCHAR);
   INSERT INTO vf_mt.docs SELECT k, 'contract number ' || k || CASE WHEN MOD(k, 3) = 0 THEN ' holmes' ELSE '' END FROM vfts.probe WHERE k <= 300;
   CREATE TABLE vf_mt.gone (doc_id INT NOT NULL, body LONG VARCHAR);
   INSERT INTO vf_mt.gone VALUES (1, 'temporary');
   COMMIT;" > /dev/null
C() { q "SELECT count FROM (SELECT vfts.tcount(* USING PARAMETERS index='$1', query='$2') OVER() FROM vf_mt.$1_live) r" 2>&1; }
caches() { q "SELECT index_name FROM (SELECT vfts.tcaches(USING PARAMETERS action='list') OVER(PARTITION NODES) FROM vfts.probe) c WHERE index_name LIKE 'vf_mt%' ORDER BY 1" | tr '\n' ' '; }
lose() { q "SELECT COUNT(*) FROM (SELECT vfts.tdrop(USING PARAMETERS index='$1') OVER(PARTITION NODES) FROM vfts.probe) d" > /dev/null; }
trig() { q "SELECT COUNT(*) FROM v_catalog.stored_proc_triggers WHERE schema_name = 'vfts' AND trigger_name = 'refresh_$1'"; }
# qn 'SQL'   like q, with the notices and warnings (q runs vsql -q, which leaves them out)
qn() { timeout "$TIMEOUT" vsql -X -A -t -v ON_ERROR_STOP=1 -c "$1" 2>&1; }

echo "-- the cache directory"
q "CALL vfts.create_index('vf_mt', 'vf_mt.docs', 'refresh_every=0')" > /dev/null
expect "default cache directory: <catalog>/vfts" \
       "$(q "SELECT COUNT(*) FROM (SELECT vfts.tcaches(USING PARAMETERS action='list') OVER(PARTITION NODES) FROM vfts.probe) c WHERE index_name = 'vf_mt' AND cache_dir LIKE '%_catalog/vfts'")" \
       "$(q "SELECT COUNT(*) FROM (SELECT vfts.tnode(k) OVER(PARTITION NODES) FROM vfts.probe) n")"
expect "the cache knows its index id" \
       "$(q "SELECT COUNT(DISTINCT index_id) FROM (SELECT vfts.tcaches(USING PARAMETERS action='list') OVER(PARTITION NODES) FROM vfts.probe) c WHERE index_name = 'vf_mt'")" "1"

echo "-- a lost cache"
lose vf_mt
expect "search without a cache: refused with what to do" "$(C vf_mt contract | grep -o 'run CALL vfts.load_all')" "run CALL vfts.load_all"
expect "load_all writes the missing segments" "$(qn "CALL vfts.load_all('vf_mt')" | grep -o '1 segments, 1 written')" "1 segments, 1 written"
expect "then the search answers" "$(C vf_mt contract)" "300"
expect "load_all again writes nothing" "$(qn "CALL vfts.load_all('vf_mt')" | grep -o '1 segments, 0 written')" "1 segments, 0 written"
lose vf_mt
q "INSERT INTO vf_mt.docs VALUES (301, 'contract late'); COMMIT;" > /dev/null
q "CALL vfts.refresh_index('vf_mt')" > /dev/null
expect "a refresh repairs a node that lacks segments" "$(C vf_mt contract)" "301"
lose vf_mt
q "CALL vfts.maintain('vf_mt')" > /dev/null 2>&1
expect "maintain repairs it when nothing changed" "$(C vf_mt contract)" "301"

echo "-- caches of vfts 0.4 (/tmp/vfts)"
q "ALTER SESSION SET UDPARAMETER FOR vfts.vfts cache_dir = '/tmp/vfts'; CALL vfts.create_index('vf_mt_l', 'vf_mt.docs', 'refresh_every=0');" > /dev/null
expect "an index whose cache is in /tmp/vfts: not found in the new place" "$(C vf_mt_l contract | grep -o 'no cache')" "no cache"
q "CALL vfts.upgrade_caches()" > /dev/null
expect "upgrade_caches moves it: the search answers" "$(C vf_mt_l contract)" "301"

echo "-- a long live tail"
q "INSERT INTO vf_mt.docs SELECT k + 1000, 'tail contract' FROM vfts.probe WHERE k <= 30; COMMIT;" > /dev/null
t0=$(q "SELECT CLOCK_TIMESTAMP()::VARCHAR")
q "SELECT count FROM (SELECT vfts.tcount(* USING PARAMETERS index='vf_mt', query='contract', tail_warn=10) OVER() FROM vf_mt.vf_mt_live) r" > /dev/null
q "SELECT count FROM (SELECT vfts.tcount(* USING PARAMETERS index='vf_mt', query='holmes', tail_warn=0) OVER() FROM vf_mt.vf_mt_live) r" > /dev/null
# a UDx's warning goes to vertica.log and the Data Collector (dc_errors), not to the client
expect "a tail above tail_warn: a warning (dc_errors), tail_warn=0: none" \
       "$(q "SELECT COUNT(*) FROM dc_errors WHERE time >= '$t0'::TIMESTAMPTZ AND message ILIKE '%live tail of index vf_mt has 30 rows%'")" "1"

echo "-- the automatic refresh"
q "CALL vfts.create_index('vf_mt_a', 'vf_mt.docs', 'refresh_every=1')" > /dev/null
expect "refresh_every=1: a trigger" "$(trig vf_mt_a)" "1"
g0=$(q "SELECT gen FROM vfts.indexes WHERE index_name = 'vf_mt_a'")
q "INSERT INTO vf_mt.docs VALUES (2001, 'contract by the scheduler'); COMMIT;" > /dev/null
waited=0
while [ "$(q "SELECT gen FROM vfts.indexes WHERE index_name = 'vf_mt_a'")" = "$g0" ] && [ $waited -lt 150 ]; do sleep 5; waited=$((waited + 5)); done
expect "the scheduler refreshed the index (within 150 s)" "$(q "SELECT (gen > $g0)::INT FROM vfts.indexes WHERE index_name = 'vf_mt_a'")" "1"
expect "... and its live tail is empty" "$(q "SELECT COUNT(*) FROM vf_mt.vf_mt_a_live WHERE doc_id IS NOT NULL")" "0"
expect "info names the automatic refresh" "$(qn "CALL vfts.info('vf_mt_a')" | grep -o 'automatic refresh: every 1 minutes')" "automatic refresh: every 1 minutes"
q "CALL vfts.alter_index('vf_mt_a', 'refresh_every=0')" > /dev/null
expect "alter_index refresh_every=0: no trigger" "$(trig vf_mt_a)" "0"
q "CALL vfts.alter_index('vf_mt_a', 'refresh_every=120')" > /dev/null
expect "alter_index refresh_every=120: a trigger again" "$(trig vf_mt_a) $(q "SELECT refresh_every FROM vfts.indexes WHERE index_name = 'vf_mt_a'")" "1 120"
expect_error "refresh_every must fit cron" "CALL vfts.alter_index('vf_mt_a', 'refresh_every=90')" "refresh_every is in minutes"
q "CALL vfts.drop_index('vf_mt_a')" > /dev/null
expect "drop_index removes the trigger" "$(trig vf_mt_a)" "0"

echo "-- leftovers"
expect "drop_index of an index that does not exist: a notice, no error" "$(qn "CALL vfts.drop_index('vf_mt_a')" | grep -o 'does not exist')" "does not exist"
q "CALL vfts.create_index('vf_mt_o', 'vf_mt.docs', 'refresh_every=0')" > /dev/null
all=$(q "SELECT COUNT(*) FROM vf_mt.docs WHERE body::VARCHAR(1000) LIKE '%contract%'")
# the index of that name is gone (as after drop_index on another subcluster): its cache here is left over
q "UPDATE vfts.indexes SET index_name = 'vf_mt_o_moved' WHERE index_name = 'vf_mt_o'; COMMIT;" > /dev/null
q "CALL vfts.clean_caches()" > /dev/null
expect "clean_caches removes the cache of an index that no longer exists" "$(caches | grep -c 'vf_mt_o ')" "0"
q "UPDATE vfts.indexes SET index_name = 'vf_mt_o' WHERE index_name = 'vf_mt_o_moved'; COMMIT;" > /dev/null
q "CALL vfts.load_all('vf_mt_o')" > /dev/null
expect "... and load_all brings it back" "$(C vf_mt_o contract)" "$all"
q "CALL vfts.create_index('vf_mt_g', 'vf_mt.gone', 'refresh_every=0')" > /dev/null
q "DROP TABLE vf_mt.gone CASCADE" > /dev/null
q "CALL vfts.maintain('vf_mt_g')" > /dev/null 2>&1
expect "a failed automatic refresh is noted" "$(q "SELECT LEFT(refresh_note, 27) FROM vfts.indexes WHERE index_name = 'vf_mt_g'")" "automatic refresh failed at"
q "CALL vfts.cleanup()" > /dev/null
expect "cleanup drops the index whose table is gone, and its cache" \
       "$(q "SELECT COUNT(*) FROM vfts.indexes WHERE index_name = 'vf_mt_g'") $(caches | grep -c 'vf_mt_g')" "0 0"
expect "cleanup keeps the others" "$(C vf_mt contract)" "$all"

cleanup
done_test
