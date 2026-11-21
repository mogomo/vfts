#!/usr/bin/env python3
"""Tables and charts from the recorded results (bench/run_all.sh).

    python3 -I bench/report.py

Reads bench/results/*.jsonl, env.json and the run files (bench/out, copied to bench/results/runs); writes
bench/results/summary.md (all tables) and bench/results/charts/*.svg. Only recorded numbers are
used. For a value measured more than once, the last record wins. Python standard library only.
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "results")
RAW = os.path.join(HERE, "out")            # written by bench/run_all.sh (not in git)
OUT = os.path.join(HERE, "results", "runs")  # the hits and times of every query run, kept with the results
CHARTS = os.path.join(RES, "charts")
ENGINES = [("vtext", "vtext (Vertica)"), ("es", "Elasticsearch"), ("os", "OpenSearch")]
# query results: vtext with the live view (every committed row) and the snap view (the rows up to the
# last refresh: the guarantee of Elasticsearch and OpenSearch)
Q_ENGINES = [("vtext", "vtext live"), ("vtext_snap", "vtext snap"), ("es", "Elasticsearch"), ("os", "OpenSearch")]
# categorical slots 1-3 of the validated reference palette (light, dark): fixed per engine
COLORS = {"vtext": ("#2a78d6", "#3987e5"), "es": ("#eb6834", "#d95926"), "os": ("#1baf7a", "#199e70"),
          # vtext sent as batches / one statement per query (throughput.json): palette slots 4 and 7
          "vtext_b": ("#eda100", "#c98500"), "vtext_s": ("#6250d6", "#9085e9")}
# the native-client comparison (bench/run_all.sh throughput -> throughput.json)
T_ENGINES = [("vtext", "vtext, tserve"), ("vtext_b", "vtext, batches"), ("vtext_s", "vtext, 1 statement per query"),
             ("es", "Elasticsearch"), ("os", "OpenSearch")]
T_KEYS = {"vtext": "vtext_serve", "vtext_b": "vtext_batch", "vtext_s": "vtext_single", "es": "es_native", "os": "os_native"}
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


def svg_bars(path, title, unit, groups, better, series=None):
    """Horizontal grouped bars: groups = [(label, {engine: value})]. Values labelled at the bar tips.
    vtext snap is the hatched blue of vtext (the same product, the other view)."""
    series = series or ENGINES
    bar, gap, ggap, left, right, top = 14, 3, 18, 120, 90, 80
    engines = [e for e, _ in series if any(v.get(e) is not None for _, v in groups)]
    height = top + sum(len(engines) * (bar + gap) + ggap for _ in groups) + 40
    width = 720
    vmax = max([v for _, g in groups for v in g.values() if v is not None] + [1e-9])
    scale = (width - left - right) / vmax
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img" aria-label="{title}">',
         "<style>",
         ".bg{fill:#fcfcfb}.t1{fill:#0b0b0b}.t2{fill:#52514e}.ax{stroke:#d9d8d4}",
         "".join(f".{e}{{fill:{c[0]}}}" for e, c in COLORS.items()),
         f".vtext_snap{{fill:url(#hatch);stroke:{COLORS['vtext'][0]};stroke-width:1}}",
         "@media (prefers-color-scheme: dark){.bg{fill:#1a1a19}.t1{fill:#f0efec}.t2{fill:#c3c2b7}.ax{stroke:#3a3a38}"
         + "".join(f".{e}{{fill:{c[1]}}}" for e, c in COLORS.items())
         + f".vtext_snap{{fill:url(#hatch);stroke:{COLORS['vtext'][1]}}}" + "}",
         "text{font-family:-apple-system,Segoe UI,Helvetica,Arial,sans-serif}",
         "</style>",
         '<defs><pattern id="hatch" patternUnits="userSpaceOnUse" width="6" height="6" patternTransform="rotate(45)">'
         '<rect class="bg" width="6" height="6"/><rect class="vtext" width="3" height="6"/></pattern></defs>',
         f'<rect class="bg" width="{width}" height="{height}" rx="6"/>',
         f'<text class="t1" x="16" y="24" font-size="15" font-weight="600">{title}</text>',
         f'<text class="t2" x="16" y="42" font-size="12">{unit}, {better}</text>']
    # legend: its own row under the subtitle, left to right
    lx = 16
    for e, name in [x for x in series if x[0] in engines]:
        s.append(f'<rect class="{e}" x="{lx}" y="52" width="10" height="10" rx="2"/>')
        s.append(f'<text class="t2" x="{lx + 14}" y="61" font-size="12">{name}</text>')
        lx += 7.2 * len(name) + 32
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


def svg_glance(path, f):
    """One-page summary card: hero numbers (each from a recorded result) and the throughput ratio
    per collection. f: dict of facts; tiles whose fact is missing are left out."""
    W, H = 960, 490
    blue, orange, aqua = COLORS["vtext"], COLORS["es"], COLORS["os"]
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" role="img" aria-label="vtext at a glance">',
         "<style>.bg{fill:#fcfcfb}.tile{fill:#f2f1ee}.t1{fill:#0b0b0b}.t2{fill:#52514e}.ax{stroke:#bdbcb7}"
         f".v{{fill:{blue[0]}}}.e{{fill:{orange[0]}}}"
         "@media (prefers-color-scheme: dark){.bg{fill:#1a1a19}.tile{fill:#262624}.t1{fill:#f0efec}.t2{fill:#c3c2b7}.ax{stroke:#55544f}"
         f".v{{fill:{blue[1]}}}.e{{fill:{orange[1]}}}}}"
         "text{font-family:-apple-system,Segoe UI,Helvetica,Arial,sans-serif}</style>",
         f'<rect class="bg" width="{W}" height="{H}" rx="8"/>',
         '<text class="t1" x="24" y="38" font-size="20" font-weight="700">vtext: full-text search inside Vertica, against Elasticsearch and OpenSearch</text>',
         '<text class="t2" x="24" y="60" font-size="13">One machine, the same documents, analyzers and queries; every number is a recorded run (bench/results/).</text>']
    tiles = []
    if f.get("tp_min"):
        tiles.append((f"{f['tp_min']:.1f}-{f['tp_max']:.1f}x", "queries per second, 32 clients",
                      f"tserve vs the faster engine, {f['tp_wins']} of {f['tp_sets']} collections"))
    if f.get("same_pct"):
        tiles.append((f"{f['same_pct']:.0f}%", "same top 10, same scores",
                      f"MS MARCO; MRR@10 {f['mrr_v']:.4f} vs {f['mrr_e']:.4f}"))
    if f.get("j2_v") and f.get("j2_e"):
        tiles.append((f"{f['j2_e'] / f['j2_v']:.0f}x faster", "change an attribute of 1% of docs",
                      f"UPDATE {f['j2_v']:.3f} s vs rewrite {f['j2_e']:.1f} s"))
    if f.get("size_lo"):
        tiles.append((f"{f['size_lo']:.2f}-{f['size_hi']:.2f}x", "index size of the engines'",
                      "the text is not copied out of Vertica"))
    tw = (W - 48 - 16 * (len(tiles) - 1)) / max(len(tiles), 1)
    for i, (big, l1, l2) in enumerate(tiles):
        x = 24 + i * (tw + 16)
        s += [f'<rect class="tile" x="{x}" y="80" width="{tw}" height="112" rx="6"/>',
              f'<text class="t1" x="{x + 16}" y="124" font-size="30" font-weight="700">{big}</text>',
              f'<text class="t1" x="{x + 16}" y="150" font-size="13" font-weight="600">{l1}</text>',
              f'<text class="t2" x="{x + 16}" y="172" font-size="12">{l2}</text>']
    # ratio bars: tserve q/s divided by the faster engine's, per collection; 1.0x line
    ratios = f.get("ratios", [])
    if ratios:
        top, left, bw, bh, gap = 236, 170, W - 170 - 110, 22, 10
        vmax = max(2.0, max(r for _, r in ratios))
        sc = bw / vmax
        s.append('<text class="t1" x="24" y="222" font-size="14" font-weight="600">Throughput, 32 clients, one query per request: vtext tserve / the faster of Elasticsearch and OpenSearch</text>')
        for i, (name, r) in enumerate(ratios):
            y = top + i * (bh + gap)
            w = r * sc
            s += [f'<text class="t1" x="{left - 10}" y="{y + 16}" font-size="13" text-anchor="end">{name}</text>',
                  f'<path class="v" d="M{left},{y} h{w - 4} a4,4 0 0 1 4,4 v{bh - 8} a4,4 0 0 1 -4,4 h{-(w - 4)} z"><title>{name}: {r:.2f}x</title></path>',
                  f'<text class="t2" x="{left + w + 8}" y="{y + 16}" font-size="12">{r:.2f}x</text>']
        x1 = left + sc
        y_end = top + len(ratios) * (bh + gap) - gap
        s += [f'<line class="ax" x1="{x1}" y1="{top - 6}" x2="{x1}" y2="{y_end + 6}" stroke-width="1.5" stroke-dasharray="4 3"/>',
              f'<text class="t2" x="{x1}" y="{y_end + 22}" font-size="11" text-anchor="middle">1.0x = the faster engine</text>']
    for i, line in enumerate(f.get("lose", [])):
        s.append(f'<text class="t2" x="24" y="{H - 38 + 17 * i}" font-size="12">{line}</text>')
    s.append("</svg>")
    with open(path, "w") as fh:
        fh.write("\n".join(s))


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


def copy_runs():
    """The run files (hits per query, times per query) of bench/out into bench/results/runs, so
    that the tables can be computed again from the repository alone."""
    import shutil
    os.makedirs(OUT, exist_ok=True)
    if os.path.isdir(RAW):
        for f in os.listdir(RAW):
            if f.endswith((".run", ".times")):
                shutil.copyfile(os.path.join(RAW, f), os.path.join(OUT, f))


def main():
    os.makedirs(CHARTS, exist_ok=True)
    copy_runs()
    env = json.load(open(os.path.join(RES, "env.json"))) if os.path.exists(os.path.join(RES, "env.json")) else {}
    load = {e: jsonl(f"{e}_load.jsonl") for e, _ in ENGINES}
    query = {e: jsonl(f"{e}_query.jsonl") for e, _ in ENGINES}
    vt = query["vtext"]
    query["vtext"] = [x for x in vt if x.get("view", "live") == "live"]
    query["vtext_snap"] = [x for x in vt if x.get("view") == "snap"]
    md = ["# Benchmark results", "", "Generated by `bench/report.py` from `bench/results/*.jsonl`. Every number is a recorded run.", ""]
    if env:
        md += ["## Machine", "", "| | |", "|---|---|"] + [f"| {k} | {v} |" for k, v in env.items()] + [""]

    # loading
    md += ["## Index build", "",
           "vtext: `CALL vtext.create_index` (analysis, index tables, node cache) + `CALL vtext.merge(name, 1)` (one segment); "
           "the COPY into the source table is listed apart. Elasticsearch / OpenSearch: bulk load + refresh + force merge to one segment. Sizes: vtext node cache file and index tables; "
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
        svg_bars(os.path.join(CHARTS, "size.svg"), "Index size",
                 "MB; vtext: node cache (the text stays in its table); engines: store with their text copy",
                 size_g, "lower is better")

    # queries
    for clients, title in ((1, "Latency, 1 client (vtext through vsql, one statement per query)"),
                           (32, "Throughput, 32 clients (first run: vsql per client, engines through one Python process)")):
        md += [f"## {title}", ""]
        if clients == 32:
            md += ["Kept for the record. The engines' client here was one Python process with 32 threads, which limited them "
                   "(Elasticsearch on PubMed, same 1,000 requests: 4,138 q/s through it, 5,943 through a native client). Use the section "
                   "'Throughput and latency through one native client' below.", ""]
        md += [
               "Server time: Vertica `v_monitor.query_requests` (statement start to end) / the engines' `took`. Client time: the whole "
               "round trip seen by the client (vsql `\\timing` / HTTP). Best of the measured runs per query, after a warm-up.", "",
               "| Dataset | Engine | Queries | Server mean (ms) | Server p50 | Server p99 | Client mean (ms) | Client p50 | Client p99 | Queries/s |",
               "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
        lat_g, qps_g = [], []
        for ds, name in SETS:
            vals_l, vals_q = {}, {}
            for e, ename in Q_ENGINES:
                r = last([x for x in query[e] if not x.get("count") and not x.get("fenced") and x.get("clients") == clients
                          and x.get("batch", 1) == 1 and not x.get("session")], index=ds)
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
            svg_bars(os.path.join(CHARTS, "latency.svg"), "Mean query latency, 1 client", "milliseconds, client round trip", lat_g, "lower is better", Q_ENGINES)
        if clients == 32 and qps_g:
            svg_bars(os.path.join(CHARTS, "throughput.svg"), "Throughput, 32 clients", "queries per second", qps_g, "higher is better", Q_ENGINES)

    # batches: many queries per statement (vtext: the parameter queries; the engines: _msearch)
    rows = []
    for ds, name in SETS:
        for e, ename in Q_ENGINES:
            b = last([x for x in query[e] if x.get("batch", 1) > 1 and x.get("clients") == 32 and not x.get("session")], index=ds)
            d = last([x for x in query[e] if x.get("batch", 1) == 1 and x.get("clients") == 32 and not x.get("session")
                      and not x.get("count") and not x.get("fenced")], index=ds)
            if b:
                rows.append(f"| {name} | {ename} | {b['batch']} | {fmt(d and d['qps_best'])} | {fmt(b['qps_best'])} |")
    if rows:
        md += ["## Batches (32 clients)", "",
               "Many queries in one request: vtext's parameter `queries` (one statement), the engines' `_msearch`. "
               "Queries per second over the same query sets.", "",
               "| Dataset | Engine | Queries per request | Queries/s, one per request | Queries/s, batches |", "|---|---|---:|---:|---:|"] + rows + [""]

    # a resource pool for search (bench/pool_test.sh)
    rows = []
    for ds, name in SETS:
        for e, ename in Q_ENGINES[:2]:
            r = last([x for x in query[e] if x.get("session") and x.get("clients") == 32], index=ds)
            d = last([x for x in query[e] if not x.get("session") and not x.get("count") and not x.get("fenced")
                      and x.get("clients") == 32 and x.get("batch", 1) == 1], index=ds)
            if r:
                rows.append(f"| {name} | {ename} | {fmt(d and d['qps_best'])} | {fmt(r['qps_best'])} | {fmt(r['client_ms']['mean'], 2)} |")
    if rows:
        md += ["## Throughput with a resource pool for search (vtext, 32 clients)", "",
               "`bench/pool_test.sh`: `CREATE RESOURCE POOL vtext_search ... EXECUTIONPARALLELISM 1 PLANNEDCONCURRENCY 64` and "
               "`SET SESSION RESOURCE_POOL = vtext_search` in every session.", "",
               "| Dataset | View | Queries/s, general pool | Queries/s, search pool | Client mean (ms), search pool |", "|---|---|---:|---:|---:|"] + rows + [""]

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
        md += ["## Relevance (MS MARCO passage, dev, 6,980 judged queries)", "", "| Engine | MRR@10 | nDCG@10 |", "|---|---:|---:|"]
        for e, ename in ENGINES:
            if e in rel:
                md.append(f"| {ename} | {rel[e]['mrr@10']} | {rel[e]['ndcg@10']} |")
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
    pairs = [(v, e) for v in ("vtext", "vtext_snap") for e in ("es", "os")]
    wins = {pq: [0, 0] for pq in pairs}
    slow = []
    for ds, name in SETS[1:]:
        qpath = os.path.join(HERE, "queries", f"{ds}.tsv")
        t = {e: read_times(os.path.join(OUT, f"vtext_{ds}_snap.times" if e == "vtext_snap" else f"{e}_{ds}.times")) for e, _ in Q_ENGINES}
        if not os.path.exists(qpath) or not t["vtext"]:
            continue
        for l in open(qpath, encoding="utf-8"):
            qid, _, text = l.rstrip("\n").partition("\t")
            if qid not in t["vtext"]:
                continue
            k = kind_of(text)
            for e, _ in Q_ENGINES:
                if t[e] and qid in t[e]:
                    kinds.setdefault(k, {}).setdefault(e, []).append(t[e][qid])
            for v, e in pairs:
                if t[v] and t[e] and qid in t[v] and qid in t[e]:
                    wins[(v, e)][0] += t[v][qid] < t[e][qid]
                    wins[(v, e)][1] += 1
                    if v == "vtext_snap" and t[e][qid] < t[v][qid]:
                        slow.append((t[v][qid] - t[e][qid], name, text, e, t[v][qid], t[e][qid]))
    if kinds:
        md += ["## Latency by kind of query (Enron, PubMed, Wikipedia; 1 client, client round trip, mean ms)", "",
               "| Kind | Queries | " + " | ".join(n for _, n in Q_ENGINES) + " |", "|---|---:|" + "---:|" * len(Q_ENGINES)]
        for k in ("words", "boolean", "phrase", "w/N, pre/N", "prefix / wildcard", "fuzzy"):
            if k in kinds:
                v = kinds[k]
                md.append(f"| {k} | {len(v.get('vtext', []))} | " + " | ".join(
                    fmt(sum(v[e]) / len(v[e]), 2) if v.get(e) else "-" for e, _ in Q_ENGINES) + " |")
        md.append("")
        names = dict(Q_ENGINES)
        md += ["Queries where vtext is faster: " + "; ".join(f"{names[v]} {wins[(v, e)][0]} of {wins[(v, e)][1]} against {names[e]}"
                                                         for v, e in pairs if wins[(v, e)][1]) + ".", ""]
        slow.sort(reverse=True)
        if slow:
            md += ["Queries where an engine is faster than vtext snap, the largest differences first:", "",
                   "| Dataset | Query | Engine | vtext snap (ms) | Engine (ms) |", "|---|---|---|---:|---:|"]
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

    # throughput and latency with one native HTTP client for every engine (bench/plan_c/stage1.sh f1)
    tp = os.path.join(RES, "throughput.json")
    if os.path.exists(tp):
        t = json.load(open(tp))
        md += ["## Throughput and latency through one native client (step throughput)", "",
               "32 closed-loop clients (tools/httpload, keep-alive HTTP), one query per request; 3 rounds in alternating order, "
               "median of the rounds' best q/s; CPU governor performance. vtext through tools/vtext_broker: tserve (one "
               "statement per session, searches over a channel), batches (8 sessions, idle-else-queue), one statement per query "
               "(32 sessions). Full table with ranges: throughput.md.", "",
               "| Dataset | " + " | ".join(n for _, n in T_ENGINES) + " |", "|---|" + "---:|" * len(T_ENGINES)]
        q_g, l_g = [], []
        for ds, name in SETS:
            row, vq, vl = [], {}, {}
            for e, _ in T_ENGINES:
                x = t.get(T_KEYS[e], {}).get(ds)
                y = t.get(T_KEYS[e] + "_1client", {}).get(ds)
                row.append(fmt(x and x["qps_median"], 0))
                if x:
                    vq[e] = x["qps_median"]
                if y:
                    vl[e] = y["ms_mean"]
            md.append(f"| {name} | " + " | ".join(row) + " |")
            if vq:
                q_g.append((name, vq))
            if vl:
                l_g.append((name, vl))
        md += ["", "One client, mean ms per query (best of 3 runs per query, median of the rounds):", "",
               "| Dataset | " + " | ".join(n for e, n in T_ENGINES if e != "vtext_b") + " |", "|---|" + "---:|" * (len(T_ENGINES) - 1)]
        for ds, name in SETS:
            md.append(f"| {name} | " + " | ".join(fmt((t.get(T_KEYS[e] + "_1client", {}).get(ds) or {}).get("ms_mean"), 2)
                                                  for e, _ in T_ENGINES if e != "vtext_b") + " |")
        md.append("")
        if q_g:
            svg_bars(os.path.join(CHARTS, "throughput.svg"), "Throughput, 32 clients", "queries per second, one query per request", q_g,
                     "higher is better", T_ENGINES)
        if l_g:
            svg_bars(os.path.join(CHARTS, "latency_http.svg"), "Mean latency, 1 client, same HTTP client for all", "milliseconds", l_g,
                     "lower is better", [x for x in T_ENGINES if x[0] != "vtext_b"])

    # search next to other data (bench/join_test.py -> join.jsonl)
    jn = {r["engine"]: r for r in jsonl("join.jsonl")}
    if jn:
        md += ["## Search next to other data (Enron, bench/join_test.py)", "",
               "An attribute custodian (150 values) for every document; 10 custodians on hold. vtext: a table joined at query time; "
               "the engines: a field in every document (_update_by_query on a clone of the index). J3 and J4: 40 queries, one "
               "client, best of 3 per query, mean.", "",
               "| Test | " + " | ".join(dict(ENGINES)[e] for e in ("vtext", "es", "os") if e in jn) + " |",
               "|---|" + "---:|" * len(jn)]
        for key, label, unit in (("j1_add_attribute_s", "J1 add the attribute to every document", "s"),
                                 ("j2_update_1pct_s", "J2 change it for 1% of the documents", "s"),
                                 ("j3_mean_ms", "J3 top 10 among the custodians on hold", "ms"),
                                 ("j4_mean_ms", "J4 matches per custodian", "ms")):
            md.append(f"| {label} ({unit}) | " + " | ".join(fmt(jn[e].get(key), 3 if unit == "s" else 2) for e in ("vtext", "es", "os") if e in jn) + " |")
        md.append("")

    # the summary card
    facts = {}
    if os.path.exists(tp):
        t = json.load(open(tp))
        ratios = []
        for ds, name in SETS:
            v = t.get("vtext_serve", {}).get(ds)
            e = [t.get(k, {}).get(ds) for k in ("es_native", "os_native")]
            e = [x["qps_median"] for x in e if x]
            if v and e:
                ratios.append((name, v["qps_median"] / max(e)))
        if ratios:
            facts.update(ratios=ratios, tp_min=min(r for _, r in ratios), tp_max=max(r for _, r in ratios),
                         tp_wins=sum(r > 1 for _, r in ratios), tp_sets=len(ratios))
    a = top_agreement("msmarco", "os")
    if a and a["queries"]:
        facts["same_pct"] = 100.0 * a["same_scores"] / a["queries"]
        if rel and "vtext" in rel and "es" in rel:
            facts["mrr_v"], facts["mrr_e"] = rel["vtext"]["mrr@10"], rel["es"]["mrr@10"]
        else:
            facts.pop("same_pct")
    sz = [g["vtext"] / min(x for x in (g.get("es"), g.get("os")) if x) for _, g in size_g if g.get("vtext") and (g.get("es") or g.get("os"))]
    if sz:
        facts["size_lo"], facts["size_hi"] = min(sz), max(sz)
    if "vtext" in jn and ("es" in jn or "os" in jn):
        facts["j2_v"] = jn["vtext"]["j2_update_1pct_s"]
        facts["j2_e"] = min(jn[e]["j2_update_1pct_s"] for e in ("es", "os") if e in jn)
    facts["lose"] = ["Where Elasticsearch and OpenSearch win: one SQL statement per search (a third to an eighth of their throughput),",
                     "filtering or counting matches by an attribute (1-3 ms vs 17-18 ms), PubMed with one client, phrases of very frequent words."]
    if facts.get("ratios"):
        svg_glance(os.path.join(CHARTS, "glance.svg"), facts)

    # the article uses the same charts
    blog = os.path.join(HERE, "..", "blog", "charts")
    if os.path.isdir(blog):
        import shutil
        for c in os.listdir(CHARTS):
            if c.endswith(".svg"):
                shutil.copyfile(os.path.join(CHARTS, c), os.path.join(blog, c))

    with open(os.path.join(RES, "summary.md"), "w") as f:
        f.write("\n".join(md))
    print("\n".join(md))


if __name__ == "__main__":
    main()
