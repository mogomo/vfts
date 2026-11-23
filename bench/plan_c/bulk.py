#!/usr/bin/env python3
"""Bulk searches: a whole query set at once, timed from the start until every result is stored.

    python3 -I bench/plan_c/bulk.py vfts <index> <queries.tsv> [options]
    python3 -I bench/plan_c/bulk.py es|os <index> <queries.tsv> [options]

Both sides get the same queries (the engines through bench/run_engines.py's translation, as in
the other tests) and store every result: vfts in a file (vsql -o) or a table (--into), the
engines in a file. The time is the client's clock, from the first request to the last result
stored, best of --repeat runs after --warmup runs (all runs are printed too). Connections are
open before the clock starts, as in an application: every vsql session logs in, then waits at a
barrier (a file) that opens when all are ready; the engines' HTTP connections are made first.

    --count          match counts (tcount; the engines: size 0, track_total_hits) instead of top k
    --plain          plain text queries (syntax='plain'; the engines: a match query)
    --k N            hits per query (default 10)
    --repeat R       measured runs (default 3)     --warmup W   (default 1)
    --out FILE       results of the last run: qid<TAB>doc_id<TAB>rank<TAB>score, or qid<TAB>count

  vfts:
    --schema S --view snap|live     the index's views (default bench, snap)
    --split P        P statements at once, each with 1/P of the queries (default 1)
    --hosts a,b,...  statement i connects to host i mod len (default VSQL_HOST): one per node
    --into TABLE     INSERT the results into TABLE (made by this script, segmented by qid) and COMMIT, instead of a file
    --param x=y      more parameters of tsearch / tcount

  es | os:
    --batch B        queries per _msearch request (default 100)
    --clients C      requests at once (default 1)
    --mcs N          max_concurrent_searches of each _msearch (default: the engine's)
    --no-cache       request_cache=false in every search (counts: no answers from the cache)

Prints one JSON line. vsql connection: VSQL_* environment. Python standard library only.
"""
import json
import os
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import run_engines  # noqa: E402  (Conn, Translator, PORTS)


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def lit(s):
    return "'" + s.replace("'", "''") + "'"


def load_queries(path):
    q = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            qid, _, text = line.rstrip("\n").partition("\t")
            if text:
                q.append((qid, text))
    return q


def run_vfts(index, queries, counting, plain, k, repeat, warmup, out_path):
    schema = arg("--schema", "bench")
    view = arg("--view", "snap")
    split = int(arg("--split", 1))
    hosts = [h for h in arg("--hosts", os.environ.get("VSQL_HOST", "")).split(",")]
    into = arg("--into")
    extra = [sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == "--param"]
    fn = "tcount" if counting else "tsearch"
    params = [f"index={lit(index)}"] + ([] if counting else [f"k={k}"]) + (["syntax='plain'"] if plain else []) + extra
    cols = "qid, count" if counting else "qid, doc_id, rank, score"
    parts = [queries[i::split] for i in range(split)]
    if into:
        typ = "qid INT, count INT" if counting else "qid INT, doc_id INT, rank INT, score FLOAT"
        subprocess.run(["vsql", "-X", "-q", "-c", f"DROP TABLE IF EXISTS {into} CASCADE; CREATE TABLE {into} ({typ}) SEGMENTED BY HASH(qid) ALL NODES;"],
                       check=True, capture_output=True)

    def statement(part):
        lines = "\n".join(f"{qid}\t{text}" for qid, text in part)
        sel = (f"SELECT {cols} FROM (SELECT vfts.{fn}(* USING PARAMETERS queries={lit(lines)}, {', '.join(params)}) "
               f"OVER() FROM {schema}.{index}_{view}) r")
        if into:
            return f"INSERT /*+DIRECT*/ INTO {into} {sel};\nCOMMIT;\n"
        return sel + ";\n"

    scripts = [statement(p) for p in parts]
    tmp = tempfile.mkdtemp(prefix="vfts_bulk_")

    def once():
        if into:
            subprocess.run(["vsql", "-X", "-q", "-c", f"TRUNCATE TABLE {into};"], check=True, capture_output=True)
        errs = []
        procs = []
        go = os.path.join(tmp, "go")
        if os.path.exists(go):
            os.remove(go)
        for i, sc in enumerate(scripts):
            env = dict(os.environ)
            if hosts[i % len(hosts)]:
                env["VSQL_HOST"] = hosts[i % len(hosts)]
            ready = os.path.join(tmp, f"ready{i}")
            if os.path.exists(ready):
                os.remove(ready)
            out = "/dev/null" if into else os.path.join(tmp, f"part{i}.tsv")
            # log in and run one statement, say ready, wait for the barrier, then the batch
            script = (f"\\o /dev/null\nSELECT 1;\n\\! touch {ready}\n"
                      f"\\! while [ ! -e {go} ]; do sleep 0.0002; done\n\\o {out}\n" + sc)
            cmd = ["vsql", "-X", "-A", "-t", "-q", "-F", "\t", "-v", "ON_ERROR_STOP=1", "-f", "-"]
            p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
            procs.append((p, script))

        def feed(p, sc):
            _, e = p.communicate(sc)
            if p.returncode != 0 or "ERROR" in e:
                errs.append(e[:400])

        th = [threading.Thread(target=feed, args=x) for x in procs]
        for t in th:
            t.start()
        while not all(os.path.exists(os.path.join(tmp, f"ready{i}")) for i in range(len(scripts))):
            time.sleep(0.0005)
        t0 = time.perf_counter()
        open(go, "w").close()
        for t in th:
            t.join()
        return time.perf_counter() - t0, errs

    runs = []
    for _ in range(warmup):
        once()
    errors = []
    for _ in range(repeat):
        s, e = once()
        runs.append(round(s, 3))
        errors += e
    # results of the last run
    rows = []
    if into:
        out = subprocess.run(["vsql", "-X", "-A", "-t", "-q", "-F", "\t", "-c", f"SELECT {cols} FROM {into} ORDER BY 1{', 3' if not counting else ''}"],
                             capture_output=True, text=True, check=True).stdout
        rows = [l for l in out.splitlines() if l]
    else:
        for i in range(split):
            with open(os.path.join(tmp, f"part{i}.tsv")) as f:
                rows += [l.rstrip("\n") for l in f if l.strip()]
    if out_path:
        with open(out_path, "w") as f:
            for r in rows:
                f.write(r + "\n")
    return runs, errors, len(rows), {"split": split, "hosts": len([h for h in hosts if h]) or 1, "into": bool(into), "params": extra}


