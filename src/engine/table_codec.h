// A segment as rows of the index tables (docs/format.md, "Tables") and back.
//
//   term_dict  (term_id, term, stem, lang, doc_freq)  one row per term and stemming language
//   postings   (term_id, chunk_id, docs)               CRoaring bitmap (portable) of the docnos
//                                                      of one 65,536-docno chunk, low 16 bits
//   positions  (term_id, chunk_id, payload)            per document of the chunk, in docno order:
//                                                      varint tf, then tf varint position deltas
//   doc_stats  (docno, doc_id, length)
//
// segment_to_rows(seg) followed by SegmentAssembler on those rows gives the same bytes again
// (tests/engine/test_segment.cpp). That is what makes the per-node cache file only a copy.
#ifndef VTEXT_ENGINE_TABLE_CODEC_H
#define VTEXT_ENGINE_TABLE_CODEC_H

#include "segment.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace vtext {

constexpr std::uint32_t CHUNK_BITS = 16;            // 65,536 docnos per postings row

struct TermDictRow { std::uint32_t term_id; std::string term, stem, lang; std::uint32_t doc_freq; };
struct PostingRow { std::uint32_t term_id, chunk_id; std::string docs, payload; };
struct DocRow { std::uint32_t docno; std::int64_t doc_id; std::uint32_t length; };

struct RowSink {
    std::function<void(const TermDictRow &)> term;
    std::function<void(const PostingRow &)> posting;
    std::function<void(const DocRow &)> doc;
};

void segment_to_rows(const Segment &seg, const RowSink &sink);

// Collects the rows of one segment (in any order) and writes the segment.
class SegmentAssembler {
public:
    void add(const TermDictRow &r);
    void add(PostingRow &&r);
    void add(const DocRow &r);
    std::string finish(std::int64_t segment_id, std::int64_t max_ver);
    std::size_t memory() const { return memory_; }

private:
    struct Term { std::string term; std::vector<std::pair<std::string, std::string>> stems; std::uint32_t df = 0; };
    std::map<std::uint32_t, Term> terms_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, PostingRow> postings_;
    std::vector<DocRow> docs_;
    std::size_t memory_ = 0;
};

} // namespace vtext

#endif
