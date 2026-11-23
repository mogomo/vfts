# Search next to other data: attributes (vfts 0.4.1)

Every document gets an attribute, its custodian (150 values, as in an e-discovery case); 10
custodians are on legal hold. vfts keeps the attribute in a table and copies it into the index with
`add_attribute` (`refresh_attributes` after a change); Elasticsearch and OpenSearch, which have no
joins, keep it in every document (`_update_by_query` on a clone of their index, then a force merge).

- J1 add the attribute to every document; J2 move 1% of the documents to another custodian.
- J3 the top 10 of a query among the custodians on hold (vfts: `tsearch` with
  `filter='custodian IN (...)'`; the engines: a `bool` query with a `terms` filter).
- J4 the matches of a query per custodian, every match counted (vfts: `tcount` with
  `by='custodian'`; the engines: `size: 0` with a `terms` aggregation).

Run by `bench/plan_c/stage1.sh j1` (bench/join_test.py for J1 and J2 and the answers; the native
HTTP load generator `tools/httpload` for J3 and J4): one client, each of the 40 queries 3 times, the
best kept, mean over the queries; 32 clients, the queries cycled to 20,000 requests, best of 3 runs.
vfts through `vfts_broker`: `--mode sql --max-batch 1` (one SQL statement per search, one session
per client) and `--mode serve` (`tserve`). Same machine as the rest of the benchmark; ES 9.5.6,
OpenSearch 3.10.0, Vertica 26.3, vfts 0.4.1 (attributes came with 0.4.0; 0.4.1 counts faster, see
"How"). MS MARCO queries as plain text (`syntax='plain'`, as the
engines' `match` query). One engine runs at a time.

**Caches.** Both engines keep answers by default: the shard request cache answers a repeated
`size: 0` request (J4) from memory, and the node query cache keeps the matches of a repeated
non-scoring clause (the J3 filter). A benchmark that repeats its 40 queries measures those caches,
so every test also runs with them off: on a clone with `index.queries.cache.enabled=false` and
`index.requests.cache.enable=false`, and `?request_cache=false`. vfts has the two same kinds of cache:
the filter cache (the documents a filter leaves out, per segment) and `tserve`'s answer cache (the
answers of counts, while the statement runs); "caches off" is `filter_cache=false` and
`request_cache=false`.

### Enron, 517,401 emails, 40 queries

| Test | vfts, one SQL statement | vfts, tserve | Elasticsearch | OpenSearch |
|---|---:|---:|---:|---:|
| J1 add the attribute to every document (s) | 0.505 | (same) | 231.491 | 240.056 |
| J2 move 1% of the documents to another custodian (s) | 0.329 | (same) | 2.727 | 2.720 |
| J3 top 10 among the custodians on hold, 1 client (ms) | 3.37 | 0.93 | 2.01 | 1.74 |
| J3 top 10 among the custodians on hold, 32 clients (q/s) | 1,262 | 14,178 | 8,345 | 8,559 |
| J3 top 10 among the custodians on hold, caches off, 1 client (ms) | | 0.98 | 1.34 | 1.45 |
| J3 top 10 among the custodians on hold, caches off, 32 clients (q/s) | | 13,694 | 7,806 | 7,804 |
| J4 matches per custodian, 1 client (ms) | 4.15 | 0.04 | 0.51 | 0.40 |
| J4 matches per custodian, 32 clients (q/s) | 1,201 | 88,248 | 39,910 | 39,355 |
| J4 matches per custodian, caches off, 1 client (ms) | | 1.06 | 2.29 | 2.22 |
| J4 matches per custodian, caches off, 32 clients (q/s) | | 6,097 | 4,500 | 4,745 |

vfts through vsql, one client (statement time as vsql reports it): J3 3.15 ms (3.17 ms without the filter cache), J4 3.81 ms; J1 = table 0.033 s + add_attribute 0.471 s; J2 = UPDATE 0.013 s + refresh_attributes 0.316 s.
Elasticsearch through its Python client, one client: J3 2.93 ms, J4 1.32 ms (caches off: 2.43 / 2.92 ms); force merge after the updates 53.3 s.
OpenSearch through its Python client, one client: J3 2.61 ms, J4 1.25 ms (caches off: 2.36 / 3.07 ms); force merge after the updates 34.8 s.

