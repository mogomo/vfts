#!/usr/bin/env python3
"""Table of the final 32-client comparison (stage1.sh step f1) from results.jsonl.

    python3 -I bench/plan_c/f1_report.py bench/out/plan_c/results.jsonl [--json out.json]

Per dataset and engine: the median over the rounds of each round's best q/s (every round runs the
request file 3 times), the range over the rounds, mean and 99th percentile client latency (ms) and the CPU cores busy
on the host (median round); the latest 3 rounds count when the step ran more than once. Markdown on stdout.
"""
import json
import statistics
import sys

SETS = ["enron", "pubmed", "msmarco", "wiki_he", "wiki_ar"]
ENGINES = [("vfts_serve", "vfts, tserve (one statement per session)"),
           ("vfts_batch", "vfts, batches (8 sessions, idle-else-queue)"),
           ("vfts_single", "vfts, one statement per query (32 sessions)"),
           ("es_native", "Elasticsearch"),
           ("os_native", "OpenSearch")]

runs, lat = {}, {}
for line in open(sys.argv[1], encoding="utf-8"):
    try:
        d = json.loads(line)
    except ValueError:
        continue
    t = d.get("test", "")
    if not (t.startswith("f1_r") or t.startswith("f1l_r")):
        continue
    kind, r, rest = t.split("_", 2)                  # f1|f1l, r1, vfts_serve_enron | es_native_enron
    ds = next((s for s in SETS if rest.endswith("_" + s)), None)
    if not ds:
        continue
    eng = rest[: -len(ds) - 1]
    (runs if kind == "f1" else lat).setdefault((eng, ds), []).append(d)
# a step run again (e.g. one dataset after a fix) appends rounds: keep the latest 3 per engine and dataset
for m in (runs, lat):
    for key in m:
        m[key] = m[key][-3:]

out = {}
print("| Engine | " + " | ".join(SETS) + " |")
print("|---|" + "---:|" * len(SETS))
for eng, name in ENGINES:
    cells = []
    for ds in SETS:
        v = runs.get((eng, ds), [])
        if not v:
            cells.append("")
            continue
        q = sorted(x["qps_best"] for x in v)
        med = statistics.median(q)
        mid = min(v, key=lambda x: abs(x["qps_best"] - med))
        out.setdefault(eng, {})[ds] = {"qps_median": round(med, 1), "qps_rounds": q, "client_ms_mean": mid["client_ms"]["mean"], "client_ms_p99": mid["client_ms"]["p99"],
                                       "cpu_host": mid["cpu"]["host"], "errors": sum(x.get("errors", 0) for x in v)}
        cells.append(f"{med:,.0f} ({q[0]:,.0f}-{q[-1]:,.0f})")
    print(f"| {name} | " + " | ".join(cells) + " |")

print()
print("One client: mean ms per query (best of 3 runs per query), median over rounds; p99 in brackets")
print()
print("| Engine | " + " | ".join(SETS) + " |")
print("|---|" + "---:|" * len(SETS))
for eng, name in ENGINES:
    cells = []
    for ds in SETS:
        v = lat.get((eng, ds), [])
        if not v:
            cells.append("")
            continue
        m = statistics.median(x["client_ms"]["mean"] for x in v)
        p = statistics.median(x["client_ms"]["p99"] for x in v)
        out.setdefault(eng + "_1client", {})[ds] = {"ms_mean": round(m, 3), "ms_p99": round(p, 3)}
        cells.append(f"{m:.2f} ({p:.1f})")
    if any(cells):
        print(f"| {name} | " + " | ".join(cells) + " |")

print()
print("vfts serve / best of Elasticsearch and OpenSearch (median q/s):")
for ds in SETS:
    v = out.get("vfts_serve", {}).get(ds)
    e = [out.get(k, {}).get(ds) for k in ("es_native", "os_native")]
    e = [x["qps_median"] for x in e if x]
    if v and e:
        print(f"- {ds}: {v['qps_median'] / max(e):.2f}x")
if "--json" in sys.argv:
    json.dump(out, open(sys.argv[sys.argv.index("--json") + 1], "w"), indent=1)
