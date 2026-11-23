#!/usr/bin/env bash
# Every example of README.md "Reference", run on a small table of its own (default: schema ex, index notes).
# Prints each statement and its output, as in the README. Safe to run again.
#
#   scripts/examples.sh            run all examples
#   scripts/examples.sh --clean    remove the schema and the index
#
# Needs: vfts deployed (make deploy) and the vsql connection variables. The tserve example also
# needs build/tools/vfts_broker (make tools) and curl. EX_SCHEMA and EX_INDEX change the names
# (install.sh uses vfts_selftest). An existing schema or index of that name that this script did
# not make is never touched: the script stops. Exit status: the number of failed statements.
set -uo pipefail
cd "$(dirname "$0")/.."
S=${EX_SCHEMA:-ex}; I=${EX_INDEX:-notes}
case "$S$I" in *[!A-Za-z0-9_]*) echo "examples.sh: EX_SCHEMA and EX_INDEX: letters, digits, underscore" >&2; exit 2 ;; esac
q1() { vsql -X -A -t -q -c "$1" 2> /dev/null; }
mine() {   # the schema is this script's when it holds its marker table
    [ "$(q1 "SELECT COUNT(*) FROM v_catalog.tables WHERE table_schema = '$S' AND table_name = 'vfts_examples_marker'")" = 1 ]
}
clean() {
    if [ "$(q1 "SELECT COUNT(*) FROM v_catalog.schemata WHERE schema_name = '$S'")" = 1 ] && ! mine; then
        echo "examples.sh: schema $S exists and was not made by this script: not touched (set EX_SCHEMA)" >&2; exit 3
    fi
    local src; src=$(q1 "SELECT source_table FROM vfts.indexes WHERE index_name = '$I'")
    if [ -n "$src" ] && [ "${src%%.*}" != "$S" ]; then
        echo "examples.sh: index $I exists on $src: not touched (set EX_INDEX)" >&2; exit 3
    fi
    [ -n "$src" ] && vsql -X -q -c "CALL vfts.drop_index('$I')" > /dev/null 2>&1
    vsql -X -q -c "DROP SCHEMA IF EXISTS $S CASCADE" > /dev/null 2>&1
    return 0
}
if [ "${1:-}" = --clean ]; then clean; echo "examples removed"; exit 0; fi
FAILS=0
ex() {   # title, then statements: each is printed with its output; a failed one is counted
    echo; echo "-- $1"; shift
    for s in "$@"; do
        s=${s//SCHEMA ex;/SCHEMA $S;}; s=${s//ex./$S.}; s=${s//\'notes\'/\'$I\'}; s=${s//notes_live/${I}_live}; s=${s//notes_snap/${I}_snap}; s=${s//notes_journal/${I}_journal}
        echo "$s"
        local out rc
        out=$(vsql -X -v ON_ERROR_STOP=1 -c "$s" 2>&1); rc=$?
        printf '%s\n' "$out" | grep -v '^$' | sed 's/^/   /'
        [ $rc -eq 0 ] || FAILS=$((FAILS + 1))
    done
}
clean

ex "A source table: doc_id, body, lang, ver (README: Prepare a table)" \
"CREATE SCHEMA ex;
CREATE TABLE ex.vfts_examples_marker (made_by VARCHAR(40));
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

ex "create_index" "CALL vfts.create_index('notes', 'ex.notes', 'lang=en journal=ex.notes_journal margin=0');"

ex "info" "CALL vfts.info('notes');"

ex "tsearch: ranked search, joined to the source table" \
"SELECT r.rank, r.doc_id, ROUND(r.score, 4) AS score, n.body
FROM (SELECT vfts.tsearch(* USING PARAMETERS index='notes', query='contract AND indemn*', k=10) OVER()
      FROM ex.notes_live) r
JOIN ex.notes n USING (doc_id)
ORDER BY r.rank;"

ex "tsearch: proximity and a join with another table, aggregated" \
"SELECT o.owner, COUNT(*) AS hits
FROM (SELECT vfts.tsearch(* USING PARAMETERS index='notes', query='contract w/5 (indemnify OR indemnity OR market)', k=1000) OVER()
      FROM ex.notes_live) r
JOIN ex.owners o USING (doc_id)
GROUP BY o.owner ORDER BY hits DESC, o.owner;"

ex "tsearch: a batch of queries in one statement (parameter queries)" \
"SELECT qid, rank, doc_id FROM (SELECT vfts.tsearch(* USING PARAMETERS index='notes', k=2,
       queries=E'1\tholmes\n2\t\"mark to market\"\n3\tcontrat') OVER() FROM ex.notes_snap) r
ORDER BY qid, rank;"

