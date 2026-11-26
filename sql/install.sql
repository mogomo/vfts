-- vfts install. Run through scripts/deploy.sh (make deploy), which sets :libfile and :fenced.
-- Safe to run again: tables and their data are kept.
\set ON_ERROR_STOP on

CREATE SCHEMA IF NOT EXISTS vfts;

CREATE OR REPLACE LIBRARY vfts.vfts AS :libfile LANGUAGE 'C++';

-- ---------------------------------------------------------------- index tables (docs/format.md)
-- UNSEGMENTED ALL NODES: every node holds a full copy, so loading a node's cache joins locally,
-- and the index is backed up and replicated with the database. PARTITION BY index_id: dropping
-- an index drops its partitions.

CREATE TABLE IF NOT EXISTS vfts.indexes (
    index_id           INT NOT NULL PRIMARY KEY,
    index_name         VARCHAR(64) NOT NULL,
    source_table       VARCHAR(300) NOT NULL,     -- schema.table
    id_col             VARCHAR(128) NOT NULL,
    text_col           VARCHAR(128) NOT NULL,
    lang_col           VARCHAR(128),              -- NULL: every row in the index language
    ver_col            VARCHAR(128),              -- indexes of vfts 0.4 and older only (changes are found by the row epoch now)
    journal_table      VARCHAR(300),              -- schema.table (doc_id, op), or NULL
    lang               VARCHAR(8) NOT NULL,
    accent_fold        VARCHAR(8) NOT NULL,
    on_unsupported     VARCHAR(8) NOT NULL,
    margin_s           INT NOT NULL,              -- seconds; indexes of vfts 0.4 and older only
    threads            INT NOT NULL,              -- build threads per node
    max_segment_mb     INT NOT NULL,
    merge_factor       INT NOT NULL,              -- tiered merge: segments per tier
    cache_dir          VARCHAR(1024),             -- NULL: session parameter or /tmp/vfts
    view_schema        VARCHAR(128) NOT NULL,
    gen                INT NOT NULL,              -- active generation (0: nothing built yet)
    boundary           TIMESTAMPTZ,               -- vfts 0.4 and older: rows with ver <= boundary are in the segments
    boundary_lit       VARCHAR(80),               -- the same, as the SQL literal in the views
    created_at         TIMESTAMPTZ NOT NULL,
    refreshed_at       TIMESTAMPTZ,
    refresh_seconds    FLOAT,
    refresh_note       VARCHAR(2000),
    refresh_started_at TIMESTAMPTZ,
    refresh_started_by VARCHAR(256),
    access_opt         VARCHAR(8),                -- option access: auto (detect policies) or rows
    access             VARCHAR(256),              -- open, rows or masked:<column> (docs/design.md "Access")
    reorder            BOOLEAN DEFAULT TRUE,      -- merges number documents by graph bisection (docs/design.md "Merge")
    boundary_epoch     INT,                       -- rows of the source with epoch <= boundary_epoch are in the segments
    grants_opt         VARCHAR(8),                -- source (NULL): the views follow the source table's SELECT grants; manual
    refresh_every      INT                        -- minutes between automatic refreshes (trigger vfts.refresh_<index>); 0 or NULL: off
) UNSEGMENTED ALL NODES;
-- tables of vfts 0.1.0
ALTER TABLE vfts.indexes ADD COLUMN IF NOT EXISTS access_opt VARCHAR(8);
ALTER TABLE vfts.indexes ADD COLUMN IF NOT EXISTS access VARCHAR(256);
ALTER TABLE vfts.indexes ADD COLUMN IF NOT EXISTS reorder BOOLEAN DEFAULT TRUE;
-- tables of vfts 0.4
ALTER TABLE vfts.indexes ADD COLUMN IF NOT EXISTS boundary_epoch INT;
ALTER TABLE vfts.indexes ADD COLUMN IF NOT EXISTS grants_opt VARCHAR(8);
ALTER TABLE vfts.indexes ADD COLUMN IF NOT EXISTS refresh_every INT;

-- The secret key of every index: the views prove with a token made from it that they are the
-- input of a search. Readable by the owner of the views only (no grant): a view reads it with its
-- owner's privileges, a user cannot. prev_secret: the previous key while vfts.check_access
-- changes it.
CREATE TABLE IF NOT EXISTS vfts.index_keys (
    index_id    INT NOT NULL PRIMARY KEY,
    secret      VARCHAR(64) NOT NULL,
    prev_secret VARCHAR(64)
) UNSEGMENTED ALL NODES;

