#!/usr/bin/env bash
# Every example of README.md "Reference", run on a small table of its own (schema ex, index notes).
# Prints each statement and its output, as in the README. Safe to run again.
#
#   scripts/examples.sh            run all examples
#   scripts/examples.sh --clean    remove the schema and the index
#
# Needs: vtext deployed (make deploy) and the vsql connection variables. The tserve example also
# needs build/tools/vtext_broker (make tools) and curl.
set -uo pipefail
cd "$(dirname "$0")/.."
clean() {
    vsql -X -q -c "CALL vtext.drop_index('notes')" > /dev/null 2>&1
    vsql -X -q -c "DROP SCHEMA IF EXISTS ex CASCADE" > /dev/null 2>&1
}
if [ "${1:-}" = --clean ]; then clean; echo "examples removed"; exit 0; fi
ex() { echo; echo "-- $1"; shift; for s in "$@"; do echo "$s"; vsql -X -c "$s" 2>&1 | grep -v '^$' | sed 's/^/   /'; done; }
clean

ex "A source table: doc_id, body, lang, ver (README: Prepare a table)" \
"CREATE SCHEMA ex;
CREATE TABLE ex.notes (doc_id INT NOT NULL, body LONG VARCHAR, lang VARCHAR(8),
                       ver TIMESTAMPTZ NOT NULL DEFAULT CLOCK_TIMESTAMP());
CREATE TABLE ex.notes_journal (doc_id INT, op CHAR(1), ver TIMESTAMPTZ DEFAULT CLOCK_TIMESTAMP());
CREATE TABLE ex.owners (doc_id INT, owner VARCHAR(20));
INSERT INTO ex.notes (doc_id, body) VALUES (1, 'The supplier shall indemnify the buyer under this contract.');
INSERT INTO ex.notes (doc_id, body) VALUES (2, 'Contract renewal: indemnity terms unchanged, payment within 30 days.');
INSERT INTO ex.notes (doc_id, body) VALUES (3, 'Meeting notes: mark to market accounting for the energy contracts.');
INSERT INTO ex.notes (doc_id, body) VALUES (4, 'Holmes'' notes on the Baskerville case.');
INSERT INTO ex.notes (doc_id, body, lang) VALUES (5, 'Le contrat prévoit une indemnité.', 'fr');
INSERT INTO ex.owners VALUES (1, 'legal'); INSERT INTO ex.owners VALUES (2, 'sales');
INSERT INTO ex.owners VALUES (3, 'trading'); INSERT INTO ex.owners VALUES (4, 'legal'); INSERT INTO ex.owners VALUES (5, 'legal');
COMMIT;"

ex "create_index" "CALL vtext.create_index('notes', 'ex.notes', 'lang=en journal=ex.notes_journal margin=0');"

ex "info" "CALL vtext.info('notes');"

ex "tsearch: ranked search, joined to the source table" \
"SELECT r.rank, r.doc_id, ROUND(r.score, 4) AS score, n.body
FROM (SELECT vtext.tsearch(* USING PARAMETERS index='notes', query='contract AND indemn*', k=10) OVER()
      FROM ex.notes_live) r
JOIN ex.notes n USING (doc_id)
ORDER BY r.rank;"

ex "tsearch: proximity and a join with another table, aggregated" \
"SELECT o.owner, COUNT(*) AS hits
FROM (SELECT vtext.tsearch(* USING PARAMETERS index='notes', query='contract w/5 (indemnify OR indemnity OR market)', k=1000) OVER()
      FROM ex.notes_live) r
JOIN ex.owners o USING (doc_id)
GROUP BY o.owner ORDER BY hits DESC, o.owner;"

ex "tsearch: a batch of queries in one statement (parameter queries)" \
"SELECT qid, rank, doc_id FROM (SELECT vtext.tsearch(* USING PARAMETERS index='notes', k=2,
       queries=E'1\tholmes\n2\t\"mark to market\"\n3\tcontrat') OVER() FROM ex.notes_snap) r
ORDER BY qid, rank;"

ex "tcount" \
"SELECT * FROM (SELECT vtext.tcount(* USING PARAMETERS index='notes', query='contract') OVER() FROM ex.notes_live) c;"

