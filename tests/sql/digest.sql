-- Results of a fixed set of queries on the vf_ref index of test_search.sh, as one line per query:
-- query, hits, and a digest of (doc_id, rank, score to 6 digits). Run on two systems and compare.
\pset tuples_only on
\pset format unaligned
SELECT (SELECT COUNT(*) FROM vf_ref.docs) || ' docs, ' || (SELECT SUM(HASH(doc_id, body)::NUMERIC(38,0)) FROM vf_ref.docs) || ' corpus digest';
SELECT 'kilo tango alpha', COUNT(*), SUM(HASH(doc_id, rank, ROUND(score, 6))::NUMERIC(38,0)) FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_ref', query='kilo tango alpha', k=500) OVER() FROM vf_ref.vf_ref_live) r;
SELECT '"alpha bravo"~2', COUNT(*), SUM(HASH(doc_id, rank, ROUND(score, 6))::NUMERIC(38,0)) FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_ref', query='"alpha bravo"~2', k=500) OVER() FROM vf_ref.vf_ref_live) r;
SELECT 'kilo w/3 tango OR lima', COUNT(*), SUM(HASH(doc_id, rank, ROUND(score, 6))::NUMERIC(38,0)) FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_ref', query='(kilo w/3 tango) OR lima', k=500) OVER() FROM vf_ref.vf_ref_live) r;
SELECT 'charlie -delta', COUNT(*), SUM(HASH(doc_id, rank, ROUND(score, 6))::NUMERIC(38,0)) FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_ref', query='charlie -delta', k=3000) OVER() FROM vf_ref.vf_ref_live) r;
SELECT 'tangp~1 ki*', COUNT(*), SUM(HASH(doc_id, rank, ROUND(score, 6))::NUMERIC(38,0)) FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_ref', query='tangp~1 ki*', k=500) OVER() FROM vf_ref.vf_ref_live) r;
