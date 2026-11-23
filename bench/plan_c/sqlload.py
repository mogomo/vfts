#!/usr/bin/env python3
"""Statements per second of any SQL statement at C concurrent sessions (plan-C tests C1, C5, C6).

    python3 -I bench/plan_c/sqlload.py --sql TEMPLATE [options]

    --sql T          the statement; {label} is replaced by a unique label (put it in /*+LABEL()*/),
                     {q} by the next query of --queries (SQL-quoted, without the quotes)
    --queries FILE   qid<TAB>query lines, used in turn for {q}
    --n N            statements per measured run (default 10000)
    --clients C      concurrent sessions (default 32)
    --warmup W       unmeasured runs first (default 1)
    --repeat R       measured runs (default 3); the best is reported
    --session SQL    run first in every session, not measured
    --client K       vsql (default: one vsql session per client, statements on stdin),
                     vsql_connect (a new vsql process and connection per statement),
                     python (vertica_python, one process and connection per client),
                     python_prepared (the same, with server-side prepared statements)
    --same-text      every statement of a session has the same text (label and query): a prepared
                     statement is prepared once per session and then only executed

Prints one JSON line: statements per second over the elapsed time of the run (qps_best) and over the
server clock (first start to last end in v_monitor.query_requests), server ms per statement.
Connection: VSQL_HOST, VSQL_PORT, VSQL_USER, VSQL_PASSWORD, VSQL_DATABASE.
"""
import json
import multiprocessing
import os
import subprocess
import sys
import threading
import time
import uuid


def arg(name, default=None):
    # the last occurrence wins (a caller can override a default it put first)
    i = [j for j, a in enumerate(sys.argv[:-1]) if a == name]
    return sys.argv[i[-1] + 1] if i else default


def vsql_query(sql):
    return subprocess.run(["vsql", "-X", "-A", "-t", "-q", "-F", "\t", "-c", sql], capture_output=True, text=True, check=True).stdout


def py_client(args):
    stmts, session_sql, prepared = args
    import vertica_python
    conn = vertica_python.connect(host=os.environ.get("VSQL_HOST", "localhost"), port=int(os.environ.get("VSQL_PORT", 5433)),
                                  user=os.environ.get("VSQL_USER", "dbadmin"), password=os.environ.get("VSQL_PASSWORD", ""),
                                  database=os.environ.get("VSQL_DATABASE", ""), use_prepared_statements=prepared)
    cur = conn.cursor()
    if session_sql:
        cur.execute(session_sql)
    errors = 0
    for s in stmts:
        try:
            cur.execute(s)
            cur.fetchall()
        except Exception:
            errors += 1
    conn.close()
    return errors


def main():
    tmpl = arg("--sql")
    n = int(arg("--n", 10000))
    clients = int(arg("--clients", 32))
    warmup = int(arg("--warmup", 1))
    repeat = int(arg("--repeat", 3))
    session_sql = arg("--session", "")
    kind = arg("--client", "vsql")
    qs = []
    if arg("--queries"):
        with open(arg("--queries"), encoding="utf-8") as f:
            qs = [l.rstrip("\n").partition("\t")[2] for l in f if "\t" in l]
    run_id = "sql" + uuid.uuid4().hex[:10]

    same = "--same-text" in sys.argv

    def stmt(label, i):
        s = tmpl.replace("{label}", f"{label}_{i % clients if same else i}")
        if qs:
            s = s.replace("{q}", qs[(i % clients if same else i) % len(qs)].replace("'", "''"))
        return s.rstrip().rstrip(";")

    def run_once(label):
        parts = [list(range(c, n, clients)) for c in range(clients)]
        errs = [0]

        def vsql_session(c):
            script = (session_sql.rstrip(";") + ";\n" if session_sql else "") + "SELECT 1;\n" + "".join(stmt(label, i) + ";\n" for i in parts[c])
            p = subprocess.run(["vsql", "-X", "-A", "-t", "-q", "-f", "-"], input=script, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
            errs[0] += p.stderr.count("ERROR")

        def vsql_connect(c):
            for i in parts[c]:
                p = subprocess.run(["vsql", "-X", "-A", "-t", "-q", "-c", (session_sql.rstrip(";") + "; " if session_sql else "") + stmt(label, i)],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
                errs[0] += p.stderr.count("ERROR")

        t0 = time.time()
        if kind.startswith("python"):
            with multiprocessing.Pool(clients) as pool:
                errs[0] = sum(pool.map(py_client, [([stmt(label, i) for i in parts[c]], session_sql, kind == "python_prepared") for c in range(clients)]))
        else:
            f = vsql_session if kind == "vsql" else vsql_connect
            th = [threading.Thread(target=f, args=(c,)) for c in range(clients)]
            for t in th:
                t.start()
            for t in th:
                t.join()
        return time.time() - t0, errs[0]

    for w in range(warmup):
        run_once(f"{run_id}w{w}")
    best = None
    errors = 0
    for r in range(repeat):
        label = f"{run_id}r{r}"
        elapsed, e = run_once(label)
        errors += e
        out = vsql_query(f"SELECT COUNT(*), AVG(DATEDIFF('microsecond', start_timestamp, end_timestamp)), "
                         f"APPROXIMATE_PERCENTILE(DATEDIFF('microsecond', start_timestamp, end_timestamp) USING PARAMETERS percentile=0.5), "
                         f"APPROXIMATE_PERCENTILE(DATEDIFF('microsecond', start_timestamp, end_timestamp) USING PARAMETERS percentile=0.99), "
                         f"DATEDIFF('microsecond', MIN(start_timestamp), MAX(end_timestamp)) FROM v_monitor.query_requests "
                         f"WHERE request_label LIKE '{label}\\_%'").strip().split("\t")
        cnt = int(out[0])
        res = {"label": label, "elapsed_s": round(elapsed, 3), "qps_elapsed": round(n / elapsed, 1), "server_rows": cnt,
               "server_ms": {"mean": round(float(out[1] or 0) / 1000, 3), "p50": round(float(out[2] or 0) / 1000, 3),
                             "p99": round(float(out[3] or 0) / 1000, 3)},
               "qps_server": round(cnt / (int(out[4]) / 1e6), 1) if cnt and out[4] and int(out[4]) > 0 else 0}
        if best is None or res["qps_elapsed"] > best["qps_elapsed"]:
            best = res
    print(json.dumps({"engine": "sql", "client": kind, "sql": tmpl[:300], "n": n, "clients": clients, "session": session_sql,
                      "errors": errors, **best, "qps_best": best["qps_elapsed"]}))


if __name__ == "__main__":
    main()
