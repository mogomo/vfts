-- vtext install. Run through scripts/deploy.sh (make deploy), which sets :libfile and :fenced.
-- Safe to run again: tables and their data are kept.
\set ON_ERROR_STOP on

CREATE SCHEMA IF NOT EXISTS vtext;

CREATE OR REPLACE LIBRARY vtext.vtext AS :libfile LANGUAGE 'C++';

-- ---------------------------------------------------------------- index tables (docs/format.md)
-- UNSEGMENTED ALL NODES: every node holds a full copy, so loading a node's cache joins locally,
-- and the index is backed up and replicated with the database. PARTITION BY index_id: dropping
-- an index drops its partitions.

CREATE TABLE IF NOT EXISTS vtext.indexes (
    index_id           INT NOT NULL PRIMARY KEY,
    index_name         VARCHAR(64) NOT NULL,
    source_table       VARCHAR(300) NOT NULL,     -- schema.table
    id_col             VARCHAR(128) NOT NULL,
    text_col           VARCHAR(128) NOT NULL,
    lang_col           VARCHAR(128),              -- NULL: every row in the index language
    ver_col            VARCHAR(128),              -- NULL: static table, refresh rebuilds
    journal_table      VARCHAR(300),              -- schema.table (doc_id, op, ver), or NULL
    lang               VARCHAR(8) NOT NULL,
    accent_fold        VARCHAR(8) NOT NULL,
    on_unsupported     VARCHAR(8) NOT NULL,
    margin_s           INT NOT NULL,              -- seconds; see docs/design.md "Freshness"
    threads            INT NOT NULL,              -- build threads per node
    max_segment_mb     INT NOT NULL,
    merge_factor       INT NOT NULL,              -- tiered merge: segments per tier
    cache_dir          VARCHAR(1024),             -- NULL: session parameter or /tmp/vtext
    view_schema        VARCHAR(128) NOT NULL,
    gen                INT NOT NULL,              -- active generation (0: nothing built yet)
    boundary           TIMESTAMPTZ,               -- rows with ver <= boundary are in the segments
    boundary_lit       VARCHAR(80),               -- the same, as the SQL literal in the views
    created_at         TIMESTAMPTZ NOT NULL,
    refreshed_at       TIMESTAMPTZ,
    refresh_seconds    FLOAT,
    refresh_note       VARCHAR(2000),
    refresh_started_at TIMESTAMPTZ,
    refresh_started_by VARCHAR(256),
    access_opt         VARCHAR(8),                -- option access: auto (detect policies) or rows
    access             VARCHAR(256),              -- open, rows or masked:<column> (docs/design.md "Access")
    reorder            BOOLEAN DEFAULT TRUE       -- merges number documents by graph bisection (docs/design.md "Merge")
) UNSEGMENTED ALL NODES;
-- tables of vtext 0.1.0
ALTER TABLE vtext.indexes ADD COLUMN IF NOT EXISTS access_opt VARCHAR(8);
ALTER TABLE vtext.indexes ADD COLUMN IF NOT EXISTS access VARCHAR(256);
ALTER TABLE vtext.indexes ADD COLUMN IF NOT EXISTS reorder BOOLEAN DEFAULT TRUE;

-- The secret key of every index: the views prove with a token made from it that they are the
-- input of a search. Readable by the owner of the views only (no grant): a view reads it with its
-- owner's privileges, a user cannot. prev_secret: the previous key while vtext.check_access
-- changes it.
CREATE TABLE IF NOT EXISTS vtext.index_keys (
    index_id    INT NOT NULL PRIMARY KEY,
    secret      VARCHAR(64) NOT NULL,
    prev_secret VARCHAR(64)
) UNSEGMENTED ALL NODES;

CREATE TABLE IF NOT EXISTS vtext.segments (
    index_id     INT NOT NULL,
    segment_id   INT NOT NULL,
    state        VARCHAR(16) NOT NULL,            -- active, merged, dropped, purged (rows deleted)
    gen_added    INT NOT NULL,
    gen_removed  INT,                             -- NULL while active
    n_docs       INT NOT NULL,
    n_terms      INT NOT NULL,
    sum_len      INT NOT NULL,
    bytes        INT NOT NULL,                    -- size of the segment file
    checksum     INT NOT NULL,                    -- of the segment file (docs/format.md)
    min_ver      TIMESTAMPTZ,
    max_ver      TIMESTAMPTZ,
    created      TIMESTAMPTZ NOT NULL
) UNSEGMENTED ALL NODES;

