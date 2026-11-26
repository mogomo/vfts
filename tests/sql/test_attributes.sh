#!/usr/bin/env bash
# Attributes (vfts.add_attribute): every filter must keep exactly the matches that SQL keeps with
# the same predicate in a WHERE clause (LEFT JOIN of the attribute table: a document without a row
# has NULL values), with the same scores; tcount by= must equal SQL's GROUP BY; a refresh shows
# changed values; refused tables and bad filters give clear errors.
set -uo pipefail
cd "$(dirname "$0")"; source ./lib.sh
echo "$TEST_NAME"

q "CALL vfts.drop_index('vf_attr')" > /dev/null 2>&1
q "DROP SCHEMA IF EXISTS vf_attr CASCADE" > /dev/null
q "CREATE SCHEMA vf_attr;
   CREATE TABLE vf_attr.docs (doc_id INT NOT NULL, body LONG VARCHAR, ver TIMESTAMPTZ DEFAULT CLOCK_TIMESTAMP());" > /dev/null
# 3072 documents of 3 to 20 words from a vocabulary of 12
q "INSERT INTO vf_attr.docs (doc_id, body)
   SELECT doc_id, LISTAGG(w USING PARAMETERS separator=' ', max_length=4000)
   FROM (SELECT d.doc_id, SPLIT_PART('alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima',
                                     ' ', (1 + MOD(HASH(d.doc_id, p.k) // 7, 12))::INT) AS w
         FROM (SELECT a.k + 1024 * b.j AS doc_id FROM vfts.probe a CROSS JOIN (SELECT 0 AS j UNION ALL SELECT 1 UNION ALL SELECT 2) b) d
         JOIN vfts.probe p ON p.k <= 3 + MOD(HASH(d.doc_id) // 11, 18)) x
   GROUP BY doc_id;
   COMMIT;" > /dev/null
expect "corpus" "$(q "SELECT COUNT(*) FROM vf_attr.docs")" "3072"
# attributes in another table: no row for every 17th document; custodian NULL for every 23rd
q "CREATE TABLE vf_attr.meta (doc_id INT NOT NULL, custodian INT, status VARCHAR(20), sent DATE, flag BOOLEAN);
   INSERT INTO vf_attr.meta SELECT doc_id, CASE WHEN doc_id % 23 = 0 THEN NULL ELSE doc_id % 7 END,
          CASE doc_id % 3 WHEN 0 THEN 'closed' WHEN 1 THEN 'open' ELSE 'on hold' END,
          '2001-05-15'::DATE + (doc_id % 40)::INT, doc_id % 2 = 0
   FROM vf_attr.docs WHERE doc_id % 17 <> 0;
   COMMIT;" > /dev/null
q "CALL vfts.create_index('vf_attr', 'vf_attr.docs', 'refresh_every=0 margin=0')" > /dev/null
for a in custodian status sent flag; do
    q "CALL vfts.add_attribute('vf_attr', '$a', 'vf_attr.meta', 'doc_id', '$a')" > /dev/null
done
expect "attributes" "$(q "SELECT attr_name || ':' || attr_type || ':' || n_rows FROM vfts.attributes a JOIN vfts.indexes i USING (index_id)
                          WHERE i.index_name = 'vf_attr' ORDER BY 1" | tr '\n' ' ')" \
       "custodian:int:$(q "SELECT COUNT(custodian) FROM vf_attr.meta") flag:bool:$(q "SELECT COUNT(*) FROM vf_attr.meta") sent:date:$(q "SELECT COUNT(*) FROM vf_attr.meta") status:text:$(q "SELECT COUNT(*) FROM vf_attr.meta") "

lit() { echo "${1//\'/\'\'}"; }
V="vf_attr.vf_attr_live"
TS() { echo "SELECT doc_id, score FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_attr', query='$1', k=$2${3:+, filter='$(lit "$3")'}) OVER() FROM $V) r"; }
REF() { echo "SELECT r.doc_id, r.score FROM ($(TS "$1" 1000000)) r LEFT JOIN vf_attr.meta m USING (doc_id) WHERE $2"; }

check_filter() {   # query, filter (the same text as a SQL predicate)
    local d top n c m
    d=$(q "SELECT COUNT(*) FROM (($(TS "$1" 1000000 "$2")) EXCEPT ($(REF "$1" "$2"))) a")$(q "SELECT COUNT(*) FROM (($(REF "$1" "$2")) EXCEPT ($(TS "$1" 1000000 "$2"))) b")
    top=$(q "SELECT COUNT(*) FROM (($(TS "$1" 10 "$2")) EXCEPT (SELECT doc_id, score FROM ($(REF "$1" "$2")) x ORDER BY score DESC, doc_id LIMIT 10)) a")
    n=$(q "SELECT COUNT(*) FROM ($(REF "$1" "$2")) x")
    c=$(q "SELECT count FROM (SELECT vfts.tcount(* USING PARAMETERS index='vf_attr', query='$1', filter='$(lit "$2")') OVER() FROM $V) r")
    m=$(q "SELECT COUNT(*) FROM (SELECT vfts.tmatch(* USING PARAMETERS index='vf_attr', query='$1', filter='$(lit "$2")') OVER() FROM $V) r")
    expect "$1 | $2 ($n docs)" "$d $top $c $m" "00 0 $n $n"
}

check_filter "kilo"                   "custodian IN (1, 2)"
check_filter "kilo OR lima"           "custodian = 3"
check_filter "kilo AND lima"          "custodian <> 3"
check_filter "alpha"                  "custodian BETWEEN 2 AND 4"
check_filter "alpha bravo"            "custodian NOT IN (1, 5)"
check_filter "golf"                   "custodian IS NULL"
check_filter "golf"                   "NOT (custodian >= 5 AND status = 'open')"
check_filter "hotel w/3 india"        "status IN ('closed', 'on hold')"
check_filter "\"alpha bravo\""        "status > 'on'"
check_filter "juliet"                 "sent >= '2001-06-01' AND custodian > 0"
check_filter "juliet OR kilo"         "sent < DATE '2001-05-20' OR flag = TRUE"
check_filter "NOT alpha"              "flag = FALSE"
check_filter "kilo"                   "status IS NOT NULL AND NOT flag = TRUE"

# a filter does not change scores
expect "scores unchanged" \
  "$(q "SELECT COUNT(*) FROM ($(TS kilo 10 "custodian = 2")) f JOIN ($(TS kilo 1000000)) a USING (doc_id) WHERE f.score <> a.score")" "0"
# without the filter cache: the same answer
expect "filter_cache=false" \
  "$(q "SELECT COUNT(*) FROM (SELECT vfts.tcount(* USING PARAMETERS index='vf_attr', query='kilo', filter='custodian IN (1, 2)', filter_cache=false) OVER() FROM $V) r")" "1"

# counting by an attribute = GROUP BY over every match
check_by() {   # query, attribute, [filter]
    local got want
    got=$(q "SELECT value || '=' || count FROM (SELECT vfts.tcount(* USING PARAMETERS index='vf_attr', query='$1', by='$2'${3:+, filter='$(lit "$3")'}) OVER() FROM $V) r ORDER BY 1" | tr '\n' ' ')
    want=$(q "SELECT v || '=' || n FROM (SELECT m.$2::VARCHAR AS v, COUNT(*) AS n FROM ($(TS "$1" 1000000)) r JOIN vf_attr.meta m USING (doc_id)
              WHERE m.$2 IS NOT NULL ${3:+AND ($3)} GROUP BY 1) x ORDER BY 1" | tr '\n' ' ')
    expect "by $2: $1 ${3:-}" "$got" "$want"
}
check_by "kilo" custodian
check_by "kilo OR lima" status
check_by "alpha" sent
check_by "bravo" flag
check_by "kilo" custodian "status = 'open'"
expect "by: qid of a batch" "$(q "SELECT COUNT(DISTINCT qid) FROM (SELECT vfts.tcount(* USING PARAMETERS index='vf_attr', queries=E'1\tkilo\n2\tlima', by='status') OVER() FROM $V) r")" "2"

# the copy changes only with a refresh
old_slots=$(q "SELECT LISTAGG(slot) FROM vfts.attributes a JOIN vfts.indexes i USING (index_id) WHERE i.index_name = 'vf_attr'")
q "UPDATE vf_attr.meta SET custodian = 6 WHERE custodian = 1; COMMIT;" > /dev/null
before=$(q "SELECT count FROM (SELECT vfts.tcount(* USING PARAMETERS index='vf_attr', query='NOT zzz', filter='custodian = 1') OVER() FROM $V) r")
q "CALL vfts.refresh_attributes('vf_attr')" > /dev/null
after=$(q "SELECT count FROM (SELECT vfts.tcount(* USING PARAMETERS index='vf_attr', query='NOT zzz', filter='custodian = 1') OVER() FROM $V) r")
expect "refresh shows the update" "$([ "$before" -gt 0 ] && echo yes) $after" "yes 0"
expect "version after refresh" "$(q "SELECT version FROM vfts.attributes a JOIN vfts.indexes i USING (index_id) WHERE i.index_name = 'vf_attr' AND attr_name = 'custodian'")" "2"
expect "old copies dropped" "$(q "SELECT COUNT(*) FROM vfts.attr_values WHERE slot IN ($old_slots)")" "0"

# the live tail: a document added after the build is filtered by its attribute too
q "INSERT INTO vf_attr.docs (doc_id, body) VALUES (5000, 'zulu kilo'); INSERT INTO vf_attr.meta VALUES (5000, 4, 'open', '2001-06-01', true); COMMIT;" > /dev/null
q "CALL vfts.refresh_attributes('vf_attr')" > /dev/null
expect "live tail filtered" "$(q "SELECT doc_id FROM ($(TS zulu 10 "custodian = 4")) r") / $(q "SELECT COUNT(*) FROM ($(TS zulu 10 "custodian = 3")) r")" "5000 / 0"

# errors
expect_error "unknown attribute"   "$(TS kilo 10 "owner = 1")" "no attribute owner"
expect_error "type mismatch"       "$(TS kilo 10 "custodian = 'x'")" "is an integer"
expect_error "syntax"              "$(TS kilo 10 "custodian IN (1, 2")" "',' or ')' expected"
expect_error "by on tsearch"       "SELECT * FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_attr', query='kilo', by='status') OVER() FROM $V) r" "by"
expect_error "by unknown"          "SELECT * FROM (SELECT vfts.tcount(* USING PARAMETERS index='vf_attr', query='kilo', by='nothing') OVER() FROM $V) r" "no attribute nothing"
expect_error "exists already"      "CALL vfts.add_attribute('vf_attr', 'status', 'vf_attr.meta', 'doc_id', 'status')" "has an attribute status already"
q "CREATE TABLE vf_attr.dup (doc_id INT, v INT, f FLOAT); INSERT INTO vf_attr.dup VALUES (1, 1, 0.5); INSERT INTO vf_attr.dup VALUES (1, 2, 0.5); COMMIT;" > /dev/null
expect_error "key not unique"      "CALL vfts.add_attribute('vf_attr', 'dupv', 'vf_attr.dup', 'doc_id', 'v')" "is not unique"
expect_error "type FLOAT"          "CALL vfts.add_attribute('vf_attr', 'f', 'vf_attr.dup', 'doc_id', 'f')" "must exist and be INT, BOOLEAN, DATE or VARCHAR"
expect_error "bad name"            "CALL vfts.add_attribute('vf_attr', 'a b', 'vf_attr.meta', 'doc_id', 'status')" "1 to 64 letters"
expect_error "injection"           "CALL vfts.add_attribute('vf_attr', 'x', 'vf_attr.meta', 'doc_id', 'status; DROP TABLE x')" "plain identifiers"
q "CREATE TABLE vf_attr.secret (doc_id INT, v INT); INSERT INTO vf_attr.secret VALUES (1, 1); COMMIT;
   CREATE ACCESS POLICY ON vf_attr.secret FOR ROWS WHERE v > 0 ENABLE;" > /dev/null
expect_error "row policy refused"  "CALL vfts.add_attribute('vf_attr', 'sv', 'vf_attr.secret', 'doc_id', 'v')" "has access policies"
expect "refused leaves nothing" "$(q "SELECT COUNT(*) FROM vfts.attributes a JOIN vfts.indexes i USING (index_id) WHERE i.index_name = 'vf_attr' AND attr_name IN ('sv', 'dupv', 'f')")" "0"

# info lists them; load_all writes them again; drop_attribute removes one
expect "info" "$(vsql -X -q -c "CALL vfts.info('vf_attr')" 2>&1 | grep -c 'custodian int from vf_attr.meta.custodian')" "1"
q "CALL vfts.load_all('vf_attr')" > /dev/null
expect "after load_all" "$(q "SELECT count FROM (SELECT vfts.tcount(* USING PARAMETERS index='vf_attr', query='kilo', filter='custodian = 6') OVER() FROM $V) r")" \
       "$(q "SELECT COUNT(*) FROM ($(TS kilo 1000000)) r JOIN vf_attr.meta m USING (doc_id) WHERE m.custodian = 6")"
q "CALL vfts.drop_attribute('vf_attr', 'flag')" > /dev/null
expect_error "dropped attribute"   "$(TS kilo 10 "flag = TRUE")" "no attribute flag"
expect "rows left" "$(q "SELECT COUNT(*) FROM vfts.attributes a JOIN vfts.indexes i USING (index_id) WHERE i.index_name = 'vf_attr'")" "3"

slots=$(q "SELECT LISTAGG(slot) FROM vfts.attributes a JOIN vfts.indexes i USING (index_id) WHERE i.index_name = 'vf_attr'")
q "CALL vfts.drop_index('vf_attr')" > /dev/null
expect "drop_index removes attributes" "$(q "SELECT COUNT(*) FROM vfts.attr_values WHERE slot IN ($slots)")" "0"
q "DROP SCHEMA vf_attr CASCADE" > /dev/null
done_test
