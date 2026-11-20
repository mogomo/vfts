# vtext plan

A milestone is done when all its tests pass, it is committed, and this file and
`docs/VERTICA_NOTES.md` are updated.

| # | Scope | Done when | State |
|---|---|---|---|
| M1 | Analyzer: tokenizer, normalizers, Snowball, Hebrew prefixes | unit tests pass for every language group, including the Sherlock Holmes cases | done 2026-11-21 |
| M2 | One-segment index build, storage format, `tcount`, boolean queries | results equal the SQL reference | done 2026-11-21 |
| M3 | Positions, phrase, proximity, `w/N`, `pre/N`; BM25 `tsearch` | BM25 scores equal the reference formula | done 2026-11-21 |
| M4 | Prefix, wildcard, fuzzy, stem expansion via the term dictionary | `match='exact'` and `match='stem'` both tested | done 2026-11-21 |
| M5 | Freshness: segments, live tail, journal deletes, global statistics, merge | correctness rules 3 and 4 pass | |
| M6 | `tsnippet`, `hybrid` with vvector | RRF output tested on a small fixture | |
| M7 | Multi-node Enterprise and Eon with a secondary subcluster; per-node cache | the same results on every system | |
| M8 | Benchmark, README, `blog/` article with charts | every number comes from a recorded run | |

## Test systems

- Enterprise: one node, Vertica 26.3.0-2 (SDK 26.2.0), Ubuntu 26.04, g++ 15.2, 22 cores, 61 GB.
- Eon: five nodes (primary subcluster of 3, secondary subcluster of 2), Vertica 26.2.0,
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
- [x] `vtext.tokenize()` UDx for inspection

Result: tests/engine/test_analyzer.cpp and tests/sql/test_tokenize.sh pass on Enterprise 26.3
(unfenced, g++ 15.2) and on the 5-node Eon cluster 26.2 (fenced, g++ 8.5). Arabic: Lucene's light
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
- [ ] same scores on 1 node and N nodes (Eon)

### M6 – M8
Filled in as each milestone starts.
