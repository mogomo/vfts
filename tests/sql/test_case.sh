#!/usr/bin/env bash
# case=sensitive (vfts 0.5.0): words keep their case in the index and in queries; stems keep it
# too; the default index stays case folded. Exact, stem, prefix, phrase, tsnippet, tokenize, the
# live tail and a refresh, export and import.
# Creates schema vf_cs; drops it at the end.
set -uo pipefail
cd "$(dirname "$0")"; source ./lib.sh
echo "$TEST_NAME"

cleanup() {
    for i in vf_cs vf_ci; do q "CALL vfts.drop_index('$i')" > /dev/null 2>&1; done
    q "DROP SCHEMA IF EXISTS vf_cs CASCADE" > /dev/null 2>&1
}
cleanup
q "CREATE SCHEMA vf_cs;
   CREATE TABLE vf_cs.docs (doc_id INT NOT NULL, body LONG VARCHAR);
   INSERT INTO vf_cs.docs VALUES (1, 'Apple released a new phone');
   INSERT INTO vf_cs.docs VALUES (2, 'an apple a day');
   INSERT INTO vf_cs.docs VALUES (3, 'APPLE PIE RECIPE');
   INSERT INTO vf_cs.docs VALUES (4, 'Apples and pears at the market');
   INSERT INTO vf_cs.docs VALUES (5, 'The US Army and us');
   COMMIT;" > /dev/null
q "CALL vfts.create_index('vf_cs', 'vf_cs.docs', 'case=sensitive refresh_every=0')" > /dev/null
q "CALL vfts.create_index('vf_ci', 'vf_cs.docs', 'refresh_every=0')" > /dev/null
ids() {   # index view query [mode] -> doc_ids, ascending
    q "SELECT doc_id FROM (SELECT vfts.tsearch(* USING PARAMETERS index='$1', query='$3', mode='${4:-stem}', k=100) OVER() FROM vf_cs.$2) r ORDER BY doc_id" | tr '\n' ' '
}
expect "case=sensitive is stored" "$(q "SELECT case_sensitive FROM vfts.indexes WHERE index_name = 'vf_cs'")" "t"
expect "exact: Apple only"            "$(ids vf_cs vf_cs_live Apple exact)" "1 "
expect "exact: apple only"            "$(ids vf_cs vf_cs_live apple exact)" "2 "
expect "exact: APPLE only"            "$(ids vf_cs vf_cs_live APPLE exact)" "3 "
expect "stem: Apple and Apples"       "$(ids vf_cs vf_cs_live Apple)" "1 4 "
expect "prefix: App*"                 "$(ids vf_cs vf_cs_live 'App*')" "1 4 "
expect "phrase keeps the case"        "$(ids vf_cs vf_cs_live '"US Army"')" "5 "
expect "US is not us"                 "$(ids vf_cs vf_cs_live 'us' exact)" "5 "
expect "the default index folds case" "$(ids vf_ci vf_ci_live Apple)" "1 2 3 4 "
expect "tsnippet marks the word in its case only" \
       "$(q "SELECT vfts.tsnippet(body, 'Apple' USING PARAMETERS index='vf_cs') FROM vf_cs.docs WHERE doc_id IN (1, 2) ORDER BY doc_id" | tr '\n' '|')" \
       "<b>Apple</b> released a new phone|an apple a day|"
expect "tokenize with case_sensitive" \
       "$(q "SELECT term || ':' || stem FROM (SELECT vfts.tokenize('Apples APPLES' USING PARAMETERS lang='en', case_sensitive=true) OVER()) t ORDER BY pos" | tr '\n' ' ')" \
       "Apples:Appl APPLES:APPL "
q "INSERT INTO vf_cs.docs VALUES (6, 'Apple Watch'); COMMIT;" > /dev/null
expect "live tail analyzed with the index's case" "$(ids vf_cs vf_cs_live Apple exact)" "1 6 "
q "CALL vfts.refresh_index('vf_cs')" > /dev/null
expect "after a refresh too" "$(ids vf_cs vf_cs_snap Apple exact)" "1 6 "
q "CALL vfts.export_index('vf_cs'); CALL vfts.drop_index('vf_cs');" > /dev/null
q "CALL vfts.import_index('vf_cs', 'vf_cs.vf_cs_vfts_export')" > /dev/null
expect "export and import keep case=sensitive" "$(q "SELECT case_sensitive FROM vfts.indexes WHERE index_name = 'vf_cs'") $(ids vf_cs vf_cs_live Apple exact)" "t 1 6 "
expect_error "case must be sensitive or insensitive" "CALL vfts.create_index('vf_cx', 'vf_cs.docs', 'case=upper')" "case must be"

cleanup
done_test
