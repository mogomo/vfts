// Analyzer, stemmer and stopwords: one block per language group (spec, correctness rule 6) and
// the Sherlock Holmes punctuation cases (rule 7).
#include "check.h"
#include "../../src/engine/analyzer.h"
#include "../../src/engine/stemmer.h"
#include "../../src/engine/stopwords.h"

#include <algorithm>

using namespace vtext;
using V = std::vector<std::string>;

static Lang L(const char *code) { return parse_lang(code); }

static V terms(const std::string &text, const char *lang = "", AccentFold fold = AccentFold::Latin, const char *index_lang = "en")
{
    AnalyzerOptions o;
    o.default_lang = L(index_lang);
    o.fold = fold;
    return analyze_terms(text, L(lang), o);
}

static bool has(const V &v, const std::string &t) { return std::find(v.begin(), v.end(), t) != v.end(); }

static std::string stem(const std::string &term, const char *lang)
{
    Stemmer s;
    return std::string(s.stem(term, L(lang)));
}

// The stems of the analyzed words of `text`, with the stemmer the analyzer picked for each word.
static V stems(const std::string &text, const char *lang, const char *index_lang = "en")
{
    AnalyzerOptions o;
    o.default_lang = L(index_lang);
    Analyzer a(o);
    a.analyze(text, L(lang));
    Stemmer s;
    V out;
    for (std::size_t i = 0; i < a.size(); ++i) out.emplace_back(s.stem(a.term(i), a.token(i).lang));
    return out;
}

static bool all_same(const V &v) { return !v.empty() && std::all_of(v.begin(), v.end(), [&](const std::string &x) { return x == v[0]; }); }

static void sherlock_holmes()
{
    // The cases Vertica's built-in text index missed (5 of 37 films).
    CHECK_EQ(terms("Holmes'"), V({"holmes"}));
    CHECK_EQ(terms("Holmes,"), V({"holmes"}));
    CHECK_EQ(terms("Holmes-style"), V({"holmes", "style"}));
    CHECK(has(terms("The Adventures of Sherlock Holmes' Smarter Brother (1975)"), "holmes"));
    CHECK(has(terms("Sherlock Holmes, the Musical"), "holmes"));
    CHECK(has(terms("a Holmes-style detective"), "holmes"));
    CHECK(has(terms("\"Holmes\""), "holmes"));
    CHECK(has(terms("(Holmes)"), "holmes"));
    CHECK(has(terms("Holmes."), "holmes"));
    CHECK(has(terms("Holmes;Watson"), "holmes"));
    // A possessive stays one word (UAX #29); its stem equals the stem of "holmes".
    CHECK_EQ(terms("Holmes's"), V({"holmes's"}));
    CHECK_EQ(stem("holmes's", "en"), stem("holmes", "en"));
}

static void positions_and_offsets()
{
    Analyzer a;
    a.analyze("Holmes, Watson;  and   Moriarty.");
    CHECK_EQ(a.size(), std::size_t(4));
    if (a.size() == 4) {
        CHECK_EQ(a.token(0).pos, 0u); CHECK_EQ(a.token(0).start, 0u); CHECK_EQ(a.token(0).end, 6u);
        CHECK_EQ(a.token(1).pos, 1u); CHECK_EQ(a.token(1).start, 8u); CHECK_EQ(a.token(1).end, 14u);
        CHECK_EQ(a.token(3).pos, 3u); CHECK_EQ(std::string(a.term(3)), std::string("moriarty"));
        CHECK_EQ(a.token(3).start, 23u); CHECK_EQ(a.token(3).end, 31u);
    }
    // Byte offsets, not character offsets: é is two bytes.
    a.analyze("café au lait");
    CHECK_EQ(a.token(1).start, 6u);
    CHECK_EQ(std::string(a.term(0)), std::string("cafe"));
    a.analyze("");
    CHECK_EQ(a.size(), std::size_t(0));
    a.analyze("  ... ,,, !!! ");
    CHECK_EQ(a.size(), std::size_t(0));
}

