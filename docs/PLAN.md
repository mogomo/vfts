# vfts plan

A milestone is done when all its tests pass, it is committed, and this file and
`docs/VERTICA_NOTES.md` are updated.

| # | Scope | Done when | State |
|---|---|---|---|
| M1 | Analyzer: tokenizer, normalizers, Snowball, Hebrew prefixes | unit tests pass for every language group, including the Sherlock Holmes cases | done 2026-11-21 |
| M2 | One-segment index build, storage format, `tcount`, boolean queries | results equal the SQL reference | done 2026-11-21 |
| M3 | Positions, phrase, proximity, `w/N`, `pre/N`; BM25 `tsearch` | BM25 scores equal the reference formula | done 2026-11-21 |
| M4 | Prefix, wildcard, fuzzy, stem expansion via the term dictionary | `match='exact'` and `match='stem'` both tested | done 2026-11-21 |
| M5 | Freshness: segments, live tail, journal deletes, global statistics, merge | correctness rules 3 and 4 pass | done 2026-11-21 |
| M6 | `tsnippet`, `hybrid` with vvector | RRF output tested on a small fixture | done 2026-11-21 |
| M7 | Multi-node Enterprise and Eon with a secondary subcluster; per-node cache | the same results on every system | done 2026-11-21 (Enterprise: 1 node only) |
| M8 | Benchmark, README, `blog/` article with charts | every number comes from a recorded run | done 2026-11-21 |
| M9 | Many clients: tserve + vfts_broker, parallel batches, faster phrases and patterns, tmatch, join test | 32-client throughput and join results recorded with one native client for all engines | done 2026-11-21 |
| M10 | Access (views as the only input, row access policies, masks refused, internal functions not granted) and document order (graph bisection at merge) | every user's results equal an index of the rows they see; same hits in both orders; affected benchmarks rerun | done 2026-11-21 |

## Test systems

- Enterprise: one node, Vertica 26.3.0-2 (SDK 26.2.0), Ubuntu 26.04, g++ 15.2, 22 cores, 61 GB.
- Eon: five nodes (primary subcluster of 3, secondary subcluster of 2), Vertica 26.3.0-2 (SDK 26.2.0),
  RHEL 8.10, g++ 8.5, 2 cores and 15 GB per node.
- Multi-node Enterprise is not available; N-node results come from the Eon cluster.

## Dependencies (pinned, linked statically)

ICU 79.1, libstemmer_c 3.2.0 (Snowball), CRoaring 5.5.5. See `third_party/fetch.sh`.

## Work items

### M1 analyzer
- [x] third_party fetch and static build on both systems
- [x] ICU word-boundary tokenizer with positions and byte offsets
- [x] NFKC + case folding; Turkish i; accent folding option
- [x] Arabic / Persian normalizer; Hebrew niqqud removal
- [x] Snowball stemmers (list read with `sb_stemmer_list()`), Hebrew light prefix stripping
- [x] unsupported scripts: error, or plain with `on_unsupported='plain'`
- [x] unit tests per language group, Sherlock Holmes cases
- [x] `vfts.tokenize()` UDx for inspection

Result: tests/engine/test_analyzer.cpp and tests/sql/test_tokenize.sh pass on Enterprise 26.3
(unfenced, g++ 15.2) and on the 5-node Eon cluster 26.3 (fenced, g++ 8.5). Arabic: Lucene's light
prefixes are removed before Snowball (found in M4: after the normalization ة -> ه, Snowball alone
stems "المدرسه" and "مدرسه" differently).

### M2 one segment, tcount, boolean
- [x] segment format (docs/format.md), writer, reader, verify
- [x] segment builder (documents -> segment), table codec (segment <-> table rows)
- [x] query parser: terms, AND, OR, NOT, -term, ( )
- [x] boolean evaluation on one segment; tcount
- [x] index tables, create_index / refresh (first build) / drop_index / info procedures
- [x] tload: per-node cache from the tables
- [x] SQL reference test: boolean results equal plain SQL over a tokenized reference table

Result (M2 and M3): tests/engine/test_segment, test_query, test_search (1000 random queries against a
brute-force model, also split into segments with tombstones and a live tail), test_cache; SQL:
tests/sql/test_search.sh (boolean, phrase, slop phrase, w/N, pre/N, OR groups, prefix, wildcard, fuzzy
equal plain SQL; BM25 scores equal the formula within 1e-4), tests/sql/test_freshness.sh (rule 3).

### M4 prefix, wildcard, fuzzy, stem
- [x] engine: expansions chosen globally (same on every segmentation), max_expansions, prefix_length
- [x] SQL test: mode='exact' and mode='stem', languages through Vertica (tests/sql/test_languages.sh)

### M5 freshness, merge
- [x] segments, live tail, journal deletes, global statistics, incremental refresh (rule 3 passes)
- [x] tiered merge; same results before and after (rule 4)
- [x] same scores on 1 node and N nodes (Eon): tests/sql/digest.sql identical on 1 node, the 3-node primary and the 2-node secondary

### M7 Eon
- [x] build on RHEL 8 / g++ 8.5, deploy fenced and unfenced, every SQL test passes
- [x] node caches on every node of the session's subcluster; load_all from a secondary subcluster
- [x] stale caches refused with a clear message; identical results on every system

### M6 snippets and hybrid
- [x] tsnippet (engine test_snippet; SQL test_snippet_hybrid.sh)
- [x] hybrid (RRF and weighted) with vvector; test on a small fixture with hand-computed answers, and tsearch + vsearch fused in SQL

