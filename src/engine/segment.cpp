#include "segment.h"

#include <algorithm>
#include <vector>

namespace vtext {

namespace {

// Lucene SmallFloat (org.apache.lucene.util.SmallFloat), used by BM25Similarity for norms.
int long_to_int4(std::uint64_t i)
{
    const int num_bits = 64 - __builtin_clzll(i | 1);   // i == 0 gives 1, the same as Lucene's subnormal path
    if (i < 8 || num_bits < 4) return static_cast<int>(i);
    const int shift = num_bits - 4;
    int encoded = static_cast<int>(i >> shift);
    encoded &= 0x07;
    encoded |= (shift + 1) << 3;
    return encoded;
}

std::uint64_t int4_to_long(int i)
{
    const std::uint64_t bits = static_cast<std::uint64_t>(i & 0x07);
    const int shift = (i >> 3) - 1;
    if (shift == -1) return bits;
    return (bits | 0x08) << shift;
}

const int MAX_INT4 = long_to_int4(0x7FFFFFFF);
const int NUM_FREE_VALUES = 255 - MAX_INT4;          // 24

[[noreturn]] void bad(const std::string &why) { throw std::runtime_error("damaged segment: " + why); }

} // namespace

std::uint8_t length_to_norm(std::uint32_t len)
{
    if (len < static_cast<std::uint32_t>(NUM_FREE_VALUES)) return static_cast<std::uint8_t>(len);
    return static_cast<std::uint8_t>(NUM_FREE_VALUES + long_to_int4(len - NUM_FREE_VALUES));
}

std::uint32_t norm_to_length(std::uint8_t norm)
{
    if (norm < NUM_FREE_VALUES) return norm;
    return static_cast<std::uint32_t>(NUM_FREE_VALUES + int4_to_long(norm - NUM_FREE_VALUES));
}

Impacts make_impacts(std::vector<std::pair<std::uint32_t, std::uint8_t>> pairs)
{
    // Pareto frontier: shortest first; a pair stays when its tf beats every shorter one
    std::sort(pairs.begin(), pairs.end(), [](const auto &a, const auto &b) {
        return a.second != b.second ? a.second < b.second : a.first > b.first;
    });
    std::vector<std::pair<std::uint32_t, std::uint8_t>> f;
    for (const auto &p : pairs)
        if (f.empty() || p.first > f.back().first) f.push_back(p);
    Impacts imp{};
    if (f.size() <= static_cast<std::size_t>(MAX_IMPACTS)) {
        imp.n = static_cast<std::uint8_t>(f.size());
        for (std::size_t i = 0; i < f.size(); ++i) { imp.tf[i] = f[i].first; imp.norm[i] = f[i].second; }
        return imp;
    }
    // more pairs: MAX_IMPACTS groups of neighbours, each (its largest tf, its smallest norm)
    imp.n = MAX_IMPACTS;
    for (int g = 0; g < MAX_IMPACTS; ++g) {
        const std::size_t a = f.size() * g / MAX_IMPACTS, b = f.size() * (g + 1) / MAX_IMPACTS - 1;
        imp.tf[g] = f[b].first;
        imp.norm[g] = f[a].second;
    }
    return imp;
}

std::string stem_key(std::string_view lang_code, std::string_view stem)
{
    std::string k;
    k.reserve(lang_code.size() + 1 + stem.size());
    k.append(lang_code);
    k.push_back('\x01');
    k.append(stem);
    return k;
}

std::uint64_t checksum_words(const void *p, std::size_t bytes, std::uint64_t first_word)
{
    const std::uint8_t *b = static_cast<const std::uint8_t *>(p);
    std::uint64_t sum = 0;
    const std::size_t words = bytes / 8;
    for (std::size_t k = 0; k < words; ++k) {
        std::uint64_t w;
        std::memcpy(&w, b + 8 * k, 8);
        const std::uint64_t i = first_word + k;
        if (w == 0 || i == 5) continue;
        std::uint64_t z = w + (i + 1) * 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        sum ^= z ^ (z >> 31);
    }
    if (bytes % 8) {
        std::uint64_t w = 0;
        std::memcpy(&w, b + 8 * words, bytes % 8);
        const std::uint64_t i = first_word + words;
        if (w != 0 && i != 5) {
            std::uint64_t z = w + (i + 1) * 0x9E3779B97F4A7C15ull;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            sum ^= z ^ (z >> 31);
        }
    }
    return sum;
}

void pack_bits(std::string &out, const std::uint32_t *v, std::uint32_t n, std::uint32_t bits)
{
    const std::size_t at = out.size();
    out.resize(at + packed_bytes(n, bits), '\0');
    if (bits == 0) return;
    std::uint8_t *p = reinterpret_cast<std::uint8_t *>(&out[at]);
    std::uint64_t acc = 0;
    std::uint32_t have = 0;
    std::size_t o = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        acc |= static_cast<std::uint64_t>(v[i]) << have;
        have += bits;
        while (have >= 8) { p[o++] = static_cast<std::uint8_t>(acc); acc >>= 8; have -= 8; }
    }
    if (have) p[o++] = static_cast<std::uint8_t>(acc);
}