def run_engine(engine, index, queries, counting, plain, k, repeat, warmup, out_path, qpath):
    batch = int(arg("--batch", 100))
    clients = int(arg("--clients", 1))
    mcs = arg("--mcs")
    no_cache = "--no-cache" in sys.argv
    port = run_engines.PORTS[engine]
    conn = run_engines.Conn(port)
    field = "body"
    if plain:
        bodies = [{"match": {field: {"query": text}}} for _, text in queries]
    else:
        out = subprocess.run([arg("--cli", "build/tools/vfts_cli"), "explain", qpath, "--lang", arg("--lang", "en")],
                             capture_output=True, text=True, check=True).stdout
        tr = run_engines.Translator(conn, index, "stem")
        bodies = []
        for l in out.splitlines():
            t = json.loads(l)
            bodies.append({"match_none": {}} if t["tree"]["k"] == "error" else tr.query(t["tree"]))
    if counting:
        reqs = [{"query": b, "size": 0, "track_total_hits": True} for b in bodies]
    else:
        reqs = [{"query": b, "size": k, "_source": False, "track_total_hits": False} for b in bodies]
    head = json.dumps({"request_cache": False} if no_cache else {})
    groups = [list(range(i, min(i + batch, len(reqs)))) for i in range(0, len(reqs), batch)]
    path = f"/{index}/_msearch" + (f"?max_concurrent_searches={mcs}" if mcs else "")
    bodies_nd = ["".join(head + "\n" + json.dumps(reqs[j]) + "\n" for j in g) for g in groups]

    def once(result_path):
        nxt = [0]
        lock = threading.Lock()
        res = [None] * len(groups)
        errs = []

        conns = [run_engines.Conn(port) for _ in range(clients)]
        for c in conns:          # connected before the clock starts
            c.post(f"/{index}/_count", {"query": {"match_none": {}}})

        def work(c):
            while True:
                with lock:
                    i = nxt[0]
                    nxt[0] += 1
                if i >= len(groups):
                    return
                st, out = c.post(path, bodies_nd[i])
                if st >= 300:
                    errs.append(f"HTTP {st}: {json.dumps(out)[:300]}")
                    continue
                res[i] = out["responses"]

        t0 = time.perf_counter()
        th = [threading.Thread(target=work, args=(c,)) for c in conns]
        for t in th:
            t.start()
        for t in th:
            t.join()
        n = 0
        with open(result_path, "w") as f:
            for g, rs in zip(groups, res):
                if rs is None:
                    continue
                for j, r in zip(g, rs):
                    if "error" in r:
                        errs.append(json.dumps(r["error"])[:300])
                        continue
                    qid = queries[j][0]
                    if counting:
                        f.write(f"{qid}\t{r['hits']['total']['value']}\n")
                        n += 1
                    else:
                        for rank, h in enumerate(r["hits"]["hits"], 1):
                            f.write(f"{qid}\t{h['_id']}\t{rank}\t{h['_score']:.7f}\n")
                            n += 1
        return time.perf_counter() - t0, errs, n

    tmp = out_path or tempfile.mktemp(prefix="engine_bulk_")
    for _ in range(warmup):
        once(tmp)
    runs, errors, n = [], [], 0
    for _ in range(repeat):
        s, e, n = once(tmp)
        runs.append(round(s, 3))
        errors += e
    return runs, errors, n, {"batch": batch, "clients": clients, "mcs": int(mcs) if mcs else None, "request_cache": not no_cache}


def main():
    engine, index, qpath = sys.argv[1], sys.argv[2], sys.argv[3]
    counting = "--count" in sys.argv
    plain = "--plain" in sys.argv
    k = int(arg("--k", 10))
    repeat = int(arg("--repeat", 3))
    warmup = int(arg("--warmup", 1))
    out_path = arg("--out")
    queries = load_queries(qpath)
    if engine == "vfts":
        runs, errors, rows, cfg = run_vfts(index, queries, counting, plain, k, repeat, warmup, out_path)
    else:
        runs, errors, rows, cfg = run_engine(engine, index, queries, counting, plain, k, repeat, warmup, out_path, qpath)
    best = min(runs) if runs else 0
    print(json.dumps({"engine": engine, "index": index, "queries": len(queries), "count": counting, "plain": plain, "k": k,
                      **cfg, "rows": rows, "errors": len(errors), "first_error": errors[0] if errors else "",
                      "runs_s": runs, "best_s": best, "qps_best": round(len(queries) / best, 1) if best else 0}))


if __name__ == "__main__":
    main()