ex "tmatch: every match without scores, for joins and GROUP BY over all matches" \
"SELECT o.owner, COUNT(*) AS documents
FROM (SELECT vtext.tmatch(* USING PARAMETERS index='notes', query='contract*') OVER() FROM ex.notes_live) m
JOIN ex.owners o USING (doc_id)
GROUP BY o.owner ORDER BY o.owner;"

ex "tsnippet" \
"SELECT doc_id, vtext.tsnippet(body, 'indemnity contract' USING PARAMETERS index='notes', words=8) AS snippet
FROM ex.notes WHERE doc_id IN (1, 2) ORDER BY doc_id;"

ex "refresh_index: a new row is found at once through the live view, after refresh_index through snap" \
"INSERT INTO ex.notes (doc_id, body) VALUES (6, 'Draft contract with an indemnity cap.'); COMMIT;" \
"SELECT 'live' AS view, doc_id FROM (SELECT vtext.tsearch(* USING PARAMETERS index='notes', query='cap') OVER() FROM ex.notes_live) r
UNION ALL
SELECT 'snap', doc_id FROM (SELECT vtext.tsearch(* USING PARAMETERS index='notes', query='cap') OVER() FROM ex.notes_snap) r;" \
"CALL vtext.refresh_index('notes');" \
"SELECT 'snap' AS view, doc_id FROM (SELECT vtext.tsearch(* USING PARAMETERS index='notes', query='cap') OVER() FROM ex.notes_snap) r;"

ex "merge into one segment" "CALL vtext.merge('notes', 1);" "CALL vtext.info('notes');" \
"SELECT s.gen, s.n_segments, s.n_docs FROM vtext.index_stats s JOIN vtext.indexes i USING (index_id)
WHERE i.index_name = 'notes' ORDER BY s.gen DESC LIMIT 1;"

ex "tinfo: the cache of every node" \
"SELECT * FROM (SELECT vtext.tinfo(USING PARAMETERS index='notes') OVER(PARTITION NODES) FROM vtext.probe) i;"

ex "load_all: write missing node caches from the index tables" "CALL vtext.load_all('notes');"

ex "rebuild (recovery only)" "CALL vtext.rebuild('notes');"

ex "tokenize: what the analyzer sees" \
"SELECT pos, term, stem, lang FROM (SELECT vtext.tokenize('Holmes'' contracts, Holmes-style' USING PARAMETERS lang='en') OVER()) t ORDER BY pos;"

ex "hybrid: two ranked lists fused with Reciprocal Rank Fusion" \
"SELECT h.rank, h.doc_id, ROUND(h.score, 5) AS score, h.sources
FROM (SELECT vtext.hybrid(src, doc_id, rank, score USING PARAMETERS k=3) OVER() FROM (
        SELECT 'text' AS src, doc_id, rank, score
        FROM (SELECT vtext.tsearch(* USING PARAMETERS index='notes', query='indemnity contract', k=10) OVER() FROM ex.notes_live) t
        UNION ALL
        SELECT 'vector', 3, 1, 0.91 UNION ALL SELECT 'vector', 2, 2, 0.88 UNION ALL SELECT 'vector', 6, 3, 0.80) u) h
ORDER BY h.rank;"

ex "tversion" "SELECT version, build, icu FROM (SELECT vtext.tversion() OVER()) v;"

if [ -x build/tools/vtext_broker ] && command -v curl > /dev/null; then
    echo; echo "-- tserve through vtext_broker (mode serve): HTTP in, the user's own Vertica session, one tserve statement"
    echo "build/tools/vtext_broker --port 9399 --schema ex --mode serve &"
    build/tools/vtext_broker --port 9399 --schema ex --mode serve > /dev/null 2>&1 &
    bp=$!
    sleep 1
    echo "curl -s -XPOST localhost:9399/notes/_search -d '{\"query\": \"contract AND indemn*\"}'"
    curl -s -m 20 -XPOST localhost:9399/notes/_search -d '{"query": "contract AND indemn*"}' | sed 's/^/   /'; echo
    kill $bp 2> /dev/null; wait $bp 2> /dev/null
fi

ex "drop_index" "CALL vtext.drop_index('notes');"
vsql -X -q -c "DROP SCHEMA IF EXISTS ex CASCADE" > /dev/null 2>&1
