// The analyzer: text in, normalized terms with positions and byte offsets out.
//
// Index time and query time use this same code (docs/design.md, "Analysis"):
//   0. The quote characters ’ ʼ ＇ ׳ ״ unified with ' and " (a geresh used as a quotation mark,
//      ׳תיאטרון מסך׳, is then cut off as an ASCII apostrophe is; offsets stay those of the text).
//   1. ICU word boundaries (UAX #29). Segments that are not words (spaces, punctuation) are
//      dropped. "Holmes'", "Holmes," and "Holmes-style" all give the token "holmes"; Hebrew
//      acronyms with gershayim (צה"ל, צה״ל) stay one token.
//   2. NFKC with case folding (ICU nfkc_cf); Turkish dotted and dotless i for documents in 'tr'.
//      A case-sensitive index (option case=sensitive) uses NFKC without case folding.
//   3. Per script: accent folding (Latin by default), the Arabic and Persian normalization of
//      Lucene, removal of Hebrew points and cantillation marks.
// Terms are stored unstemmed; stemming is a column of the term dictionary (stemmer.h).
//
// An Analyzer is not thread safe: use one per thread. Creating one is cheap (a clone of a
// prototype break iterator).
#ifndef VFTS_ENGINE_ANALYZER_H
#define VFTS_ENGINE_ANALYZER_H

#include "language.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vfts {

enum class AccentFold : std::uint8_t {
    None,       // keep every accent
    Latin,      // remove accents from words in Latin script (the default)
    All,        // also Greek and Cyrillic (never Arabic, Hebrew or Indic: there the marks are
                // handled by the script's own normalizer or are part of the letters)
};

struct AnalyzerOptions {
    Lang default_lang = LANG_NONE;      // the index language; also picks the stemmer for Latin words
    AccentFold fold = AccentFold::Latin;
    bool unsupported_plain = false;     // on_unsupported='plain': keep such words, no stemming
    std::uint32_t max_token_bytes = 255; // longer normalized words are skipped (no position)
    bool case_sensitive = false;        // case=sensitive: NFKC without case folding; stems keep the case
};

// Thrown when text contains words in a script the index does not support.
struct UnsupportedText : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Token {
    std::uint32_t off, len;     // the term, in Analyzer::arena()
    std::uint32_t pos;          // word number, from 0
    std::uint32_t start, end;   // byte offsets in the input text
    Lang lang;                  // language whose stemmer applies (LANG_NONE: none)
    Script script;
};

class Analyzer {
public:
    explicit Analyzer(const AnalyzerOptions &opts = AnalyzerOptions());
    ~Analyzer();
    Analyzer(const Analyzer &) = delete;
    Analyzer &operator=(const Analyzer &) = delete;

    // Analyzes one text. doc_lang LANG_NONE means the index default. Previous results are dropped.
    // Throws UnsupportedText (unless unsupported_plain).
    void analyze(std::string_view text, Lang doc_lang = LANG_NONE);

    std::size_t size() const { return tokens_.size(); }
    const Token &token(std::size_t i) const { return tokens_[i]; }
    std::string_view term(std::size_t i) const { return std::string_view(arena_).substr(tokens_[i].off, tokens_[i].len); }
    const std::vector<Token> &tokens() const { return tokens_; }

    // Normalizes text that is known to be part of one word (the literal pieces of a wildcard
    // pattern): steps 2 and 3 only. Returns the script found (Other if none).
    Script normalize_piece(std::string_view piece, Lang doc_lang, std::string &out);

    const AnalyzerOptions &options() const { return opts_; }

private:
    struct Impl;
    AnalyzerOptions opts_;
    std::unique_ptr<Impl> impl_;
    std::string arena_;
    std::vector<Token> tokens_;
};

// "Holmes" -> "holmes" etc.: the terms of a text, for tests and the tokenize() function.
std::vector<std::string> analyze_terms(std::string_view text, Lang lang = LANG_NONE,
                                       const AnalyzerOptions &opts = AnalyzerOptions());

// The ICU version linked in, e.g. "79.1".
std::string icu_version();

} // namespace vfts

#endif
