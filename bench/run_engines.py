#!/usr/bin/env python3
"""Run a query set against Elasticsearch or OpenSearch and time it.

    python3 -I bench/run_engines.py <es|os> <index> <queries.tsv> [options]

    --plain            queries are plain text: a match query (every word a should clause), the
                       equivalent of vtext syntax='plain'
    --mode stem|exact  field body (stemmed) or body.exact; default stem
    --k N              hits per query (default 10)
    --count            count matches (_count) instead of returning hits
    --clients C        concurrent clients (default 1)
    --warmup W         runs of the whole set before measuring (default 1)
    --repeat R         measured runs; per query the best time is kept (default 3)
    --run FILE         write the hits of the first measured run: qid, doc_id, rank, score
    --times FILE       write the time of every query: qid, server ms, client ms (best of the runs)
    --batch B          B queries per request (_msearch); times per query are the request's time
                       divided by B (the counterpart of run_vertica.py --batch)
    --cli PATH         vtext_cli (default build/tools/vtext_cli): parses the query syntax, so that
                       the engines get exactly the query vtext runs (see translate())

Prints one JSON line: server time ("took", ms) and client time (ms) percentiles, queries per
second, errors. Python standard library only.
"""
import http.client
import json
import os
import subprocess
import sys
import threading
import time

PORTS = {"es": int(os.environ.get("ES_PORT", 9200)), "os": int(os.environ.get("OS_PORT", 9201))}


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


class Conn:
    def __init__(self, port):
        self.port = port
        self.c = http.client.HTTPConnection("127.0.0.1", port, timeout=600)

    def post(self, path, body):
        if isinstance(body, str):       # ndjson (_msearch)
            data, ctype = body.encode(), "application/x-ndjson"
        else:
            data, ctype = json.dumps(body).encode(), "application/json"
        for attempt in range(2):
            try:
                self.c.request("POST", path, body=data, headers={"Content-Type": ctype})
                r = self.c.getresponse()
                return r.status, json.loads(r.read())
            except (http.client.HTTPException, ConnectionError, OSError):
                self.c.close()
                self.c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=600)
        raise RuntimeError("connection failed")


class Translator:
    """vtext's parsed query tree -> Elasticsearch / OpenSearch query DSL."""

    def __init__(self, conn, index, mode):
        self.conn, self.index = conn, index
        self.field = "body" if mode == "stem" else "body.exact"
        self.stems = {}

    def token(self, word, field):
        if field == "body.exact":
            return word
        if word not in self.stems:
            st, out = self.conn.post(f"/{self.index}/_analyze", {"field": "body", "text": word})
            toks = out.get("tokens", [])
            self.stems[word] = toks[0]["token"] if toks else word
        return self.stems[word]

    @staticmethod
    def has_pattern(t):
        if t["k"] in ("prefix", "wildcard", "fuzzy"):
            return True
        return any(Translator.has_pattern(x) for k in ("a", "b") if k in t for x in [t[k]]) or \
            any(Translator.has_pattern(x) for k in ("must", "should", "must_not") for x in t.get(k, []))

    def span_word(self, w, field):
        # a word inside a span query on body.exact (the query has a pattern) in stem mode: its stem
        # on body, masked as body.exact, so that all clauses share one field (positions are the same)
        if field == "body.exact" and self.field == "body":
            return {"field_masking_span": {"query": {"span_term": {"body": self.token(w, "body")}}, "field": "body.exact"}}
        return {"span_term": {field: self.token(w, field)}}

    def span(self, t, field):
        k = t["k"]
        if k == "term":
            return self.span_word(t["w"], field)
        if k == "phrase":
            return {"span_near": {"clauses": [self.span_word(w, field) for w in t["w"] if w],
                                  "slop": t["slop"], "in_order": True}}
        if k == "prefix":
            return {"span_multi": {"match": {"prefix": {field: {"value": t["p"]}}}}}
        if k == "wildcard":
            return {"span_multi": {"match": {"wildcard": {field: {"value": t["p"]}}}}}
        if k == "fuzzy":
            return {"span_multi": {"match": {"fuzzy": {field: {"value": t["w"], "fuzziness": t["edits"], "transpositions": True}}}}}
        if k == "bool":
            return {"span_or": {"clauses": [self.span(x, field) for x in t["should"]]}}
        if k == "near":
            return {"span_near": {"clauses": [self.span(t["a"], field), self.span(t["b"], field)],
                                  "slop": t["dist"] - 1, "in_order": t["ordered"]}}
        raise ValueError(f"not positional: {k}")

    def query(self, t):
        k = t["k"]
        if k == "none":
            return {"match_none": {}}
        if k == "term":
            return {"match": {self.field: {"query": t["w"]}}}
        if k == "phrase":
            return {"match_phrase": {self.field: {"query": " ".join(w for w in t["w"] if w), "slop": t["slop"]}}}
        if k == "prefix":
            return {"prefix": {"body.exact": {"value": t["p"], "rewrite": "constant_score"}}}
        if k == "wildcard":
            return {"wildcard": {"body.exact": {"value": t["p"], "rewrite": "constant_score"}}}
        if k == "fuzzy":
            return {"fuzzy": {"body.exact": {"value": t["w"], "fuzziness": t["edits"], "transpositions": True, "max_expansions": 1000}}}
        if k == "near":
            field = "body.exact" if self.has_pattern(t) else self.field   # one field per span query
            return self.span(t, field)
        if k == "bool":
            b = {}
            for occ in ("must", "should", "must_not"):
                if t[occ]:
                    b[occ] = [self.query(x) for x in t[occ]]
            return {"bool": b}
        raise ValueError(f"cannot translate {k}: {t}")