CREATE TABLE IF NOT EXISTS vtext.term_dict (
    index_id    INT NOT NULL,
    segment_id  INT NOT NULL,
    term_id     INT NOT NULL,                     -- rank of the term in byte order, per segment
    term        VARCHAR(1024) NOT NULL,           -- normalized, not stemmed
    stem        VARCHAR(1024) NOT NULL,
    lang        VARCHAR(8) NOT NULL,              -- language of the stemmer ('' = none)
    doc_freq    INT NOT NULL
) ORDER BY index_id, segment_id, term UNSEGMENTED ALL NODES PARTITION BY index_id;

CREATE TABLE IF NOT EXISTS vtext.postings (
    index_id    INT NOT NULL,
    segment_id  INT NOT NULL,
    term_id     INT NOT NULL,
    chunk_id    INT NOT NULL,                     -- docno / 65536
    docs        VARBINARY(65000) NOT NULL         -- CRoaring bitmap (portable) of docno % 65536
) ORDER BY index_id, segment_id, term_id, chunk_id UNSEGMENTED ALL NODES PARTITION BY index_id;

CREATE TABLE IF NOT EXISTS vtext.positions (
    index_id    INT NOT NULL,
    segment_id  INT NOT NULL,
    term_id     INT NOT NULL,
    chunk_id    INT NOT NULL,
    part        INT NOT NULL,                     -- a payload above 65,000 bytes continues in the next part
    payload     LONG VARBINARY(65000) NOT NULL    -- per document: varint tf, then varint position deltas
) ORDER BY index_id, segment_id, term_id, chunk_id, part UNSEGMENTED ALL NODES PARTITION BY index_id;

CREATE TABLE IF NOT EXISTS vtext.doc_stats (
    index_id    INT NOT NULL,
    segment_id  INT NOT NULL,
    docno       INT NOT NULL,                     -- position of the document in its segment
    doc_id      INT NOT NULL,
    length      INT NOT NULL                      -- tokens
) ORDER BY index_id, segment_id, docno UNSEGMENTED ALL NODES PARTITION BY index_id;

CREATE TABLE IF NOT EXISTS vtext.tombstones (
    index_id    INT NOT NULL,
    segment_id  INT NOT NULL,
    docno       INT NOT NULL,
    doc_id      INT NOT NULL,
    gen         INT NOT NULL                      -- generation that deleted it
) ORDER BY index_id, segment_id, docno UNSEGMENTED ALL NODES PARTITION BY index_id;

CREATE TABLE IF NOT EXISTS vtext.index_stats (
    index_id    INT NOT NULL,
    gen         INT NOT NULL,
    n_docs      INT NOT NULL,                     -- live documents
    sum_len     INT NOT NULL,
    avg_length  FLOAT,
    n_segments  INT NOT NULL,
    n_terms     INT NOT NULL,                     -- sum over segments
    bytes       INT NOT NULL,
    created     TIMESTAMPTZ NOT NULL
) UNSEGMENTED ALL NODES;

-- Index and segment ids: never reused, so a cache file of a dropped index is never taken for a
-- newer one.
CREATE SEQUENCE IF NOT EXISTS vtext.index_seq CACHE 1;
CREATE SEQUENCE IF NOT EXISTS vtext.segment_seq CACHE 1;

-- Rows on every node, so that functions with OVER(PARTITION NODES) run on every node. It must be
-- segmented: Vertica reads an unsegmented table on one node only.
CREATE TABLE IF NOT EXISTS vtext.probe (k INT NOT NULL) SEGMENTED BY HASH(k) ALL NODES;
INSERT INTO vtext.probe
SELECT n FROM (SELECT ROW_NUMBER() OVER() AS n
               FROM (SELECT 1 FROM (SELECT '2000-01-01 00:00:00'::TIMESTAMP AS t UNION ALL SELECT '2000-01-01 00:17:03'::TIMESTAMP) b
                     TIMESERIES ts AS '1 second' OVER (ORDER BY t)) g) x
WHERE n NOT IN (SELECT k FROM vtext.probe);
COMMIT;

-- ---------------------------------------------------------------- functions

