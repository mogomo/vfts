#!/usr/bin/env bash
# tsearch and tcount against plain SQL (correctness rules 1 and 2): a random corpus, tokenized into a
# reference table with the same analyzer (vfts.tokenize); boolean, phrase, w/N and pre/N results must
# equal the SQL answer, and BM25 scores must equal the formula computed in SQL.
set -uo pipefail
cd "$(dirname "$0")"; source ./lib.sh
echo "$TEST_NAME"

q "CALL vfts.drop_index('vf_ref')" > /dev/null 2>&1
q "DROP SCHEMA IF EXISTS vf_ref CASCADE" > /dev/null
q "CREATE SCHEMA vf_ref;
   CREATE TABLE vf_ref.docs (doc_id INT NOT NULL, body LONG VARCHAR, ver TIMESTAMPTZ DEFAULT CLOCK_TIMESTAMP());
   CREATE TABLE vf_ref.docs_journal (doc_id INT, op CHAR(1), ver TIMESTAMPTZ DEFAULT CLOCK_TIMESTAMP());" > /dev/null

# 3072 documents of 3 to 20 words from a vocabulary of 24 (documents stay below 24 words, so the
# one-byte length norm is exact and the SQL formula needs no quantization).
q "INSERT INTO vf_ref.docs (doc_id, body)
   SELECT doc_id, LISTAGG(w USING PARAMETERS separator=' ', max_length=4000)
   FROM (SELECT d.doc_id, SPLIT_PART('alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima mike november oscar papa quebec romeo sierra tango uniform victor whiskey xray',
                                     ' ', (1 + MOD(HASH(d.doc_id, p.k) // 7, 24) * MOD(HASH(p.k, d.doc_id) // 3, 24) // 23)::INT) AS w
         FROM (SELECT a.k + 1024 * b.j AS doc_id FROM vfts.probe a CROSS JOIN (SELECT 0 AS j UNION ALL SELECT 1 UNION ALL SELECT 2) b) d
         JOIN vfts.probe p ON p.k <= 3 + MOD(HASH(d.doc_id) // 11, 18)) x
   GROUP BY doc_id;
   COMMIT;" > /dev/null
expect "corpus" "$(q "SELECT COUNT(*) FROM vf_ref.docs")" "3072"

q "CALL vfts.create_index('vf_ref', 'vf_ref.docs', 'journal=vf_ref.docs_journal margin=0')" > /dev/null
q "CREATE TABLE vf_ref.tok AS SELECT doc_id, pos, term FROM (SELECT doc_id, vfts.tokenize(body) FROM vf_ref.docs) t; COMMIT;" > /dev/null

S() { echo "SELECT doc_id FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_ref', query='$1', k=1000000, mode='exact') OVER() FROM vf_ref.vf_ref_live) r"; }
C() { echo "SELECT count FROM (SELECT vfts.tcount(* USING PARAMETERS index='vf_ref', query='$1', mode='exact') OVER() FROM vf_ref.vf_ref_live) r"; }
T() { echo "SELECT DISTINCT doc_id FROM vf_ref.tok WHERE term = '$1'"; }
ALL="SELECT doc_id FROM vf_ref.docs"

# same set (both directions), same count from tcount, and not trivially empty
check_set() {   # name, vfts query, reference SQL
    local diff n c
    diff=$(q "SELECT COUNT(*) FROM (($(S "$2")) EXCEPT ($3)) a")$(q "SELECT COUNT(*) FROM (($3) EXCEPT ($(S "$2"))) b")
    n=$(q "SELECT COUNT(*) FROM ($3) x")
    c=$(q "$(C "$2")")
    expect "$1 ($n docs)" "$diff $c" "00 $n"
}

check_set "term"            "kilo"                        "$(T kilo)"
check_set "OR"              "kilo OR tango"               "$(T kilo) UNION $(T tango)"
check_set "implicit OR"     "kilo tango"                  "$(T kilo) UNION $(T tango)"
check_set "AND"             "kilo AND tango"              "$(T kilo) INTERSECT $(T tango)"
check_set "NOT"             "kilo NOT tango"              "$(T kilo) EXCEPT $(T tango)"
check_set "-term"           "kilo -tango"                 "$(T kilo) EXCEPT $(T tango)"
check_set "pure NOT"        "NOT alpha"                   "$ALL EXCEPT $(T alpha)"
check_set "nested"          "(kilo OR lima) AND NOT (tango OR alpha)" "($(T kilo) UNION $(T lima)) EXCEPT ($(T tango) UNION $(T alpha))"
check_set "+must should"    "+kilo lima"                  "$(T kilo)"
check_set "phrase"          '"alpha bravo"' \
          "SELECT DISTINCT a.doc_id FROM vf_ref.tok a JOIN vf_ref.tok b ON a.doc_id = b.doc_id AND b.pos = a.pos + 1 WHERE a.term = 'alpha' AND b.term = 'bravo'"
check_set "phrase of 3"     '"alpha bravo alpha"' \
          "SELECT DISTINCT a.doc_id FROM vf_ref.tok a JOIN vf_ref.tok b ON a.doc_id = b.doc_id AND b.pos = a.pos + 1 JOIN vf_ref.tok c ON c.doc_id = a.doc_id AND c.pos = a.pos + 2 WHERE a.term = 'alpha' AND b.term = 'bravo' AND c.term = 'alpha'"
check_set "w/3"             "kilo w/3 tango" \
          "SELECT DISTINCT a.doc_id FROM vf_ref.tok a JOIN vf_ref.tok b ON a.doc_id = b.doc_id WHERE a.term = 'kilo' AND b.term = 'tango' AND ABS(a.pos - b.pos) BETWEEN 1 AND 3"
check_set "pre/2"           "kilo pre/2 tango" \
          "SELECT DISTINCT a.doc_id FROM vf_ref.tok a JOIN vf_ref.tok b ON a.doc_id = b.doc_id WHERE a.term = 'kilo' AND b.term = 'tango' AND b.pos - a.pos BETWEEN 1 AND 2"
check_set "w/1 any order"   "bravo w/1 alpha" \
          "SELECT DISTINCT a.doc_id FROM vf_ref.tok a JOIN vf_ref.tok b ON a.doc_id = b.doc_id WHERE a.term = 'bravo' AND b.term = 'alpha' AND ABS(a.pos - b.pos) = 1"
check_set "slop phrase"     '"kilo tango"~2' \
          "SELECT DISTINCT a.doc_id FROM vf_ref.tok a JOIN vf_ref.tok b ON a.doc_id = b.doc_id WHERE a.term = 'kilo' AND b.term = 'tango' AND ((b.pos > a.pos AND b.pos - a.pos - 1 <= 2) OR (b.pos < a.pos AND a.pos - b.pos + 1 <= 2))"
check_set "OR group w/2"    "(kilo OR lima) w/2 tango" \
          "SELECT DISTINCT a.doc_id FROM vf_ref.tok a JOIN vf_ref.tok b ON a.doc_id = b.doc_id WHERE a.term IN ('kilo', 'lima') AND b.term = 'tango' AND ABS(a.pos - b.pos) BETWEEN 1 AND 2"
check_set "w/N AND term"    "(kilo w/2 tango) AND alpha" \
          "(SELECT DISTINCT a.doc_id FROM vf_ref.tok a JOIN vf_ref.tok b ON a.doc_id = b.doc_id WHERE a.term = 'kilo' AND b.term = 'tango' AND ABS(a.pos - b.pos) BETWEEN 1 AND 2) INTERSECT $(T alpha)"
check_set "prefix"          "ki*"                         "SELECT DISTINCT doc_id FROM vf_ref.tok WHERE term LIKE 'ki%'"
check_set "wildcard"        "?ango"                       "SELECT DISTINCT doc_id FROM vf_ref.tok WHERE term LIKE '_ango'"
check_set "fuzzy"           "tangp~1"                     "$(T tango)"

# BM25: k1 = 1.2, b = 0.75, idf = ln(1 + (N - df + 0.5) / (df + 0.5)),
# score = idf * tf / (tf + k1 * (1 - b + b * len / avgdl)) (Lucene, OpenSearch); with bm25='classic'
# times (k1 + 1) (Elasticsearch). Scores equal the formula
BM25="WITH st AS (SELECT COUNT(*) AS n, AVG(len) AS avgdl FROM (SELECT doc_id, MAX(pos) + 1 AS len FROM vf_ref.tok GROUP BY doc_id) l),
      dl AS (SELECT doc_id, MAX(pos) + 1 AS len FROM vf_ref.tok GROUP BY doc_id),
      tf AS (SELECT doc_id, term, COUNT(*) AS tf FROM vf_ref.tok GROUP BY doc_id, term),
      df AS (SELECT term, COUNT(*) AS df FROM tf GROUP BY term),
      sc AS (SELECT tf.doc_id, tf.term, LN(1 + (st.n - df.df + 0.5) / (df.df + 0.5)) * tf.tf / (tf.tf + 1.2 * (1 - 0.75 + 0.75 * dl.len / st.avgdl)) AS s
             FROM tf JOIN df USING (term) JOIN dl USING (doc_id) CROSS JOIN st)"
expect "BM25 single term, max |diff| < 1e-4" \
       "$(q "$BM25 SELECT (MAX(ABS(r.score - sc.s)) < 0.0001)::INT || ' ' || COUNT(*) FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_ref', query='kilo', k=1000000) OVER() FROM vf_ref.vf_ref_live) r JOIN sc ON sc.doc_id = r.doc_id AND sc.term = 'kilo'")" \
       "1 $(q "SELECT COUNT(*) FROM ($(T kilo)) x")"
expect "BM25 OR sums, max |diff| < 1e-4" \
       "$(q "$BM25 SELECT (MAX(ABS(r.score - x.s)) < 0.0001)::INT FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_ref', query='kilo tango alpha', k=1000000) OVER() FROM vf_ref.vf_ref_live) r JOIN (SELECT doc_id, SUM(s) AS s FROM sc WHERE term IN ('kilo', 'tango', 'alpha') GROUP BY doc_id) x USING (doc_id)")" \
       "1"
expect "bm25=classic multiplies by k1 + 1" \
       "$(q "$BM25 SELECT (MAX(ABS(r.score - 2.2 * sc.s)) < 0.0002)::INT FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_ref', query='kilo', k=1000000, bm25='classic') OVER() FROM vf_ref.vf_ref_live) r JOIN sc ON sc.doc_id = r.doc_id AND sc.term = 'kilo'")" "1"
expect "ranks follow scores, ties by doc_id" \
       "$(q "SELECT COUNT(*) FROM (SELECT rank, ROW_NUMBER() OVER(ORDER BY score DESC, doc_id) AS rn FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_ref', query='kilo tango', k=200) OVER() FROM vf_ref.vf_ref_live) r) x WHERE rank <> rn")" "0"
expect "batch of queries (qid rows)" \
       "$(q "SELECT qid || ':' || COUNT(*) FROM (SELECT vfts.tcount(* USING PARAMETERS index='vf_ref', mode='exact') OVER() FROM (SELECT * FROM vf_ref.vf_ref_live UNION ALL SELECT 1, 'kilo', NULL, NULL, NULL, NULL, NULL UNION ALL SELECT 2, 'tango', NULL, NULL, NULL, NULL, NULL) i) r GROUP BY qid, count ORDER BY qid")" \
       "$(printf '1:1\n2:1')"
expect "\"match\" (quoted, reserved word) works too" \
       "$(q "SELECT count FROM (SELECT vfts.tcount(* USING PARAMETERS index='vf_ref', query='kilo', \"match\"='exact') OVER() FROM vf_ref.vf_ref_live) r")" \
       "$(q "SELECT COUNT(*) FROM ($(T kilo)) x")"
expect "snap view" "$(q "SELECT COUNT(*) FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_ref', query='kilo', k=5) OVER() FROM vf_ref.vf_ref_snap) r")" "5"
expect_error "syntax error is reported" "SELECT vfts.tsearch(* USING PARAMETERS index='vf_ref', query='kilo AND') OVER() FROM vf_ref.vf_ref_live" "AND without a right operand"
expect_error "unknown index" "SELECT vfts.tsearch(* USING PARAMETERS index='nope', query='kilo') OVER() FROM vf_ref.vf_ref_live" "no cache for index nope"

# session defaults: ALTER SESSION SET UDPARAMETER FOR vfts.vfts (the library's qualified name)
expect "session default k = 3" \
       "$(q "ALTER SESSION SET UDPARAMETER FOR vfts.vfts k = '3'; SELECT COUNT(*) FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_ref', query='kilo') OVER() FROM vf_ref.vf_ref_live) r")" "3"
expect "a parameter overrides the session default" \
       "$(q "ALTER SESSION SET UDPARAMETER FOR vfts.vfts k = '3'; SELECT COUNT(*) FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_ref', query='kilo', k=5) OVER() FROM vf_ref.vf_ref_live) r")" "5"
done_test