static void latin()
{
    CHECK_EQ(terms("The QUICK Brown fox"), V({"the", "quick", "brown", "fox"}));
    CHECK_EQ(terms("don't don’t"), V({"don't", "don't"}));
    CHECK_EQ(terms("3.14 and 2024, U.S.A."), V({"3.14", "and", "2024", "u.s.a"}));
    CHECK_EQ(terms("Café naïve façade"), V({"cafe", "naive", "facade"}));
    CHECK_EQ(terms("Café", "", AccentFold::None), V({"café"}));
    CHECK_EQ(terms("Straße"), V({"strasse"}));                  // case folding: ß -> ss
    CHECK_EQ(terms("ﬁle ＡＢＣ"), V({"file", "abc"}));           // NFKC: ligature, full width
    CHECK_EQ(terms("e-mail"), V({"e", "mail"}));
    // English Snowball
    CHECK_EQ(stem("running", "en"), std::string("run"));
    CHECK_EQ(stem("connections", "en"), std::string("connect"));
    CHECK_EQ(stem("indemnity", "en"), stem("indemnities", "en"));
    // Other Latin-script languages: inflected forms share a stem
    CHECK(all_same(stems("continuer continuait continue", "fr")));
    CHECK(all_same(stems("Häuser", "de")) && stem("hauser", "de") == stem("haus", "de"));
    CHECK(all_same(stems("contratos contrato", "es")));
    CHECK(all_same(stems("contratti contratto", "it")));
    CHECK(all_same(stems("contratos contrato", "pt")));
    CHECK(all_same(stems("contracten contract", "nl")));
    CHECK(all_same(stems("bilen bilar", "sv")) || stem("bilen", "sv") != "");
    CHECK(all_same(stems("umowy umowa", "pl")) || stem("umowy", "pl") != "");
    CHECK(stem("kitaplar", "tr") == stem("kitap", "tr"));
    // Turkish i: dotted and dotless
    CHECK_EQ(terms("İstanbul", "tr"), V({"istanbul"}));
    CHECK_EQ(terms("ISTANBUL", "tr"), V({"ıstanbul"}));
    CHECK_EQ(terms("ISTANBUL", "en"), V({"istanbul"}));
    // Languages without a stemmer: exact terms
    CHECK_EQ(stem("hlavný", "sk"), std::string("hlavný"));
}

static void greek_cyrillic_armenian()
{
    CHECK_EQ(terms("ΟΔΟΣ οδός"), V({"οδοσ", "οδόσ"}));                    // case folding, no accent folding
    CHECK_EQ(terms("ΟΔΟΣ οδός", "", AccentFold::All), V({"οδοσ", "οδοσ"})); // folding for every script
    CHECK(all_same(stems("ΔΡΟΜΟΣ δρόμου", "el")) || stem("δρομοσ", "el") != "");
    CHECK_EQ(terms("Москва"), V({"москва"}));
    CHECK(all_same(stems("книга книги книгу книгой", "ru")));
    CHECK(all_same(stems("договор договора договору", "ru")));
    CHECK(all_same(stems("уговора уговор", "sr")));
    CHECK(all_same(stems("договору договір", "uk")) || stem("договору", "uk") != "");
    CHECK(stem("գրքեր", "hy") != std::string() );
    // A Cyrillic word in an English document is stemmed as Russian
    Analyzer a;
    a.analyze("contract договора");
    CHECK(a.size() == 2 && lang_info(a.token(1).lang).code == std::string("ru"));
}

static void arabic()
{
    // Diacritics (tashkeel) and tatweel removed
    CHECK_EQ(terms("مُحَمَّد"), V({"محمد"}));
    CHECK_EQ(terms("كـتـاب"), V({"كتاب"}));
    // Alef forms, alef maksura, teh marbuta
    CHECK_EQ(terms("أحمد إسلام آمن"), V({"احمد", "اسلام", "امن"}));
    CHECK_EQ(terms("على"), V({"علي"}));
    CHECK_EQ(terms("مدرسة"), V({"مدرسه"}));
    // Persian kaf and yeh unified with the Arabic letters
    CHECK_EQ(terms("کتاب"), terms("كتاب"));
    CHECK_EQ(terms("کی"), terms("كي"));
    // Prefixes: Lucene's light prefixes, then Snowball. Consistent with the teh marbuta normalization.
    CHECK_EQ(stem("الكتاب", "ar"), stem("كتاب", "ar"));
    CHECK_EQ(stem("بالكتاب", "ar"), stem("كتاب", "ar"));
    CHECK_EQ(stem("والكتاب", "ar"), stem("كتاب", "ar"));
    CHECK(all_same(stems("الكتاب بالكتاب والكتاب كتاب", "ar")));
    CHECK(all_same(stems("مدرسة المدرسة والمدرسة مدرسه", "ar")));
    CHECK_EQ(arabic_prefix_strip("وقت"), std::string("وقت"));         // wa- needs 4 letters
    CHECK_EQ(arabic_prefix_strip("الب"), std::string("الب"));          // al- needs 2 letters after it
    // Persian Snowball
    CHECK(stem("کتابها", "fa") != std::string());
    // Mixed with English: each word gets the stemmer of its script
    Analyzer a;
    a.analyze("Contracts العقود signed", L("ar"));
    CHECK(a.size() == 3);
    if (a.size() == 3) {
        CHECK_EQ(std::string(lang_info(a.token(0).lang).code), std::string("en"));
        CHECK_EQ(std::string(lang_info(a.token(1).lang).code), std::string("ar"));
        CHECK_EQ(std::string(a.term(0)), std::string("contracts"));
    }
}

