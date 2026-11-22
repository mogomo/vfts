// Stopword lists, used only at query time when a query asks for stopwords=true. They are never
// removed at index time: that would break exact phrase search.
#ifndef VFTS_ENGINE_STOPWORDS_H
#define VFTS_ENGINE_STOPWORDS_H

#include "analyzer.h"

#include <string>
#include <string_view>
#include <unordered_set>

namespace vfts {

class Stopwords {
public:
    // The list of `lang` (none for languages without a list), normalized with `analyzer`'s options,
    // so that it compares with analyzed terms.
    Stopwords(Lang lang, Analyzer &analyzer);
    bool contains(std::string_view term) const { return set_.count(std::string(term)) != 0; }
    std::size_t size() const { return set_.size(); }

private:
    std::unordered_set<std::string> set_;
};

// The raw list of a language code, space separated ("" if there is none).
const char *stopword_list(std::string_view code);

} // namespace vfts

#endif
