#!/usr/bin/env bash
# vtext demo: every function on a small table, with explanations. Safe to run again.
#
#   scripts/demo.sh            run the whole demo
#   scripts/demo.sh --pause    wait for Enter between the steps (for a presentation)
#   scripts/demo.sh --clean    remove the demo schema and index
#
# Needs: vtext deployed (make deploy); vsql connection variables (VSQL_HOST, VSQL_PORT, VSQL_USER,
# VSQL_PASSWORD, VSQL_DATABASE). With vvector installed, the hybrid step uses real vector search.
set -uo pipefail
PAUSE=no
[ "${1:-}" = --pause ] && PAUSE=yes
if [ "${1:-}" = --clean ]; then
    vsql -X -q -c "CALL vtext.drop_index('cases')" > /dev/null 2>&1
    vsql -X -q -c "CALL vvector.unregister_index('cases_vec')" > /dev/null 2>&1
    vsql -X -q -c "DROP SCHEMA IF EXISTS demo CASCADE" > /dev/null 2>&1
    echo "demo removed"; exit 0
fi

B=$(tput bold 2>/dev/null || true); N=$(tput sgr0 2>/dev/null || true)
step() { echo; echo "${B}== $1${N}"; shift; for l in "$@"; do echo "   $l"; done; [ "$PAUSE" = yes ] && read -r -p "   (Enter) " _; }
sql() { echo "${B}SQL>${N} $1"; vsql -X -c "$1" 2>&1 | grep -v "^$"; }

step "0. Start clean" "The demo uses the schema demo and the index cases."
vsql -X -q -c "CALL vtext.drop_index('cases')" > /dev/null 2>&1
vsql -X -q -c "CALL vvector.unregister_index('cases_vec')" > /dev/null 2>&1
vsql -X -q -c "DROP SCHEMA IF EXISTS demo CASCADE" > /dev/null 2>&1

step "1. A source table" \
     "The table contract: doc_id INT, body LONG VARCHAR, lang (ISO 639-1, NULL = index language)," \
     "ver TIMESTAMPTZ DEFAULT CLOCK_TIMESTAMP(). A journal records updates and deletes."
sql "CREATE SCHEMA demo;
CREATE TABLE demo.cases (doc_id INT NOT NULL, body LONG VARCHAR, lang VARCHAR(8), vec ARRAY[FLOAT],
                         del BOOLEAN NOT NULL DEFAULT FALSE, ver TIMESTAMPTZ NOT NULL DEFAULT CLOCK_TIMESTAMP());
CREATE TABLE demo.cases_journal (doc_id INT, op CHAR(1), ver TIMESTAMPTZ DEFAULT CLOCK_TIMESTAMP());"
sql "INSERT INTO demo.cases (doc_id, body, lang, vec) VALUES (1, 'The indemnity clause of the supply contract protects the buyer against third-party claims.', 'en', ARRAY[0.9, 0.1, 0.0]);
INSERT INTO demo.cases (doc_id, body, lang, vec) VALUES (2, 'Contract terms: the seller shall indemnify the buyer within 30 days.', 'en', ARRAY[0.8, 0.2, 0.0]);
INSERT INTO demo.cases (doc_id, body, lang, vec) VALUES (3, 'Sherlock Holmes'' notes on the Baskerville case, Holmes-style deduction.', 'en', ARRAY[0.0, 1.0, 0.0]);
INSERT INTO demo.cases (doc_id, body, lang, vec) VALUES (4, 'Running the payment system: late payments run up interest.', 'en', ARRAY[0.2, 0.0, 0.9]);
INSERT INTO demo.cases (doc_id, body, lang, vec) VALUES (5, 'Les contrats signés prévoient une indemnité.', 'fr', ARRAY[0.7, 0.1, 0.2]);
INSERT INTO demo.cases (doc_id, body, lang, vec) VALUES (6, 'החוזה נחתם בבית המשפט; דובר צה״ל אישר.', 'he', ARRAY[0.5, 0.3, 0.2]);
INSERT INTO demo.cases (doc_id, body, lang, vec) VALUES (7, 'وقّع الطرفان العقد في المحكمة، والكتاب في المكتبة.', 'ar', ARRAY[0.4, 0.2, 0.4]);
INSERT INTO demo.cases (doc_id, body, lang, vec) VALUES (8, 'Indemnification is not covered by this agreement.', 'en', ARRAY[0.6, 0.0, 0.4]);
COMMIT;"

step "2. What the analyzer makes of a text: vtext.tokenize" \
     "ICU word boundaries split Holmes' and Holmes-style; terms are case folded and accent folded;" \
     "the stem is shown but not indexed."
