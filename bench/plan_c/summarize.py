#!/usr/bin/env python3
"""Tables of plan-C stage 1 from bench/out/plan_c/results.jsonl (Markdown on stdout).

    python3 -I bench/plan_c/summarize.py [results.jsonl] [bench/results]

The Elasticsearch / OpenSearch q/s at 32 clients (one query per request) come from the benchmark
results (bench/results/{es,os}_query.jsonl), measured on the same host and cores.
"""
import json
import os
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "bench/out/plan_c/results.jsonl"
ref = sys.argv[2] if len(sys.argv) > 2 else "bench/results"
R = {}
for line in open(path):
    r = json.loads(line)
    R[r["test"]] = r                  # the last run of a test wins
eng = {}
for e in ("es", "os"):
    p = os.path.join(ref, f"{e}_query.jsonl")
    if os.path.exists(p):
        for line in open(p):
            r = json.loads(line)
            if r.get("clients") == 32 and not r.get("count") and r.get("batch", 1) in (1, None):
                eng[(e, r["index"])] = r["qps_best"]


def cpu(r):
    c = r.get("cpu", {})
    return f"{c.get('host', '')} / {c.get('server', '')}"


def g(t, *keys):
    r = R.get(t)
    for k in keys:
        if r is None:
            return ""
        r = r.get(k) if isinstance(r, dict) else None
    return "" if r is None else r


out = []
out += ["## C0 baseline (32 clients, one query per request)", "",
        "| Dataset | View | q/s | server ms | client ms | CPU host / Vertica (cores) | ES q/s | OS q/s |", "|---|---|---:|---:|---:|---|---:|---:|"]
for ds in ("enron", "msmarco"):
    for v in ("snap", "live"):
        t = f"c0_{ds}_{v}"
        if t in R:
            out.append(f"| {ds} | {v} | {g(t, 'qps_best')} | {g(t, 'server_ms', 'mean')} | {g(t, 'client_ms', 'mean')} | {cpu(R[t])} | "
                       f"{eng.get(('es', ds), '')} | {eng.get(('os', ds), '')} |")
out += ["", "## C1 SQL ceiling (statements per second; server ms per statement; CPU cores host / Vertica)", "",
        "| Statement | " + " | ".join(f"{c} sessions" for c in (1, 8, 16, 32, 64)) + " |", "|---|" + "---:|" * 5]
for s, name in (("select1", "SELECT 1"), ("udx0", "trivial UDx (tversion)"), ("tsearch_snap", "tsearch snap, 1 word"),
                ("tsearch_live", "tsearch live, 1 word")):
    cells = []
    for c in (1, 8, 16, 32, 64):
        t = f"c1_{s}_c{c}"
        cells.append(f"{g(t, 'qps_best')} ({g(t, 'server_ms', 'mean')} ms; {cpu(R[t])})" if t in R else "")
    out.append(f"| {name} | " + " | ".join(cells) + " |")
out += ["", "## C2 where a statement's time goes (ms per statement, data collector)", ""]
steps = ["BeforePlan:Parser", "BeforePlan:AnalyzeRewrite", "Plan", "PreparePlan", "PreparePlan:LocalPlan", "PreparePlan:TunePlanResources",
         "PreparePlan:EEcompile", "CompilePlan", "CompilePlan:EEpreexecute", "ExecutePlan"]
out += ["| Statement | sessions | request | queue wait | " + " | ".join(steps) + " |", "|---|---:|---:|---:|" + "---:|" * len(steps)]
for s in ("select1", "udx0", "tsearch_snap", "tsearch_inline", "tsearch_live"):
    for c in (1, 32):
        t = f"c2_{s}_c{c}"
        if t in R and "steps_ms" in R[t]:
            r = R[t]
            out.append(f"| {s} | {c} | {r['request_ms']} | {r['queue_wait_ms']} | " + " | ".join(str(r["steps_ms"].get(x, "")) for x in steps) + " |")
