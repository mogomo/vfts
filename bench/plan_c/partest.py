#!/usr/bin/env python3
"""One statement with B queries: sequential (parameter queries) vs one input row per query with
OVER(PARTITION BY qid) (Vertica runs the partitions in parallel). Prints the statement times.

    python3 -I bench/plan_c/partest.py <index> <queries.tsv> <B> [threads]
"""
import subprocess
import sys
import time

index, qpath, B = sys.argv[1], sys.argv[2], int(sys.argv[3])
th = sys.argv[4] if len(sys.argv) > 4 else "1"
qs = [l.rstrip("\n").split("\t", 1)[1] for l in open(qpath, encoding="utf-8") if "\t" in l][:B]
lit = lambda s: "'" + s.replace("'", "''") + "'"
P = f"index={lit(index)}, mode='stem', k=10, max_expansions=100000"
seq = (f"SELECT COUNT(*) FROM (SELECT vfts.tsearch(* USING PARAMETERS queries={lit(chr(10).join(f'{i}{chr(9)}{q}' for i, q in enumerate(qs)))}, {P}) "
       f"OVER() FROM bench.{index}_snap) r")
rows = " UNION ALL ".join(f"SELECT {i} AS qid, {lit(q)} AS query" for i, q in enumerate(qs))
par = (f"SELECT COUNT(*) FROM (SELECT vfts.tsearch(q.qid, q.query, v.doc_id, v.body, v.lang, v.op, v.gen USING PARAMETERS {P}, threads={th}) "
       f"OVER(PARTITION BY q.qid) FROM bench.{index}_snap v CROSS JOIN ({rows}) q) r")
for name, sql in (("sequential", seq), ("partitioned", par)):
    script = "\\timing on\n" + (sql + ";\n") * 6
    out = subprocess.run(["vsql", "-X", "-A", "-t", "-f", "-"], input=script, capture_output=True, text=True)
    ms = [float(l.split("All rows formatted: ")[1].split()[0]) for l in out.stdout.splitlines() if "All rows formatted" in l]
    rows_out = [l for l in out.stdout.splitlines() if l.strip().isdigit()]
    print(name, "B", B, "rows", rows_out[:1], "ms", ms[1:], out.stderr[:300])
