#!/usr/bin/env python3
"""Tables and charts from the recorded results (bench/run_all.sh).

    python3 -I bench/report.py

Reads bench/results/*.jsonl, env.json and the run files in bench/out; writes
bench/results/summary.md (all tables) and bench/results/charts/*.svg. Only recorded numbers are
used. For a value measured more than once, the last record wins. Python standard library only.
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "results")
OUT = os.path.join(HERE, "out")
CHARTS = os.path.join(RES, "charts")
ENGINES = [("vtext", "vtext (Vertica)"), ("es", "Elasticsearch"), ("os", "OpenSearch")]
# categorical slots 1-3 of the validated reference palette (light, dark): fixed per engine
COLORS = {"vtext": ("#2a78d6", "#3987e5"), "es": ("#eb6834", "#d95926"), "os": ("#1baf7a", "#199e70")}
SETS = [("msmarco", "MS MARCO"), ("enron", "Enron"), ("pubmed", "PubMed"), ("wiki_he", "Wikipedia he"), ("wiki_ar", "Wikipedia ar")]


def jsonl(name):
    p = os.path.join(RES, name)
    if not os.path.exists(p):
        return []
    with open(p) as f:
        return [json.loads(l) for l in f if l.strip().startswith("{")]


def last(rows, **match):
    hit = None
    for r in rows:
        if all(r.get(k) == v for k, v in match.items()):
            hit = r
    return hit


def fmt(x, d=1):
    if x is None:
        return "-"
    if isinstance(x, float):
        return f"{x:,.{d}f}"
    return f"{x:,}"


def mb(b):
    return None if b in (None, "") else round(int(b) / 1e6)


def svg_bars(path, title, unit, groups, better):
    """Horizontal grouped bars: groups = [(label, {engine: value})]. Values labelled at the bar tips."""
    bar, gap, ggap, left, right, top = 14, 3, 18, 120, 90, 56
    engines = [e for e, _ in ENGINES if any(v.get(e) is not None for _, v in groups)]
    height = top + sum(len(engines) * (bar + gap) + ggap for _ in groups) + 40
    width = 720
    vmax = max([v for _, g in groups for v in g.values() if v is not None] + [1e-9])
    scale = (width - left - right) / vmax
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img" aria-label="{title}">',
         "<style>",
         ".bg{fill:#fcfcfb}.t1{fill:#0b0b0b}.t2{fill:#52514e}.ax{stroke:#d9d8d4}",
         "".join(f".{e}{{fill:{c[0]}}}" for e, c in COLORS.items()),
         "@media (prefers-color-scheme: dark){.bg{fill:#1a1a19}.t1{fill:#f0efec}.t2{fill:#c3c2b7}.ax{stroke:#3a3a38}"
         + "".join(f".{e}{{fill:{c[1]}}}" for e, c in COLORS.items()) + "}",
         "text{font-family:-apple-system,Segoe UI,Helvetica,Arial,sans-serif}",
         "</style>",
         f'<rect class="bg" width="{width}" height="{height}" rx="6"/>',
         f'<text class="t1" x="16" y="24" font-size="15" font-weight="600">{title}</text>',
         f'<text class="t2" x="16" y="42" font-size="12">{unit}, {better}</text>']
    # legend
    lx = width - right - 10
    for e, name in reversed([x for x in ENGINES if x[0] in engines]):
        w = 7.2 * len(name) + 18
        lx -= w
        s.append(f'<rect class="{e}" x="{lx}" y="32" width="10" height="10" rx="2"/>')
        s.append(f'<text class="t2" x="{lx + 14}" y="41" font-size="12">{name}</text>')
    y = top
    for label, vals in groups:
        s.append(f'<text class="t1" x="{left - 10}" y="{y + (len(engines) * (bar + gap)) / 2 + 4}" font-size="12" text-anchor="end">{label}</text>')
        for e in engines:
            v = vals.get(e)
            if v is not None:
                w = max(v * scale, 2)
                # square at the baseline, 4px rounded data end
                s.append(f'<path class="{e}" d="M{left},{y} h{max(w - 4, 0)} a4,4 0 0 1 4,4 v{bar - 8} a4,4 0 0 1 -4,4 h{-max(w - 4, 0)} z"><title>{e}: {v}</title></path>')
                s.append(f'<text class="t2" x="{left + w + 6}" y="{y + bar - 3}" font-size="11">{fmt(v, 1 if v < 100 else 0)}</text>')
            else:
                s.append(f'<text class="t2" x="{left + 4}" y="{y + bar - 3}" font-size="11">not measured</text>')
            y += bar + gap
        y += ggap
    s.append(f'<line class="ax" x1="{left}" y1="{top - 6}" x2="{left}" y2="{y - ggap + 4}" stroke-width="1"/>')
    s.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(s))


def relevance():
    qrels = os.path.join(os.environ.get("VTEXT_DATA", os.path.expanduser("~/vtext_data")), "msmarco", "qrels.dev.small.tsv")
    runs = [os.path.join(OUT, f"{e}_msmarco.run") for e, _ in ENGINES]
    runs = [r for r in runs if os.path.exists(r)]
    if not os.path.exists(qrels) or not runs:
        return {}
    out = subprocess.run([sys.executable, "-I", os.path.join(HERE, "eval.py"), qrels] + runs, capture_output=True, text=True).stdout
    rel = {}
    for line in out.splitlines():
        j = json.loads(line)
        e = os.path.basename(j["run"]).split("_")[0]
        rel[e] = j
    return rel


def count_agreement(ds, other):
    a = os.path.join(OUT, f"vtext_{ds}_count.run")
    b = os.path.join(OUT, f"{other}_{ds}_count.run")
    if not (os.path.exists(a) and os.path.exists(b)):
        return None
    ra = dict(l.split("\t")[:2] for l in open(a) if "\t" in l)
    rb = dict(l.split("\t")[:2] for l in open(b) if "\t" in l)
    same = sum(1 for q in ra if q in rb and ra[q].strip() == rb[q].strip())
    return same, len(ra)


def read_run(path):
    """qid -> [(doc_id, score)] in rank order."""
    out = {}
    if not os.path.exists(path):
        return None
    for l in open(path):
        p = l.rstrip("\n").split("\t")
        if len(p) >= 4:
            out.setdefault(p[0], []).append((p[1], float(p[3])))
    return out


def top_agreement(ds, other):
    """The top 10 of vtext against an engine. Elasticsearch is compared with the vtext run with
    bm25='classic' (its scale), OpenSearch with the default run. "Same scores": at every rank the
    score is the same (relative difference at most 1e-4). "Same documents": the documents above the
    10th score are the same (documents tied with the 10th may differ: the engines break ties
    differently). Score difference: over the documents both returned."""
    mine = os.path.join(OUT, f"vtext_{ds}_classic.run" if other == "es" else f"vtext_{ds}.run")
    a, b = read_run(mine), read_run(os.path.join(OUT, f"{other}_{ds}.run"))
    if not a or b is None:
        return None
    q = same_scores = same_docs = 0
    diffs = []
    for qid in set(a) | set(b):
        x, y = a.get(qid, []), b.get(qid, [])
        q += 1
        same_scores += len(x) == len(y) and all(abs(p[1] - r[1]) <= 1e-4 * max(abs(p[1]), 1e-9) for p, r in zip(x, y))
        cut_x = x[-1][1] if len(x) >= 10 else -1.0
        cut_y = y[-1][1] if len(y) >= 10 else -1.0
        above_x = set(d for d, sc in x if sc > cut_x * (1 + 1e-4))
        above_y = set(d for d, sc in y if sc > cut_y * (1 + 1e-4))
        same_docs += above_x == above_y
        ys = dict(y)
        for d, sc in x:
            if d in ys and sc > 0:
                diffs.append(abs(ys[d] - sc) / sc)
    diffs.sort()
    return {"queries": q, "same_scores": same_scores, "same_docs": same_docs,
            "median_rel": diffs[len(diffs) // 2] if diffs else None, "max_rel": diffs[-1] if diffs else None}


def kind_of(query):
    """The operator that decides the cost of a query of the syntax sets."""
    import re
    if re.search(r"~\d?(\s|$|\))", query) and '"' not in query.split("~")[0][-1:]:
        return "fuzzy"
    if "*" in query or "?" in query:
        return "prefix / wildcard"
    if re.search(r"\b(w|pre)/\d+", query):
        return "w/N, pre/N"
    if '"' in query:
        return "phrase"
    if re.search(r"\b(AND|OR|NOT)\b|^-|\s-", query):
        return "boolean"
    return "words"


def read_times(path):
    if not os.path.exists(path):
        return None
    out = {}
    for l in open(path):
        p = l.rstrip("\n").split("\t")
        if len(p) >= 3 and p[2] != "inf":
            out[p[0]] = float(p[2])
    return out


def main():
    os.makedirs(CHARTS, exist_ok=True)
    env = json.load(open(os.path.join(RES, "env.json"))) if os.path.exists(os.path.join(RES, "env.json")) else {}
    load = {e: jsonl(f"{e}_load.jsonl") for e, _ in ENGINES}
    query = {e: jsonl(f"{e}_query.jsonl") for e, _ in ENGINES}
    md = ["# Benchmark results", "", "Generated by `bench/report.py` from `bench/results/*.jsonl`. Every number is a recorded run.", ""]
    if env:
        md += ["## Machine", "", "| | |", "|---|---|"] + [f"| {k} | {v} |" for k, v in env.items()] + [""]

    # loading
    md += ["## Index build", "",
           "vtext: `index_s` is `CALL vtext.create_index` (analysis, index tables, node cache); the COPY into the source table is listed apart. "
           "Elasticsearch / OpenSearch: bulk load + refresh + force merge to one segment. Sizes: vtext node cache file and index tables; "
           "the engines' store (includes their copy of the text, `_source`).", "",
           "| Dataset | Documents | vtext index (s) | Vertica COPY (s) | Elasticsearch (s) | OpenSearch (s) | vtext cache (MB) | vtext tables (MB) | Elasticsearch (MB) | OpenSearch (MB) |",
           "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    build_g, size_g = [], []
    for ds, name in SETS:
        v, e, o = last(load["vtext"], dataset=ds), last(load["es"], dataset=ds), last(load["os"], dataset=ds)
        if not (v or e or o):
            continue
        docs = (v or e or o).get("docs")
        md.append(f"| {name} | {fmt(docs)} | {fmt(v and v.get('index_s'))} | {fmt(v and v.get('copy_s'))} | {fmt(e and e.get('index_s'))} | "
                  f"{fmt(o and o.get('index_s'))} | {fmt(mb(v and v.get('cache_bytes')))} | {fmt(mb(v and v.get('index_table_bytes')))} | "
                  f"{fmt(mb(e and e.get('bytes')))} | {fmt(mb(o and o.get('bytes')))} |")
        build_g.append((name, {"vtext": v and v.get("index_s"), "es": e and e.get("index_s"), "os": o and o.get("index_s")}))
        size_g.append((name, {"vtext": mb(v and v.get("cache_bytes")), "es": mb(e and e.get("bytes")), "os": mb(o and o.get("bytes"))}))
    md.append("")
    if build_g:
        svg_bars(os.path.join(CHARTS, "build.svg"), "Index build time", "seconds", build_g, "lower is better")
        svg_bars(os.path.join(CHARTS, "size.svg"), "Index size", "MB", size_g, "lower is better")

    # queries
    for clients, title in ((1, "Latency, 1 client"), (32, "Throughput, 32 clients")):
        md += [f"## {title}", "",
               "Server time: Vertica `v_monitor.query_requests` (statement start to end) / the engines' `took`. Client time: the whole "
               "round trip seen by the client (vsql `\\timing` / HTTP). Best of the measured runs per query, after a warm-up.", "",
               "| Dataset | Engine | Queries | Server mean (ms) | Server p50 | Server p99 | Client mean (ms) | Client p50 | Client p99 | Queries/s |",
               "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
        lat_g, qps_g = [], []
        for ds, name in SETS:
            vals_l, vals_q = {}, {}
            for e, ename in ENGINES:
                r = last([x for x in query[e] if not x.get("count") and not x.get("fenced") and x.get("clients") == clients], index=ds)
                if not r:
                    continue
                md.append(f"| {name} | {ename} | {r['queries']} | {fmt(r['server_ms']['mean'], 2)} | {fmt(r['server_ms']['p50'], 2)} | "
                          f"{fmt(r['server_ms']['p99'], 2)} | {fmt(r['client_ms']['mean'], 2)} | {fmt(r['client_ms']['p50'], 2)} | "
                          f"{fmt(r['client_ms']['p99'], 2)} | {fmt(r['qps_best'])} |")
                vals_l[e] = r["client_ms"]["mean"]
                vals_q[e] = r["qps_best"]
            if vals_l:
                lat_g.append((name, vals_l))
                qps_g.append((name, vals_q))
        md.append("")
        if clients == 1 and lat_g:
            svg_bars(os.path.join(CHARTS, "latency.svg"), "Mean query latency, 1 client", "milliseconds, client round trip", lat_g, "lower is better")
        if clients == 32 and qps_g:
            svg_bars(os.path.join(CHARTS, "throughput.svg"), "Throughput, 32 clients", "queries per second", qps_g, "higher is better")

    # fenced
    f = last([x for x in query["vtext"] if x.get("fenced")], index="msmarco")
    u = last([x for x in query["vtext"] if not x.get("fenced") and x.get("clients") == 1 and not x.get("count")], index="msmarco")
    if f and u:
        md += ["## Fenced and unfenced (vtext, MS MARCO, 1 client)", "", "| Mode | Server mean (ms) | Client mean (ms) |", "|---|---:|---:|",
               f"| unfenced | {fmt(u['server_ms']['mean'], 2)} | {fmt(u['client_ms']['mean'], 2)} |",
               f"| fenced | {fmt(f['server_ms']['mean'], 2)} | {fmt(f['client_ms']['mean'], 2)} |", ""]

    # relevance
    rel = relevance()
    if rel:
        md += ["## Relevance (MS MARCO passage, dev, 6,980 judged queries)", "", "| Engine | MRR@10 | nDCG@10 | Same top 10 as vtext |", "|---|---:|---:|---:|"]
        for e, ename in ENGINES:
            if e in rel:
                md.append(f"| {ename} | {rel[e]['mrr@10']} | {rel[e]['ndcg@10']} | {rel[e].get('same_top10_as_first', 1.0) if e != 'vtext' else '-'} |")
        md.append("")

    # agreement of counts
    rows = []
    for ds, name in SETS[1:]:
        for o, oname in ENGINES[1:]:
            a = count_agreement(ds, o)
            if a:
                rows.append(f"| {name} | {oname} | {a[0]} of {a[1]} |")
    if rows:
        md += ["## Same matches (counts per query, syntax queries)", "", "| Dataset | Engine | Queries with the same count as vtext |", "|---|---|---:|"] + rows + [""]


    # agreement of the top 10
    rows = []
    for ds, name in SETS:
        for o, oname in ENGINES[1:]:
            a = top_agreement(ds, o)
            if a:
                med = "-" if a["median_rel"] is None else f"{a['median_rel']:.1e}"
                mx = "-" if a["max_rel"] is None else f"{a['max_rel']:.1e}"
                rows.append(f"| {name} | {oname} | {a['queries']} | {a['same_scores']} | {a['same_docs']} | {med} | {mx} |")
    if rows:
        md += ["## Same answers (top 10)", "",
               "Elasticsearch against vtext with `bm25='classic'` (the scale of Elasticsearch), OpenSearch against vtext's default. "
               "Same scores: the score at every rank is the same (relative difference at most 1e-4). Same documents: the documents "
               "scored above the 10th are the same (documents tied with the 10th may differ: each engine breaks ties its own way). "
               "Score difference: relative, over the documents both engines returned.", "",
               "| Dataset | Engine | Queries | Same scores | Same documents | Score difference, median | Score difference, max |",
               "|---|---|---:|---:|---:|---:|---:|"] + rows + [""]

    # latency per kind of query (syntax sets, 1 client, client round trip)
    kinds = {}
    wins = {e: [0, 0] for e, _ in ENGINES[1:]}
    slow = []
    for ds, name in SETS[1:]:
        qpath = os.path.join(HERE, "queries", f"{ds}.tsv")
        t = {e: read_times(os.path.join(OUT, f"{e}_{ds}.times")) for e, _ in ENGINES}
        if not os.path.exists(qpath) or not t["vtext"]:
            continue
        for l in open(qpath, encoding="utf-8"):
            qid, _, text = l.rstrip("\n").partition("\t")
            if qid not in t["vtext"]:
                continue
            k = kind_of(text)
            for e, _ in ENGINES:
                if t[e] and qid in t[e]:
                    kinds.setdefault(k, {}).setdefault(e, []).append(t[e][qid])
            for e, _ in ENGINES[1:]:
                if t[e] and qid in t[e]:
                    wins[e][0] += t["vtext"][qid] < t[e][qid]
                    wins[e][1] += 1
                    if t[e][qid] < t["vtext"][qid]:
                        slow.append((t["vtext"][qid] - t[e][qid], name, text, e, t["vtext"][qid], t[e][qid]))
    if kinds:
        md += ["## Latency by kind of query (Enron, PubMed, Wikipedia; 1 client, client round trip, mean ms)", "",
               "| Kind | Queries | " + " | ".join(n for _, n in ENGINES) + " |", "|---|---:|" + "---:|" * len(ENGINES)]
        for k in ("words", "boolean", "phrase", "w/N, pre/N", "prefix / wildcard", "fuzzy"):
            if k in kinds:
                v = kinds[k]
                md.append(f"| {k} | {len(v.get('vtext', []))} | " + " | ".join(
                    fmt(sum(v[e]) / len(v[e]), 2) if v.get(e) else "-" for e, _ in ENGINES) + " |")
        md.append("")
        md += ["Queries where vtext is faster: " + ", ".join(f"{wins[e][0]} of {wins[e][1]} against {n}" for e, n in ENGINES[1:] if wins[e][1]) + ".", ""]
        slow.sort(reverse=True)
        if slow:
            md += ["Queries where an engine is faster than vtext, the largest differences first:", "",
                   "| Dataset | Query | Engine | vtext (ms) | Engine (ms) |", "|---|---|---|---:|---:|"]
            for d, name, text, e, a, b in slow[:15]:
                md.append(f"| {name} | `{text}` | {dict(ENGINES)[e]} | {fmt(a, 2)} | {fmt(b, 2)} |")
            md.append("")

    # refresh
    ref = jsonl("vtext_refresh.jsonl")
    if ref:
        md += ["## New rows (1% of MS MARCO)", "", "| Engine | Rows added | Searchable without refresh | Refresh (s) | Query with the rows in the live tail (ms) |", "|---|---:|---|---:|---:|"]
        for r in ref:
            if r["engine"] == "vtext":
                md.append(f"| vtext | {fmt(r['added'])} | yes (live tail) | {fmt(r['refresh_s'])} | {fmt(r['query_with_tail_ms'])} |")
            else:
                md.append(f"| {dict(ENGINES)[r['engine']]} | {fmt(r['added'])} | after refresh | {fmt(r['refresh_s'])} | - |")
        md.append("")

    with open(os.path.join(RES, "summary.md"), "w") as f:
        f.write("\n".join(md))
    print("\n".join(md))


if __name__ == "__main__":
    main()
