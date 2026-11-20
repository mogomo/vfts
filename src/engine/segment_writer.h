// Writes the bytes of one segment (segment.h, docs/format.md).
//
// Usage: set_docs, then for every term in ascending byte order begin_term / add_doc (ascending
// docno) / end_term, add_stem in any order, finish. Every way of making a segment (new documents,
// table rows, merge) goes through this class, so equal input always gives equal bytes.
#ifndef VTEXT_ENGINE_SEGMENT_WRITER_H
#define VTEXT_ENGINE_SEGMENT_WRITER_H

#include "segment.h"

#include <string>
#include <utility>
#include <vector>

namespace vtext {

class SegmentWriter {
public:
    SegmentWriter(std::int64_t segment_id, std::int64_t max_ver);

    // The documents, by docno: doc ids ascending and unique, and their token counts.
    void set_docs(std::vector<std::int64_t> doc_ids, std::vector<std::uint32_t> doc_len);

    void begin_term(std::string_view term);
    // positions: tf values, ascending. Documents in ascending docno.
    void add_doc(std::uint32_t docno, const std::uint32_t *positions, std::uint32_t tf);
    // Returns the term id (its rank in byte order).
    std::uint32_t end_term();

    // Records that term `term_id` belongs to the stem group `key` (stem_key()).
    void add_stem(std::string key, std::uint32_t term_id);

    std::uint32_t n_terms() const { return static_cast<std::uint32_t>(term_off_.size() - 1); }

    // The finished segment. The writer cannot be used afterwards.
    std::string finish();

private:
    void flush_block();

    std::int64_t segment_id_, max_ver_;
    std::vector<std::int64_t> doc_ids_;
    std::vector<std::uint32_t> doc_len_;
    std::string pool_;
    std::vector<std::uint32_t> term_off_;
    std::vector<TermInfo> info_;
    std::string post_, pos_, bitmaps_;
    std::vector<BitmapEntry> bitmap_dir_;
    std::vector<std::pair<std::string, std::uint32_t>> stems_;

    // the term being written
    bool in_term_ = false;
    std::string last_term_;
    std::vector<std::uint32_t> docs_, tfs_;
    std::vector<std::uint64_t> block_pos_;      // positions offset of each block's first doc
    std::uint64_t term_pos_off_ = 0;
};

} // namespace vtext

#endif
