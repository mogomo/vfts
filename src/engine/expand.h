// Term expansion for prefix, wildcard and fuzzy words: which dictionary terms they match.
// All comparisons are on Unicode code points of the normalized terms.
#ifndef VFTS_ENGINE_EXPAND_H
#define VFTS_ENGINE_EXPAND_H

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace vfts {

// UTF-8 to code points (invalid bytes become U+FFFD).
void to_code_points(std::string_view s, std::vector<std::uint32_t> &out);

// Glob match: * any run of code points (also empty), ? exactly one code point.
bool wildcard_match(const std::vector<std::uint32_t> &pattern, const std::vector<std::uint32_t> &text);

// Damerau-Levenshtein distance (optimal string alignment: adjacent transpositions count 1, as in
// Lucene's FuzzyQuery with transpositions=true), or max_edits + 1 when it is larger than max_edits.
int bounded_osa_distance(const std::vector<std::uint32_t> &a, const std::vector<std::uint32_t> &b, int max_edits);

// The terms of a sorted dictionary (byte order: term(i), i < n; lower_bound(s) is the first index
// whose term is >= s) within max_edits of word (bounded_osa_distance), among the terms that start
// with the bytes prefix. The dictionary is walked as a trie: the rows of the distance table are kept
// per code point of the current term and reused for the prefix it shares with the previous term;
// a prefix that can no longer come within max_edits makes every term under it skipped at once.
// hit(index, term, distance) is called in dictionary order.
void fuzzy_terms(std::uint32_t n, const std::function<std::string_view(std::uint32_t)> &term,
                 const std::function<std::uint32_t(std::string_view)> &lower_bound,
                 const std::vector<std::uint32_t> &word, int max_edits, const std::string &prefix,
                 const std::function<void(std::uint32_t, std::string_view, int)> &hit);

// The literal prefix of a wildcard pattern (before the first * or ?), as bytes.
std::string literal_prefix(std::string_view pattern);

} // namespace vfts

#endif
