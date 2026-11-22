#!/usr/bin/env bash
# Languages through Vertica (correctness rules 6 and 7): mode='stem' and mode='exact', a test set per
# language group, hand-written Arabic and Hebrew cases, and the Sherlock Holmes punctuation cases.
set -uo pipefail
cd "$(dirname "$0")"; source ./lib.sh
echo "$TEST_NAME"

for i in vf_lang vf_plain vf_strict; do q "CALL vfts.drop_index('$i')" > /dev/null 2>&1; done
q "DROP SCHEMA IF EXISTS vf_lang CASCADE" > /dev/null
q "CREATE SCHEMA vf_lang; CREATE TABLE vf_lang.docs (doc_id INT NOT NULL, body LONG VARCHAR, lang VARCHAR(8), ver TIMESTAMPTZ DEFAULT CLOCK_TIMESTAMP())" > /dev/null
ins() { q "INSERT INTO vf_lang.docs (doc_id, body, lang) VALUES ($1, '$2', $3); COMMIT;" > /dev/null; }
# Sherlock Holmes titles with the punctuation the built-in text index did not split
ins 1  "The Adventures of Sherlock Holmes'' Smarter Brother" NULL
ins 2  "Sherlock Holmes, the Musical" NULL
ins 3  "A Holmes-style mystery" NULL
ins 4  "(Holmes) and Watson" NULL
ins 5  "Holmes; the final problem" NULL
ins 6  "Mr. Holmes" NULL
ins 7  "Watson alone" NULL
# English stems
ins 10 "He runs every morning" NULL
ins 11 "Running a contract business" NULL
ins 12 "The contracts were signed" NULL
# French, German, Spanish, Russian, Greek
ins 20 "Les contrats sont signés" "'fr'"
ins 21 "Le contrat est signé" "'fr'"
ins 22 "Die Häuser sind alt" "'de'"
ins 23 "Das Haus ist alt" "'de'"
ins 24 "Los contratos firmados" "'es'"
ins 25 "Книги на столе" "'ru'"
ins 26 "Я читаю книгу" "'ru'"
ins 27 "Ο δρόμος είναι μακρύς" "'el'"
# Hebrew: prefixes, niqqud, acronyms with both quote forms, final letters, English inside
ins 40 "הבית הגדול" "'he'"
ins 41 "ובבית שלנו" "'he'"
ins 42 "בַּיִת יָפֶה" "'he'"
ins 43 "דובר צה\"ל אמר" "'he'"
ins 44 "דובר צה״ל הודיע" "'he'"
ins 45 "החוזה signed by both parties" "'he'"
# Arabic and Persian: article, diacritics, tatweel, alef forms, Persian letters
ins 60 "الكتاب على الطاولة" "'ar'"
ins 61 "كِتَابٌ جديد" "'ar'"
ins 62 "كتـــاب قديم" "'ar'"
ins 63 "أحمد في المدرسة" "'ar'"
ins 64 "احمد والمدرسه" "'ar'"
ins 65 "کتاب فارسی" "'fa'"
ins 66 "The contract العقد" "'ar'"
# Hindi, Tamil, Georgian
ins 80 "लड़कों ने खेला" "'hi'"
ins 81 "லண்டன் நகரம்" "'ta'"
ins 82 "გამარჯობა მსოფლიო" "'ka'"

q "CALL vfts.create_index('vf_lang', 'vf_lang.docs', 'lang=en margin=0')" > /dev/null
ids() {   # query [extra parameters] -> sorted doc ids, space separated
    q "SELECT LISTAGG(doc_id::VARCHAR USING PARAMETERS separator=' ') FROM (SELECT doc_id FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_lang', query='$1', k=100 ${2:-}) OVER() FROM vf_lang.vf_lang_live) r ORDER BY doc_id) x"
}

expect "Sherlock Holmes: every punctuation case found" "$(ids holmes)" "1 2 3 4 5 6"
expect "English stem: run, runs, running"       "$(ids running)" "10 11"
expect "English exact"                          "$(ids running ", mode='exact'")" "11"
expect "English stem: contracts"                "$(ids contract ", mode='stem'")" "11 12 66"
expect "French stem (lang=fr)"                  "$(ids contrats ", lang='fr'")" "20 21"
expect "French exact"                           "$(ids contrats ", lang='fr', mode='exact'")" "20"
expect "accent folding: signes = signés"        "$(ids signes ", mode='exact'")" "20"
expect "German stem: Häuser, Haus"              "$(ids haus ", lang='de'")" "22 23"
expect "Spanish stem"                           "$(ids contrato ", lang='es'")" "24"
expect "Russian stem: книги, книгу"             "$(ids книга ", lang='ru'")" "25 26"
expect "Greek: accents kept, case folded"       "$(ids ΔΡΌΜΟΣ ", mode='exact'")" "27"
expect "Hebrew stem: prefixes ה, וב"            "$(ids בית ", lang='he'")" "40 41 42"
expect "Hebrew exact: no prefix stripping"      "$(ids בית ", lang='he', mode='exact'")" "42"
expect "Hebrew niqqud removed"                  "$(ids בית ", mode='exact'")" "42"
expect "Hebrew acronym, gershayim"              "$(ids 'צה״ל')" "43 44"
expect "Hebrew acronym, ASCII quote"            "$(ids 'צה\"ל' ", mode='exact'")" "43 44"
expect "English word in a Hebrew document"      "$(ids signed)" "12 45"
expect "Arabic: article, diacritics, tatweel (and Persian letters)" "$(ids كتاب ", lang='ar'")" "60 61 62 65"
expect "Arabic exact: no article stripping"     "$(ids كتاب ", lang='ar', mode='exact'")" "61 62 65"
expect "Arabic alef forms, teh marbuta"         "$(ids أحمد ", mode='exact'")" "63 64"
expect "Arabic teh marbuta = heh"               "$(ids مدرسة ", lang='ar'")" "63 64"
expect "Persian keheh = kaf"                    "$(ids كتاب ", mode='exact'")" "61 62 65"
expect "Arabic document with English"           "$(ids العقد ", lang='ar'")" "66"
expect "Hindi"                                  "$(ids लड़कों ", mode='exact'")" "80"
expect "Tamil"                                  "$(ids லண்டன் ", mode='exact'")" "81"
expect "Georgian (no stemmer)"                  "$(ids გამარჯობა)" "82"
expect "w/N across scripts"                     "$(ids 'contract w/1 العقد' ", lang='ar'")" "66"

# unsupported scripts: an error by default, indexed without stemming with on_unsupported='plain'
q "CREATE TABLE vf_lang.cjk (doc_id INT NOT NULL, body LONG VARCHAR); INSERT INTO vf_lang.cjk VALUES (1, 'hello 北京欢迎你'); COMMIT;" > /dev/null
expect_error "unsupported script refused" "CALL vfts.create_index('vf_strict', 'vf_lang.cjk', 'margin=0')" "not supported"
q "CALL vfts.drop_index('vf_strict')" > /dev/null 2>&1
q "CALL vfts.create_index('vf_plain', 'vf_lang.cjk', 'on_unsupported=plain margin=0')" > /dev/null
expect "on_unsupported=plain" "$(q "SELECT COUNT(*) FROM (SELECT vfts.tsearch(* USING PARAMETERS index='vf_plain', query='hello', k=5) OVER() FROM vf_lang.vf_plain_live) r")" "1"

done_test
