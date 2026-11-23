#!/usr/bin/env python3
"""Many users' batches: each user's own statement, or one shared statement by a service user.

    python3 -I bench/plan_c/bulk_shared.py <index> <queries.tsv> [--plain] [--count] [--users 10]
                                            [--split 4] [--repeat 3] [--schema bench]

The queries are dealt to N users (bulk_u1 .. bulk_uN, made for the test with a random password and
dropped at the end). Every result goes into one table, bench.bulk_master (requester, qid, ...),
with a row access policy: requester = CURRENT_USER(), GRANT TRUSTED (without it, Vertica allows no
INSERT into a table whose policy reads its rows; with it, INSERT follows the grants). Two ways, each timed from the start until
the last COMMIT, best of --repeat after one warm-up:

  per_user   every user at once, in their own session, as themselves: one statement with their
             queries, INSERT ... SELECT CURRENT_USER(), ... and COMMIT. Vertica checks each user's
             grants (and would apply a row access policy of the source table to each user).
  shared     one service user (bulk_svc: no superuser, only SELECT on the view and INSERT on the
             table) runs every user's queries in --split statements at once and writes each
             result with the user it belongs to. Only for an index whose source table has no
             row access policy: every user sees the same rows, so top k, counts and scores are
             the same for all of them.

Checks: both ways store the same rows; each user reads back exactly their own rows and none of the
others'. Prints one JSON line. Run as dbadmin (VSQL_* environment). Python standard library only.
"""
import json
import os
import secrets
import subprocess
import sys
import threading
import time


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def lit(s):
    return "'" + s.replace("'", "''") + "'"


def vsql(sql, user=None, pw=None, script=None):
    env = dict(os.environ)
    if user:
        env["VSQL_USER"], env["VSQL_PASSWORD"] = user, pw
    cmd = ["vsql", "-X", "-A", "-t", "-q", "-F", "\t", "-v", "ON_ERROR_STOP=1"] + (["-f", "-"] if script else ["-c", sql])
    p = subprocess.run(cmd, input=script, capture_output=True, text=True, env=env)
    if p.returncode != 0 or "ERROR" in p.stderr:
        raise RuntimeError(f"{user or 'dbadmin'}: {p.stderr[:500]}")
    return p.stdout


