// Languages and scripts known to the analyzer.
//
// A language is given as an ISO 639-1 code ('en', 'he', ...). Every language belongs to one
// script. The stemmer of a word is chosen from the language of its document when the word is
// written in that language's script, and from the script otherwise (an English word inside a
// Hebrew document is stemmed as English). See docs/design.md, "Languages".
#ifndef VFTS_ENGINE_LANGUAGE_H
#define VFTS_ENGINE_LANGUAGE_H

#include <cstdint>
#include <string>
#include <string_view>

namespace vfts {

// Scripts the analyzer distinguishes. Unsupported scripts are written without spaces between
// words (or, for Hangul, need a morphological analyser): see README "Languages".
enum class Script : std::uint8_t {
    Other = 0,      // a script with spaces and no stemmer (Georgian, Ethiopic, ...), digits only
    Latin, Greek, Cyrillic, Armenian, Arabic, Hebrew, Devanagari, Tamil,
    Unsupported,    // Han, Hiragana, Katakana, Hangul, Thai, Lao, Khmer, Myanmar
};

// Stemming method of a language.
enum class StemKind : std::uint8_t { None, Snowball, HebrewPrefix };

// A language id is an index into the table in language.cpp. 0 is "none": no stemming.
using Lang = std::uint8_t;
constexpr Lang LANG_NONE = 0;

struct LangInfo {
    const char *code;       // ISO 639-1, lower case ('' for LANG_NONE)
    const char *snowball;   // libstemmer algorithm name, or nullptr
    Script script;
    StemKind stem;
    bool unsupported;       // zh, ja, ko, th, lo, km, my
};

const LangInfo &lang_info(Lang id);
std::size_t lang_count();

// Finds a code ('en', 'EN', 'iw' for Hebrew, 'nb'/'nn' for Norwegian). Returns false if unknown.
bool find_lang(std::string_view code, Lang &out);
// Same, but throws std::runtime_error naming the code when it is unknown. Empty = LANG_NONE.
Lang parse_lang(std::string_view code);

// The language whose stemmer applies to a word of script `s` in a document of language `doc`:
// doc itself when the word is written in doc's script, else the default for the script
// (Latin: `latin_default` (the index's language when it is a Latin-script one, else English),
// Cyrillic: Russian, Arabic: Arabic, Hebrew: Hebrew, Greek: Greek, Armenian: Armenian,
// Devanagari: Hindi, Tamil: Tamil, others: none).
Lang stem_lang_for(Script s, Lang doc, Lang latin_default);

const char *script_name(Script s);

} // namespace vfts

#endif
