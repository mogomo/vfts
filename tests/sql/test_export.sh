#!/usr/bin/env bash
# export_index / import_index (vfts 0.5.0): an index copied into a table of the source table's
# schema and made again from it, as after a restore of that schema from a backup:
#   - imported without changes: every search equals the index before (ids, ranks, scores), the
#     attributes too
#   - the table changed after the export (rows deleted and added, as a restore of an older copy
#     gives): after the import (which ends with a refresh) the searches equal a fresh index
# Creates schema vf_ex; drops it at the end.
set -uo pipefail
cd "$(dirname "$0")"; source ./lib.sh
echo "$TEST_NAME"

VOCAB="contract indemnity holmes watson agreement breach party clause notice term liability damages court settlement payment"
cleanup() {
    for i in vf_ex vf_ex_ref; do q "CALL vfts.drop_index('$i')" > /dev/null 2>&1; done
    q "DROP SCHEMA IF EXISTS vf_ex CASCADE" > /dev/null 2>&1
}
cleanup
q "CREATE SCHEMA vf_ex;
   CREATE TABLE vf_ex.docs (doc_id INT NOT NULL, body LONG VARCHAR);
   CREATE TABLE vf_ex.meta (doc_id INT, owner VARCHAR(20));" > /dev/null
gen_docs() {   # first last seed
    q "INSERT INTO vf_ex.docs (doc_id, body)
       SELECT doc_id, LISTAGG(w USING PARAMETERS separator=' ', max_length=8000)
       FROM (SELECT d.k AS doc_id, SPLIT_PART('$VOCAB', ' ', (1 + MOD(HASH(d.k, p.k, $3) // 5, 15))::INT) AS w
             FROM (SELECT a.k + 1024 * b.j AS k FROM vfts.probe a CROSS JOIN (SELECT 0 AS j UNION ALL SELECT 1) b) d
             JOIN vfts.probe p ON p.k <= 4 + MOD(HASH(d.k, $3) // 13, 40)
             WHERE d.k BETWEEN $1 AND $2) x
       GROUP BY doc_id; COMMIT;" > /dev/null
}
gen_docs 1 1500 5
q "INSERT INTO vf_ex.meta SELECT doc_id, CASE MOD(doc_id, 3) WHEN 0 THEN 'legal' WHEN 1 THEN 'sales' ELSE 'it' END FROM vf_ex.docs; COMMIT;" > /dev/null
q "CALL vfts.create_index('vf_ex', 'vf_ex.docs', 'refresh_every=0')" > /dev/null
# a second segment and tombstones, and an attribute
q "UPDATE vf_ex.docs SET body = body || ' holmes' WHERE MOD(doc_id, 7) = 0; DELETE FROM vf_ex.docs WHERE MOD(doc_id, 11) = 0; COMMIT;" > /dev/null
q "CALL vfts.refresh_index('vf_ex')" > /dev/null
q "CALL vfts.add_attribute('vf_ex', 'owner', 'vf_ex.meta', 'doc_id', 'owner')" > /dev/null

QUERIES=("holmes" "contract indemnity" "watson AND breach" "\"contract indemnity\"" "contract w/3 indemnity" "agree*")
R() { q "SELECT doc_id || ':' || rank || ':' || ROUND(score, 6) FROM (SELECT vfts.tsearch(* USING PARAMETERS index='$1', query='$2', k=30) OVER() FROM vf_ex.$1_live) r ORDER BY rank"; }
B() { q "SELECT value || ':' || count FROM (SELECT vfts.tcount(* USING PARAMETERS index='$1', query='contract', by='owner') OVER() FROM vf_ex.$1_live) r ORDER BY 1" | tr '\n' ' '; }
snapshot() { local qq; for qq in "${QUERIES[@]}"; do R "$1" "$qq"; done; B "$1"; }

before=$(snapshot vf_ex)
q "CALL vfts.export_index('vf_ex')" > /dev/null
expect "export: a table in the source table's schema" \
       "$(q "SELECT COUNT(*) FROM v_catalog.tables WHERE table_schema = 'vf_ex' AND table_name = 'vf_ex_vfts_export'")" "1"
expect "export: segments and tombstones" \
       "$(q "SELECT (SUM(CASE WHEN kind = 'S' THEN 1 ELSE 0 END) >= 2 AND SUM(CASE WHEN kind = 'X' THEN 1 ELSE 0 END) > 0)::INT FROM vf_ex.vf_ex_vfts_export")" "1"

q "CALL vfts.drop_index('vf_ex')" > /dev/null
q "CALL vfts.import_index('vf_ex', 'vf_ex.vf_ex_vfts_export')" > /dev/null
expect "import of an unchanged table: every search and count by attribute as before" "$(snapshot vf_ex)" "$before"
expect "the imported index has its own key" "$(q "SELECT COUNT(*) FROM vfts.index_keys k JOIN vfts.indexes i USING (index_id) WHERE i.index_name = 'vf_ex'")" "1"

# the table moves on after the export (rows gone, rows added); then the index is made from the export again
q "CALL vfts.export_index('vf_ex')" > /dev/null
q "DELETE FROM vf_ex.docs WHERE MOD(doc_id, 5) = 0; COMMIT;" > /dev/null
gen_docs 1501 1700 9
q "CALL vfts.drop_index('vf_ex')" > /dev/null
q "CALL vfts.import_index('vf_ex', 'vf_ex.vf_ex_vfts_export')" > /dev/null
q "DROP TABLE IF EXISTS vf_ex.ref; CREATE TABLE vf_ex.ref AS SELECT doc_id, body FROM vf_ex.docs; COMMIT;" > /dev/null
q "CALL vfts.create_index('vf_ex_ref', 'vf_ex.ref', 'refresh_every=0')" > /dev/null
bad=0
for qq in "${QUERIES[@]}"; do
    if [ "$(R vf_ex "$qq")" != "$(q "SELECT doc_id || ':' || rank || ':' || ROUND(score, 6) FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_ex_ref', query='$qq', k=30) OVER() FROM vf_ex.vf_ex_ref_live) r ORDER BY rank")" ]; then
        bad=$((bad + 1)); echo "       differs: $qq"
    fi
done
expect "import after the table changed: the refresh brings it to the table (${#QUERIES[@]} queries)" "$bad" "0"
expect "the refresh after the import found the rows that came and went" \
       "$(q "SELECT refresh_note FROM vfts.indexes WHERE index_name = 'vf_ex'" | grep -o 'documents left the source')" "documents left the source"
expect_error "import refuses an existing index" "CALL vfts.import_index('vf_ex', 'vf_ex.vf_ex_vfts_export')" "exists"
expect_error "import refuses a table that is not an export" "CALL vfts.import_index('vf_ex_x', 'vf_ex.meta')" "not an export"

cleanup
done_test
