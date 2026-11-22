# vfts design notes

This file explains the decisions and records the measurements behind them. The user's view is in
the README; the exact bytes are in docs/format.md.

## Pieces

    source table ──tbuild──> index tables ──tload──> node cache files ──mmap──> tsearch / tcount
    (customer: doc_id,       (vfts.term_dict,       (/tmp/vfts/<index>/)      tsnippet, hybrid
     body, lang, ver)         postings, positions,
    journal (doc_id, op, ver) doc_stats, ...)

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
   word takes a fast path (lower-casing is all NFKC_CF does to ASCII).
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

The source table has a version column `ver` (TIMESTAMPTZ, `DEFAULT CLOCK_TIMESTAMP()`); changes
that do not move `ver` (an UPDATE that keeps it, a DELETE) are recorded in a journal table
(doc_id, op, ver). This follows vvector and vgraph.

- **Boundary.** A refresh indexes the rows with `ver <= boundary`, where
  `boundary = LEAST(now, earliest lock request of an open writer on the source or the journal) - margin`
  (`v_monitor.locks`). A row committed after the build started reading was written after now or
  by a transaction open now, which holds an insert lock since its request; the margin (default
  60 s) covers clock differences between nodes and versions taken at statement start.
- **Segments.** Each refresh adds segments; nothing already indexed is rewritten.
- **Tombstones.** Every doc_id that has a newer row, or a journal row, in the refresh's range gets
  a tombstone in the older segments (`vfts.tombstones`, a bitmap per segment in the cache).
- **Live tail.** The view `<schema>.<index>_live` returns the source rows with `ver > boundary`,
  the doc_ids of journal rows with `ver > boundary` (with the current row's text when it still
  exists) and one sentinel row with the generation. tsearch masks every doc_id of the tail in the
  segments and analyzes the tail's rows into an in-memory segment, searched beside the others. So
  a search sees every committed change, without a refresh. The boundary is a literal in the view,
  so Vertica can prune storage containers.
- **Update = delete + insert**: a changed row masks (tail) or tombstones (refresh) its old version.
- A doc_id in both the tail and a segment is harmless: the tail wins. This makes every step order
  below safe.

`tests/sql/test_freshness.sh`: inserts, updates (with and without a version change), deletes,
searched without refresh, after refreshes, after merges and after a rebuild: always the ids,
ranks and scores of an index built from scratch on the current rows.

## Refresh

`vfts.refresh_index` (the name `refresh` is a Vertica built-in):

1. Mark the index (one refresh at a time; a mark whose session is gone is ignored).
2. Compute the boundary.
3. `tbuild` over the new and changed rows (partitioned by node, threads per node) into a session
   temporary table: segments as rows.
4. Insert the rows into the index tables; tombstones for the older versions.
5. `tload` on every node of the session's subcluster: the new segment files.
6. `tload` with the generation: the generation file (active segments, tombstones, their effect
   on df), then ACTIVE. Every node must report, or the refresh fails.
7. Statistics, the manifest (new generation and boundary) - and commit. Steps 3 to 7 are one
   transaction: a failure leaves the tables as they were.
8. Replace the views (DDL, after the commit). Until then the old views show a wider tail, which is
   still exact.
9. The tiered merge policy.

A failure after step 6 on some node leaves that node's cache ahead of the tables; its segments only
hold rows that are also in the (old, wider) tail, so results stay exact, and the next refresh or
`load_all` replaces the files.

## Merge

`vfts.merge(index [, max_segments])` merges segments from the node cache (`tmerge`, no text is
analyzed again): live documents, postings and positions merged term by term, tombstoned documents
dropped, stem groups carried over. Then the same steps 4 to 8 as a refresh.

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
  (`ALTER SESSION SET UDPARAMETER FOR vfts.vfts cache_dir = '...'`), then `/tmp/vfts`. The procedures
  pass an index's `cache_dir` option to the functions they call; a search cannot read the manifest,
  so searches of such an index set the same directory as a parameter or session parameter.
- `OVER(PARTITION NODES)` runs on every node only for a segmented input: `vfts.probe` (1,024
  rows, segmented) and `vfts.tnode(k)`, which returns one row per node. Loads join the replicated
  index tables with that one row per node: a local join (`DISTRIB(L,L)`), with `SYNTACTIC_JOIN`
  so that the row per node is the inner side (docs/VERTICA_NOTES.md: the planner reserves the
  declared width of LONG VARBINARY for every inner row).
- A process keeps its mappings of segment files and reuses them in later generations, so a
  refresh maps only its new segments. What ACTIVE says is trusted for 200 ms; a query whose view
  carries a newer generation reads it at once.
- A node whose cache is older than the view's generation refuses to answer ("stale: run
  CALL vfts.load_all"); a node without cache says so too.
- Eon: a statement reaches the nodes of the session's subcluster only. Every subcluster keeps its
  own caches; `load_all` from a session on a secondary subcluster loads it (tested on 3 + 2 nodes).

## Snippets and hybrid search

- `tsnippet(body, query)` analyzes the given text with the index's options, marks the words that
  match the query's words, stems, patterns and fuzzy words (not those under NOT), and returns the
  best windows (most different query words, then most hits) with marks at the original bytes.
- `hybrid(source, doc_id, rank, score)` fuses result lists by doc_id: Reciprocal Rank Fusion
  (`sum w / (60 + rank)`) or a weighted sum of min-max normalized scores (with distances inverted).
  The lists come from `tsearch` and vvector's `vsearch` in one SQL statement; Vertica procedures
  cannot return rows, so the fusion is a function over their union.