def main():
    index, qpath = sys.argv[1], sys.argv[2]
    plain = "--plain" in sys.argv
    counting = "--count" in sys.argv
    n_users = int(arg("--users", 10))
    split = int(arg("--split", 4))
    repeat = int(arg("--repeat", 3))
    schema = arg("--schema", "bench")
    view = f"{schema}.{index}_snap"
    queries = []
    with open(qpath, encoding="utf-8") as f:
        for line in f:
            qid, _, text = line.rstrip("\n").partition("\t")
            if text:
                queries.append((int(qid), text))
    users = [f"bulk_u{i + 1}" for i in range(n_users)]
    owner = {qid: users[i % n_users] for i, (qid, _) in enumerate(queries)}
    pw = {u: secrets.token_hex(12) for u in users + ["bulk_svc"]}
    fn = "tcount" if counting else "tsearch"
    params = [f"index={lit(index)}", "max_expansions=100000"] + ([] if counting else ["k=10"]) + (["syntax='plain'"] if plain else [])
    cols = "qid, count" if counting else "qid, doc_id, rank, score"
    typ = "count INT" if counting else "doc_id INT, rank INT, score FLOAT"
    master = f"{schema}.bulk_master"

    def search(qs):
        lines = "\n".join(f"{q}\t{t}" for q, t in qs)
        return f"(SELECT {cols} FROM (SELECT vfts.{fn}(* USING PARAMETERS queries={lit(lines)}, {', '.join(params)}) OVER() FROM {view}) s) r"

    def cleanup():
        vsql(f"DROP TABLE IF EXISTS {master} CASCADE; DROP TABLE IF EXISTS {schema}.bulk_owner CASCADE;")
        for u in users + ["bulk_svc"]:
            vsql(f"DROP USER IF EXISTS {u} CASCADE;")

    cleanup()
    try:
        admin = vsql("SELECT CURRENT_USER()").strip()      # the test's own user reads every row (the checks)
        setup = [f"CREATE TABLE {master} (requester VARCHAR(128), qid INT, {typ}) UNSEGMENTED ALL NODES;",
                 f"CREATE ACCESS POLICY ON {master} FOR ROWS WHERE requester = CURRENT_USER() OR CURRENT_USER() = {lit(admin)} "
                 f"GRANT TRUSTED ENABLE;",
                 # which user each query belongs to (the service user writes it next to each result)
                 f"CREATE TABLE {schema}.bulk_owner (qid INT, requester VARCHAR(128)) UNSEGMENTED ALL NODES;"]
        for u in users + ["bulk_svc"]:
            setup += [f"CREATE USER {u} IDENTIFIED BY {lit(pw[u])};", f"GRANT USAGE ON SCHEMA {schema} TO {u};",
                      f"GRANT SELECT ON {view} TO {u};", f"GRANT INSERT ON {master} TO {u};"]
        for u in users:
            setup.append(f"GRANT SELECT ON {master} TO {u};")
        setup.append(f"GRANT SELECT ON {schema}.bulk_owner TO bulk_svc;")
        vsql(None, script="\n".join(setup) + "\n")
        vsql(None, script=f"COPY {schema}.bulk_owner FROM STDIN DELIMITER E'\\t' DIRECT;\n"
             + "".join(f"{q}\t{u}\n" for q, u in owner.items()) + "\\.\nCOMMIT;\n")

        per_user_sql = {u: f"INSERT /*+DIRECT*/ INTO {master} SELECT CURRENT_USER(), r.* FROM {search([q for q in queries if owner[q[0]] == u])};\nCOMMIT;\n"
                        for u in users}
        parts = [queries[i::split] for i in range(split)]
        shared_sql = [f"INSERT /*+DIRECT*/ INTO {master} SELECT o.requester, r.* FROM {search(p)} JOIN {schema}.bulk_owner o ON o.qid = r.qid;\nCOMMIT;\n"
                      for p in parts]

        def timed(jobs):
            vsql(f"TRUNCATE TABLE {master};")
            errs = []

            def go(u, sc):
                try:
                    vsql(None, u, pw[u], sc)
                except RuntimeError as e:
                    errs.append(str(e))
            th = [threading.Thread(target=go, args=j) for j in jobs]
            t0 = time.perf_counter()
            for t in th:
                t.start()
            for t in th:
                t.join()
            if errs:
                raise RuntimeError(errs[0])
            return round(time.perf_counter() - t0, 3)

        def snapshot():
            return sorted(vsql(f"SELECT * FROM {master}").splitlines())

        res = {}
        for way, jobs in (("per_user", [(u, per_user_sql[u]) for u in users]), ("shared", [("bulk_svc", s) for s in shared_sql])):
            timed(jobs)
            runs = [timed(jobs) for _ in range(repeat)]
            res[way] = {"runs_s": runs, "best_s": min(runs), "statements": len(jobs), "rows": snapshot()}
        same = res["per_user"]["rows"] == res["shared"]["rows"]
        # every user reads back their own rows only
        own_ok = 0
        rows = res["shared"]["rows"]
        for u in users:
            mine = sorted(vsql(f"SELECT * FROM {master}", u, pw[u]).splitlines())
            want = [r for r in rows if r.split("\t", 1)[0] == u]
            own_ok += mine == want and all(r.split("\t", 1)[0] == u for r in mine)
        try:     # the service user writes results but may not read them back
            vsql(f"SELECT COUNT(*) FROM {master}", "bulk_svc", pw["bulk_svc"])
            svc_reads = True
        except RuntimeError:
            svc_reads = False
        out = {"index": index, "queries": len(queries), "count": counting, "plain": plain, "users": n_users, "split": split,
               "per_user": {k: v for k, v in res["per_user"].items() if k != "rows"},
               "shared": {k: v for k, v in res["shared"].items() if k != "rows"},
               "rows": len(rows), "same_rows": same, "users_see_only_their_rows": f"{own_ok} of {n_users}",
               "service_user_can_read_results": svc_reads}
        print(json.dumps(out))
    finally:
        cleanup()


if __name__ == "__main__":
    main()
