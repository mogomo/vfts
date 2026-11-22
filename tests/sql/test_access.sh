#!/usr/bin/env bash
# Access (docs/design.md "Access"): who may search an index, and what row access policies and
# column masks of the source table do to searches.
#   - users get the search functions only; the functions that write node caches are refused
#   - a statement that makes its own input (not the index's view) is refused
#   - row access policy: each user's results (ids, ranks, scores) equal those of an index built
#     from the rows that user sees; leaving out the view's H rows or reusing another user's input
#     is refused
#   - column mask on the text column: searches are refused
#   - check_access makes a new key: a token read before stops working
# Creates users vf_ac_a, vf_ac_b, vf_ac_x and schema vf_ac; drops them at the end.
set -uo pipefail
cd "$(dirname "$0")"; source ./lib.sh
echo "$TEST_NAME"

VOCAB="contract indemnity holmes watson agreement breach party clause notice term liability damages court settlement payment"
USERS="vf_ac_a vf_ac_b vf_ac_x"
cleanup() {
    for i in vf_ac vf_ac_ref_a vf_ac_ref_b; do q "CALL vfts.drop_index('$i')" > /dev/null 2>&1; done
    q "DROP SCHEMA IF EXISTS vf_ac CASCADE" > /dev/null 2>&1
    for u in $USERS; do q "DROP USER IF EXISTS $u CASCADE" > /dev/null 2>&1; done
}
# as USER 'SQL'    one statement as that user (rows like q; errors on stdout)
as() { local u=$1; shift; timeout "$TIMEOUT" env VSQL_USER="$u" VSQL_PASSWORD="Pw_${u}_9" vsql -X -A -t -q -v ON_ERROR_STOP=1 -c "$1" 2>&1; }
expect_error_as() {   # what user 'SQL' "part of the message"
    local out
    out=$(as "$2" "$3") && { echo "  FAIL $1 (no error: $(echo "$out" | head -3 | tr '\n' ' '))"; FAILS=$((FAILS + 1)); return; }
    if echo "$out" | grep -qiF -- "$4"; then echo "  ok   $1"
    else echo "  FAIL $1"; echo "       got:  $out"; echo "       want an error containing: $4"; FAILS=$((FAILS + 1)); fi
}
cleanup
q "CREATE SCHEMA vf_ac;
   CREATE TABLE vf_ac.docs (doc_id INT NOT NULL, owner VARCHAR(32), body LONG VARCHAR, ver TIMESTAMPTZ DEFAULT CLOCK_TIMESTAMP());" > /dev/null