namespace {

// One width known at compile time: 8 values (8 * B bits = B bytes) per step, so every shift is a
// constant and the loop unrolls. Reads 8 bytes at a time like the generic loop (the format pads).
template <unsigned B> void unpack_fixed(const std::uint8_t *p, std::uint32_t n, std::uint32_t *out)
{
    constexpr std::uint64_t mask = B == 32 ? 0xFFFFFFFFull : ((1ull << B) - 1);
    std::uint32_t i = 0;
    for (; i + 8 <= n; i += 8, p += B) {
        for (unsigned k = 0; k < 8; ++k) {
            std::uint64_t w;
            std::memcpy(&w, p + (k * B) / 8, 8);
            out[i + k] = static_cast<std::uint32_t>((w >> ((k * B) % 8)) & mask);
        }
    }
    for (std::uint64_t bitpos = 0; i < n; ++i, bitpos += B) {
        std::uint64_t w;
        std::memcpy(&w, p + (bitpos >> 3), 8);
        out[i] = static_cast<std::uint32_t>((w >> (bitpos & 7)) & mask);
    }
}

template <unsigned... Bs> struct UnpackTable {
    static constexpr void (*fns[sizeof...(Bs)])(const std::uint8_t *, std::uint32_t, std::uint32_t *) = {&unpack_fixed<Bs>...};
};
template <unsigned... Bs> constexpr void (*UnpackTable<Bs...>::fns[sizeof...(Bs)])(const std::uint8_t *, std::uint32_t, std::uint32_t *);
using Unpackers = UnpackTable<1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32>;

} // namespace

void unpack_bits(const std::uint8_t *p, std::uint32_t n, std::uint32_t bits, std::uint32_t *out)
{
    if (bits == 0) { std::fill(out, out + n, 0u); return; }
    Unpackers::fns[bits - 1](p, n, out);
}

