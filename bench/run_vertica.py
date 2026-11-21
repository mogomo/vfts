#!/usr/bin/env python3
"""Run a query set against a vtext index in Vertica and time it.

    python3 -I bench/run_vertica.py <index> <schema> <queries.tsv> [options]

    --plain            syntax='plain' (every word a should clause, like a match query)
    --mode stem|exact  default stem
    --k N              hits per query (default 10)
    --count            tcount instead of tsearch
    --view live|snap   the input view: live (default: the always-exact path, with the live tail) or
                       snap (the segments only)
    --clients C        concurrent vsql sessions (default 1)
    --warmup W         runs of the whole set before measuring (default 1)
    --repeat R         measured runs; per query the best time is kept (default 3)
    --run FILE         write the hits of the first measured run: qid, doc_id, rank, score
    --times FILE       write the time of every query: qid, server ms, client ms (best of the runs)
    --param 'x=y'      more tsearch parameters (for example threads=1, max_expansions=100000)
    --batch B          B queries per statement (the parameter queries, one per line); times per
                       query are the statement's time divided by B

Every client is one vsql session that runs its share of the queries, each a labelled statement.
Server time per query: v_monitor.query_requests (end - start, microseconds). Client time: vsql
\\timing ("All rows formatted"). Queries per second: queries / (last end - first start) over the
server clock. Prints one JSON line. Connection: VSQL_HOST, VSQL_PORT, VSQL_USER, VSQL_PASSWORD,
VSQL_DATABASE. Python standard library only.
"""
import json
import re
import subprocess
import sys
import threading
import time
import uuid


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def lit(s):
    return "'" + s.replace("'", "''") + "'"


def vsql_query(sql):
    return subprocess.run(["vsql", "-X", "-A", "-t", "-q", "-F", "\t", "-c", sql], capture_output=True, text=True, check=True).stdout


