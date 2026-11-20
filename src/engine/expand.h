// Term expansion for prefix, wildcard and fuzzy words: which dictionary terms they match.
// All comparisons are on Unicode code points of the normalized terms.
#ifndef VTEXT_ENGINE_EXPAND_H
#define VTEXT_ENGINE_EXPAND_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vtext {

// UTF-8 to code points (invalid bytes become U+FFFD).
void to_code_points(std::string_view s, std::vector<std::uint32_t> &out);

// Glob match: * any run of code points (also empty), ? exactly one code point.
bool wildcard_match(const std::vector<std::uint32_t> &pattern, const std::vector<std::uint32_t> &text);

// Damerau-Levenshtein distance (optimal string alignment: adjacent transpositions count 1, as in
// Lucene's FuzzyQuery with transpositions=true), or max_edits + 1 when it is larger than max_edits.
int bounded_osa_distance(const std::vector<std::uint32_t> &a, const std::vector<std::uint32_t> &b, int max_edits);

// The literal prefix of a wildcard pattern (before the first * or ?), as bytes.
std::string literal_prefix(std::string_view pattern);

} // namespace vtext

#endif