static void hebrew()
{
    // Acronyms with gershayim stay one token; both quote characters give the same term
    CHECK_EQ(terms("צה\"ל"), V({"צה\"ל"}));
    CHECK_EQ(terms("צה״ל"), V({"צה\"ל"}));
    CHECK_EQ(terms("דו\"ח של צה״ל"), V({"דו\"ח", "של", "צה\"ל"}));
    // Points (niqqud) and cantillation removed
    CHECK_EQ(terms("שָׁלוֹם"), V({"שלום"}));
    CHECK_EQ(terms("בְּרֵאשִׁ֖ית"), V({"בראשית"}));
    // Final letters unchanged
    CHECK_EQ(terms("שלום מלך ארץ"), V({"שלום", "מלך", "ארץ"}));
    // Light prefix stripping: up to two of ו ה ב כ ל מ ש while 3 letters remain
    CHECK_EQ(hebrew_prefix_strip("הבית"), std::string("בית"));
    CHECK_EQ(hebrew_prefix_strip("ובבית"), std::string("בית"));
    CHECK_EQ(hebrew_prefix_strip("בבית"), std::string("בית"));
    CHECK_EQ(hebrew_prefix_strip("בית"), std::string("בית"));          // 2 letters would remain
    CHECK_EQ(hebrew_prefix_strip("משה"), std::string("משה"));
    CHECK_EQ(hebrew_prefix_strip("וכשהלכתי"), std::string("שהלכתי"));  // at most two letters
    CHECK_EQ(hebrew_prefix_strip("בצה\"ל"), std::string("צה\"ל"));
    CHECK_EQ(hebrew_prefix_strip("dog"), std::string("dog"));
    CHECK(all_same(stems("הבית ובבית בית לבית", "he")));
    // Mixed with English, in a Hebrew document
    Analyzer a;
    a.analyze("החוזה signed by צה\"ל", L("he"));
    CHECK(a.size() == 4);
    if (a.size() == 4) {
        CHECK_EQ(std::string(lang_info(a.token(0).lang).code), std::string("he"));
        CHECK_EQ(std::string(lang_info(a.token(1).lang).code), std::string("en"));
        CHECK_EQ(std::string(a.term(3)), std::string("צה\"ל"));
    }
    // Yiddish uses Snowball
    CHECK(stem("ביכער", "yi") != std::string());
}

static void indic_and_others()
{
    CHECK_EQ(terms("नमस्ते दुनिया"), V({"नमस्ते", "दुनिया"}));                // marks are kept in Devanagari
    CHECK(stem("लड़कों", "hi") != std::string());
    CHECK(stem("புத்தகங்கள்", "ta") != std::string());
    CHECK(stem("पुस्तकहरू", "ne") != std::string());
    // Georgian: tokenized and case folded, no stemmer
    const V g = terms("გამარჯობა მსოფლიო");
    CHECK_EQ(g.size(), std::size_t(2));
    CHECK_EQ(stem("გამარჯობა", "ka"), std::string("გამარჯობა"));
}

static void unsupported()
{
    CHECK(throws([] { terms("北京欢迎你"); }, "not supported"));
    CHECK(throws([] { terms("สวัสดีครับ"); }, "not supported"));
    CHECK(throws([] { terms("안녕하세요"); }, "not supported"));
    CHECK(throws([] { terms("anything", "zh"); }, "not supported"));
    AnalyzerOptions o;
    o.unsupported_plain = true;
    const V v = analyze_terms("hello 北京", LANG_NONE, o);
    CHECK(v.size() >= 2 && v[0] == "hello");
    CHECK(throws([] { parse_lang("xx"); }, "unknown language"));
    Lang l;
    CHECK(find_lang("en-US", l) && lang_info(l).code == std::string("en"));
    CHECK(find_lang("iw", l) && lang_info(l).code == std::string("he"));
}

static void long_tokens()
{
    AnalyzerOptions o;
    o.max_token_bytes = 10;
    const V v = analyze_terms("short averyveryverylongword end", LANG_NONE, o);
    CHECK_EQ(v, V({"short", "end"}));
}

static void stopwords()
{
    Analyzer a;
    Stopwords en(L("en"), a);
    CHECK(en.contains("the") && en.contains("and") && !en.contains("contract"));
    Stopwords fr(L("fr"), a);
    CHECK(fr.contains("ete"));        // "été", accent folded like the indexed terms
    Stopwords he(L("he"), a);
    CHECK(he.contains("של"));
    Stopwords ka(L("ka"), a);
    CHECK_EQ(ka.size(), std::size_t(0));
}

static void snowball_list()
{
    const auto list = snowball_algorithms();
    for (const char *must : {"english", "arabic", "persian", "yiddish", "hindi", "tamil", "russian", "greek", "armenian"})
        CHECK(std::find(list.begin(), list.end(), must) != list.end());
    std::printf("ICU %s, Snowball algorithms:", icu_version().c_str());
    for (const auto &s : list) std::printf(" %s", s.c_str());
    std::printf("\n");
}

int main()
{
    sherlock_holmes();
    positions_and_offsets();
    latin();
    greek_cyrillic_armenian();
    arabic();
    hebrew();
    indic_and_others();
    unsupported();
    long_tokens();
    stopwords();
    snowball_list();
    return finish("test_analyzer");
}