for k in ("python", "python_prepared"):
    for c in (1, 32):
        t = f"c5_tsearch_same_c{c}_{k}"
        if t in R and "steps_ms" in R[t]:
            r = R[t]
            out.append(f"| tsearch same text, {k} | {c} | {r['request_ms']} | {r['queue_wait_ms']} | " + " | ".join(str(r["steps_ms"].get(x, "")) for x in steps) + " |")
for ds in ("enron", "msmarco", "pubmed"):
    for v in ("snap", "live"):
        rows = [(c, b) for c in (4, 8, 16, 32) for b in (1, 2, 5, 10, 20, 50, 100, 200, 500) if f"c3_{ds}_{v}_c{c}_b{b}" in R]
        if not rows:
            continue
        bs = sorted({b for _, b in rows})
        out += ["", f"## C3 batch curve: {ds} {v} (q/s; ES {eng.get(('es', ds), '?')}, OS {eng.get(('os', ds), '?')} at 32 clients, 1 query per request)", "",
                "| sessions | " + " | ".join(f"B={b}" for b in bs) + " |", "|---:|" + "---:|" * len(bs)]
        for c in (4, 8, 16, 32):
            cells = []
            for b in bs:
                t = f"c3_{ds}_{v}_c{c}_b{b}"
                if t in R:
                    q = R[t]["qps_best"]
                    best_eng = max(eng.get(("es", ds), 0), eng.get(("os", ds), 0))
                    cells.append(f"**{q}**" if best_eng and q > best_eng else f"{q}")
                else:
                    cells.append("")
            out.append(f"| {c} | " + " | ".join(cells) + " |")
        out += ["", "statement ms (server) at 32 sessions: " + ", ".join(
            f"B={b}: {round(R[f'c3_{ds}_{v}_c32_b{b}']['server_ms']['mean'] * b, 1)}" for b in bs if f"c3_{ds}_{v}_c32_b{b}" in R)]
out += ["", "## C4 engine only (vtext_cli, no SQL; q/s, mean ms, CPU host cores)", "",
        "| Dataset | " + " | ".join(f"{c} clients" for c in (1, 4, 8, 16, 32, 64)) + " | 32 clients, all threads |", "|---|" + "---:|" * 7]
for ds in ("enron", "pubmed", "msmarco"):
    cells = []
    for c in (1, 4, 8, 16, 32, 64):
        t = f"c4_{ds}_c{c}_t1"
        cells.append(f"{g(t, 'qps_best')} ({g(t, 'ms', 'mean')} ms; {g(t, 'cpu', 'host')})" if t in R else "")
    tt = [t for t in R if t.startswith(f"c4_{ds}_c32_t") and not t.endswith("_t1")]
    cells.append(f"{g(tt[0], 'qps_best')} ({g(tt[0], 'ms', 'mean')} ms; {g(tt[0], 'cpu', 'host')})" if tt else "")
    out.append(f"| {ds} | " + " | ".join(cells) + " |")
out += ["", "## C5 client and session cost (32 sessions unless noted; q/s wall clock, server ms)", "", "| Test | q/s | server ms | CPU host / Vertica / clients |", "|---|---:|---:|---|"]
for t in sorted(x for x in R if x.startswith("c5_") and "steps_ms" not in R[x]):
    r = R[t]
    out.append(f"| {t[3:]} | {r.get('qps_best')} | {g(t, 'server_ms', 'mean')} | {cpu(r)} / {g(t, 'cpu', 'clients')} |")
out += ["", "## C6 sessions, threads, resource pool (q/s; server ms)", "", "| Test | q/s | server ms | client ms | CPU host / Vertica |", "|---|---:|---:|---:|---|"]
for t in sorted(x for x in R if x.startswith("c6_")):
    r = R[t]
    out.append(f"| {t[3:]} | {r.get('qps_best')} | {g(t, 'server_ms', 'mean')} | {g(t, 'client_ms', 'mean')} | {cpu(r)} |")
print("\n".join(out))
