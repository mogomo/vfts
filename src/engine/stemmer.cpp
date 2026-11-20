#include "stemmer.h"

#include <libstemmer.h>

#include <stdexcept>

namespace vtext {

namespace {

// Hebrew letters are U+05D0 to U+05EA: two bytes in UTF-8, 0xD7 0x90 to 0xD7 0xAA.
bool hebrew_letter_at(std::string_view s, std::size_t i)
{
    return i + 1 < s.size() && static_cast<unsigned char>(s[i]) == 0xD7 &&
           static_cast<unsigned char>(s[i + 1]) >= 0x90 && static_cast<unsigned char>(s[i + 1]) <= 0xAA;
}

bool hebrew_prefix_letter(std::string_view s, std::size_t i)
{
    if (!hebrew_letter_at(s, i)) return false;
    switch (static_cast<unsigned char>(s[i + 1])) {
    case 0x95:  // ו vav
    case 0x94:  // ה he
    case 0x91:  // ב bet
    case 0x9B:  // כ kaf
    case 0x9C:  // ל lamed
    case 0x9E:  // מ mem
    case 0xA9:  // ש shin
        return true;
    default:
        return false;
    }
}

std::size_t hebrew_letters(std::string_view s)
{
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size();) {
        if (hebrew_letter_at(s, i)) { ++n; i += 2; }
        else ++i;
    }
    return n;
}

} // namespace

std::string hebrew_prefix_strip(std::string_view term)
{
    std::size_t letters = hebrew_letters(term);
    std::size_t at = 0;
    for (int k = 0; k < 2; ++k) {
        if (!hebrew_prefix_letter(term, at) || letters - 1 < 3) break;
        at += 2;
        --letters;
    }
    return std::string(term.substr(at));
}

Stemmer::Stemmer() : sb_(lang_count(), nullptr), tried_(lang_count(), false) {}

Stemmer::~Stemmer()
{
    for (sb_stemmer *s : sb_) if (s) sb_stemmer_delete(s);
}

std::string_view Stemmer::stem(std::string_view term, Lang lang)
{
    const LangInfo &info = lang_info(lang);
    if (lang == LANG_NONE || info.stem == StemKind::None) return term;
    if (info.stem == StemKind::HebrewPrefix) {
        buf_ = hebrew_prefix_strip(term);
        return buf_;
    }
    if (!tried_[lang]) {
        tried_[lang] = true;
        sb_[lang] = sb_stemmer_new(info.snowball, "UTF_8");
    }
    sb_stemmer *s = sb_[lang];
    if (!s) return term;            // not in this libstemmer build: exact terms only
    const sb_symbol *r = sb_stemmer_stem(s, reinterpret_cast<const sb_symbol *>(term.data()), static_cast<int>(term.size()));
    if (!r) throw std::runtime_error("out of memory in the Snowball stemmer");
    buf_.assign(reinterpret_cast<const char *>(r), static_cast<std::size_t>(sb_stemmer_length(s)));
    return buf_;
}

std::vector<std::string> snowball_algorithms()
{
    std::vector<std::string> out;
    for (const char **p = sb_stemmer_list(); *p; ++p) out.emplace_back(*p);
    return out;
}

} // namespace vtext
