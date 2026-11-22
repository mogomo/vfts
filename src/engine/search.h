// Searching a set of segments: BM25 top-k (tsearch) and counting (tcount).
//
// Global statistics: N (documents with at least one token), the average document length and the
// document frequency of every query word are summed over all segments and the live tail, minus
// deleted and masked documents. A document's score therefore depends only on the document and on
// the live collection, never on how documents are split into segments or nodes.
//
// BM25 (k1 = 1.2, b = 0.75) as Lucene's BM25Similarity computes it, the scores of OpenSearch, in the
// same float arithmetic, with Lucene's one-byte length norms (segment.h). With classic = true the
// weights are multiplied by (1 + k1), the classic BM25 formula and the scores of Elasticsearch
// (its LegacyBM25Similarity). The order of the hits is the same. docs/design.md "Scoring".
#ifndef VFTS_ENGINE_SEARCH_H
#define VFTS_ENGINE_SEARCH_H

#include "analyzer.h"
#include "query.h"
#include "segment.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vfts {

enum class MatchMode : std::uint8_t { Stem, Exact };

struct SearchOptions {
    MatchMode match = MatchMode::Stem;
    QueryOptions query;                 // lang, implicit operator, stopwords
    std::uint32_t k = 10;
    int prefix_length = 0;              // fuzzy: leading code points that must match exactly
    std::uint32_t max_expansions = 50;  // prefix, wildcard, fuzzy: terms per word at most
    float k1 = 1.2f, b = 0.75f;
    bool classic = false;               // bm25='classic': weights times (1 + k1), the scores of Elasticsearch
    int threads = 1;                    // segments searched in parallel
    bool pruning = true;                // MaxScore for OR queries (false: score every match; same result)
    std::uint32_t window = 0;           // MaxScore window in docnos (0: 8192)
    struct Stats {                      // filled when set (debugging and measurements)
        double plan_us = 0, exec_us = 0;
        std::uint64_t candidates = 0, scored = 0, windows_skipped = 0, probes = 0, terms = 0;
    } *stats = nullptr;
};

// How deletions change the document frequencies of one segment: computed once per set of
// tombstones (cache.h), so that a query does not scan postings for them.
struct DeadCounts {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> terms;   // (term id, deleted docs), ascending
    std::vector<std::pair<std::uint32_t, std::uint32_t>> stems;   // (stem entry, deleted docs), ascending
    std::uint32_t term(std::uint32_t t) const;
    std::uint32_t stem(std::uint32_t s) const;
};

// Bit set over the docnos of a segment.
struct DocSet {
    std::vector<std::uint64_t> bits;
    std::uint32_t count = 0;
    void resize(std::uint32_t n) { bits.assign((n + 63) / 64, 0); count = 0; }
    bool test(std::uint32_t d) const { return !bits.empty() && (bits[d >> 6] >> (d & 63) & 1); }
    bool set(std::uint32_t d) { std::uint64_t &w = bits[d >> 6]; const std::uint64_t m = 1ull << (d & 63); if (w & m) return false; w |= m; ++count; return true; }
};

// A segment as one search sees it: the bytes, plus its deleted documents (tombstones of the
// active generation) and their effect on document frequencies.
struct SegmentRef {
    const Segment *seg = nullptr;
    std::shared_ptr<const DocSet> dead;             // may be null: nothing deleted
    std::shared_ptr<const DeadCounts> dead_counts;  // required when dead is set
};

// DeadCounts of a segment for a set of deleted docnos (scans every posting list once).
DeadCounts compute_dead_counts(const Segment &seg, const DocSet &dead);

struct Hit {
    std::int64_t doc_id;
    float score;
};

struct CollectionStats {
    std::uint64_t n_docs = 0;           // live documents with at least one token (BM25 N)
    std::uint64_t sum_len = 0;          // their tokens
    std::uint64_t live_docs = 0;        // all live documents, also empty ones
    float avgdl() const { return n_docs ? static_cast<float>(static_cast<double>(sum_len) / static_cast<double>(n_docs)) : 1.0f; }
};

class Searcher {
public:
    Searcher(const AnalyzerOptions &analyzer_options, std::vector<SegmentRef> segments);
    ~Searcher();

    // Hides these doc ids in every segment (rows that changed or were deleted after the segments
    // were made). Call before add_tail and before searching.
    void mask(const std::vector<std::int64_t> &doc_ids);
    // A segment of documents analyzed at query time (the live tail), searched beside the others.
    void add_tail(std::shared_ptr<const std::string> segment_bytes);

    std::vector<Hit> search(const std::string &query, const SearchOptions &opts);
    std::uint64_t count(const std::string &query, const SearchOptions &opts);
    // The doc_ids of every match (no scores, no order): for joins and GROUP BY over all matches.
    std::vector<std::int64_t> matches(const std::string &query, const SearchOptions &opts);
    // For searches from several threads at once: call ready() first (after mask / add_tail), then
    // each thread passes its own analyzer (Analyzer is not shared between threads).
    void ready() { prepare(); }
    std::vector<Hit> search(const std::string &query, const SearchOptions &opts, Analyzer &analyzer);
    // Many queries at once, searched in parallel (one thread each); results in query order.
    std::vector<std::vector<Hit>> search_many(const std::vector<std::string> &queries, const SearchOptions &opts);
    std::vector<std::uint64_t> count_many(const std::vector<std::string> &queries, const SearchOptions &opts);

    const CollectionStats &stats() const { return stats_; }
    // The parsed query, for messages and tests.
    std::string explain(const std::string &query, const SearchOptions &opts);

    struct Impl;

private:
    void prepare();
    std::vector<Hit> run_search(const QNode &q, const SearchOptions &opts);
    std::uint64_t run_count(const QNode &q, const SearchOptions &opts);
    AnalyzerOptions aopts_;
    std::vector<SegmentRef> segs_;
    std::vector<std::shared_ptr<const std::string>> tail_bytes_;
    std::vector<std::unique_ptr<Segment>> tail_segs_;
    std::vector<std::vector<std::uint32_t>> masked_;    // per segment: extra dead docnos, ascending
    std::vector<std::shared_ptr<DocSet>> combined_dead_; // per segment: tombstones + masked
    CollectionStats stats_;
    bool prepared_ = false;
    std::unique_ptr<Analyzer> analyzer_;
};

} // namespace vfts

#endif
