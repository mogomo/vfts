# vtext storage format

An index lives in Vertica tables (the data, backed up with the database) and, on every node, in a
cache directory (a copy of the active segments in a form that is read in place). Code:
`src/engine/segment.h` (segment file), `src/engine/table_codec.h` (rows), `src/engine/cache.h`
(cache directory), `sql/install.sql` (tables).

## Tables (schema `vtext`)

All index tables are `UNSEGMENTED ALL NODES` (every node holds a copy, so a node loads its cache
from local data) and partitioned by `index_id` (dropping an index drops partitions).

| Table | Columns | One row per |
|---|---|---|
| `indexes` | index_id, index_name, source_table, id_col, text_col, lang_col, ver_col, journal_table, lang, accent_fold, on_unsupported, margin_s, threads, max_segment_mb, merge_factor, cache_dir, view_schema, gen, boundary, boundary_lit, created_at, refreshed_at, refresh_seconds, refresh_note, refresh_started_at, refresh_started_by, access_opt, access, reorder | index (the manifest) |
| `index_keys` | index_id, secret, prev_secret | index: its key (docs/design.md "Access"); no grant to users |
| `segments` | index_id, segment_id, state, gen_added, gen_removed, n_docs, n_terms, sum_len, bytes, checksum, min_ver, max_ver, created | segment; active while `gen_removed IS NULL` |
| `term_dict` | index_id, segment_id, term_id, term, stem, lang, doc_freq | term and stemming language |
| `postings` | index_id, segment_id, term_id, chunk_id, docs `VARBINARY(65000)` | term and chunk of 65,536 docnos |
| `positions` | index_id, segment_id, term_id, chunk_id, payload `LONG VARBINARY(32000000)` | term and chunk |
| `doc_stats` | index_id, segment_id, docno, doc_id, length | document |
| `tombstones` | index_id, segment_id, docno, doc_id, gen | deleted document version |
| `index_stats` | index_id, gen, n_docs, sum_len, avg_length, n_segments, n_terms, bytes, created | generation |
| `probe` | k | 1,024 rows, segmented: makes `OVER(PARTITION NODES)` reach every node |

- **docno**: inside a segment, documents are numbered 0 .. n-1: in doc_id order in a new segment,
  in the order of recursive graph bisection in a merged one (docs/design.md "Merge").
- **term_id**: the rank of the term in byte order inside its segment (term ids are consecutive from 0).
- **term_dict**: `term` is normalized and not stemmed; `stem` is its stem in the language `lang`
  (the stemming language chosen for the words of that term, `''` for none). A term seen in
  documents of two languages has two rows with the same term_id. `doc_freq` counts the documents
  of the segment that contain the term (tombstones not subtracted).
- **postings.docs**: a CRoaring bitmap in the portable serialization (the format of the CRoaring,
  Java and Go libraries) of `docno mod 65536` for the docnos of chunk `chunk_id = docno div 65536`.
  A chunk is at most 8 KB as a bitmap, below the 65,000-byte VARBINARY limit.
- **positions.payload**: for every document of the chunk, in docno order: `tf` as an unsigned
  LEB128 varint, then `tf` varints: the first position, then the differences between positions.
  Positions are word numbers from 0. The largest payload of the test data (Wikipedia, frequent words
  in long articles) stays far below the 32 MB limit.
- **segments.checksum**: the checksum of the segment file (below). `tload` assembles the file from
  the rows and refuses it when the checksum differs.

Byte offsets of words are not stored: `tsnippet` analyzes the text it is given again (it is joined
from the source table anyway), which keeps the positions table about 40% smaller.

## Segment file (format 4 and 5)

`<cache_dir>/<index>/seg_<segment_id>.vts`. Little-endian, every section 8-byte aligned. Read in
place through `mmap`; nothing is parsed or copied.

### Header, 512 bytes

| Offset | Type | Field |
|---:|---|---|
| 0 | char[8] | magic `VTEXTSG1` |
| 8 | uint32 | format: 4 (docnos in doc_id order) or 5 (any order, with the doc_order section) |
| 12 | uint32 | flags (0) |
| 16 | int64 | segment_id |
| 24 | uint32 | n_docs |
| 28 | uint32 | n_terms |
| 32 | uint64 | sum_len: tokens of all documents |
| 40 | uint64 | checksum |
| 48 | uint64 | total_bytes |
| 56 | uint32 | n_stems |
| 60 | uint32 | n_nonempty: documents with at least one token |
| 64 | int64 | max_ver (0; versions are kept in `vtext.segments`) |
| 72 | uint64[14] | section offsets |
| 184 | uint64[14] | section lengths |
| 296 | uint64 | format 5: offset of the doc_order section (0 in format 4) |
| 304 | uint64 | format 5: its length, 4 * n_docs (0 in format 4) |

