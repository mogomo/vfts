// Builds a segment from documents: analyze, collect postings per term, write (segment_writer.h).
// Documents may arrive in any order; the segment numbers them in doc_id order.
// Not thread safe: one builder per thread (a build with T threads makes T segments).
#ifndef VFTS_ENGINE_BUILDER_H
#define VFTS_ENGINE_BUILDER_H

#include "analyzer.h"
#include "stemmer.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vfts {

class SegmentBuilder {
public:
    explicit SegmentBuilder(const AnalyzerOptions &opts);

    // Adds one document. lang LANG_NONE = the index default. ver: its version (microseconds),
    // only the largest is kept (informational).
    void add(std::int64_t doc_id, std::string_view text, Lang lang = LANG_NONE, std::int64_t ver = 0);

    std::size_t docs() const { return doc_ids_.size(); }
    std::size_t terms() const { return terms_.size(); }
    // Rough memory held, bytes.
    std::size_t memory() const { return memory_; }

    // The segment's bytes. Throws on a duplicate doc_id. The builder is empty afterwards.
    std::string finish(std::int64_t segment_id);

    Analyzer &analyzer() { return analyzer_; }

private:
    struct TermAcc {
        std::string buf;            // per document: varint docidx delta, varint tf, varint position deltas
        std::uint32_t last = 0;     // last document index + 1 (0: none yet)
        std::uint64_t langs = 0;    // bit per stemming language seen
    };
    std::uint32_t intern(std::string_view term);
    void grow();

    Analyzer analyzer_;
    Stemmer stemmer_;
    // open addressing: slot -> term index + 1 (0 empty)
    std::vector<std::uint32_t> slots_;
    std::vector<std::uint64_t> hashes_;     // by term index
    std::string keys_;
    std::vector<std::uint32_t> key_off_;    // by term index, plus one
    std::vector<TermAcc> terms_;
    std::vector<std::int64_t> doc_ids_;
    std::vector<std::uint32_t> doc_len_;
    std::int64_t max_ver_ = 0;
    std::size_t memory_ = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> scratch_;   // (term index, position)
};

// The hash of the term map (FNV-1a 64, mixed). Exposed for tests.
std::uint64_t term_hash(std::string_view s);

} // namespace vfts

#endif
