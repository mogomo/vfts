#include "language.h"

#include <cctype>
#include <stdexcept>

namespace vtext {

namespace {

// Order matters only for LANG_NONE at 0; ids are not stored anywhere (the term dictionary stores
// the code), so entries may be added anywhere after it.
const LangInfo LANGS[] = {
    {"", nullptr, Script::Other, StemKind::None, false},
    // Latin script, Snowball
    {"en", "english", Script::Latin, StemKind::Snowball, false},
    {"fr", "french", Script::Latin, StemKind::Snowball, false},
    {"de", "german", Script::Latin, StemKind::Snowball, false},
    {"es", "spanish", Script::Latin, StemKind::Snowball, false},
    {"it", "italian", Script::Latin, StemKind::Snowball, false},
    {"pt", "portuguese", Script::Latin, StemKind::Snowball, false},
    {"nl", "dutch", Script::Latin, StemKind::Snowball, false},
    {"da", "danish", Script::Latin, StemKind::Snowball, false},
    {"no", "norwegian", Script::Latin, StemKind::Snowball, false},
    {"sv", "swedish", Script::Latin, StemKind::Snowball, false},
    {"fi", "finnish", Script::Latin, StemKind::Snowball, false},
    {"et", "estonian", Script::Latin, StemKind::Snowball, false},
    {"lt", "lithuanian", Script::Latin, StemKind::Snowball, false},
    {"pl", "polish", Script::Latin, StemKind::Snowball, false},
    {"cs", "czech", Script::Latin, StemKind::Snowball, false},
    {"ro", "romanian", Script::Latin, StemKind::Snowball, false},
    {"hu", "hungarian", Script::Latin, StemKind::Snowball, false},
    {"tr", "turkish", Script::Latin, StemKind::Snowball, false},
    {"id", "indonesian", Script::Latin, StemKind::Snowball, false},
    {"ga", "irish", Script::Latin, StemKind::Snowball, false},
    {"eu", "basque", Script::Latin, StemKind::Snowball, false},
    {"ca", "catalan", Script::Latin, StemKind::Snowball, false},
    {"eo", "esperanto", Script::Latin, StemKind::Snowball, false},
    {"st", "sesotho", Script::Latin, StemKind::Snowball, false},
    // Latin script, no stemmer: exact terms only
    {"sk", nullptr, Script::Latin, StemKind::None, false},
    {"sl", nullptr, Script::Latin, StemKind::None, false},
    {"hr", nullptr, Script::Latin, StemKind::None, false},
    {"lv", nullptr, Script::Latin, StemKind::None, false},
    {"is", nullptr, Script::Latin, StemKind::None, false},
    {"ms", nullptr, Script::Latin, StemKind::None, false},
    {"vi", nullptr, Script::Latin, StemKind::None, false},
    {"sw", nullptr, Script::Latin, StemKind::None, false},
    // Greek, Cyrillic, Armenian
    {"el", "greek", Script::Greek, StemKind::Snowball, false},
    {"ru", "russian", Script::Cyrillic, StemKind::Snowball, false},
    {"sr", "serbian", Script::Cyrillic, StemKind::Snowball, false},
    {"uk", "ukrainian", Script::Cyrillic, StemKind::Snowball, false},
    {"bg", nullptr, Script::Cyrillic, StemKind::None, false},
    {"mk", nullptr, Script::Cyrillic, StemKind::None, false},
    {"hy", "armenian", Script::Armenian, StemKind::Snowball, false},
    // Arabic script
    {"ar", "arabic", Script::Arabic, StemKind::Snowball, false},
    {"fa", "persian", Script::Arabic, StemKind::Snowball, false},
    {"ur", nullptr, Script::Arabic, StemKind::None, false},
    // Hebrew script
    {"he", nullptr, Script::Hebrew, StemKind::HebrewPrefix, false},
    {"yi", "yiddish", Script::Hebrew, StemKind::Snowball, false},
    // Indic
    {"hi", "hindi", Script::Devanagari, StemKind::Snowball, false},
    {"ne", "nepali", Script::Devanagari, StemKind::Snowball, false},
    {"mr", nullptr, Script::Devanagari, StemKind::None, false},
    {"ta", "tamil", Script::Tamil, StemKind::Snowball, false},
    // Others with spaces, no stemmer
    {"ka", nullptr, Script::Other, StemKind::None, false},
    {"am", nullptr, Script::Other, StemKind::None, false},
    // Written without spaces (or needing a morphological analyser): refused unless
    // on_unsupported='plain'
    {"zh", nullptr, Script::Unsupported, StemKind::None, true},
    {"ja", nullptr, Script::Unsupported, StemKind::None, true},
    {"ko", nullptr, Script::Unsupported, StemKind::None, true},
    {"th", nullptr, Script::Unsupported, StemKind::None, true},
    {"lo", nullptr, Script::Unsupported, StemKind::None, true},
    {"km", nullptr, Script::Unsupported, StemKind::None, true},
    {"my", nullptr, Script::Unsupported, StemKind::None, true},
};

constexpr std::size_t N_LANGS = sizeof(LANGS) / sizeof(LANGS[0]);

Lang by_code(const char *code)
{
    for (std::size_t i = 1; i < N_LANGS; ++i)
        if (std::string_view(LANGS[i].code) == code) return static_cast<Lang>(i);
    return LANG_NONE;
}

} // namespace

const LangInfo &lang_info(Lang id) { return LANGS[id < N_LANGS ? id : 0]; }
std::size_t lang_count() { return N_LANGS; }

bool find_lang(std::string_view code, Lang &out)
{
    std::string c;
    for (char ch : code) c += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    // Trim a region or script suffix: 'en-US', 'pt_BR', 'sr-Latn'.
    const std::size_t cut = c.find_first_of("-_");
    if (cut != std::string::npos) c.resize(cut);
    if (c.empty()) { out = LANG_NONE; return true; }
    if (c == "iw") c = "he";                    // old code for Hebrew
    if (c == "nb" || c == "nn") c = "no";       // Norwegian Bokmal, Nynorsk
    if (c == "ji") c = "yi";                    // old code for Yiddish
    if (c == "in") c = "id";                    // old code for Indonesian
    for (std::size_t i = 1; i < N_LANGS; ++i)
        if (c == LANGS[i].code) { out = static_cast<Lang>(i); return true; }
    return false;
}

Lang parse_lang(std::string_view code)
{
    Lang l;
    if (!find_lang(code, l)) throw std::runtime_error("unknown language code '" + std::string(code) + "'");
    return l;
}

Lang stem_lang_for(Script s, Lang doc, Lang latin_default)
{
    if (doc != LANG_NONE && lang_info(doc).script == s) return doc;
    switch (s) {
    case Script::Latin: return latin_default != LANG_NONE && lang_info(latin_default).script == Script::Latin
                                   ? latin_default : by_code("en");
    case Script::Greek: return by_code("el");
    case Script::Cyrillic: return by_code("ru");
    case Script::Armenian: return by_code("hy");
    case Script::Arabic: return by_code("ar");
    case Script::Hebrew: return by_code("he");
    case Script::Devanagari: return by_code("hi");
    case Script::Tamil: return by_code("ta");
    default: return LANG_NONE;
    }
}

const char *script_name(Script s)
{
    switch (s) {
    case Script::Latin: return "Latin";
    case Script::Greek: return "Greek";
    case Script::Cyrillic: return "Cyrillic";
    case Script::Armenian: return "Armenian";
    case Script::Arabic: return "Arabic";
    case Script::Hebrew: return "Hebrew";
    case Script::Devanagari: return "Devanagari";
    case Script::Tamil: return "Tamil";
    case Script::Unsupported: return "unsupported";
    default: return "other";
    }
}

} // namespace vtext
