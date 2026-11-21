// The query language (README "Query syntax"), parsed into a tree.
//
//   term                      one word (analyzed like the documents)
//   "exact phrase"            adjacent words in order;  "roman empire"~10  Lucene sloppy phrase
//   a AND b, a OR b, a NOT b  boolean; also +a (required), -a (excluded), ( )
//   a b                       implicit operator: OR (operator='and' makes it AND)
//   contract w/5 indemnity    within 5 words, any order (e-discovery)
//   contract pre/5 indemnity  the first precedes the second by at most 5 words
//   indemn*  te?t             prefix and wildcard
//   holmse~  holmse~1         fuzzy, Damerau-Levenshtein, at most 2 edits (~ alone = 2)
//
// Precedence, lowest first: implicit operator, OR, AND / NOT, unary (+ - NOT), w/N and pre/N.
// Operands of w/N and pre/N must be words, phrases, prefix/wildcard/fuzzy words, OR groups of
// those, or other w/N / pre/N expressions.
//
// The tree maps one to one onto Lucene's BooleanQuery (must / should / must_not), which is how
// the benchmark builds the equivalent OpenSearch / Elasticsearch query.
#ifndef VTEXT_ENGINE_QUERY_H
#define VTEXT_ENGINE_QUERY_H

#include "analyzer.h"
#include "stopwords.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vtext {

enum class QKind : std::uint8_t { None, Term, Phrase, Prefix, Wildcard, Fuzzy, Bool, Near };

struct QNode {
    QKind kind = QKind::None;
    // Term, Fuzzy: words[0]. Phrase: words in order; "" marks a removed stopword (a gap).
    std::vector<std::string> words;
    std::vector<Lang> langs;            // stemming language per word
    std::string pattern;                // Prefix: the prefix; Wildcard: pattern with * and ?
    int slop = 0;                       // Phrase
    int edits = 2;                      // Fuzzy
    int dist = 0;                       // Near
    bool ordered = false;               // Near: pre/N
    std::vector<QNode> must, should, must_not;  // Bool
    std::vector<QNode> kids;            // Near: two operands
};

struct QueryOptions {
    Lang lang = LANG_NONE;              // query language (LANG_NONE: the index default)
    bool op_and = false;                // implicit operator AND
    bool stopwords = false;             // drop stopwords of the query language
    bool plain = false;                 // no syntax: every analyzed word is a clause (like a match query)
    std::size_t max_clauses = 1024;
};

// Parses a query. Throws std::runtime_error with a message that points at the problem.
QNode parse_query(std::string_view query, Analyzer &analyzer, const QueryOptions &opts);

// A canonical text form, for tests and messages: t:word "a b"~2 p:pre* w:te?t f:word~1
// (+must should -must_not) (a w/5 b) (a pre/5 b)
std::string to_string(const QNode &n);

// True if the node can be an operand of w/N or pre/N (it produces word positions).
bool positional(const QNode &n);

// Number of leaves (terms, phrases, patterns) in the tree.
std::size_t count_leaves(const QNode &n);

} // namespace vtext

#endif
