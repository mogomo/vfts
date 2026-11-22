#!/usr/bin/env python3
"""Request file for tools/httpload from a query file (qid<TAB>query), for tools/vfts_broker.

    python3 -I bench/plan_c/mkreq.py <index> <queries.tsv> <out.req> [--min N]

One line per query: "/<index>/_search<TAB>{"query": ...}"; the queries are cycled until there
are at least N lines (default: the number of queries).
"""
import json
import sys

index, src, out = sys.argv[1], sys.argv[2], sys.argv[3]
n = int(sys.argv[sys.argv.index("--min") + 1]) if "--min" in sys.argv else 0
qs = [l.rstrip("\n").split("\t", 1)[1] for l in open(src, encoding="utf-8") if "\t" in l]
with open(out, "w", encoding="utf-8") as f:
    for i in range(max(n, len(qs))):
        f.write(f"/{index}/_search\t" + json.dumps({"query": qs[i % len(qs)]}) + "\n")
