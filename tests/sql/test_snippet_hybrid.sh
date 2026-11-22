#!/usr/bin/env bash
# tsnippet and hybrid (milestone M6): snippets with the index's analyzer; RRF and weighted fusion on
# a fixture with known answers; and, when vvector is installed, BM25 + vector search fused in SQL.
set -uo pipefail
cd "$(dirname "$0")"; source ./lib.sh
echo "$TEST_NAME"

q "CALL vfts.drop_index('vf_hy')" > /dev/null 2>&1
q "DROP SCHEMA IF EXISTS vf_hy CASCADE" > /dev/null
q "CREATE SCHEMA vf_hy;
   CREATE TABLE vf_hy.docs (doc_id INT NOT NULL, body LONG VARCHAR, vec ARRAY[FLOAT], del BOOLEAN NOT NULL DEFAULT FALSE,
                            ver TIMESTAMPTZ NOT NULL DEFAULT CLOCK_TIMESTAMP());
   INSERT INTO vf_hy.docs (doc_id, body, vec) VALUES (1, 'The indemnity clause of the contract protects both parties.', ARRAY[1.0, 0.0, 0.0]);
   INSERT INTO vf_hy.docs (doc_id, body, vec) VALUES (2, 'A contract without an indemnity is risky.', ARRAY[0.9, 0.1, 0.0]);
   INSERT INTO vf_hy.docs (doc_id, body, vec) VALUES (3, 'Payment terms and late fees.', ARRAY[0.8, 0.2, 0.1]);
   INSERT INTO vf_hy.docs (doc_id, body, vec) VALUES (4, 'Sherlock Holmes, the detective.', ARRAY[0.0, 1.0, 0.0]);
   INSERT INTO vf_hy.docs (doc_id, body, vec) VALUES (5, 'Contracts are signed by both parties after review.', ARRAY[0.0, 0.0, 1.0]);
   COMMIT;" > /dev/null
q "CALL vfts.create_index('vf_hy', 'vf_hy.docs', 'margin=0')" > /dev/null

# tsnippet
expect "snippet marks hits" \
       "$(q "SELECT vfts.tsnippet(body, 'contract w/5 indemnity' USING PARAMETERS index='vf_hy') FROM vf_hy.docs WHERE doc_id = 2")" \
       "A <b>contract</b> without an <b>indemnity</b> is risky."
expect "snippet with stems and tags" \
       "$(q "SELECT vfts.tsnippet(body, 'contract' USING PARAMETERS index='vf_hy', pre_tag='[', post_tag=']') FROM vf_hy.docs WHERE doc_id = 5")" \
       "[Contracts] are signed by both parties after review."
expect "snippet of the top hits, joined to the search" \
       "$(q "SELECT r.rank || ' ' || vfts.tsnippet(d.body, 'holmes' USING PARAMETERS index='vf_hy') FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_hy', query='holmes', k=3) OVER() FROM vf_hy.vf_hy_live) r JOIN vf_hy.docs d USING (doc_id)")" \
       "1 Sherlock <b>Holmes</b>, the detective."
expect "snippet window" \
       "$(q "SELECT vfts.tsnippet(body, 'parties' USING PARAMETERS index='vf_hy', words=3, fragments=1) FROM vf_hy.docs WHERE doc_id = 1")" \
       "... protects both <b>parties</b>."

# hybrid on fixed lists: text ranks d1, d2, d3; vector ranks d3, d1, d4. RRF (k = 60):
# d1 = 1/61 + 1/62, d3 = 1/63 + 1/61, d2 = 1/62, d4 = 1/63
LISTS="SELECT 'text' AS src, 1 AS doc_id, 1 AS rank, 9.0 AS score UNION ALL SELECT 'text', 2, 2, 5.0 UNION ALL SELECT 'text', 3, 3, 1.0
       UNION ALL SELECT 'vector', 3, 1, 0.1 UNION ALL SELECT 'vector', 1, 2, 0.4 UNION ALL SELECT 'vector', 4, 3, 0.9"
expect "RRF order and scores" \
       "$(q "SELECT doc_id || ':' || ROUND(score, 6) || ':' || sources FROM (SELECT vfts.hybrid(src, doc_id, rank, score USING PARAMETERS k=10) OVER() FROM ($LISTS) l) h ORDER BY rank")" \
       "$(printf '1:0.032522:text:1 vector:2\n3:0.032266:text:3 vector:1\n2:0.016129:text:2\n4:0.015873:vector:3')"
expect "RRF weights" \
       "$(q "SELECT LISTAGG(doc_id::VARCHAR USING PARAMETERS separator=' ') FROM (SELECT doc_id FROM (SELECT vfts.hybrid(src, doc_id, rank, score USING PARAMETERS k=2, weights='vector=3') OVER() FROM ($LISTS) l) h ORDER BY rank) x")" \
       "3 1"
# weighted: text min-max (9 -> 1, 5 -> 0.5, 1 -> 0); vector is a distance (0.1 -> 1, 0.4 -> 0.625, 0.9 -> 0)
expect "weighted, with a distance source" \
       "$(q "SELECT doc_id || ':' || ROUND(score, 4) FROM (SELECT vfts.hybrid(src, doc_id, rank, score USING PARAMETERS method='weighted', distance='vector', k=10) OVER() FROM ($LISTS) l) h ORDER BY rank")" \
       "$(printf '1:1.625\n3:1\n2:0.5\n4:0')"
expect_error "bad method" "SELECT vfts.hybrid(src, doc_id, rank, score USING PARAMETERS method='x') OVER() FROM ($LISTS) l" "method must be"

# BM25 + vectors with vvector, when it is installed
if [ "$(q "SELECT COUNT(*) FROM v_monitor.user_libraries WHERE lib_name = 'vvector'")" = "1" ]; then
    q "CALL vvector.register_index('vf_hy_vec', 'vf_hy.docs', 'doc_id', 'vec', 'del', 'ver', 'cosine', 0, 'flat')" > /dev/null 2>&1
    q "CALL vvector.refresh_index('vf_hy_vec')" > /dev/null 2>&1
    # text (BM25): docs 1, 2, 5 contain contract or indemnity; vector (cosine to [1, 0.05, 0]): 1, 2, 3, 4, 5.
    # RRF: doc 5 (text rank 3 + vector rank 5) beats doc 3 (vector rank 3 only): 1/63 + 1/65 > 1/63.
    expect "tsearch + vsearch fused with RRF" \
           "$(q "SELECT LISTAGG(doc_id::VARCHAR USING PARAMETERS separator=' ') FROM (SELECT doc_id FROM (SELECT vfts.hybrid(src, doc_id, rank, score USING PARAMETERS k=4) OVER() FROM (
                   SELECT 'text' AS src, doc_id, rank, score FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_hy', query='contract indemnity', k=10) OVER() FROM vf_hy.vf_hy_live) t
                   UNION ALL
                   SELECT 'vector', id, rank, score FROM (SELECT vvector.vsearch(qid, qvec, id, vec, del, ver, snapshot_id USING PARAMETERS index_name='vf_hy_vec', query='[1, 0.05, 0]', k=10) OVER() FROM vf_hy.vf_hy_vec_snap) v
                 ) u) h ORDER BY rank) x")" \
           "1 2 5 3"
    q "CALL vvector.unregister_index('vf_hy_vec')" > /dev/null 2>&1
else
    echo "  skip tsearch + vsearch (vvector is not installed)"
fi

done_test
