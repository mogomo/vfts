#include "analyzer.h"

#include <unicode/brkiter.h>
#include <unicode/normalizer2.h>
#include <unicode/uchar.h>
#include <unicode/unistr.h>
#include <unicode/uscript.h>
#include <unicode/utext.h>
#include <unicode/uversion.h>

#include <mutex>

namespace vtext {

namespace {

// The word break iterator is built once per process from ICU's rules and cloned per Analyzer.
const icu::BreakIterator &prototype()
{
    static std::once_flag once;
    static icu::BreakIterator *proto = nullptr;
    static std::string error;
    std::call_once(once, [] {
        UErrorCode st = U_ZERO_ERROR;
        proto = icu::BreakIterator::createWordInstance(icu::Locale::getRoot(), st);
        if (U_FAILURE(st)) { delete proto; proto = nullptr; error = u_errorName(st); }
    });
    if (!proto) throw std::runtime_error("ICU word break iterator cannot be created: " + error);
    return *proto;
}

const icu::Normalizer2 &normalizer(int which)
{
    static std::once_flag once;
    static const icu::Normalizer2 *n[3] = {nullptr, nullptr, nullptr};
    static std::string error;
    std::call_once(once, [] {
        UErrorCode st = U_ZERO_ERROR;
        n[0] = icu::Normalizer2::getNFKCCasefoldInstance(st);
        if (U_SUCCESS(st)) n[1] = icu::Normalizer2::getNFDInstance(st);
        if (U_SUCCESS(st)) n[2] = icu::Normalizer2::getNFCInstance(st);
        if (U_FAILURE(st)) { n[0] = n[1] = n[2] = nullptr; error = u_errorName(st); }
    });
    if (!n[which]) throw std::runtime_error("ICU normalizer cannot be created: " + error);
    return *n[which];
}
enum { NFKC_CF = 0, NFD = 1, NFC = 2 };

Script script_of(UScriptCode sc)
{
    switch (sc) {
    case USCRIPT_LATIN: return Script::Latin;
    case USCRIPT_GREEK: return Script::Greek;
    case USCRIPT_CYRILLIC: return Script::Cyrillic;
    case USCRIPT_ARMENIAN: return Script::Armenian;
    case USCRIPT_ARABIC: return Script::Arabic;
    case USCRIPT_HEBREW: return Script::Hebrew;
    case USCRIPT_DEVANAGARI: return Script::Devanagari;
    case USCRIPT_TAMIL: return Script::Tamil;
    case USCRIPT_HAN: case USCRIPT_HIRAGANA: case USCRIPT_KATAKANA: case USCRIPT_HANGUL:
    case USCRIPT_THAI: case USCRIPT_LAO: case USCRIPT_KHMER: case USCRIPT_MYANMAR:
        return Script::Unsupported;
    default: return Script::Other;
    }
}

// The script of the first character that has one (digits and punctuation have none).
Script find_script(const icu::UnicodeString &u)
{
    for (int32_t i = 0; i < u.length();) {
        const UChar32 c = u.char32At(i);
        UErrorCode st = U_ZERO_ERROR;
        const UScriptCode sc = uscript_getScript(c, &st);
        if (U_SUCCESS(st) && sc != USCRIPT_COMMON && sc != USCRIPT_INHERITED && sc != USCRIPT_UNKNOWN)
            return script_of(sc);
        i += U16_LENGTH(c);
    }
    return Script::Other;
}

// Remove non-spacing marks after canonical decomposition, then compose again: "é" -> "e".
void fold_accents(icu::UnicodeString &u)
{
    UErrorCode st = U_ZERO_ERROR;
    icu::UnicodeString d = normalizer(NFD).normalize(u, st);
    if (U_FAILURE(st)) return;
    icu::UnicodeString kept;
    bool changed = false;
    for (int32_t i = 0; i < d.length();) {
        const UChar32 c = d.char32At(i);
        if (u_charType(c) == U_NON_SPACING_MARK) changed = true;
        else kept.append(c);
        i += U16_LENGTH(c);
    }
    if (!changed) return;
    u = normalizer(NFC).normalize(kept, st);
}

// Lucene ArabicNormalizer and PersianNormalizer, applied to every word in Arabic script.
void normalize_arabic(icu::UnicodeString &u)
{
    icu::UnicodeString out;
    for (int32_t i = 0; i < u.length(); ++i) {
        UChar c = u.charAt(i);
        if ((c >= 0x064B && c <= 0x0652) || c == 0x0640 || c == 0x0654) continue;  // tashkeel, tatweel, hamza above
        switch (c) {
        case 0x0622: case 0x0623: case 0x0625: c = 0x0627; break;   // alef with madda / hamza -> alef
        case 0x0649: c = 0x064A; break;                             // alef maksura -> yeh
        case 0x0629: c = 0x0647; break;                             // teh marbuta -> heh
        case 0x06CC: case 0x06D2: c = 0x064A; break;                // farsi yeh, yeh barree -> yeh
        case 0x06A9: c = 0x0643; break;                             // keheh -> kaf
        case 0x06C0: case 0x06C1: c = 0x0647; break;                // heh with yeh, heh goal -> heh
        default: break;
        }
        out.append(c);
    }
    u = out;
}

// Hebrew: points and cantillation marks (U+0591 to U+05C7) removed; geresh and gershayim unified
// with the ASCII apostrophe and quote. Final letters stay as they are.
void normalize_hebrew(icu::UnicodeString &u)
{
    icu::UnicodeString out;
    for (int32_t i = 0; i < u.length(); ++i) {
        UChar c = u.charAt(i);
        if (c >= 0x0591 && c <= 0x05C7) continue;
        if (c == 0x05F4) c = '"';
        else if (c == 0x05F3) c = '\'';
        out.append(c);
    }
    u = out;
}

// Apostrophes that UAX #29 keeps inside words, unified with the ASCII one ("don’t" = "don't").
void unify_apostrophes(icu::UnicodeString &u)
{
    for (int32_t i = 0; i < u.length(); ++i) {
        const UChar c = u.charAt(i);
        if (c == 0x2019 || c == 0x02BC || c == 0xFF07) u.setCharAt(i, '\'');
    }
}

} // namespace

struct Analyzer::Impl {
    std::unique_ptr<icu::BreakIterator> bi;
    UText *ut = nullptr;
    std::string scratch;
    Impl() : bi(prototype().clone()) { if (!bi) throw std::runtime_error("ICU break iterator clone failed"); }
    ~Impl() { if (ut) utext_close(ut); }
};

Analyzer::Analyzer(const AnalyzerOptions &opts) : opts_(opts), impl_(new Impl) {}
Analyzer::~Analyzer() = default;

Script Analyzer::normalize_piece(std::string_view piece, Lang doc_lang, std::string &out)
{
    out.clear();
    const bool turkish = doc_lang != LANG_NONE && lang_info(doc_lang).code == std::string_view("tr");
    bool ascii = true, alpha = false, cap_i = false;
    for (unsigned char ch : piece) {
        ascii &= ch < 0x80;
        alpha |= (ch | 0x20) >= 'a' && (ch | 0x20) <= 'z';
        cap_i |= ch == 'I';
    }
    if (ascii && !(turkish && cap_i)) {
        // NFKC with case folding changes nothing in ASCII but the upper case letters.
        out.resize(piece.size());
        for (std::size_t i = 0; i < piece.size(); ++i) {
            const char ch = piece[i];
            out[i] = ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + 32) : ch;
        }
        return alpha ? Script::Latin : Script::Other;
    }
    icu::UnicodeString u = icu::UnicodeString::fromUTF8(icu::StringPiece(piece.data(), static_cast<int32_t>(piece.size())));
    if (turkish) {
        for (int32_t i = 0; i < u.length(); ++i) {
            if (u.charAt(i) == 0x0130) u.setCharAt(i, 'i');         // I with dot -> i
            else if (u.charAt(i) == 'I') u.setCharAt(i, 0x0131);    // I -> dotless i
        }
    }
    UErrorCode st = U_ZERO_ERROR;
    u = normalizer(NFKC_CF).normalize(u, st);
    if (U_FAILURE(st)) throw std::runtime_error(std::string("ICU normalization failed: ") + u_errorName(st));
    if (turkish) u.findAndReplace(icu::UnicodeString((UChar32)'i').append((UChar32)0x0307), icu::UnicodeString((UChar32)'i'));
    unify_apostrophes(u);
    const Script s = find_script(u);
    switch (s) {
    case Script::Latin:
        if (opts_.fold != AccentFold::None) fold_accents(u);
        break;
    case Script::Greek: case Script::Cyrillic:
        if (opts_.fold == AccentFold::All) fold_accents(u);
        break;
    case Script::Arabic: normalize_arabic(u); break;
    case Script::Hebrew: normalize_hebrew(u); break;
    default: break;
    }
    u.toUTF8String(out);
    return s;
}

