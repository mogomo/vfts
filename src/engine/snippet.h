// Snippets: the parts of a text that match a query, with the matching words marked.
//
// The text is analyzed with the index's analyzer; a word is a hit when it matches a word, prefix,
// wildcard or fuzzy word of the query that is not under NOT (with mode 'stem', words with the same
// stem match). The best windows of `words` words are chosen by the number of different query words
// they contain, then by the number of hits; they are returned in text order. The original text is
// kept: marks are inserted at the byte offsets of the hits.
#ifndef VTEXT_ENGINE_SNIPPET_H
#define VTEXT_ENGINE_SNIPPET_H

#include "analyzer.h"
#include "search.h"

#include <string>

namespace vtext {

struct SnippetOptions {
    MatchMode match = MatchMode::Stem;
    QueryOptions query;
    int fragments = 2;                  // windows at most
    int words = 20;                     // words per window
    int prefix_length = 0;              // fuzzy
    std::string pre = "<b>", post = "</b>", separator = " ... ";
};

class Snippeter {
public:
    Snippeter(const AnalyzerOptions &aopts, const std::string &query, const SnippetOptions &opts);
    ~Snippeter();
    std::string snippet(std::string_view text, Lang lang = LANG_NONE);
    // Number of hits in the last text.
    std::size_t hits() const { return hits_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::size_t hits_ = 0;
};

} // namespace vtext

#endif