def pct(v, p):
    return round(v[min(len(v) - 1, int(p * len(v)))], 3) if v else 0


def main():
    engine, index, qpath = sys.argv[1], sys.argv[2], sys.argv[3]
    plain = "--plain" in sys.argv
    counting = "--count" in sys.argv
    mode = arg("--mode", "stem")
    k = int(arg("--k", 10))
    clients = int(arg("--clients", 1))
    warmup = int(arg("--warmup", 1))
    repeat = int(arg("--repeat", 3))
    run_path = arg("--run")
    times_path = arg("--times")
    batch = int(arg("--batch", 1))
    cli = arg("--cli", "build/tools/vtext_cli")
    port = PORTS[engine]
    queries = []
    with open(qpath, encoding="utf-8") as f:
        for line in f:
            qid, _, text = line.rstrip("\n").partition("\t")
            if text:
                queries.append((qid, text))
    conn = Conn(port)
    field = "body" if mode == "stem" else "body.exact"
    bodies = []
    if plain:
        for qid, text in queries:
            bodies.append({"match": {field: {"query": text}}})
    else:
        lang = arg("--lang", "en")
        out = subprocess.run([cli, "explain", qpath, "--lang", lang], capture_output=True, text=True, check=True).stdout
        tr = Translator(conn, index, mode)
        trees = [json.loads(l) for l in out.splitlines()]
        for t in trees:
            bodies.append({"match_none": {}} if t["tree"]["k"] == "error" else tr.query(t["tree"]))
    if counting:
        reqs = [(f"/{index}/_count", {"query": b}) for b in bodies]
    else:
        reqs = [(f"/{index}/_search", {"query": b, "size": k, "_source": False, "track_total_hits": False}) for b in bodies]

    n = len(reqs)
    if batch > 1:
        # one _msearch request per batch of consecutive queries; its time is shared by them
        groups = [list(range(i, min(i + batch, n))) for i in range(0, n, batch)]
        mreqs = [(f"/{index}/_msearch", "".join(json.dumps({}) + "\n" + json.dumps(reqs[i][1]) + "\n" for i in g)) for g in groups]
    took = [1e18] * n
    client = [1e18] * n
    errors = [0]
    hits = {}
    qps = []

    def run_once(record):
        nxt = [0]
        lock = threading.Lock()

        def work():
            c = Conn(port)
            c.post(f"/{index}/_count", {"query": {"match_none": {}}})   # not measured: the connection is set up
            while True:
                with lock:
                    i = nxt[0]
                    nxt[0] += 1
                if batch > 1:
                    if i >= len(mreqs):
                        return
                    t0 = time.perf_counter()
                    st, out = c.post(*mreqs[i])
                    dt = (time.perf_counter() - t0) * 1000
                    if st >= 300 or any("error" in r for r in out.get("responses", [])):
                        errors[0] += 1
                        continue
                    g = groups[i]
                    for j, r in zip(g, out["responses"]):
                        if record:
                            took[j] = min(took[j], out.get("took", dt) / len(g))
                            client[j] = min(client[j], dt / len(g))
                            if record == 2:
                                hits[j] = r
                    continue
                if i >= n:
                    return
                t0 = time.perf_counter()
                st, out = c.post(*reqs[i])
                dt = (time.perf_counter() - t0) * 1000
                if st >= 300:
                    errors[0] += 1
                    if errors[0] <= 3:
                        print(f"query {queries[i][0]}: HTTP {st}: {json.dumps(out)[:300]}", file=sys.stderr)
                    continue
                if record:
                    took[i] = min(took[i], out.get("took", dt) if not counting else dt)
                    client[i] = min(client[i], dt)
                    if record == 2:
                        hits[i] = out
        t0 = time.perf_counter()
        th = [threading.Thread(target=work) for _ in range(clients)]
        for t in th:
            t.start()
        for t in th:
            t.join()
        return n / (time.perf_counter() - t0)

    for _ in range(warmup):
        run_once(0)
    for r in range(repeat):
        qps.append(run_once(2 if r == 0 else 1))
    if run_path:
        with open(run_path, "w") as f:
            for i, (qid, _) in enumerate(queries):
                out = hits.get(i, {})
                if counting:
                    f.write(f"{qid}\t{out.get('count', -1)}\n")
                else:
                    for rank, h in enumerate(out.get("hits", {}).get("hits", []), 1):
                        f.write(f"{qid}\t{h['_id']}\t{rank}\t{h['_score']:.7f}\n")
    if times_path:
        with open(times_path, "w") as f:
            for i, (qid, _) in enumerate(queries):
                if took[i] < 1e17:
                    f.write(f"{qid}\t{took[i]:.3f}\t{client[i]:.3f}\n")
    t = sorted(x for x in took if x < 1e17)
    c = sorted(x for x in client if x < 1e17)
    print(json.dumps({"engine": engine, "index": index, "queries": n, "clients": clients, "batch": batch, "mode": mode, "plain": plain,
                      "count": counting, "k": k, "errors": errors[0],
                      "server_ms": {"mean": round(sum(t) / max(len(t), 1), 3), "p50": pct(t, .5), "p90": pct(t, .9), "p99": pct(t, .99), "max": pct(t, 1)},
                      "client_ms": {"mean": round(sum(c) / max(len(c), 1), 3), "p50": pct(c, .5), "p90": pct(c, .9), "p99": pct(c, .99), "max": pct(c, 1)},
                      "qps_best": round(max(qps), 1)}))


if __name__ == "__main__":
    main()