gen_docs() {   # first last seed
    q "INSERT INTO vf_ac.docs (doc_id, owner, body)
       SELECT doc_id, CASE WHEN MOD(doc_id, 3) = 0 THEN 'vf_ac_b' ELSE 'vf_ac_a' END, LISTAGG(w USING PARAMETERS separator=' ', max_length=8000)
       FROM (SELECT d.k AS doc_id, SPLIT_PART('$VOCAB', ' ', (1 + MOD(HASH(d.k, p.k, $3) // 5, 15))::INT) AS w
             FROM vfts.probe d JOIN vfts.probe p ON p.k <= 4 + MOD(HASH(d.k, $3) // 13, 40)
             WHERE d.k BETWEEN $1 AND $2) x
       GROUP BY doc_id; COMMIT;" > /dev/null
}
gen_docs 1 600 7
q "CALL vfts.create_index('vf_ac', 'vf_ac.docs', 'margin=0')" > /dev/null
for u in $USERS; do
    q "CREATE USER $u IDENTIFIED BY 'Pw_${u}_9'; GRANT USAGE ON SCHEMA vf_ac TO $u;" > /dev/null
done
for u in vf_ac_a vf_ac_b; do q "GRANT SELECT ON vf_ac.vf_ac_live, vf_ac.vf_ac_snap, vf_ac.docs TO $u" > /dev/null; done

S() { echo "SELECT doc_id || ':' || rank || ':' || ROUND(score, 6) FROM (SELECT vfts.tsearch(* USING PARAMETERS index='$1', query='$2', k=40) OVER() FROM $3) r ORDER BY rank"; }
QUERIES=("holmes" "contract indemnity" "watson AND breach" "settlement OR holmes -court" "\"contract indemnity\"" "agree*")
DUAL="(SELECT NULL::INT AS qid, NULL::VARCHAR AS query, NULL::INT AS doc_id, NULL::LONG VARCHAR AS body, NULL::VARCHAR AS lang, NULL::VARCHAR AS op, 1::INT AS gen) d"

echo "-- privileges"
expect_error_as "internal function tdrop refused to a user" vf_ac_x \
    "SELECT * FROM (SELECT vfts.tdrop(USING PARAMETERS index='vf_ac') OVER(PARTITION NODES) FROM vfts.probe) d" "permission"
expect_error_as "a user without a grant cannot read the view" vf_ac_x "SELECT COUNT(*) FROM vf_ac.vf_ac_snap" "denied"
expect_error_as "input made by the statement (no token) refused" vf_ac_x "$(S vf_ac holmes "$DUAL")" "not a view"
TOK=$(q "SELECT query FROM vf_ac.vf_ac_snap WHERE doc_id IS NULL")
G=$(q "SELECT gen FROM vfts.indexes WHERE index_name = 'vf_ac'")
FAKE="(SELECT NULL::INT AS qid, 'aaaabbbbccccddddeeeeffff00001111'::VARCHAR AS query, NULL::INT AS doc_id, NULL::LONG VARCHAR AS body, NULL::VARCHAR AS lang, NULL::VARCHAR AS op, $G::INT AS gen) d"
expect_error_as "input with a made-up token refused" vf_ac_x "$(S vf_ac holmes "$FAKE")" "wrong access token"
expect "open: a user with the grant gets the owner's results" "$(as vf_ac_a "$(S vf_ac holmes vf_ac.vf_ac_snap)")" "$(q "$(S vf_ac holmes vf_ac.vf_ac_snap)")"
expect "access mode open" "$(q "SELECT access FROM vfts.indexes WHERE index_name = 'vf_ac'")" "open"
expect "a user's session default (vfts_k) reaches the search" \
    "$(as vf_ac_a "ALTER SESSION SET UDPARAMETER vfts_k = '3'; SELECT COUNT(*) FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_ac', query='holmes') OVER() FROM vf_ac.vf_ac_snap) r")" "3"

echo "-- revoke: a token read before works until check_access makes a new key"
STOLEN="(SELECT NULL::INT AS qid, '$TOK'::VARCHAR AS query, NULL::INT AS doc_id, NULL::LONG VARCHAR AS body, NULL::VARCHAR AS lang, NULL::VARCHAR AS op, $G::INT AS gen) d"
expect "a copied token works before check_access (documented)" "$(as vf_ac_x "$(S vf_ac holmes "$STOLEN")" | wc -l | tr -d ' ')" "$(q "$(S vf_ac holmes vf_ac.vf_ac_snap)" | wc -l | tr -d ' ')"
q "CALL vfts.check_access('vf_ac')" > /dev/null
expect_error_as "a copied token is refused after check_access" vf_ac_x "$(S vf_ac holmes "$STOLEN")" "wrong access token"

echo "-- row access policy"
q "CREATE ACCESS POLICY ON vf_ac.docs FOR ROWS WHERE owner = CURRENT_USER() OR CURRENT_USER() NOT LIKE 'vf\\_ac\\_%' ENABLE" > /dev/null
q "CALL vfts.check_access('vf_ac')" > /dev/null
expect "access mode rows" "$(q "SELECT access FROM vfts.indexes WHERE index_name = 'vf_ac'")" "rows"
# references: indexes built from the rows each user sees
for u in a b; do
    q "CREATE TABLE vf_ac.ref_$u AS SELECT doc_id, body FROM vf_ac.docs WHERE owner = 'vf_ac_$u'; COMMIT;" > /dev/null
    q "CALL vfts.create_index('vf_ac_ref_$u', 'vf_ac.ref_$u', 'margin=0')" > /dev/null
done
compare_users() {   # label view
    for u in a b; do
        local bad=0
        for qq in "${QUERIES[@]}"; do
            local got want
            got=$(as vf_ac_$u "$(S vf_ac "$qq" vf_ac.$2)")
            want=$(q "$(S vf_ac_ref_$u "$qq" vf_ac.vf_ac_ref_${u}_snap)")
            if [ "$got" != "$want" ] || [ -z "$want" ]; then bad=$((bad + 1)); echo "       differs ($u): $qq"; fi
        done
        expect "$1: user $u, ${#QUERIES[@]} queries equal an index of the rows $u sees" "$bad" "0"
        expect "$1: user $u, tcount" \
            "$(as vf_ac_$u "SELECT count FROM (SELECT vfts.tcount(* USING PARAMETERS index='vf_ac', query='contract') OVER() FROM vf_ac.$2) r")" \
            "$(q "SELECT count FROM (SELECT vfts.tcount(* USING PARAMETERS index='vf_ac_ref_$u', query='contract') OVER() FROM vf_ac.vf_ac_ref_${u}_snap) r")"
    done
}
compare_users "snap" vf_ac_snap
compare_users "live" vf_ac_live
expect "the owner (no policy for it) still sees every row" "$(q "SELECT count FROM (SELECT vfts.tcount(* USING PARAMETERS index='vf_ac', query='contract') OVER() FROM vf_ac.vf_ac_snap) r")" \
    "$(q "SELECT COUNT(*) FROM vf_ac.docs WHERE body::VARCHAR(65000) ILIKE '%contract%'")"
expect_error_as "the view without its H rows is refused" vf_ac_a \
    "$(S vf_ac holmes "(SELECT * FROM vf_ac.vf_ac_snap WHERE COALESCE(op, '') <> 'H') v")" "wrong access token"
expect_error_as "one hidden doc_id left out of a list is refused" vf_ac_a \
    "$(S vf_ac holmes "(SELECT qid, CASE WHEN op = 'H' THEN REGEXP_REPLACE(query, '^-?[0-9]+,', '') ELSE query END AS query, doc_id, body, lang, op, gen FROM vf_ac.vf_ac_snap) v")" "wrong access token"
# user b replays the complete input of user a (a's H rows and token)
A_IN=$(as vf_ac_a "SELECT COALESCE(query, '') || '|' || COALESCE(op, '') FROM vf_ac.vf_ac_snap WHERE doc_id IS NULL")
REPLAY=$(echo "$A_IN" | awk -F'|' -v g="$(q "SELECT gen FROM vfts.indexes WHERE index_name = 'vf_ac'")" '
    BEGIN { printf "(SELECT NULL::INT AS qid, NULL::VARCHAR AS query, NULL::INT AS doc_id, NULL::LONG VARCHAR AS body, NULL::VARCHAR AS lang, NULL::VARCHAR AS op, %s::INT AS gen", g }
    { printf " UNION ALL SELECT NULL, '\''%s'\'', NULL, NULL, NULL, %s, %s", $1, ($2 == "" ? "NULL" : "'\''" $2 "'\''"), g }
    END { printf ") v" }')
expect "user a's own copied input works for a" "$(as vf_ac_a "$(S vf_ac holmes "$REPLAY")")" "$(as vf_ac_a "$(S vf_ac holmes vf_ac.vf_ac_snap)")"
expect_error_as "user a's input replayed by user b is refused" vf_ac_b "$(S vf_ac holmes "$REPLAY")" "wrong access token"

echo "-- an older version in the segments: a row of b with a word, then redacted and given to a"
q "INSERT INTO vf_ac.docs (doc_id, owner, body) VALUES (9001, 'vf_ac_b', 'zanzibarword hidden draft'); COMMIT;" > /dev/null
q "CALL vfts.refresh_index('vf_ac')" > /dev/null
sleep 1
q "UPDATE vf_ac.docs SET owner = 'vf_ac_a', body = 'redacted note', ver = CLOCK_TIMESTAMP() WHERE doc_id = 9001; COMMIT;" > /dev/null
for v in snap live; do
    expect "the old version is not found by the new owner ($v)" \
        "$(as vf_ac_a "SELECT COUNT(*) FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_ac', query='zanzibarword') OVER() FROM vf_ac.vf_ac_$v) r")" "0"
done
expect "the current version is found by the new owner (live)" \
    "$(as vf_ac_a "SELECT COUNT(*) FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_ac', query='redacted') OVER() FROM vf_ac.vf_ac_live) r")" "1"
q "DELETE FROM vf_ac.docs WHERE doc_id = 9001; COMMIT;" > /dev/null
q "CALL vfts.refresh_index('vf_ac')" > /dev/null

echo "-- refresh with the policy in place: new rows of b, a row of a moved to b"
sleep 1
gen_docs 601 700 8
q "UPDATE vf_ac.docs SET owner = 'vf_ac_b', ver = CLOCK_TIMESTAMP() WHERE doc_id IN (1, 2, 4); COMMIT;" > /dev/null
for u in a b; do
    q "CALL vfts.drop_index('vf_ac_ref_$u'); DROP TABLE vf_ac.ref_$u; CREATE TABLE vf_ac.ref_$u AS SELECT doc_id, body FROM vf_ac.docs WHERE owner = 'vf_ac_$u'; COMMIT;" > /dev/null
    q "CALL vfts.create_index('vf_ac_ref_$u', 'vf_ac.ref_$u', 'margin=0')" > /dev/null
done
compare_users "live tail, before refresh" vf_ac_live
q "CALL vfts.refresh_index('vf_ac')" > /dev/null
compare_users "after refresh" vf_ac_snap
q "CALL vfts.merge('vf_ac', 1)" > /dev/null
compare_users "after merge" vf_ac_snap

echo "-- column mask on the text column"
q "CREATE ACCESS POLICY ON vf_ac.docs FOR COLUMN body CASE WHEN CURRENT_USER() = 'vf_ac_a' THEN body ELSE '***' END ENABLE" > /dev/null
q "CALL vfts.check_access('vf_ac')" > /dev/null
expect "access mode masked:body" "$(q "SELECT access FROM vfts.indexes WHERE index_name = 'vf_ac'")" "masked:body"
expect_error_as "searches refused for a user" vf_ac_b "$(S vf_ac holmes vf_ac.vf_ac_snap)" "column access policy"
expect_error "searches refused for the owner too" "$(S vf_ac holmes vf_ac.vf_ac_snap)" "column access policy"

echo "-- policies dropped: open again"
q "DROP ACCESS POLICY ON vf_ac.docs FOR COLUMN body; DROP ACCESS POLICY ON vf_ac.docs FOR ROWS;" > /dev/null
q "CALL vfts.refresh_index('vf_ac')" > /dev/null
expect "refresh_index follows the policies (open)" "$(q "SELECT access FROM vfts.indexes WHERE index_name = 'vf_ac'")" "open"
expect "user b sees every row again" "$(as vf_ac_b "SELECT count FROM (SELECT vfts.tcount(* USING PARAMETERS index='vf_ac', query='contract') OVER() FROM vf_ac.vf_ac_snap) r")" \
    "$(q "SELECT COUNT(*) FROM vf_ac.docs WHERE body::VARCHAR(65000) ILIKE '%contract%'")"
expect "access=rows option without a policy" "$(q "CALL vfts.drop_index('vf_ac_ref_b'); CALL vfts.create_index('vf_ac_ref_b', 'vf_ac.ref_b', 'margin=0 access=rows')" > /dev/null; q "SELECT access FROM vfts.indexes WHERE index_name = 'vf_ac_ref_b'")" "rows"

cleanup
done_test
