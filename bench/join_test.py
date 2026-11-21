#!/usr/bin/env python3
"""Search next to other data: vtext (a join in Vertica) against Elasticsearch / OpenSearch (an
attribute stored in every document), on Enron.

    python3 -I bench/join_test.py <vtext|es|os> [--dataset enron] [--queries bench/queries/enron.tsv]

Every document gets an attribute custodian (150 values: doc_id % 150, like the custodians of an
e-discovery case); 10 custodians are on legal hold (table / terms list).

    J1  add the attribute to every document
        vtext: CREATE TABLE <ds>_meta AS SELECT doc_id, doc_id % 150 ... (the text index is not touched)
        engines: _update_by_query with a script on every document (each one is written again)
    J2  move 1% of the documents to another custodian (doc_id % 100 = 7)
        vtext: UPDATE <ds>_meta ...; engines: _update_by_query on those documents
    J3  top 10 of each query among the documents of the custodians on hold
        vtext: tsearch (every match) JOIN <ds>_meta JOIN hold, ORDER BY score LIMIT 10
        engines: bool query with a terms filter on custodian
    J4  number of matches per custodian, for each query
        vtext: tmatch (every match, no scores) JOIN <ds>_meta GROUP BY custodian; engines: terms aggregation

The engines work on a clone of the dataset's index (<ds>_j, _clone API: no copy of the data),
deleted at the end; J3 and J4 are measured after a force merge of the clone into one segment, as
in the other tests. Times: J1 and J2 once (wall clock, until searchable: COMMIT / refresh);
J3 and J4 one client, each query 3 times after a warm-up, best time per query, mean over the
queries. The answers of J3 and J4 are written to bench/out/join_<engine>_*.tsv for comparison.
Prints one JSON line. Connection: VSQL_* for vtext, ES_PORT / OS_PORT for the engines.
"""
import http.client
import json
import os
import re
import subprocess
import sys
import time

http.client._MAXHEADERS = 10000     # OpenSearch sends a header per deprecation warning
HERE = os.path.dirname(os.path.abspath(__file__))
PORTS = {"es": int(os.environ.get("ES_PORT", 9200)), "os": int(os.environ.get("OS_PORT", 9201))}
CUSTODIANS, HOLD = 150, [3, 17, 29, 42, 58, 71, 86, 99, 113, 140]
REPEAT = 3


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def lit(s):
    return "'" + s.replace("'", "''") + "'"


