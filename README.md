# vertica-text-udx (vtext)

Full-text search inside Vertica, as a C++ UDx library: BM25 ranking, boolean, phrase and
proximity search (with the e-discovery operators `w/N` and `pre/N`), prefix, wildcard and fuzzy
words, every language written with spaces between words (Arabic and Hebrew included), new and
changed rows searchable at once without a rebuild, snippets, and hybrid search with
[vvector](https://github.com/mogomo/vertica-vector-db).

    SELECT r.rank, d.doc_id, vtext.tsnippet(d.body, 'contract w/5 indemn*' USING PARAMETERS index='emails') AS snippet
    FROM (SELECT vtext.tsearch(* USING PARAMETERS index='emails', query='contract w/5 indemn*', k=10) OVER()
          FROM app.emails_live) r
    JOIN app.emails d USING (doc_id)
    ORDER BY r.rank;

Use cases it was built and measured for: legal e-discovery (Enron email), pharma literature search
(PubMed), support logs and general passage search (MS MARCO), Arabic and Hebrew text (Wikipedia).

<!-- toc -->
- [What it does](#what-it-does)
- [How it was tested](#how-it-was-tested)
- [Quick start](#quick-start)
- [Languages](#languages)
- [No rebuild after new loads](#no-rebuild-after-new-loads)
- [Install](#install)
- [Prepare a table](#prepare-a-table)
- [Reference](#reference)
  - [Index management](#index-management): create_index, refresh_index, merge, rebuild, load_all, info, drop_index
  - [Search functions](#search-functions): tsearch, tcount, tsnippet, hybrid, tokenize, tversion
  - [Query syntax](#query-syntax)
- [Search patterns](#search-patterns)
- [Performance](#performance)
- [FAQ](#faq)
- [Restrictions and future work](#restrictions-and-future-work)
- [Files](#files)
- [License](#license)

## What it does

| | |
|---|---|
| Ranking | BM25 (k1 = 1.2, b = 0.75), the formula and float arithmetic of Lucene, Elasticsearch and OpenSearch: on MS MARCO the scores match theirs to 1e-4 with equivalent analyzers |
| Queries | words; `"exact phrases"`; `AND`, `OR`, `NOT`, `-word`, `+word`, `( )`; `"phrase"~N`; `a w/N b` (within N words, any order); `a pre/N b` (a before b, at most N words); `indemn*`; `te?t`; `holmse~`, `holmse~1` (Damerau-Levenshtein); or plain text (`syntax='plain'`) |
| Matching | `mode='stem'` (default: running, runs, run) or `mode='exact'`; one index serves both |
| Languages | every language written with spaces: ICU word boundaries, NFKC with case folding, accent folding, Snowball stemmers, Arabic/Persian normalization, Hebrew points and prefixes ([Languages](#languages)) |
| Freshness | inserts, updates and deletes are searchable at once (the live tail); `refresh_index` adds segments incrementally; a tiered merge keeps their number small ([No rebuild](#no-rebuild-after-new-loads)) |
| Scale | segments searched in parallel; global statistics, so scores do not depend on segments or nodes; Enterprise and Eon (also secondary subclusters) |
| Storage | the index is in Vertica tables (backed up with the database); every node keeps a memory-mapped copy |
| Extras | `tcount` (matches only), `tsnippet` (marked fragments), `hybrid` (BM25 + vectors with Reciprocal Rank Fusion), `tokenize` |

## How it was tested

- **Engine unit tests** (`make test`, no Vertica): the analyzer per language group (Latin,
  Greek/Cyrillic/Armenian, Arabic, Hebrew, Indic, others) including the Sherlock Holmes
  punctuation cases; the segment format against a brute-force model (140,000 documents, several
  65,536-document chunks), with a byte-identical round trip through the table rows; 1,000 random
  queries (boolean, phrase, w/N, pre/N, exact and stem) against a brute-force search, on one
  segment and on three segments with tombstones and a live tail; the pruned top 10 against the
  full result; merges (bit-identical scores before and after); the node cache; snippets.
- **SQL tests** (`tests/sql/*.sh`, on Vertica): results of every operator equal plain SQL over a
  table tokenized with the same analyzer; BM25 scores equal the formula computed in SQL (1e-4);
  freshness (inserts, updates with and without a new version, deletes: searched without refresh,
  after refreshes, merges and a rebuild, always equal to an index built from scratch); every
  language group and the hand-written Arabic and Hebrew cases; snippets; hybrid fusion with known
  answers and with vvector.
- **Systems**: Vertica 26.3.0-2 Enterprise (single node, Ubuntu 26.04, g++ 15.2, unfenced and
  fenced) and Vertica 26.2.0 Eon (5 nodes: a primary subcluster of 3 and a secondary of 2, RHEL
  8.10, g++ 8.5, fenced and unfenced). The same corpus and queries give identical ids, ranks and
  scores on 1 node, on 3 nodes and on the secondary subcluster (`tests/sql/digest.sql`). Multi-node
  Enterprise was not available for testing.
- **Benchmark** against Elasticsearch 9.5.6 and OpenSearch 3.10.0 on the same machine with the
  same data and equivalent analyzers ([Performance](#performance)). MS MARCO relevance: MRR@10
  0.1815 (vtext) and 0.1814 (Elasticsearch).

## Quick start

On a Vertica node, with the `vsql` connection variables set ([Install](#install)):

    make deps && make && make test && make deploy
    scripts/demo.sh          # every function on a small table, with explanations

By hand:

    CREATE TABLE app.docs (doc_id INT NOT NULL, body LONG VARCHAR, lang VARCHAR(8),
                           ver TIMESTAMPTZ NOT NULL DEFAULT CLOCK_TIMESTAMP());
    INSERT INTO app.docs (doc_id, body) VALUES (1, 'The indemnity clause of the supply contract.');
    INSERT INTO app.docs (doc_id, body) VALUES (2, 'Sherlock Holmes'' notes, Holmes-style.');
    COMMIT;
    CALL vtext.create_index('docs', 'app.docs', 'lang=en');
    SELECT * FROM (SELECT vtext.tsearch(* USING PARAMETERS index='docs', query='contract w/5 indemnity', k=10) OVER()
                   FROM app.docs_live) r;

## Languages

**Which languages work, and why.** Every language written with spaces between words works with
three generic steps, and no dictionary or thesaurus: ICU finds the words (Unicode word boundaries),
ICU normalizes them (NFKC, case folding, accents), and a rule-based stemmer reduces them to a stem.
BM25, phrases, proximity and fuzzy search need nothing more: only word positions, a few counts and
the list of distinct words.

| Group | Examples | Stemming |
|---|---|---|
| Latin script | English, Italian, French, Spanish, Portuguese, German, Dutch, Nordic languages, Polish, Czech, Romanian, Hungarian, Finnish, Estonian, Lithuanian, Turkish, Indonesian, Irish, Basque, Catalan, Esperanto, Sesotho | Snowball |
| Greek, Cyrillic, Armenian | Greek, Russian, Serbian, Ukrainian, Armenian | Snowball |
| Arabic script | Arabic, Persian | Snowball, after Arabic/Persian normalization (diacritics, tatweel, alef forms, ى, ة, ك/ک, ي/ی); Arabic also removes the prefixes ال وال بال كال فال لل و first |
| Hebrew script | Hebrew, Yiddish | Hebrew: points removed, light prefix stripping (up to two of ו ה ב כ ל מ ש while 3 letters remain); Yiddish: Snowball |
| Indic | Hindi, Nepali, Tamil | Snowball |
| Others with spaces | Georgian, Slovak, Croatian, Bulgarian, ... | none: exact words |

The Snowball stemmers linked in (libstemmer_c 3.2.0, `SELECT * FROM (SELECT vtext.tversion() OVER()) v;`):
arabic, armenian, basque, catalan, czech, danish, dutch, english, esperanto, estonian, finnish,
french, german, greek, hindi, hungarian, indonesian, irish, italian, lithuanian, nepali, norwegian,
persian, polish, portuguese, romanian, russian, serbian, sesotho, spanish, swedish, tamil, turkish,
ukrainian, yiddish.

A document's language is its `lang` column (ISO 639-1), or the index language. Words in another
script are stemmed with that script's default: an English word inside a Hebrew document is
stemmed as English. Hebrew has no free stemmer; recall in Hebrew is good but below a full
morphological analyser. Hybrid search with vvector covers most of that gap.

**Excluded, and why.**

| Language | Reason |
|---|---|
| Chinese, Japanese | no spaces: words are found with a dictionary |
| Thai, Lao, Khmer, Myanmar | no spaces: words are found with a dictionary |
| Korean | spaces exist, but useful search needs a morphological analyser |

Text in these scripts raises a clear error. With the index option `on_unsupported=plain` it is
indexed as ICU splits it, without stemming.

**Stopwords** are never removed from the index (that would break exact phrases). A query can drop
them with `stopwords=true`.

## No rebuild after new loads

**Why no rebuild is needed.** vtext works the way Lucene does:

1. New rows go into small new index segments (`refresh_index`). Nothing indexed is rewritten.
2. Rows that are not indexed yet are searched directly at query time: the view `<index>_live`
   returns them, and `tsearch` analyzes them on the fly (the **live tail**). Results are always exact.
3. Deleted and replaced rows are hidden with a tombstone bitmap. An update is a delete plus an insert.
4. A background merge combines small segments and drops deleted documents.

The one thing vtext needs from the source table is a version column (`ver`, the time a row was
written) and, for deletes and for updates that keep `ver`, a journal table that records them.
BM25 statistics (number of documents, average length, document frequencies) are computed over all
segments, the live tail and all nodes, minus tombstones: scores do not depend on how rows are split
into segments or nodes.

## Install

### Prerequisites

- Vertica 26.2 or later (26.3 has no core changes; tested on 26.2.0 and 26.3.0-2), the SDK in
  `/opt/vertica/sdk` (it must match the server: `SELECT sdk_version();`).
- g++ 8 or later on the same Linux as the cluster (tested with 8.5 on RHEL 8 and 15.2 on Ubuntu
  26.04), make, curl, python3 (for the benchmark only).
- vsql connection variables: `VSQL_HOST`, `VSQL_PORT`, `VSQL_USER`, `VSQL_PASSWORD`, `VSQL_DATABASE`.
  Nothing is stored by the scripts.

### Build, test, deploy

    make deps        # downloads ICU 79.1, libstemmer_c 3.2.0, CRoaring 5.5.5 (checksums pinned) and builds them statically
    make             # build/libvtext.so (all three libraries linked in; nothing to install on the nodes)
    make test        # engine unit tests, no Vertica needed
    make deploy      # library, functions, procedures and tables in schema vtext (unfenced)
    make deploy FENCED=yes      # the same, fenced (about 10% slower, a crash cannot stop the node)
    tests/sql/run_all.sh        # SQL tests against the database

`CREATE LIBRARY` reads the .so on the initiator node and copies it to the others. `make undeploy`
removes the library and functions; the index tables stay (`scripts/deploy.sh --undeploy --drop`
drops the schema).

## Prepare a table

**Source table contract.** One table, one text column:

    doc_id  INT           NOT NULL                        -- unique
    body    LONG VARCHAR                                   -- the text
    lang    VARCHAR(8)                                     -- optional; ISO 639-1; NULL = the index language
    ver     TIMESTAMPTZ   NOT NULL DEFAULT CLOCK_TIMESTAMP()   -- when the row was written

**Journal** (optional; needed for deletes and for updates that do not set `ver`):

    doc_id  INT
    op      CHAR(1)                                        -- 'I', 'U', 'D'
    ver     TIMESTAMPTZ   DEFAULT CLOCK_TIMESTAMP()

Rules:

- Use `TIMESTAMPTZ` and `CLOCK_TIMESTAMP()`: the clock at write time. (`NOW()` is the transaction
  start and is not suitable; plain `TIMESTAMP` depends on the session time zone.)
- An update either sets `ver = CLOCK_TIMESTAMP()` or writes a journal row `'U'`. A delete writes
  a journal row `'D'`.
- Columns with other names are mapped with the options `id_col`, `text_col`, `lang_col`, `ver_col`.
- For tables with a high write rate, partition the source table by `(ver AT TIME ZONE 'UTC')::DATE`
  so the live tail reads only the newest storage containers (docs/VERTICA_NOTES.md).

Ready to run:

    CREATE SCHEMA app;
    CREATE TABLE app.emails (doc_id INT NOT NULL, body LONG VARCHAR, lang VARCHAR(8),
                             ver TIMESTAMPTZ NOT NULL DEFAULT CLOCK_TIMESTAMP());
    CREATE TABLE app.emails_journal (doc_id INT, op CHAR(1), ver TIMESTAMPTZ DEFAULT CLOCK_TIMESTAMP());
    -- load: COPY app.emails (doc_id, body, lang) FROM ... ;
    CALL vtext.create_index('emails', 'app.emails', 'lang=en journal=app.emails_journal');

    -- later: changes
    UPDATE app.emails SET body = '...', ver = CLOCK_TIMESTAMP() WHERE doc_id = 42;
    DELETE FROM app.emails WHERE doc_id = 43; INSERT INTO app.emails_journal (doc_id, op) VALUES (43, 'D');
    COMMIT;
    -- searchable now through app.emails_live; folded into the index with:
    CALL vtext.refresh_index('emails');

## Reference

Every object lives in schema `vtext`. Procedures manage indexes (`CALL vtext.x(...)`); functions
search (`SELECT ...`). Examples use the demo table `demo.cases` of `scripts/demo.sh`; their output
is copied from a real run.

### Index management

#### create_index

Registers a source table as an index, creates the views `<schema>.<index>_live` and
`<index>_snap` in the table's schema, builds the index from every row and loads it on every node of
the session's subcluster.

    CALL vtext.create_index(index, source_table [, options]);

| Argument | Meaning |
|---|---|
| index | 1 to 64 letters, digits or underscores; unique |
| source_table | `schema.table` ([Prepare a table](#prepare-a-table)) |
| options | space or comma separated `key=value` pairs, all optional (below) |

| Option | Default | Meaning |
|---|---|---|
| lang | en | index language (ISO 639-1): documents without a `lang` value, and the stemmer of Latin words |
| journal | none | `schema.table` with columns doc_id, op, ver |
| margin | 60 | seconds the boundary stays behind a refresh (clock differences between nodes; 0 is fine on one node) |
| accent_fold | latin | `latin` (remove accents of Latin letters), `none`, `all` (also Greek and Cyrillic) |
| on_unsupported | error | `error` or `plain` for Chinese, Japanese, Korean, Thai, Lao, Khmer, Myanmar text |
| id_col, text_col, lang_col, ver_col | doc_id, body, lang, ver | column names in the source table |
| threads | 0 | build threads per instance (0: a quarter of the cores) |
| max_segment_mb | 2048 | a build thread starts a new segment above this memory |
| merge_factor | 10 | segments per tier of the merge policy |
| cache_dir | /tmp/vtext | directory of the node caches |
| build | true | `false`: register and create the views only |

    CALL vtext.create_index('cases', 'demo.cases', 'lang=en journal=demo.cases_journal margin=0');

#### refresh_index

Indexes the rows written since the last refresh into new segments, tombstones their older versions
and the journal's deletes, loads every node, and moves the views' boundary. Incremental; the table
changes are one transaction. Then runs the tiered merge policy. (The name `refresh` is a Vertica
built-in function and cannot be a procedure.)

    CALL vtext.refresh_index(index);

When to run it: whenever convenient - every minute, every hour. Searches are exact without it; a
longer live tail only costs query time (every tail row is analyzed per query).

#### merge

Merges segments into one, dropping deleted documents. Searches keep working on the previous
generation while it runs.

    CALL vtext.merge(index);                -- the tiered policy (also run by refresh_index)
    CALL vtext.merge(index, max_segments);  -- merge the smallest segments until at most max_segments remain

Fewer segments use less CPU per query; more segments are searched in parallel (lower latency for
one query at a time). The benchmark shows both ([Performance](#performance)).

#### rebuild

For recovery only: indexes every row again as new segments and drops the old ones.

    CALL vtext.rebuild(index);

#### load_all

Writes the node cache of every node of the session's subcluster from the index tables: for a new
node, a secondary subcluster (Eon), or a damaged cache directory. Segments a node has already are
not written again.

    CALL vtext.load_all(index);

On Eon, every subcluster keeps its own caches: run `load_all` from a session on each secondary
subcluster after a refresh on the primary. A node whose cache is behind refuses to answer:
`cache of index cases is stale on this node (generation 3, the view needs 4): run CALL vtext.load_all('cases')`.

#### info

Segments, documents, the live tail, the last refresh and the caches of the subcluster, as notices.

    CALL vtext.info(index);

Per node: `SELECT * FROM (SELECT vtext.tinfo(USING PARAMETERS index='cases') OVER(PARTITION NODES) FROM vtext.probe) i;`
Tables to read: `vtext.indexes` (the manifest), `vtext.segments`, `vtext.index_stats`.

#### drop_index

Removes the index: its rows (partitions), the views and the node caches of the subcluster.

    CALL vtext.drop_index(index);

#### The views _live and _snap

`create_index` makes two views in the schema of the source table; they are the input of `tsearch`
and `tcount` (call the functions with `*`):

| View | Rows | Use |
|---|---|---|
| `<index>_live` | the live tail (rows written after the boundary of the last refresh, journal changes) and one sentinel row | always exact: sees every committed change |
| `<index>_snap` | the sentinel row only | the segments only: ignores changes since the last refresh |

Both have the columns `(qid, query, doc_id, body, lang, op, gen)`. The sentinel carries the
generation, so a node with an old cache fails instead of answering from it. Grant SELECT on the
views (and the source table) to the users who search.

### Search functions

#### tsearch

The k best documents of a query, by BM25.

    SELECT vtext.tsearch(* USING PARAMETERS index='name', query='...' [, parameter=value ...]) OVER()
    FROM <schema>.<index>_live;

Output: `(qid, doc_id, score, rank)`; rank 1 is the best; ties by doc_id. Join to the source
table with `USING (doc_id)`.

| Parameter | Default | Meaning |
|---|---|---|
| index | (required) | the index |
| query | | the query ([Query syntax](#query-syntax)); or query rows (below) |
| k | 10 | hits per query, 1 to 1,000,000 |
| mode | stem | `stem`: words match their stem group (running, runs, run); `exact`: the word only. `"match"` (quoted: MATCH is reserved in Vertica) works too |
| syntax | query | `plain`: no operators, every word is an OR clause (like a match query of Elasticsearch) |
| operator | or | the implicit operator between words: `or` or `and` |
| lang | the index language | language of the query (stemming) |
| stopwords | false | drop the query language's stopwords |
| max_expansions | 50 | prefix, wildcard and fuzzy words expand to at most this many terms (the most frequent first; fuzzy: fewest edits first) |
| prefix_length | 0 | fuzzy: leading letters that must match exactly |
| threads | 0 | segments searched in parallel (0: one per core) |
| cache_dir | /tmp/vtext | node cache directory (or the session parameter) |

Many queries in one statement: add query rows `(qid, query)` to the view's rows:

    SELECT * FROM (SELECT vtext.tsearch(* USING PARAMETERS index='cases', k=3) OVER()
                   FROM (SELECT * FROM demo.cases_live
                         UNION ALL SELECT 1, 'indemnity', NULL, NULL, NULL, NULL, NULL
                         UNION ALL SELECT 2, 'holmes', NULL, NULL, NULL, NULL, NULL) i) r
    ORDER BY qid, rank;

Session defaults: `ALTER SESSION SET UDPARAMETER FOR vtext mode = 'exact';` (any parameter except
index and query).

#### tcount

The number of matching documents (no scoring).

    SELECT vtext.tcount(* USING PARAMETERS index='name', query='...' [, mode, syntax, operator, lang, ...]) OVER()
    FROM <schema>.<index>_live;

Output: `(qid, count)`.

#### tsnippet

The parts of a text that match a query, with the matching words marked. A scalar function: pass
the text (joined from the source table) and the query.

    vtext.tsnippet(body, query [, lang] USING PARAMETERS index='name' [, mode, words, fragments, pre_tag, post_tag, separator])

| Parameter | Default | Meaning |
|---|---|---|
| index | | use that index's analyzer settings (recommended); without it: `lang`, `accent_fold` |
| words | 20 | words per fragment |
| fragments | 2 | fragments at most (the best ones, in text order) |
| pre_tag, post_tag | `<b>`, `</b>` | marks around a hit |
| separator | ` ... ` | between fragments |

Hits are the words that match a word, stem, prefix, wildcard or fuzzy word of the query (not those
under NOT). Call it for the top hits only: it analyzes the text again.

#### hybrid

Fuses ranked lists by doc_id: BM25 from `tsearch` and vectors from vvector's `vsearch`.

    SELECT vtext.hybrid(source, doc_id, rank, score USING PARAMETERS [k, method, rrf_k, weights, distance]) OVER()
    FROM (<list 1> UNION ALL <list 2> ...) u;

| Parameter | Default | Meaning |
|---|---|---|
| method | rrf | `rrf`: score = sum of w / (rrf_k + rank); `weighted`: sum of w * min-max normalized score |
| k | 10 | results |
| rrf_k | 60 | the constant of Reciprocal Rank Fusion |
| weights | 1 each | `'text=0.7,vector=0.3'` |
| distance | | sources whose score is a distance (smaller is better), for `weighted` |

Output: `(doc_id, score, rank, sources)`; sources shows the rank in each list (`text:2 vector:1`).
Embeddings are computed outside Vertica; vtext includes no model.

#### tokenize

What the analyzer makes of a text: one row per word.

    SELECT vtext.tokenize(text [, lang] [USING PARAMETERS lang, accent_fold, on_unsupported]) OVER() ...;
    SELECT doc_id, vtext.tokenize(body, lang) FROM t;      -- beside other columns

Output: `(pos, term, stem, lang, start_byte, end_byte)`.

#### tversion

    SELECT vtext.tversion() OVER();

Output: library version, build flags, ICU version, the Snowball stemmers linked in.

### Query syntax

| Syntax | Meaning | Example |
|---|---|---|
| `word` | one word, analyzed like the documents | `holmes` finds Holmes, Holmes', Holmes-style |
| `"words"` | phrase: adjacent words in order | `"indemnity clause"` |
| `"words"~N` | sloppy phrase (Lucene): within N moves, any order | `"supply contract indemnity"~3` |
| `a AND b`, `a && b` | both | `contract AND indemnity` |
| `a OR b`, `a \|\| b` | either | `fraud OR scandal` |
| `a NOT b`, `a AND NOT b`, `-b` | without | `contract NOT indemnify`, `contract -indemnify` |
| `+a` | required (with other words optional) | `+contract indemnity` |
| `( )` | grouping | `(fraud OR scandal) AND audit*` |
| `a b` | implicit operator: OR (`operator='and'` for AND) | `contract indemnity` |
| `a w/N b` | within N words, any order (adjacent = 1) | `contract w/5 indemnity` |
| `a pre/N b` | a before b by at most N words | `seller pre/3 indemnify` |
| `word*` | prefix | `indemn*` |
| `te?t`, `*tract` | wildcard: `?` one letter, `*` any | `contr?ct` |
| `word~`, `word~1` | fuzzy: at most 2 (or 1) edits, Damerau-Levenshtein | `holmse~1` |
| `\x` | the next character literally | `\AND` (the word "and") |

- Precedence, lowest first: implicit operator, OR, AND / NOT, unary (+ - NOT), w/N and pre/N.
- Operands of w/N and pre/N: words, phrases, prefix/wildcard/fuzzy words, OR groups of them, and
  other w/N or pre/N: `(contract OR agreement) w/5 indemn*`, `a w/3 b w/2 c`.
- A word that the analyzer splits (`Holmes-style`, `e-mail`) becomes a phrase.
- `AND`, `OR`, `NOT` are operators in upper case only. A Hebrew acronym in a query: write it with
  gershayim `צה״ל` or escape the quote `צה\"ל` (a plain `"` starts a phrase).
- Scores: words and stem groups BM25; phrases, w/N and pre/N BM25 with the sum of their words' idf
  and their frequency (1 / (1 + distance) per match); prefix and wildcard words a constant 1 (as
  Elasticsearch); fuzzy words like a stem group of the terms they match.

## Search patterns

**Join the hits to the table** (the function returns ids and scores; the text stays in your table):

    SELECT r.rank, d.doc_id, LEFT(d.body, 80) AS body, r.score
    FROM (SELECT vtext.tsearch(* USING PARAMETERS index='emails', query='contract w/5 indemnity', k=20) OVER()
          FROM app.emails_live) r
    JOIN app.emails d USING (doc_id)
    ORDER BY r.rank;

**Filter by other columns.** Search with a larger k, then filter in SQL; or count first:

    SELECT r.doc_id, r.score FROM (SELECT vtext.tsearch(* USING PARAMETERS index='emails', query='fraud', k=1000) OVER()
                                   FROM app.emails_live) r
    JOIN app.emails d USING (doc_id) WHERE d.sent > '2001-06-01' ORDER BY r.score DESC LIMIT 10;

**E-discovery review set**: every matching document, no ranking needed (`tcount` for the size,
`tsearch` with a large k for the ids):

    SELECT * FROM (SELECT vtext.tcount(* USING PARAMETERS index='emails',
                   query='(shred* OR destroy*) w/5 document* AND NOT newsletter') OVER() FROM app.emails_live) c;

**A search box** (users type words, not syntax): `syntax='plain'`, optionally `operator='and'`:

    SELECT * FROM (SELECT vtext.tsearch(* USING PARAMETERS index='kb', query='how do I reset my password?',
                   syntax='plain', k=10) OVER() FROM app.kb_live) r;

**Several languages in one table**: give each row its `lang`; give the query its `lang`:

    SELECT * FROM (SELECT vtext.tsearch(* USING PARAMETERS index='news', query='الكتاب', lang='ar', k=10) OVER()
                   FROM app.news_live) r;

**Hybrid: BM25 + vectors** (vvector index `kb_vec` on the same ids):

    SELECT h.rank, h.doc_id, h.sources
    FROM (SELECT vtext.hybrid(src, doc_id, rank, score USING PARAMETERS k=10) OVER() FROM (
            SELECT 'text' AS src, doc_id, rank, score FROM (SELECT vtext.tsearch(* USING PARAMETERS index='kb',
                   query='reset password', syntax='plain', k=50) OVER() FROM app.kb_live) t
            UNION ALL
            SELECT 'vector', id, rank, score FROM (SELECT vvector.vsearch(qid, qvec, id, vec, del, ver, snapshot_id
                   USING PARAMETERS index_name='kb_vec', query='[0.12, -0.03, ...]', k=50) OVER() FROM app.kb_vec_snap) v) u) h
    ORDER BY h.rank;

## Performance

PERFORMANCE_PLACEHOLDER

## FAQ

**Why not Vertica's built-in text index?** It stores `(token, doc_id)` rows only: no ranking, no
positions, no phrases, no fuzzy words, and its tokenizer keeps punctuation (`Holmes'`, `Holmes,` and
`Holmes-style` are not `holmes`: an earlier test missed 5 of 37 Sherlock Holmes films that way).

**Are the scores those of Elasticsearch?** Yes, with equivalent analyzers: the same BM25 (k1 1.2,
b 0.75, Lucene's one-byte document lengths, the factor k1 + 1 that Elasticsearch and OpenSearch
apply) and the same statistics. The benchmark compares them per document ([Performance](#performance)).

**Do I have to refresh?** No: searches over `<index>_live` see every committed change. A refresh
folds the changes into segments, so queries do not analyze them again and again. Search over
`<index>_snap` when the changes since the last refresh do not matter and the last millisecond does.

**What does a refresh cost?** Proportional to the rows written since the last one, plus a load
of the new segments on every node. The benchmark measures 1% of MS MARCO.

**How do I schedule refreshes?** With Vertica's scheduler: `CREATE SCHEDULE s USING CRON '*/5 * * * *';
CREATE TRIGGER t ON SCHEDULE s EXECUTE PROCEDURE vtext.refresh_index('emails') AS DEFINER;`.
On Eon the trigger runs on the primary subcluster; a secondary subcluster needs `load_all` from a
session there (docs/design.md).

**Is the cache safe to delete?** Yes. It is a copy of the index tables; `CALL vtext.load_all('<index>')`
writes it again. A node without its cache answers with an error that says so.

**Fenced or unfenced?** Unfenced is faster (no process boundary); fenced protects the node from a
crash in the library. Both are tested; the benchmark reports both.

**How much memory?** The node cache is memory-mapped: the operating system keeps the used parts in
its file cache, shared by every query. A build needs about the size of the segments it writes; a
search needs a few MB per query plus the live tail it analyzes.

**Does it work with a column other than `body`?** Yes: `create_index(..., 'text_col=content id_col=id')`.
One text column per index; index several columns by concatenating them in a view or a table.

**Can I search several indexes at once?** One function call searches one index. Combine results in
SQL (UNION ALL, `vtext.hybrid` with weights).

## Restrictions and future work

- Languages without spaces between words (Chinese, Japanese, Thai, Lao, Khmer, Myanmar) and Korean
  are not supported ([Languages](#languages)); `on_unsupported=plain` indexes them without stemming.
- Hebrew uses light prefix stripping, not a morphological analyser. The Arabic stemmer is Snowball
  after Lucene's normalization and light prefixes.
- Scores of `w/N` and `pre/N` follow vtext's own definition (1 / distance per match) and of fuzzy
  words (a stem group of the terms matched); Elasticsearch's span and fuzzy scoring differ. Matches
  are the same.
- One text column per index. Byte offsets of words are not stored: `tsnippet` needs the text.
- A sloppy phrase with the same word twice (`"run run"~2`) uses Lucene's rules without its
  special handling of repeated words.
- Words longer than 255 bytes (after normalization) are not indexed.
- The live tail is analyzed for every query: refresh when it grows (thousands of rows are fine).
- A search reads the cache of its node; a node that missed a refresh or a load refuses to answer
  until `load_all`. On Eon, every subcluster is loaded from a session on it.
- Tested on Vertica 26.2 and 26.3, single node Enterprise and 5-node Eon. Multi-node Enterprise
  was not available for testing.

**Future work** (out of scope for this version):

- Dictionary-based lemmatization.
- German compound splitting.
- Synonyms and thesauri (MeSH, UMLS, legal thesauri). Hybrid search with vvector is the planned
  answer for meaning-based search.
- Languages written without spaces (they need dictionaries).

## Files

| Path | Content |
|---|---|
| `src/engine/` | the engine, plain C++17: analyzer, stemmer, segments, search, merge, cache, snippets |
| `src/udx/` | the Vertica functions (thin adapters) |
| `sql/` | install, procedures, uninstall |
| `tests/engine/`, `tests/sql/` | unit tests and SQL tests |
| `scripts/` | deploy, demo |
| `bench/` | benchmark: data preparation, loaders, runners, report; `bench/results/` the recorded numbers |
| `blog/` | the article with charts |
| `docs/` | design, format, Vertica notes, plan |
| `third_party/` | download and build scripts of the three static libraries |
| `tools/vtext_cli.cpp` | the engine without Vertica (build, search, merge, explain) |

## License

MIT, see LICENSE. The libraries linked in have their own licenses: ICU (Unicode License),
libstemmer_c (BSD), CRoaring (Apache 2.0 / MIT). The benchmark datasets are not part of this
repository; their licenses are listed in `bench/data/fetch.sh`.
