#!/usr/bin/env bash
# vtext.tserve through tools/vtext_broker: the broker's answers (serve and sql modes) equal
# tsearch's, a tsearch batch (parameter queries) equals the queries one by one, and tserve refuses
# an endpoint that is not a broker's socket. Needs build/tools/vtext_broker (make tools) and curl.
set -uo pipefail
cd "$(dirname "$0")"; source ./lib.sh
echo "$TEST_NAME"
BROKER=../../build/tools/vtext_broker
[ -x "$BROKER" ] || { echo "  FAIL $BROKER missing (make tools)"; exit 1; }

q "CALL vtext.drop_index('vt_sv')" > /dev/null 2>&1
q "DROP SCHEMA IF EXISTS vt_sv CASCADE" > /dev/null
q "CREATE SCHEMA vt_sv;
   CREATE TABLE vt_sv.docs (doc_id INT NOT NULL, body LONG VARCHAR);
   INSERT INTO vt_sv.docs SELECT k, 'alpha ' || CASE WHEN MOD(k, 3) = 0 THEN 'bravo charlie ' ELSE 'delta ' END ||
       CASE WHEN MOD(k, 7) = 0 THEN 'echo echo ' ELSE '' END || 'doc' || k::VARCHAR FROM vtext.probe;
   COMMIT;" > /dev/null
q "CALL vtext.create_index('vt_sv', 'vt_sv.docs', '')" > /dev/null

QUERIES=('alpha' 'bravo OR echo' '"bravo charlie"' 'echo w/2 alpha' 'doc1*' 'charlie AND NOT echo')
ts() { q "SELECT doc_id || ' ' || ROUND(score, 4) FROM (SELECT vtext.tsearch(* USING PARAMETERS index='vt_sv', query='$1', k=5) OVER() FROM vt_sv.vt_sv_snap) r ORDER BY score DESC, doc_id"; }
via() {   # port query -> "doc score" lines
    curl -s -m 30 -XPOST "localhost:$1/vt_sv/_search" -d "{\"query\": \"$(printf '%s' "$2" | sed 's/\\/\\\\/g; s/"/\\"/g')\"}" |
        python3 -c 'import json,sys; d=json.load(sys.stdin); [print(h["id"], "%.4f" % h["score"]) for h in sorted(d["hits"], key=lambda h: (-h["score"], h["id"]))]'
}
for mode in serve sql; do
    port=$((19400 + RANDOM % 500))
    "$BROKER" --port "$port" --schema vt_sv --mode "$mode" --k 5 > /tmp/vt_sv_broker.$$.log 2>&1 &
    bp=$!
    sleep 1
    for qq in "${QUERIES[@]}"; do
        expect "$mode: $qq" "$(via "$port" "$qq")" "$(ts "$qq" | sed 's/\.\([0-9]*\)$/.\1/' | awk '{printf "%s %.4f\n", $1, $2}')"
    done
    kill $bp 2> /dev/null; wait $bp 2> /dev/null
done

# a batch gives what the queries give one by one
B=$(q "SELECT qid || ' ' || doc_id || ' ' || ROUND(score, 4) FROM (SELECT vtext.tsearch(* USING PARAMETERS index='vt_sv', k=5,
       queries=E'1\talpha\n2\tbravo OR echo\n3\t\"bravo charlie\"\n4\techo w/2 alpha') OVER() FROM vt_sv.vt_sv_snap) r ORDER BY qid, score DESC, doc_id")
O=$(i=0; for qq in 'alpha' 'bravo OR echo' '"bravo charlie"' 'echo w/2 alpha'; do i=$((i + 1)); ts "$qq" | sed "s/^/$i /"; done)
expect "batch equals single queries" "$B" "$O"

# tmatch: every match, the same documents as tsearch with a large k
for qq in 'alpha' 'bravo OR echo' '"bravo charlie"' 'echo w/2 alpha' 'doc1*'; do
    expect "tmatch: $qq" "$(q "SELECT doc_id FROM (SELECT vtext.tmatch(* USING PARAMETERS index='vt_sv', query='$qq') OVER() FROM vt_sv.vt_sv_live) r ORDER BY 1")" \
        "$(q "SELECT doc_id FROM (SELECT vtext.tsearch(* USING PARAMETERS index='vt_sv', query='$qq', k=1000000) OVER() FROM vt_sv.vt_sv_live) r ORDER BY 1")"
done

expect_error "endpoint must be a broker socket" "SELECT vtext.tserve(* USING PARAMETERS index='vt_sv', endpoint='/tmp/other.sock', token='0123456789abcdef0123') OVER() FROM vt_sv.vt_sv_snap" "endpoint must be the socket of vtext_broker"
expect_error "token required" "SELECT vtext.tserve(* USING PARAMETERS index='vt_sv', endpoint='/tmp/vtext_broker.x.sock', token='short') OVER() FROM vt_sv.vt_sv_snap" "token"

q "CALL vtext.drop_index('vt_sv')" > /dev/null
q "DROP SCHEMA vt_sv CASCADE" > /dev/null
rm -f /tmp/vt_sv_broker.$$.log
done_test