def main():
    index, schema, qpath = sys.argv[1], sys.argv[2], sys.argv[3]
    plain = "--plain" in sys.argv
    counting = "--count" in sys.argv
    mode = arg("--mode", "stem")
    k = int(arg("--k", 10))
    view = arg("--view", "live")
    clients = int(arg("--clients", 1))
    warmup = int(arg("--warmup", 1))
    repeat = int(arg("--repeat", 3))
    run_path = arg("--run")
    times_path = arg("--times")
    extra = [sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == "--param"]
    batch = int(arg("--batch", 1))
    queries = []
    with open(qpath, encoding="utf-8") as f:
        for line in f:
            qid, _, text = line.rstrip("\n").partition("\t")
            if text:
                queries.append((qid, text))
    n = len(queries)
    fn = "tcount" if counting else "tsearch"
    params = [f"index={lit(index)}", f"mode={lit(mode)}"] + ([] if counting else [f"k={k}"]) + (["syntax='plain'"] if plain else []) + extra

    def statement(i, label):
        if batch > 1:
            # queries i .. i+batch-1 in the parameter queries (one "qid<TAB>query" per line)
            ids = list(range(i, min(i + batch, n)))
            lines = "\n".join(f"{j}\t{queries[j][1]}" for j in ids)
            cols = "qid, count" if counting else "qid, doc_id, rank, score"
            return (f"SELECT /*+LABEL({label}_{i})*/ {cols} FROM (SELECT vtext.{fn}(* USING PARAMETERS queries={lit(lines)}, "
                    f"{', '.join(params)}) OVER() FROM {schema}.{index}_{view}) r;\n")
        qid, text = queries[i]
        cols = f"{i}, count" if counting else f"{i}, doc_id, rank, score"
        return (f"SELECT /*+LABEL({label}_{i})*/ {cols} FROM (SELECT vtext.{fn}(* USING PARAMETERS query={lit(text)}, "
                f"{', '.join(params)}) OVER() FROM {schema}.{index}_{view}) r;\n")

    def run_once(label, record):
        starts = list(range(0, n, batch))           # one statement per batch of consecutive queries
        parts = [starts[c::clients] for c in range(clients)]
        outs = [None] * clients
        errs = [0]

        def client(c):
            # a marker before every statement: the timing line after it belongs to that statement,
            # also when the query has no hits
            # one statement first that is not measured: the session is set up (the engines' clients
            # likewise send one request on each new connection first)
            script = "\\timing on\nSELECT 1;\n" + "".join(f"\\echo @@{i}\n" + statement(i, label) for i in parts[c])
            p = subprocess.run(["vsql", "-X", "-A", "-t", "-F", "\t", "-f", "-"], input=script, capture_output=True, text=True)
            outs[c] = p.stdout
            if p.returncode != 0 or "ERROR" in p.stderr:
                errs[0] += p.stderr.count("ERROR")
                if errs[0] <= 3:
                    print(p.stderr[:500], file=sys.stderr)

        th = [threading.Thread(target=client, args=(c,)) for c in range(clients)]
        t0 = time.time()
        for t in th:
            t.start()
        for t in th:
            t.join()
        wall = time.time() - t0
        rows, client_ms = {}, {}
        for c in range(clients):
            stmt = None           # first query of the current statement (its marker)
            for line in outs[c].splitlines():
                if line.startswith("@@"):
                    stmt = int(line[2:])
                    continue
                m = re.match(r"Time: First fetch.*All rows formatted: ([0-9.]+) ms", line)
                if m:
                    if stmt is not None:
                        for j in range(stmt, min(stmt + batch, n)):
                            client_ms[j] = float(m.group(1)) / min(batch, n - stmt)
                    continue
                p = line.split("\t")
                if len(p) >= 2 and p[0].isdigit() and record:
                    rows.setdefault(int(p[0]), []).append(p[1:])
        return rows, client_ms, wall, errs[0]

    run_id = "vtb" + uuid.uuid4().hex[:10]
    for w in range(warmup):
        run_once(f"{run_id}w{w}", False)
    server = [1e18] * n
    client = [1e18] * n
    qps = []
    first_rows = None
    errors = 0
    for r in range(repeat):
        label = f"{run_id}r{r}"
        rows, cms, wall, e = run_once(label, r == 0)
        errors += e
        if r == 0:
            first_rows = rows
        for i, ms in cms.items():
            client[i] = min(client[i], ms)
        out = vsql_query(f"SELECT request_label, DATEDIFF('microsecond', start_timestamp, end_timestamp), "
                         f"start_timestamp, end_timestamp FROM v_monitor.query_requests "
                         f"WHERE request_label LIKE '{label}\\_%' AND success")
        starts, ends = [], []
        for line in out.splitlines():
            lab, us, st, en = line.split("\t")
            i = int(lab.rsplit("_", 1)[1])
            m = min(batch, n - i)
            for j in range(i, i + m):
                server[j] = min(server[j], int(us) / 1000.0 / m)
            starts.append(st)
            ends.append(en)
        if starts:
            span = vsql_query(f"SELECT DATEDIFF('microsecond', '{min(starts)}'::TIMESTAMPTZ, '{max(ends)}'::TIMESTAMPTZ)").strip()
            qps.append(n / (int(span) / 1e6) if int(span) > 0 else 0)
    if run_path:
        with open(run_path, "w") as f:
            for i, (qid, _) in enumerate(queries):
                for row in first_rows.get(i, []):
                    if counting:
                        f.write(f"{qid}\t{row[0]}\n")
                    else:
                        f.write(f"{qid}\t{row[0]}\t{row[1]}\t{float(row[2]):.7f}\n")
    if times_path:
        with open(times_path, "w") as f:
            for i, (qid, _) in enumerate(queries):
                if server[i] < 1e17:
                    f.write(f"{qid}\t{server[i]:.3f}\t{client[i]:.3f}\n")
    s = sorted(x for x in server if x < 1e17)
    c = sorted(x for x in client if x < 1e17)

    def pct(v, p):
        return round(v[min(len(v) - 1, int(p * len(v)))], 3) if v else 0

    print(json.dumps({"engine": "vtext", "index": index, "view": view, "batch": batch, "queries": n, "measured": len(s), "clients": clients,
                      "mode": mode, "plain": plain, "count": counting, "k": k, "params": extra, "errors": errors,
                      "server_ms": {"mean": round(sum(s) / max(len(s), 1), 3), "p50": pct(s, .5), "p90": pct(s, .9), "p99": pct(s, .99), "max": pct(s, 1)},
                      "client_ms": {"mean": round(sum(c) / max(len(c), 1), 3), "p50": pct(c, .5), "p90": pct(c, .9), "p99": pct(c, .99), "max": pct(c, 1)},
                      "qps_best": round(max(qps), 1) if qps else 0}))


if __name__ == "__main__":
    main()