void Analyzer::analyze(std::string_view text, Lang doc_lang)
{
    tokens_.clear();
    arena_.clear();
    if (doc_lang == LANG_NONE) doc_lang = opts_.default_lang;
    if (text.empty()) return;
    if (text.size() > 0x7FFFFFFF) throw std::runtime_error("text longer than 2 GB");
    if (lang_info(doc_lang).unsupported && !opts_.unsupported_plain)
        throw UnsupportedText(std::string("language '") + lang_info(doc_lang).code +
                              "' is not supported (written without spaces between words, or needs a morphological "
                              "analyser); create the index with on_unsupported='plain' to index such text without stemming");

    UErrorCode st = U_ZERO_ERROR;
    impl_->ut = utext_openUTF8(impl_->ut, text.data(), static_cast<int64_t>(text.size()), &st);
    if (U_FAILURE(st)) throw std::runtime_error(std::string("ICU utext_openUTF8 failed: ") + u_errorName(st));
    icu::BreakIterator &bi = *impl_->bi;
    bi.setText(impl_->ut, st);
    if (U_FAILURE(st)) throw std::runtime_error(std::string("ICU setText failed: ") + u_errorName(st));

    std::string &norm = impl_->scratch;
    std::uint32_t pos = 0;
    int32_t start = bi.first();
    for (int32_t end = bi.next(); end != icu::BreakIterator::DONE; start = end, end = bi.next()) {
        if (bi.getRuleStatus() < UBRK_WORD_NONE_LIMIT) continue;      // spaces, punctuation
        const std::string_view word = text.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start));
        const Script s = normalize_piece(word, doc_lang, norm);
        if (norm.empty() || norm.size() > opts_.max_token_bytes) continue;
        Lang stem_lang = LANG_NONE;
        if (s == Script::Unsupported) {
            if (!opts_.unsupported_plain)
                throw UnsupportedText("text in a script without spaces between words (Chinese, Japanese, Korean, Thai, "
                                      "Lao, Khmer, Myanmar) is not supported: '" + std::string(word) +
                                      "'; create the index with on_unsupported='plain' to index it without stemming");
        } else {
            stem_lang = stem_lang_for(s, doc_lang, opts_.default_lang);
        }
        Token t;
        t.off = static_cast<std::uint32_t>(arena_.size());
        t.len = static_cast<std::uint32_t>(norm.size());
        t.pos = pos++;
        t.start = static_cast<std::uint32_t>(start);
        t.end = static_cast<std::uint32_t>(end);
        t.lang = stem_lang;
        t.script = s;
        arena_.append(norm);
        tokens_.push_back(t);
    }
}

std::vector<std::string> analyze_terms(std::string_view text, Lang lang, const AnalyzerOptions &opts)
{
    Analyzer a(opts);
    a.analyze(text, lang);
    std::vector<std::string> out;
    for (std::size_t i = 0; i < a.size(); ++i) out.emplace_back(a.term(i));
    return out;
}

std::string icu_version() { return U_ICU_VERSION; }

} // namespace vtext
