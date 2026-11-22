-- vfts stored procedures (PL/vSQL). Installed by sql/install.sql.
--
--   vfts.create_index(name, source_table [, options])
--   vfts.refresh_index(name)              (refresh is a built-in function name in Vertica)
--   vfts.merge(name)
--   vfts.rebuild(name)
--   vfts.load_all(name)
--   vfts.check_access(name)              (after access policies or grants change: new key, views)
--   vfts.info(name)
--   vfts.drop_index(name)
--
-- Identifiers given by the caller are checked against strict patterns before they are put into
-- dynamic SQL. docs/design.md explains the order of every step.

-- ------------------------------------------------------------------ helpers

-- The views of an index, from its manifest row:
--   <schema>.<name>_live   rows newer than the segments (the live tail), journal changes, and the
--                          sentinel row with the generation. The input of tsearch and tcount.
--   <schema>.<name>_snap   the sentinel only: search the segments, ignore newer rows (fastest).
-- The sentinel carries the access token in its query column (docs/design.md "Access"):
--   access open: md5(key:gen:key), a literal.
--   access rows: the views also return the documents of the generation's segments that the user
--     cannot see in the source table (its row access policies apply to the user who queries the
--     view), as H rows of 2,500 comma-separated doc_ids, and the token md5(key:gen:user:n:s1:s2:key)
--     over those documents, with the key read from vfts.index_keys (the view owner's privilege;
--     users cannot read it).
CREATE OR REPLACE PROCEDURE vfts.make_views(nm VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    src VARCHAR(300); idc VARCHAR(128); txt VARCHAR(128); lng VARCHAR(128); vc VARCHAR(128);
    jt VARCHAR(300); vs VARCHAR(128); g INT; b VARCHAR(80); iid INT; acc VARCHAR(256); k VARCHAR(64);
    cols VARCHAR(2000); sentinel VARCHAR(16000); stmt VARCHAR(30000); lang_expr VARCHAR(300);
    hid VARCHAR(4000);
BEGIN
    SELECT source_table, id_col, text_col, lang_col, ver_col, journal_table, view_schema, gen, boundary_lit, index_id, access
      INTO src, idc, txt, lng, vc, jt, vs, g, b, iid, acc
      FROM vfts.indexes WHERE index_name = nm;
    IF src IS NULL THEN
        RAISE EXCEPTION 'vfts: index % does not exist', nm;
    END IF;
    k := (SELECT MAX(secret) FROM vfts.index_keys WHERE index_id = iid);
    IF k IS NULL OR acc IS NULL THEN
        RAISE EXCEPTION 'vfts: index % has no access key: run CALL vfts.check_access(''%'')', nm, nm;
    END IF;
    lang_expr := CASE WHEN lng IS NULL THEN 'NULL::VARCHAR(8)' ELSE 's.' || lng || '::VARCHAR(8)' END;
    IF acc = 'rows' THEN
        -- one pass: groups of hidden documents and one NULL row (the sentinel), each with the count
        -- and the two digest sums over all hidden ones (window aggregates over the whole set).
        -- Duplicates (a doc_id in two active segments) are counted on both sides.
        hid := 'SELECT d.doc_id FROM vfts.doc_stats d LEFT JOIN ' || src || ' s ON s.' || idc || ' = d.doc_id WHERE d.index_id = ' || iid
            || ' AND d.segment_id IN (SELECT segment_id FROM vfts.segments WHERE index_id = ' || iid || ' AND gen_added <= ' || g
            || ' AND (gen_removed IS NULL OR gen_removed > ' || g || ')) AND (s.' || idc || ' IS NULL'
            -- a row changed since the boundary: the segments hold an older version, whose visibility
            -- the policy cannot tell (the live view searches the current one through its tail)
            || CASE WHEN vc IS NOT NULL AND b IS NOT NULL THEN ' OR s.' || vc || ' > ' || b ELSE '' END
            || CASE WHEN jt IS NOT NULL AND b IS NOT NULL THEN ' OR d.doc_id IN (SELECT doc_id FROM ' || jt || ' WHERE ver > ' || b || ')' ELSE '' END
            || ')';
        -- H rows carry 2,500 doc_ids each, comma separated, in query: one row per hidden document
        -- would cost the declared width of body and query per row (258,701 rows: 1.36 s against
        -- 0.04 s, docs/VERTICA_NOTES.md)
        sentinel := 'SELECT NULL::INT AS qid, (CASE WHEN a.b IS NULL THEN MD5(a.secret || '':' || g || ':'' || CURRENT_USER() || '':'' || a.tn || '':'' || a.ts1 || '':'' || a.ts2 || '':'' || a.secret) ELSE a.ids END)::VARCHAR(65000) AS query, '
                 || 'NULL::INT AS doc_id, NULL::LONG VARCHAR(32000000) AS body, NULL::VARCHAR(8) AS lang, '
                 || '(CASE WHEN a.b IS NULL THEN NULL ELSE ''H'' END)::VARCHAR(1) AS op, ' || g || '::INT AS gen '
                 || 'FROM (SELECT y.b, y.ids, y.secret, SUM(y.n) OVER () AS tn, SUM(y.s1) OVER () AS ts1, SUM(y.s2) OVER () AS ts2 FROM ('
                 || 'SELECT c.b, LISTAGG(c.doc_id::VARCHAR USING PARAMETERS max_length = 65000) AS ids, COUNT(*) AS n, '
                 || 'SUM(HEX_TO_INTEGER(''0x'' || SUBSTR(c.m, 1, 8))) AS s1, SUM(HEX_TO_INTEGER(''0x'' || SUBSTR(c.m, 9, 8))) AS s2, MAX(c.secret) AS secret '
                 || 'FROM (SELECT (ROW_NUMBER() OVER () - 1) // 2500 AS b, h.doc_id, k.secret, MD5(k.secret || '':'' || h.doc_id) AS m '
                 || 'FROM (' || hid || ') h CROSS JOIN vfts.index_keys k WHERE k.index_id = ' || iid || ') c GROUP BY c.b '
                 || 'UNION ALL SELECT NULL, NULL, 0, 0, 0, k.secret FROM vfts.index_keys k WHERE k.index_id = ' || iid || ') y) a';
    ELSE
        sentinel := 'SELECT NULL::INT AS qid, ' || QUOTE_LITERAL(MD5(k || ':' || g || ':' || k)) || '::VARCHAR(65000) AS query, NULL::INT AS doc_id, '
                 || 'NULL::LONG VARCHAR(32000000) AS body, NULL::VARCHAR(8) AS lang, NULL::VARCHAR(1) AS op, ' || g || '::INT AS gen';
    END IF;
    cols := 'SELECT NULL::INT AS qid, NULL::VARCHAR(65000) AS query, s.' || idc || '::INT AS doc_id, s.' || txt
         || '::LONG VARCHAR(32000000) AS body, ' || lang_expr || ' AS lang, ''T''::VARCHAR(1) AS op, ' || g || '::INT AS gen FROM ' || src || ' s';
    stmt := 'CREATE OR REPLACE VIEW ' || vs || '.' || nm || '_live AS ';
    IF b IS NULL THEN
        IF g = 0 THEN
            stmt := stmt || cols || ' UNION ALL ';          -- nothing built yet: every row is in the tail
        END IF;
    ELSIF vc IS NOT NULL THEN
        stmt := stmt || cols || ' WHERE s.' || vc || ' > ' || b || ' UNION ALL ';
        IF jt IS NOT NULL THEN
            -- changed or deleted since the boundary (journal): mask, and search the current row
            stmt := stmt || 'SELECT NULL::INT, NULL::VARCHAR(65000), j.doc_id::INT, NULL::LONG VARCHAR(32000000), NULL::VARCHAR(8), ''D''::VARCHAR(1), '
                 || g || '::INT FROM ' || jt || ' j WHERE j.ver > ' || b || ' UNION ALL '
                 || cols || ' WHERE s.' || idc || ' IN (SELECT doc_id FROM ' || jt || ' WHERE ver > ' || b || ') AND (s.' || vc || ' <= ' || b
                 || ' OR s.' || vc || ' IS NULL) UNION ALL ';
        END IF;
    END IF;
    EXECUTE stmt || sentinel;
    EXECUTE 'CREATE OR REPLACE VIEW ' || vs || '.' || nm || '_snap AS ' || sentinel;
END;
$$;

-- The access mode of an index from the access policies of its source table, into
-- vfts.indexes.access (the caller commits):
--   masked:<column>  an enabled column access policy on the id, text, lang or version column:
--                    searches are refused (masked text must not be searchable)
--   rows             an enabled row access policy (or the option access=rows): the views hide the
--                    documents each user cannot see
--   open             no policy: the views carry a literal token
-- Only a superuser or the table's owner sees the policies (v_catalog.access_policy), so only they
-- may run this.
CREATE OR REPLACE PROCEDURE vfts.detect_access(nm VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    iid INT; src VARCHAR(300); idc VARCHAR(128); txt VARCHAR(128); lng VARCHAR(128); vc VARCHAR(128); opt VARCHAR(8);
    sup INT; own VARCHAR(256); col VARCHAR(128); nrow INT; acc VARCHAR(256);
BEGIN
    SELECT index_id, source_table, id_col, text_col, lang_col, ver_col, access_opt INTO iid, src, idc, txt, lng, vc, opt
      FROM vfts.indexes WHERE index_name = nm;
    sup := (SELECT COUNT(*) FROM v_catalog.users WHERE user_name = CURRENT_USER() AND is_super_user);
    own := (SELECT MAX(owner_name) FROM v_catalog.tables WHERE LOWER(table_schema || '.' || table_name) = LOWER(src));
    IF sup = 0 AND COALESCE(own, '') <> CURRENT_USER() THEN
        RAISE EXCEPTION 'vfts: only a superuser or the owner of % sees its access policies: run this as one of them', src;
    END IF;
    col := (SELECT MIN(column_name) FROM v_catalog.access_policy
            WHERE LOWER(table_name) = LOWER(src) AND policy_type ILIKE 'column%'
              AND (is_policy_enabled::VARCHAR ILIKE 'enabled' OR is_policy_enabled::VARCHAR ILIKE 't%')
              AND LOWER(column_name) IN (LOWER(idc), LOWER(txt), LOWER(COALESCE(lng, idc)), LOWER(COALESCE(vc, idc))));
    nrow := (SELECT COUNT(*) FROM v_catalog.access_policy
             WHERE LOWER(table_name) = LOWER(src) AND policy_type ILIKE 'row%'
               AND (is_policy_enabled::VARCHAR ILIKE 'enabled' OR is_policy_enabled::VARCHAR ILIKE 't%'));
    acc := CASE WHEN col IS NOT NULL THEN 'masked:' || col WHEN nrow > 0 OR opt = 'rows' THEN 'rows' ELSE 'open' END;
    PERFORM UPDATE vfts.indexes SET access = acc WHERE index_id = iid;
END;
$$;

-- A new random key (128 bits from /dev/urandom through UUID_GENERATE); the old one stays
-- accepted as prev_secret until vfts.check_access removes it. The caller commits.
CREATE OR REPLACE PROCEDURE vfts.new_key(iid INT) LANGUAGE PLvSQL AS $$
DECLARE
    k VARCHAR(64);
BEGIN
    k := (SELECT MD5(UUID_GENERATE()::VARCHAR || UUID_GENERATE()::VARCHAR));
    IF (SELECT COUNT(*) FROM vfts.index_keys WHERE index_id = iid) = 0 THEN
        PERFORM INSERT INTO vfts.index_keys VALUES (iid, k, NULL);
    ELSE
        PERFORM UPDATE vfts.index_keys SET prev_secret = secret, secret = k WHERE index_id = iid;
    END IF;
END;
$$;

-- ------------------------------------------------------------------ create_index

-- options: space or comma separated key=value pairs, all optional:
--   lang=en  journal=schema.table  margin=60  accent_fold=latin|none|all  on_unsupported=error|plain
--   id_col=doc_id  text_col=body  lang_col=lang  ver_col=ver  threads=0  max_segment_mb=2048
--   merge_factor=10  cache_dir=/path  build=true|false  access=auto|rows  reorder=true|false
CREATE OR REPLACE PROCEDURE vfts.create_index(nm VARCHAR, src VARCHAR, opts VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    ident VARCHAR(64) := '^[A-Za-z_][A-Za-z0-9_]*$';
    qual VARCHAR(100) := '^[A-Za-z_][A-Za-z0-9_]*\.[A-Za-z_][A-Za-z0-9_]*$';
    o VARCHAR(4000); sch VARCHAR(128); tbl VARCHAR(128);
    idc VARCHAR(128); txt VARCHAR(128); lng VARCHAR(128); vc VARCHAR(128); jt VARCHAR(300);
    lang_ VARCHAR(16); fold VARCHAR(16); unsup VARCHAR(16); margin_ INT; threads_ INT; maxmb INT; mf INT;
    cdir VARCHAR(1024); build VARCHAR(8); n INT; t VARCHAR(128); iid INT; accopt VARCHAR(8); reord VARCHAR(8);
BEGIN
    IF nm IS NULL OR NOT REGEXP_LIKE(nm, '^[A-Za-z0-9_]{1,64}$') THEN
        RAISE EXCEPTION 'vfts.create_index: index name must be 1 to 64 letters, digits or underscores';
    END IF;
    IF src IS NULL OR NOT REGEXP_LIKE(src, qual) THEN
        RAISE EXCEPTION 'vfts.create_index: source_table must be given as schema.table';
    END IF;
    IF (SELECT COUNT(*) FROM vfts.indexes WHERE index_name = nm) > 0 THEN
        RAISE EXCEPTION 'vfts.create_index: index % already exists', nm;
    END IF;
    o := ' ' || COALESCE(REPLACE(opts, ',', ' '), '') || ' ';
    sch := SPLIT_PART(src, '.', 1);
    tbl := SPLIT_PART(src, '.', 2);
    idc := COALESCE(REGEXP_SUBSTR(o, ' id_col=([^ ]+)', 1, 1, '', 1), 'doc_id');
    txt := COALESCE(REGEXP_SUBSTR(o, ' text_col=([^ ]+)', 1, 1, '', 1), 'body');
    lng := REGEXP_SUBSTR(o, ' lang_col=([^ ]+)', 1, 1, '', 1);
    vc := REGEXP_SUBSTR(o, ' ver_col=([^ ]+)', 1, 1, '', 1);
    jt := REGEXP_SUBSTR(o, ' journal=([^ ]+)', 1, 1, '', 1);
    lang_ := COALESCE(REGEXP_SUBSTR(o, ' lang=([^ ]+)', 1, 1, '', 1), 'en');
    fold := COALESCE(REGEXP_SUBSTR(o, ' accent_fold=([^ ]+)', 1, 1, '', 1), 'latin');
    unsup := COALESCE(REGEXP_SUBSTR(o, ' on_unsupported=([^ ]+)', 1, 1, '', 1), 'error');
    margin_ := COALESCE(REGEXP_SUBSTR(o, ' margin=([0-9]+)', 1, 1, '', 1), '60')::INT;
    threads_ := COALESCE(REGEXP_SUBSTR(o, ' threads=([0-9]+)', 1, 1, '', 1), '0')::INT;
    maxmb := COALESCE(REGEXP_SUBSTR(o, ' max_segment_mb=([0-9]+)', 1, 1, '', 1), '2048')::INT;
    mf := COALESCE(REGEXP_SUBSTR(o, ' merge_factor=([0-9]+)', 1, 1, '', 1), '10')::INT;
    cdir := REGEXP_SUBSTR(o, ' cache_dir=([^ ]+)', 1, 1, '', 1);
    build := COALESCE(REGEXP_SUBSTR(o, ' build=([a-z]+)', 1, 1, '', 1), 'true');
    accopt := COALESCE(REGEXP_SUBSTR(o, ' access=([a-z]+)', 1, 1, '', 1), 'auto');
    reord := COALESCE(REGEXP_SUBSTR(o, ' reorder=([a-z]+)', 1, 1, '', 1), 'true');
    IF reord NOT IN ('true', 'false') THEN
        RAISE EXCEPTION 'vfts.create_index: reorder must be true or false';
    END IF;
    IF accopt NOT IN ('auto', 'rows') THEN
        RAISE EXCEPTION 'vfts.create_index: access must be auto (follow the access policies of the table) or rows (always apply row policies)';
    END IF;
    IF NOT REGEXP_LIKE(idc, ident) OR NOT REGEXP_LIKE(txt, ident) OR NOT REGEXP_LIKE(COALESCE(lng, 'x'), ident)
       OR NOT REGEXP_LIKE(COALESCE(vc, 'x'), ident) THEN
        RAISE EXCEPTION 'vfts.create_index: column names must be plain identifiers';
    END IF;
    IF jt IS NOT NULL AND NOT REGEXP_LIKE(jt, qual) THEN
        RAISE EXCEPTION 'vfts.create_index: journal must be given as schema.table';
    END IF;
    IF cdir IS NOT NULL AND NOT REGEXP_LIKE(cdir, '^/[A-Za-z0-9_./-]*$') THEN
        RAISE EXCEPTION 'vfts.create_index: cache_dir must be an absolute path of letters, digits, _ . / -';
    END IF;
    IF fold NOT IN ('latin', 'none', 'all') OR unsup NOT IN ('error', 'plain') OR build NOT IN ('true', 'false') THEN
        RAISE EXCEPTION 'vfts.create_index: accent_fold must be latin, none or all; on_unsupported error or plain; build true or false';
    END IF;
    IF mf < 2 OR maxmb < 1 THEN
        RAISE EXCEPTION 'vfts.create_index: merge_factor must be at least 2 and max_segment_mb at least 1';
    END IF;
    -- columns: id INT, text a character type
    t := (SELECT MAX(data_type) FROM v_catalog.columns WHERE table_schema ILIKE sch AND table_name ILIKE tbl AND column_name ILIKE idc);
    IF t IS NULL THEN
        RAISE EXCEPTION 'vfts.create_index: table % or its column % does not exist', src, idc;
    END IF;
    IF NOT t ILIKE 'int%' THEN
        RAISE EXCEPTION 'vfts.create_index: % must be INT, not %', idc, t;
    END IF;
    t := (SELECT MAX(data_type) FROM v_catalog.columns WHERE table_schema ILIKE sch AND table_name ILIKE tbl AND column_name ILIKE txt);
    IF t IS NULL OR NOT (t ILIKE '%char%') THEN
        RAISE EXCEPTION 'vfts.create_index: text column % must exist and be VARCHAR or LONG VARCHAR', txt;
    END IF;
    -- lang and ver columns: taken by their contract names when the option is not given
    IF lng IS NULL AND (SELECT COUNT(*) FROM v_catalog.columns WHERE table_schema ILIKE sch AND table_name ILIKE tbl AND column_name ILIKE 'lang') > 0 THEN
        lng := 'lang';
    END IF;
    IF vc IS NULL AND (SELECT COUNT(*) FROM v_catalog.columns WHERE table_schema ILIKE sch AND table_name ILIKE tbl AND column_name ILIKE 'ver') > 0 THEN
        vc := 'ver';
    END IF;
    IF lng IS NOT NULL AND (SELECT COUNT(*) FROM v_catalog.columns WHERE table_schema ILIKE sch AND table_name ILIKE tbl AND column_name ILIKE lng) = 0 THEN
        RAISE EXCEPTION 'vfts.create_index: lang column % does not exist', lng;
    END IF;
    IF vc IS NOT NULL THEN
        t := (SELECT MAX(data_type) FROM v_catalog.columns WHERE table_schema ILIKE sch AND table_name ILIKE tbl AND column_name ILIKE vc);
        IF t IS NULL OR NOT (t ILIKE 'timestamptz%' OR t ILIKE 'timestamp with time zone%') THEN
            RAISE EXCEPTION 'vfts.create_index: version column % must exist and be TIMESTAMPTZ (found %)', vc, COALESCE(t, 'nothing');
        END IF;
    END IF;
    IF jt IS NOT NULL THEN
        IF vc IS NULL THEN
            RAISE EXCEPTION 'vfts.create_index: a journal needs a version column in the source table';
        END IF;
        n := (SELECT COUNT(*) FROM v_catalog.columns WHERE table_schema ILIKE SPLIT_PART(jt, '.', 1) AND table_name ILIKE SPLIT_PART(jt, '.', 2)
              AND column_name IN ('doc_id', 'op', 'ver'));
        IF n <> 3 THEN
            RAISE EXCEPTION 'vfts.create_index: journal % must have the columns doc_id, op, ver', jt;
        END IF;
    END IF;

    iid := (SELECT NEXTVAL('vfts.index_seq'));
    PERFORM INSERT INTO vfts.indexes (index_id, index_name, source_table, id_col, text_col, lang_col, ver_col, journal_table,
                                       lang, accent_fold, on_unsupported, margin_s, threads, max_segment_mb, merge_factor,
                                       cache_dir, view_schema, gen, created_at, access_opt, reorder)
            VALUES (iid, nm, src, idc, txt, lng, vc, jt, lang_, fold, unsup, margin_, threads_, maxmb, mf, cdir, sch, 0, CLOCK_TIMESTAMP(), accopt, reord = 'true');
    PERFORM CALL vfts.new_key(iid);
    PERFORM CALL vfts.detect_access(nm);
    PERFORM COMMIT;
    PERFORM CALL vfts.make_views(nm);
    IF build = 'true' THEN
        PERFORM CALL vfts.refresh_index(nm);
    END IF;
    RAISE NOTICE 'vfts: index % created on %. Search: SELECT * FROM (SELECT vfts.tsearch(* USING PARAMETERS index=''%'', query=''...'', k=10) OVER() FROM %.%_live) r;',
                 nm, src, nm, sch, nm;
END;
$$;

CREATE OR REPLACE PROCEDURE vfts.create_index(nm VARCHAR, src VARCHAR) LANGUAGE PLvSQL AS $$
BEGIN
    PERFORM CALL vfts.create_index(nm, src, '');
END;
$$;

-- ------------------------------------------------------------------ loading node caches

-- Writes the segment files of the rows `rows_sql` (index row shape) on every node of the session's
-- subcluster, then fails unless every UP node did it. The index tables are replicated; each node
-- joins its own copy with the one row tnode returns on that node (a local join, DISTRIB(L,L)).
-- SYNTACTIC_JOIN keeps that row as the inner side: the planner reserves the declared width of a
-- LONG VARBINARY column for every inner row, and with the index rows as the inner side a load on
-- the secondary subcluster ran out of memory (docs/VERTICA_NOTES.md).
CREATE OR REPLACE PROCEDURE vfts.load_rows(nm VARCHAR, cdir VARCHAR, rows_sql VARCHAR(65000)) LANGUAGE PLvSQL AS $$
DECLARE
    want INT; got INT; params VARCHAR(2000);
BEGIN
    want := (SELECT COUNT(*) FROM (SELECT vfts.tnode(k) OVER(PARTITION NODES) FROM vfts.probe) n);
    params := 'index=' || QUOTE_LITERAL(nm) || CASE WHEN cdir IS NULL THEN '' ELSE ', cache_dir=' || QUOTE_LITERAL(cdir) END;
    got := EXECUTE 'SELECT /*+LABEL(vfts_load)*/ COUNT(DISTINCT node_name) FROM (SELECT vfts.tload(r.kind, r.segment_id, r.term_id, r.chunk_id, '
        || 'r.docno, r.doc_id, r.n, r.term, r.stem, r.lang, r.bin USING PARAMETERS ' || params || ') OVER(PARTITION NODES) '
        || 'FROM (SELECT /*+SYNTACTIC_JOIN*/ s.* FROM (' || rows_sql || ') s CROSS JOIN /*+DISTRIB(L,L)*/ '
        || '(SELECT vfts.tnode(k) OVER(PARTITION NODES) FROM vfts.probe) p) r) l '
        || 'WHERE status = ''loaded''';
    IF got <> want THEN
        RAISE EXCEPTION 'vfts: index % was loaded on % of % nodes', nm, got, want;
    END IF;
END;
$$;

-- Makes generation g active on every node of the session's subcluster: the active segments and
-- their tombstones (G and X rows), the access mode and key(s) (K rows). Fails unless every UP node
-- did it.
CREATE OR REPLACE PROCEDURE vfts.activate(nm VARCHAR, g INT) LANGUAGE PLvSQL AS $$
DECLARE
    iid INT; cdir VARCHAR(1024); opts VARCHAR(256); want INT; got INT; params VARCHAR(2000); act VARCHAR(2000); rows_sql VARCHAR(8000);
BEGIN
    SELECT index_id, cache_dir, 'lang=' || lang || ';accent_fold=' || accent_fold || ';on_unsupported=' || on_unsupported
      INTO iid, cdir, opts FROM vfts.indexes WHERE index_name = nm;
    act := '(SELECT segment_id FROM vfts.segments WHERE index_id = ' || iid || ' AND gen_added <= ' || g
        || ' AND (gen_removed IS NULL OR gen_removed > ' || g || '))';
    rows_sql := 'SELECT ''G''::VARCHAR(1) AS kind, segment_id, NULL::INT AS term_id, NULL::INT AS chunk_id, NULL::INT AS docno, '
        || 'checksum AS doc_id, NULL::INT AS n, NULL::VARCHAR(256) AS term, NULL::VARCHAR(256) AS stem, NULL::VARCHAR(8) AS lang, '
        || 'NULL::LONG VARBINARY(65000) AS bin FROM vfts.segments WHERE index_id = ' || iid || ' AND segment_id IN ' || act
        || ' UNION ALL SELECT ''X'', segment_id, NULL, NULL, docno, doc_id, NULL, NULL, NULL, NULL, NULL FROM vfts.tombstones '
        || 'WHERE index_id = ' || iid || ' AND gen <= ' || g || ' AND segment_id IN ' || act
        -- the access mode and the key(s): read from the tables, never part of the statement text
        || ' UNION ALL SELECT ''K'', 0, NULL, NULL, NULL, NULL, NULL, i.access::VARCHAR(256), k.secret::VARCHAR(256), NULL, NULL '
        || 'FROM vfts.index_keys k JOIN vfts.indexes i ON i.index_id = k.index_id WHERE k.index_id = ' || iid
        || ' UNION ALL SELECT ''K'', 1, NULL, NULL, NULL, NULL, NULL, i.access::VARCHAR(256), k.prev_secret::VARCHAR(256), NULL, NULL '
        || 'FROM vfts.index_keys k JOIN vfts.indexes i ON i.index_id = k.index_id WHERE k.index_id = ' || iid || ' AND k.prev_secret IS NOT NULL';
    want := (SELECT COUNT(*) FROM (SELECT vfts.tnode(k) OVER(PARTITION NODES) FROM vfts.probe) n);
    params := 'index=' || QUOTE_LITERAL(nm) || ', gen=' || g || ', options=' || QUOTE_LITERAL(opts)
           || CASE WHEN cdir IS NULL THEN '' ELSE ', cache_dir=' || QUOTE_LITERAL(cdir) END;
    got := EXECUTE 'SELECT /*+LABEL(vfts_activate)*/ COUNT(DISTINCT node_name) FROM (SELECT vfts.tload(r.kind, r.segment_id, r.term_id, r.chunk_id, '
        || 'r.docno, r.doc_id, r.n, r.term, r.stem, r.lang, r.bin USING PARAMETERS ' || params || ') OVER(PARTITION NODES) '
        || 'FROM (SELECT /*+SYNTACTIC_JOIN*/ s.* FROM (' || rows_sql || ') s CROSS JOIN /*+DISTRIB(L,L)*/ '
        || '(SELECT vfts.tnode(k) OVER(PARTITION NODES) FROM vfts.probe) p) r) l '
        || 'WHERE status = ''activated''';
    IF got <> want THEN
        RAISE EXCEPTION 'vfts: generation % of index % was activated on % of % nodes', g, nm, got, want;
    END IF;
END;
$$;

-- Loads every active segment of an index from the tables into the caches of the session's
-- subcluster and activates the current generation. For new nodes, a secondary subcluster, or a
-- damaged cache. Segments a node already has are not written again.
CREATE OR REPLACE PROCEDURE vfts.load_all(nm VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    iid INT; g INT; cdir VARCHAR(1024); seg INT; n INT; i INT; rows_sql VARCHAR(8000); t0 TIMESTAMPTZ;
BEGIN
    SELECT index_id, gen, cache_dir INTO iid, g, cdir FROM vfts.indexes WHERE index_name = nm;
    IF iid IS NULL THEN
        RAISE EXCEPTION 'vfts.load_all: index % does not exist', nm;
    END IF;
    t0 := (SELECT CLOCK_TIMESTAMP());
    n := (SELECT COUNT(*) FROM vfts.segments WHERE index_id = iid AND gen_removed IS NULL);
    i := 0;
    WHILE i < n LOOP
        seg := (SELECT MAX(segment_id) FROM (SELECT segment_id, ROW_NUMBER() OVER(ORDER BY segment_id) AS rn
                FROM vfts.segments WHERE index_id = iid AND gen_removed IS NULL) s WHERE rn = i + 1);
        rows_sql := 'SELECT ''D''::VARCHAR(1) AS kind, segment_id, NULL::INT AS term_id, NULL::INT AS chunk_id, docno, doc_id, length AS n, '
            || 'NULL::VARCHAR(256) AS term, NULL::VARCHAR(256) AS stem, NULL::VARCHAR(8) AS lang, NULL::LONG VARBINARY(65000) AS bin '
            || 'FROM vfts.doc_stats WHERE index_id = ' || iid || ' AND segment_id = ' || seg
            || ' UNION ALL SELECT ''T'', segment_id, term_id, NULL, NULL, NULL, doc_freq, term::VARCHAR(256), stem::VARCHAR(256), lang, NULL FROM vfts.term_dict WHERE index_id = '
            || iid || ' AND segment_id = ' || seg
            || ' UNION ALL SELECT ''P'', segment_id, term_id, chunk_id, NULL, NULL, NULL, NULL, NULL, NULL, docs::LONG VARBINARY(65000) FROM vfts.postings WHERE index_id = '
            || iid || ' AND segment_id = ' || seg
            || ' UNION ALL SELECT ''Q'', segment_id, term_id, chunk_id, part, NULL, NULL, NULL, NULL, NULL, payload FROM vfts.positions WHERE index_id = '
            || iid || ' AND segment_id = ' || seg
            || ' UNION ALL SELECT ''S'', segment_id, n_terms, bytes, sum_len, checksum, n_docs, NULL, NULL, NULL, NULL FROM vfts.segments WHERE index_id = '
            || iid || ' AND segment_id = ' || seg;
        PERFORM CALL vfts.load_rows(nm, cdir, rows_sql);
        i := i + 1;
    END LOOP;
    PERFORM CALL vfts.activate(nm, g);
    RAISE NOTICE 'vfts: index % generation % (% segments) loaded on the nodes of this subcluster in % s', nm, g, n,
                 (SELECT ROUND(DATEDIFF('millisecond', t0, CLOCK_TIMESTAMP()) / 1000.0, 1));
END;
$$;

-- Stores the segments in the session temp table vfts_build (index rows) in the index tables, as
-- segments added by generation g_new. Returns nothing; the caller commits.
CREATE OR REPLACE PROCEDURE vfts.store_build(iid INT, g_new INT) LANGUAGE PLvSQL AS $$
BEGIN
    PERFORM INSERT /*+DIRECT*/ INTO vfts.term_dict SELECT iid, segment_id, term_id, term, stem, COALESCE(lang, ''), n FROM vfts_build WHERE kind = 'T';
    PERFORM INSERT /*+DIRECT*/ INTO vfts.postings SELECT iid, segment_id, term_id, chunk_id, bin::VARBINARY(65000) FROM vfts_build WHERE kind = 'P';
    PERFORM INSERT /*+DIRECT*/ INTO vfts.positions SELECT iid, segment_id, term_id, chunk_id, docno, bin FROM vfts_build WHERE kind = 'Q';
    PERFORM INSERT /*+DIRECT*/ INTO vfts.doc_stats SELECT iid, segment_id, docno, doc_id, n FROM vfts_build WHERE kind = 'D';
    PERFORM INSERT INTO vfts.segments (index_id, segment_id, state, gen_added, n_docs, n_terms, sum_len, bytes, checksum, created)
            SELECT iid, segment_id, 'active', g_new, n, term_id, docno, chunk_id, doc_id, CLOCK_TIMESTAMP() FROM vfts_build WHERE kind = 'S';
END;
$$;

-- The session temp table that holds a build's index rows until the commit.
CREATE OR REPLACE PROCEDURE vfts.build_table() LANGUAGE PLvSQL AS $$
BEGIN
    EXECUTE 'CREATE LOCAL TEMPORARY TABLE IF NOT EXISTS vfts_build (kind VARCHAR(1), segment_id INT, term_id INT, chunk_id INT, docno INT, '
         || 'doc_id INT, n INT, term VARCHAR(256), stem VARCHAR(256), lang VARCHAR(8), bin LONG VARBINARY(65000)) '
         || 'ON COMMIT DELETE ROWS UNSEGMENTED ALL NODES';
END;
$$;

-- Loads the segments of the session temp table vfts_build on the nodes that do not have them yet
-- (the node that built a segment wrote it into its cache already).
CREATE OR REPLACE PROCEDURE vfts.load_build(nm VARCHAR, cdir VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    nodes INT; lst VARCHAR(30000); missing VARCHAR(30000); params VARCHAR(32000);
BEGIN
    nodes := (SELECT COUNT(*) FROM (SELECT vfts.tnode(k) OVER(PARTITION NODES) FROM vfts.probe) n);
    lst := (SELECT LISTAGG(segment_id || ':' || doc_id USING PARAMETERS max_length = 30000) FROM vfts_build WHERE kind = 'S');
    IF lst IS NULL OR lst = '' THEN
        RETURN;
    END IF;
    params := 'index=' || QUOTE_LITERAL(nm) || ', segments=' || QUOTE_LITERAL(lst) || CASE WHEN cdir IS NULL THEN '' ELSE ', cache_dir=' || QUOTE_LITERAL(cdir) END;
    missing := EXECUTE 'SELECT LISTAGG(s.segment_id::VARCHAR USING PARAMETERS max_length = 30000) FROM (SELECT segment_id FROM vfts_build WHERE kind = ''S'') s '
            || 'LEFT JOIN (SELECT segment_id, COUNT(DISTINCT node_name) AS n FROM (SELECT vfts.thave(USING PARAMETERS ' || params
            || ') OVER(PARTITION NODES) FROM vfts.probe) h GROUP BY 1) h USING (segment_id) WHERE COALESCE(h.n, 0) < ' || nodes;
    IF missing IS NOT NULL AND missing <> '' THEN
        PERFORM CALL vfts.load_rows(nm, cdir, 'SELECT * FROM vfts_build WHERE segment_id IN (' || missing || ')');
    END IF;
END;
$$;

-- ------------------------------------------------------------------ refresh

-- Deletes the rows of the segments that a merge or a full build retired (they are no longer in
-- any generation that can be loaded). Vertica reclaims the space at its next mergeout.
CREATE OR REPLACE PROCEDURE vfts.purge_segments(iid INT) LANGUAGE PLvSQL AS $$
DECLARE
    lst VARCHAR(65000);
BEGIN
    lst := (SELECT LISTAGG(segment_id::VARCHAR USING PARAMETERS max_length = 65000) FROM vfts.segments
            WHERE index_id = iid AND gen_removed IS NOT NULL AND state <> 'purged');
    IF lst IS NULL OR lst = '' THEN
        RETURN;
    END IF;
    EXECUTE 'DELETE FROM vfts.term_dict  WHERE index_id = ' || iid || ' AND segment_id IN (' || lst || ')';
    EXECUTE 'DELETE FROM vfts.postings   WHERE index_id = ' || iid || ' AND segment_id IN (' || lst || ')';
    EXECUTE 'DELETE FROM vfts.positions  WHERE index_id = ' || iid || ' AND segment_id IN (' || lst || ')';
    EXECUTE 'DELETE FROM vfts.doc_stats  WHERE index_id = ' || iid || ' AND segment_id IN (' || lst || ')';
    EXECUTE 'DELETE FROM vfts.tombstones WHERE index_id = ' || iid || ' AND segment_id IN (' || lst || ')';
    EXECUTE 'UPDATE vfts.segments SET state = ''purged'' WHERE index_id = ' || iid || ' AND segment_id IN (' || lst || ')';
    PERFORM COMMIT;
END;
$$;

-- Incremental and exact: indexes the rows newer than the last boundary into new segments,
-- tombstones their older versions and journal deletes, loads every node, then switches the
-- views. docs/design.md "Refresh" explains the steps and why the result is exact.
CREATE OR REPLACE PROCEDURE vfts.refresh_index(nm VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    iid INT; src VARCHAR(300); idc VARCHAR(128); txt VARCHAR(128); lng VARCHAR(128); vc VARCHAR(128); jt VARCHAR(300);
    lang_ VARCHAR(16); fold VARCHAR(16); unsup VARCHAR(16); margin_ INT; threads_ INT; maxmb INT; cdir VARCHAR(1024);
    g INT; g_new INT; b_prev VARCHAR(80); b_new VARCHAR(80); cut TIMESTAMPTZ; t0 TIMESTAMPTZ;
    sel VARCHAR(20000); changed VARCHAR(20000); n_rows INT; n_del INT; parts INT; th INT; nodes INT; cores INT;
    seg_base INT; n_segs INT; n_tomb INT; marked INT; note VARCHAR(2000); lang_expr VARCHAR(300); secs FLOAT;
    full_ BOOLEAN; n_dead INT; me VARCHAR(256); acc_old VARCHAR(256);
BEGIN
    SELECT index_id, source_table, id_col, text_col, lang_col, ver_col, journal_table, lang, accent_fold, on_unsupported,
           margin_s, threads, max_segment_mb, cache_dir, gen, boundary_lit
      INTO iid, src, idc, txt, lng, vc, jt, lang_, fold, unsup, margin_, threads_, maxmb, cdir, g, b_prev
      FROM vfts.indexes WHERE index_name = nm;
    IF iid IS NULL THEN
        RAISE EXCEPTION 'vfts.refresh_index: index % does not exist', nm;
    END IF;
    t0 := (SELECT CLOCK_TIMESTAMP());

    -- 0. One refresh per index at a time. A mark whose session is gone (a killed refresh) is ignored.
    me := (SELECT session_id FROM v_monitor.current_session);
    marked := (SELECT COUNT(*) FROM vfts.indexes i WHERE i.index_name = nm AND i.refresh_started_at IS NOT NULL
               AND i.refresh_started_at > CLOCK_TIMESTAMP() - INTERVAL '6 hours'
               AND i.refresh_started_by IN (SELECT session_id FROM v_monitor.sessions) AND i.refresh_started_by <> me);
    IF marked > 0 THEN
        RAISE EXCEPTION 'vfts.refresh_index: a refresh of % is running (session %)', nm,
                        (SELECT refresh_started_by FROM vfts.indexes WHERE index_name = nm);
    END IF;
    PERFORM UPDATE vfts.indexes SET refresh_started_at = CLOCK_TIMESTAMP(), refresh_started_by = me WHERE index_name = nm;
    PERFORM COMMIT;

    -- 1. The new boundary, taken before the build reads the table. A row is missing from the
    --    segments only if it is committed after the build starts reading: written after now, or by
    --    a transaction open now, which holds an insert lock (v_monitor.locks) since its request.
    --    boundary = LEAST(now, earliest lock request of an open writer) - margin.
    full_ := vc IS NULL OR b_prev IS NULL;
    IF vc IS NOT NULL THEN
        cut := (SELECT LEAST(CLOCK_TIMESTAMP(), COALESCE(MIN(request_timestamp), CLOCK_TIMESTAMP()))
                FROM v_monitor.locks
                WHERE (LOWER(object_name) = LOWER('Table:' || src) OR LOWER(object_name) = LOWER('Table:' || COALESCE(jt, src)))
                  AND (lock_mode ILIKE '%I%' OR lock_mode = 'X')
                  AND transaction_id <> (SELECT transaction_id FROM v_monitor.current_session));
        cut := (SELECT cut - margin_ * INTERVAL '1 second');
        b_new := (SELECT 'TIMESTAMPTZ ''' || TO_CHAR(cut AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS.US') || '+00''');
    END IF;

    -- 2. The rows to index, and every doc_id that changed (their older versions get tombstones).
    lang_expr := CASE WHEN lng IS NULL THEN 'NULL::VARCHAR(8)' ELSE 's.' || lng || '::VARCHAR(8)' END;
    sel := 'SELECT s.' || idc || '::INT AS doc_id, s.' || txt || ' AS body, ' || lang_expr || ' AS lang FROM ' || src || ' s WHERE s.' || idc || ' IS NOT NULL';
    IF vc IS NOT NULL THEN
        IF full_ THEN
            sel := sel || ' AND (s.' || vc || ' <= ' || b_new || ' OR s.' || vc || ' IS NULL)';
        ELSE
            sel := sel || ' AND ((s.' || vc || ' > ' || b_prev || ' AND s.' || vc || ' <= ' || b_new || ')';
            IF jt IS NOT NULL THEN
                sel := sel || ' OR (s.' || idc || ' IN (SELECT doc_id FROM ' || jt || ' WHERE ver > ' || b_prev || ' AND ver <= ' || b_new
                     || ') AND (s.' || vc || ' <= ' || b_new || ' OR s.' || vc || ' IS NULL))';
            END IF;
            sel := sel || ')';
        END IF;
    END IF;
    n_rows := EXECUTE 'SELECT COUNT(*) FROM (' || sel || ') x';
    n_del := 0;
    IF NOT full_ AND jt IS NOT NULL THEN
        n_del := EXECUTE 'SELECT COUNT(*) FROM ' || jt || ' WHERE ver > ' || b_prev || ' AND ver <= ' || b_new;
    END IF;
    g_new := g + 1;

    IF n_rows = 0 AND n_del = 0 AND NOT (full_ AND g > 0) AND g > 0 THEN
        -- nothing changed: move the boundary only (and follow a change of the access policies)
        acc_old := (SELECT access FROM vfts.indexes WHERE index_name = nm);
        PERFORM CALL vfts.detect_access(nm);
        IF acc_old IS NULL OR acc_old <> (SELECT access FROM vfts.indexes WHERE index_name = nm) THEN
            PERFORM CALL vfts.activate(nm, g);
        END IF;
        PERFORM UPDATE vfts.indexes SET boundary_lit = b_new, refreshed_at = CLOCK_TIMESTAMP(), refresh_note = 'no changes',
                       refresh_started_at = NULL WHERE index_name = nm;
        PERFORM COMMIT;
        PERFORM CALL vfts.make_views(nm);
        RAISE NOTICE 'vfts: index % has no changes; boundary moved', nm;
        RETURN;
    END IF;

    -- 3. Build: segments as index rows into a session temp table (emptied by the commit).
    nodes := (SELECT COUNT(*) FROM (SELECT vfts.tnode(k) OVER(PARTITION NODES) FROM vfts.probe) n);
    cores := (SELECT COALESCE(MIN(processor_core_count), 1) FROM v_monitor.host_resources);
    -- Vertica runs several instances per node (PARTITION BEST); each gets a few threads.
    th := CASE WHEN threads_ > 0 THEN threads_ ELSE 1 END;     -- PARTITION BEST runs an instance per core already
    -- fewer than 200,000 rows: one instance (OVER()), so a small refresh makes one segment
    PERFORM CALL vfts.build_table();
    seg_base := EXECUTE 'SELECT NEXTVAL(''vfts.segment_seq'')';
    IF n_rows > 0 THEN
        EXECUTE 'INSERT /*+DIRECT, LABEL(vfts_build)*/ INTO vfts_build SELECT vfts.tbuild(0, doc_id, body, lang, NULL::INT USING PARAMETERS seg_base='
             || seg_base || ', lang=' || QUOTE_LITERAL(lang_) || ', accent_fold=' || QUOTE_LITERAL(fold) || ', on_unsupported=' || QUOTE_LITERAL(unsup)
             || ', threads=' || th || ', max_segment_mb=' || maxmb || ', index=' || QUOTE_LITERAL(nm)
             || CASE WHEN cdir IS NULL THEN '' ELSE ', cache_dir=' || QUOTE_LITERAL(cdir) END || ') OVER('
             || CASE WHEN n_rows < 200000 THEN '' ELSE 'PARTITION BEST' END || ') FROM (' || sel || ') x';
        IF (SELECT COUNT(*) - COUNT(DISTINCT segment_id) FROM vfts_build WHERE kind = 'S') > 0
           OR (SELECT COUNT(*) FROM vfts.segments WHERE segment_id IN (SELECT segment_id FROM vfts_build WHERE kind = 'S')) > 0 THEN
            RAISE EXCEPTION 'vfts.refresh_index: two segments got the same random id (about one chance in a billion): run the refresh again';
        END IF;
    END IF;

    -- 4. Store the new segments in the index tables.
    PERFORM CALL vfts.store_build(iid, g_new);
    n_segs := (SELECT COUNT(*) FROM vfts_build WHERE kind = 'S');

    -- 5. Tombstones: older versions of every changed doc_id, in the segments that were active.
    n_tomb := 0;
    IF full_ AND g > 0 THEN
        -- a full build replaces every segment
        PERFORM UPDATE vfts.segments SET state = 'dropped', gen_removed = g_new
                WHERE index_id = iid AND gen_removed IS NULL AND gen_added < g_new;
    ELSIF NOT full_ THEN
        changed := 'SELECT doc_id FROM vfts_build WHERE kind = ''D''';
        IF jt IS NOT NULL THEN
            changed := changed || ' UNION SELECT doc_id FROM ' || jt || ' WHERE ver > ' || b_prev || ' AND ver <= ' || b_new;
        END IF;
        n_tomb := EXECUTE 'INSERT /*+DIRECT*/ INTO vfts.tombstones SELECT d.index_id, d.segment_id, d.docno, d.doc_id, ' || g_new
             || ' FROM vfts.doc_stats d WHERE d.index_id = ' || iid
             || ' AND d.segment_id IN (SELECT segment_id FROM vfts.segments WHERE index_id = ' || iid || ' AND gen_removed IS NULL AND gen_added < ' || g_new || ')'
             || ' AND d.doc_id IN (' || changed || ')'
             || ' AND (d.segment_id, d.docno) NOT IN (SELECT segment_id, docno FROM vfts.tombstones WHERE index_id = ' || iid || ')';
    END IF;

    -- 6. Every node: the new segment files, then the new generation (with the access mode the
    --    table's policies give now).
    IF n_segs > 0 THEN
        PERFORM CALL vfts.load_build(nm, cdir);
    END IF;
    PERFORM CALL vfts.detect_access(nm);
    PERFORM CALL vfts.activate(nm, g_new);

    -- 7. Statistics and the manifest, in the same transaction as the rows.
    n_dead := (SELECT COUNT(*) FROM vfts.tombstones t WHERE t.index_id = iid
               AND t.segment_id IN (SELECT segment_id FROM vfts.segments WHERE index_id = iid AND gen_removed IS NULL));
    PERFORM INSERT INTO vfts.index_stats
            SELECT iid, g_new, COALESCE(SUM(n_docs), 0) - n_dead, COALESCE(SUM(sum_len), 0), NULL, COUNT(*),
                   COALESCE(SUM(n_terms), 0), COALESCE(SUM(bytes), 0), CLOCK_TIMESTAMP()
            FROM vfts.segments WHERE index_id = iid AND gen_removed IS NULL;
    secs := (SELECT DATEDIFF('millisecond', t0, CLOCK_TIMESTAMP()) / 1000.0);
    note := (CASE WHEN full_ THEN 'full build' ELSE 'incremental' END) || ': ' || n_rows || ' rows into ' || n_segs || ' segments, '
         || n_tomb || ' tombstones, ' || n_del || ' journal rows';
    PERFORM UPDATE vfts.indexes SET gen = g_new, boundary_lit = b_new, refreshed_at = CLOCK_TIMESTAMP(), refresh_seconds = secs,
                   refresh_note = note, refresh_started_at = NULL WHERE index_name = nm;
    PERFORM COMMIT;

    -- 8. Views with the new boundary and generation (DDL: after the commit; until then the old
    --    views show a wider tail, which is still exact).
    PERFORM CALL vfts.make_views(nm);
    RAISE NOTICE 'vfts: index % generation %: % (% s)', nm, g_new, note, ROUND(secs, 1);
    IF full_ THEN
        PERFORM CALL vfts.purge_segments(iid);
    END IF;
    -- 9. Tiered merge, when the policy finds a tier with merge_factor segments. Not after a full build
    --    (its segments have the same size; vfts.merge(name, 1) makes one segment of them when wanted).
    IF NOT full_ THEN
        PERFORM CALL vfts.merge(nm, 0);
    END IF;
END;
$$;

-- ------------------------------------------------------------------ merge

-- Merges segments into one, dropping tombstoned documents, in one generation step: build the new
-- segment from the node cache (tmerge), store it, mark the inputs merged, load every node, activate.
CREATE OR REPLACE PROCEDURE vfts.merge_step(nm VARCHAR, seg_list VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    iid INT; g INT; g_new INT; cdir VARCHAR(1024); new_id INT; params VARCHAR(65000); n INT; n_dead INT; reord BOOLEAN;
BEGIN
    SELECT index_id, gen, cache_dir, reorder INTO iid, g, cdir, reord FROM vfts.indexes WHERE index_name = nm;
    g_new := g + 1;
    new_id := EXECUTE 'SELECT NEXTVAL(''vfts.segment_seq'') * 2147483648';
    PERFORM CALL vfts.build_table();
    params := 'index=' || QUOTE_LITERAL(nm) || ', gen=' || g || ', segments=' || QUOTE_LITERAL(seg_list) || ', new_id=' || new_id
           || ', reorder=' || CASE WHEN reord IS FALSE THEN 'false' ELSE 'true' END
           || CASE WHEN cdir IS NULL THEN '' ELSE ', cache_dir=' || QUOTE_LITERAL(cdir) END;
    EXECUTE 'INSERT /*+DIRECT, LABEL(vfts_merge)*/ INTO vfts_build SELECT vfts.tmerge(USING PARAMETERS ' || params || ') OVER()';
    PERFORM CALL vfts.store_build(iid, g_new);
    n := EXECUTE 'UPDATE vfts.segments SET state = ''merged'', gen_removed = ' || g_new || ' WHERE index_id = ' || iid
                 || ' AND gen_removed IS NULL AND segment_id IN (' || seg_list || ')';
    PERFORM CALL vfts.load_build(nm, cdir);
    PERFORM CALL vfts.detect_access(nm);
    PERFORM CALL vfts.activate(nm, g_new);
    -- statistics of the new generation (as refresh_index writes them)
    n_dead := (SELECT COUNT(*) FROM vfts.tombstones t WHERE t.index_id = iid
               AND t.segment_id IN (SELECT segment_id FROM vfts.segments WHERE index_id = iid AND gen_removed IS NULL));
    PERFORM INSERT INTO vfts.index_stats
            SELECT iid, g_new, COALESCE(SUM(n_docs), 0) - n_dead, COALESCE(SUM(sum_len), 0), NULL, COUNT(*),
                   COALESCE(SUM(n_terms), 0), COALESCE(SUM(bytes), 0), CLOCK_TIMESTAMP()
            FROM vfts.segments WHERE index_id = iid AND gen_removed IS NULL;
    PERFORM UPDATE vfts.indexes SET gen = g_new WHERE index_name = nm;
    PERFORM COMMIT;
    PERFORM CALL vfts.make_views(nm);
    PERFORM CALL vfts.purge_segments(iid);
END;
$$;

-- max_segments = 0: the tiered policy. Segments are grouped in tiers by size (tier t holds
-- segments of merge_factor^t to merge_factor^(t+1) MB); a tier with merge_factor segments has its
-- merge_factor smallest merged into one, which lands in a higher tier. Segments with 30% or more
-- deleted documents are merged as well. Repeats until nothing qualifies.
-- max_segments = N > 0: merges the smallest segments until at most N are left (N = 1: one segment).
CREATE OR REPLACE PROCEDURE vfts.merge(nm VARCHAR, max_segments INT) LANGUAGE PLvSQL AS $$
DECLARE
    iid INT; mf INT; c INT; k INT; lst VARCHAR(65000); steps INT; t0 TIMESTAMPTZ; me VARCHAR(256); marked INT;
BEGIN
    SELECT index_id, merge_factor INTO iid, mf FROM vfts.indexes WHERE index_name = nm;
    IF iid IS NULL THEN
        RAISE EXCEPTION 'vfts.merge: index % does not exist', nm;
    END IF;
    me := (SELECT session_id FROM v_monitor.current_session);
    marked := (SELECT COUNT(*) FROM vfts.indexes i WHERE i.index_name = nm AND i.refresh_started_at IS NOT NULL
               AND i.refresh_started_by IN (SELECT session_id FROM v_monitor.sessions) AND i.refresh_started_by <> me);
    IF marked > 0 THEN
        RAISE EXCEPTION 'vfts.merge: a refresh or merge of % is running', nm;
    END IF;
    t0 := (SELECT CLOCK_TIMESTAMP());
    steps := 0;
    LOOP
        lst := NULL;
        c := (SELECT COUNT(*) FROM vfts.segments WHERE index_id = iid AND gen_removed IS NULL);
        IF max_segments > 0 THEN
            IF c > max_segments THEN
                k := c - max_segments + 1;
                lst := (SELECT LISTAGG(segment_id::VARCHAR USING PARAMETERS max_length = 65000) FROM
                        (SELECT segment_id, ROW_NUMBER() OVER(ORDER BY bytes, segment_id) AS rn FROM vfts.segments
                         WHERE index_id = iid AND gen_removed IS NULL) s WHERE rn <= k);
            END IF;
        ELSE
            lst := (SELECT LISTAGG(segment_id::VARCHAR USING PARAMETERS max_length = 65000) FROM
                    (SELECT segment_id, tier, ROW_NUMBER() OVER(PARTITION BY tier ORDER BY bytes, segment_id) AS rn,
                            COUNT(*) OVER(PARTITION BY tier) AS cnt, MIN(tier) OVER() AS t
                     FROM (SELECT segment_id, bytes, FLOOR(LN(GREATEST(bytes, 1048576) / 1048576.0) / LN(mf))::INT AS tier
                           FROM vfts.segments WHERE index_id = iid AND gen_removed IS NULL) x) y
                    WHERE cnt >= mf AND rn <= mf AND tier = (SELECT MIN(tier) FROM
                        (SELECT FLOOR(LN(GREATEST(bytes, 1048576) / 1048576.0) / LN(mf))::INT AS tier, COUNT(*) AS n
                         FROM vfts.segments WHERE index_id = iid AND gen_removed IS NULL GROUP BY 1) z WHERE n >= mf));
            IF lst IS NULL OR lst = '' THEN
                -- segments with many deletions
                lst := (SELECT LISTAGG(segment_id::VARCHAR USING PARAMETERS max_length = 65000) FROM
                        (SELECT s.segment_id, ROW_NUMBER() OVER(ORDER BY s.bytes) AS rn FROM vfts.segments s
                         JOIN (SELECT segment_id, COUNT(*) AS dead FROM vfts.tombstones WHERE index_id = iid GROUP BY segment_id) t USING (segment_id)
                         WHERE s.index_id = iid AND s.gen_removed IS NULL AND t.dead >= 0.3 * s.n_docs) d WHERE rn <= mf);
            END IF;
        END IF;
        EXIT WHEN lst IS NULL OR lst = '';
        PERFORM CALL vfts.merge_step(nm, lst);
        steps := steps + 1;
        EXIT WHEN steps >= 1000;
    END LOOP;
    IF steps > 0 THEN
        RAISE NOTICE 'vfts: index % merged in % steps (% s); % segments now', nm, steps,
                     (SELECT TO_CHAR(DATEDIFF('millisecond', t0, CLOCK_TIMESTAMP()) / 1000.0, 'FM999999990.0')),
                     (SELECT COUNT(*) FROM vfts.segments WHERE index_id = iid AND gen_removed IS NULL);
    END IF;
END;
$$;

CREATE OR REPLACE PROCEDURE vfts.merge(nm VARCHAR) LANGUAGE PLvSQL AS $$
BEGIN
    PERFORM CALL vfts.merge(nm, 0);
END;
$$;

-- ------------------------------------------------------------------ info, drop, rebuild

CREATE OR REPLACE PROCEDURE vfts.info(nm VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    iid INT; g INT; src VARCHAR(300); vs VARCHAR(128); b VARCHAR(80); note VARCHAR(2000); cdir VARCHAR(1024);
    segs INT; docs INT; dead INT; nbytes INT; tail INT; nodes INT; ok INT; params VARCHAR(2000); acc VARCHAR(256);
BEGIN
    SELECT index_id, gen, source_table, view_schema, boundary_lit, refresh_note, cache_dir, access INTO iid, g, src, vs, b, note, cdir, acc
      FROM vfts.indexes WHERE index_name = nm;
    IF iid IS NULL THEN
        RAISE EXCEPTION 'vfts.info: index % does not exist', nm;
    END IF;
    SELECT COUNT(*), COALESCE(SUM(n_docs), 0), COALESCE(SUM(bytes), 0) INTO segs, docs, nbytes
      FROM vfts.segments WHERE index_id = iid AND gen_removed IS NULL;
    dead := (SELECT COUNT(*) FROM vfts.tombstones WHERE index_id = iid
             AND segment_id IN (SELECT segment_id FROM vfts.segments WHERE index_id = iid AND gen_removed IS NULL));
    tail := EXECUTE 'SELECT COUNT(*) FROM ' || vs || '.' || nm || '_live WHERE doc_id IS NOT NULL AND COALESCE(op, '''') <> ''H''';
    params := 'index=' || QUOTE_LITERAL(nm) || CASE WHEN cdir IS NULL THEN '' ELSE ', cache_dir=' || QUOTE_LITERAL(cdir) END;
    nodes := (SELECT COUNT(*) FROM (SELECT vfts.tnode(k) OVER(PARTITION NODES) FROM vfts.probe) n);
    ok := EXECUTE 'SELECT COUNT(*) FROM (SELECT vfts.tinfo(USING PARAMETERS ' || params || ') OVER(PARTITION NODES) FROM vfts.probe) i WHERE gen = ' || g;
    RAISE NOTICE 'index %: source %, generation %, % segments, % documents (% deleted), % MB', nm, src, g, segs, docs - dead, dead,
                 TO_CHAR(nbytes / 1048576.0, 'FM999999990.0');
    RAISE NOTICE 'boundary %; live tail % rows (%.%_live); last refresh: %', COALESCE(b, '(none)'), tail, vs, nm, COALESCE(note, '(none)');
    RAISE NOTICE 'access: % (open: no access policy; rows: each user searches the rows that row access policies show; masked:<column>: searches refused)',
                 COALESCE(acc, '(no key: run CALL vfts.check_access)');
    RAISE NOTICE 'node caches: % of % nodes of this subcluster have generation % (details: SELECT * FROM (SELECT vfts.tinfo(USING PARAMETERS index=''%'') OVER(PARTITION NODES) FROM vfts.probe) i;)',
                 ok, nodes, g, nm;
END;
$$;

CREATE OR REPLACE PROCEDURE vfts.drop_index(nm VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    iid INT; vs VARCHAR(128); cdir VARCHAR(1024); params VARCHAR(2000); n INT;
BEGIN
    SELECT index_id, view_schema, cache_dir INTO iid, vs, cdir FROM vfts.indexes WHERE index_name = nm;
    IF iid IS NULL THEN
        RAISE EXCEPTION 'vfts.drop_index: index % does not exist', nm;
    END IF;
    params := 'index=' || QUOTE_LITERAL(nm) || CASE WHEN cdir IS NULL THEN '' ELSE ', cache_dir=' || QUOTE_LITERAL(cdir) END;
    n := EXECUTE 'SELECT COUNT(*) FROM (SELECT vfts.tdrop(USING PARAMETERS ' || params || ') OVER(PARTITION NODES) FROM vfts.probe) d';
    EXECUTE 'DROP VIEW IF EXISTS ' || vs || '.' || nm || '_live';
    EXECUTE 'DROP VIEW IF EXISTS ' || vs || '.' || nm || '_snap';
    PERFORM SELECT DROP_PARTITIONS('vfts.term_dict', iid, iid);
    PERFORM SELECT DROP_PARTITIONS('vfts.postings', iid, iid);
    PERFORM SELECT DROP_PARTITIONS('vfts.positions', iid, iid);
    PERFORM SELECT DROP_PARTITIONS('vfts.doc_stats', iid, iid);
    PERFORM SELECT DROP_PARTITIONS('vfts.tombstones', iid, iid);
    PERFORM DELETE FROM vfts.segments WHERE index_id = iid;
    PERFORM DELETE FROM vfts.index_stats WHERE index_id = iid;
    PERFORM DELETE FROM vfts.index_keys WHERE index_id = iid;
    PERFORM DELETE FROM vfts.indexes WHERE index_id = iid;
    PERFORM COMMIT;
    RAISE NOTICE 'vfts: index % dropped (caches removed on % nodes of this subcluster)', nm, n;
END;
$$;

-- For recovery only: indexes every row again, as one new set of segments.
CREATE OR REPLACE PROCEDURE vfts.rebuild(nm VARCHAR) LANGUAGE PLvSQL AS $$
BEGIN
    IF (SELECT COUNT(*) FROM vfts.indexes WHERE index_name = nm) = 0 THEN
        RAISE EXCEPTION 'vfts.rebuild: index % does not exist', nm;
    END IF;
    PERFORM UPDATE vfts.indexes SET boundary_lit = NULL WHERE index_name = nm;
    PERFORM COMMIT;
    PERFORM CALL vfts.refresh_index(nm);
END;
$$;

-- ------------------------------------------------------------------ access

-- Run after creating, changing, enabling or dropping an access policy on the source table, and
-- after revoking SELECT on the views from someone: re-reads the policies (access mode), makes a
-- new key (tokens that anyone saw before stop working), activates it on the nodes of this
-- subcluster and makes new views. refresh_index and merge re-read the policies too.
CREATE OR REPLACE PROCEDURE vfts.check_access(nm VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    iid INT; g INT;
BEGIN
    SELECT index_id, gen INTO iid, g FROM vfts.indexes WHERE index_name = nm;
    IF iid IS NULL THEN
        RAISE EXCEPTION 'vfts.check_access: index % does not exist', nm;
    END IF;
    PERFORM CALL vfts.detect_access(nm);
    PERFORM CALL vfts.new_key(iid);
    PERFORM COMMIT;
    -- the nodes accept the old and the new key while the views change, then the new one only
    IF g > 0 THEN
        PERFORM CALL vfts.activate(nm, g);
    END IF;
    PERFORM CALL vfts.make_views(nm);
    PERFORM UPDATE vfts.index_keys SET prev_secret = NULL WHERE index_id = iid;
    PERFORM COMMIT;
    IF g > 0 THEN
        PERFORM CALL vfts.activate(nm, g);
    END IF;
    RAISE NOTICE 'vfts: index % access %, new key active', nm, (SELECT access FROM vfts.indexes WHERE index_id = iid);
END;
$$;

-- For indexes made by vfts 0.1.0 (no key): check_access for each. Run by sql/install.sql.
CREATE OR REPLACE PROCEDURE vfts.upgrade_access() LANGUAGE PLvSQL AS $$
DECLARE
    nm VARCHAR(64); last VARCHAR(64);
BEGIN
    last := '';
    LOOP
        nm := (SELECT MIN(index_name) FROM vfts.indexes
               WHERE (access IS NULL OR index_id NOT IN (SELECT index_id FROM vfts.index_keys)) AND index_name > last);
        EXIT WHEN nm IS NULL;
        last := nm;
        BEGIN
            PERFORM CALL vfts.check_access(nm);
        EXCEPTION WHEN OTHERS THEN
            RAISE NOTICE 'vfts: index % has no key yet (%): run CALL vfts.check_access(''%'') when its caches are loaded', nm, SQLERRM, nm;
        END;
    END LOOP;
END;
$$;
