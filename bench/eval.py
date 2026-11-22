#!/usr/bin/env python3
"""Relevance of a run against judgements (MS MARCO passage dev): MRR@10 and nDCG@10.

    python3 -I bench/eval.py <qrels.tsv> <run.tsv> [<run2.tsv> ...]

qrels: "qid 0 doc_id grade" (MS MARCO: grade 1 for every judged passage).
run:   "qid <tab> doc_id <tab> rank <tab> score" (vfts_cli --run, bench/run_*.py).
Queries without any result count as 0. Prints one JSON line per run.
Also compares two runs: with two or more runs, the share of queries whose top 10 doc ids are the
same, in the same order, as in the first run.
"""
import json
import math
import sys


def read_qrels(path):
    q = {}
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) >= 4 and int(p[3]) > 0:
                q.setdefault(p[0], {})[p[2]] = int(p[3])
    return q


def read_run(path):
    r = {}
    with open(path) as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) < 3:
                continue
            r.setdefault(p[0], []).append((int(p[2]), p[1], float(p[3]) if len(p) > 3 else 0.0))
    for k in r:
        r[k].sort()
    return r


def evaluate(qrels, run):
    mrr = ndcg = 0.0
    for qid, rel in qrels.items():
        docs = [d for _, d, _ in run.get(qid, [])][:10]
        for i, d in enumerate(docs):
            if d in rel:
                mrr += 1.0 / (i + 1)
                break
        dcg = sum((2 ** rel.get(d, 0) - 1) / math.log2(i + 2) for i, d in enumerate(docs))
        ideal = sorted(rel.values(), reverse=True)[:10]
        idcg = sum((2 ** g - 1) / math.log2(i + 2) for i, g in enumerate(ideal))
        ndcg += dcg / idcg if idcg else 0.0
    n = len(qrels)
    return {"queries": n, "mrr@10": round(mrr / n, 4), "ndcg@10": round(ndcg / n, 4)}


def main():
    qrels = read_qrels(sys.argv[1])
    runs = [read_run(p) for p in sys.argv[2:]]
    for p, run in zip(sys.argv[2:], runs):
        out = {"run": p}
        out.update(evaluate(qrels, run))
        if run is not runs[0]:
            same = sum(1 for q in qrels if [d for _, d, _ in run.get(q, [])][:10] == [d for _, d, _ in runs[0].get(q, [])][:10])
            out["same_top10_as_first"] = round(same / len(qrels), 4)
        print(json.dumps(out))


if __name__ == "__main__":
    main()
