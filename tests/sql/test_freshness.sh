#!/usr/bin/env bash
# Freshness (correctness rule 3): insert, update and delete rows, then search without refreshing;
# the results (ids, ranks and scores) equal those of an index built from scratch on the current
# rows. Then the same after an incremental refresh, a merge and a rebuild.
set -uo pipefail
cd "$(dirname "$0")"; source ./lib.sh
echo "$TEST_NAME"

VOCAB="contract indemnity holmes watson agreement breach party clause notice term liability damages court settlement payment"
for i in vf_fr vf_fr_ref; do q "CALL vfts.drop_index('$i')" > /dev/null 2>&1; done
q "DROP SCHEMA IF EXISTS vf_fr CASCADE" > /dev/null
q "CREATE SCHEMA vf_fr;
   CREATE TABLE vf_fr.docs (doc_id INT NOT NULL, body LONG VARCHAR, lang VARCHAR(8), ver TIMESTAMPTZ DEFAULT CLOCK_TIMESTAMP());
   CREATE TABLE vf_fr.docs_journal (doc_id INT, op CHAR(1), ver TIMESTAMPTZ DEFAULT CLOCK_TIMESTAMP());" > /dev/null
gen_docs() {   # first last seed -> INSERT of documents doc_id first..last
    q "INSERT INTO vf_fr.docs (doc_id, body)
       SELECT doc_id, LISTAGG(w USING PARAMETERS separator=' ', max_length=8000)
       FROM (SELECT d.k AS doc_id, SPLIT_PART('$VOCAB', ' ', (1 + MOD(HASH(d.k, p.k, $3) // 5, 15))::INT) AS w
             FROM (SELECT a.k + 1024 * b.j AS k FROM vfts.probe a CROSS JOIN (SELECT 0 AS j UNION ALL SELECT 1 UNION ALL SELECT 2) b) d
             JOIN vfts.probe p ON p.k <= 4 + MOD(HASH(d.k, $3) // 13, 40)
             WHERE d.k BETWEEN $1 AND $2) x
       GROUP BY doc_id; COMMIT;" > /dev/null
}
gen_docs 1 2000 1
q "CALL vfts.create_index('vf_fr', 'vf_fr.docs', 'refresh_every=0 journal=vf_fr.docs_journal margin=0')" > /dev/null

# Changes after the index was built: 100 new rows, 60 updated (ver bumped), 20 updated with only a
# journal row (ver unchanged), 50 deleted, and 5 rows updated twice.
sleep 1
gen_docs 2001 2100 2
q "UPDATE vf_fr.docs SET body = body || ' settlement holmes', ver = CLOCK_TIMESTAMP() WHERE doc_id BETWEEN 1 AND 60;
   INSERT INTO vf_fr.docs_journal (doc_id, op) SELECT doc_id, 'U' FROM vf_fr.docs WHERE doc_id BETWEEN 1 AND 60;
   UPDATE vf_fr.docs SET body = 'watson watson breach' WHERE doc_id BETWEEN 101 AND 120;
   INSERT INTO vf_fr.docs_journal (doc_id, op) SELECT doc_id, 'U' FROM vf_fr.docs WHERE doc_id BETWEEN 101 AND 120;
   DELETE FROM vf_fr.docs WHERE doc_id BETWEEN 201 AND 250;
   INSERT INTO vf_fr.docs_journal (doc_id, op) SELECT k, 'D' FROM vfts.probe WHERE k BETWEEN 201 AND 250;
   UPDATE vf_fr.docs SET body = 'contract contract indemnity', ver = CLOCK_TIMESTAMP() WHERE doc_id BETWEEN 1 AND 5;
   COMMIT;" > /dev/null

# Reference: an index built from scratch on a copy of the current rows.
q "CREATE TABLE vf_fr.ref AS SELECT doc_id, body, lang FROM vf_fr.docs; COMMIT;" > /dev/null
q "CALL vfts.create_index('vf_fr_ref', 'vf_fr.ref', 'refresh_every=0')" > /dev/null

QUERIES=("holmes" "contract indemnity" "watson AND breach" "settlement OR holmes -court" "\"contract indemnity\"" "contract w/3 indemnity" "agree*" "NOT contract")
R() {   # index view query -> doc_id:rank:score lines
    q "SELECT doc_id || ':' || rank || ':' || ROUND(score, 6) FROM (SELECT vfts.tsearch(* USING PARAMETERS index='$1', query='$3', k=30) OVER() FROM vf_fr.$2) r ORDER BY rank"
}
compare_all() {   # label
    local ok=0 bad=0
    for qq in "${QUERIES[@]}"; do
        local a b
        a=$(R vf_fr vf_fr_live "$qq")
        b=$(R vf_fr_ref vf_fr_ref_live "$qq")
        if [ "$a" == "$b" ] && [ -n "$a" ]; then ok=$((ok + 1)); else bad=$((bad + 1)); echo "       differs: $qq"; fi
    done
    expect "$1: ${#QUERIES[@]} queries equal the fresh index" "$bad" "0"
    expect "$1: counts equal" "$(q "SELECT count FROM (SELECT vfts.tcount(* USING PARAMETERS index='vf_fr', query='contract') OVER() FROM vf_fr.vf_fr_live) r")" \
                              "$(q "SELECT count FROM (SELECT vfts.tcount(* USING PARAMETERS index='vf_fr_ref', query='contract') OVER() FROM vf_fr.vf_fr_ref_live) r")"
}

expect "the live tail holds the changes" "$(q "SELECT (COUNT(*) >= 230)::INT FROM vf_fr.vf_fr_live WHERE doc_id IS NOT NULL")" "1"
compare_all "without refresh"

q "CALL vfts.refresh_index('vf_fr')" > /dev/null
expect "after refresh the tail is empty" "$(q "SELECT COUNT(*) FROM vf_fr.vf_fr_live WHERE doc_id IS NOT NULL")" "0"
expect "tombstones for the 130 changed or deleted rows (60 + 20 updated, 50 deleted)" \
       "$(q "SELECT COUNT(*) FROM vfts.tombstones t JOIN vfts.indexes i USING (index_id) WHERE i.index_name = 'vf_fr'")" "130"
compare_all "after an incremental refresh"

# more changes on top of the refreshed index, then searched without refresh again
sleep 1
q "UPDATE vf_fr.docs SET body = 'holmes holmes holmes', ver = CLOCK_TIMESTAMP() WHERE doc_id BETWEEN 2001 AND 2010;
   DELETE FROM vf_fr.docs WHERE doc_id BETWEEN 300 AND 309;
   INSERT INTO vf_fr.docs_journal (doc_id, op) SELECT k, 'D' FROM vfts.probe WHERE k BETWEEN 300 AND 309; COMMIT;" > /dev/null
q "CALL vfts.drop_index('vf_fr_ref'); DROP TABLE vf_fr.ref; CREATE TABLE vf_fr.ref AS SELECT doc_id, body, lang FROM vf_fr.docs; COMMIT;" > /dev/null
q "CALL vfts.create_index('vf_fr_ref', 'vf_fr.ref', 'refresh_every=0')" > /dev/null
compare_all "second round without refresh"
q "CALL vfts.refresh_index('vf_fr')" > /dev/null
compare_all "second refresh"
q "CALL vfts.merge('vf_fr', 1)" > /dev/null
expect "merge into one segment" \
       "$(q "SELECT COUNT(*) FROM vfts.segments s JOIN vfts.indexes i USING (index_id) WHERE i.index_name = 'vf_fr' AND s.gen_removed IS NULL")" "1"
expect "merged-away segments leave no rows in the index tables" \
       "$(q "SELECT (SELECT COUNT(*) FROM vfts.postings p JOIN vfts.segments s USING (index_id, segment_id) WHERE s.gen_removed IS NOT NULL AND s.index_id = (SELECT index_id FROM vfts.indexes WHERE index_name = 'vf_fr'))
                  + (SELECT COUNT(*) FROM vfts.positions p JOIN vfts.segments s USING (index_id, segment_id) WHERE s.gen_removed IS NOT NULL AND s.index_id = (SELECT index_id FROM vfts.indexes WHERE index_name = 'vf_fr'))
                  + (SELECT COUNT(*) FROM vfts.term_dict p JOIN vfts.segments s USING (index_id, segment_id) WHERE s.gen_removed IS NOT NULL AND s.index_id = (SELECT index_id FROM vfts.indexes WHERE index_name = 'vf_fr'))
                  + (SELECT COUNT(*) FROM vfts.doc_stats p JOIN vfts.segments s USING (index_id, segment_id) WHERE s.gen_removed IS NOT NULL AND s.index_id = (SELECT index_id FROM vfts.indexes WHERE index_name = 'vf_fr'))
                  + (SELECT COUNT(*) FROM vfts.tombstones p JOIN vfts.segments s USING (index_id, segment_id) WHERE s.gen_removed IS NOT NULL AND s.index_id = (SELECT index_id FROM vfts.indexes WHERE index_name = 'vf_fr'))")" "0"
compare_all "after merge (deleted documents dropped)"
# tiered policy: ten small refreshes make ten small segments; the tenth refresh merges them
for r in 1 2 3 4 5 6 7 8 9 10; do
    sleep 0.2
    q "UPDATE vf_fr.docs SET body = body || ' notice', ver = CLOCK_TIMESTAMP() WHERE doc_id BETWEEN $((r * 10 + 400)) AND $((r * 10 + 405)); COMMIT;" > /dev/null
    q "CALL vfts.refresh_index('vf_fr')" > /dev/null
done
expect "tiered merge keeps the segment count below merge_factor" \
       "$(q "SELECT (COUNT(*) < 10)::INT FROM vfts.segments s JOIN vfts.indexes i USING (index_id) WHERE i.index_name = 'vf_fr' AND s.gen_removed IS NULL")" "1"
q "CALL vfts.drop_index('vf_fr_ref'); DROP TABLE vf_fr.ref; CREATE TABLE vf_fr.ref AS SELECT doc_id, body, lang FROM vf_fr.docs; COMMIT;" > /dev/null
q "CALL vfts.create_index('vf_fr_ref', 'vf_fr.ref', 'refresh_every=0')" > /dev/null
compare_all "after tiered merges"
g_before=$(q "SELECT gen FROM vfts.indexes WHERE index_name = 'vf_fr'")
q "CALL vfts.rebuild('vf_fr')" > /dev/null
compare_all "after rebuild"
expect "rebuild leaves no segment from before it" \
       "$(q "SELECT COUNT(*) FROM vfts.segments s JOIN vfts.indexes i USING (index_id) WHERE i.index_name = 'vf_fr' AND s.gen_removed IS NULL AND s.gen_added <= $g_before")" "0"
q "CALL vfts.refresh_index('vf_fr')" > /dev/null
expect "a refresh without changes keeps the generation" \
       "$(q "SELECT refresh_note FROM vfts.indexes WHERE index_name = 'vf_fr'")" "no changes"

done_test
