#!/usr/bin/env bash
# Plan-C test C13: what Vertica's access rules do to vtext's answers (facts for the broker design).
# Creates schema c13, users c13_a / c13_b, a row access policy and a column mask on the source
# table, searches as each user (snap, live tail, joined to the source), checks labels in the
# monitoring tables, and drops everything again. Prints what it sees; nothing is asserted.
#
#   bench/plan_c/c13_security.sh
set -uo pipefail
q() { timeout 300 vsql -X -A -t -q -c "$1" 2>&1; }
as() { local u=$1; shift; timeout 300 env VSQL_USER="$u" VSQL_PASSWORD="Pw_${u}_1" vsql -X -A -t -q -c "$1" 2>&1; }
cleanup() {
    q "CALL vtext.drop_index('c13docs')" > /dev/null
    q "DROP SCHEMA IF EXISTS c13 CASCADE" > /dev/null
    q "DROP USER IF EXISTS c13_a" > /dev/null
    q "DROP USER IF EXISTS c13_b" > /dev/null
}
cleanup
q "CREATE SCHEMA c13"
q "CREATE TABLE c13.docs (doc_id INT NOT NULL, owner VARCHAR(32), body LONG VARCHAR, lang VARCHAR(8), ver TIMESTAMPTZ NOT NULL DEFAULT CLOCK_TIMESTAMP())"
q "INSERT INTO c13.docs (doc_id, owner, body) SELECT 1, 'c13_a', 'secret merger plan alpha' UNION ALL SELECT 2, 'c13_b', 'secret merger plan beta' UNION ALL SELECT 3, 'c13_a', 'weekly lunch menu' UNION ALL SELECT 4, 'c13_b', 'merger rumours'; COMMIT"
q "CALL vtext.create_index('c13docs', 'c13.docs', 'lang=en margin=0')" | tail -4
for u in c13_a c13_b; do
    q "CREATE USER $u IDENTIFIED BY 'Pw_${u}_1'"
    q "GRANT USAGE ON SCHEMA c13 TO $u"
    q "GRANT SELECT ON ALL TABLES IN SCHEMA c13 TO $u"
    q "GRANT USAGE ON SCHEMA vtext TO $u"
    q "GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA vtext TO $u"
done
# a row after the index was built: it is in the live tail (inserted before the row policy, which
# would also hide it from the inserting user)
q "INSERT INTO c13.docs (doc_id, owner, body) VALUES (5, 'c13_a', 'merger signed'); COMMIT"
q "CREATE ACCESS POLICY ON c13.docs FOR ROWS WHERE owner = CURRENT_USER() ENABLE"
q "CREATE ACCESS POLICY ON c13.docs FOR COLUMN body CASE WHEN CURRENT_USER() = 'c13_a' THEN body ELSE 'masked text' END ENABLE"
echo "== access policies in the catalog"
q "SELECT table_name, policy_type, column_name, expression, is_policy_enabled FROM v_catalog.access_policy WHERE table_name ILIKE '%docs%'"
S="SELECT vtext.tsearch(* USING PARAMETERS index='c13docs', query='merger', k=10) OVER()"
for u in c13_a c13_b; do
    echo "== as $u: rows of the source table"
    as $u "SELECT doc_id, body FROM c13.docs ORDER BY 1"
    echo "== as $u: tsearch snap (doc_id, score)"
    as $u "SELECT doc_id, ROUND(score, 4) FROM ($S FROM c13.c13docs_snap) r ORDER BY 1"
    echo "== as $u: tsearch live"
    as $u "SELECT doc_id, ROUND(score, 4) FROM ($S FROM c13.c13docs_live) r ORDER BY 1"
    echo "== as $u: tsearch live joined to the source table"
    as $u "SELECT r.doc_id, d.body FROM ($S FROM c13.c13docs_live) r JOIN c13.docs d USING (doc_id) ORDER BY 1"
done
echo "== labels: client label and statement label in the monitoring tables"
as c13_a "SELECT SET_CLIENT_LABEL('broker:c13_a'); SELECT /*+LABEL(c13_probe)*/ COUNT(*) FROM c13.docs;" > /dev/null
q "SELECT user_name, request_label, LEFT(request, 40) FROM v_monitor.query_requests WHERE request_label = 'c13_probe'"
q "SELECT user_name, client_label, label, LEFT(request, 40) FROM dc_requests_issued WHERE label = 'c13_probe' OR client_label LIKE 'broker:%' ORDER BY time DESC LIMIT 3"
cleanup
echo "== cleaned up"
