// Reading the postings of one term of a segment: documents in docno order, their tf and their
// positions. Blocks of 128 documents are decoded when first touched; positions are decoded only
// for documents that ask for them (phrase and proximity checks).
#ifndef VFTS_ENGINE_POSTINGS_H
#define VFTS_ENGINE_POSTINGS_H

#include "segment.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace vfts {

constexpr std::uint32_t NO_DOC = 0xFFFFFFFFu;

class PostingIter {
public:
    PostingIter() = default;
    PostingIter(const Segment &seg, std::uint32_t term) { init(seg, seg.info(term)); }
    // A list given by its TermInfo (also the postings of a stem group, which have no positions).
    PostingIter(const Segment &seg, const TermInfo &ti) { init(seg, ti); }

    void init(const Segment &seg, const TermInfo &ti)
    {
        df_ = ti.df;
        n_blocks_ = ti.n_blocks;
        pos_base_ = ti.pos_off == NO_POSITIONS ? nullptr : seg.positions() + ti.pos_off;
        max_tf_ = ti.max_tf;
        min_len_ = ti.min_len;
        if (df_ == 1) {
            single_ = true;
            docs_[0] = static_cast<std::uint32_t>(ti.post_off);
            tfs_[0] = ti.max_tf;
            block_n_ = 1;
            meta_ = nullptr;
            single_imp_ = Impacts{};
            single_imp_.n = 1;
            single_imp_.tf[0] = ti.max_tf;
            single_imp_.norm[0] = seg.norm(docs_[0]);
            term_imp_ = &single_imp_;
        } else {
            single_ = false;
            term_imp_ = reinterpret_cast<const Impacts *>(seg.postings() + ti.post_off);
            meta_ = reinterpret_cast<const BlockMeta *>(seg.postings() + ti.post_off + sizeof(Impacts));
            l1_ = reinterpret_cast<const L1Meta *>(seg.postings() + ti.post_off + sizeof(Impacts) + n_blocks_ * sizeof(BlockMeta));
            data_ = reinterpret_cast<const std::uint8_t *>(l1_ + l1_count(n_blocks_));
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
    // Norms to prefetch when a block is decoded (the scorer reads norms[doc] of its documents).
    void prefetch_norms(const std::uint8_t *norms) { norms_ = norms; }
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
            if (l1_ && n_blocks_ - b > L1_BLOCKS) {
                // whole groups of L1_BLOCKS blocks first, by the group's last docno
                std::uint32_t g = b / L1_BLOCKS;
                const std::uint32_t ng = l1_count(n_blocks_);
                while (g + 1 < ng && l1_[g].last_doc < target) ++g;
                b = std::max(b, g * L1_BLOCKS);
            }
            while (b < n_blocks_ && meta_[b].last_doc < target) ++b;
            if (b >= n_blocks_) { block_ = n_blocks_ - 1; return doc_ = NO_DOC; }
            block_ = b;
            i_ = 0;
        }
        if (decoded_ != block_) { decode(block_); i_ = 0; }
        // the block's last doc is >= target: a few steps first (the next document is the usual
        // target), then a binary search for a far one
        for (int k = 0; k < 8 && docs_[i_] < target; ++k) ++i_;
        if (docs_[i_] < target) i_ = static_cast<std::uint32_t>(std::lower_bound(docs_ + i_ + 1, docs_ + block_n_, target) - docs_);
        return doc_ = docs_[i_];
    }

    // The impacts of all the term's documents.
    const Impacts &term_impacts() const { return *term_imp_; }

    // Block-max support: the block that would hold `target`, without decoding it: its last docno
    // and its impacts. Returns false when target is past the last document.
    bool shallow(std::uint32_t target, std::uint32_t &last_doc, const Impacts *&imp) const
    {
        if (single_) {
            if (docs_[0] < target) return false;
            last_doc = docs_[0];
            imp = term_imp_;
            return true;
        }
        std::uint32_t b = block_;
        while (b < n_blocks_ && meta_[b].last_doc < target) ++b;
        if (b >= n_blocks_) return false;
        last_doc = meta_[b].last_doc;
        imp = &meta_[b].imp;
        return true;
    }