### MS MARCO, 8,841,823 passages, the first 40 questions of the dev set

| Test | vfts, one SQL statement | vfts, tserve | Elasticsearch | OpenSearch |
|---|---:|---:|---:|---:|
| J1 add the attribute to every document (s) | 3.205 | (same) | 888.303 | 955.758 |
| J2 move 1% of the documents to another custodian (s) | 2.812 | (same) | 12.115 | 11.891 |
| J3 top 10 among the custodians on hold, 1 client (ms) | 4.06 | 2.48 | 3.69 | 3.32 |
| J3 top 10 among the custodians on hold, 32 clients (q/s) | 939 | 3,978 | 3,881 | 4,336 |
| J3 top 10 among the custodians on hold, caches off, 1 client (ms) | | 3.77 | 3.95 | 3.63 |
| J3 top 10 among the custodians on hold, caches off, 32 clients (q/s) | | 2,828 | 2,625 | 2,858 |
| J4 matches per custodian, 1 client (ms) | 4.52 | 0.06 | 0.45 | 0.39 |
| J4 matches per custodian, 32 clients (q/s) | 865 | 61,000 | 40,651 | 42,030 |
| J4 matches per custodian, caches off, 1 client (ms) | | 0.81 | 76.30 | 93.52 |
| J4 matches per custodian, caches off, 32 clients (q/s) | | 2,280 | 136 | 118 |

vfts through vsql, one client (statement time as vsql reports it): J3 3.94 ms (4.26 ms without the filter cache), J4 4.16 ms; J1 = table 0.291 s + add_attribute 2.913 s; J2 = UPDATE 0.029 s + refresh_attributes 2.782 s.
Elasticsearch through its Python client, one client: J3 4.53 ms, J4 1.13 ms (caches off: 4.84 / 72.97 ms); force merge after the updates 409.9 s.
OpenSearch through its Python client, one client: J3 4.2 ms, J4 1.03 ms (caches off: 4.76 / 88.97 ms); force merge after the updates 160.0 s.

**The plain SQL join** (no attribute: `tsearch` with a large k or `tmatch`, joined with the
custodian table, `GROUP BY`), one client through vsql: Enron J3 16.81 ms, J4 18.74 ms (vfts 0.2.0,
bench/results/join.jsonl); MS MARCO J3 905.7 ms, J4 835.4 ms (vfts 0.4.0, 40 questions; the line
`j1_vfts_sqljoin_msmarco_c1` of joins.jsonl). Every match leaves the engine as a row: millions on
MS MARCO. Attributes keep the filter and the count inside the engine.

**Answers.** The attribute path gives exactly the answers of the SQL join on both collections (J3:
the same documents and scores at every rank for 40 of 40 queries; J4: the same count for every
custodian of every query). Against the engines: J4 identical for 40 of 40 Enron queries; on MS
MARCO identical for 29 of 40, the others within 0.097% of the matches (words the analyzers see
differently; Elasticsearch and OpenSearch agree with each other). J3: the same documents above the
10th score for 40 of 40 (Enron) and 39 of 40 (MS MARCO) queries; documents tied at the 10th score
are ordered by doc_id in vfts and by internal order in the engines, and fuzzy words score a little
differently (see "Same answers" in the README).

**How.** A filter is a set of left-out documents per segment, computed once per attribute version
and kept (or, with `filter_cache=false`, a check per candidate); phrases and `w/N` skip those
documents before reading positions. A count walks an OR window by window in a bit set (each word's
documents in a row, the left-out documents removed a word at a time) and adds one per match to its
value's counter. Since 0.4.1 a word's documents are set in the window block by block (128
docnos at a time), and a word in at least 1 of 32 documents is turned into a bit set of the
segment the first time a count uses it (kept while the segment is mapped): the MS MARCO count by
custodian with the caches off went from 5.44 ms (0.4.0) to 0.81 ms, 32 clients from 238 to 2,280
counts per second, with the same answers. `tserve` gives a count all free cores (a fair share
under load). See docs/design.md, "Attributes" and "Counting an OR".