### Sections

| # | Section | Content |
|---:|---|---|
| 0 | doc_ids | int64[n_docs], unique; ascending in format 4 |
| 1 | doc_len | uint32[n_docs], tokens per document |
| 2 | norms | uint8[n_docs]: the length as Lucene encodes it (`SmallFloat.intToByte4`; exact below 24) |
| 3 | term_off | uint32[n_terms + 1]: offsets into term_pool |
| 4 | term_pool | the terms, UTF-8, ascending, no separators |
| 5 | term_info | TermInfo[n_terms] (32 bytes): df, n_blocks, post_off, pos_off, max_tf, min_len |
| 6 | stem_entry | StemEntry[n_stems] (24 bytes): key_off, key_len, first, count, union_df, reserved; ascending by key |
| 7 | stem_pool | the keys `<lang>\x01<stem>` |
| 8 | stem_terms | uint32 term ids, grouped by stem entry, ascending inside a group |
| 9 | postings | per term (below); followed by 16 zero bytes |
| 10 | positions | per term, per document: tf varints (position differences) |
| 11 | bitmap_dir | BitmapEntry[] (24 bytes): term, reserved, off, len; for terms with df >= 2,048 |
| 12 | bitmaps | CRoaring portable bitmaps of docnos |
| 13 | group_info | TermInfo[n_stems]: the postings of every stem group (summed tf, no positions) |
| - | doc_order | format 5 only, after section 13: uint32[n_docs], the docnos sorted by doc_id (binary search for a doc_id) |

A segment whose doc ids ascend is always written as format 4 (byte for byte the files of vtext
1.0), any other as format 5; tables hold no format, the order of the doc_stats rows decides.

### Postings of a term

- **df = 1**: no data. `post_off` holds the docno and `max_tf` the tf.
- **df > 1**: at `post_off` (8-aligned): `Impacts` of the whole list (24 bytes), then
  `BlockMeta[n_blocks]` (40 bytes each), then `L1Meta[ceil(n_blocks / 32)]` (32 bytes each), then the
  block data.
  - A block holds 128 documents (the last block fewer).
  - `BlockMeta`: last_doc (uint32), data_off (uint32, from the end of the L1Meta array), pos_off
    (uint64, from the term's pos_off: the positions of the block's first document), Impacts.
  - Block data: one byte doc_bits, one byte tf_bits, then n values `gap - 1` packed in doc_bits each
    (gap = docno minus the previous docno; the first document of the list counts from -1), then n
    values `tf - 1` packed in tf_bits each. Bits are packed LSB first.
  - `Impacts` (24 bytes): n (uint8), norm[4] (uint8), 3 bytes padding, tf[4] (uint32). Pairs
    (tf, norm) such that every document of the block has a pair with `tf_pair >= tf` and
    `norm_pair <= norm` (the Pareto frontier, reduced to 4 pairs by merging neighbours into the
    larger tf and the smaller norm). BM25 grows with tf and falls with the length, so the best
    pair bounds the score of every document of the block (MaxScore, docs/design.md).
  - `L1Meta`: last_doc (uint32), padding, Impacts of 32 blocks.
- Stem groups (`group_info`) use the same layout with `pos_off = 2^64 - 1` (no positions). A group
  of one term points to that term's postings.

### Checksum

The file is taken as little-endian 8-byte words; word i at byte 8i. The checksum is the XOR of one
value per non-zero word, mixed with its index (splitmix64 finalizer); word 5 (the checksum itself)
is left out:

    for each word w at index i, w != 0, i != 5:
        z = w + (i + 1) * 0x9E3779B97F4A7C15
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9
        z = (z ^ (z >> 27)) * 0x94D049BB133111EB
        sum ^= z ^ (z >> 31)

`tload` and `vtext_cli` verify the whole file (checksum, order of ids and terms, every block, every
impact) before it is used. Queries check the header only.

## Cache directory

| File | Content |
|---|---|
| `seg_<id>.vts` | a segment, never changed after it is written |
| `gen_<g>.vtg` | generation g: header `VTEXTGN1`, version (2; 1 in vtext 0.1.0, without access mode and keys), number of segments, g, an FNV-1a checksum of the rest, size; the index's analyzer options; the access mode and the key(s) (docs/design.md "Access"); per segment: segment_id, the segment file's checksum, its tombstoned docnos, and how the tombstones lower the document frequency of each affected term and stem group |
| `ACTIVE` | the active generation number, as text |

Files are written to a temporary name, synced and renamed; `ACTIVE` is replaced last. The files
of the active and the previous generation are kept; other `seg_*` and `gen_*` files are removed.
A missing or damaged file is never a data loss: `CALL vtext.load_all('<index>')` writes it again
from the tables.
