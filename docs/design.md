# vfts design notes

This file explains the decisions and records the measurements behind them. The user's view is in
the README; the exact bytes are in docs/format.md.

## Pieces

    source table ──tbuild──> index tables ──tload──> node cache files ──mmap──> tsearch / tcount
    (customer: doc_id,       (vfts.term_dict,       (<cache_dir>/<index>/)    tsnippet, hybrid
     body, lang)              postings, positions,
    journal (doc_id, op),     doc_stats, ...)
    optional

- `src/engine` is plain C++17 without Vertica includes (analysis, segments, search, merge, cache).
  `src/udx` holds thin adapters: they read rows and parameters, call the engine and write rows.
- Only the thread that Vertica calls uses the SDK. Builds, loads and searches use worker threads
  for engine work only (`tbuild`, `tload`, `tsearch`).
- The index tables are the data: replicated on every node, backed up with the database. The
  cache file of a node is a copy; a missing or damaged file is rebuilt from the tables.
- Static libraries: ICU 79.1, libstemmer_c 3.2.0 (Snowball), CRoaring 5.5.5, linked with
  `-Wl,--exclude-libs,ALL` so they never clash with the ICU inside the Vertica server process.

## Analysis

One pipeline for documents and queries (`src/engine/analyzer.cpp`):

1. ICU word boundaries (UAX #29) over the UTF-8 text, so offsets are byte offsets. Segments with
   no word status (spaces, punctuation) are dropped, except emoji, which are words as in Lucene's
   ICUTokenizer (found comparing token counts with Elasticsearch on MS MARCO: 7,600 of 506
   million tokens differed, all emoji). "Holmes'", "Holmes," and "Holmes-style" give
   "holmes"; צה"ל and צה״ל stay one word. A word whose letters belong to more than one script
   (`lewisi.הכנסת`) is cut into runs of one script first (a digit or punctuation mark stays in the
   run before it; Han, Kana and Hangul count as one), and every run is broken again: Lucene's
   ICUTokenizer cuts the text into script runs the same way.
2. NFKC with case folding (ICU `nfkc_cf`); in Turkish documents I -> ı and İ -> i first. An ASCII
   word takes a fast path (lower-casing is all NFKC_CF does to ASCII). An index made with `case=sensitive`
   uses NFKC without case folding (ASCII words are kept as they are; accents are still folded,
   step 3), and its stems keep the case: the stem of the lower-case word, with the word's own
   letters when the stem is a prefix of it (`Apples` -> `Appl`, `APPLES` -> `APPL`), else the
   lower-case stem. So `mode='stem'` stays case-sensitive (`Apple` finds `Apples`, not `apples`).
   The option is in the generation file's options (`;case=sensitive`), which every search, snippet
   and live tail reads; an index without it writes exactly the bytes it wrote before.
3. Per script: Latin accents removed (NFD, non-spacing marks dropped, NFC; option `accent_fold`);
   Arabic and Persian: Lucene's ArabicNormalizer and PersianNormalizer (tashkeel, tatweel, alef
   forms, alef maksura, teh marbuta, keheh, Farsi yeh); Hebrew: U+0591..U+05C7 removed, geresh
   and gershayim unified with ' and ". Curly apostrophes become '. These quote characters are
   unified before the word boundaries are found (UAX #29 counts the geresh, the gershayim and ʼ
   as letters: a geresh used as a quotation mark would otherwise stay on the word).
4. Words of more than 255 bytes are skipped. Chinese, Japanese, Korean, Thai, Lao, Khmer and
   Myanmar text is refused, or kept without stemming with `on_unsupported='plain'`.

Stems are not indexed. The stem is a column of the term dictionary: each term is stored once per
stemming language it was seen in, with its stem. The stemming language of a word is the document's
language when the word is written in that language's script, otherwise the default of its script
(an English word in a Hebrew document is stemmed as English). Snowball for every language it has
(read with `sb_stemmer_list()`), plus:

- Hebrew: no free stemmer exists. Up to two leading letters from ו ה ב כ ל מ ש are removed while
  at least three letters remain.
- Arabic: Lucene's light prefixes (ال وال بال كال فال لل و) are removed before Snowball. Found
  while testing: after the normalization ة -> ه, Snowball alone stems "المدرسه" and "مدرسه"
  differently.

`mode='stem'` expands a query word to every term of its stem group, `mode='exact'` to the term
only. A wrong stem changes which terms a query expands to; it never changes the index.

## Scoring

