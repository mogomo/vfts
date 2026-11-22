#!/usr/bin/env python3
"""Plan-C test C12: Elasticsearch / OpenSearch time per query, split by its profile API.

    python3 -I bench/plan_c/es_profile.py <es|os> <index> <queries.tsv> <out.tsv> [--mode stem|exact] [--lang en] [--repeat 5]

Every query of the file is sent with "profile": true (k = 10, after one unprofiled warm-up run),
R times; for each query the run with the smallest total is kept. Writes per query:
qid, took ms, rewrite ns, query ns (sum of the top-level query nodes), collector ns.
The profile instruments every scorer call, so these times are higher than unprofiled ones; they
show the split (rewrite = expansion of fuzzy / prefix / wildcard terms, query = matching and
scoring), not the speed.
"""
import importlib.util
import os
import subprocess
import sys


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("run_engines", os.path.join(here, "..", "run_engines.py"))
re_ = importlib.util.module_from_spec(spec)
spec.loader.exec_module(re_)
import json  # noqa: E402


def main():
    engine, index, qpath, out_path = sys.argv[1:5]
    mode = arg("--mode", "stem")
    lang = arg("--lang", "en")
    repeat = int(arg("--repeat", 5))
    conn = re_.Conn(re_.PORTS[engine])
    queries = [l.rstrip("\n").split("\t", 1) for l in open(qpath, encoding="utf-8") if "\t" in l]
    out = subprocess.run(["build/tools/vfts_cli", "explain", qpath, "--lang", lang], capture_output=True, text=True, check=True).stdout
    tr = re_.Translator(conn, index, mode)
    trees = [json.loads(l) for l in out.splitlines()]
    bodies = [{"match_none": {}} if t["tree"]["k"] == "error" else tr.query(t["tree"]) for t in trees]
    for b in bodies:
        conn.post(f"/{index}/_search", {"query": b, "size": 10, "_source": False, "track_total_hits": False})
    best = {}
    for r in range(repeat):
        for (qid, _), b in zip(queries, bodies):
            st, res = conn.post(f"/{index}/_search", {"query": b, "size": 10, "_source": False, "track_total_hits": False, "profile": True})
            rw = qn = cn = 0
            for sh in res.get("profile", {}).get("shards", []):
                for s in sh.get("searches", []):
                    rw += s.get("rewrite_time", 0)
                    qn += sum(q.get("time_in_nanos", 0) for q in s.get("query", []))
                    cn += sum(c.get("time_in_nanos", 0) for c in s.get("collector", []))
            row = (res.get("took", 0), rw, qn, cn)
            if qid not in best or rw + cn < best[qid][1] + best[qid][3]:
                best[qid] = row
    with open(out_path, "w") as f:
        for qid, _ in queries:
            t = best[qid]
            f.write(f"{qid}\t{t[0]}\t{t[1]}\t{t[2]}\t{t[3]}\n")
    print(json.dumps({"engine": engine, "index": index, "queries": len(queries), "out": out_path}))


if __name__ == "__main__":
    main()
