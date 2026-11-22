#!/usr/bin/env python3
"""Plan-C test C12 summary: per kind of query, vfts's engine (vfts_cli, no SQL) against the
Elasticsearch profile split, from the files of stage1.sh step c12. Prints a Markdown table and one
JSON line per kind (appended to results.jsonl by the caller).

    python3 -I bench/plan_c/c12_report.py <plan_c dir> <dataset ...>
"""
import importlib.util
import json
import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("report", os.path.join(here, "..", "report.py"))
rep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rep)

d, sets = sys.argv[1], sys.argv[2:]
kinds = {}
for ds in sets:
    qs = dict(l.rstrip("\n").split("\t", 1) for l in open(f"bench/queries/{ds}.tsv", encoding="utf-8") if "\t" in l)
    cli = {p[0]: p[1:] for p in (l.split() for l in open(f"{d}/c12_cli_{ds}.tsv"))}
    es = {p[0]: p[1:] for p in (l.split() for l in open(f"{d}/c12_es_{ds}.tsv"))} if os.path.exists(f"{d}/c12_es_{ds}.tsv") else {}
    for qid, text in qs.items():
        k = rep.kind_of(text)
        v = kinds.setdefault(k, {"n": 0, "cli_ms": 0, "plan_ms": 0, "exec_ms": 0, "es_n": 0, "es_took": 0, "es_rewrite_ms": 0, "es_query_ms": 0, "es_collect_ms": 0})
        if qid in cli:
            c = cli[qid]
            v["n"] += 1
            v["cli_ms"] += float(c[0])
            v["plan_ms"] += float(c[1]) / 1000
            v["exec_ms"] += float(c[2]) / 1000
        if qid in es:
            e = es[qid]
            v["es_n"] += 1
            v["es_took"] += float(e[0])
            v["es_rewrite_ms"] += float(e[1]) / 1e6
            v["es_query_ms"] += float(e[2]) / 1e6      # matching and scoring (the query tree)
            v["es_collect_ms"] += float(e[3]) / 1e6    # the collectors' own time
print("| Kind | Queries | vfts engine ms | of it plan (expansion) | of it execute | ES profiled: rewrite ms | ES profiled: query ms | ES profiled: collect ms | ES took ms |")
print("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
for k, v in kinds.items():
    n, en = max(v["n"], 1), max(v["es_n"], 1)
    row = {"test": f"c12_{k}", "kind": k, "queries": v["n"], "cli_ms": round(v["cli_ms"] / n, 3), "plan_ms": round(v["plan_ms"] / n, 3),
           "exec_ms": round(v["exec_ms"] / n, 3), "es_rewrite_ms": round(v["es_rewrite_ms"] / en, 3), "es_query_ms": round(v["es_query_ms"] / en, 3),
           "es_collect_ms": round(v["es_collect_ms"] / en, 3), "es_took_ms": round(v["es_took"] / en, 2)}
    print(f"| {k} | {v['n']} | {row['cli_ms']} | {row['plan_ms']} | {row['exec_ms']} | {row['es_rewrite_ms']} | {row['es_query_ms']} | {row['es_collect_ms']} | {row['es_took_ms']} |")
    print(json.dumps(row), file=sys.stderr)
