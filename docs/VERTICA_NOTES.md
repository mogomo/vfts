# Vertica behaviour verified for this project

Taken from github.com/mogomo/vertica-graph-udx (marked "graph project": tested on 26.2,
single node and 3-node Eon) and extended with what vtext verified itself (marked with the
milestone). Check again on other versions.
## UDx SDK
- The SDK headers compile with `-std=c++17` (g++ 8.5 and 11.5). Flags come from
  `/opt/vertica/sdk/examples/makefile`; `_GLIBCXX_USE_CXX11_ABI=1`.
- A transform function with no arguments and no FROM works: `SELECT f() OVER()`.
- Vertica does not call a transform function on empty input. Hence the sentinel
  row in the delta view and `FROM dual` for snapshot-only queries.
- Two factories may share one SQL name with different argument lists
  (`gbuild(INT, INT)` and `gbuild(INT, INT, FLOAT)`).
- Every input column costs row transfer time: dropping two columns halved the
  100M build (38 s to 19 s).
- A large generic lambda around the output loop stopped inlining and cost 30%
  when returning 5.8M rows. Keep hot output loops in small functions; batch.
- Session parameter for the library: `getUDSessionParamReader("library")`,
  set with `ALTER SESSION SET UDPARAMETER FOR vgraph cache_dir = '...'`. It is
  also visible inside stored procedures of the same session.
- `vt_report_error` inside `try` is fine; messages reach the client with file
  and line.
- Fenced against unfenced: about 1.2x for row-heavy work, no difference for
  gkhop_count.

## SQL
- `OVER(PARTITION NODES)` over an UNSEGMENTED table runs on one node only. Over
  a segmented table it runs on every node that holds rows.
- A scalar subquery next to an aggregate in the select list is rejected
  (ERROR 4817). Cross join one-row subqueries instead.
- A subquery in an ON clause is rejected (ERROR 4816). Put it in WHERE.
- `CREATE ROLE` has no IF NOT EXISTS. `CREATE SEQUENCE IF NOT EXISTS` exists;
  use `CACHE 1`, or ids jump by 250,000 per session.
