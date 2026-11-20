#include "segment_writer.h"
#include "postings.h"

#include <roaring.h>

#include <algorithm>
#include <stdexcept>

namespace vtext {

namespace {

void align8(std::string &s) { while (s.size() % 8) s.push_back('\0'); }

template <class T> void append_raw(std::string &out, const T *p, std::size_t n)
{
    out.append(reinterpret_cast<const char *>(p), n * sizeof(T));
}

} // namespace

SegmentWriter::SegmentWriter(std::int64_t segment_id, std::int64_t max_ver)
    : segment_id_(segment_id), max_ver_(max_ver)
{
    term_off_.push_back(0);
}

void SegmentWriter::set_docs(std::vector<std::int64_t> doc_ids, std::vector<std::uint32_t> doc_len)
{
    if (doc_ids.size() != doc_len.size()) throw std::runtime_error("segment writer: doc ids and lengths differ in count");
    if (doc_ids.size() >= NO_DOC) throw std::runtime_error("segment writer: too many documents for one segment");
    for (std::size_t i = 1; i < doc_ids.size(); ++i)
        if (doc_ids[i - 1] >= doc_ids[i])
            throw std::runtime_error(doc_ids[i - 1] == doc_ids[i] ? "duplicate doc_id " + std::to_string(doc_ids[i])
                                                                  : std::string("segment writer: doc ids not ascending"));
    doc_ids_ = std::move(doc_ids);
    doc_len_ = std::move(doc_len);
}

void SegmentWriter::begin_term(std::string_view term)
{
    if (in_term_) throw std::runtime_error("segment writer: begin_term inside a term");
    if (!info_.empty() && !(std::string_view(last_term_) < term))
        throw std::runtime_error("segment writer: terms not in ascending order");
    if (term.size() > 0xFFFF) throw std::runtime_error("segment writer: term too long");
    in_term_ = true;
    last_term_.assign(term);
    docs_.clear();
    tfs_.clear();
    block_pos_.clear();
    term_pos_off_ = pos_.size();
}

void SegmentWriter::add_doc(std::uint32_t docno, const std::uint32_t *positions, std::uint32_t tf)
{
    if (docno >= doc_ids_.size()) throw std::runtime_error("segment writer: docno out of range");
    if (!docs_.empty() && docs_.back() >= docno) throw std::runtime_error("segment writer: docnos not ascending in a term");
    if (tf == 0) throw std::runtime_error("segment writer: tf 0");
    if (docs_.size() % BLOCK == 0) block_pos_.push_back(pos_.size());
    docs_.push_back(docno);
    tfs_.push_back(tf);
    std::uint32_t prev = 0;
    for (std::uint32_t k = 0; k < tf; ++k) {
        if (k > 0 && positions[k] <= positions[k - 1]) throw std::runtime_error("segment writer: positions not ascending");
        put_varint(pos_, positions[k] - prev);
        prev = positions[k];
    }
}

std::uint32_t SegmentWriter::end_term()
{
    if (!in_term_) throw std::runtime_error("segment writer: end_term without begin_term");
    in_term_ = false;
    const std::uint32_t df = static_cast<std::uint32_t>(docs_.size());
    if (df == 0) throw std::runtime_error("segment writer: term without documents");
    TermInfo ti{};
    ti.df = df;
    ti.pos_off = term_pos_off_;
    ti.max_tf = *std::max_element(tfs_.begin(), tfs_.end());
    ti.min_len = 0xFFFFFFFFu;
    for (std::uint32_t d : docs_) ti.min_len = std::min(ti.min_len, doc_len_[d]);
    if (df == 1) {
        ti.n_blocks = 0;
        ti.post_off = docs_[0];
    } else {
        align8(post_);
        ti.n_blocks = (df + BLOCK - 1) / BLOCK;
        ti.post_off = post_.size();
        std::vector<BlockMeta> meta(ti.n_blocks);
        std::string data;
        std::uint32_t gaps[BLOCK], tfm1[BLOCK];
        std::int64_t prev = -1;
        for (std::uint32_t b = 0; b < ti.n_blocks; ++b) {
            const std::uint32_t at = b * BLOCK, n = std::min(BLOCK, df - at);
            std::uint32_t gmax = 0, tmax = 0, mx = 0, mn = 0xFFFFFFFFu;
            for (std::uint32_t k = 0; k < n; ++k) {
                gaps[k] = static_cast<std::uint32_t>(docs_[at + k] - prev - 1);
                prev = docs_[at + k];
                tfm1[k] = tfs_[at + k] - 1;
                gmax |= gaps[k];
                tmax |= tfm1[k];
                mx = std::max(mx, tfs_[at + k]);
                mn = std::min(mn, doc_len_[docs_[at + k]]);
            }
            const std::uint32_t db = bits_needed(gmax), tb = bits_needed(tmax);
            meta[b].last_doc = docs_[at + n - 1];
            meta[b].data_off = static_cast<std::uint32_t>(data.size());
            meta[b].max_tf = mx;
            meta[b].min_len = mn;
            meta[b].pos_off = block_pos_[b] - term_pos_off_;
            data.push_back(static_cast<char>(db));
            data.push_back(static_cast<char>(tb));
            pack_bits(data, gaps, n, db);
            pack_bits(data, tfm1, n, tb);
        }
        if (data.size() > 0xFFFFFFFFull) throw std::runtime_error("segment writer: postings of one term exceed 4 GB");
        append_raw(post_, meta.data(), meta.size());
        post_.append(data);
    }
    if (df >= BITMAP_MIN_DF) {
        roaring_bitmap_t *r = roaring_bitmap_create_with_capacity(df);
        roaring_bitmap_add_many(r, df, docs_.data());
        roaring_bitmap_run_optimize(r);
        const std::size_t len = roaring_bitmap_portable_size_in_bytes(r);
        align8(bitmaps_);
        BitmapEntry e{};
        e.term = static_cast<std::uint32_t>(info_.size());
        e.off = bitmaps_.size();
        e.len = len;
        bitmaps_.resize(bitmaps_.size() + len);
        roaring_bitmap_portable_serialize(r, &bitmaps_[e.off]);
        roaring_bitmap_free(r);
        bitmap_dir_.push_back(e);
    }
    info_.push_back(ti);
    pool_.append(last_term_);
    if (pool_.size() > 0xFFFFFFFFull) throw std::runtime_error("segment writer: term pool exceeds 4 GB");
    term_off_.push_back(static_cast<std::uint32_t>(pool_.size()));
    return static_cast<std::uint32_t>(info_.size() - 1);
}

void SegmentWriter::add_stem(std::string key, std::uint32_t term_id)
{
    stems_.emplace_back(std::move(key), term_id);
}

std::string SegmentWriter::finish()
{
    if (in_term_) throw std::runtime_error("segment writer: finish inside a term");
    const std::uint32_t n_docs = static_cast<std::uint32_t>(doc_ids_.size());
    const std::uint32_t n_terms = static_cast<std::uint32_t>(info_.size());
    for (const auto &s : stems_)
        if (s.second >= n_terms) throw std::runtime_error("segment writer: stem of an unknown term");

    // Stem table: keys ascending, term ids ascending inside a key, duplicates removed.
    std::sort(stems_.begin(), stems_.end());
    stems_.erase(std::unique(stems_.begin(), stems_.end()), stems_.end());
    std::vector<StemEntry> entries;
    std::string stem_pool;
    std::vector<std::uint32_t> stem_terms;
    for (std::size_t i = 0; i < stems_.size();) {
        std::size_t j = i;
        while (j < stems_.size() && stems_[j].first == stems_[i].first) ++j;
        StemEntry e{};
        e.key_off = static_cast<std::uint32_t>(stem_pool.size());
        e.key_len = static_cast<std::uint32_t>(stems_[i].first.size());
        e.first = static_cast<std::uint32_t>(stem_terms.size());
        e.count = static_cast<std::uint32_t>(j - i);
        stem_pool.append(stems_[i].first);
        for (std::size_t k = i; k < j; ++k) stem_terms.push_back(stems_[k].second);
        entries.push_back(e);
        i = j;
    }

    // Assemble the file.
    std::uint64_t sum_len = 0, nonempty = 0;
    std::vector<std::uint8_t> norms(n_docs);
    for (std::uint32_t d = 0; d < n_docs; ++d) {
        sum_len += doc_len_[d];
        nonempty += doc_len_[d] > 0;
        norms[d] = length_to_norm(doc_len_[d]);
    }
    SegmentHeader h{};
    std::memcpy(h.magic, SEGMENT_MAGIC, 8);
    h.format = SEGMENT_FORMAT;
    h.segment_id = segment_id_;
    h.n_docs = n_docs;
    h.n_terms = n_terms;
    h.sum_len = sum_len;
    h.n_stems = static_cast<std::uint32_t>(entries.size());
    h.n_nonempty = static_cast<std::uint32_t>(nonempty);
    h.max_ver = max_ver_;

    std::string out(HEADER_BYTES, '\0');
    auto section = [&](int s, const void *p, std::size_t bytes, std::size_t pad_after = 0) {
        align8(out);
        h.off[s] = out.size();
        h.len[s] = bytes;
        out.append(static_cast<const char *>(p), bytes);
        out.append(pad_after, '\0');
    };
    const char empty = 0;
    section(S_DOC_IDS, doc_ids_.empty() ? &empty : static_cast<const void *>(doc_ids_.data()), 8ull * n_docs);
    section(S_DOC_LEN, doc_len_.empty() ? &empty : static_cast<const void *>(doc_len_.data()), 4ull * n_docs);
    section(S_NORMS, norms.empty() ? &empty : static_cast<const void *>(norms.data()), n_docs);
    section(S_TERM_OFF, term_off_.data(), 4ull * term_off_.size());
    section(S_TERM_POOL, pool_.data(), pool_.size());
    section(S_TERM_INFO, info_.empty() ? &empty : static_cast<const void *>(info_.data()), sizeof(TermInfo) * info_.size());
    // union_df is filled in below, once the postings are in place
    section(S_STEM_ENTRY, entries.empty() ? &empty : static_cast<const void *>(entries.data()), sizeof(StemEntry) * entries.size());
    section(S_STEM_POOL, stem_pool.data(), stem_pool.size());
    section(S_STEM_TERMS, stem_terms.empty() ? &empty : static_cast<const void *>(stem_terms.data()), 4ull * stem_terms.size());
    section(S_POSTINGS, post_.data(), post_.size(), TAIL_PAD);
    section(S_POSITIONS, pos_.data(), pos_.size());
    section(S_BITMAP_DIR, bitmap_dir_.empty() ? &empty : static_cast<const void *>(bitmap_dir_.data()), sizeof(BitmapEntry) * bitmap_dir_.size());
    section(S_BITMAPS, bitmaps_.data(), bitmaps_.size());
    align8(out);
    h.total_bytes = out.size();
    std::memcpy(&out[0], &h, sizeof(h));

    // Documents per stem group: the union of its terms' postings.
    {
        Segment seg(out.data(), out.size());
        StemEntry *se = reinterpret_cast<StemEntry *>(&out[h.off[S_STEM_ENTRY]]);
        std::vector<std::uint32_t> stamp(n_docs, 0);
        for (std::uint32_t i = 0; i < entries.size(); ++i) {
            if (se[i].count == 1) { se[i].union_df = info_[stem_terms[se[i].first]].df; continue; }
            std::uint32_t n = 0;
            for (std::uint32_t k = 0; k < se[i].count; ++k) {
                PostingIter it(seg, stem_terms[se[i].first + k]);
                for (std::uint32_t d = it.next(); d != NO_DOC; d = it.next())
                    if (stamp[d] != i + 1) { stamp[d] = i + 1; ++n; }
            }
            se[i].union_df = n;
        }
    }
    h.checksum = checksum_words(out.data(), out.size(), 0);
    std::memcpy(&out[0], &h, sizeof(h));
    return out;
}

} // namespace vtext
