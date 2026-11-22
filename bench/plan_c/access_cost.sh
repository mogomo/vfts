#!/usr/bin/env bash
# What access mode "rows" costs per statement (docs/design.md "Access"), on the Enron index of the
# benchmark: a one-word tsearch through the snap view, as one SQL statement, at 1 and 32 sessions,
#   open      no access policy (the benchmark's mode)
#   rows-all  mode rows (option access=rows), a user who sees every row: no hidden document
#   rows-half a row access policy that hides half of the documents from user vfts_cost
# Afterwards the policy, the user and the option are removed again and the index is open again.
# Results: bench/out/plan_c/results.jsonl (sqlload.py lines, tagged).
#
#   bench/plan_c/access_cost.sh
set -uo pipefail
cd "$(dirname "$0")/../.."
RES=bench/out/plan_c/results.jsonl
mkdir -p bench/out/plan_c
q() { timeout 600 vsql -X -A -t -q -c "$1"; }
SQL="SELECT /*+LABEL({label})*/ doc_id FROM (SELECT vfts.tsearch(* USING PARAMETERS query='fraud', index='enron', mode='stem', k=10) OVER() FROM bench.enron_snap) r"
run() {   # tag user
    local tag=$1 u=$2 pw=${3:-}
    for c in 1 32; do
        local n=$(( c == 1 ? ${N1:-1000} : ${N32:-6000} ))
        out=$(VSQL_USER=$u VSQL_PASSWORD=${pw:-${VSQL_PASSWORD:-}} timeout 1800 python3 -I bench/plan_c/sqlload.py --sql "$SQL" --clients $c --n $n --repeat 2)
        echo "$out" | python3 -I -c 'import json,sys; d=json.loads(sys.stdin.read()); d["test"]=sys.argv[1]; print(json.dumps(d))' "access_${tag}_c$c" | tee -a "$RES" \
            | python3 -I -c 'import json,sys; d=json.loads(sys.stdin.read()); print(d["test"], "server mean ms", d["server_ms"]["mean"], "q/s", d["qps_best"], "errors", d["errors"])'
    done
}
cleanup() {
    q "DROP ACCESS POLICY ON bench.enron FOR ROWS" > /dev/null 2>&1
    q "DROP USER IF EXISTS vfts_cost CASCADE" > /dev/null 2>&1
    q "UPDATE vfts.indexes SET access_opt = 'auto' WHERE index_name = 'enron'; COMMIT;" > /dev/null
    q "CALL vfts.check_access('enron')" > /dev/null
}
cleanup
run open "${VSQL_USER:-dbadmin}"
q "UPDATE vfts.indexes SET access_opt = 'rows' WHERE index_name = 'enron'; COMMIT;" > /dev/null
q "CALL vfts.check_access('enron')"
run rows_all "${VSQL_USER:-dbadmin}"
q "CREATE USER vfts_cost IDENTIFIED BY 'Cost_pw_1'; GRANT USAGE ON SCHEMA bench TO vfts_cost; GRANT SELECT ON bench.enron_snap TO vfts_cost;" > /dev/null
q "CREATE ACCESS POLICY ON bench.enron FOR ROWS WHERE MOD(doc_id, 2) = 0 OR CURRENT_USER() <> 'vfts_cost' ENABLE" > /dev/null
q "CALL vfts.check_access('enron')"
echo "hidden for vfts_cost: $(VSQL_USER=vfts_cost VSQL_PASSWORD=Cost_pw_1 vsql -X -A -t -c "SELECT SUM(REGEXP_COUNT(query, ',') + 1) FROM bench.enron_snap WHERE op = 'H'")"
run rows_half vfts_cost Cost_pw_1
cleanup
echo "access now: $(q "SELECT access FROM vfts.indexes WHERE index_name = 'enron'")"
