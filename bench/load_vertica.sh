#!/usr/bin/env bash
# Load a dataset into Vertica and build its vtext index; prints one JSON line with the timings.
#
#   bench/load_vertica.sh <dataset> [create_index options]      e.g. bench/load_vertica.sh msmarco 'lang=en'
#
# Table bench.<dataset> (doc_id, lang, body, ver) follows the source table contract of vtext
# (README "Prepare a table"). COPY reads $VTEXT_DATA/<dataset>/docs.dat (bench/data/prepare.py).
# Connection: vsql reads VSQL_HOST, VSQL_PORT, VSQL_USER, VSQL_PASSWORD, VSQL_DATABASE.
set -euo pipefail
DS=$1
OPTS=${2:-lang=en}
DATA=${VTEXT_DATA:-$HOME/vtext_data}
Q() { timeout "${VTEXT_TIMEOUT:-7200}" vsql -X -A -t -q -v ON_ERROR_STOP=1 -c "$1"; }
Q "CREATE SCHEMA IF NOT EXISTS bench" > /dev/null
vsql -X -q -c "CALL vtext.drop_index('$DS')" > /dev/null 2>&1 || true
t0=$(date +%s.%N)
if [ "${SKIP_COPY:-0}" != 1 ]; then       # SKIP_COPY=1: the table is loaded already, build the index only
    Q "DROP TABLE IF EXISTS bench.$DS CASCADE" > /dev/null
    Q "CREATE TABLE bench.$DS (doc_id INT NOT NULL, lang VARCHAR(8), body LONG VARCHAR(8000000),
                               ver TIMESTAMPTZ NOT NULL DEFAULT CLOCK_TIMESTAMP()) ORDER BY doc_id SEGMENTED BY HASH(doc_id) ALL NODES" > /dev/null
    t0=$(date +%s.%N)
    Q "COPY bench.$DS (doc_id, lang, body) FROM '$DATA/$DS/docs.dat' DELIMITER E'\001' RECORD TERMINATOR E'\002' NO ESCAPE DIRECT ABORT ON ERROR" > /dev/null
fi
t1=$(date +%s.%N)
Q "CALL vtext.create_index('$DS', 'bench.$DS', '$OPTS margin=0')" > /dev/null
t2=$(date +%s.%N)
docs=$(Q "SELECT COUNT(*) FROM bench.$DS")
segs=$(Q "SELECT COUNT(*) FROM vtext.segments s JOIN vtext.indexes i USING (index_id) WHERE i.index_name = '$DS' AND s.gen_removed IS NULL")
cache=$(Q "SELECT SUM(bytes) FROM vtext.segments s JOIN vtext.indexes i USING (index_id) WHERE i.index_name = '$DS' AND s.gen_removed IS NULL")
tables=$(Q "SELECT SUM(used_bytes) FROM v_monitor.projection_storage WHERE anchor_table_schema = 'vtext' AND anchor_table_name IN ('term_dict','postings','positions','doc_stats')")
source=$(Q "SELECT SUM(used_bytes) FROM v_monitor.projection_storage WHERE anchor_table_schema = 'bench' AND anchor_table_name = '$DS'")
printf '{"engine": "vtext", "dataset": "%s", "docs": %s, "copy_s": %.1f, "index_s": %.1f, "segments": %s, "cache_bytes": %s, "index_table_bytes": %s, "source_table_bytes": %s}\n' \
    "$DS" "$docs" "$(echo "$t1 - $t0" | bc)" "$(echo "$t2 - $t1" | bc)" "$segs" "$cache" "$tables" "$source"
