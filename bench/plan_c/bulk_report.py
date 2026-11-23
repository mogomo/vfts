#!/usr/bin/env python3
"""Tables of the bulk tests (bench/plan_c/bulk.sh) from their results.jsonl, and the answers compared.

    python3 -I bench/plan_c/bulk_report.py bench/out/bulk/results.jsonl [--answers bench/out/bulk] [--eon eon.jsonl]

Markdown on stdout. Per set: the best time of each engine over its configurations (vfts: P
statements at once, into a file or a table; engines: batch size x requests at once), the cold
first run after a restart, every configuration, the shared batches (b2), the cluster (b3, --eon).
--answers DIR: the result files of the last runs (<set>_<engine>.tsv): counts equal per query; top
10: the same scores at every rank (relative difference at most 1e-4, as bench/report.py) and the
same documents above the 10th score (Elasticsearch
scores on its legacy scale, x2.2 of Lucene's, and constant-score patterns score 1 on both).
"""
import collections
import json
import os
import sys

SETS = [("msmarco_top10", "6,980 MS MARCO questions, top 10 each"),
        ("enron_top10", "500 Enron search terms, top 10 each"),
        ("enron_count", "500 Enron search terms, matches of each"),
        ("msmarco_count", "6,980 MS MARCO questions, matches of each")]
ENG = [("vfts", "vfts"), ("es", "Elasticsearch"), ("os", "OpenSearch")]


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def load(path):
    rows = {}
    for line in open(path, encoding="utf-8"):
        try:
            d = json.loads(line)
        except ValueError:
            continue
        if "test" in d and "error" not in d:
            rows[d["test"]] = d            # a step run again: the latest line counts
    return rows


def fmt(s):
    return f"{s:.3f} s" if s < 10 else f"{s:.1f} s"


def cfg(t, e):
    if e == "vfts":
        p = t.rsplit("_p", 1)[1]
        return f"{p} statement{'s' if p != '1' else ''}" + (", into a table" if "_table_" in t else "")
    tag = t.split(f"_{e}_", 1)[1]
    parts = tag.split("_")
    b, c = parts[0][1:], parts[1][1:]
    return f"batches of {b}, {c} at once" + (f", max_concurrent_searches {parts[2][3:]}" if len(parts) > 2 else "")


def answers(d):
    out = []
    for s, name in SETS:
        base = os.path.join(d, f"{s}_vfts.tsv")
        if not os.path.exists(base):
            continue
        for e, en in ENG[1:]:
            other = os.path.join(d, f"{s}_{e}.tsv")
            if not os.path.exists(other):
                continue
            if s.endswith("_count"):
                a = dict(l.split()[:2] for l in open(base))
                b = dict(l.split()[:2] for l in open(other))
                eq = sum(a[q] == b.get(q) for q in a)
                near = sum(a[q] == b.get(q) or (b.get(q) and abs(int(a[q]) - int(b[q])) <= 0.001 * max(int(b[q]), 1)) for q in a)
                out.append(f"| {name} | {en} | counts equal for {eq:,} of {len(a):,}; within 0.1% for {near:,} |")
            else:
                def rl(p):
                    t = collections.defaultdict(list)
                    for l in open(p):
                        q, doc, r, sc = l.split()[:4]
                        t[q].append((int(r), doc, float(sc)))
                    return {q: sorted(v) for q, v in t.items()}
                a, b = rl(base), rl(other)
                qs = sorted(set(a) | set(b))
                same, above = 0, 0
                for q in qs:
                    x, y = a.get(q, []), b.get(q, [])
                    ok_s, ok_a = False, False
                    for f in ((1.0, 2.2) if e == "es" else (1.0,)):
                        sx = [v for _, _, v in x]
                        sy = [v / f for _, _, v in y]
                        if len(sx) == len(sy) and all(abs(i - j) <= 1e-4 * max(abs(i), 1e-9) for i, j in zip(sx, sy)):
                            ok_s = True
                            tenth = sx[-1] if sx else 0
                            A = {doc for _, doc, v in x if v > tenth * (1 + 1e-4)}
                            B = {doc for _, doc, v in y if v / f > tenth * (1 + 1e-4)}
                            ok_a = ok_a or A == B
                    same += ok_s
                    above += ok_a
                out.append(f"| {name} | {en} | same scores at every rank for {same:,} of {len(qs):,}; "
                           f"same documents above the 10th score for {above:,} |")
    return out