def vsql(sql):
    t0 = time.time()
    p = subprocess.run(["vsql", "-X", "-A", "-t", "-q", "-v", "ON_ERROR_STOP=1", "-c", sql], capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError(p.stderr.strip())
    return p.stdout, time.time() - t0


def vsql_timed(stmts):
    """Runs statements in one session; returns (outputs, ms per statement from \\timing)."""
    script = "\\timing on\nSELECT 1;\n" + "".join(f"\\echo @@{i}\n{s}\n" for i, s in enumerate(stmts))
    p = subprocess.run(["vsql", "-X", "-A", "-t", "-F", "\t", "-f", "-"], input=script, capture_output=True, text=True)
    if "ERROR" in p.stderr:
        raise RuntimeError(p.stderr[:500])
    outs, ms, cur = [[] for _ in stmts], [None] * len(stmts), None
    for line in p.stdout.splitlines():
        if line.startswith("@@"):
            cur = int(line[2:])
            continue
        m = re.match(r"Time: First fetch.*All rows formatted: ([0-9.]+) ms", line)
        if m:
            if cur is not None:
                ms[cur] = float(m.group(1))
            continue
        if cur is not None and line.strip() and not line.startswith("Timing is"):
            outs[cur].append(line)
    return outs, ms


class Conn:
    def __init__(self, port):
        self.port = port
        self.c = http.client.HTTPConnection("127.0.0.1", port, timeout=3600)

    def req(self, method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        self.c.request(method, path, body=data, headers={"Content-Type": "application/json"})
        r = self.c.getresponse()
        out = json.loads(r.read() or b"{}")
        if r.status >= 300:
            raise RuntimeError(f"{method} {path}: HTTP {r.status}: {json.dumps(out)[:400]}")
        return out


def queries(path):
    return [tuple(l.rstrip("\n").split("\t", 1)) for l in open(path, encoding="utf-8") if "\t" in l]


def run_vtext(ds, qs, out_dir):
    res = {"engine": "vtext", "dataset": ds}
    vsql(f"DROP TABLE IF EXISTS bench.{ds}_meta; DROP TABLE IF EXISTS bench.{ds}_hold;")
    _, t = vsql(f"CREATE TABLE bench.{ds}_meta AS SELECT doc_id, (doc_id % {CUSTODIANS})::INT AS custodian FROM bench.{ds} "
                f"ORDER BY doc_id SEGMENTED BY HASH(doc_id) ALL NODES; COMMIT;")
    res["j1_add_attribute_s"] = round(t, 3)
    vsql(f"CREATE TABLE bench.{ds}_hold (custodian INT) UNSEGMENTED ALL NODES; "
         f"INSERT INTO bench.{ds}_hold " + " UNION ALL ".join(f"SELECT {c}" for c in HOLD) + "; COMMIT;")
    _, t = vsql(f"UPDATE bench.{ds}_meta SET custodian = (custodian + 1) % {CUSTODIANS} WHERE doc_id % 100 = 7; COMMIT;")
    res["j2_update_1pct_s"] = round(t, 3)
    res["j2_rows"] = int(vsql(f"SELECT COUNT(*) FROM bench.{ds}_meta WHERE doc_id % 100 = 7")[0])
    p = "mode='stem', max_expansions=100000, bm25='classic'"
    j3 = [f"SELECT r.doc_id, r.score FROM (SELECT vtext.tsearch(* USING PARAMETERS index='{ds}', query={lit(q)}, k=1000000, {p}) OVER() "
          f"FROM bench.{ds}_snap) r JOIN bench.{ds}_meta m USING (doc_id) JOIN bench.{ds}_hold h USING (custodian) "
          f"ORDER BY r.score DESC, r.doc_id LIMIT 10;" for _, q in qs]
    j4 = [f"SELECT m.custodian, COUNT(*) FROM (SELECT vtext.tmatch(* USING PARAMETERS index='{ds}', query={lit(q)}, {p}) OVER() "
          f"FROM bench.{ds}_snap) r JOIN bench.{ds}_meta m USING (doc_id) GROUP BY m.custodian ORDER BY m.custodian;" for _, q in qs]
    for name, stmts in (("j3", j3), ("j4", j4)):
        best, outs = [1e18] * len(stmts), None
        vsql_timed(stmts)   # warm-up
        for _ in range(REPEAT):
            o, ms = vsql_timed(stmts)
            outs = outs or o
            best = [min(b, m) if m is not None else b for b, m in zip(best, ms)]
        res[f"{name}_mean_ms"] = round(sum(best) / len(best), 2)
        with open(os.path.join(out_dir, f"join_vtext_{ds}_{name}.tsv"), "w") as f:
            for (qid, _), rows in zip(qs, outs):
                for r in rows:
                    a = r.split("\t")
                    f.write(f"{qid}\t{a[0]}\t{float(a[1]):.4f}\n" if name == "j3" else f"{qid}\t{a[0]}\t{a[1]}\n")
    return res


def run_engine(engine, ds, qpath, qs, out_dir):
    res = {"engine": engine, "dataset": ds}
    port = PORTS[engine]
    c = Conn(port)
    dump = os.path.join(out_dir, f"join_{engine}_{ds}.req")
    subprocess.run([sys.executable, "-I", os.path.join(HERE, "run_engines.py"), engine, ds, qpath, "--mode", "stem", "--dump", dump], check=True)
    bodies = [json.loads(l.split("\t", 1)[1])["query"] for l in open(dump, encoding="utf-8")]
    j = f"{ds}_j"
    try:
        c.req("DELETE", f"/{j}")
    except RuntimeError:
        pass
    c.req("PUT", f"/{ds}/_settings", {"index.blocks.write": True})
    c.req("POST", f"/{ds}/_clone/{j}", {"settings": {"index.blocks.write": None, "index.number_of_replicas": 0}})
    c.req("PUT", f"/{ds}/_settings", {"index.blocks.write": None})
    c.req("GET", f"/_cluster/health/{j}?wait_for_status=green&timeout=120s")
    c.req("PUT", f"/{j}/_mapping", {"properties": {"custodian": {"type": "integer"}}})
    t0 = time.time()
    r = c.req("POST", f"/{j}/_update_by_query?conflicts=proceed&slices=auto&refresh=true&wait_for_completion=true",
              {"script": {"source": f"ctx._source.custodian = (int)(ctx._source.doc_id % {CUSTODIANS})", "lang": "painless"}})
    res["j1_add_attribute_s"] = round(time.time() - t0, 3)
    res["j1_updated"] = r.get("updated")
    t0 = time.time()
    r = c.req("POST", f"/{j}/_update_by_query?conflicts=proceed&slices=auto&refresh=true&wait_for_completion=true",
              {"query": {"script": {"script": {"source": "doc['doc_id'].value % 100 == 7", "lang": "painless"}}},
               "script": {"source": f"ctx._source.custodian = (ctx._source.custodian + 1) % {CUSTODIANS}", "lang": "painless"}})
    res["j2_update_1pct_s"] = round(time.time() - t0, 3)
    res["j2_rows"] = r.get("updated")
    t0 = time.time()
    c.req("POST", f"/{j}/_forcemerge?max_num_segments=1")
    res["merge_after_updates_s"] = round(time.time() - t0, 1)
    c.req("POST", f"/{j}/_refresh")
    j3 = [{"query": {"bool": {"must": [b], "filter": [{"terms": {"custodian": HOLD}}]}}, "size": 10, "_source": False,
           "track_total_hits": False} for b in bodies]
    j4 = [{"query": b, "size": 0, "track_total_hits": False, "aggs": {"c": {"terms": {"field": "custodian", "size": CUSTODIANS}}}}
          for b in bodies]
    for name, reqs in (("j3", j3), ("j4", j4)):
        best, outs = [1e18] * len(reqs), [None] * len(reqs)
        for run in range(REPEAT + 1):
            for i, b in enumerate(reqs):
                t0 = time.perf_counter()
                o = c.req("POST", f"/{j}/_search", b)
                ms = (time.perf_counter() - t0) * 1000
                if run:
                    best[i] = min(best[i], ms)
                    outs[i] = outs[i] or o
        res[f"{name}_mean_ms"] = round(sum(best) / len(best), 2)
        with open(os.path.join(out_dir, f"join_{engine}_{ds}_{name}.tsv"), "w") as f:
            for (qid, _), o in zip(qs, outs):
                if name == "j3":
                    for h in o["hits"]["hits"]:
                        f.write(f"{qid}\t{h['_id']}\t{h['_score']:.4f}\n")
                else:
                    for bkt in sorted(o["aggregations"]["c"]["buckets"], key=lambda x: x["key"]):
                        f.write(f"{qid}\t{bkt['key']}\t{bkt['doc_count']}\n")
    c.req("DELETE", f"/{j}")
    return res


def main():
    engine = sys.argv[1]
    ds = arg("--dataset", "enron")
    qpath = arg("--queries", os.path.join(HERE, "queries", f"{ds}.tsv"))
    out_dir = os.path.join(HERE, "out")
    os.makedirs(out_dir, exist_ok=True)
    qs = queries(qpath)
    res = run_vtext(ds, qs, out_dir) if engine == "vtext" else run_engine(engine, ds, qpath, qs, out_dir)
    res["queries"] = len(qs)
    print(json.dumps(res))


if __name__ == "__main__":
    main()