CREATE TABLE IF NOT EXISTS vfts.segments (
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

CREATE TABLE IF NOT EXISTS vfts.term_dict (
    index_id    INT NOT NULL,
    segment_id  INT NOT NULL,
    term_id     INT NOT NULL,                     -- rank of the term in byte order, per segment
    term        VARCHAR(1024) NOT NULL,           -- normalized, not stemmed
    stem        VARCHAR(1024) NOT NULL,
    lang        VARCHAR(8) NOT NULL,              -- language of the stemmer ('' = none)
    doc_freq    INT NOT NULL
) ORDER BY index_id, segment_id, term UNSEGMENTED ALL NODES PARTITION BY index_id;

CREATE TABLE IF NOT EXISTS vfts.postings (
    index_id    INT NOT NULL,
    segment_id  INT NOT NULL,
    term_id     INT NOT NULL,
    chunk_id    INT NOT NULL,                     -- docno / 65536
    docs        VARBINARY(65000) NOT NULL         -- CRoaring bitmap (portable) of docno % 65536
) ORDER BY index_id, segment_id, term_id, chunk_id UNSEGMENTED ALL NODES PARTITION BY index_id;

CREATE TABLE IF NOT EXISTS vfts.positions (
    index_id    INT NOT NULL,
    segment_id  INT NOT NULL,
    term_id     INT NOT NULL,
    chunk_id    INT NOT NULL,
    part        INT NOT NULL,                     -- a payload above 65,000 bytes continues in the next part
    payload     LONG VARBINARY(65000) NOT NULL    -- per document: varint tf, then varint position deltas
) ORDER BY index_id, segment_id, term_id, chunk_id, part UNSEGMENTED ALL NODES PARTITION BY index_id;

CREATE TABLE IF NOT EXISTS vfts.doc_stats (
    index_id    INT NOT NULL,
    segment_id  INT NOT NULL,
    docno       INT NOT NULL,                     -- position of the document in its segment
    doc_id      INT NOT NULL,
    length      INT NOT NULL                      -- tokens
) ORDER BY index_id, segment_id, docno UNSEGMENTED ALL NODES PARTITION BY index_id;

CREATE TABLE IF NOT EXISTS vfts.tombstones (
    index_id    INT NOT NULL,
    segment_id  INT NOT NULL,
    docno       INT NOT NULL,
    doc_id      INT NOT NULL,
    gen         INT NOT NULL                      -- generation that deleted it
) ORDER BY index_id, segment_id, docno UNSEGMENTED ALL NODES PARTITION BY index_id;

CREATE TABLE IF NOT EXISTS vfts.index_stats (
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

-- Attributes (docs/design.md "Attributes"): a column of another table copied into the index, so
-- that searches filter and count by it (parameters filter and by). The table stays the place of
-- the value; CALL vfts.refresh_attributes copies it into vfts.attr_values and every node's cache.
-- slot: the partition of the current copy in vfts.attr_values (a new one at every refresh).
CREATE TABLE IF NOT EXISTS vfts.attributes (
    index_id        INT NOT NULL,
    attr_name       VARCHAR(64) NOT NULL,
    source_table    VARCHAR(300) NOT NULL,     -- schema.table
    key_col         VARCHAR(128) NOT NULL,     -- the index's doc_id
    value_col       VARCHAR(128) NOT NULL,
    attr_type       VARCHAR(8) NOT NULL,       -- int, bool, date or text
    version         INT NOT NULL,              -- refreshes so far
    slot            INT,
    n_rows          INT,
    refreshed_at    TIMESTAMPTZ,
    refresh_seconds FLOAT
) UNSEGMENTED ALL NODES;

-- The copies: (doc_id, value) with the value as an integer (int, bool 0/1, date: days since
-- 2000-01-01) or as text. Replicated like the index tables, so every node loads locally.
CREATE TABLE IF NOT EXISTS vfts.attr_values (
    slot   INT NOT NULL,
    doc_id INT NOT NULL,
    ival   INT,
    sval   VARCHAR(1024)
) ORDER BY slot, doc_id UNSEGMENTED ALL NODES PARTITION BY slot;

CREATE SEQUENCE IF NOT EXISTS vfts.attr_seq CACHE 1;

-- Index and segment ids: never reused, so a cache file of a dropped index is never taken for a
-- newer one.
CREATE SEQUENCE IF NOT EXISTS vfts.index_seq CACHE 1;
CREATE SEQUENCE IF NOT EXISTS vfts.segment_seq CACHE 1;

-- Rows on every node, so that functions with OVER(PARTITION NODES) run on every node. It must be
-- segmented: Vertica reads an unsegmented table on one node only.
CREATE TABLE IF NOT EXISTS vfts.probe (k INT NOT NULL) SEGMENTED BY HASH(k) ALL NODES;
INSERT INTO vfts.probe
SELECT n FROM (SELECT ROW_NUMBER() OVER() AS n
               FROM (SELECT 1 FROM (SELECT '2000-01-01 00:00:00'::TIMESTAMP AS t UNION ALL SELECT '2000-01-01 00:17:03'::TIMESTAMP) b
                     TIMESERIES ts AS '1 second' OVER (ORDER BY t)) g) x
WHERE n NOT IN (SELECT k FROM vfts.probe);
COMMIT;

-- ---------------------------------------------------------------- functions

CREATE OR REPLACE TRANSFORM FUNCTION vfts.tversion AS LANGUAGE 'C++' NAME 'TVersionFactory' LIBRARY vfts.vfts :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vfts.tokenize AS LANGUAGE 'C++' NAME 'TokenizeFactory1' LIBRARY vfts.vfts :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vfts.tokenize AS LANGUAGE 'C++' NAME 'TokenizeFactory2' LIBRARY vfts.vfts :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vfts.tsearch  AS LANGUAGE 'C++' NAME 'TSearchFactoryS' LIBRARY vfts.vfts :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vfts.tcount   AS LANGUAGE 'C++' NAME 'TCountFactory'   LIBRARY vfts.vfts :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vfts.tmatch   AS LANGUAGE 'C++' NAME 'TMatchFactory'   LIBRARY vfts.vfts :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vfts.tserve   AS LANGUAGE 'C++' NAME 'TServeFactory'   LIBRARY vfts.vfts :fenced;
CREATE OR REPLACE FUNCTION vfts.tsnippet AS LANGUAGE 'C++' NAME 'TSnippetFactory2' LIBRARY vfts.vfts :fenced;
CREATE OR REPLACE FUNCTION vfts.tsnippet AS LANGUAGE 'C++' NAME 'TSnippetFactory3' LIBRARY vfts.vfts :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vfts.hybrid   AS LANGUAGE 'C++' NAME 'HybridFactory'   LIBRARY vfts.vfts :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vfts.tbuild   AS LANGUAGE 'C++' NAME 'TBuildFactory'   LIBRARY vfts.vfts :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vfts.tmerge   AS LANGUAGE 'C++' NAME 'TMergeFactory'   LIBRARY vfts.vfts :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vfts.tload    AS LANGUAGE 'C++' NAME 'TLoadFactory'    LIBRARY vfts.vfts :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vfts.tnode    AS LANGUAGE 'C++' NAME 'TNodeFactory'    LIBRARY vfts.vfts :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vfts.tinfo    AS LANGUAGE 'C++' NAME 'TInfoFactory'    LIBRARY vfts.vfts :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vfts.thave    AS LANGUAGE 'C++' NAME 'THaveFactory'    LIBRARY vfts.vfts :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vfts.tdrop    AS LANGUAGE 'C++' NAME 'TDropFactory'    LIBRARY vfts.vfts :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vfts.tcaches  AS LANGUAGE 'C++' NAME 'TCachesFactory'  LIBRARY vfts.vfts :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vfts.tattr    AS LANGUAGE 'C++' NAME 'TAttrIntFactory'  LIBRARY vfts.vfts :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vfts.tattr    AS LANGUAGE 'C++' NAME 'TAttrTextFactory' LIBRARY vfts.vfts :fenced;

GRANT USAGE ON SCHEMA vfts TO PUBLIC;
GRANT SELECT ON vfts.indexes, vfts.segments, vfts.index_stats, vfts.probe, vfts.attributes TO PUBLIC;

\i sql/procedures.sql

-- Users get the search functions only. The functions that write node caches (tbuild, tmerge,
-- tload, thave, tdrop, tattr) and the procedures that manage indexes are for the index owner
-- (a superuser); vfts 0.1.0 granted every function to PUBLIC, this takes that back.
REVOKE EXECUTE ON ALL FUNCTIONS IN SCHEMA vfts FROM PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vfts.tversion() TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vfts.tokenize(LONG VARCHAR) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vfts.tokenize(LONG VARCHAR, VARCHAR) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vfts.tsearch(INT, VARCHAR, INT, LONG VARCHAR, VARCHAR, VARCHAR, INT) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vfts.tcount(INT, VARCHAR, INT, LONG VARCHAR, VARCHAR, VARCHAR, INT) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vfts.tmatch(INT, VARCHAR, INT, LONG VARCHAR, VARCHAR, VARCHAR, INT) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vfts.tserve(INT, VARCHAR, INT, LONG VARCHAR, VARCHAR, VARCHAR, INT) TO PUBLIC;
GRANT EXECUTE ON FUNCTION vfts.tsnippet(LONG VARCHAR, VARCHAR) TO PUBLIC;
GRANT EXECUTE ON FUNCTION vfts.tsnippet(LONG VARCHAR, VARCHAR, VARCHAR) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vfts.hybrid(VARCHAR, INT, INT, FLOAT) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vfts.tnode(INT) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vfts.tinfo() TO PUBLIC;
GRANT EXECUTE ON PROCEDURE vfts.info(VARCHAR) TO PUBLIC;

-- Indexes made by vfts 0.1.0 get a key, their access mode and new views (best effort: a node
-- without cache reports it, and CALL vfts.check_access('<index>') can be run again later).
CALL vfts.upgrade_access();

-- Caches of vfts 0.4 and older (in /tmp/vfts) move to the new default, <catalog>/vfts, on the nodes
-- of this subcluster (on Eon: CALL vfts.upgrade_caches() from a session on each other subcluster).
CALL vfts.upgrade_caches();

-- The automatic refresh of every index that has one (an uninstall removed the triggers).
CALL vfts.upgrade_schedules();
