#!/usr/bin/env bash
# Grants (vfts 0.5.0, docs/design.md "Access"): with grants=source (the default) the views follow
# the SELECT grants of the source table: whoever may read the table may search it (directly, through
# a chain of roles, through PUBLIC), nobody else; after a REVOKE on the table and a refresh (or
# check_access) the user is refused, also with a token read before. grants=manual leaves the views'
# grants alone. Creates users vf_gr_a .. vf_gr_d, roles vf_gr_r1, vf_gr_r2 and schema vf_gr; drops
# them at the end.
set -uo pipefail
cd "$(dirname "$0")"; source ./lib.sh
echo "$TEST_NAME"

USERS="vf_gr_a vf_gr_b vf_gr_c vf_gr_d"
cleanup() {
    q "CALL vfts.drop_index('vf_gr')" > /dev/null 2>&1
    q "DROP SCHEMA IF EXISTS vf_gr CASCADE" > /dev/null 2>&1
    for u in $USERS; do q "DROP USER IF EXISTS $u CASCADE" > /dev/null 2>&1; done
    for r in vf_gr_r2 vf_gr_r1; do q "DROP ROLE IF EXISTS $r CASCADE" > /dev/null 2>&1; done
}
as() { local u=$1; shift; timeout "$TIMEOUT" env VSQL_USER="$u" VSQL_PASSWORD="Pw_${u}_9" vsql -X -A -t -q -v ON_ERROR_STOP=1 -c "$1" 2>&1; }
expect_error_as() {   # what user 'SQL' "part of the message"
    local out
    out=$(as "$2" "$3") && { echo "  FAIL $1 (no error: $(echo "$out" | head -3 | tr '\n' ' '))"; FAILS=$((FAILS + 1)); return; }
    if echo "$out" | grep -qiF -- "$4"; then echo "  ok   $1"
    else echo "  FAIL $1"; echo "       got:  $out"; echo "       want an error containing: $4"; FAILS=$((FAILS + 1)); fi
}
cleanup
q "CREATE SCHEMA vf_gr;
   CREATE TABLE vf_gr.docs (doc_id INT NOT NULL, body LONG VARCHAR);
   INSERT INTO vf_gr.docs VALUES (1, 'contract with indemnity');
   INSERT INTO vf_gr.docs VALUES (2, 'holmes and watson');
   INSERT INTO vf_gr.docs VALUES (3, 'another contract');
   COMMIT;" > /dev/null
for u in $USERS; do q "CREATE USER $u IDENTIFIED BY 'Pw_${u}_9'; GRANT USAGE ON SCHEMA vf_gr TO $u;" > /dev/null; done
# vf_gr_a: SELECT on the table; vf_gr_c: through role vf_gr_r2, which holds role vf_gr_r1, which has SELECT
q "GRANT SELECT ON vf_gr.docs TO vf_gr_a;
   CREATE ROLE vf_gr_r1; CREATE ROLE vf_gr_r2; GRANT vf_gr_r1 TO vf_gr_r2; GRANT vf_gr_r2 TO vf_gr_c;
   ALTER USER vf_gr_c DEFAULT ROLE vf_gr_r2; GRANT SELECT ON vf_gr.docs TO vf_gr_r1;" > /dev/null 2>&1
q "CALL vfts.create_index('vf_gr', 'vf_gr.docs')" > /dev/null

S="SELECT COUNT(*) FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_gr', query='contract') OVER() FROM vf_gr.vf_gr_live) r"
expect "SELECT on the table: may search, without a grant on the views" "$(as vf_gr_a "$S")" "2"
expect "SELECT through a chain of roles: may search" "$(as vf_gr_c "$S")" "2"
expect_error_as "no SELECT on the table: refused" vf_gr_b "$S" "permission denied"
expect "the views are granted to the role, not to its members" \
       "$(q "SELECT grantee FROM v_catalog.grants WHERE object_name = 'vf_gr_live' AND grantee LIKE 'vf_gr%' ORDER BY 1" | tr '\n' ' ')" "vf_gr_a vf_gr_r1 "

# a grant on the views alone (not on the table) is taken back by the next refresh
q "GRANT SELECT ON vf_gr.vf_gr_live, vf_gr.vf_gr_snap TO vf_gr_d" > /dev/null
q "CALL vfts.refresh_index('vf_gr')" > /dev/null
expect_error_as "a grant on the views without SELECT on the table: taken back by refresh_index" vf_gr_d "$S" "permission denied"

# REVOKE on the table: refused after the next refresh, also with the token read before
TOK=$(as vf_gr_a "SELECT query FROM vf_gr.vf_gr_snap WHERE doc_id IS NULL")
G=$(q "SELECT gen FROM vfts.indexes WHERE index_name = 'vf_gr'")
OLD="(SELECT NULL::INT AS qid, '$TOK'::VARCHAR AS query, NULL::INT AS doc_id, NULL::LONG VARCHAR AS body, NULL::VARCHAR AS lang, NULL::VARCHAR AS op, $G::INT AS gen) d"
expect "the token works before the REVOKE" \
       "$(as vf_gr_a "SELECT COUNT(*) FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_gr', query='contract') OVER() FROM $OLD) r")" "2"
q "REVOKE SELECT ON vf_gr.docs FROM vf_gr_a" > /dev/null
q "CALL vfts.refresh_index('vf_gr')" > /dev/null
expect_error_as "after REVOKE on the table and a refresh: refused" vf_gr_a "$S" "permission denied"
expect_error_as "after REVOKE: the token read before is refused" vf_gr_a \
    "SELECT COUNT(*) FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_gr', query='contract') OVER() FROM $OLD) r" "access token"
expect "the others still search" "$(as vf_gr_c "$S")" "2"

# check_access follows grants at once; PUBLIC counts
q "GRANT SELECT ON vf_gr.docs TO vf_gr_b" > /dev/null
q "CALL vfts.check_access('vf_gr')" > /dev/null
expect "GRANT on the table, then check_access: may search" "$(as vf_gr_b "$S")" "2"
q "REVOKE SELECT ON vf_gr.docs FROM vf_gr_b; GRANT SELECT ON vf_gr.docs TO PUBLIC" > /dev/null
q "CALL vfts.refresh_index('vf_gr')" > /dev/null
expect "SELECT granted to PUBLIC: every user with USAGE on the schema may search" "$(as vf_gr_b "$S") $(as vf_gr_a "$S")" "2 2"
q "REVOKE SELECT ON vf_gr.docs FROM PUBLIC" > /dev/null
q "CALL vfts.refresh_index('vf_gr')" > /dev/null
expect_error_as "REVOKE from PUBLIC: refused again" vf_gr_b "$S" "permission denied"

# grants=manual: the views' grants are left alone
q "CALL vfts.alter_index('vf_gr', 'grants=manual')" > /dev/null
q "GRANT SELECT ON vf_gr.vf_gr_live, vf_gr.vf_gr_snap TO vf_gr_d" > /dev/null
q "CALL vfts.refresh_index('vf_gr')" > /dev/null
expect "grants=manual: a grant on the views stays" "$(as vf_gr_d "$S")" "2"
expect_error "alter_index refuses an unknown option" "CALL vfts.alter_index('vf_gr', 'colour=red')" "unknown"

cleanup
done_test
