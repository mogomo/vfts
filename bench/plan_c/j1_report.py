#!/usr/bin/env python3
"""The tables of step j1 (bench/plan_c/stage1.sh j1: search next to other data, with attributes).

    python3 -I bench/plan_c/j1_report.py [results.jsonl] > bench/results/joins.md

Reads the j1_* lines (the latest of each test) and prints one Markdown table per dataset:
J1 and J2 in seconds; J3 and J4 as the mean time of one client (ms, through the native HTTP
client, each query 3 times, best kept) and the queries per second of 32 clients (best of 3 runs).
vfts columns: one SQL statement per search (vfts_broker --mode sql, one session per client),
and tserve (vfts_broker --mode serve). Engines: as they come, and with their caches off.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "out", "plan_c", "results.jsonl")
T = {}
for line in open(path, encoding="utf-8"):
    try:
        d = json.loads(line)
    except ValueError:
        continue
    if str(d.get("test", "")).startswith("j1_"):
        T[d["test"]] = d
NAMES = {"enron": "Enron, 517,401 emails, 40 queries", "msmarco": "MS MARCO, 8,841,823 passages, the first 40 questions of the dev set"}


def ms(t):
    d = T.get(t)
    return f"{d['client_ms']['mean']:.2f}" if d else "-"


def qps(t):
    d = T.get(t)
    return f"{d['qps_best']:,.0f}" if d else "-"


def sec(t, k):
    d = T.get(t)
    return f"{d[k]:,.3f}" if d and k in d else "-"


for ds in ("enron", "msmarco"):
    if not any(k.endswith(f"_{ds}_c1") or f"_{ds}_" in k for k in T):
        continue
    v, es, os_ = f"j1_vfts_attr_{ds}", f"j1_es_{ds}", f"j1_os_{ds}"
    print(f"### {NAMES[ds]}\n")
    print("| Test | vfts, one SQL statement | vfts, tserve | Elasticsearch | OpenSearch |")
    print("|---|---:|---:|---:|---:|")
    print(f"| J1 add the attribute to every document (s) | {sec(v + '_sql_c1', 'j1_add_attribute_s')} | (same) | "
          f"{sec(es + '_c1', 'j1_add_attribute_s')} | {sec(os_ + '_c1', 'j1_add_attribute_s')} |")
    print(f"| J2 move 1% of the documents to another custodian (s) | {sec(v + '_sql_c1', 'j2_update_1pct_s')} | (same) | "
          f"{sec(es + '_c1', 'j2_update_1pct_s')} | {sec(os_ + '_c1', 'j2_update_1pct_s')} |")
    for j, what in (("j3", "J3 top 10 among the custodians on hold"), ("j4", "J4 matches per custodian")):
        print(f"| {what}, 1 client (ms) | {ms(f'{v}_single_{j}_c1')} | {ms(f'{v}_serve_{j}_c1')} | "
              f"{ms(f'{es}_{j}_c1')} | {ms(f'{os_}_{j}_c1')} |")
        print(f"| {what}, 32 clients (q/s) | {qps(f'{v}_single_{j}_c32')} | {qps(f'{v}_serve_{j}_c32')} | "
              f"{qps(f'{es}_{j}_c32')} | {qps(f'{os_}_{j}_c32')} |")
        vnc = f"{v}_servenc_{j}"        # tserve with filter_cache=false and request_cache=false
        print(f"| {what}, caches off, 1 client (ms) | | {ms(vnc + '_c1')} | {ms(f'{es}_{j}nc_c1')} | {ms(f'{os_}_{j}nc_c1')} |")
        print(f"| {what}, caches off, 32 clients (q/s) | | {qps(vnc + '_c32')} | {qps(f'{es}_{j}nc_c32')} | {qps(f'{os_}_{j}nc_c32')} |")
    d = T.get(v + "_sql_c1")
    if d:
        print(f"\nvfts through vsql, one client (statement time as vsql reports it): J3 {d.get('j3_mean_ms')} ms "
              f"({d.get('j3_nocache_mean_ms')} ms without the filter cache), J4 {d.get('j4_mean_ms')} ms; "
              f"J1 = table {d.get('j1_table_s')} s + add_attribute {d.get('j1_attribute_s')} s; "
              f"J2 = UPDATE {d.get('j2_table_s')} s + refresh_attributes {d.get('j2_refresh_s')} s.")
    for e, name in ((es, "Elasticsearch"), (os_, "OpenSearch")):
        d = T.get(e + "_c1")
        if d:
            print(f"{name} through its Python client, one client: J3 {d.get('j3_mean_ms')} ms, J4 {d.get('j4_mean_ms')} ms "
                  f"(caches off: {d.get('j3_nocache_mean_ms')} / {d.get('j4_nocache_mean_ms')} ms); "
                  f"force merge after the updates {d.get('merge_after_updates_s')} s.")
    print()
