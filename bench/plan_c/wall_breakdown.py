#!/usr/bin/env python3
"""Where do J3 and J4 (bench/join_test.py) spend their time? One session, vsql \\timing, every
query of a set through a ladder of statements, each one step closer to the full J3 / J4:

  s0  SELECT 1                                         the statement floor
  s1  tcount                                           the engine finds every match, one row out
  s2  COUNT(*) over tmatch                             every match leaves the UDx as a row
  s3  J4: tmatch JOIN meta GROUP BY custodian          + the join and the aggregation
  s4  tsearch k=10                                     a plain top 10
  s5  COUNT(*) over tsearch k=1000000                  every match scored and sent as a row
  s6  J3: tsearch k=1000000 JOIN meta JOIN hold, top 10

Per statement the best of R runs after a warm-up, then the mean over the queries. Needs the tables
bench.<ds>_meta and bench.<ds>_hold that join_test.py creates and leaves in place. Prints one
JSON line.

usage: wall_breakdown.py [--dataset enron] [--queries FILE] [--plain] [--limit N] [--repeat 3]
  --queries FILE  qid<TAB>query (default bench/queries/<dataset>.tsv)
  --plain         syntax='plain' (MS MARCO questions)
  --limit N       the first N queries only
"""
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from join_test import arg, lit, queries, vsql_timed  # noqa: E402

ds = arg("--dataset", "enron")
R = int(arg("--repeat", "3"))
qs = queries(arg("--queries", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "queries", f"{ds}.tsv")))
qs = qs[:int(arg("--limit", len(qs)))]
p = "mode='stem', max_expansions=100000, bm25='classic'" + (", syntax='plain'" if "--plain" in sys.argv else "")
src = f"bench.{ds}_snap"


def u(fn, q, extra=""):
    return f"(SELECT vfts.{fn}(* USING PARAMETERS index='{ds}', query={lit(q)}{extra}, {p}) OVER() FROM {src}) r"


ladder = {
    "s0_select1": lambda q: "SELECT 1;",
    "s1_tcount": lambda q: f"SELECT count FROM {u('tcount', q)};",
    "s2_tmatch_rows": lambda q: f"SELECT COUNT(*) FROM {u('tmatch', q)};",
    "s3_j4": lambda q: (f"SELECT m.custodian, COUNT(*) FROM {u('tmatch', q)} JOIN bench.{ds}_meta m USING (doc_id) "
                        f"GROUP BY m.custodian ORDER BY m.custodian;"),
    "s4_tsearch_k10": lambda q: f"SELECT doc_id, score FROM {u('tsearch', q, ', k=10')};",
    "s5_tsearch_all_rows": lambda q: f"SELECT COUNT(*) FROM {u('tsearch', q, ', k=1000000')};",
    "s6_j3": lambda q: (f"SELECT r.doc_id, r.score FROM {u('tsearch', q, ', k=1000000')} JOIN bench.{ds}_meta m USING (doc_id) "
                        f"JOIN bench.{ds}_hold h USING (custodian) ORDER BY r.score DESC, r.doc_id LIMIT 10;"),
}

res = {"dataset": ds, "queries": len(qs), "repeat": R, "plain": "--plain" in sys.argv}
for name, mk in ladder.items():
    stmts = [mk(q) for _, q in qs]
    vsql_timed(stmts)
    best, outs = [1e18] * len(stmts), None
    for _ in range(R):
        o, ms = vsql_timed(stmts)
        outs = outs or o
        best = [min(b, m) if m is not None else b for b, m in zip(best, ms)]
    res[name + "_ms"] = round(sum(best) / len(best), 2)
    if name == "s1_tcount":
        n = [int(o[0]) for o in outs]
        res["matches_mean"] = round(sum(n) / len(n))
        res["matches_max"] = max(n)
print(json.dumps(res))