Segment::Segment(const void *data, std::size_t size) : base_(static_cast<const std::uint8_t *>(data)), size_(size)
{
    if (size < HEADER_BYTES) bad("shorter than the header");
    const SegmentHeader *h = reinterpret_cast<const SegmentHeader *>(base_);
    if (std::memcmp(h->magic, SEGMENT_MAGIC, 8) != 0) bad("not a vtext segment (magic)");
    if (h->format != SEGMENT_FORMAT && h->format != SEGMENT_FORMAT_ORDERED)
        bad("format " + std::to_string(h->format) + ", expected " + std::to_string(SEGMENT_FORMAT) + " or " + std::to_string(SEGMENT_FORMAT_ORDERED));
    if (h->total_bytes != size) bad("size " + std::to_string(size) + " differs from header " + std::to_string(h->total_bytes));
    for (int s = 0; s < S_COUNT; ++s) {
        if (h->off[s] % 8 || h->off[s] < HEADER_BYTES || h->off[s] > size || h->len[s] > size - h->off[s])
            bad("section " + std::to_string(s) + " out of bounds");
    }
    const std::uint64_t nd = h->n_docs, nt = h->n_terms, ns = h->n_stems;
    if (h->len[S_DOC_IDS] != 8 * nd || h->len[S_DOC_LEN] != 4 * nd || h->len[S_NORMS] != nd ||
        h->len[S_TERM_OFF] != 4 * (nt + 1) || h->len[S_TERM_INFO] != sizeof(TermInfo) * nt ||
        h->len[S_STEM_ENTRY] != sizeof(StemEntry) * ns || h->len[S_GROUP_INFO] != sizeof(TermInfo) * ns)
        bad("section sizes do not match the counts");
    if (h->off[S_POSTINGS] + h->len[S_POSTINGS] + TAIL_PAD > size) bad("postings padding missing");
    if (h->format == SEGMENT_FORMAT_ORDERED) {
        if (h->order_off % 8 || h->order_off < HEADER_BYTES || h->order_off > size || h->order_len != 4 * nd ||
            h->order_len > size - h->order_off)
            bad("doc_order section out of bounds");
        order_ = reinterpret_cast<const std::uint32_t *>(base_ + h->order_off);
    } else if (h->order_off || h->order_len) {
        bad("doc_order section in a format 4 segment");
    }
    if (h->len[S_BITMAP_DIR] % sizeof(BitmapEntry)) bad("bitmap directory size");
    h_ = h;
    doc_ids_ = reinterpret_cast<const std::int64_t *>(base_ + h->off[S_DOC_IDS]);
    doc_len_ = reinterpret_cast<const std::uint32_t *>(base_ + h->off[S_DOC_LEN]);
    norms_ = base_ + h->off[S_NORMS];
    term_off_ = reinterpret_cast<const std::uint32_t *>(base_ + h->off[S_TERM_OFF]);
    pool_ = reinterpret_cast<const char *>(base_ + h->off[S_TERM_POOL]);
    info_ = reinterpret_cast<const TermInfo *>(base_ + h->off[S_TERM_INFO]);
    stems_ = reinterpret_cast<const StemEntry *>(base_ + h->off[S_STEM_ENTRY]);
    stem_pool_ = reinterpret_cast<const char *>(base_ + h->off[S_STEM_POOL]);
    stem_terms_ = reinterpret_cast<const std::uint32_t *>(base_ + h->off[S_STEM_TERMS]);
    post_ = base_ + h->off[S_POSTINGS];
    pos_ = base_ + h->off[S_POSITIONS];
    groups_ = reinterpret_cast<const TermInfo *>(base_ + h->off[S_GROUP_INFO]);
    bitmap_dir_ = reinterpret_cast<const BitmapEntry *>(base_ + h->off[S_BITMAP_DIR]);
    n_bitmaps_ = h->len[S_BITMAP_DIR] / sizeof(BitmapEntry);
    bitmaps_ = base_ + h->off[S_BITMAPS];
    for (std::size_t i = 0; i < n_bitmaps_; ++i)
        if (bitmap_dir_[i].term >= nt || bitmap_dir_[i].off + bitmap_dir_[i].len > h->len[S_BITMAPS] ||
            (i > 0 && bitmap_dir_[i - 1].term >= bitmap_dir_[i].term))
            bad("bitmap directory entry " + std::to_string(i));
    if (term_off_[0] != 0 || term_off_[nt] != h->len[S_TERM_POOL]) bad("term offsets");
}

std::int64_t Segment::find_doc(std::int64_t doc_id) const
{
    if (order_) {
        const std::uint32_t *e = order_ + h_->n_docs;
        const std::uint32_t *p = std::lower_bound(order_, e, doc_id, [this](std::uint32_t d, std::int64_t id) { return doc_ids_[d] < id; });
        return p != e && doc_ids_[*p] == doc_id ? static_cast<std::int64_t>(*p) : -1;
    }
    const std::int64_t *e = doc_ids_ + h_->n_docs;
    const std::int64_t *p = std::lower_bound(doc_ids_, e, doc_id);
    return p != e && *p == doc_id ? p - doc_ids_ : -1;
}

void Segment::find_docs(const std::int64_t *ids, std::size_t n, std::vector<std::uint32_t> &out) const
{
    const std::uint32_t nd = h_->n_docs;
    if (n * 32 < nd) {
        for (std::size_t i = 0; i < n; ++i) { const std::int64_t d = find_doc(ids[i]); if (d >= 0) out.push_back(static_cast<std::uint32_t>(d)); }
        return;
    }
    std::size_t j = 0;
    for (std::uint32_t i = 0; i < nd && j < n; ++i) {
        const std::uint32_t d = order_ ? order_[i] : i;
        const std::int64_t id = doc_ids_[d];
        while (j < n && ids[j] < id) ++j;
        if (j < n && ids[j] == id) { out.push_back(d); ++j; }
    }
}