sql "SELECT doc_id, pos, term, stem, lang FROM (SELECT doc_id, vtext.tokenize(body, lang) FROM demo.cases WHERE doc_id IN (3, 6)) t ORDER BY doc_id, pos;"

step "3. Create the index: vtext.create_index" \
     "Builds the index from every row and loads it on every node. The views demo.cases_live and" \
     "demo.cases_snap are the input of the search functions. margin=0 is fine on one node."
sql "CALL vtext.create_index('cases', 'demo.cases', 'lang=en journal=demo.cases_journal margin=0');"
sql "CALL vtext.info('cases');"

step "4. Ranked search: vtext.tsearch (BM25)" \
     "mode='stem' (the default) finds contract, contracts; Holmes', Holmes-style are found."
sql "SELECT * FROM (SELECT vtext.tsearch(* USING PARAMETERS index='cases', query='contract indemnity', k=5) OVER() FROM demo.cases_live) r ORDER BY rank;"
sql "SELECT r.rank, r.doc_id, c.body FROM (SELECT vtext.tsearch(* USING PARAMETERS index='cases', query='holmes', k=5) OVER() FROM demo.cases_live) r JOIN demo.cases c USING (doc_id) ORDER BY r.rank;"

step "5. The query language" "Boolean, phrase, proximity (w/N any order, pre/N in order), prefix, wildcard, fuzzy."
for q in 'contract AND NOT indemnify' '"indemnity clause"' 'contract w/5 indemnity' 'seller pre/3 indemnify' 'indemn*' 'contr?ct' 'holmse~1' '"contract supply"~2'; do
    sql "SELECT '$(echo "$q" | sed "s/'/''/g")' AS query, LISTAGG(doc_id::VARCHAR USING PARAMETERS separator=' ') AS doc_ids FROM (SELECT doc_id FROM (SELECT vtext.tsearch(* USING PARAMETERS index='cases', query='$(echo "$q" | sed "s/'/''/g")', k=10) OVER() FROM demo.cases_live) r ORDER BY doc_id) x;"
done

step "6. Stem or exact: mode" "contracts in mode stem finds contract (documents 1 and 2); in mode exact only the word" "contracts itself (no document has it)."
sql "SELECT 'stem' AS mode, LISTAGG(doc_id::VARCHAR USING PARAMETERS separator=' ') AS doc_ids FROM (SELECT doc_id FROM (SELECT vtext.tsearch(* USING PARAMETERS index='cases', query='contracts', k=10) OVER() FROM demo.cases_live) r ORDER BY doc_id) x
     UNION ALL SELECT 'exact', LISTAGG(doc_id::VARCHAR USING PARAMETERS separator=' ') FROM (SELECT doc_id FROM (SELECT vtext.tsearch(* USING PARAMETERS index='cases', query='contracts', k=10, mode='exact') OVER() FROM demo.cases_live) r ORDER BY doc_id) x;"

step "7. Other languages" "French with lang='fr'; Hebrew prefixes and acronyms; Arabic article and diacritics."
sql "SELECT doc_id FROM (SELECT vtext.tsearch(* USING PARAMETERS index='cases', query='contrat', lang='fr', k=5) OVER() FROM demo.cases_live) r;"
sql "SELECT doc_id FROM (SELECT vtext.tsearch(* USING PARAMETERS index='cases', query='בית', lang='he', k=5) OVER() FROM demo.cases_live) r;"
sql "SELECT doc_id FROM (SELECT vtext.tsearch(* USING PARAMETERS index='cases', query='צה״ל', k=5) OVER() FROM demo.cases_live) r;"
sql "SELECT doc_id FROM (SELECT vtext.tsearch(* USING PARAMETERS index='cases', query='كتاب', lang='ar', k=5) OVER() FROM demo.cases_live) r;"

step "8. Counting, batches, score scale" "tcount: the number of matching documents, without scoring."
sql "SELECT * FROM (SELECT vtext.tcount(* USING PARAMETERS index='cases', query='contract OR indemn*') OVER() FROM demo.cases_live) r;"
sql "SELECT qid, rank, doc_id FROM (SELECT vtext.tsearch(* USING PARAMETERS index='cases', k=2,
     queries=E'1\tindemnity\n2\tholmes\n3\tcontract w/5 indemnity') OVER() FROM demo.cases_live) r ORDER BY qid, rank;"
echo "   (above: three queries in one statement, the parameter queries; a batch pays Vertica's statement cost once)"
sql "SELECT a.doc_id, a.score AS opensearch_scale, c.score AS elasticsearch_scale FROM
     (SELECT vtext.tsearch(* USING PARAMETERS index='cases', query='indemnity', k=1) OVER() FROM demo.cases_live) a
     JOIN (SELECT vtext.tsearch(* USING PARAMETERS index='cases', query='indemnity', k=1, bm25='classic') OVER() FROM demo.cases_live) c ON a.doc_id = c.doc_id;"
echo "   (bm25='classic' gives the scores of Elasticsearch: k1 + 1 = 2.2 times those of Lucene and OpenSearch)"

step "9. Snippets: vtext.tsnippet" "The matching words marked in the original text."
sql "SELECT r.rank, vtext.tsnippet(c.body, 'contract w/5 indemnity' USING PARAMETERS index='cases') AS snippet
     FROM (SELECT vtext.tsearch(* USING PARAMETERS index='cases', query='contract w/5 indemnity', k=3) OVER() FROM demo.cases_live) r
     JOIN demo.cases c USING (doc_id) ORDER BY r.rank;"

step "10. No rebuild after new loads" \
     "New, updated and deleted rows are searchable at once: the live tail is analyzed at query time." \
     "A delete or an update that keeps ver is written to the journal."
sql "INSERT INTO demo.cases (doc_id, body, lang, vec) VALUES (9, 'New contract with an indemnity cap.', 'en', ARRAY[0.85, 0.15, 0.0]);
UPDATE demo.cases SET body = 'Payment terms only.', ver = CLOCK_TIMESTAMP() WHERE doc_id = 2;
DELETE FROM demo.cases WHERE doc_id = 8; INSERT INTO demo.cases_journal (doc_id, op) VALUES (8, 'D');
COMMIT;"
sql "SELECT doc_id, op FROM demo.cases_live WHERE doc_id IS NOT NULL ORDER BY doc_id;"
sql "SELECT * FROM (SELECT vtext.tsearch(* USING PARAMETERS index='cases', query='indemn*', k=10) OVER() FROM demo.cases_live) r ORDER BY rank;"
step "11. Refresh and merge" "refresh_index makes the tail a new segment (incremental); merge combines segments."
sql "CALL vtext.refresh_index('cases');"
sql "CALL vtext.merge('cases', 1);"
sql "SELECT segment_id, state, n_docs, gen_added, gen_removed FROM vtext.segments s JOIN vtext.indexes i USING (index_id) WHERE i.index_name = 'cases' ORDER BY segment_id;"
sql "SELECT * FROM (SELECT vtext.tsearch(* USING PARAMETERS index='cases', query='indemn*', k=10) OVER() FROM demo.cases_live) r ORDER BY rank;"

step "12. Hybrid search: BM25 + vectors (vtext.hybrid)" "Reciprocal Rank Fusion of tsearch and vvector's vsearch by doc_id."
if [ "$(vsql -X -At -c "SELECT COUNT(*) FROM v_catalog.schemata WHERE schema_name = 'vvector'")" = "1" ]; then
    vsql -X -q -c "CALL vvector.register_index('cases_vec', 'demo.cases', 'doc_id', 'vec', 'del', 'ver', 'cosine', 0, 'flat');" > /dev/null 2>&1
    vsql -X -q -c "CALL vvector.refresh_index('cases_vec');" > /dev/null 2>&1
    sql "SELECT h.rank, h.doc_id, h.score, h.sources FROM (SELECT vtext.hybrid(src, doc_id, rank, score USING PARAMETERS k=5) OVER() FROM (
       SELECT 'text' AS src, doc_id, rank, score FROM (SELECT vtext.tsearch(* USING PARAMETERS index='cases', query='indemnity contract', k=10) OVER() FROM demo.cases_live) t
       UNION ALL
       SELECT 'vector', id, rank, score FROM (SELECT vvector.vsearch(qid, qvec, id, vec, del, ver, snapshot_id USING PARAMETERS index_name='cases_vec', query='[0.9, 0.1, 0.1]', k=10) OVER() FROM demo.cases_vec_snap) v
     ) u) h ORDER BY h.rank;"
else
    echo "   (vvector is not installed: the same fusion over two fixed lists)"
    sql "SELECT * FROM (SELECT vtext.hybrid(src, doc_id, rank, score USING PARAMETERS k=5) OVER() FROM (
       SELECT 'text' AS src, 1 AS doc_id, 1 AS rank, 2.5 AS score UNION ALL SELECT 'text', 9, 2, 2.1
       UNION ALL SELECT 'vector', 9, 1, 0.98 UNION ALL SELECT 'vector', 5, 2, 0.91) u) h ORDER BY rank;"
fi

step "13. Done" "Remove the demo with: scripts/demo.sh --clean"
