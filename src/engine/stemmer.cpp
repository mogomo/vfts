#include "stemmer.h"

#include <libstemmer.h>

#include <stdexcept>

namespace vfts {

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

// Lucene ArabicStemmer's prefixes, in its order: removed before Snowball, because the normalization
// of teh marbuta (ة -> ه) makes Snowball treat "المدرسه" and "مدرسه" differently.
std::string arabic_prefix_strip(std::string_view term)
{
    static const char *prefixes[] = {"\xD8\xA7\xD9\x84",                 // ال
                                     "\xD9\x88\xD8\xA7\xD9\x84",       // وال
                                     "\xD8\xA8\xD8\xA7\xD9\x84",       // بال
                                     "\xD9\x83\xD8\xA7\xD9\x84",       // كال
                                     "\xD9\x81\xD8\xA7\xD9\x84",       // فال
                                     "\xD9\x84\xD9\x84",               // لل
                                     "\xD9\x88"};                      // و
    // length in code points
    std::size_t len = 0;
    for (unsigned char c : term) len += (c & 0xC0) != 0x80;
    for (const char *p : prefixes) {
        const std::string_view pre(p);
        const std::size_t plen = pre.size() / 2;      // all two-byte letters
        if (term.compare(0, pre.size(), pre) != 0) continue;
        if (plen == 1 ? len < 4 : len < plen + 2) continue;   // Lucene: wa- needs 4 letters, the others prefix + 2
        return std::string(term.substr(pre.size()));
    }
    return std::string(term);
}

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
    if (info.code[0] == 'a' && info.code[1] == 'r' && info.code[2] == 0) {
        pre_ = arabic_prefix_strip(term);
        term = pre_;
    }
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

} // namespace vfts