ex "tsearch: mode stem (default) against mode exact, and operator and" \
"SELECT 'stem' AS mode, doc_id, rank FROM (SELECT vfts.tsearch(* USING PARAMETERS index='notes', query='contracts') OVER() FROM ex.notes_live) r
UNION ALL
SELECT 'exact', doc_id, rank FROM (SELECT vfts.tsearch(* USING PARAMETERS index='notes', query='contracts', mode='exact') OVER() FROM ex.notes_live) r
UNION ALL
SELECT 'and', doc_id, rank FROM (SELECT vfts.tsearch(* USING PARAMETERS index='notes', query='contract indemnity', operator='and') OVER() FROM ex.notes_live) r
ORDER BY 1 DESC, 3;"

ex "tcount" \
"SELECT * FROM (SELECT vfts.tcount(* USING PARAMETERS index='notes', query='contract') OVER() FROM ex.notes_live) c;"

ex "tcount: several counts in one statement (parameter queries)" \
"SELECT qid, count FROM (SELECT vfts.tcount(* USING PARAMETERS index='notes',
       queries=E'1\tcontract\n2\tcontract AND indemn*\n3\tholmse~1\n4\tsupplier pre/3 indemnify') OVER() FROM ex.notes_live) c
ORDER BY qid;"

ex "tmatch: every match without scores, for joins and GROUP BY over all matches" \
"SELECT o.owner, COUNT(*) AS documents
FROM (SELECT vfts.tmatch(* USING PARAMETERS index='notes', query='contract*') OVER() FROM ex.notes_live) m
JOIN ex.owners o USING (doc_id)
GROUP BY o.owner ORDER BY o.owner;"

ex "add_attribute: a column of another table, copied into the index for filters and counts" \
"CALL vfts.add_attribute('notes', 'owner', 'ex.owners', 'doc_id', 'owner');"

ex "tsearch with filter: only the matches whose attribute passes (scores unchanged)" \
"SELECT doc_id, ROUND(score, 4) AS score, rank
FROM (SELECT vfts.tsearch(* USING PARAMETERS index='notes', query='contract* OR indemn*', filter='owner IN (''legal'', ''sales'')') OVER()
      FROM ex.notes_live) r
ORDER BY rank;"

ex "tcount with by: matches per attribute value, counted while matching" \
"SELECT value AS owner, count
FROM (SELECT vfts.tcount(* USING PARAMETERS index='notes', query='contract*', by='owner') OVER() FROM ex.notes_live) c
ORDER BY owner;"

ex "refresh_attributes: after the table changes, copy it again" \
"UPDATE ex.owners SET owner = 'legal' WHERE doc_id = 3; COMMIT;" \
"CALL vfts.refresh_attributes('notes');" \
"SELECT value AS owner, count
FROM (SELECT vfts.tcount(* USING PARAMETERS index='notes', query='contract*', by='owner', filter='owner <> ''sales''') OVER() FROM ex.notes_live) c
ORDER BY owner;"

ex "refresh_attribute: copy one attribute again" \
"CALL vfts.refresh_attribute('notes', 'owner');" \
"SELECT a.attr_name, a.attr_type, a.source_table, a.version, a.n_rows FROM vfts.attributes a JOIN vfts.indexes i USING (index_id)
WHERE i.index_name = 'notes';"

ex "tsnippet" \
"SELECT doc_id, vfts.tsnippet(body, 'indemnity contract' USING PARAMETERS index='notes', words=8) AS snippet
FROM ex.notes WHERE doc_id IN (1, 2) ORDER BY doc_id;"

ex "tsnippet: the snippets of the top hits, with tags of your own" \
"SELECT r.rank, vfts.tsnippet(n.body, 'contract AND indemn*' USING PARAMETERS index='notes', words=6, pre_tag='[', post_tag=']') AS snippet
FROM (SELECT vfts.tsearch(* USING PARAMETERS index='notes', query='contract AND indemn*', k=3) OVER() FROM ex.notes_live) r
JOIN ex.notes n USING (doc_id)
ORDER BY r.rank;"

ex "refresh_index: a new row is found at once through the live view, after refresh_index through snap" \
"INSERT INTO ex.notes (doc_id, body) VALUES (6, 'Draft contract with an indemnity cap.'); COMMIT;" \
"SELECT 'live' AS view, doc_id FROM (SELECT vfts.tsearch(* USING PARAMETERS index='notes', query='cap') OVER() FROM ex.notes_live) r
UNION ALL
SELECT 'snap', doc_id FROM (SELECT vfts.tsearch(* USING PARAMETERS index='notes', query='cap') OVER() FROM ex.notes_snap) r;" \
"CALL vfts.refresh_index('notes');" \
"SELECT 'snap' AS view, doc_id FROM (SELECT vfts.tsearch(* USING PARAMETERS index='notes', query='cap') OVER() FROM ex.notes_snap) r;"

ex "merge: the tiered policy (what refresh_index runs after each refresh)" "CALL vfts.merge('notes');"

ex "merge into one segment" "CALL vfts.merge('notes', 1);" "CALL vfts.info('notes');" \
"SELECT s.gen, s.n_segments, s.n_docs FROM vfts.index_stats s JOIN vfts.indexes i USING (index_id)
WHERE i.index_name = 'notes' ORDER BY s.gen DESC LIMIT 1;"

