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
--   vfts.add_attribute(name, attribute, table, key_column, value_column)
--   vfts.refresh_attributes(name)
--   vfts.drop_attribute(name, attribute)
--
-- Identifiers given by the caller are checked against strict patterns before they are put into
-- dynamic SQL. docs/design.md explains the order of every step.

-- ------------------------------------------------------------------ helpers

-- The views of an index, from its manifest row:
--   <schema>.<name>_live   rows newer than the segments (the live tail: epoch > boundary_epoch),
--                          journal changes, and the sentinel row with the generation. The input of
--                          tsearch and tcount.
--   <schema>.<name>_snap   the sentinel only: search the segments, ignore newer rows (fastest).
-- The tail is found by the row epoch, Vertica's commit epoch of every row: an INSERT, an UPDATE,
-- a COPY, MOVE/SWAP/COPY_PARTITIONS and a restore all give their rows a new epoch, so the source
-- needs no version column (docs/design.md "Freshness"). Indexes of vfts 0.4 and older keep their
-- ver boundary until their next refresh.
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
    hid VARCHAR(4000); e INT; chg VARCHAR(1000); jchg VARCHAR(1000);
BEGIN
    SELECT source_table, id_col, text_col, lang_col, ver_col, journal_table, view_schema, gen, boundary_lit, index_id, access, boundary_epoch
      INTO src, idc, txt, lng, vc, jt, vs, g, b, iid, acc, e
      FROM vfts.indexes WHERE index_name = nm;
    IF src IS NULL THEN
        RAISE EXCEPTION 'vfts: index % does not exist', nm;
    END IF;
    k := (SELECT MAX(secret) FROM vfts.index_keys WHERE index_id = iid);
    IF k IS NULL OR acc IS NULL THEN
        RAISE EXCEPTION 'vfts: index % has no access key: run CALL vfts.check_access(''%'')', nm, nm;
    END IF;
    lang_expr := CASE WHEN lng IS NULL THEN 'NULL::VARCHAR(8)' ELSE 's.' || lng || '::VARCHAR(8)' END;
    -- rows changed since the segments were built: by epoch, or (an index of vfts 0.4 and older
    -- before its first refresh) by its ver boundary
    chg := CASE WHEN e IS NOT NULL THEN 's.epoch > ' || e WHEN vc IS NOT NULL AND b IS NOT NULL THEN 's.' || vc || ' > ' || b END;
    jchg := CASE WHEN jt IS NULL THEN NULL WHEN e IS NOT NULL THEN 'SELECT doc_id FROM ' || jt || ' WHERE epoch > ' || e
                 WHEN b IS NOT NULL THEN 'SELECT doc_id FROM ' || jt || ' WHERE ver > ' || b END;
    IF acc = 'rows' THEN
        -- one pass: groups of hidden documents and one NULL row (the sentinel), each with the count
        -- and the two digest sums over all hidden ones (window aggregates over the whole set).
        -- Duplicates (a doc_id in two active segments) are counted on both sides.
        hid := 'SELECT d.doc_id FROM vfts.doc_stats d LEFT JOIN ' || src || ' s ON s.' || idc || ' = d.doc_id WHERE d.index_id = ' || iid
            || ' AND d.segment_id IN (SELECT segment_id FROM vfts.segments WHERE index_id = ' || iid || ' AND gen_added <= ' || g
            || ' AND (gen_removed IS NULL OR gen_removed > ' || g || ')) AND (s.' || idc || ' IS NULL'
            -- a row changed since the boundary: the segments hold an older version, whose visibility
            -- the policy cannot tell (the live view searches the current one through its tail)
            || CASE WHEN chg IS NOT NULL THEN ' OR ' || chg ELSE '' END
            || CASE WHEN jchg IS NOT NULL THEN ' OR d.doc_id IN (' || jchg || ')' ELSE '' END
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
    IF chg IS NULL THEN
        IF g = 0 THEN
            stmt := stmt || cols || ' UNION ALL ';          -- nothing built yet: every row is in the tail
        END IF;
    ELSE
        -- `epoch > e` alone, so that Vertica skips every storage container older than the
        -- boundary (with `OR epoch IS NULL` it reads them all): a session's own uncommitted rows
        -- are not in the tail, committed rows are.
        stmt := stmt || cols || ' WHERE ' || chg || ' UNION ALL ';
        IF jchg IS NOT NULL THEN
            -- changed or deleted since the boundary (journal): mask, and search the current row
            stmt := stmt || 'SELECT NULL::INT, NULL::VARCHAR(65000), j.doc_id::INT, NULL::LONG VARCHAR(32000000), NULL::VARCHAR(8), ''D''::VARCHAR(1), '
                 || g || '::INT FROM (' || jchg || ') j UNION ALL '
                 || cols || ' WHERE s.' || idc || ' IN (' || jchg || ') AND (NOT (' || chg || ')'
                 || CASE WHEN e IS NULL THEN ' OR s.' || vc || ' IS NULL' ELSE '' END || ') UNION ALL ';
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
--   lang=en  journal=schema.table  accent_fold=latin|none|all  on_unsupported=error|plain
--   id_col=doc_id  text_col=body  lang_col=lang  threads=0  max_segment_mb=2048
--   merge_factor=10  cache_dir=/path  build=true|false  access=auto|rows  reorder=true|false
--   grants=source|manual
CREATE OR REPLACE PROCEDURE vfts.create_index(nm VARCHAR, src VARCHAR, opts VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    ident VARCHAR(64) := '^[A-Za-z_][A-Za-z0-9_]*$';
    qual VARCHAR(100) := '^[A-Za-z_][A-Za-z0-9_]*\.[A-Za-z_][A-Za-z0-9_]*$';
    o VARCHAR(4000); sch VARCHAR(128); tbl VARCHAR(128);
    idc VARCHAR(128); txt VARCHAR(128); lng VARCHAR(128); vc VARCHAR(128); jt VARCHAR(300);
    lang_ VARCHAR(16); fold VARCHAR(16); unsup VARCHAR(16); margin_ INT; threads_ INT; maxmb INT; mf INT;
    cdir VARCHAR(1024); build VARCHAR(8); n INT; t VARCHAR(128); iid INT; accopt VARCHAR(8); reord VARCHAR(8); grt VARCHAR(8);
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
    grt := COALESCE(REGEXP_SUBSTR(o, ' grants=([a-z]+)', 1, 1, '', 1), 'source');
    IF grt NOT IN ('source', 'manual') THEN
        RAISE EXCEPTION 'vfts.create_index: grants must be source (the views follow the SELECT grants of the table) or manual';
    END IF;
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
    -- the lang column: taken by its contract name when the option is not given
    IF lng IS NULL AND (SELECT COUNT(*) FROM v_catalog.columns WHERE table_schema ILIKE sch AND table_name ILIKE tbl AND column_name ILIKE 'lang') > 0 THEN
        lng := 'lang';
    END IF;
    IF lng IS NOT NULL AND (SELECT COUNT(*) FROM v_catalog.columns WHERE table_schema ILIKE sch AND table_name ILIKE tbl AND column_name ILIKE lng) = 0 THEN
        RAISE EXCEPTION 'vfts.create_index: lang column % does not exist', lng;
    END IF;
    -- changes are found by the row epoch (docs/design.md "Freshness"): a version column is not
    -- needed; the options ver_col and margin of vfts 0.4 are accepted and not used
    IF vc IS NOT NULL THEN
        RAISE NOTICE 'vfts.create_index: ver_col is not needed since vfts 0.5.0 (changes are found by the row epoch); ignored';
        vc := NULL;
    END IF;
    IF jt IS NOT NULL THEN
        n := (SELECT COUNT(*) FROM v_catalog.columns WHERE table_schema ILIKE SPLIT_PART(jt, '.', 1) AND table_name ILIKE SPLIT_PART(jt, '.', 2)
              AND column_name IN ('doc_id', 'op'));
        IF n <> 2 THEN
            RAISE EXCEPTION 'vfts.create_index: journal % must have the columns doc_id and op', jt;
        END IF;
    END IF;

    iid := (SELECT NEXTVAL('vfts.index_seq'));
    PERFORM INSERT INTO vfts.indexes (index_id, index_name, source_table, id_col, text_col, lang_col, ver_col, journal_table,
                                       lang, accent_fold, on_unsupported, margin_s, threads, max_segment_mb, merge_factor,
                                       cache_dir, view_schema, gen, created_at, access_opt, reorder, grants_opt)
            VALUES (iid, nm, src, idc, txt, lng, vc, jt, lang_, fold, unsup, margin_, threads_, maxmb, mf, cdir, sch, 0, CLOCK_TIMESTAMP(), accopt, reord = 'true', grt);
    PERFORM CALL vfts.new_key(iid);
    PERFORM CALL vfts.detect_access(nm);
    PERFORM COMMIT;
    PERFORM CALL vfts.make_views(nm);
    PERFORM CALL vfts.sync_grants(nm, FALSE);
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
    iid INT; g INT; cdir VARCHAR(1024); seg INT; n INT; i INT; rows_sql VARCHAR(8000); t0 TIMESTAMPTZ; na INT; a VARCHAR(64);
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
    -- the attributes, from their copies (not from the tables: those may have changed since)
    na := (SELECT COUNT(*) FROM vfts.attributes WHERE index_id = iid AND slot IS NOT NULL);
    i := 0;
    WHILE i < na LOOP
        a := (SELECT MAX(attr_name) FROM (SELECT attr_name, ROW_NUMBER() OVER(ORDER BY attr_name) AS rn FROM vfts.attributes
              WHERE index_id = iid AND slot IS NOT NULL) x WHERE rn = i + 1);
        PERFORM CALL vfts.load_attribute(nm, a);
        i := i + 1;
    END LOOP;
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

-- The session temp tables, emptied by the commit: vfts_build holds a build's index rows,
-- vfts_recon the doc_ids a refresh found gone from the source (G) or missing from the index (M).
CREATE OR REPLACE PROCEDURE vfts.build_table() LANGUAGE PLvSQL AS $$
BEGIN
    EXECUTE 'CREATE LOCAL TEMPORARY TABLE IF NOT EXISTS vfts_build (kind VARCHAR(1), segment_id INT, term_id INT, chunk_id INT, docno INT, '
         || 'doc_id INT, n INT, term VARCHAR(256), stem VARCHAR(256), lang VARCHAR(8), bin LONG VARBINARY(65000)) '
         || 'ON COMMIT DELETE ROWS UNSEGMENTED ALL NODES';
    EXECUTE 'CREATE LOCAL TEMPORARY TABLE IF NOT EXISTS vfts_recon (doc_id INT, kind VARCHAR(1)) ON COMMIT DELETE ROWS UNSEGMENTED ALL NODES';
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

-- Incremental and exact: indexes the rows committed since the last refresh (their epoch is newer
-- than the boundary) into new segments, tombstones their older versions, journal deletes and the
-- documents that left the source table, loads every node, then switches the views.
-- docs/design.md "Refresh" explains the steps and why the result is exact.
CREATE OR REPLACE PROCEDURE vfts.refresh_index(nm VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    iid INT; src VARCHAR(300); idc VARCHAR(128); txt VARCHAR(128); lng VARCHAR(128); vc VARCHAR(128); jt VARCHAR(300);
    lang_ VARCHAR(16); fold VARCHAR(16); unsup VARCHAR(16); threads_ INT; maxmb INT; cdir VARCHAR(1024);
    g INT; g_new INT; b_prev VARCHAR(80); e_prev INT; e_new INT; t0 TIMESTAMPTZ;
    rng VARCHAR(2000); jsel VARCHAR(2000); base VARCHAR(2000); sel VARCHAR(20000); changed VARCHAR(20000); act VARCHAR(2000); live VARCHAR(4000);
    n_rows INT; n_del INT; n_live INT; n_masked INT; n_src INT; n_gone INT; n_missing INT; th INT;
    seg_base INT; n_segs INT; n_tomb INT; marked INT; note VARCHAR(2000); lang_expr VARCHAR(300); secs FLOAT;
    full_ BOOLEAN; n_dead INT; me VARCHAR(256); acc_old VARCHAR(256);
BEGIN
    SELECT index_id, source_table, id_col, text_col, lang_col, ver_col, journal_table, lang, accent_fold, on_unsupported,
           threads, max_segment_mb, cache_dir, gen, boundary_lit, boundary_epoch
      INTO iid, src, idc, txt, lng, vc, jt, lang_, fold, unsup, threads_, maxmb, cdir, g, b_prev, e_prev
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
    PERFORM CALL vfts.build_table();        -- the session temp tables (DDL: before anything is written)

    -- 1. The new boundary. Every row committed so far has an epoch below the current epoch, and
    --    every statement below sees it (a statement sees what was committed when it started); a row
    --    committed from now on gets the current epoch or a later one. So the segments take the rows
    --    with epoch <= e_new and the live view the rows with epoch > e_new: none is missed, none is
    --    taken twice, without clocks or margins.
    e_new := (SELECT current_epoch FROM v_monitor.system) - 1;
    full_ := e_prev IS NULL AND b_prev IS NULL;

    -- 2. The rows to index: committed since the last refresh (an index of vfts 0.4 and older: its
    --    first refresh after the upgrade takes the rows newer than its ver boundary), and the current
    --    row of every doc_id in the journal since then.
    lang_expr := CASE WHEN lng IS NULL THEN 'NULL::VARCHAR(8)' ELSE 's.' || lng || '::VARCHAR(8)' END;
    rng := CASE WHEN full_ THEN 's.epoch <= ' || e_new
                WHEN e_prev IS NOT NULL THEN 's.epoch > ' || e_prev || ' AND s.epoch <= ' || e_new
                ELSE 's.' || vc || ' > ' || b_prev || ' AND s.epoch <= ' || e_new END;
    jsel := CASE WHEN jt IS NULL OR full_ THEN NULL
                 WHEN e_prev IS NOT NULL THEN 'SELECT doc_id FROM ' || jt || ' WHERE epoch > ' || e_prev || ' AND epoch <= ' || e_new
                 ELSE 'SELECT doc_id FROM ' || jt || ' WHERE ver > ' || b_prev || ' AND epoch <= ' || e_new END;
    base := 'SELECT s.' || idc || '::INT AS doc_id, s.' || txt || ' AS body, ' || lang_expr || ' AS lang FROM ' || src || ' s WHERE s.' || idc || ' IS NOT NULL AND (('
         || rng || ')' || CASE WHEN jsel IS NULL THEN '' ELSE ' OR (s.' || idc || ' IN (' || jsel || ') AND s.epoch <= ' || e_new || ')' END;
    sel := base || ')';
    n_rows := EXECUTE 'SELECT COUNT(*) FROM (' || sel || ') x';
    n_del := 0;
    IF jsel IS NOT NULL THEN
        n_del := EXECUTE 'SELECT COUNT(*) FROM (' || jsel || ') j';
    END IF;
    g_new := g + 1;

    -- 3. Rows that left the source table without a journal row (DELETE, DROP_PARTITIONS,
    --    MOVE_PARTITIONS_TO_TABLE, TRUNCATE, a restore of an older copy) have no epoch to find
    --    them by. Every other change gives its rows a new epoch, so with unique doc_ids the source
    --    has exactly the documents the index will have, unless some left: compare the counts
    --    (one scan of the doc_id column), and only when they differ find the documents that left
    --    (an anti-join of the index's doc_ids with the source's) and any source row the index lacks.
    n_gone := 0;
    n_missing := 0;
    IF NOT full_ THEN
        act := '(SELECT segment_id FROM vfts.segments WHERE index_id = ' || iid || ' AND gen_removed IS NULL)';
        live := 'SELECT d.doc_id FROM vfts.doc_stats d WHERE d.index_id = ' || iid || ' AND d.segment_id IN ' || act
             || ' AND (d.segment_id, d.docno) NOT IN (SELECT segment_id, docno FROM vfts.tombstones WHERE index_id = ' || iid || ')';
        changed := 'SELECT doc_id FROM (' || sel || ') x' || CASE WHEN jsel IS NULL THEN '' ELSE ' UNION ' || jsel END;
        n_live := (SELECT COALESCE(SUM(n_docs), 0) FROM vfts.segments WHERE index_id = iid AND gen_removed IS NULL)
                - (SELECT COUNT(*) FROM vfts.tombstones t WHERE t.index_id = iid
                   AND t.segment_id IN (SELECT segment_id FROM vfts.segments WHERE index_id = iid AND gen_removed IS NULL));
        n_masked := 0;
        IF n_rows > 0 OR n_del > 0 THEN
            n_masked := EXECUTE 'SELECT COUNT(*) FROM (' || live || ') l WHERE l.doc_id IN (' || changed || ')';
        END IF;
        n_src := EXECUTE 'SELECT COUNT(*) FROM ' || src || ' s WHERE s.' || idc || ' IS NOT NULL AND s.epoch <= ' || e_new;
        IF n_src <> n_live - n_masked + n_rows THEN
            n_gone := EXECUTE 'INSERT INTO vfts_recon SELECT l.doc_id, ''G'' FROM (' || live || ') l WHERE l.doc_id NOT IN (SELECT s.' || idc
                   || ' FROM ' || src || ' s WHERE s.' || idc || ' IS NOT NULL)';
            n_missing := EXECUTE 'INSERT INTO vfts_recon SELECT s.' || idc || ', ''M'' FROM ' || src || ' s WHERE s.' || idc || ' IS NOT NULL AND s.epoch <= ' || e_new
                   || ' AND s.' || idc || ' NOT IN (' || live || ') AND s.' || idc || ' NOT IN (SELECT doc_id FROM (' || sel || ') x)';
            IF n_missing > 0 THEN
                sel := base || ' OR (s.' || idc || ' IN (SELECT doc_id FROM vfts_recon WHERE kind = ''M'') AND s.epoch <= ' || e_new || '))';
                n_rows := n_rows + n_missing;
            END IF;
            IF n_src <> n_live - n_masked + n_rows - n_gone THEN
                RAISE NOTICE 'vfts: % has % rows (doc_id not NULL), the index % documents: are the doc_ids unique?',
                             src, n_src, n_live - n_masked + n_rows - n_gone;
            END IF;
        END IF;
    END IF;

    IF n_rows = 0 AND n_del = 0 AND n_gone = 0 AND NOT (full_ AND g > 0) AND g > 0 THEN
        -- nothing changed: move the boundary only (and follow a change of the access policies)
        acc_old := (SELECT access FROM vfts.indexes WHERE index_name = nm);
        PERFORM CALL vfts.detect_access(nm);
        IF acc_old IS NULL OR acc_old <> (SELECT access FROM vfts.indexes WHERE index_name = nm) THEN
            PERFORM CALL vfts.activate(nm, g);
        END IF;
        PERFORM UPDATE vfts.indexes SET boundary_epoch = e_new, boundary_lit = NULL, refreshed_at = CLOCK_TIMESTAMP(), refresh_note = 'no changes',
                       refresh_started_at = NULL WHERE index_name = nm;
        PERFORM COMMIT;
        PERFORM CALL vfts.make_views(nm);
        PERFORM CALL vfts.sync_grants(nm, TRUE);
        RAISE NOTICE 'vfts: index % has no changes; boundary moved', nm;
        RETURN;
    END IF;

    -- 4. Build: segments as index rows into a session temp table (emptied by the commit).
    -- Vertica runs several instances per node (PARTITION BEST: an instance per core already); each
    -- gets `threads` threads. Fewer than 200,000 rows: one instance (OVER()), one segment.
    th := CASE WHEN threads_ > 0 THEN threads_ ELSE 1 END;
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

    -- 5. Store the new segments in the index tables.
    PERFORM CALL vfts.store_build(iid, g_new);
    n_segs := (SELECT COUNT(*) FROM vfts_build WHERE kind = 'S');

    -- 6. Tombstones: older versions of every changed doc_id and the documents that left the
    --    source, in the segments that were active.
    n_tomb := 0;
    IF full_ AND g > 0 THEN
        -- a full build replaces every segment
        PERFORM UPDATE vfts.segments SET state = 'dropped', gen_removed = g_new
                WHERE index_id = iid AND gen_removed IS NULL AND gen_added < g_new;
    ELSIF NOT full_ THEN
        changed := 'SELECT doc_id FROM vfts_build WHERE kind = ''D'''
                || CASE WHEN jsel IS NULL THEN '' ELSE ' UNION ' || jsel END
                || CASE WHEN n_gone > 0 THEN ' UNION SELECT doc_id FROM vfts_recon WHERE kind = ''G''' ELSE '' END;
        n_tomb := EXECUTE 'INSERT /*+DIRECT*/ INTO vfts.tombstones SELECT d.index_id, d.segment_id, d.docno, d.doc_id, ' || g_new
             || ' FROM vfts.doc_stats d WHERE d.index_id = ' || iid
             || ' AND d.segment_id IN (SELECT segment_id FROM vfts.segments WHERE index_id = ' || iid || ' AND gen_removed IS NULL AND gen_added < ' || g_new || ')'
             || ' AND d.doc_id IN (' || changed || ')'
             || ' AND (d.segment_id, d.docno) NOT IN (SELECT segment_id, docno FROM vfts.tombstones WHERE index_id = ' || iid || ')';
    END IF;

    -- 7. Every node: the new segment files, then the new generation (with the access mode the
    --    table's policies give now).
    IF n_segs > 0 THEN
        PERFORM CALL vfts.load_build(nm, cdir);
    END IF;
    PERFORM CALL vfts.detect_access(nm);
    PERFORM CALL vfts.activate(nm, g_new);

    -- 8. Statistics and the manifest, in the same transaction as the rows.
    n_dead := (SELECT COUNT(*) FROM vfts.tombstones t WHERE t.index_id = iid
               AND t.segment_id IN (SELECT segment_id FROM vfts.segments WHERE index_id = iid AND gen_removed IS NULL));
    PERFORM INSERT INTO vfts.index_stats
            SELECT iid, g_new, COALESCE(SUM(n_docs), 0) - n_dead, COALESCE(SUM(sum_len), 0), NULL, COUNT(*),
                   COALESCE(SUM(n_terms), 0), COALESCE(SUM(bytes), 0), CLOCK_TIMESTAMP()
            FROM vfts.segments WHERE index_id = iid AND gen_removed IS NULL;
    secs := (SELECT DATEDIFF('millisecond', t0, CLOCK_TIMESTAMP()) / 1000.0);
    note := (CASE WHEN full_ THEN 'full build' ELSE 'incremental' END) || ': ' || n_rows || ' rows into ' || n_segs || ' segments, '
         || n_tomb || ' tombstones, ' || n_del || ' journal rows'
         || CASE WHEN n_gone > 0 OR n_missing > 0 THEN ', ' || n_gone || ' documents left the source, ' || n_missing || ' rows were missing' ELSE '' END;
    PERFORM UPDATE vfts.indexes SET gen = g_new, boundary_epoch = e_new, boundary_lit = NULL, refreshed_at = CLOCK_TIMESTAMP(), refresh_seconds = secs,
                   refresh_note = note, refresh_started_at = NULL WHERE index_name = nm;
    PERFORM COMMIT;

    -- 9. Views with the new boundary and generation (DDL: after the commit; until then the old
    --    views show a wider tail, which is still exact).
    PERFORM CALL vfts.make_views(nm);
    PERFORM CALL vfts.sync_grants(nm, TRUE);
    RAISE NOTICE 'vfts: index % generation %: % (% s)', nm, g_new, note, ROUND(secs, 1);
    IF full_ THEN
        PERFORM CALL vfts.purge_segments(iid);
    END IF;
    -- 10. Tiered merge, when the policy finds a tier with merge_factor segments. Not after a full
    --     build (its segments have the same size; vfts.merge(name, 1) makes one segment of them).
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
    segs INT; docs INT; dead INT; nbytes INT; tail INT; nodes INT; ok INT; params VARCHAR(2000); acc VARCHAR(256); e INT;
BEGIN
    SELECT index_id, gen, source_table, view_schema, boundary_lit, refresh_note, cache_dir, access, boundary_epoch INTO iid, g, src, vs, b, note, cdir, acc, e
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
    b := CASE WHEN e IS NOT NULL THEN 'epoch ' || e WHEN b IS NOT NULL THEN b ELSE '(none)' END;
    RAISE NOTICE 'boundary %; live tail % rows (%.%_live); last refresh: %', b, tail, vs, nm, COALESCE(note, '(none)');
    RAISE NOTICE 'access: % (open: no access policy; rows: each user searches the rows that row access policies show; masked:<column>: searches refused)',
                 COALESCE(acc, '(no key: run CALL vfts.check_access)');
    RAISE NOTICE 'attributes: %', COALESCE((SELECT LISTAGG(attr_name || ' ' || attr_type || ' from ' || source_table || '.' || value_col || ' ('
                 || COALESCE(n_rows, 0) || ' values, refreshed ' || COALESCE(TO_CHAR(refreshed_at, 'YYYY-MM-DD HH24:MI:SS'), 'never') || ')' USING PARAMETERS separator='; ')
                 FROM vfts.attributes WHERE index_id = iid), '(none)');
    RAISE NOTICE 'node caches: % of % nodes of this subcluster have generation % (details: SELECT * FROM (SELECT vfts.tinfo(USING PARAMETERS index=''%'') OVER(PARTITION NODES) FROM vfts.probe) i;)',
                 ok, nodes, g, nm;
END;
$$;

CREATE OR REPLACE PROCEDURE vfts.drop_index(nm VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    iid INT; vs VARCHAR(128); cdir VARCHAR(1024); params VARCHAR(2000); n INT; na INT; sl INT;
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
    na := (SELECT COUNT(*) FROM vfts.attributes WHERE index_id = iid AND slot IS NOT NULL);
    WHILE na > 0 LOOP
        sl := (SELECT MIN(slot) FROM vfts.attributes WHERE index_id = iid AND slot IS NOT NULL);
        PERFORM SELECT DROP_PARTITIONS('vfts.attr_values', sl, sl);
        PERFORM UPDATE vfts.attributes SET slot = NULL WHERE index_id = iid AND slot = sl;
        na := na - 1;
    END LOOP;
    PERFORM DELETE FROM vfts.attributes WHERE index_id = iid;
    PERFORM DELETE FROM vfts.segments WHERE index_id = iid;
    PERFORM DELETE FROM vfts.index_stats WHERE index_id = iid;
    PERFORM DELETE FROM vfts.index_keys WHERE index_id = iid;
    PERFORM DELETE FROM vfts.indexes WHERE index_id = iid;
    PERFORM COMMIT;
    RAISE NOTICE 'vfts: index % dropped (caches removed on % nodes of this subcluster)', nm, n;
END;
$$;

-- Changes options of an index (the same names as create_index): grants=source|manual,
-- merge_factor, threads, max_segment_mb, reorder=true|false. An option not given keeps its value.
CREATE OR REPLACE PROCEDURE vfts.alter_index(nm VARCHAR, opts VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    o VARCHAR(4000); iid INT; grt VARCHAR(16); mf VARCHAR(16); th VARCHAR(16); mx VARCHAR(16); reord VARCHAR(16); bad VARCHAR(4000);
BEGIN
    iid := (SELECT MAX(index_id) FROM vfts.indexes WHERE index_name = nm);
    IF iid IS NULL THEN
        RAISE EXCEPTION 'vfts.alter_index: index % does not exist', nm;
    END IF;
    o := ' ' || COALESCE(REPLACE(opts, ',', ' '), '') || ' ';
    bad := REGEXP_REPLACE(o, ' (grants|merge_factor|threads|max_segment_mb|reorder)=[^ ]+', '');
    IF TRIM(bad) <> '' THEN
        RAISE EXCEPTION 'vfts.alter_index: unknown or malformed option(s): % (allowed: grants, merge_factor, threads, max_segment_mb, reorder)', TRIM(bad);
    END IF;
    grt := REGEXP_SUBSTR(o, ' grants=([^ ]+)', 1, 1, '', 1);
    mf := REGEXP_SUBSTR(o, ' merge_factor=([^ ]+)', 1, 1, '', 1);
    th := REGEXP_SUBSTR(o, ' threads=([^ ]+)', 1, 1, '', 1);
    mx := REGEXP_SUBSTR(o, ' max_segment_mb=([^ ]+)', 1, 1, '', 1);
    reord := REGEXP_SUBSTR(o, ' reorder=([^ ]+)', 1, 1, '', 1);
    IF grt IS NOT NULL AND grt NOT IN ('source', 'manual') THEN
        RAISE EXCEPTION 'vfts.alter_index: grants must be source or manual';
    END IF;
    IF (mf IS NOT NULL AND (NOT REGEXP_LIKE(mf, '^[0-9]{1,6}$') OR mf::INT < 2)) OR (th IS NOT NULL AND NOT REGEXP_LIKE(th, '^[0-9]{1,2}$'))
       OR (mx IS NOT NULL AND (NOT REGEXP_LIKE(mx, '^[0-9]{1,7}$') OR mx::INT < 1)) OR (reord IS NOT NULL AND reord NOT IN ('true', 'false')) THEN
        RAISE EXCEPTION 'vfts.alter_index: merge_factor must be at least 2, threads 0 to 99, max_segment_mb at least 1, reorder true or false';
    END IF;
    PERFORM UPDATE vfts.indexes SET grants_opt = COALESCE(grt, grants_opt), merge_factor = COALESCE(mf::INT, merge_factor),
                   threads = COALESCE(th::INT, threads), max_segment_mb = COALESCE(mx::INT, max_segment_mb),
                   reorder = COALESCE((reord = 'true'), reorder) WHERE index_id = iid;
    PERFORM COMMIT;
    IF grt = 'source' THEN
        PERFORM CALL vfts.sync_grants(nm, TRUE);
    END IF;
    RAISE NOTICE 'vfts: index % altered: %', nm, TRIM(o);
END;
$$;

-- For recovery only: indexes every row again, as one new set of segments.
CREATE OR REPLACE PROCEDURE vfts.rebuild(nm VARCHAR) LANGUAGE PLvSQL AS $$
BEGIN
    IF (SELECT COUNT(*) FROM vfts.indexes WHERE index_name = nm) = 0 THEN
        RAISE EXCEPTION 'vfts.rebuild: index % does not exist', nm;
    END IF;
    PERFORM UPDATE vfts.indexes SET boundary_lit = NULL, boundary_epoch = NULL WHERE index_name = nm;
    PERFORM COMMIT;
    PERFORM CALL vfts.refresh_index(nm);
END;
$$;

-- ------------------------------------------------------------------ access

-- A new key (tokens that anyone saw before stop working), activated on the nodes of this
-- subcluster, and new views. The nodes accept the old and the new key while the views change, then
-- the new one only.
CREATE OR REPLACE PROCEDURE vfts.rotate_key(nm VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    iid INT; g INT;
BEGIN
    SELECT index_id, gen INTO iid, g FROM vfts.indexes WHERE index_name = nm;
    PERFORM CALL vfts.new_key(iid);
    PERFORM COMMIT;
    IF g > 0 THEN
        PERFORM CALL vfts.activate(nm, g);
    END IF;
    PERFORM CALL vfts.make_views(nm);
    PERFORM UPDATE vfts.index_keys SET prev_secret = NULL WHERE index_id = iid;
    PERFORM COMMIT;
    IF g > 0 THEN
        PERFORM CALL vfts.activate(nm, g);
    END IF;
END;
$$;

-- The views follow the source table's grants (option grants=source, the default; docs/design.md
-- "Access"): every user and role that has SELECT on the source table - granted on the table, to
-- PUBLIC, inherited from the schema, or its owner - gets SELECT on the views, and nobody else.
-- Vertica then decides as it does for the table itself (roles granted to roles, default roles,
-- SET ROLE). When someone lost SELECT on the views, rotate = TRUE makes a new key, so that a token
-- they saw before stops working. Run by create_index, refresh_index, check_access and alter_index.
CREATE OR REPLACE PROCEDURE vfts.sync_grants(nm VARCHAR, rotate BOOLEAN) LANGUAGE PLvSQL AS $$
DECLARE
    iid INT; src VARCHAR(300); vs VARCHAR(128); opt VARCHAR(8); sch VARCHAR(128); tbl VARCHAR(128); vown VARCHAR(256);
    want VARCHAR(4000); have VARCHAR(4000); who VARCHAR(256); last VARCHAR(256); lost INT; added INT;
BEGIN
    SELECT index_id, source_table, view_schema, grants_opt INTO iid, src, vs, opt FROM vfts.indexes WHERE index_name = nm;
    IF iid IS NULL OR COALESCE(opt, 'source') <> 'source' THEN
        RETURN;
    END IF;
    sch := LOWER(SPLIT_PART(src, '.', 1));
    tbl := LOWER(SPLIT_PART(src, '.', 2));
    vown := (SELECT MAX(owner_name) FROM v_catalog.views WHERE LOWER(table_schema) = LOWER(vs) AND LOWER(table_name) = LOWER(nm || '_live'));
    want := 'SELECT grantee AS who FROM v_catalog.grants WHERE object_type = ''TABLE'' AND LOWER(object_schema) = ' || QUOTE_LITERAL(sch)
         || ' AND LOWER(object_name) = ' || QUOTE_LITERAL(tbl) || ' AND privileges_description ILIKE ''%SELECT%'''
         || ' UNION SELECT principal FROM v_catalog.inherited_privileges WHERE LOWER(object_schema) = ' || QUOTE_LITERAL(sch)
         || ' AND LOWER(object_name) = ' || QUOTE_LITERAL(tbl) || ' AND privileges_description ILIKE ''%SELECT%'''
         || ' UNION SELECT owner_name FROM v_catalog.tables WHERE LOWER(table_schema) = ' || QUOTE_LITERAL(sch) || ' AND LOWER(table_name) = ' || QUOTE_LITERAL(tbl);
    have := 'SELECT grantee AS who FROM v_catalog.grants WHERE object_type = ''VIEW'' AND LOWER(object_schema) = ' || QUOTE_LITERAL(LOWER(vs))
         || ' AND LOWER(object_name) = ' || QUOTE_LITERAL(LOWER(nm) || '_live') || ' AND privileges_description ILIKE ''%SELECT%''';
    added := 0;
    last := '';
    LOOP
        who := EXECUTE 'SELECT MIN(who) FROM (' || want || ') w WHERE who > ' || QUOTE_LITERAL(last) || ' AND who <> ' || QUOTE_LITERAL(COALESCE(vown, ''))
                     || ' AND who NOT IN (' || have || ')';
        EXIT WHEN who IS NULL;
        last := who;
        EXECUTE 'GRANT SELECT ON ' || vs || '.' || nm || '_live, ' || vs || '.' || nm || '_snap TO ' || QUOTE_IDENT(who);
        added := added + 1;
    END LOOP;
    lost := 0;
    last := '';
    LOOP
        who := EXECUTE 'SELECT MIN(who) FROM (' || have || ') h WHERE who > ' || QUOTE_LITERAL(last) || ' AND who <> ' || QUOTE_LITERAL(COALESCE(vown, ''))
                     || ' AND who NOT IN (' || want || ')';
        EXIT WHEN who IS NULL;
        last := who;
        EXECUTE 'REVOKE SELECT ON ' || vs || '.' || nm || '_live, ' || vs || '.' || nm || '_snap FROM ' || QUOTE_IDENT(who);
        lost := lost + 1;
    END LOOP;
    IF lost > 0 AND rotate THEN
        PERFORM CALL vfts.rotate_key(nm);
    END IF;
    IF added > 0 OR lost > 0 THEN
        RAISE NOTICE 'vfts: index % views granted to % and revoked from % users or roles (they follow the SELECT grants of %)', nm, added, lost, src;
    END IF;
END;
$$;

-- Run after creating, changing, enabling or dropping an access policy on the source table, and
-- after grants on it changed: re-reads the policies (access mode), makes a new key (tokens that
-- anyone saw before stop working), activates it on the nodes of this subcluster, makes new views
-- and makes their grants follow the source table's again. refresh_index and merge re-read the
-- policies too; refresh_index also follows the grants.
CREATE OR REPLACE PROCEDURE vfts.check_access(nm VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    iid INT;
BEGIN
    iid := (SELECT MAX(index_id) FROM vfts.indexes WHERE index_name = nm);
    IF iid IS NULL THEN
        RAISE EXCEPTION 'vfts.check_access: index % does not exist', nm;
    END IF;
    PERFORM CALL vfts.detect_access(nm);
    PERFORM CALL vfts.rotate_key(nm);
    PERFORM CALL vfts.sync_grants(nm, FALSE);
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

-- ------------------------------------------------------------------ attributes (docs/design.md "Attributes")

-- Writes the attribute's current copy (vfts.attr_values, its slot) into the cache of every node of
-- the session's subcluster, as load_rows does for segments: each node joins its own replica with
-- the one row tnode returns there. One row with NULLs per node (from a replicated table) makes
-- every node run tattr, also when the attribute has no rows.
CREATE OR REPLACE PROCEDURE vfts.load_attribute(nm VARCHAR, attr VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    iid INT; cdir VARCHAR(1024); typ VARCHAR(8); sl INT; ver INT; want INT; got INT; w INT;
    params VARCHAR(2000); val VARCHAR(200); nul VARCHAR(100);
BEGIN
    SELECT i.index_id, i.cache_dir, a.attr_type, a.slot, a.version INTO iid, cdir, typ, sl, ver
      FROM vfts.attributes a JOIN vfts.indexes i ON i.index_id = a.index_id WHERE i.index_name = nm AND a.attr_name = attr;
    IF iid IS NULL OR sl IS NULL THEN
        RAISE EXCEPTION 'vfts: index % has no attribute % (CALL vfts.add_attribute)', nm, attr;
    END IF;
    IF typ = 'text' THEN
        -- the declared width of a UDx input is what every row costs: the longest value, not 1024
        w := (SELECT GREATEST(1, COALESCE(MAX(OCTET_LENGTH(sval)), 1)) FROM vfts.attr_values WHERE slot = sl);
        val := 'sval::VARCHAR(' || w || ')';
        nul := 'NULL::VARCHAR(' || w || ')';
    ELSE
        val := 'ival';
        nul := 'NULL::INT';
    END IF;
    want := (SELECT COUNT(*) FROM (SELECT vfts.tnode(k) OVER(PARTITION NODES) FROM vfts.probe) n);
    params := 'index=' || QUOTE_LITERAL(nm) || ', name=' || QUOTE_LITERAL(attr) || ', type=' || QUOTE_LITERAL(typ) || ', version=' || ver
           || CASE WHEN cdir IS NULL THEN '' ELSE ', cache_dir=' || QUOTE_LITERAL(cdir) END;
    got := EXECUTE 'SELECT /*+LABEL(vfts_attr)*/ COUNT(DISTINCT node_name) FROM (SELECT vfts.tattr(r.doc_id, r.v USING PARAMETERS ' || params
        || ') OVER(PARTITION NODES) FROM (SELECT /*+SYNTACTIC_JOIN*/ s.* FROM (SELECT doc_id, ' || val || ' AS v FROM vfts.attr_values WHERE slot = ' || sl
        || ' UNION ALL SELECT NULL::INT, ' || nul || ' FROM vfts.indexes WHERE index_id = ' || iid
        || ') s CROSS JOIN /*+DISTRIB(L,L)*/ (SELECT vfts.tnode(k) OVER(PARTITION NODES) FROM vfts.probe) p) r) l WHERE status = ''loaded''';
    IF got <> want THEN
        RAISE EXCEPTION 'vfts: attribute % of index % was loaded on % of % nodes', attr, nm, got, want;
    END IF;
END;
$$;

-- Copies the attribute's column again (a new slot), loads it on the nodes, then drops the old copy.
CREATE OR REPLACE PROCEDURE vfts.refresh_attribute(nm VARCHAR, attr VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    iid INT; src VARCHAR(300); kc VARCHAR(128); vc VARCHAR(128); typ VARCHAR(8); prev_sl INT; sl INT; n INT; d INT; np INT; w INT;
    t0 TIMESTAMPTZ; expr VARCHAR(600); secs FLOAT;
BEGIN
    SELECT a.index_id, a.source_table, a.key_col, a.value_col, a.attr_type, a.slot INTO iid, src, kc, vc, typ, prev_sl
      FROM vfts.attributes a JOIN vfts.indexes i ON i.index_id = a.index_id WHERE i.index_name = nm AND a.attr_name = attr;
    IF iid IS NULL THEN
        RAISE EXCEPTION 'vfts: index % has no attribute % (CALL vfts.add_attribute)', nm, attr;
    END IF;
    t0 := (SELECT CLOCK_TIMESTAMP());
    -- a policy added after add_attribute: the copy would show values the policy hides (only a
    -- superuser and the table's owner see policies)
    IF (SELECT COUNT(*) FROM v_catalog.users WHERE user_name = CURRENT_USER() AND is_super_user) = 0
       AND COALESCE((SELECT MAX(owner_name) FROM v_catalog.tables WHERE LOWER(table_schema || '.' || table_name) = LOWER(src)), '') <> CURRENT_USER() THEN
        RAISE EXCEPTION 'vfts: only a superuser or the owner of % sees its access policies: refresh attributes as one of them', src;
    END IF;
    np := (SELECT COUNT(*) FROM v_catalog.access_policy WHERE LOWER(table_name) = LOWER(src)
           AND (is_policy_enabled::VARCHAR ILIKE 'enabled' OR is_policy_enabled::VARCHAR ILIKE 't%'));
    IF np > 0 THEN
        RAISE EXCEPTION 'vfts: % has access policies: an attribute copy would show values they hide, so vfts refuses it (README: Attributes)', src;
    END IF;
    IF typ = 'text' THEN
        w := EXECUTE 'SELECT COALESCE(MAX(OCTET_LENGTH(' || vc || ')), 0) FROM ' || src;
        IF w > 1024 THEN
            RAISE EXCEPTION 'vfts: attribute values are at most 1024 bytes; %.% has one of % bytes', src, vc, w;
        END IF;
    END IF;
    expr := CASE typ WHEN 'int' THEN vc || '::INT'
                     WHEN 'bool' THEN 'CASE WHEN ' || vc || ' THEN 1 ELSE 0 END'
                     WHEN 'date' THEN '(' || vc || ' - ''2000-01-01''::DATE)'
                     ELSE vc || '::VARCHAR(1024)' END;
    sl := (SELECT NEXTVAL('vfts.attr_seq'));
    EXECUTE 'INSERT INTO vfts.attr_values (slot, doc_id, ' || CASE WHEN typ = 'text' THEN 'sval' ELSE 'ival' END || ') SELECT '
         || sl || ', ' || kc || ', ' || expr || ' FROM ' || src || ' WHERE ' || kc || ' IS NOT NULL AND ' || vc || ' IS NOT NULL';
    PERFORM COMMIT;
    SELECT COUNT(*), COUNT(DISTINCT doc_id) INTO n, d FROM vfts.attr_values WHERE slot = sl;
    IF n <> d THEN
        PERFORM SELECT DROP_PARTITIONS('vfts.attr_values', sl, sl);
        RAISE EXCEPTION 'vfts: %.% is not unique (% rows share a value): an attribute needs one value per document', src, kc, n - d;
    END IF;
    PERFORM UPDATE vfts.attributes SET slot = sl, version = version + 1, n_rows = n WHERE index_id = iid AND attr_name = attr;
    PERFORM COMMIT;
    PERFORM CALL vfts.load_attribute(nm, attr);
    IF prev_sl IS NOT NULL THEN
        PERFORM SELECT DROP_PARTITIONS('vfts.attr_values', prev_sl, prev_sl);
    END IF;
    secs := (SELECT DATEDIFF('millisecond', t0, CLOCK_TIMESTAMP()) / 1000.0);
    PERFORM UPDATE vfts.attributes SET refreshed_at = CLOCK_TIMESTAMP(), refresh_seconds = secs WHERE index_id = iid AND attr_name = attr;
    PERFORM COMMIT;
    RAISE NOTICE 'vfts: attribute % of index % refreshed: % values in % s', attr, nm, n, ROUND(secs, 3);
END;
$$;

-- CALL vfts.add_attribute('emails', 'custodian', 'app.email_meta', 'doc_id', 'custodian')
-- A column of a table with one row per document (key_col: the index's doc_id) becomes attribute
-- <attr> of the index: searches filter and count by it while they match (filter='custodian IN (3,
-- 17)', tcount by='custodian'). INT, BOOLEAN, DATE or VARCHAR (at most 1024 bytes). Copied now;
-- after the table changes, CALL vfts.refresh_attributes('<index>'). Tables with access policies
-- are refused.
CREATE OR REPLACE PROCEDURE vfts.add_attribute(nm VARCHAR, attr VARCHAR, src VARCHAR, kc VARCHAR, vc VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    ident VARCHAR(64) := '^[A-Za-z_][A-Za-z0-9_]*$';
    qual VARCHAR(100) := '^[A-Za-z_][A-Za-z0-9_]*\.[A-Za-z_][A-Za-z0-9_]*$';
    iid INT; sch VARCHAR(128); tbl VARCHAR(128); t VARCHAR(128); typ VARCHAR(8); sup INT; own VARCHAR(256); np INT;
BEGIN
    iid := (SELECT MAX(index_id) FROM vfts.indexes WHERE index_name = nm);
    IF iid IS NULL THEN
        RAISE EXCEPTION 'vfts.add_attribute: index % does not exist', nm;
    END IF;
    IF attr IS NULL OR NOT REGEXP_LIKE(attr, '^[A-Za-z0-9_]{1,64}$') THEN
        RAISE EXCEPTION 'vfts.add_attribute: attribute name must be 1 to 64 letters, digits or underscores';
    END IF;
    IF (SELECT COUNT(*) FROM vfts.attributes WHERE index_id = iid AND attr_name = attr) > 0 THEN
        RAISE EXCEPTION 'vfts.add_attribute: index % has an attribute % already (vfts.refresh_attributes or vfts.drop_attribute)', nm, attr;
    END IF;
    IF src IS NULL OR NOT REGEXP_LIKE(src, qual) THEN
        RAISE EXCEPTION 'vfts.add_attribute: the table must be given as schema.table';
    END IF;
    IF kc IS NULL OR vc IS NULL OR NOT REGEXP_LIKE(kc, ident) OR NOT REGEXP_LIKE(vc, ident) THEN
        RAISE EXCEPTION 'vfts.add_attribute: column names must be plain identifiers';
    END IF;
    sch := SPLIT_PART(src, '.', 1);
    tbl := SPLIT_PART(src, '.', 2);
    t := (SELECT MAX(data_type) FROM v_catalog.columns WHERE table_schema ILIKE sch AND table_name ILIKE tbl AND column_name ILIKE kc);
    IF t IS NULL THEN
        RAISE EXCEPTION 'vfts.add_attribute: table % or its column % does not exist', src, kc;
    END IF;
    IF NOT t ILIKE 'int%' THEN
        RAISE EXCEPTION 'vfts.add_attribute: key column % must be INT (the index''s doc_id), not %', kc, t;
    END IF;
    t := (SELECT MAX(data_type) FROM v_catalog.columns WHERE table_schema ILIKE sch AND table_name ILIKE tbl AND column_name ILIKE vc);
    typ := CASE WHEN t ILIKE 'int%' THEN 'int' WHEN t ILIKE 'boolean' THEN 'bool' WHEN t ILIKE 'date' THEN 'date'
                WHEN (t ILIKE 'varchar%' OR t ILIKE 'char%') THEN 'text' END;
    IF typ IS NULL THEN
        RAISE EXCEPTION 'vfts.add_attribute: column % must exist and be INT, BOOLEAN, DATE or VARCHAR (found %)', vc, COALESCE(t, 'nothing');
    END IF;
    -- access policies are visible to a superuser and to the table's owner only
    sup := (SELECT COUNT(*) FROM v_catalog.users WHERE user_name = CURRENT_USER() AND is_super_user);
    own := (SELECT MAX(owner_name) FROM v_catalog.tables WHERE LOWER(table_schema || '.' || table_name) = LOWER(src));
    IF sup = 0 AND COALESCE(own, '') <> CURRENT_USER() THEN
        RAISE EXCEPTION 'vfts.add_attribute: only a superuser or the owner of % sees its access policies: run this as one of them', src;
    END IF;
    np := (SELECT COUNT(*) FROM v_catalog.access_policy WHERE LOWER(table_name) = LOWER(src)
           AND (is_policy_enabled::VARCHAR ILIKE 'enabled' OR is_policy_enabled::VARCHAR ILIKE 't%'));
    IF np > 0 THEN
        RAISE EXCEPTION 'vfts.add_attribute: % has access policies: an attribute copy would show values they hide, so vfts refuses it (README: Attributes)', src;
    END IF;
    -- checked before anything is written (refresh_attribute checks again)
    np := EXECUTE 'SELECT COUNT(*) - COUNT(DISTINCT ' || kc || ') FROM ' || src || ' WHERE ' || kc || ' IS NOT NULL AND ' || vc || ' IS NOT NULL';
    IF np > 0 THEN
        RAISE EXCEPTION 'vfts.add_attribute: %.% is not unique (% rows share a value): an attribute needs one value per document', src, kc, np;
    END IF;
    IF typ = 'text' THEN
        np := EXECUTE 'SELECT COALESCE(MAX(OCTET_LENGTH(' || vc || ')), 0) FROM ' || src;
        IF np > 1024 THEN
            RAISE EXCEPTION 'vfts.add_attribute: attribute values are at most 1024 bytes; %.% has one of % bytes', src, vc, np;
        END IF;
    END IF;
    PERFORM INSERT INTO vfts.attributes (index_id, attr_name, source_table, key_col, value_col, attr_type, version)
            VALUES (iid, attr, src, kc, vc, typ, 0);
    PERFORM COMMIT;
    PERFORM CALL vfts.refresh_attribute(nm, attr);
END;
$$;

-- Copies every attribute of an index again (after the tables changed).
CREATE OR REPLACE PROCEDURE vfts.refresh_attributes(nm VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    iid INT; n INT; i INT; a VARCHAR(64);
BEGIN
    iid := (SELECT MAX(index_id) FROM vfts.indexes WHERE index_name = nm);
    IF iid IS NULL THEN
        RAISE EXCEPTION 'vfts.refresh_attributes: index % does not exist', nm;
    END IF;
    n := (SELECT COUNT(*) FROM vfts.attributes WHERE index_id = iid);
    i := 0;
    WHILE i < n LOOP
        a := (SELECT MAX(attr_name) FROM (SELECT attr_name, ROW_NUMBER() OVER(ORDER BY attr_name) AS rn FROM vfts.attributes WHERE index_id = iid) x
              WHERE rn = i + 1);
        PERFORM CALL vfts.refresh_attribute(nm, a);
        i := i + 1;
    END LOOP;
    IF n = 0 THEN
        RAISE NOTICE 'vfts: index % has no attributes', nm;
    END IF;
END;
$$;

-- Removes an attribute: its node files, its copy and its row.
CREATE OR REPLACE PROCEDURE vfts.drop_attribute(nm VARCHAR, attr VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    iid INT; cdir VARCHAR(1024); sl INT; n INT; params VARCHAR(2000);
BEGIN
    SELECT i.index_id, i.cache_dir, a.slot INTO iid, cdir, sl
      FROM vfts.attributes a JOIN vfts.indexes i ON i.index_id = a.index_id WHERE i.index_name = nm AND a.attr_name = attr;
    IF iid IS NULL THEN
        RAISE EXCEPTION 'vfts.drop_attribute: index % has no attribute %', nm, attr;
    END IF;
    params := 'index=' || QUOTE_LITERAL(nm) || ', name=' || QUOTE_LITERAL(attr) || ', remove=true'
           || CASE WHEN cdir IS NULL THEN '' ELSE ', cache_dir=' || QUOTE_LITERAL(cdir) END;
    n := EXECUTE 'SELECT COUNT(*) FROM (SELECT vfts.tattr(k, k USING PARAMETERS ' || params || ') OVER(PARTITION NODES) FROM vfts.probe) d';
    IF sl IS NOT NULL THEN
        PERFORM SELECT DROP_PARTITIONS('vfts.attr_values', sl, sl);
    END IF;
    PERFORM DELETE FROM vfts.attributes WHERE index_id = iid AND attr_name = attr;
    PERFORM COMMIT;
    RAISE NOTICE 'vfts: attribute % of index % dropped (files removed on % nodes of this subcluster)', attr, nm, n;
END;
$$;
