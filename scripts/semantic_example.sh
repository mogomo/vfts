#!/usr/bin/env bash
# The "Semantic search" recipe of README.md, run end to end on a small table of its own
# (schema sem, index kb_sem): words with vfts, meaning with vectors, both fused by vfts.hybrid.
# Prints each step and its output, as in the README. Safe to run again.
#
#   scripts/semantic_example.sh            run the recipe
#   scripts/semantic_example.sh --clean    remove the schema and the index
#
# Needs: vfts deployed, the vsql connection variables, and a Python with sentence-transformers
# (EMBED_PY, default python3; the model is downloaded once, about 470 MB). An existing schema sem
# that this script did not make is never touched: the script stops. Exit status: failed statements.
set -uo pipefail
cd "$(dirname "$0")/.."
PY=${EMBED_PY:-python3}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
q1() { vsql -X -A -t -q -c "$1" 2> /dev/null; }
mine() { [ "$(q1 "SELECT COUNT(*) FROM v_catalog.tables WHERE table_schema = 'sem' AND table_name = 'semantic_example_marker'")" = 1 ]; }
clean() {
    if [ "$(q1 "SELECT COUNT(*) FROM v_catalog.schemata WHERE schema_name = 'sem'")" = 1 ] && ! mine; then
        echo "semantic_example.sh: schema sem exists and was not made by this script: not touched" >&2; exit 3
    fi
    [ -n "$(q1 "SELECT index_name FROM vfts.indexes WHERE index_name = 'kb_sem'")" ] && vsql -X -q -c "CALL vfts.drop_index('kb_sem')" > /dev/null 2>&1
    vsql -X -q -c "DROP SCHEMA IF EXISTS sem CASCADE" > /dev/null 2>&1
    return 0
}
if [ "${1:-}" = --clean ]; then clean; echo "semantic example removed"; exit 0; fi
FAILS=0
ex() {   # title, then statements: each is printed with its output; a failed one is counted
    echo; echo "-- $1"; shift
    for s in "$@"; do
        echo "$s"
        local out
        if ! out=$(vsql -X -v ON_ERROR_STOP=1 -c "$s" 2>&1); then FAILS=$((FAILS + 1)); fi
        echo "$out" | sed 's/^/   /'
    done
}
sh_() {   # title, a shell command: printed, run, its output shown
    echo; echo "-- $1"; echo "$2"
    local out
    if ! out=$(bash -c "set -o pipefail; $2" 2>&1); then FAILS=$((FAILS + 1)); fi
    echo "$out" | sed 's/^/   /'
}
clean

ex "a table with text, and the index of its words" \
"CREATE SCHEMA sem;
CREATE TABLE sem.semantic_example_marker (x INT);
CREATE TABLE sem.kb (doc_id INT NOT NULL, body LONG VARCHAR, lang VARCHAR(8),
                     ver TIMESTAMPTZ NOT NULL DEFAULT CLOCK_TIMESTAMP());
INSERT INTO sem.kb (doc_id, body) VALUES (1, 'The supplier shall indemnify the buyer under this contract.');
INSERT INTO sem.kb (doc_id, body) VALUES (2, 'The vendor must compensate the purchaser for any losses.');
INSERT INTO sem.kb (doc_id, body) VALUES (3, 'Contract renewal: payment within 30 days.');
INSERT INTO sem.kb (doc_id, body) VALUES (4, 'Meeting notes: mark to market accounting for the energy contracts.');
INSERT INTO sem.kb (doc_id, body) VALUES (5, 'Holmes'' notes on the Baskerville case.');
INSERT INTO sem.kb (doc_id, body, lang) VALUES (6, 'הספק ישפה את הקונה על כל נזק.', 'he');
COMMIT;" \
"CALL vfts.create_index('kb_sem', 'sem.kb', 'lang=en');"

ex "a table for the vectors (384 dimensions: the model below), and one for the questions" \
"CREATE TABLE sem.kb_vec (doc_id INT NOT NULL, vec ARRAY[FLOAT, 384]);
CREATE TABLE sem.questions (qid INT NOT NULL, question VARCHAR(500), qvec ARRAY[FLOAT, 384]);
INSERT INTO sem.questions (qid, question) VALUES (1, 'damage compensation');
COMMIT;"

sh_ "embed the documents outside Vertica, then load the vectors" \
"vsql -X -At -F \$'\\t' -c \"SELECT doc_id, REGEXP_REPLACE(body, '\\\\s+', ' ') FROM sem.kb\" | $PY scripts/embed.py > $TMP/kb_vec.txt
vsql -X -c \"COPY sem.kb_vec FROM LOCAL '$TMP/kb_vec.txt' DELIMITER '|' ABORT ON ERROR;\""

sh_ "embed the question with the same model" \
"vsql -X -At -F \$'\\t' -c \"SELECT qid, question FROM sem.questions\" | $PY scripts/embed.py > $TMP/q.txt
vsql -X -c \"CREATE LOCAL TEMP TABLE qv (qid INT, qvec ARRAY[FLOAT, 384]) ON COMMIT PRESERVE ROWS;
            COPY qv FROM LOCAL '$TMP/q.txt' DELIMITER '|' ABORT ON ERROR;
            UPDATE sem.questions q SET qvec = qv.qvec FROM qv WHERE q.qid = qv.qid; COMMIT;\""

ex "words only: tsearch finds the one note that shares a word with the question" \
"SELECT r.rank, r.doc_id, ROUND(r.score, 4) AS score, LEFT(k.body::VARCHAR(500), 60) AS body
FROM (SELECT vfts.tsearch(* USING PARAMETERS index='kb_sem', query='damage compensation', syntax='plain', k=10) OVER()
      FROM sem.kb_sem_live) r JOIN sem.kb k USING (doc_id)
ORDER BY r.rank;"

ex "meaning only: cosine similarity to the question (exact, every vector compared)" \
"SELECT ROW_NUMBER() OVER (ORDER BY s.sim DESC) AS rank, s.doc_id, ROUND(s.sim, 4) AS similarity, LEFT(k.body::VARCHAR(500), 60) AS body
FROM (SELECT v.doc_id, COSINE_SIMILARITY(v.vec, q.qvec) AS sim
      FROM sem.kb_vec v CROSS JOIN sem.questions q WHERE q.qid = 1) s JOIN sem.kb k USING (doc_id)
ORDER BY rank;"

ex "both, fused by rank: vfts.hybrid" \
"SELECT h.rank, h.doc_id, ROUND(h.score, 5) AS score, h.sources, LEFT(k.body::VARCHAR(500), 60) AS body
FROM (SELECT vfts.hybrid(src, doc_id, rank, score USING PARAMETERS k=3) OVER() FROM (
        SELECT 'text' AS src, doc_id, rank, score
        FROM (SELECT vfts.tsearch(* USING PARAMETERS index='kb_sem', query='damage compensation', syntax='plain', k=50) OVER()
              FROM sem.kb_sem_live) t
        UNION ALL
        SELECT 'vector', doc_id, ROW_NUMBER() OVER (ORDER BY sim DESC), sim
        FROM (SELECT v.doc_id, COSINE_SIMILARITY(v.vec, q.qvec) AS sim
              FROM sem.kb_vec v CROSS JOIN sem.questions q WHERE q.qid = 1
              ORDER BY sim DESC LIMIT 50) s) u) h
     JOIN sem.kb k USING (doc_id)
ORDER BY h.rank;"

echo; echo "-- remove the example: scripts/semantic_example.sh --clean"
exit $FAILS