ex "tinfo: the cache of every node" \
"SELECT * FROM (SELECT vfts.tinfo(USING PARAMETERS index='notes') OVER(PARTITION NODES) FROM vfts.probe) i;"

ex "load_all: write missing node caches from the index tables" "CALL vfts.load_all('notes');"
ex "check_access: read the access policies again and make a new key" "CALL vfts.check_access('notes');" \
"SELECT index_name, access, reorder FROM vfts.indexes WHERE index_name = 'notes';"

ex "rebuild (recovery only)" "CALL vfts.rebuild('notes');"

ex "tokenize: what the analyzer sees" \
"SELECT pos, term, stem, lang FROM (SELECT vfts.tokenize('Holmes'' contracts, Holmes-style' USING PARAMETERS lang='en') OVER()) t ORDER BY pos;"

ex "tokenize: the rows of a table, beside their doc_id (each row in its own language)" \
"SELECT doc_id, pos, term, stem, lang FROM (SELECT doc_id, vfts.tokenize(body, lang) FROM ex.notes WHERE doc_id IN (4, 5)) t
ORDER BY doc_id, pos;"

ex "hybrid: two ranked lists fused with Reciprocal Rank Fusion" \
"SELECT h.rank, h.doc_id, ROUND(h.score, 5) AS score, h.sources
FROM (SELECT vfts.hybrid(src, doc_id, rank, score USING PARAMETERS k=3) OVER() FROM (
        SELECT 'text' AS src, doc_id, rank, score
        FROM (SELECT vfts.tsearch(* USING PARAMETERS index='notes', query='indemnity contract', k=10) OVER() FROM ex.notes_live) t
        UNION ALL
        SELECT 'vector', 3, 1, 0.91 UNION ALL SELECT 'vector', 2, 2, 0.88 UNION ALL SELECT 'vector', 6, 3, 0.80) u) h
ORDER BY h.rank;"

ex "hybrid: the same lists, weighted (min-max normalized scores)" \
"SELECT h.rank, h.doc_id, ROUND(h.score, 4) AS score, h.sources
FROM (SELECT vfts.hybrid(src, doc_id, rank, score USING PARAMETERS k=3, method='weighted', weights='text=0.7,vector=0.3') OVER() FROM (
        SELECT 'text' AS src, doc_id, rank, score
        FROM (SELECT vfts.tsearch(* USING PARAMETERS index='notes', query='indemnity contract', k=10) OVER() FROM ex.notes_live) t
        UNION ALL
        SELECT 'vector', 3, 1, 0.91 UNION ALL SELECT 'vector', 2, 2, 0.88 UNION ALL SELECT 'vector', 6, 3, 0.80) u) h
ORDER BY h.rank;"

ex "tversion" "SELECT version, build, icu FROM (SELECT vfts.tversion() OVER()) v;"

if [ -x build/tools/vfts_broker ] && command -v curl > /dev/null; then
    echo; echo "-- tserve through vfts_broker (mode serve): HTTP in, the user's own Vertica session, one tserve statement"
    echo "build/tools/vfts_broker --port 9399 --schema $S --mode serve &"
    build/tools/vfts_broker --port 9399 --schema "$S" --mode serve > /dev/null 2>&1 &
    bp=$!
    sleep 1
    echo "curl -s -XPOST localhost:9399/$I/_search -d '{\"query\": \"contract AND indemn*\"}'"
    r=$(curl -s -m 20 -XPOST "localhost:9399/$I/_search" -d '{"query": "contract AND indemn*"}')
    echo "   $r"
    case "$r" in *'"hits"'*) ;; *) FAILS=$((FAILS + 1)) ;; esac
    echo "curl -s -XPOST localhost:9399/$I/_search -d '{\"query\": \"contract*\", \"filter\": \"owner = '\\''legal'\\''\", \"k\": 2}'"
    r=$(curl -s -m 20 -XPOST "localhost:9399/$I/_search" -d '{"query": "contract*", "filter": "owner = '\''legal'\''", "k": 2}')
    echo "   $r"
    case "$r" in *'"hits"'*) ;; *) FAILS=$((FAILS + 1)) ;; esac
    echo "curl -s -XPOST localhost:9399/$I/_count -d '{\"query\": \"contract*\", \"by\": \"owner\"}'"
    r=$(curl -s -m 20 -XPOST "localhost:9399/$I/_count" -d '{"query": "contract*", "by": "owner"}')
    echo "   $r"
    case "$r" in *'"buckets"'*) ;; *) FAILS=$((FAILS + 1)) ;; esac
    kill $bp 2> /dev/null; wait $bp 2> /dev/null
fi

ex "drop_attribute" "CALL vfts.drop_attribute('notes', 'owner');" \
"SELECT COUNT(*) AS attributes FROM vfts.attributes a JOIN vfts.indexes i USING (index_id) WHERE i.index_name = 'notes';"

ex "drop_index" "CALL vfts.drop_index('notes');"
vsql -X -q -c "DROP SCHEMA IF EXISTS $S CASCADE" > /dev/null 2>&1
exit $FAILS
