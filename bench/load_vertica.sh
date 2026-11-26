#!/usr/bin/env bash
# Load a dataset into Vertica, build its vfts index and merge it into one segment; prints one JSON
# line with the timings and sizes.
#
#   bench/load_vertica.sh <dataset> [create_index options]      e.g. bench/load_vertica.sh msmarco 'lang=en'
#
# Table bench.<dataset> (doc_id, lang, body, ver) follows the source table contract of vfts
# (README "Prepare a table"). COPY reads $VFTS_DATA/<dataset>/docs.dat (bench/data/prepare.py).
# Connection: vsql reads VSQL_HOST, VSQL_PORT, VSQL_USER, VSQL_PASSWORD, VSQL_DATABASE.
set -euo pipefail
DS=$1
OPTS=${2:-lang=en}
DATA=${VFTS_DATA:-$HOME/vfts_data}
Q() { timeout "${VFTS_TIMEOUT:-7200}" vsql -X -A -t -q -v ON_ERROR_STOP=1 -c "$1"; }
Q "CREATE SCHEMA IF NOT EXISTS bench" > /dev/null
vsql -X -q -c "CALL vfts.drop_index('$DS')" > /dev/null 2>&1 || true
t0=$(date +%s.%N)
if [ "${SKIP_COPY:-0}" != 1 ]; then       # SKIP_COPY=1: the table is loaded already, build the index only
    Q "DROP TABLE IF EXISTS bench.$DS CASCADE" > /dev/null
    Q "CREATE TABLE bench.$DS (doc_id INT NOT NULL, lang VARCHAR(8), body LONG VARCHAR(8000000),
                               ver TIMESTAMPTZ NOT NULL DEFAULT CLOCK_TIMESTAMP()) ORDER BY doc_id SEGMENTED BY HASH(doc_id) ALL NODES" > /dev/null
    t0=$(date +%s.%N)
    Q "COPY bench.$DS (doc_id, lang, body) FROM '$DATA/$DS/docs.dat' DELIMITER E'\001' RECORD TERMINATOR E'\002' NO ESCAPE DIRECT ABORT ON ERROR" > /dev/null
fi
t1=$(date +%s.%N)
Q "CALL vfts.create_index('$DS', 'bench.$DS', 'refresh_every=0 $OPTS margin=0')" > /dev/null
t2=$(date +%s.%N)
# merged into one segment, as the engines are force-merged into one segment per shard (VFTS_MERGE=0: not merged)
[ "${VFTS_MERGE:-1}" = 1 ] && Q "CALL vfts.merge('$DS', 1)" > /dev/null
t3=$(date +%s.%N)
docs=$(Q "SELECT COUNT(*) FROM bench.$DS")
segs=$(Q "SELECT COUNT(*) FROM vfts.segments s JOIN vfts.indexes i USING (index_id) WHERE i.index_name = '$DS' AND s.gen_removed IS NULL")
cache=$(Q "SELECT SUM(bytes) FROM vfts.segments s JOIN vfts.indexes i USING (index_id) WHERE i.index_name = '$DS' AND s.gen_removed IS NULL")
# rows of the merged-away segments are deleted by vfts.merge; purge them now (Vertica would at its
# next mergeout) so that the size is the size of the live index (benchmark database only)
Q "SELECT MAKE_AHM_NOW()" > /dev/null
for t in term_dict postings positions doc_stats tombstones; do Q "SELECT PURGE_TABLE('vfts.$t')" > /dev/null; done
# the index tables are partitioned by index_id and unsegmented: count this index's partitions on one node
tables=$(Q "SELECT COALESCE(SUM(p.ros_size_bytes), 0) FROM v_monitor.partitions p JOIN v_catalog.projections j ON j.projection_id = p.projection_id
            WHERE j.projection_schema = 'vfts' AND j.anchor_table_name IN ('term_dict', 'postings', 'positions', 'doc_stats')
              AND p.partition_key = (SELECT index_id FROM vfts.indexes WHERE index_name = '$DS')::VARCHAR
              AND p.node_name = (SELECT MIN(node_name) FROM v_catalog.nodes)")
source=$(Q "SELECT SUM(used_bytes) FROM v_monitor.projection_storage WHERE anchor_table_schema = 'bench' AND anchor_table_name = '$DS'")
# index_s = build + merge (the engines' index_s = bulk + refresh + force merge)
printf '{"engine": "vfts", "dataset": "%s", "docs": %s, "copy_s": %.1f, "build_s": %.1f, "merge_s": %.1f, "index_s": %.1f, "segments": %s, "cache_bytes": %s, "index_table_bytes": %s, "source_table_bytes": %s}\n' \
    "$DS" "$docs" "$(echo "$t1 - $t0" | bc)" "$(echo "$t2 - $t1" | bc)" "$(echo "$t3 - $t2" | bc)" "$(echo "$t3 - $t1" | bc)" "$segs" "$cache" "$tables" "$source"