### M8 benchmark and documents
- [x] datasets downloaded and prepared (bench/data), Elasticsearch 9.5.6 and OpenSearch 3.10.0 installed natively
- [x] loaders and runners (vfts through vsql, engines through HTTP), relevance evaluation
- [x] speed work: MaxScore with impacts, windows, stem group postings, doc-range slices, thread pool
- [x] build path in Vertica: PARTITION BEST, declared widths, local cache write
- [x] MS MARCO measured on vfts, Elasticsearch, OpenSearch
- [x] Enron, PubMed, Wikipedia he/ar on all three (bench/run_all.sh, 2026-11-21)
- [x] fenced run, refresh of 1% new rows, batches (`queries` / `_msearch`), resource pool test
- [x] answers compared query by query; fixes found that way: w/N and pre/N with Lucene's span
      enumeration and widths, words cut at script changes, fuzzy expansion as a dictionary walk
- [x] report (bench/results/summary.md, charts), README performance, blog, demo run
- [x] Eon retest after the M8 changes: engine tests, 105 SQL checks, digest identical on Enterprise
      (1 node), the Eon primary subcluster (3 nodes) and the secondary subcluster (2 nodes)

Result: bench/results/summary.md. Same matches for 139 of 140 syntax queries (140 of 140 with 0.3.1: a Hebrew geresh used as a quotation mark); same top 10 and scores
for 97% of MS MARCO and 133 of 140 syntax queries; MRR@10 0.1815 / 0.1815 / 0.1813. Latency on MS
MARCO between OpenSearch and Elasticsearch (snap view), p99 half of theirs; slower on small
collections and on single-query throughput (Vertica's statement cost, about 2 ms); faster on
proximity, on Enron batches, on refresh; index half to two thirds of the engines' size.

### M9 many clients, joins
- [x] measurements of the statement cost (bench/plan_c: C0-C7, C12, C13, shapes s1): any UDx statement peaks at about
      2,000 per second on the test machine (SELECT 1: 6,000), so one statement per query cannot reach the engines
- [x] native load generator (tools/httpload) for all engines; the Python client had limited Elasticsearch and OpenSearch
- [x] tsearch batches in parallel (search_many: every (query, slice) piece on the pool, cost-based slices)
- [x] vfts.tserve (one statement per session, searches over a Unix socket, one-time token) and tools/vfts_broker
      (HTTP, each user's own sessions, modes serve and sql); tests/sql/test_serve.sh
- [x] engine: phrase and w/N bounds before positions are read, merged-impact window bounds for phrases, bit sets for
      large constant-score patterns, stem groups through their own list; every hit and score unchanged (A/B against
      the M8 engine on all benchmark queries, k = 10 and 100)
- [x] tmatch (every match, no scores) for joins; bench/join_test.py (attribute changes, filtered search, counts per group)
- [x] final 32-client and 1-client comparison (bench/run_all.sh throughput), README and blog rewritten
- [x] Eon (RHEL 8, g++ 8.5, fenced): engine tests and every SQL test, serve included

Result: bench/results/throughput.md, join.jsonl. With 32 clients through tserve, 1.1 to 2.0 times the
queries per second of the faster engine on all five collections; one statement per query a third to an
eighth of theirs. Attribute changes about 190 to 8,000 times faster than in the engines; filtering and counting
by an attribute 6 to 15 times slower.

### M10 access and document order
- [x] found: every function was granted to PUBLIC and tsearch trusted its input, so any user could search any index
      (input made with a generation number) and call the functions that write node caches
- [x] per-index key (vfts.index_keys, no grant), generation file version 2 with the access mode and keys, access
      token in the views' sentinel, checked by every search function (src/udx/view_input.h); REVOKE + GRANT of the
      search functions only; vfts.check_access (new key, policies read again); upgrade of existing indexes at install
- [x] row access policies: the views return the indexed documents the caller cannot see (packed, 2,500 per row) and a
      digest bound to the user and the generation; scores equal an index of the visible rows (tests/sql/test_access.sh)
- [x] column masks on indexed columns: searches refused
- [x] session parameters for any user (vfts_<name> in the public namespace)
- [x] recursive graph bisection at merge (src/engine/reorder.cpp), segment format 5 (doc_order section), format 4
      byte for byte when not reordered; index option reorder
- [x] engine A/B on all five collections (bench/plan_c/reorder_ab.sh): identical hits; faster on Enron and PubMed,
      slower on MS MARCO and Arabic Wikipedia phrases; the benchmark reorders Enron and PubMed only
- [x] reruns: vfts builds of Enron, PubMed, Hebrew Wikipedia; vfts rounds of the 32-client and 1-client comparison
      (stage1.sh f1v); access cost (bench/plan_c/access_cost.sh)


### M11 the name vfts
- [x] vtext renamed to vfts (repository vfts; schema, library, tools, session parameters, test schemas, file magic
      bytes VFTSSEG1 and VFTSGEN1, cache files .vfs and .vfg), version 0.3.0; nothing else changed
      (the benchmark files differ only in the name)
- [x] functionality tests with the new names on Ubuntu (unfenced) and Eon (fenced): engine tests, the SQL suites,
      install.sh, uninstall, examples.sh: all passed (169 SQL checks on each); the README examples equal a new
      run apart from the version; bench/report.py on the renamed files gives the same tables
