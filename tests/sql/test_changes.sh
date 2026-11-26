#!/usr/bin/env bash
# Changes found by the row epoch (vfts 0.5.0): a source table with nothing but doc_id and text (no
# version column, no journal). Every kind of write - INSERT, UPDATE, DELETE, DROP_PARTITIONS,
# MOVE_PARTITIONS_TO_TABLE (out of and into an indexed table), SWAP_PARTITIONS_BETWEEN_TABLES,
# COPY_PARTITIONS_TO_TABLE, TRUNCATE - then the results (ids, ranks, scores, counts) compared with an
# index built from scratch on the current rows: through the live view at once where the change
# gives rows a new epoch, and after one refresh_index for every change.
set -uo pipefail
cd "$(dirname "$0")"; source ./lib.sh
echo "$TEST_NAME"

VOCAB="contract indemnity holmes watson agreement breach party clause notice term liability damages court settlement payment"
for i in vf_ch vf_ch2 vf_ch_ref; do q "CALL vfts.drop_index('$i')" > /dev/null 2>&1; done
q "DROP SCHEMA IF EXISTS vf_ch CASCADE" > /dev/null
q "CREATE SCHEMA vf_ch;
   CREATE TABLE vf_ch.docs (doc_id INT NOT NULL, p INT NOT NULL, body LONG VARCHAR) PARTITION BY p;
   CREATE TABLE vf_ch.other (doc_id INT NOT NULL, p INT NOT NULL, body LONG VARCHAR) PARTITION BY p;
   CREATE TABLE vf_ch.stage (doc_id INT NOT NULL, p INT NOT NULL, body LONG VARCHAR) PARTITION BY p;" > /dev/null
