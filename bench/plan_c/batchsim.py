#!/usr/bin/env python3
"""Plan-C test C7: how a gateway should group queries into statements (a simulation).

    python3 -I bench/plan_c/batchsim.py <results.jsonl> [dataset view]

A model, not a measurement: the statement times come from test C3 (bench/run_vertica.py --batch B
at C sessions: server ms per query x B = the time of one statement of B queries while C statements
run at once), interpolated in log(B) and C. The gateway in front of Vertica holds S sessions.
32 clients send one query each, wait for the answer and send the next one at once (closed loop,
like the benchmark). Grouping rules:

    idle      a query goes out at once if a session is idle; otherwise it waits, and a session that
              finishes takes every waiting query (up to max_batch) as one statement
    wait=W    the first waiting query starts a timer of W ms; when it fires (and a session is idle)
              every waiting query goes out as one statement

Prints per rule and S: queries per second, latency mean / p99 (ms, from the client's send to the
answer), mean batch size per statement and as seen by a query. Statement times are those the
measured clients saw (server + network + vsql), so the gateway's own costs are not modelled.
"""
import bisect
import heapq
import json
import math
import random
import sys

CLIENTS = 32
MAX_BATCH = 500
SIM_QUERIES = 60000


def load(path, ds, view):
    grid = {}
    for line in open(path):
        r = json.loads(line)
        t = r.get("test", "")
        if not t.startswith(f"c3_{ds}_{view}_c"):
            continue
        c, b = r["clients"], r["batch"]
        # one statement of b queries as the client sees it (server, network, client): from the
        # measured closed loop, Little's law: c statements of b queries in flight -> c * b / qps
        grid[(c, b)] = c * b / r["qps_best"] * 1000.0
    return grid


class Model:
    def __init__(self, grid):
        self.cs = sorted({c for c, _ in grid})
        self.bs = sorted({b for _, b in grid})
        self.g = grid

    def _at_c(self, c, b):
        bs = [x for x in self.bs if (c, x) in self.g]
        lb = math.log(b)
        if b <= bs[0]:
            return self.g[(c, bs[0])] * b / bs[0] if b < bs[0] else self.g[(c, bs[0])]
        if b >= bs[-1]:
            return self.g[(c, bs[-1])] * b / bs[-1]
        i = bisect.bisect_left(bs, b)
        b0, b1 = bs[i - 1], bs[i]
        f = (lb - math.log(b0)) / (math.log(b1) - math.log(b0))
        return self.g[(c, b0)] * (1 - f) + self.g[(c, b1)] * f

    def stmt_ms(self, b, inflight):
        c = min(max(inflight, self.cs[0]), self.cs[-1])
        i = bisect.bisect_left(self.cs, c)
        if self.cs[i] == c:
            return self._at_c(c, b)
        c0, c1 = self.cs[i - 1], self.cs[i]
        f = (c - c0) / (c1 - c0)
        return self._at_c(c0, b) * (1 - f) + self._at_c(c1, b) * f


def simulate(model, sessions, wait_ms, seed=1):
    rnd = random.Random(seed)
    ev = []                     # (time, seq, kind, data)
    seq = 0

    def push(t, kind, data=None):
        nonlocal seq
        seq += 1
        heapq.heappush(ev, (t, seq, kind, data))

    for c in range(CLIENTS):
        push(rnd.random() * 0.1, "arrive", c)
    idle = sessions
    queue = []                  # (arrival time, client)
    timer_set = False
    inflight = 0
    lat, batches = [], []
    done = 0
    t_first = None
    t = 0.0

    def send(now):
        nonlocal idle, inflight, queue
        b = queue[:MAX_BATCH]
        queue = queue[MAX_BATCH:]
        idle -= 1
        inflight += 1
        d = model.stmt_ms(len(b), inflight) * (0.9 + 0.2 * rnd.random())
        push(now + d, "finish", b)
        batches.append(len(b))

    while ev and done < SIM_QUERIES:
        t, _, kind, data = heapq.heappop(ev)
        if kind == "arrive":
            queue.append((t, data))
            if wait_ms == 0:
                if idle:
                    send(t)
            elif not timer_set:
                timer_set = True
                push(t + wait_ms, "timer")
        elif kind == "timer":
            timer_set = False
            if queue and idle:
                send(t)
            elif queue:
                timer_set = True          # no idle session: keep collecting, look again soon
                push(t + 0.05, "timer")
        elif kind == "finish":
            idle += 1
            inflight -= 1
            for (ta, c) in data:
                if t_first is None and done >= SIM_QUERIES // 10:
                    t_first = t
                if t_first is not None:
                    lat.append(t - ta)
                done += 1
                push(t, "arrive", c)
            if wait_ms == 0 and queue and idle:
                send(t)
    n = len(lat)
    lat.sort()
    return {"qps": round(n / ((t - t_first) / 1000), 1), "lat_mean": round(sum(lat) / n, 2), "lat_p99": round(lat[int(.99 * n)], 2),
            "batch_mean": round(sum(batches) / len(batches), 1),                          # per statement
            "batch_of_query": round(sum(b * b for b in batches) / sum(batches), 1)}       # seen by a query


def main():
    path = sys.argv[1]
    sets = [tuple(sys.argv[2:4])] if len(sys.argv) > 3 else [(d, v) for d in ("enron", "msmarco", "pubmed") for v in ("snap", "live")]
    for ds, view in sets:
        grid = load(path, ds, view)
        if not grid:
            continue
        m = Model(grid)
        for s in (4, 8, 16, 32):
            for w in (0, 1, 2, 5):
                r = simulate(m, s, w)
                print(json.dumps({"test": f"c7_{ds}_{view}_s{s}_{'idle' if w == 0 else f'wait{w}'}", "dataset": ds, "view": view,
                                  "sessions": s, "rule": "idle" if w == 0 else f"wait={w}ms", **r, "simulation": True}))


if __name__ == "__main__":
    main()
