// Stems of normalized terms. The index stores terms unstemmed; the stem is a column of the term
// dictionary, used at query time to expand a word to every indexed term with the same stem
// (match='stem'). A wrong stem therefore only changes which terms a query expands to.
//
//   Snowball (libstemmer_c) for every language it has (sb_stemmer_list()).
//   Arabic: Lucene's light prefixes (ال وال بال كال فال لل و) removed first, then Snowball.
//   Hebrew: light prefix stripping. Up to 2 leading letters from ו ה ב כ ל מ ש are removed while
//   at least 3 letters remain. There is no free Hebrew stemmer; recall is good but below a full
//   morphological analyser.
//
// A Stemmer is not thread safe: use one per thread.
#ifndef VFTS_ENGINE_STEMMER_H
#define VFTS_ENGINE_STEMMER_H

#include "language.h"

#include <string>
#include <string_view>
#include <vector>

struct sb_stemmer;

namespace vfts {

class Stemmer {
public:
    Stemmer();
    ~Stemmer();
    Stemmer(const Stemmer &) = delete;
    Stemmer &operator=(const Stemmer &) = delete;

    // The stem of `term` (already normalized) in language `lang`. LANG_NONE or a language
    // without a stemmer returns the term itself. The view is valid until the next call.
    std::string_view stem(std::string_view term, Lang lang);

private:
    std::vector<sb_stemmer *> sb_;      // by language id, created on first use
    std::vector<bool> tried_;
    std::string buf_, pre_;
};

// The Arabic prefix step on its own (UTF-8 in, UTF-8 out).
std::string arabic_prefix_strip(std::string_view term);

// The Hebrew light stemmer on its own (UTF-8 in, UTF-8 out).
std::string hebrew_prefix_strip(std::string_view term);

// Names of the Snowball algorithms linked in (sb_stemmer_list()).
std::vector<std::string> snowball_algorithms();

} // namespace vfts

#endif