def main():
    r = load(sys.argv[1])
    print("## Best of each (warm: the best of 3 runs after a warm-up; time until every result is stored)\n")
    print("| Set | vfts | Elasticsearch | OpenSearch | vfts against the faster engine |")
    print("|---|---:|---:|---:|---:|")
    best = {}
    for s, name in SETS:
        cells = []
        for e, _ in ENG:
            ts = [t for t in r if t.startswith(f"b1_{s}_{e}_")]
            if not ts:
                cells.append("")
                continue
            t = min(ts, key=lambda x: r[x]["best_s"])
            best[(s, e)] = (r[t]["best_s"], t)
            cells.append(f"{fmt(r[t]['best_s'])} ({cfg(t, e)})")
        f = ""
        if all((s, e) in best for e, _ in ENG):
            v, eng = best[(s, "vfts")][0], min(best[(s, "es")][0], best[(s, "os")][0])
            f = f"{eng / v:.2f}x faster" if v <= eng else f"{v / eng:.2f}x slower"
        print(f"| {name} | " + " | ".join(cells) + f" | {f} |")
    print("\n## Cold: the first run after a restart of the database or the engine (no warm-up)\n")
    print("| Set | vfts (4 statements) | Elasticsearch | OpenSearch |")
    print("|---|---:|---:|---:|")
    for s, name in SETS:
        cells = []
        for e, _ in ENG:
            t = f"b1cold_{s}_vfts_file_p4" if e == "vfts" else f"b1cold_{s}_{e}_b100_c8"
            cells.append(fmt(r[t]["best_s"]) if t in r else "")
        print(f"| {name} | " + " | ".join(cells) + " |")
    print("\n## Every configuration (best of 3 runs)\n")
    print("| Set | Engine | Configuration | Best | Runs |")
    print("|---|---|---|---:|---|")
    for s, name in SETS:
        for e, en in ENG:
            for t in sorted((t for t in r if t.startswith(f"b1_{s}_{e}_")), key=lambda x: r[x]["best_s"]):
                print(f"| {name} | {en} | {cfg(t, e)} | {fmt(r[t]['best_s'])} | {', '.join(str(x) for x in r[t]['runs_s'])} |")
    b2 = [t for t in r if t.startswith("b2_")]
    if b2:
        print("\n## Many users' batches into one table (b2): each user as themselves, or one service user\n")
        print("| Set | Users | Each user's own statement | One service user | Same rows | Each user sees only their rows | Service user reads results |")
        print("|---|---:|---:|---:|---|---|---|")
        for t in sorted(b2):
            d = r[t]
            nm = dict(SETS).get(t[3:], t[3:])
            print(f"| {nm} | {d['users']} | {fmt(d['per_user']['best_s'])} ({d['per_user']['statements']} statements) | "
                  f"{fmt(d['shared']['best_s'])} ({d['shared']['statements']} statements) | {'yes' if d['same_rows'] else 'NO'} | "
                  f"{d['users_see_only_their_rows']} | {'yes' if d['service_user_can_read_results'] else 'no'} |")
    eon = arg("--eon")
    if eon and os.path.exists(eon):
        e = load(eon)
        print("\n## A cluster (b3): the same batch on more nodes, one statement per node\n")
        print("| Batch | Results | Nodes | Statements | Best | Runs |")
        print("|---|---|---:|---:|---:|---|")

        def key(t):
            d = e[t]
            ctl = "_node1_split" in t
            return (d["queries"], "_top10" not in t, d["into"], ctl, 0 if ctl else int(t.rsplit("nodes", 1)[1]))
        for t in sorted(e, key=key):
            d = e[t]
            kind = "top 10 each" if "_top10" in t else "matches of each"
            nodes = "1 (control)" if "_node1_split" in t else t.rsplit("nodes", 1)[1]
            where = "a table of the cluster" if d["into"] else "the client (node 1)"
            print(f"| {d['queries']:,} Enron terms ({d['queries'] // 500} x the 500), {kind} | {where} | {nodes} | {d['split']} | "
                  f"{fmt(d['best_s'])} | {', '.join(str(x) for x in d['runs_s'])} |")
    d = arg("--answers")
    if d:
        a = answers(d)
        if a:
            print("\n## The same answers\n")
            print("| Set | Against | Result |")
            print("|---|---|---|")
            print("\n".join(a))


if __name__ == "__main__":
    main()