BM25 (k1 = 1.2, b = 0.75) exactly as Lucene's `BM25Similarity` and OpenSearch compute it, in the
same float operations:

    idf    = (float) ln(1 + (N - df + 0.5) / (df + 0.5))
    norm_i = 1 / (k1 * ((1 - b) + b * length(norm byte i) / avgdl))       per norm byte, float
    score  = idf - idf / (1 + tf * norm_i)

`bm25='classic'` multiplies the weight by (1 + k1) first (in float): the classic formula
`idf * tf * (k1 + 1) / (tf + K)`, which Elasticsearch uses (its `LegacyBM25Similarity`). Measured on
MS MARCO: Elasticsearch 9.5.6 scores are exactly 2.2 times those of OpenSearch 3.10.0 for the
same documents; the order is the same. vfts defaults to OpenSearch's scale (the formula the
design asks for) and offers Elasticsearch's.

with the one-byte document length of Lucene (exact below 24 tokens, 4 significant bits above).
A phrase scores with the sum of its words' idf and its frequency (exact: matches; sloppy: the sum of
`1 / (1 + match length)`, Lucene's SloppyPhraseMatcher). w/N and pre/N are Lucene's span queries
(`SpanNearQuery` with slop N - 1, in order for pre/N): the clauses give their spans per document
(a word its positions, an OR group the merged spans of its members as `SpanOrQuery`, a phrase an
ordered span query of its words, a w/N its matches); the matches are enumerated as
`NearSpansOrdered` (each later clause moves forward to the first span after the previous one) and
`NearSpansUnordered` (a window of one span per clause, the smallest moves on); the frequency is
the sum of `1 / (1 + width)` over the matches (ordered: the gaps between the clauses; unordered:
end - start of the match, as Lucene computes them), and the weight the sum of the idf of the distinct
terms below (every expansion of a pattern counts, as with Lucene's span_multi). A stem group is one "word": df = documents that contain any of its terms, tf =
their summed tf - what a stemmed Lucene field gives. Prefix and wildcard words score a constant 1
(Lucene's default constant-score rewrite); fuzzy words are scored like a stem group of the terms
they match. Clauses are summed in double and cast to float, in query order.

**Global statistics.** N (documents with at least one token, as Lucene's docCount), the total of
lengths and every df are summed over all segments and the live tail, minus tombstones and masked
documents. Tombstones' effect on df is computed once per generation (`compute_dead_counts`) and
stored in the generation file; the few documents masked by the live tail are checked per query.
So a score depends only on the document and on the live collection: the same on one node and on
five, in one segment or in twenty, before and after a merge (`tests/sql/digest.sql`,
`tests/engine/test_merge.cpp`, `tests/engine/test_search.cpp`).

Ties are broken by doc_id (ascending), on every node and every segmentation.

## Query evaluation

Document at a time over the segment's posting lists (`src/engine/search.cpp`):

- Leaves: one term, or the union of several (stem group, expansion). Phrases, w/N and pre/N are
  conjunctions verified on positions. Booleans follow Lucene's BooleanQuery: must, should (at
  least one if there is no must), must_not.
- Expansions of prefix, wildcard and fuzzy words are chosen over all segments together (the best
  `max_expansions`: fewest edits, then most documents, then term order) and then looked up in
  every segment, so the choice never depends on the segmentation.
- Fuzzy candidates come from a walk over the sorted term dictionary as a trie: the rows of the
  edit-distance table are kept per code point and reused for the prefix a term shares with the
  previous one; a prefix that can no longer come within the edits is skipped with all its terms,
  and a prefix at the limit jumps to its next child that matches the word (the letters a
  Levenshtein automaton would allow), with a galloping search.
- Segments, and slices of documents of large segments, are searched in parallel (`threads`,
  default one per core; while several queries run, each gets its share of the pool) with a top-k
  each, merged.

**Top k of OR queries: MaxScore.** The common ranked query is a disjunction of words. vfts uses
MaxScore with block-max bounds:

- every posting block of 128 documents stores its competitive (tf, norm) pairs ("impacts"), and
  every 32 blocks have a level-1 entry; a term has impacts over its whole list;
- the documents are processed in windows of 8,192 docnos. In each window every list gets a bound
  from its impacts (no decoding); sorted by bound, the lists whose bounds together stay below the
  current k-th score only probe candidates; candidates come from the other lists; a window whose
  lists together stay below the threshold is skipped;
- the k-th score is shared by the segments searched in parallel;
- pruning is strict (a document whose bound equals the threshold is still scored, it may win the
  tie by doc_id) and the final score is summed in clause order, so the result is exactly that of
  scoring every match (checked on 1,000 random queries in the unit tests and on 1,500 MS MARCO
  queries);
- a stem group has its own postings in the segment file (summed tf, no positions), so a stemmed
  word is one list, as in a stemmed Lucene field. The tables keep only the unstemmed terms.

Measured on MS MARCO (8.84 million passages, 1 segment, 1 thread, 1,500 dev queries, top 10;
the steps in the order they were made):

| Version | Mean latency |
|---|---:|
| every match scored | 216 ms |
| MaxScore, one (max tf, min length) pair per block | 15.1 ms |
| + block windows cached | 8.75 ms (20 segments) |
| one segment instead of 20 | 7.48 ms |
| + stem group postings | 4.36 ms |
| + level-1 impacts, windows of 8,192 docnos, partition per window | 4.00 ms |

The same queries with `mode='exact'` (no stem groups at all): 3.37 ms before the last step.

## Freshness: exact results between refreshes

Changes are found by the **row epoch**: every Vertica row has an implicit column `epoch`, the
epoch in which it was committed (NULL while it is not committed yet). Measured on Vertica 26.3
(tests/sql/test_changes.sh): an INSERT, an UPDATE (a new row version), a COPY,
`MOVE_PARTITIONS_TO_TABLE`, `SWAP_PARTITIONS_BETWEEN_TABLES` and `COPY_PARTITIONS_TO_TABLE` give
their rows the epoch of that commit; a mergeout keeps the epochs, also after the AHM moved; vbr
restores data with the current epoch (Vertica documentation). So the source table needs no
version column and no rules for writers. Before 0.5.0 vfts asked for a `ver` column
(`CLOCK_TIMESTAMP()` at write time) and a journal for deletes and for updates that kept `ver`; an
update that forgot both was never seen.

- **Boundary.** A refresh reads the current epoch C first. Every row committed before has an
  epoch below C, and every statement of the refresh sees it (a statement sees what was committed
  when it started); every row committed later gets C or more. So the segments take the rows with
  `epoch <= C - 1` (the boundary, `vfts.indexes.boundary_epoch`) and the live view the rows with
  `epoch > boundary`: no row is missed or taken twice, with no clock, margin or lock analysis.
- **Rows that leave.** A DELETE, `DROP_PARTITIONS`, `MOVE_PARTITIONS_TO_TABLE` (on the table the
  rows leave), `TRUNCATE` or the restore of an older copy leave no row with a new epoch. A refresh
  compares counts: with unique doc_ids, the source holds exactly the documents the index will hold
  after the refresh (every row of the source either is in the segments with its current epoch, or
  is newer and taken by this refresh), unless documents left. So `COUNT(*)` of the source (doc_id
  not NULL, `epoch <= boundary`; one scan of two integer columns: 10 ms for 8.8 million rows)
  equal to the live documents of the segments, minus those this refresh replaces, plus the rows
  it adds, means nothing left. Only when the counts differ does the refresh find the documents that
  left (an anti-join of the index's doc_ids with the source's: 0.4 s for 8.8 million) and tombstone
  them; it also adds any row of the source the index lacks. Measured on a copy of MS MARCO
  (8,841,823 rows, one segment, one node): a refresh with no change 0.31 to 0.33 s; after
  `DELETE` of 1% of the rows (88,419, no journal) 4.8 s, which found exactly those 88,419; after an
  `UPDATE` of another 1% (no version column) 5.6 s. A count of `water` took 12 ms before, 16 to
  30 ms after both refreshes, and 2.1 s while the 88,419 updated rows were in the live tail.
- **Segments.** Each refresh adds segments; nothing already indexed is rewritten.
- **Tombstones.** Every doc_id that has a newer row, a journal row, or left the source gets a
  tombstone in the older segments (`vfts.tombstones`, a bitmap per segment in the cache).
- **Live tail.** The view `<schema>.<index>_live` returns the source rows with `epoch > boundary`,
  the doc_ids of journal rows committed after the boundary (with the current row's text when it
  still exists) and one sentinel row with the generation. tsearch masks every doc_id of the tail
  in the segments and analyzes the tail's rows into an in-memory segment, searched beside the
  others. So a search sees every committed insert and update without a refresh. The boundary is a
  literal in the view, and the predicate is `epoch > boundary` alone, so Vertica skips every
  storage container older than the boundary (measured on 8.8 million rows: 1.4 ms when nothing is
  newer; adding `OR epoch IS NULL`, the session's own uncommitted rows, made it read every
  container: 41 ms, so those are not in the tail).
- **Deletes before the refresh.** A deleted row is not in the source any more, so the live view
  cannot list it: between the DELETE and the next refresh, searches still find the deleted
  document. A journal row (doc_id, op 'D') makes the delete visible at once; without one, the
  next refresh (or the automatic one, [Refresh](#refresh)) removes it.
- **Update = delete + insert**: a changed row masks (tail) or tombstones (refresh) its old version.
- A doc_id in both the tail and a segment is harmless: the tail wins. This makes every step order
  below safe.
- **Indexes of vfts 0.4 and older** keep their `ver` boundary (a timestamp literal) until their
  first refresh after the upgrade, which takes the rows newer than it and moves the index to an
  epoch boundary.

`tests/sql/test_freshness.sh` (with a journal) and `tests/sql/test_changes.sh` (without version
column or journal: every kind of write above, live and after a refresh): always the ids, ranks
and scores of an index built from scratch on the current rows.

## Refresh

`vfts.refresh_index` (the name `refresh` is a Vertica built-in):

1. Mark the index (one refresh at a time; a mark whose session is gone is ignored).
2. The new boundary: the current epoch minus one.
3. The rows to index: `epoch` above the old boundary and at most the new one, and the current row
   of every doc_id in the journal since the old boundary.
4. The count check above; when the counts differ, the documents that left and the rows that are
   missing.
5. `tbuild` over those rows (partitioned by node, threads per node) into a session temporary
   table: segments as rows.
6. Insert the rows into the index tables; tombstones for the older versions and the documents that
   left.
7. `tload` on every node of the session's subcluster: the new segment files.
8. `tload` with the generation: the generation file (active segments, tombstones, their effect
   on df), then ACTIVE. Every node must report, or the refresh fails.
9. Statistics, the manifest (new generation and boundary) - and commit. Steps 5 to 9 are one
   transaction: a failure leaves the tables as they were.
10. Replace the views (DDL, after the commit). Until then the old views show a wider tail, which
    is still exact.
11. The tiered merge policy.

A refresh that finds nothing (no new epoch, no journal row, equal counts) moves the boundary only.
A failure after step 8 on some node leaves that node's cache ahead of the tables; its segments only
hold rows that are also in the (old, wider) tail, so results stay exact, and the next refresh or
`load_all` replaces the files.

## Backup and restore

The index lives in the shared tables of schema `vfts` (one partition per index), so a full backup
holds every index, and an object backup of a user schema holds none. After an object restore of a
source table (or a schema), the rows of the restore get the current epoch (vbr restores with the
current epoch): the next refresh indexes them again, and the count check tombstones the documents
the restore took away. So one refresh makes the index equal to the restored table, at the cost of
analyzing the restored rows again.

`export_index` avoids that cost: it copies the index (options, the rows of the active segments,
tombstones, attribute copies; not the key) into `<schema>.<index>_vfts_export`, a table of the
source table's schema, so an object backup of that schema holds it. `import_index` writes the rows
back under a new index id with a new key, loads the nodes, makes the views, sets the boundary to
the current epoch and runs a refresh: the count check then finds the rows that came or went since
the export (`tests/sql/test_export.sh`: equal to a fresh index after deletes and inserts made
after the export). A row changed in place between the export and the backup keeps its doc_id and
cannot be found this way; the procedure says so, and `rebuild` is the remedy.

## Merge

`vfts.merge(index [, max_segments])` merges segments from the node cache (`tmerge`, no text is
analyzed again): live documents, postings and positions merged term by term, tombstoned documents
dropped, stem groups carried over. Then the same steps 4 to 8 as a refresh.

**In parallel (0.5.0).** The terms are cut into runs of about equal work (the documents to read);
each run is written by its own thread into a part of the segment writer (`SegmentWriter::part`),
and the parts are appended in term order (`append`): every term's postings start 8-aligned, so
shifting a part's offsets by its 8-aligned start gives the bytes one thread writes. The postings of
the stem groups (`finish`) are made the same way. The new segment is the same byte for byte
whatever the number of threads (`tests/engine/test_merge.cpp`; MS MARCO's merged segment of
2.79 GB compared with `cmp`). Threads: the index option `threads`, 0 = half the cores (searches
keep the other half); at most a wave of `threads` parts is in memory besides the writer. Measured
with `vfts_cli merge` on the 22 segments of MS MARCO (8,841,823 passages, no reordering, the
benchmark host): 68.7 s on one thread, 25.4 s on 11. Inside Vertica (`vfts.merge(index, 1)` of a
copy of MS MARCO in 16 segments, threads 11): 164 s in all, of which the `tmerge` statement took
88 s (the merge, then the new segment written out as index rows into the session's temporary
table) and storing those rows in the index tables 70 s (positions 36.5 s, postings 29.5 s). The
merge itself is no longer the larger part; writing 2.8 GB of index rows into Vertica tables is.

A merge whose inputs have no live document left (a `TRUNCATE`, a dropped partition: every
document tombstoned) retires the inputs and makes no segment: an empty segment has no rows in the
index tables, so the other nodes could not load it (found on the 3-node Eon subcluster).

**Document order.** A merge numbers the documents by recursive graph bisection
(`src/engine/reorder.cpp`; Dhulipala et al., KDD 2016; Lucene's `BPIndexReorderer`, PISA), unless
the index was made with `reorder=false`. Documents that share words get nearby docnos, so
postings have smaller gaps (fewer bits per document) and the documents that score high for a word
fall into fewer blocks, which MaxScore's block bounds then skip. The documents (the terms of each
document that occur in at least n / 2,048 documents, at most 4,096) are cut in two halves;
documents are swapped between the halves while that lowers the log-gap cost
`sum over terms of d * log2(n / (d + 1))` per half (d: documents of the half that contain the
term, n: the half's size), at most 20 rounds; then each half is cut again, down to 32 documents.
All halves of one level are independent and run in parallel. The order changes only the docnos:
hits, scores and counts are the same (ties are broken by doc_id, never by docno; the engine tests
compare every query on both orders, and the benchmark compares every hit). A reordered segment is
format 5: format 4 plus the docnos sorted by doc_id, for finding a doc_id (masks, tombstones);
a segment in doc_id order stays format 4, byte for byte as before, so existing indexes load
unchanged. Measured: [Performance](../README.md#performance) and the table below.

Tiered policy (`max_segments = 0`, also run at the end of every refresh): segments are grouped in
tiers by size (tier t holds 1 MB * merge_factor^t up to the next), and a tier with merge_factor
segments (default 10) has its merge_factor smallest merged into one; segments with 30% or more
deleted documents are merged too. `max_segments = N` merges the smallest segments until N are left.
Searches keep running on the previous generation while a merge runs.

## Access

Who may search an index, and what the access policies of the source table do to a search
(`src/udx/view_input.h`, `sql/procedures.sql`: `make_views`, `detect_access`, `check_access`).

- **Functions.** Users get the search functions (`tsearch`, `tcount`, `tmatch`, `tserve`,
  `tsnippet`, `hybrid`, `tokenize`, `tversion`, `tinfo`, `tnode`) and the procedure `info`. The
  functions that write node caches (`tbuild`, `tmerge`, `tload`, `thave`, `tdrop`) and the
  procedures that manage indexes are for the index owner (a superuser).
- **The views are the only way in.** A search function reads the index from the node cache;
  what it is allowed to see must come with its input. Every index has a secret key (128 bits from
  `/dev/urandom` through `UUID_GENERATE`) in `vfts.index_keys`, which no user can read, and on
  every node in the generation file (mode 0600, written from rows of that table, so the key never
  appears in the text of a statement). The sentinel row of the views carries a token made from it;
  the functions recompute the token and refuse input without the right one. A statement that makes
  its own input (`FROM dual` with a generation number) is refused, so a search needs SELECT on the
  views: Vertica's own grants decide who searches.
- **The views follow the source table's grants** (option `grants=source`, the default since
  0.5.0). A view reads its tables with its owner's privileges, so SELECT on a view alone would let
  a user search a table they may not read. Checking the caller's privilege inside the view is not
  possible: `HAS_TABLE_PRIVILEGE` is a meta-function, which Vertica refuses in a view, and a
  lookup in `v_catalog.grants` with `ENABLED_ROLE()` misses roles granted to roles and cost 8 ms
  per statement (measured). So vfts keeps the views' grants equal to the table's
  (`vfts.sync_grants`): every grantee of SELECT on the table (users, roles, `PUBLIC`, privileges
  inherited from the schema, the owner) gets SELECT on both views, every other grantee loses it,
  and Vertica decides as for the table itself (role chains, default roles, `SET ROLE`). It runs at
  `create_index`, `refresh_index`, `check_access` and `alter_index`; when someone lost access, the
  index gets a new key at once, so a token they read from a view before stops working. A REVOKE on
  the table therefore counts at the next refresh (or `check_access`), not at the next statement.
  `grants=manual` leaves the views' grants to the administrator (vfts 0.4's behaviour).
- **Access mode**, read from `v_catalog.access_policy` when the index is created, refreshed or
  merged, and by `vfts.check_access(index)` (only a superuser or the table's owner sees the
  policies, so only they run these):
  - `open` (no policy): the token is `md5(key:gen:key)`, a literal in the views; whoever may read
    the views may know it.
  - `rows` (an enabled row access policy, or `access=rows`): the views also return the documents
    of the generation's segments that are not visible in the source table, as `H` rows of 2,500
    comma-separated doc_ids each (one row per document would cost the declared width of the views'
    columns per row: 1.36 s for 258,701 rows against 0.04 s, docs/VERTICA_NOTES.md). Rows changed
    since the last refresh (a newer version, a journal row) are hidden too: the segments hold an
    older version, and the policy can only judge the current one (a row redacted and given to a
    user must not be found by the words of its old text); the live view searches the current
    version through its tail, the snap view leaves it out until the next refresh. A table without
    a version column cannot tell which rows changed: an updated row's old text stays searchable
    for the users who may see its new version until the refresh. The
    policy applies to the user who queries the view (tested: `CURRENT_USER()` inside a view is the
    caller, and the view reads the source with the policy of the caller), so each user gets the
    documents they cannot see. The functions mask them like deleted rows, so **scores are those of
    an index of the rows the user sees**: N, average length and every df leave the hidden
    documents out (in Lucene the term statistics come from the whole index, so a filter such as Elasticsearch's document level security does not change them; not measured here). The token is
    `md5(key:gen:user:n:s1:s2:key)`, computed by the view per statement with the key read from
    `vfts.index_keys` (the view owner's privilege): n hidden documents, s1 and s2 the sums of the first
    and second 32 bits of `md5(key:doc_id)` over them. Leaving out a hidden doc_id, adding one, or using
    the input of another user gives a wrong token; the user cannot compute one, the key is not in
    the view's text. The function opens exactly the generation the H rows were computed for
    (the previous generation's files stay on the node during a refresh).
  - `masked:<column>` (an enabled column access policy on the id, text, language or version
    column): every search is refused. A masked text would otherwise be searchable through the
    index (a hit reveals that the hidden text contains the word); searching the masked text
    instead would mean reading and analyzing the whole column per query.
- **Changes.** Policies created after the last check are not seen until the next refresh, merge
  or `check_access`; `access=rows` applies the row filter from the start. `check_access` also makes
  a new key: a token that someone copied before (for example a user whose SELECT was revoked) stops
  working. The nodes accept the old and the new key while the views are replaced, then the new one
  only.
- **Cost.** `open`: one MD5 per statement. `rows`: the view joins the index's doc_ids with the
  source table per statement and computes one MD5 per hidden document (in SQL and in the
  function); measured in [Performance](../README.md#performance).

## Node cache

- Layout and files: docs/format.md. cache_dir: function parameter, then session parameter
  (`ALTER SESSION SET UDPARAMETER FOR vfts.vfts cache_dir = '...'`), then `<catalog>/vfts`: the
  server process runs in the node's catalog directory and a fenced UDx process in its `UDxLogs`
  subdirectory (checked on Ubuntu and RHEL), so both find the same persistent, node-local
  directory without a parameter (src/udx/udx_common.h). Before 0.5.0 the default was `/tmp/vfts`,
  a tmpfs on many systems: after a reboot every search failed until `load_all` (`/var/tmp` was not
  taken: systemd removes its files after 30 days). The install moves the caches of `/tmp/vfts` to
  the new place (`vfts.upgrade_caches`, function `tcaches` with `action='adopt'`: a rename, or a
  copy in 8 MB pieces across file systems). The procedures pass an index's `cache_dir` option to
  the functions they call; a search cannot read the manifest, so searches of such an index set
  the same directory as a parameter or session parameter.
- Every cache directory holds `INDEX_ID`: the first load or build of an index claims the
  directory, and a directory of another index of the same name (dropped and created again while a
  node or a subcluster kept its cache) is emptied first. A cache written by vfts 0.4 has no
  `INDEX_ID` and is taken as it is (its generation file carries the index's keys, so a cache of
  another index would still be refused by the token check).
- `OVER(PARTITION NODES)` runs on every node only for a segmented input: `vfts.probe` (1,024
  rows, segmented) and `vfts.tnode(k)`, which returns one row per node. Loads join the replicated
  index tables with that one row per node: a local join (`DISTRIB(L,L)`), with `SYNTACTIC_JOIN`
  so that the row per node is the inner side (docs/VERTICA_NOTES.md: the planner reserves the
  declared width of LONG VARBINARY for every inner row).
- **Writing only what is missing** (`vfts.load_missing`): `thave` reports which segments of the
  generation each node has (header and checksum); only the segments some node lacks are read from
  the tables. Every refresh and merge does this for all segments of the new generation, not only
  its new ones, so a node that was down, lost its cache or is new is brought up to date by the
  next refresh; `load_all` does the same at once.
- A process keeps its mappings of segment files and reuses them in later generations, so a
  refresh maps only its new segments. What ACTIVE says is trusted for 200 ms; a query whose view
  carries a newer generation reads it at once.
- A node whose cache is older than the view's generation refuses to answer ("stale: run
  CALL vfts.load_all"); a node without cache says so too. A search cannot load: a UDx runs no SQL,
  and its input is the view, not the index tables.
- Eon: a statement reaches the nodes of the session's subcluster only. Every subcluster keeps its
  own caches; `load_all` or `maintain` from a session on a subcluster loads it (tested on 3 + 2
  nodes). A refresh on one subcluster makes the others stale until then.
- Leftovers: `tcaches` (`action='list'` or `'clean'`) lists the cache directories of a node, and
  removes those whose index no longer exists (by name and `INDEX_ID`). `vfts.clean_caches` runs it
  on the session's subcluster: from `load_all`, `maintain`, `cleanup`.

## Automatic refresh

On request only: a scheduled job in a database is something its administrators decide on, so an
index gets one only with the option `refresh_every` (minutes, default 0: none) at `create_index`
or `alter_index`, run by a superuser. The notices of `create_index` say which an index has, and
how to change it; `info` says it too. The option makes a Vertica schedule
of its own (`vfts.refresh_<index>_schedule`, `CREATE SCHEDULE ... USING CRON`; a schedule carries
one trigger, so indexes cannot share one: found when a second index got the same interval) and a
trigger `vfts.refresh_<index>` that runs `vfts.maintain('<index>')` as its definer
(stored procedure scheduling, Vertica 12 and later; the database parameter
`EnableStoredProcedureScheduler` must be 1, its default). Cron has no seconds, so the interval is
1 to 59 minutes or whole hours. `maintain`:

1. returns at once when a refresh of the index is running (the mark of `refresh_index`);
2. `refresh_index`; a failure is caught and written to `refresh_note` with its time (a trigger has
   no client to report to);
3. when a node of the subcluster does not have the current generation (`tinfo`), `load_all` (a
   refresh without changes does not touch the nodes);
4. `clean_caches`.

The trigger runs where Vertica's scheduler runs it; on Eon, subclusters that the scheduler does not
reach need `maintain` (or `load_all`) from a session on them. Without a trigger (`refresh_every=0`,
the default, or an index created by a user who is not a superuser) the refresh is the user's:
`refresh_index` by hand or from any scheduler, or `maintain` (which never fails its caller: a
failure goes to `refresh_note`). Changing the schedule needs a superuser (`alter_index` refuses
`refresh_every` for anyone else, so that the option and the catalog never disagree).
`import_index` makes again the schedule the exported index had.

## Attributes (0.4.0)

A search engine filters and counts by a field while it matches, because the field (doc values) is
next to the postings. Before 0.4.0, vfts handed every match to Vertica (`tmatch`, `tsearch` with a
large k) and Vertica joined and grouped them: on MS MARCO, where a question matches 4.4 million of
8.8 million documents on average, that took about a second for one client and 15 seconds each for
32 clients, although the engine found and counted the matches in 14 ms. Attributes bring the
column to the engine instead of the matches to the column.

- **The table stays the place of the value.** `vfts.add_attribute(index, attr, table, key, value)`
  copies `(key, value)` into `vfts.attr_values` (replicated, a partition per copy: `slot`) and from
  there into every node's cache (`attr_<attr>.vfa`, docs/format.md), with the same local join as
  segment loads. After the table changes, `vfts.refresh_attributes` makes a new copy (a new slot;
  the old partition is dropped). The copy is not live: between an `UPDATE` and the refresh,
  searches see the old values. A refresh copies the whole column (Enron, 517,401 values: 0.32 s;
  MS MARCO, 8.8 million: 2.6 s).
- **Types.** INT, BOOLEAN (0/1), DATE (days since 2000-01-01, Vertica's DateADT) and VARCHAR (at
  most 1,024 bytes). The file keeps a sorted dictionary of the distinct values and one code per
  document, in value order, so a range of values is a range of codes.
- **Documents without a value** (no row, or NULL) are missing: a comparison with them is unknown,
  as in SQL, and a filter keeps a document only when it is true; `by` does not count them (an inner
  join's `GROUP BY`). `IS NULL` finds them.
- **Codes per segment.** The first search of a segment with an attribute builds an array docno ->
  code (a direct table when the doc_ids are dense, else binary search), kept with the attribute file
  in the process. A live tail's segment is built per statement.
- **Filters** (parameter `filter`, SQL's WHERE syntax over attributes) become a bit set per segment
  of the documents to leave out. The search skips them while it walks postings, as it skips
  deleted documents, but the statistics (N, average length, document frequencies) stay those of
  the whole collection: a filter never changes a score (as in Elasticsearch, where a filter clause
  does not score either). The bit sets are cached (LRU, 512 MB) by filter text, segment and the
  identity of the attribute files; `filter_cache=false` asks the filter for each candidate
  document instead (one code lookup per candidate: the cost follows the matches, not the size of the
  collection).
- **Positions last.** Phrases and `w/N` / `pre/N` do not read the positions of a document that is no
  result anyway (left out by the filter, or deleted): the cheap checks first, as Lucene's two-phase
  iteration does. This is safe at any depth of the query, because such a document is dropped at the
  top and no other document's answer changes. On Enron's J3 (top 10 among 10 of 150 custodians) it
  halved the time of one client and doubled the searches per second of 32.
- **Counting** (`tcount` with `by`) adds one to the value's counter for every match: no rows leave
  the engine, one row per value comes out.
- **Counting an OR** (`tcount`, `tcount` with `by`, `tmatch`): no scores are needed, so the clauses
  are not walked together document by document. For each window of 32,768 docnos, each clause's
  documents are set in a bit set, one clause after the other; the deleted and filtered documents
  are taken out a 64-bit word at a time; then the bits are counted (or their values looked up).
  The work follows the postings read, not documents times clauses (Lucene's BooleanScorer does the
  same for counts and aggregations). On MS MARCO (questions of 5 to 10 words, up to 7.8 million
  matches each) it brought a count by custodian from 15.4 to 8.7 ms as one SQL statement.
  Since 0.4.1 a clause sets its window block by block (128 docnos decoded at a time, without their
  tfs, which are decoded later only if something asks for them), and a word in at least 1 of 32
  documents of a segment (and 4,096) is turned into a bit set of the whole segment the first time a
  count uses it; the segment keeps it while it is mapped (up to 1 GB of such sets per process), and
  a window then takes 512 word ORs, however many documents the word has there. Lucene stores the
  dense blocks of a list as bit sets in its index for the same reason. On MS MARCO the 6,980 dev
  questions counted in one statement went from 41 s to 0.5 s with the same counts.
- **Batches** (parameter `queries`, or query rows). `tsearch`: every query on one thread of the
  pool, many at once. `tcount`: every query cut into slices of its segments, and the slices of the
  whole batch in one run of the pool, the costliest queries first, so that one long query (a phrase
  of two common words) does not hold the batch up on one core. A call of the pool from inside the
  pool's work (a query of a batch searching its slices) runs in the calling thread and does not
  count as running, so a second batch that starts meanwhile still gets its share of the cores.
- **tserve** takes filter, k and by per request (a request that starts with the byte 0, see
  src/udx/tserve.cpp); `vfts_broker` passes them from its JSON (`filter`, `k`; `_count` with `by`).
  A search runs on one thread (many run at once); a count reads every match, so it takes its fair
  share of the engine's pool (all cores when it runs alone, about one each under load).
- **Answer cache of tserve.** The answers of counts are kept while the statement runs (LRU, 64 MB),
  as Elasticsearch's shard request cache keeps the answers of requests without hits until the next
  refresh. A `tserve` statement searches one snapshot, so an answer cannot go stale while it runs;
  the key holds the request and the attribute files it reads (name, version and the file's serial
  in the process), because `refresh_attribute` may replace an attribute meanwhile. Searches with
  hits are not kept (neither does Elasticsearch by default). `request_cache=false` turns it off.
- **Access.** An attribute copy would show values that a row access policy or a mask hides from a
  user, so tables with access policies are refused (checked at add and at every refresh; only a
  superuser or the table's owner sees policies, so only they can add or refresh). The index's own
  access mode still applies to every search.

## Snippets and hybrid search

- `tsnippet(body, query)` analyzes the given text with the index's options, marks the words that
  match the query's words, stems, patterns and fuzzy words (not those under NOT), and returns the
  best windows (most different query words, then most hits) with marks at the original bytes.
- `hybrid(source, doc_id, rank, score)` fuses result lists by doc_id: Reciprocal Rank Fusion
  (`sum w / (60 + rank)`) or a weighted sum of min-max normalized scores (with distances inverted).
  The lists come from `tsearch` and vvector's `vsearch` in one SQL statement; Vertica procedures
  cannot return rows, so the fusion is a function over their union.
