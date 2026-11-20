// Reading the postings of one term of a segment: documents in docno order, their tf and their
// positions. Blocks of 128 documents are decoded when first touched; positions are decoded only
// for documents that ask for them (phrase and proximity checks).
#ifndef VTEXT_ENGINE_POSTINGS_H
#define VTEXT_ENGINE_POSTINGS_H

#include "segment.h"

#include <cstdint>
#include <vector>

namespace vtext {

constexpr std::uint32_t NO_DOC = 0xFFFFFFFFu;

class PostingIter {
public:
    PostingIter() = default;
    PostingIter(const Segment &seg, std::uint32_t term) { init(seg, term); }

    void init(const Segment &seg, std::uint32_t term)
    {
        const TermInfo &ti = seg.info(term);
        df_ = ti.df;
        n_blocks_ = ti.n_blocks;
        pos_base_ = seg.positions() + ti.pos_off;
        max_tf_ = ti.max_tf;
        min_len_ = ti.min_len;
        if (df_ == 1) {
            single_ = true;
            docs_[0] = static_cast<std::uint32_t>(ti.post_off);
            tfs_[0] = ti.max_tf;
            block_n_ = 1;
            meta_ = nullptr;
        } else {
            single_ = false;
            meta_ = reinterpret_cast<const BlockMeta *>(seg.postings() + ti.post_off);
            data_ = seg.postings() + ti.post_off + n_blocks_ * sizeof(BlockMeta);
            block_n_ = 0;
        }
        block_ = 0;
        decoded_ = single_ ? 0 : NO_DOC;
        i_ = 0;
        doc_ = NO_DOC;
        positioned_ = false;
        pos_ptr_ = pos_base_;
        pos_block_ = NO_DOC;
        pos_i_ = 0;
    }

    std::uint32_t df() const { return df_; }
    std::uint32_t doc() const { return doc_; }
    std::uint32_t tf() const { return tfs_[i_]; }
    std::uint32_t term_max_tf() const { return max_tf_; }
    std::uint32_t term_min_len() const { return min_len_; }

    // Moves to the next document; returns it (NO_DOC at the end).
    std::uint32_t next()
    {
        if (!positioned_) return advance(0);
        if (doc_ == NO_DOC) return NO_DOC;
        if (i_ + 1 < block_n_) { ++i_; return doc_ = docs_[i_]; }
        if (single_ || block_ + 1 >= n_blocks_) return doc_ = NO_DOC;
        ++block_;
        decode(block_);
        i_ = 0;
        return doc_ = docs_[0];
    }

    // Moves to the first document >= target (never backwards); returns it or NO_DOC.
    std::uint32_t advance(std::uint32_t target)
    {
        if (positioned_ && (doc_ == NO_DOC || doc_ >= target)) return doc_;
        const bool first = !positioned_;
        positioned_ = true;
        if (single_) return doc_ = (first && docs_[0] >= target) ? docs_[0] : NO_DOC;
        if (meta_[block_].last_doc < target) {
            // skip whole blocks by their last docno
            std::uint32_t b = block_ + 1;
            while (b < n_blocks_ && meta_[b].last_doc < target) ++b;
            if (b >= n_blocks_) { block_ = n_blocks_ - 1; return doc_ = NO_DOC; }
            block_ = b;
            i_ = 0;
        }
        if (decoded_ != block_) { decode(block_); i_ = 0; }
        while (docs_[i_] < target) ++i_;     // the block's last doc is >= target
        return doc_ = docs_[i_];
    }

    // Block-max support: the block that would hold `target`, without decoding it.
    // Returns false when target is past the last document.
    bool shallow(std::uint32_t target, std::uint32_t &last_doc, std::uint32_t &max_tf, std::uint32_t &min_len) const
    {
        if (single_) {
            if (docs_[0] < target) return false;
            last_doc = docs_[0]; max_tf = tfs_[0]; min_len = min_len_;
            return true;
        }
        std::uint32_t b = block_;
        while (b < n_blocks_ && meta_[b].last_doc < target) ++b;
        if (b >= n_blocks_) return false;
        last_doc = meta_[b].last_doc; max_tf = meta_[b].max_tf; min_len = meta_[b].min_len;
        return true;
    }

    // The positions of the current document, appended to out (ascending).
    void positions(std::vector<std::uint32_t> &out)
    {
        if (pos_block_ != (single_ ? 0 : block_) || pos_i_ > i_) {
            pos_block_ = single_ ? 0 : block_;
            pos_ptr_ = pos_base_ + (single_ ? 0 : meta_[block_].pos_off);
            pos_i_ = 0;
        }
        while (pos_i_ < i_) { skip_varints(pos_ptr_, tfs_[pos_i_]); ++pos_i_; }
        const std::uint32_t n = tfs_[i_];
        std::uint32_t p = 0;
        for (std::uint32_t k = 0; k < n; ++k) { p += get_varint(pos_ptr_); out.push_back(p); }
        ++pos_i_;
    }

private:
    void decode(std::uint32_t b)
    {
        const BlockMeta &m = meta_[b];
        const std::uint32_t n = b + 1 < n_blocks_ ? BLOCK : df_ - b * BLOCK;
        const std::uint8_t *p = data_ + m.data_off;
        const std::uint32_t db = p[0], tb = p[1];
        unpack_bits(p + 2, n, db, docs_);
        unpack_bits(p + 2 + packed_bytes(n, db), n, tb, tfs_);
        std::uint32_t prev = b == 0 ? NO_DOC : meta_[b - 1].last_doc;     // NO_DOC + 1 wraps to 0
        for (std::uint32_t k = 0; k < n; ++k) { prev += docs_[k] + 1; docs_[k] = prev; tfs_[k] += 1; }
        block_n_ = n;
        decoded_ = b;
    }

    std::uint32_t df_ = 0, n_blocks_ = 0, max_tf_ = 0, min_len_ = 0;
    bool single_ = false, positioned_ = false;
    const BlockMeta *meta_ = nullptr;
    const std::uint8_t *data_ = nullptr;
    const std::uint8_t *pos_base_ = nullptr;
    std::uint32_t block_ = 0, decoded_ = NO_DOC, block_n_ = 0, i_ = 0, doc_ = NO_DOC;
    std::uint32_t docs_[BLOCK], tfs_[BLOCK];
    const std::uint8_t *pos_ptr_ = nullptr;
    std::uint32_t pos_block_ = NO_DOC, pos_i_ = 0;
};

// All docnos of a term, ascending.
inline void term_docs(const Segment &seg, std::uint32_t term, std::vector<std::uint32_t> &out)
{
    PostingIter it(seg, term);
    for (std::uint32_t d = it.next(); d != NO_DOC; d = it.next()) out.push_back(d);
}

} // namespace vtext

#endif