    // Calls f(impacts) for impacts that together cover every document in [t, upto] (and maybe more):
    // single blocks near the edges, groups of L1_BLOCKS blocks where a whole group starts inside.
    // Without decoding. Returns false (having stopped) after max_calls calls.
    template <class F> bool cover(std::uint32_t t, std::uint32_t upto, int max_calls, F f) const
    {
        if (single_) {
            if (docs_[0] >= t && docs_[0] <= upto) f(*term_imp_);
            return true;
        }
        std::uint32_t b = block_;
        if (b < n_blocks_ && meta_[b].last_doc < t) {
            std::uint32_t g = b / L1_BLOCKS;
            while (g + 1 < l1_count(n_blocks_) && l1_[g].last_doc < t) ++g;
            b = std::max(b, g * L1_BLOCKS);
            while (b < n_blocks_ && meta_[b].last_doc < t) ++b;
        }
        if (b >= n_blocks_) return true;
        if (meta_[b].last_doc >= upto) { f(meta_[b].imp); return true; }
        if (b + 1 < n_blocks_ && meta_[b + 1].last_doc >= upto) { f(meta_[b].imp); f(meta_[b + 1].imp); return true; }
        int calls = 0;
        for (std::uint32_t g = b / L1_BLOCKS; g < l1_count(n_blocks_); ++g) {     // groups: a superset of the range
            if (++calls > max_calls) return false;
            f(l1_[g].imp);
            if (l1_[g].last_doc >= upto) break;
        }
        return true;
    }

    // Calls f(impacts) for every block that overlaps [t, upto], without decoding. Returns false
    // (having stopped) when that is more than max_blocks blocks.
    template <class F> bool blocks_upto(std::uint32_t t, std::uint32_t upto, int max_blocks, F f) const
    {
        if (single_) {
            if (docs_[0] >= t && docs_[0] <= upto) f(*term_imp_);
            return true;
        }
        std::uint32_t b = block_;
        while (b < n_blocks_ && meta_[b].last_doc < t) ++b;
        for (int k = 0; b < n_blocks_; ++b) {
            if (++k > max_blocks) return false;
            f(meta_[b].imp);
            if (meta_[b].last_doc >= upto) break;
        }
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
    // prefetch: also the norms of the block's documents (a list that is read document by document;
    // a list that is only probed at a few documents does not need them)
    void decode(std::uint32_t b, bool prefetch = true)
    {
        const BlockMeta &m = meta_[b];
        const std::uint32_t n = b + 1 < n_blocks_ ? BLOCK : df_ - b * BLOCK;
        const std::uint8_t *p = data_ + m.data_off;
        const std::uint32_t db = p[0], tb = p[1];
        unpack_bits(p + 2, n, db, docs_);
        unpack_bits(p + 2 + packed_bytes(n, db), n, tb, tfs_);
        std::uint32_t prev = b == 0 ? NO_DOC : meta_[b - 1].last_doc;     // NO_DOC + 1 wraps to 0
        for (std::uint32_t k = 0; k < n; ++k) { prev += docs_[k] + 1; docs_[k] = prev; tfs_[k] += 1; }
        if (norms_ && prefetch) for (std::uint32_t k = 0; k < n; k += 1) __builtin_prefetch(norms_ + docs_[k]);
        block_n_ = n;
        decoded_ = b;
    }

    std::uint32_t df_ = 0, n_blocks_ = 0, max_tf_ = 0, min_len_ = 0;
    bool single_ = false, positioned_ = false;
    const BlockMeta *meta_ = nullptr;
    const L1Meta *l1_ = nullptr;
    const Impacts *term_imp_ = nullptr;
    const std::uint8_t *norms_ = nullptr;
    Impacts single_imp_{};
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

} // namespace vfts

#endif
