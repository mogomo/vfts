#!/usr/bin/env bash
# Refresh time for 1% new rows: rows are added to the source table (copies of existing documents
# with new ids), then searched without refresh (the live tail), then vtext.refresh_index makes them
# a new segment. Prints JSON lines. With an engine running (es|os as second argument) the same
# documents are indexed there with _bulk and _refresh, for comparison.
#
#   bench/refresh_test.sh <dataset> [es|os]
set -euo pipefail
cd "$(dirname "$0")/.."
DS=$1
ENGINE=${2:-}
Q() { timeout 7200 vsql -X -A -t -q -v ON_ERROR_STOP=1 -c "$1"; }
N=$(Q "SELECT COUNT(*) FROM bench.$DS")
ADD=$((N / 100))
MAXID=$(Q "SELECT MAX(doc_id) FROM bench.$DS")
if [ -z "$ENGINE" ]; then
    # the new rows: the first ADD documents again, with ids above the largest one
    Q "INSERT /*+DIRECT*/ INTO bench.$DS (doc_id, lang, body) SELECT doc_id + $MAXID + 1, lang, body FROM bench.$DS WHERE doc_id IN
       (SELECT doc_id FROM bench.$DS ORDER BY doc_id LIMIT $ADD); COMMIT;" > /dev/null
    sleep 1
    # a search over the live tail (the new rows are analyzed at query time)
    t0=$(date +%s.%N)
    Q "SELECT COUNT(*) FROM (SELECT vtext.tsearch(* USING PARAMETERS index='$DS', query='what is the population of canada', syntax='plain', k=10) OVER() FROM bench.${DS}_live) r" > /dev/null
    t1=$(date +%s.%N)
    Q "CALL vtext.refresh_index('$DS')" > /dev/null
    t2=$(date +%s.%N)
    Q "SELECT COUNT(*) FROM (SELECT vtext.tsearch(* USING PARAMETERS index='$DS', query='what is the population of canada', syntax='plain', k=10) OVER() FROM bench.${DS}_live) r" > /dev/null
    t3=$(date +%s.%N)
    note=$(Q "SELECT refresh_note FROM vtext.indexes WHERE index_name = '$DS'")
    printf '{"engine": "vtext", "dataset": "%s", "added": %s, "query_with_tail_ms": %.1f, "refresh_s": %.1f, "query_after_ms": %.1f, "note": "%s"}\n' \
        "$DS" "$ADD" "$(echo "($t1 - $t0) * 1000" | bc)" "$(echo "$t2 - $t1" | bc)" "$(echo "($t3 - $t2) * 1000" | bc)" "$note"
else
    # the same new documents for Elasticsearch / OpenSearch: bulk them, then refresh
    python3 -I - "$ENGINE" "$DS" "$ADD" "$MAXID" <<'EOF'
import http.client, json, os, sys, time
engine, ds, add, maxid = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
port = int(os.environ.get("ES_PORT", 9200)) if engine == "es" else int(os.environ.get("OS_PORT", 9201))
path = os.path.join(os.environ.get("VTEXT_DATA", os.path.expanduser("~/vtext_data")), ds, "docs.dat")
docs = []
with open(path, "rb") as f:
    buf = b""
    while len(docs) < add:
        chunk = f.read(1 << 20)
        if not chunk: break
        buf += chunk
        while len(docs) < add:
            i = buf.find(b"\x02")
            if i < 0: break
            rec, buf = buf[:i], buf[i + 1:]
            d, _, rest = rec.partition(b"\x01"); _, _, body = rest.partition(b"\x01")
            docs.append((int(d), body.decode("utf-8")))
docs.sort()
c = http.client.HTTPConnection("127.0.0.1", port, timeout=3600)
t0 = time.time()
for s in range(0, len(docs), 5000):
    lines = []
    for d, body in docs[s:s + 5000]:
        nid = d + maxid + 1
        lines.append(json.dumps({"index": {"_id": str(nid)}})); lines.append(json.dumps({"doc_id": nid, "body": body}, ensure_ascii=False))
    c.request("POST", f"/{ds}/_bulk", body=("\n".join(lines) + "\n").encode(), headers={"Content-Type": "application/x-ndjson"})
    r = c.getresponse(); r.read()
c.request("POST", f"/{ds}/_refresh"); c.getresponse().read()
print(json.dumps({"engine": engine, "dataset": ds, "added": len(docs), "refresh_s": round(time.time() - t0, 1)}))
EOF
fi