- `epoch` cannot be selected into or sort a projection ("Column name epoch is
  reserved").
- `WithClauseRecursionLimit` defaults to 8; deeper levels of WITH RECURSIVE are
  dropped silently.
- vsql prints only the last result of several statements given with one `-c`.
- `/*+LABEL(x)*/` works in dynamic SQL inside procedures; find times in
  `v_monitor.query_requests` by label and `CURRENT_SESSION()`.
- The table-level hint goes after the table name: `FROM t /*+PROJS('s.p')*/`.
  The optimizer does not choose a projection sorted by a timestamp by itself.
- `CREATE LOCAL TEMPORARY TABLE ... ON COMMIT PRESERVE ROWS AS SELECT ... KSAFE 0`
  keeps a large result in the database without a buddy projection. A transform
  function call can feed it directly.
- For a 5.8M-row result `FROM dual` costs about 0.2 s more than a real one-row table.

## Time and order
- `CLOCK_TIMESTAMP()`: real clock, evaluated per row, accepted as a column
  DEFAULT (300,000 rows of one INSERT got 4,657 distinct values).
  `SYSDATE()`/`GETDATE()`: statement start. `NOW()`/`CURRENT_TIMESTAMP`:
  transaction start.
- `CLOCK_TIMESTAMP()::TIMESTAMP` depends on the session time zone. Use TIMESTAMPTZ.
- `PARTITION BY ts::DATE` is rejected for TIMESTAMPTZ (non-deterministic);
  `(ts AT TIME ZONE 'UTC')::DATE` is accepted.
- IDENTITY columns cannot be set by an INSERT, but sequence numbers do not
  follow write order across sessions: three rows written in order by two
  sessions got 250001, 500001, 250002.
- `v_monitor.locks` (columns node_names, object_name = 'Table:schema.table',
  transaction_id, lock_mode, request_timestamp, grant_timestamp) shows the
  insert locks (mode I) of other sessions' open transactions, for INSERT and
  COPY. Uncommitted rows are invisible to every query.

## Storage and the delta read (100M-row table, 1000 new rows)
- New rows in their own ROS container: 1 to 3 ms.
- After mergeout into the big container: 12 to 46 ms with default encoding,
  5 s with `ENCODING RLE` on the source column (COUNT(*) alone: 35 ms; fetching
  src and dst is the slow part). Not an I/O effect: it stays that way.
- Partitioned by version date: containers of different partitions are never
  merged, 1 ms, with any encoding.
- `DO_TM_TASK('mergeout', 'schema.table')` forces the case for tests.

## PL/vSQL
- `v := EXECUTE 'SELECT ...';` assigns from dynamic SQL. `x := (SELECT ...)`
  fails with "Query returned 0 rows" when nothing matches: use MAX().
- A local variable with the name of a column used in static SQL is an error
  ("Column reference ... is ambiguous").
- `refresh` and `load_snapshot` are built-in function names and cannot be
  procedure names.
- `PERFORM COMMIT;` works inside a procedure. NOTICEs of nested CALLs are not
  shown to the caller.
- A BOOLEAN concatenated into SQL text becomes `t`/`f`: write `true`/`false`.
- Scheduling: `CREATE SCHEDULE s USING CRON '...'` and `CREATE TRIGGER t ON
  SCHEDULE s EXECUTE PROCEDURE p('arg') AS DEFINER`; rows in
  `v_catalog.stored_proc_triggers`.

## Memory
- `FencedUDxMemoryLimitMB` is -1 by default (no limit). UDx heap is outside the
  resource pools; an mmap'ed file is page cache, shared by all queries and
  reclaimable.
- The general pool takes 95% of RAM. The 1B streaming build needed no change.

## Verified for vtext

- Vertica 26.3.0-2 reports `sdk_version()` = 26.2.0 (Ubuntu test host, 2026-11-20). The 26.3
  release changed nothing in the core database; libraries are built against the 26.2 SDK headers.
- (M1, 26.3.0-2 single node, unfenced; 26.2.0 5-node Eon, fenced) A library that links ICU 79.1
  statically (with `-Wl,--exclude-libs,ALL -Wl,-Bsymbolic`) loads into the Vertica server process
  and works next to Vertica's own ICU; the library is 43 MB, mostly ICU data.
- (M1) ICU must be built with its tools: `--disable-tools` leaves `libicudata.a` as a 1 KB stub, and
  the word break iterator then fails with U_MISSING_RESOURCE_ERROR.
- (M1) The SDK headers print "Vertica builds using GCC v14.0 or above were not tested" with g++ 15.2
  (Ubuntu 26.04); the library built with it passes every test.
- (M1) `vsql` is not on the PATH of a non-interactive ssh session to a node; the scripts call it
  through the PATH the caller sets (`/opt/vertica/bin`).
- (M1) A transform function declared with `isExploder = true` and two factories under one name
  (`tokenize(text)` and `tokenize(text, lang)`) work fenced and unfenced, on one node and on Eon.
- (M2, 26.3.0-2) A transform function accepts `*` and `t.*` as its argument list:
  `SELECT vtext.tokenize(*) OVER() FROM (SELECT body, lang FROM t) x` passes both columns. vtext's
  search functions are called this way over the generated views.
- (M2) `CREATE PROCEDURE vtext.refresh(...)` fails: "ROLLBACK 2278: Built-in function with the same
  name already exists: refresh" (also inside a schema). `vtext.merge`, `vtext.info` and
  `vtext.rebuild` are accepted as procedure names. vtext uses `vtext.refresh_index`.
- (M2) An exploder transform function called with `OVER(PARTITION BY ...)` gets "NOTICE 10927: OVER
  clause converted to OVER(PARTITION ROW)".
- (M2) PL/vSQL: `x := (SELECT NEXTVAL('s')) * 1000` fails with "ERROR 9016: Sequence manipulation functions not allowed in subqueries"; `x := (SELECT NEXTVAL('s'))` alone works, and `x := EXECUTE 'SELECT NEXTVAL(''s'') * 1000';` works.
- (M2) `match` is a reserved word in `USING PARAMETERS` ("ERROR 4856: Syntax error at or near \"match\""); a quoted `"match"='exact'` works. vtext calls the parameter `mode` and accepts `"match"` too.
