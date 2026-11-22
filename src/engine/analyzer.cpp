#include "analyzer.h"

#include <unicode/brkiter.h>
#include <unicode/normalizer2.h>
#include <unicode/uchar.h>
#include <unicode/unistr.h>
#include <unicode/uscript.h>
#include <unicode/utext.h>
#include <unicode/utf8.h>
#include <unicode/uversion.h>

#include <mutex>

namespace vfts {

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

// Emoji are words too, as in Lucene's ICUTokenizer (ICU's word rules give them no word status):
// characters shown as emoji by default (Emoji_Presentation), a character followed by the emoji
// variation selector U+FE0F (❤️), flags (regional indicators) and keycaps (U+20E3). Symbols in text
// presentation (© ™) are not words.
bool is_emoji(std::string_view seg)
{
    if (seg.empty() || (static_cast<unsigned char>(seg[0]) < 0xC0 && seg.find("\xE2\x83\xA3") == std::string_view::npos)) return false;
    UChar32 c;
    for (int32_t i = 0, n = static_cast<int32_t>(seg.size()); i < n;) {
        U8_NEXT(reinterpret_cast<const uint8_t *>(seg.data()), i, n, c);
        if (c < 0) continue;
        if (c == 0xFE0F || c == 0x20E3) return true;
        if (u_hasBinaryProperty(c, UCHAR_EMOJI_PRESENTATION) || u_hasBinaryProperty(c, UCHAR_REGIONAL_INDICATOR)) return true;
    }
    return false;
}

// The script of a character for splitting words at script changes: Common and Inherited
// characters (digits, punctuation, marks) have none (-1); Han, Hiragana, Katakana and Hangul are
// one group, as in Lucene's ScriptIterator.
int split_script(UChar32 c)
{
    UErrorCode st = U_ZERO_ERROR;
    const UScriptCode sc = uscript_getScript(c, &st);
    if (U_FAILURE(st) || sc == USCRIPT_COMMON || sc == USCRIPT_INHERITED || sc == USCRIPT_UNKNOWN) return -1;
    if (sc == USCRIPT_HIRAGANA || sc == USCRIPT_KATAKANA || sc == USCRIPT_HANGUL) return USCRIPT_HAN;
    return sc;
}

// The byte ranges of the script runs of a word that mixes scripts ("lewisi.הכנסת"), or nothing.
// Lucene's ICUTokenizer cuts the text into runs of one script before it looks for words; a
// character without a script stays in the run before it.
bool script_runs(std::string_view w, std::vector<std::pair<std::size_t, std::size_t>> &runs)
{
    runs.clear();
    bool non_ascii = false;
    for (unsigned char ch : w) non_ascii |= ch >= 0x80;
    if (!non_ascii) return false;
    int cur = -1;
    std::size_t run_start = 0;
    for (int32_t i = 0, n = static_cast<int32_t>(w.size()); i < n;) {
        const int32_t at = i;
        UChar32 c;
        U8_NEXT(reinterpret_cast<const uint8_t *>(w.data()), i, n, c);
        if (c < 0) continue;
        const int sc = split_script(c);
        if (sc < 0) continue;
        if (cur < 0) { cur = sc; continue; }
        if (sc != cur) {
            runs.emplace_back(run_start, static_cast<std::size_t>(at));
            run_start = static_cast<std::size_t>(at);
            cur = sc;
        }
    }
    if (runs.empty()) return false;
    runs.emplace_back(run_start, w.size());
    return true;
}

// Quote characters unified with the ASCII apostrophe and quote before the word boundaries are
// found: UAX #29 gives the geresh, the gershayim and the modifier letter apostrophe the word
// break class of a letter, so a geresh used as a quotation mark (׳תיאטרון מסך׳) would stay on the
// word, where an ASCII apostrophe is cut off; U+2019 and U+FF07 behave as the ASCII apostrophe
// except after a Hebrew letter. The ASCII character for the UTF-8 sequence at p, its length in
// len; 0 if there is none.
char quote_at(const unsigned char *p, std::size_t left, std::size_t &len)
{
    if (left >= 2) {
        if (p[0] == 0xD7 && p[1] == 0xB3) { len = 2; return '\''; }     // U+05F3 geresh
        if (p[0] == 0xD7 && p[1] == 0xB4) { len = 2; return '"'; }      // U+05F4 gershayim
        if (p[0] == 0xCA && p[1] == 0xBC) { len = 2; return '\''; }     // U+02BC modifier letter apostrophe
    }
    if (left >= 3) {
        if (p[0] == 0xE2 && p[1] == 0x80 && p[2] == 0x99) { len = 3; return '\''; }   // U+2019
        if (p[0] == 0xEF && p[1] == 0xBC && p[2] == 0x87) { len = 3; return '\''; }   // U+FF07
    }
    return 0;
}

bool quote_lead(unsigned char ch) { return ch == 0xD7 || ch == 0xCA || ch == 0xE2 || ch == 0xEF; }

} // namespace