CREATE OR REPLACE TRANSFORM FUNCTION vtext.tversion AS LANGUAGE 'C++' NAME 'TVersionFactory' LIBRARY vtext.vtext :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vtext.tokenize AS LANGUAGE 'C++' NAME 'TokenizeFactory1' LIBRARY vtext.vtext :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vtext.tokenize AS LANGUAGE 'C++' NAME 'TokenizeFactory2' LIBRARY vtext.vtext :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vtext.tsearch  AS LANGUAGE 'C++' NAME 'TSearchFactoryS' LIBRARY vtext.vtext :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vtext.tcount   AS LANGUAGE 'C++' NAME 'TCountFactory'   LIBRARY vtext.vtext :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vtext.tmatch   AS LANGUAGE 'C++' NAME 'TMatchFactory'   LIBRARY vtext.vtext :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vtext.tserve   AS LANGUAGE 'C++' NAME 'TServeFactory'   LIBRARY vtext.vtext :fenced;
CREATE OR REPLACE FUNCTION vtext.tsnippet AS LANGUAGE 'C++' NAME 'TSnippetFactory2' LIBRARY vtext.vtext :fenced;
CREATE OR REPLACE FUNCTION vtext.tsnippet AS LANGUAGE 'C++' NAME 'TSnippetFactory3' LIBRARY vtext.vtext :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vtext.hybrid   AS LANGUAGE 'C++' NAME 'HybridFactory'   LIBRARY vtext.vtext :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vtext.tbuild   AS LANGUAGE 'C++' NAME 'TBuildFactory'   LIBRARY vtext.vtext :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vtext.tmerge   AS LANGUAGE 'C++' NAME 'TMergeFactory'   LIBRARY vtext.vtext :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vtext.tload    AS LANGUAGE 'C++' NAME 'TLoadFactory'    LIBRARY vtext.vtext :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vtext.tnode    AS LANGUAGE 'C++' NAME 'TNodeFactory'    LIBRARY vtext.vtext :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vtext.tinfo    AS LANGUAGE 'C++' NAME 'TInfoFactory'    LIBRARY vtext.vtext :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vtext.thave    AS LANGUAGE 'C++' NAME 'THaveFactory'    LIBRARY vtext.vtext :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vtext.tdrop    AS LANGUAGE 'C++' NAME 'TDropFactory'    LIBRARY vtext.vtext :fenced;

GRANT USAGE ON SCHEMA vtext TO PUBLIC;
GRANT SELECT ON vtext.indexes, vtext.segments, vtext.index_stats, vtext.probe TO PUBLIC;

\i sql/procedures.sql

-- Users get the search functions only. The functions that write node caches (tbuild, tmerge,
-- tload, thave, tdrop) and the procedures that manage indexes are for the index owner
-- (a superuser); vtext 0.1.0 granted every function to PUBLIC, this takes that back.
REVOKE EXECUTE ON ALL FUNCTIONS IN SCHEMA vtext FROM PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vtext.tversion() TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vtext.tokenize(LONG VARCHAR) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vtext.tokenize(LONG VARCHAR, VARCHAR) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vtext.tsearch(INT, VARCHAR, INT, LONG VARCHAR, VARCHAR, VARCHAR, INT) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vtext.tcount(INT, VARCHAR, INT, LONG VARCHAR, VARCHAR, VARCHAR, INT) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vtext.tmatch(INT, VARCHAR, INT, LONG VARCHAR, VARCHAR, VARCHAR, INT) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vtext.tserve(INT, VARCHAR, INT, LONG VARCHAR, VARCHAR, VARCHAR, INT) TO PUBLIC;
GRANT EXECUTE ON FUNCTION vtext.tsnippet(LONG VARCHAR, VARCHAR) TO PUBLIC;
GRANT EXECUTE ON FUNCTION vtext.tsnippet(LONG VARCHAR, VARCHAR, VARCHAR) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vtext.hybrid(VARCHAR, INT, INT, FLOAT) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vtext.tnode(INT) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vtext.tinfo() TO PUBLIC;
GRANT EXECUTE ON PROCEDURE vtext.info(VARCHAR) TO PUBLIC;

-- Indexes made by vtext 0.1.0 get a key, their access mode and new views (best effort: a node
-- without cache reports it, and CALL vtext.check_access('<index>') can be run again later).
CALL vtext.upgrade_access();
