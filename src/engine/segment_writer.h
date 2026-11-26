// Writes the bytes of one segment (segment.h, docs/format.md).
//
// Usage: set_docs, then for every term in ascending byte order begin_term / add_doc (ascending
// docno) / end_term, add_stem in any order, finish. Every way of making a segment (new documents,
// table rows, merge) goes through this class, so equal input always gives equal bytes.
#ifndef VFTS_ENGINE_SEGMENT_WRITER_H
#define VFTS_ENGINE_SEGMENT_WRITER_H

#include "segment.h"

#include <string>
#include <utility>
#include <vector>

namespace vfts {

class SegmentWriter {
public:
    SegmentWriter(std::int64_t segment_id, std::int64_t max_ver);

    // The documents, by docno: doc ids (unique, in any order) and their token counts. Doc ids in
    // ascending order give a format 4 segment, any other order format 5 (segment.h).
    void set_docs(std::vector<std::int64_t> doc_ids, std::vector<std::uint32_t> doc_len);

    void begin_term(std::string_view term);
    // positions: tf values, ascending. Documents in ascending docno.
    void add_doc(std::uint32_t docno, const std::uint32_t *positions, std::uint32_t tf);
    // Returns the term id (its rank in byte order).
    std::uint32_t end_term();

    // Records that term `term_id` belongs to the stem group `key` (stem_key()).
    void add_stem(std::string key, std::uint32_t term_id);

    std::uint32_t n_terms() const { return static_cast<std::uint32_t>(term_off_.size() - 1); }

    // Writing in parallel (merge): part() is a writer for a run of terms, with the documents of
    // this one (shared, read only; this writer must outlive it). Its term ids start at 0.
    // append(part) puts the part's terms, postings, positions, bitmaps and stems behind this
    // writer's, as if they had been written here: the bytes are the same as one writer's.
    SegmentWriter part() const;
    void append(SegmentWriter &&part);

    // The finished segment. The writer cannot be used afterwards. threads: stem groups' postings
    // are made in parallel (the bytes do not depend on it).
    std::string finish(unsigned threads = 1);

private:
    const std::vector<std::uint32_t> &lens() const { return shared_len_ ? *shared_len_ : doc_len_; }
    std::size_t n_docs() const { return shared_len_ ? shared_len_->size() : doc_ids_.size(); }
    void flush_block();
    TermInfo write_postings(const std::vector<std::uint32_t> &docs, const std::vector<std::uint32_t> &tfs,
                            std::uint64_t pos_off, const std::vector<std::uint64_t> *block_pos);
    std::string assemble(SegmentHeader &h, const std::vector<std::uint8_t> &norms, const std::vector<StemEntry> &entries,
                         const std::string &stem_pool, const std::vector<std::uint32_t> &stem_terms,
                         const std::vector<TermInfo> &groups);

    std::int64_t segment_id_, max_ver_;
    std::vector<std::int64_t> doc_ids_;
    std::vector<std::uint32_t> order_;          // docnos by doc_id; empty when doc ids ascend
    std::vector<std::uint32_t> doc_len_;
    const std::vector<std::uint32_t> *shared_len_ = nullptr;   // a part(): the main writer's lengths
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

} // namespace vfts

#endif