std::uint32_t Segment::lower_bound(std::string_view t) const
{
    std::uint32_t lo = 0, hi = h_->n_terms;
    while (lo < hi) {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        if (term(mid) < t) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

std::int64_t Segment::find_term(std::string_view t) const
{
    const std::uint32_t i = lower_bound(t);
    return i < h_->n_terms && term(i) == t ? static_cast<std::int64_t>(i) : -1;
}

std::string_view Segment::stem_key_at(std::uint32_t i) const
{
    return std::string_view(stem_pool_ + stems_[i].key_off, stems_[i].key_len);
}

const std::uint32_t *Segment::stem_terms_at(std::uint32_t i, std::uint32_t &count) const
{
    count = stems_[i].count;
    return stem_terms_ + stems_[i].first;
}

const std::uint32_t *Segment::stem_terms(std::string_view key, std::uint32_t &count) const
{
    std::uint32_t lo = 0, hi = h_->n_stems;
    while (lo < hi) {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        if (stem_key_at(mid) < key) lo = mid + 1;
        else hi = mid;
    }
    if (lo < h_->n_stems && stem_key_at(lo) == key) return stem_terms_at(lo, count);
    count = 0;
    return nullptr;
}

std::int64_t Segment::find_stem(std::string_view key) const
{
    std::uint32_t lo = 0, hi = h_->n_stems;
    while (lo < hi) {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        if (stem_key_at(mid) < key) lo = mid + 1;
        else hi = mid;
    }
    return lo < h_->n_stems && stem_key_at(lo) == key ? static_cast<std::int64_t>(lo) : -1;
}

const std::uint8_t *Segment::bitmap(std::uint32_t term, std::size_t &len) const
{
    const BitmapEntry *b = bitmap_dir_, *e = bitmap_dir_ + n_bitmaps_;
    const BitmapEntry *p = std::lower_bound(b, e, term, [](const BitmapEntry &x, std::uint32_t t) { return x.term < t; });
    if (p == e || p->term != term) { len = 0; return nullptr; }
    len = p->len;
    return bitmaps_ + p->off;
}

void Segment::verify() const
{
    const SegmentHeader &h = *h_;
    const std::uint64_t sum = checksum_words(base_, size_, 0);
    if (sum != h.checksum) bad("checksum mismatch");
    if (order_) {
        // a permutation of the docnos, by ascending doc_id
        std::vector<bool> seen(h.n_docs, false);
        for (std::uint32_t i = 0; i < h.n_docs; ++i) {
            if (order_[i] >= h.n_docs || seen[order_[i]]) bad("doc_order is not a permutation at " + std::to_string(i));
            seen[order_[i]] = true;
            if (i > 0 && doc_ids_[order_[i - 1]] >= doc_ids_[order_[i]]) bad("doc_order not by ascending doc id at " + std::to_string(i));
        }
    } else {
        for (std::uint32_t d = 1; d < h.n_docs; ++d)
            if (doc_ids_[d - 1] >= doc_ids_[d]) bad("doc ids not ascending at docno " + std::to_string(d));
    }
    std::uint64_t total = 0, nonempty = 0;
    for (std::uint32_t d = 0; d < h.n_docs; ++d) {
        total += doc_len_[d];
        nonempty += doc_len_[d] > 0;
        if (norms_[d] != length_to_norm(doc_len_[d])) bad("norm of docno " + std::to_string(d));
    }
    if (total != h.sum_len || nonempty != h.n_nonempty) bad("document lengths do not add up");
    std::uint64_t postings_total = 0;
    std::vector<std::uint32_t> docs(BLOCK), tfs(BLOCK);
    for (std::uint32_t t = 0; t < h.n_terms; ++t) {
        if (t > 0 && !(term(t - 1) < term(t))) bad("terms not ascending at " + std::to_string(t));
        const TermInfo &ti = info_[t];
        if (ti.df == 0 || ti.df > h.n_docs) bad("df of term " + std::to_string(t));
        if (ti.pos_off > h.len[S_POSITIONS]) bad("positions offset of term " + std::to_string(t));
        if (verify_postings(ti, t)) postings_total += ti.df;
    }
    for (std::uint32_t g = 0; g < h.n_stems; ++g) {
        if (groups_[g].df != stems_[g].union_df) bad("postings of stem group " + std::to_string(g));
        if (stems_[g].count > 1) verify_postings(groups_[g], g);
    }
    (void)postings_total;
}

bool Segment::verify_postings(const TermInfo &ti, std::uint32_t t) const
{
    const SegmentHeader &h = *h_;
    std::vector<std::uint32_t> docs(BLOCK), tfs(BLOCK);
    {
        if (ti.df == 1) {
            if (ti.n_blocks != 0 || ti.post_off >= h.n_docs || ti.max_tf == 0) bad("single posting of term " + std::to_string(t));
            return true;
        }
        if (ti.n_blocks != (ti.df + BLOCK - 1) / BLOCK) bad("block count of term " + std::to_string(t));
        if (ti.post_off + sizeof(Impacts) + ti.n_blocks * sizeof(BlockMeta) + l1_count(ti.n_blocks) * sizeof(L1Meta) > h.len[S_POSTINGS])
            bad("postings offset of term " + std::to_string(t));
        const Impacts *ti_imp = reinterpret_cast<const Impacts *>(post_ + ti.post_off);
        if (ti_imp->n == 0 || ti_imp->n > MAX_IMPACTS) bad("impacts of term " + std::to_string(t));
        const BlockMeta *bm = reinterpret_cast<const BlockMeta *>(post_ + ti.post_off + sizeof(Impacts));
        const L1Meta *l1 = reinterpret_cast<const L1Meta *>(post_ + ti.post_off + sizeof(Impacts) + ti.n_blocks * sizeof(BlockMeta));
        for (std::uint32_t g = 0; g < l1_count(ti.n_blocks); ++g) {
            const std::uint32_t last = std::min(ti.n_blocks, (g + 1) * L1_BLOCKS) - 1;
            if (l1[g].last_doc != bm[last].last_doc || l1[g].imp.n == 0 || l1[g].imp.n > MAX_IMPACTS) bad("level-1 impacts of term " + std::to_string(t));
            for (std::uint32_t b = g * L1_BLOCKS; b <= last; ++b)       // every block's pairs are covered by the group's
                for (int k = 0; k < bm[b].imp.n; ++k) {
                    bool covered = false;
                    for (int j = 0; j < l1[g].imp.n && !covered; ++j) covered = l1[g].imp.tf[j] >= bm[b].imp.tf[k] && l1[g].imp.norm[j] <= bm[b].imp.norm[k];
                    if (!covered) bad("level-1 impacts of term " + std::to_string(t) + " do not cover a block");
                }
        }
        const std::uint8_t *data = post_ + ti.post_off + sizeof(Impacts) + ti.n_blocks * sizeof(BlockMeta) + l1_count(ti.n_blocks) * sizeof(L1Meta);
        std::int64_t prev = -1;
        for (std::uint32_t b = 0; b < ti.n_blocks; ++b) {
            const std::uint32_t n = b + 1 < ti.n_blocks ? BLOCK : ti.df - b * BLOCK;
            const std::uint8_t *p = data + bm[b].data_off;
            if (p + 2 > post_ + h.len[S_POSTINGS]) bad("block data of term " + std::to_string(t));
            const std::uint32_t db = p[0], tb = p[1];
            if (db > 32 || tb > 32) bad("bit widths of term " + std::to_string(t));
            if (p + 2 + packed_bytes(n, db) + packed_bytes(n, tb) > post_ + h.len[S_POSTINGS]) bad("block length of term " + std::to_string(t));
            unpack_bits(p + 2, n, db, docs.data());
            unpack_bits(p + 2 + packed_bytes(n, db), n, tb, tfs.data());
            const Impacts &im = bm[b].imp;
            if (im.n == 0 || im.n > MAX_IMPACTS) bad("impacts of a block in term " + std::to_string(t));
            for (std::uint32_t i = 0; i < n; ++i) {
                prev += static_cast<std::int64_t>(docs[i]) + 1;
                if (prev >= h.n_docs) bad("docno out of range in term " + std::to_string(t));
                // every document must be covered by a pair with tf >= its tf and norm <= its norm
                bool covered = false;
                for (int k = 0; k < im.n && !covered; ++k) covered = im.tf[k] >= tfs[i] + 1 && im.norm[k] <= norms_[prev];
                if (!covered) bad("impacts of a block in term " + std::to_string(t) + " do not cover a document");
            }
            if (static_cast<std::uint32_t>(prev) != bm[b].last_doc) bad("last docno of a block in term " + std::to_string(t));
        }
    }
    return true;
}

} // namespace vtext