struct Analyzer::Impl {
    std::unique_ptr<icu::BreakIterator> bi, sub;    // sub: words inside a script run of a mixed word
    UText *ut = nullptr, *sub_ut = nullptr;
    std::string scratch;
    std::string mapped;                             // the text with its quote characters unified
    std::vector<std::uint32_t> omap;                // byte of mapped -> byte of the text (one more at the end)
    std::vector<std::pair<std::size_t, std::size_t>> runs;
    Impl() : bi(prototype().clone()) { if (!bi) throw std::runtime_error("ICU break iterator clone failed"); }
    ~Impl()
    {
        if (ut) utext_close(ut);
        if (sub_ut) utext_close(sub_ut);
    }
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

    // the quote characters unified first (quote_at); token offsets stay those of the text
    std::string_view src = text;
    std::vector<std::uint32_t> &omap = impl_->omap;
    omap.clear();
    {
        const auto *p = reinterpret_cast<const unsigned char *>(text.data());
        const std::size_t n = text.size();
        std::size_t i = 0, len = 0;
        while (i < n && !(quote_lead(p[i]) && quote_at(p + i, n - i, len))) ++i;
        if (i < n) {
            std::string &m = impl_->mapped;
            m.assign(text.data(), i);
            omap.resize(i);
            for (std::size_t k = 0; k < i; ++k) omap[k] = static_cast<std::uint32_t>(k);
            while (i < n) {
                const char c = quote_lead(p[i]) ? quote_at(p + i, n - i, len) : 0;
                omap.push_back(static_cast<std::uint32_t>(i));
                if (c) {
                    m.push_back(c);
                    i += len;
                } else {
                    m.push_back(text[i]);
                    ++i;
                }
            }
            omap.push_back(static_cast<std::uint32_t>(n));
            src = m;
        }
    }
    auto orig = [&](std::size_t at) { return static_cast<std::uint32_t>(omap.empty() ? at : omap[at]); };

    UErrorCode st = U_ZERO_ERROR;
    impl_->ut = utext_openUTF8(impl_->ut, src.data(), static_cast<int64_t>(src.size()), &st);
    if (U_FAILURE(st)) throw std::runtime_error(std::string("ICU utext_openUTF8 failed: ") + u_errorName(st));
    icu::BreakIterator &bi = *impl_->bi;
    bi.setText(impl_->ut, st);
    if (U_FAILURE(st)) throw std::runtime_error(std::string("ICU setText failed: ") + u_errorName(st));

    std::string &norm = impl_->scratch;
    std::uint32_t pos = 0;
    auto emit = [&](std::string_view word, std::uint32_t start, std::uint32_t end) {
        const Script s = normalize_piece(word, doc_lang, norm);
        if (norm.empty() || norm.size() > opts_.max_token_bytes) return;
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
        t.start = start;
        t.end = end;
        t.lang = stem_lang;
        t.script = s;
        arena_.append(norm);
        tokens_.push_back(t);
    };
    int32_t start = bi.first();
    for (int32_t end = bi.next(); end != icu::BreakIterator::DONE; start = end, end = bi.next()) {
        const std::string_view word = src.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start));
        if (bi.getRuleStatus() < UBRK_WORD_NONE_LIMIT && !is_emoji(word)) continue;      // spaces, punctuation
        if (!script_runs(word, impl_->runs)) {
            emit(word, orig(static_cast<std::size_t>(start)), orig(static_cast<std::size_t>(end)));
            continue;
        }
        // a word across scripts: the words of each script run
        if (!impl_->sub) {
            impl_->sub.reset(prototype().clone());
            if (!impl_->sub) throw std::runtime_error("ICU break iterator clone failed");
        }
        icu::BreakIterator &sb = *impl_->sub;
        for (const auto &r : impl_->runs) {
            const std::string_view run = word.substr(r.first, r.second - r.first);
            impl_->sub_ut = utext_openUTF8(impl_->sub_ut, run.data(), static_cast<int64_t>(run.size()), &st);
            if (U_FAILURE(st)) throw std::runtime_error(std::string("ICU utext_openUTF8 failed: ") + u_errorName(st));
            sb.setText(impl_->sub_ut, st);
            if (U_FAILURE(st)) throw std::runtime_error(std::string("ICU setText failed: ") + u_errorName(st));
            int32_t s0 = sb.first();
            for (int32_t e0 = sb.next(); e0 != icu::BreakIterator::DONE; s0 = e0, e0 = sb.next()) {
                const std::string_view w = run.substr(static_cast<std::size_t>(s0), static_cast<std::size_t>(e0 - s0));
                if (sb.getRuleStatus() < UBRK_WORD_NONE_LIMIT && !is_emoji(w)) continue;
                const std::size_t base = static_cast<std::size_t>(start) + r.first;
                emit(w, orig(base + static_cast<std::size_t>(s0)), orig(base + static_cast<std::size_t>(e0)));
            }
        }
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

} // namespace vfts
