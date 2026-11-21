#!/usr/bin/env python3
"""Where the time of a statement goes (plan-C test C2), from Vertica's data collector.

    python3 -I bench/plan_c/phases.py <label-prefix> [name]

For the statements whose request_label starts with <label-prefix>_ (a run of sqlload.py), prints one
JSON line: request time (query_requests), resource queue wait (resource_acquisitions), and the mean
time of every execution step in dc_query_executions (parse, plan, compile, reserve resources, locks,
execute ...), in ms per statement. Steps with ':' are parts of the step before the ':'.
"""
import json
import subprocess
import sys


def q(sql):
    return subprocess.run(["vsql", "-X", "-A", "-t", "-q", "-F", "\t", "-c", sql], capture_output=True, text=True, check=True).stdout


lab = sys.argv[1]
name = sys.argv[2] if len(sys.argv) > 2 else lab
where = f"r.request_label LIKE '{lab}\\_%'"
req = q(f"SELECT COUNT(*), AVG(DATEDIFF('microsecond', start_timestamp, end_timestamp)) FROM v_monitor.query_requests r WHERE {where}").split("\t")
steps = q(f"""SELECT e.execution_step, COUNT(*), AVG(DATEDIFF('microsecond', e.time, e.completion_time))
  FROM dc_query_executions e JOIN v_monitor.query_requests r
    ON e.session_id = r.session_id AND e.transaction_id = r.transaction_id AND e.statement_id = r.statement_id
 WHERE {where} GROUP BY 1 ORDER BY MIN(e.time - r.start_timestamp)""")
wait = q(f"""SELECT COUNT(*), AVG(DATEDIFF('microsecond', a.queue_entry_timestamp, a.acquisition_timestamp))
  FROM v_monitor.resource_acquisitions a JOIN v_monitor.query_requests r
    ON a.transaction_id = r.transaction_id AND a.statement_id = r.statement_id
 WHERE {where}""").split("\t")
out = {"test": name, "label": lab, "statements": int(req[0]), "request_ms": round(float(req[1] or 0) / 1000, 3),
       "queue_wait_ms": round(float(wait[1] or 0) / 1000, 3), "acquisitions": int(wait[0] or 0), "steps_ms": {}}
for line in steps.splitlines():
    st, cnt, us = line.split("\t")
    out["steps_ms"][st] = round(float(us) / 1000, 3)
print(json.dumps(out))