gen_docs() {   # table first last seed -> INSERT of documents doc_id first..last, partition doc_id % 10
    q "INSERT INTO vf_ch.$1 (doc_id, p, body)
       SELECT doc_id, MOD(doc_id, 10), LISTAGG(w USING PARAMETERS separator=' ', max_length=8000)
       FROM (SELECT d.k AS doc_id, SPLIT_PART('$VOCAB', ' ', (1 + MOD(HASH(d.k, p.k, $4) // 5, 15))::INT) AS w
             FROM (SELECT a.k + 1024 * b.j AS k FROM vfts.probe a CROSS JOIN (SELECT 0 AS j UNION ALL SELECT 1 UNION ALL SELECT 2) b) d
             JOIN vfts.probe p ON p.k <= 4 + MOD(HASH(d.k, $4) // 13, 40)
             WHERE d.k BETWEEN $2 AND $3) x
       GROUP BY doc_id; COMMIT;" > /dev/null
}
gen_docs docs 1 2000 1
gen_docs other 3001 3300 7
q "CALL vfts.create_index('vf_ch', 'vf_ch.docs', 'refresh_every=0')" > /dev/null
q "CALL vfts.create_index('vf_ch2', 'vf_ch.other', 'refresh_every=0')" > /dev/null
expect "no version column: the index records an epoch boundary" \
       "$(q "SELECT (boundary_epoch IS NOT NULL AND ver_col IS NULL)::INT FROM vfts.indexes WHERE index_name = 'vf_ch'")" "1"

QUERIES=("holmes" "contract indemnity" "watson AND breach" "settlement OR holmes -court" "\"contract indemnity\"" "contract w/3 indemnity" "agree*" "zebra OR holmes")
R() {   # index view query -> doc_id:rank:score lines
    q "SELECT doc_id || ':' || rank || ':' || ROUND(score, 6) FROM (SELECT vfts.tsearch(* USING PARAMETERS index='$1', query='$3', k=30) OVER() FROM vf_ch.$2) r ORDER BY rank"
}
C() { q "SELECT count FROM (SELECT vfts.tcount(* USING PARAMETERS index='$1', query='$3') OVER() FROM vf_ch.$2) r"; }
# compare TABLE INDEX VIEW LABEL: every query on INDEX/VIEW equals a fresh index of a copy of TABLE
compare() {
    q "CALL vfts.drop_index('vf_ch_ref')" > /dev/null 2>&1
    q "DROP TABLE IF EXISTS vf_ch.ref; CREATE TABLE vf_ch.ref AS SELECT doc_id, body FROM vf_ch.$1; COMMIT;" > /dev/null
    q "CALL vfts.create_index('vf_ch_ref', 'vf_ch.ref', 'refresh_every=0')" > /dev/null
    local bad=0 qq a b
    for qq in "${QUERIES[@]}"; do
        a=$(R "$2" "$3" "$qq"); b=$(R vf_ch_ref vf_ch_ref_live "$qq")
        if [ "$a" != "$b" ]; then bad=$((bad + 1)); echo "       differs: $qq"; fi
    done
    expect "$4: ${#QUERIES[@]} queries equal a fresh index" "$bad" "0"
    expect "$4: counts equal" "$(C "$2" "$3" contract) $(C "$2" "$3" "watson OR holmes")" \
                              "$(C vf_ch_ref vf_ch_ref_live contract) $(C vf_ch_ref vf_ch_ref_live "watson OR holmes")"
}
gen_of() { q "SELECT gen FROM vfts.indexes WHERE index_name = '$1'"; }

# 1. INSERT and UPDATE without anything else: searchable at once through the live view
gen_docs docs 2001 2100 2
q "UPDATE vf_ch.docs SET body = 'watson watson breach zebra' WHERE doc_id BETWEEN 101 AND 140; COMMIT;" > /dev/null
expect "update without a version column: in the live view at once" "$(C vf_ch vf_ch_live zebra)" "40"
expect "the snap view does not see it before a refresh" "$(C vf_ch vf_ch_snap zebra)" "0"
compare docs vf_ch vf_ch_live "inserts and updates, live view, no refresh"
q "CALL vfts.refresh_index('vf_ch')" > /dev/null
expect "after refresh the tail is empty" "$(q "SELECT COUNT(*) FROM vf_ch.vf_ch_live WHERE doc_id IS NOT NULL")" "0"
compare docs vf_ch vf_ch_snap "inserts and updates, after refresh"

# 2. DELETE without a journal row: after one refresh
q "DELETE FROM vf_ch.docs WHERE doc_id BETWEEN 201 AND 260; COMMIT;" > /dev/null
q "CALL vfts.refresh_index('vf_ch')" > /dev/null
expect "refresh finds the 60 deleted rows" "$(q "SELECT refresh_note FROM vfts.indexes WHERE index_name = 'vf_ch'" | grep -o '60 documents left the source')" "60 documents left the source"
compare docs vf_ch vf_ch_snap "delete without journal, after refresh"

# 3. DROP_PARTITIONS
q "SELECT DROP_PARTITIONS('vf_ch.docs', 3, 3); COMMIT;" > /dev/null
q "CALL vfts.refresh_index('vf_ch')" > /dev/null
compare docs vf_ch vf_ch_snap "DROP_PARTITIONS, after refresh"

# 4. MOVE_PARTITIONS_TO_TABLE from one indexed table to another: out of vf_ch, into vf_ch2
q "SELECT MOVE_PARTITIONS_TO_TABLE('vf_ch.docs', 4, 4, 'vf_ch.other'); COMMIT;" > /dev/null
compare other vf_ch2 vf_ch2_live "moved into a table: live view at once"
q "CALL vfts.refresh_index('vf_ch')" > /dev/null
q "CALL vfts.refresh_index('vf_ch2')" > /dev/null
compare docs vf_ch vf_ch_snap "MOVE_PARTITIONS_TO_TABLE, source side, after refresh"
compare other vf_ch2 vf_ch2_snap "MOVE_PARTITIONS_TO_TABLE, target side, after refresh"

# 5. SWAP_PARTITIONS_BETWEEN_TABLES with a staging table: new text for the same doc_ids, and some new ones
q "INSERT INTO vf_ch.stage SELECT doc_id, p, 'swapped zebra holmes ' || body FROM vf_ch.docs WHERE p = 5;
   INSERT INTO vf_ch.stage SELECT doc_id + 100000, 5, 'swapped new zebra' FROM vf_ch.docs WHERE p = 5 AND doc_id < 300; COMMIT;" > /dev/null
q "SELECT SWAP_PARTITIONS_BETWEEN_TABLES('vf_ch.stage', 5, 5, 'vf_ch.docs'); COMMIT;" > /dev/null
compare docs vf_ch vf_ch_live "SWAP_PARTITIONS_BETWEEN_TABLES: live view at once"
q "CALL vfts.refresh_index('vf_ch')" > /dev/null
compare docs vf_ch vf_ch_snap "SWAP_PARTITIONS_BETWEEN_TABLES, after refresh"

# 6. COPY_PARTITIONS_TO_TABLE into an indexed table
q "SELECT COPY_PARTITIONS_TO_TABLE('vf_ch.stage', 5, 5, 'vf_ch.other'); COMMIT;" > /dev/null
compare other vf_ch2 vf_ch2_live "COPY_PARTITIONS_TO_TABLE: live view at once"
q "CALL vfts.refresh_index('vf_ch2')" > /dev/null
compare other vf_ch2 vf_ch2_snap "COPY_PARTITIONS_TO_TABLE, after refresh"

# 7. A journal without a version column (doc_id, op): deletes visible at once
q "CREATE TABLE vf_ch.journal (doc_id INT, op CHAR(1)); COMMIT;" > /dev/null
q "CALL vfts.drop_index('vf_ch')" > /dev/null
q "CALL vfts.create_index('vf_ch', 'vf_ch.docs', 'refresh_every=0 journal=vf_ch.journal')" > /dev/null
q "DELETE FROM vf_ch.docs WHERE doc_id BETWEEN 300 AND 349; INSERT INTO vf_ch.journal SELECT k, 'D' FROM vfts.probe WHERE k BETWEEN 300 AND 349; COMMIT;" > /dev/null
compare docs vf_ch vf_ch_live "journal (doc_id, op): delete in the live view at once"
q "CALL vfts.refresh_index('vf_ch')" > /dev/null
compare docs vf_ch vf_ch_snap "journal delete, after refresh"

# 8. Nothing changed: the generation stays
g=$(gen_of vf_ch)
q "CALL vfts.refresh_index('vf_ch')" > /dev/null
expect "a refresh without changes keeps the generation" "$(gen_of vf_ch) $(q "SELECT refresh_note FROM vfts.indexes WHERE index_name = 'vf_ch'")" "$g no changes"

# 9. TRUNCATE
q "TRUNCATE TABLE vf_ch.other" > /dev/null
q "CALL vfts.refresh_index('vf_ch2')" > /dev/null
expect "TRUNCATE, after refresh: nothing found" "$(C vf_ch2 vf_ch2_snap "contract OR holmes OR zebra OR watson")" "0"
# the merge after that refresh found segments whose every document was deleted: retired, no empty
# segment made (one could not be loaded on the other nodes: it has no rows)
expect "TRUNCATE, after refresh: no active segment left" \
       "$(q "SELECT COUNT(*) FROM vfts.segments s JOIN vfts.indexes i USING (index_id) WHERE i.index_name = 'vf_ch2' AND s.gen_removed IS NULL")" "0"
gen_docs other 5001 5050 9
compare other vf_ch2 vf_ch2_live "rows loaded after TRUNCATE, live view"

# 10. An index of vfts 0.4 (ver boundary, no epoch boundary): its first refresh goes on from the ver boundary
q "CALL vfts.drop_index('vf_ch')" > /dev/null
q "CREATE TABLE vf_ch.old (doc_id INT NOT NULL, p INT NOT NULL, body LONG VARCHAR, ver TIMESTAMPTZ NOT NULL DEFAULT CLOCK_TIMESTAMP()); COMMIT;" > /dev/null
gen_docs old 1 1000 3
q "CALL vfts.create_index('vf_ch', 'vf_ch.old', 'refresh_every=0')" > /dev/null
q "UPDATE vfts.indexes SET ver_col = 'ver', boundary_epoch = NULL, boundary_lit = 'TIMESTAMPTZ ''' || TO_CHAR(CLOCK_TIMESTAMP() AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS.US') || '+00''' WHERE index_name = 'vf_ch'; COMMIT;" > /dev/null
q "CALL vfts.make_views('vf_ch')" > /dev/null
sleep 1
q "UPDATE vf_ch.old SET body = 'holmes zebra', ver = CLOCK_TIMESTAMP() WHERE doc_id BETWEEN 1 AND 30; DELETE FROM vf_ch.old WHERE doc_id BETWEEN 31 AND 40; COMMIT;" > /dev/null
expect "an index of vfts 0.4 before its refresh: live view by its ver boundary" "$(C vf_ch vf_ch_live zebra)" "30"
q "CALL vfts.refresh_index('vf_ch')" > /dev/null
expect "an index of vfts 0.4 moves to an epoch boundary at its first refresh" \
       "$(q "SELECT (boundary_epoch IS NOT NULL AND boundary_lit IS NULL)::INT FROM vfts.indexes WHERE index_name = 'vf_ch'")" "1"
compare old vf_ch vf_ch_snap "index of vfts 0.4, first refresh"

for i in vf_ch vf_ch2 vf_ch_ref; do q "CALL vfts.drop_index('$i')" > /dev/null 2>&1; done
q "DROP SCHEMA vf_ch CASCADE" > /dev/null
done_test
