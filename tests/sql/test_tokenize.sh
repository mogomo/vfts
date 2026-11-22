#!/usr/bin/env bash
# vfts.tokenize and tversion: the analyzer inside Vertica (milestone M1).
set -uo pipefail
cd "$(dirname "$0")"; source ./lib.sh
echo "$TEST_NAME"

expect "tversion" "$(q "SELECT version FROM (SELECT vfts.tversion() OVER()) v")" "0.3.1"
expect "snowball list has arabic and yiddish" \
       "$(q "SELECT (snowball LIKE '%arabic%' AND snowball LIKE '%yiddish%')::INT FROM (SELECT vfts.tversion() OVER()) v")" "1"

# Sherlock Holmes punctuation cases (the built-in text index missed them)
expect "Holmes' Holmes, Holmes-style" \
       "$(q "SELECT LISTAGG(term USING PARAMETERS separator=' ') FROM (SELECT vfts.tokenize('Holmes'' Holmes, Holmes-style') OVER()) t")" \
       "holmes holmes holmes style"
expect "positions and byte offsets" \
       "$(q "SELECT pos || ':' || term || ':' || start_byte || '-' || end_byte FROM (SELECT vfts.tokenize('Café, au lait') OVER()) t ORDER BY pos")" \
       "$(printf '0:cafe:0-5\n1:au:7-9\n2:lait:10-14')"
expect "stem column" "$(q "SELECT stem FROM (SELECT vfts.tokenize('running') OVER()) t")" "run"

# Hebrew: acronym, niqqud, prefixes, mixed with English
expect "hebrew acronym gershayim" \
       "$(q "SELECT LISTAGG(term USING PARAMETERS separator=' ') FROM (SELECT vfts.tokenize('צה״ל צה\"ל') OVER()) t")" \
       'צה"ל צה"ל'
expect "hebrew geresh as quotation mark, offsets of the text" \
       "$(q "SELECT LISTAGG(term || ':' || start_byte || '-' || end_byte USING PARAMETERS separator=' ') FROM (SELECT vfts.tokenize('׳תיאטרון מסך׳') OVER()) t")" \
       "תיאטרון:2-16 מסך':17-25"
expect "hebrew niqqud and prefix stem" \
       "$(q "SELECT term || '/' || stem || '/' || lang FROM (SELECT vfts.tokenize('הַבַּיִת' USING PARAMETERS lang='he') OVER()) t")" \
       "הבית/בית/he"
expect "english word in hebrew text" \
       "$(q "SELECT LISTAGG(lang USING PARAMETERS separator=' ') FROM (SELECT vfts.tokenize('חוזה contracts' USING PARAMETERS lang='he') OVER()) t")" \
       "he en"

# Arabic normalization
expect "arabic tashkeel, alef, teh marbuta" \
       "$(q "SELECT LISTAGG(term USING PARAMETERS separator=' ') FROM (SELECT vfts.tokenize('مُحَمَّد أحمد مدرسة') OVER()) t")" \
       "محمد احمد مدرسه"

# The language per row, and as an exploder beside other columns
q "DROP TABLE IF EXISTS public.vfts_tok_test; CREATE TABLE public.vfts_tok_test (id INT, body LONG VARCHAR, lang VARCHAR(8));
   INSERT INTO public.vfts_tok_test VALUES (1, 'Les contrats', 'fr');
   INSERT INTO public.vfts_tok_test VALUES (2, 'The contracts', 'en'); COMMIT;" > /dev/null
expect "exploder with lang column" \
       "$(q "SELECT id, term, stem FROM (SELECT id, vfts.tokenize(body, lang) FROM public.vfts_tok_test) t WHERE pos = 1 ORDER BY id")" \
       "$(printf '1|contrats|contrat\n2|contracts|contract')"
q "DROP TABLE public.vfts_tok_test" > /dev/null

expect_error "unsupported script" "SELECT vfts.tokenize('北京欢迎你') OVER()" "not supported"
expect "unsupported script, plain" \
       "$(q "SELECT COUNT(*) > 0 FROM (SELECT vfts.tokenize('北京欢迎你' USING PARAMETERS on_unsupported='plain') OVER()) t")" "t"
expect_error "unknown language" "SELECT vfts.tokenize('x' USING PARAMETERS lang='xx') OVER()" "unknown language code"

done_test
