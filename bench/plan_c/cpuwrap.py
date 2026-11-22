#!/usr/bin/env python3
"""Run a benchmark command and record the CPU it used, next to its result.

    python3 -I bench/plan_c/cpuwrap.py <out.jsonl> <test-name> -- <command> [args ...]

The command prints one JSON line (bench/run_vertica.py, bench/run_engines.py, vfts_cli --clients,
bench/plan_c/sqlload.py). That line gets the fields "test" and "cpu" and is appended to out.jsonl
(and printed). CPU over the whole command (warm-up included), in cores (1.0 = one core busy):

    host      all processes (from /proc/stat: everything but idle and iowait)
    server    the database / engine process (vertica, or the Elasticsearch / OpenSearch JVM)
    clients   the command and its children (the vsql sessions or the Python client)

Linux only; Python standard library only.
"""
import json
import os
import subprocess
import sys
import time

TCK = os.sysconf("SC_CLK_TCK")


def host_ticks():
    with open("/proc/stat") as f:
        v = [int(x) for x in f.readline().split()[1:]]
    busy = sum(v) - v[3] - v[4]          # minus idle, iowait
    return busy


def server_pids():
    out = subprocess.run(["pgrep", "-f", r"/opt/vertica/bin/vertica -D|org\.elasticsearch|org\.opensearch"], capture_output=True, text=True).stdout
    return [int(p) for p in out.split()]


def proc_ticks(pid):
    try:
        with open(f"/proc/{pid}/stat") as f:
            s = f.read()
        v = s[s.rindex(")") + 2:].split()
        return int(v[11]) + int(v[12])       # utime + stime
    except (OSError, ValueError):
        return 0


def main():
    out_path, test = sys.argv[1], sys.argv[2]
    cmd = sys.argv[sys.argv.index("--") + 1:]
    pids = server_pids()
    h0, s0, t0, c0 = host_ticks(), sum(proc_ticks(p) for p in pids), time.time(), os.times()
    p = subprocess.run(cmd, stdout=subprocess.PIPE, text=True)
    h1, s1, t1, c1 = host_ticks(), sum(proc_ticks(p) for p in pids), time.time(), os.times()
    wall = t1 - t0
    lines = [l for l in p.stdout.splitlines() if l.startswith("{")]
    res = json.loads(lines[-1]) if lines else {"error": "no result", "returncode": p.returncode, "stdout": p.stdout[-500:]}
    res["test"] = test
    res["cpu"] = {"seconds": round(wall, 1), "host": round((h1 - h0) / TCK / wall, 2), "server": round((s1 - s0) / TCK / wall, 2),
                  "clients": round((c1.children_user + c1.children_system - c0.children_user - c0.children_system) / wall, 2),
                  "cores": os.cpu_count()}
    line = json.dumps(res)
    print(line, flush=True)
    with open(out_path, "a") as f:
        f.write(line + "\n")
    sys.exit(p.returncode)


if __name__ == "__main__":
    main()
