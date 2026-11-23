// One index segment in its binary form (format "VFTSSEG1", docs/format.md).
//
// The same bytes are the per-node cache file (mmap'ed, read in place) and the in-memory form of
// the live tail. They are made by SegmentWriter (segment_writer.h) from a SegmentBuilder (new
// documents), from the rows of the index tables (tload), or from other segments (merge).
//
// Inside a segment documents are numbered 0 .. n_docs-1 ("docno"). A segment made from new
// documents numbers them in doc_id order (format 4). A merge may number them in another order, which
// puts documents with similar words next to each other (reorder.h: smaller postings, longer runs
// that MaxScore can skip); such a segment (format 5) has one more section, the docnos in doc_id
// order, for finding a doc_id. Ties in score are broken by doc_id, never by docno (search.cpp).
#ifndef VFTS_ENGINE_SEGMENT_H
#define VFTS_ENGINE_SEGMENT_H

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "vfts segments are little-endian only"
#endif

namespace vfts {

constexpr char SEGMENT_MAGIC[8] = {'V', 'F', 'T', 'S', 'S', 'E', 'G', '1'};
constexpr std::uint32_t SEGMENT_FORMAT = 4;      // 2: impacts; 3: postings of stem groups; 4: level-1 impacts
constexpr std::uint32_t SEGMENT_FORMAT_ORDERED = 5;  // 4 + docnos in any order, and the doc_order section
constexpr std::uint32_t BLOCK = 128;                 // documents per postings block
constexpr std::size_t HEADER_BYTES = 512;
constexpr std::size_t TAIL_PAD = 16;                 // zero bytes after the postings for unaligned 8-byte reads

// Section order in the file; header.off[] holds their byte offsets.
enum Section {
    S_DOC_IDS = 0,      // int64[n_docs], unique; ascending in format 4
    S_DOC_LEN,          // uint32[n_docs], tokens per document
    S_NORMS,            // uint8[n_docs], Lucene's one-byte length (SmallFloat.intToByte4)
    S_TERM_OFF,         // uint32[n_terms + 1], offsets into S_TERM_POOL
    S_TERM_POOL,        // the terms, UTF-8, ascending byte order, no separators
    S_TERM_INFO,        // TermInfo[n_terms]
    S_STEM_ENTRY,       // StemEntry[n_stems], ascending by key
    S_STEM_POOL,        // the keys: "<lang>\x01<stem>"
    S_STEM_TERMS,       // uint32 term ids, grouped by stem entry
    S_POSTINGS,         // per term: BlockMeta[n_blocks] then the packed blocks (see below)
    S_POSITIONS,        // per term, per document in docno order: tf varints (position deltas)
    S_BITMAP_DIR,       // BitmapEntry[], ascending by term id: terms with a stored bitmap
    S_BITMAPS,          // CRoaring portable bitmaps of docnos
    S_GROUP_INFO,       // TermInfo[n_stems]: postings of each stem group (docs and summed tf, no
                        // positions: pos_off = NO_POSITIONS); for a group of one term, that term's
    S_COUNT
};

#pragma pack(push, 1)
struct SegmentHeader {
    char magic[8];
    std::uint32_t format;
    std::uint32_t flags;
    std::int64_t segment_id;
    std::uint32_t n_docs;
    std::uint32_t n_terms;
    std::uint64_t sum_len;          // all tokens of all documents
    std::uint64_t checksum;         // word 5 of the file (byte 40): see segment.cpp
    std::uint64_t total_bytes;
    std::uint32_t n_stems;
    std::uint32_t n_nonempty;       // documents with at least one token (they count for BM25)
    std::int64_t max_ver;           // newest source version in the segment (microseconds), informational
    std::uint64_t off[S_COUNT];     // byte offset of each section
    std::uint64_t len[S_COUNT];     // byte length of each section
    std::uint64_t order_off;        // format 5: the doc_order section, uint32[n_docs]: the docnos
    std::uint64_t order_len;        // sorted by doc_id (0 in format 4)
    std::uint8_t reserved[HEADER_BYTES - 72 - 16 * S_COUNT - 16];
};
static_assert(sizeof(SegmentHeader) == HEADER_BYTES, "header size");

constexpr std::uint64_t NO_POSITIONS = ~0ull;

// A term with df == 1 has no blocks: post_off holds its docno and max_tf its tf.
struct TermInfo {
    std::uint32_t df;           // documents that contain the term
    std::uint32_t n_blocks;     // ceil(df / BLOCK), 0 when df == 1
    std::uint64_t post_off;     // offset in S_POSTINGS of the term's BlockMeta array (or the docno)
    std::uint64_t pos_off;      // offset in S_POSITIONS of the term's positions
    std::uint32_t max_tf;       // highest tf of the term
    std::uint32_t min_len;      // shortest document that contains it (for score upper bounds)
};
static_assert(sizeof(TermInfo) == 32, "TermInfo size");

// Competitive (tf, norm) pairs of a set of documents (Lucene's "impacts"): every document of the
// set scores at most the best of these pairs, because BM25 grows with tf and falls with the length.
// Pairs are the Pareto frontier (norm ascending, tf ascending); when it has more than MAX_IMPACTS
// pairs, neighbours are merged into (larger tf, smaller norm), which keeps the bound valid.
constexpr int MAX_IMPACTS = 4;
struct Impacts {
    std::uint8_t n;
    std::uint8_t norm[MAX_IMPACTS];
    std::uint8_t pad[3];
    std::uint32_t tf[MAX_IMPACTS];
};
static_assert(sizeof(Impacts) == 24, "Impacts size");

// A term with blocks: at post_off its Impacts over all documents, then BlockMeta[n_blocks], then
// L1Meta[l1_count(n_blocks)], then the block data (data_off counts from the end of the L1Meta array). Block b holds documents b*128 .. b*128+127 of the term's list. Block data at
// post_off + sizeof(Impacts) + n_blocks*sizeof(BlockMeta) + data_off: one byte doc_bits, one byte
// tf_bits, then n values gap-1 packed in doc_bits each (gap = docno - previous docno; the previous
// docno of a block's first document is the last docno of the block before, or -1), then n values
// tf-1 packed in tf_bits each. Bits are packed LSB first. A run of consecutive docnos with tf 1
// takes 0 bits.
struct BlockMeta {
    std::uint32_t last_doc;     // docno of the block's last document
    std::uint32_t data_off;     // byte offset of the block data after the BlockMeta array
    std::uint64_t pos_off;      // offset of the block's first document's positions, from TermInfo::pos_off
    Impacts imp;                // of the block's documents
};
static_assert(sizeof(BlockMeta) == 40, "BlockMeta size");

// Level-1 impacts: one per L1_BLOCKS blocks of a term, after its BlockMeta array, so that the bound
// of a long range of documents needs one entry per 4,096 postings instead of one per block.
constexpr std::uint32_t L1_BLOCKS = 32;
struct L1Meta {
    std::uint32_t last_doc;     // last docno of the group of blocks
    std::uint32_t pad;
    Impacts imp;                // of the group's documents
};
static_assert(sizeof(L1Meta) == 32, "L1Meta size");
inline std::uint32_t l1_count(std::uint32_t n_blocks) { return (n_blocks + L1_BLOCKS - 1) / L1_BLOCKS; }

// The impacts of (tf, norm) pairs.
Impacts make_impacts(std::vector<std::pair<std::uint32_t, std::uint8_t>> pairs);

struct StemEntry {
    std::uint32_t key_off;      // in S_STEM_POOL
    std::uint32_t key_len;
    std::uint32_t first;        // index into S_STEM_TERMS
    std::uint32_t count;
    std::uint32_t union_df;     // documents that contain at least one of the terms
    std::uint32_t reserved;
};
static_assert(sizeof(StemEntry) == 24, "StemEntry size");

// Terms with df >= BITMAP_MIN_DF also have their documents as a CRoaring bitmap (portable
// serialization), for counting and boolean filtering without decoding postings (tcount).
constexpr std::uint32_t BITMAP_MIN_DF = 2048;
struct BitmapEntry {
    std::uint32_t term;
    std::uint32_t reserved;
    std::uint64_t off;          // in S_BITMAPS
    std::uint64_t len;
};
static_assert(sizeof(BitmapEntry) == 24, "BitmapEntry size");
#pragma pack(pop)

// Lucene's SmallFloat.intToByte4 / byte4ToInt: the one-byte document length of BM25. Lengths below
// 24 are exact; above, 4 significant bits are kept.
std::uint8_t length_to_norm(std::uint32_t len);
std::uint32_t norm_to_length(std::uint8_t norm);

// The stem key of the stem table: "<lang>\x01<stem>".
std::string stem_key(std::string_view lang_code, std::string_view stem);

// The file checksum: XOR over the non-zero 8-byte words, mixed with their index (splitmix64),
// word 5 (the checksum field) excluded. The checksum of a file is the XOR of the checksums of its
// parts, so parts written separately combine. `first_word` is the index of p's first word.
std::uint64_t checksum_words(const void *p, std::size_t bytes, std::uint64_t first_word);

// A read-only view of a segment's bytes (an mmap'ed file or a buffer owned elsewhere).
class Segment {
public:
    Segment() = default;
    // Checks magic, format, sizes and section bounds (not the checksum: see verify()).
    Segment(const void *data, std::size_t size);

    // Full check: checksum, ordering of doc ids and terms, postings structure. Throws.
    void verify() const;

    bool valid() const { return h_ != nullptr; }
    const SegmentHeader &header() const { return *h_; }
    std::int64_t id() const { return h_->segment_id; }
    std::uint32_t n_docs() const { return h_->n_docs; }
    std::uint32_t n_terms() const { return h_->n_terms; }
    std::uint64_t sum_len() const { return h_->sum_len; }
    std::size_t bytes() const { return size_; }

    std::int64_t doc_id(std::uint32_t docno) const { return doc_ids_[docno]; }
    // True when docnos follow doc_id order (format 4).
    bool in_doc_id_order() const { return order_ == nullptr; }
    const std::int64_t *doc_ids() const { return doc_ids_; }
    std::uint32_t doc_len(std::uint32_t docno) const { return doc_len_[docno]; }
    std::uint8_t norm(std::uint32_t docno) const { return norms_[docno]; }
    const std::uint8_t *norms() const { return norms_; }
    // The docno of a doc_id, or -1.
    std::int64_t find_doc(std::int64_t doc_id) const;
    // The docnos of many doc ids (ascending, unique), appended to out; ids not in the segment are
    // skipped. One pass over the segment's doc ids when there are many (no random lookups).
    void find_docs(const std::int64_t *ids, std::size_t n, std::vector<std::uint32_t> &out) const;

    std::string_view term(std::uint32_t t) const
    {
        return std::string_view(pool_ + term_off_[t], term_off_[t + 1] - term_off_[t]);
    }
    const TermInfo &info(std::uint32_t t) const { return info_[t]; }
    // Term id, or -1.
    std::int64_t find_term(std::string_view term) const;
    // First term id >= term (n_terms if none).
    std::uint32_t lower_bound(std::string_view term) const;

    // The term ids of a stem key (stem_key()); count 0 if the key is not there.
    const std::uint32_t *stem_terms(std::string_view key, std::uint32_t &count) const;
    std::uint32_t n_stems() const { return h_->n_stems; }
    std::string_view stem_key_at(std::uint32_t i) const;
    const std::uint32_t *stem_terms_at(std::uint32_t i, std::uint32_t &count) const;

    std::uint32_t stem_union_df(std::uint32_t i) const { return stems_[i].union_df; }
    // The postings of stem group i (summed tf, no positions; for one term, the term's).
    const TermInfo &group_info(std::uint32_t i) const { return groups_[i]; }
    // Index of a stem key in the stem table, or -1.
    std::int64_t find_stem(std::string_view key) const;

    // The stored bitmap of a term (CRoaring portable bytes), or nullptr.
    const std::uint8_t *bitmap(std::uint32_t term, std::size_t &len) const;

    // Lists of many documents as bit sets (one bit per docno), made on first use by counting and kept
    // while the segment is mapped (Lucene stores the dense blocks of a list as bit sets in its index).
    // Key: the list's post_off (a term's or a stem group's). nullptr: not made yet.
    using DenseBits = std::shared_ptr<const std::vector<std::uint64_t>>;
    DenseBits dense(std::uint64_t key) const
    {
        std::lock_guard<std::mutex> l(dense_->mu);
        auto it = dense_->bits.find(key);
        return it == dense_->bits.end() ? nullptr : it->second;
    }
    // Keeps b while all kept sets of the process stay within budget bytes (else b serves one search
    // only); returns the kept one (another thread may have been first).
    DenseBits keep_dense(std::uint64_t key, DenseBits b, std::size_t budget) const
    {
        std::lock_guard<std::mutex> l(dense_->mu);
        auto it = dense_->bits.find(key);
        if (it != dense_->bits.end()) return it->second;
        const std::size_t sz = b->size() * sizeof(std::uint64_t);
        if (dense_bytes().fetch_add(sz) + sz > budget) { dense_bytes().fetch_sub(sz); return b; }
        dense_->bytes += sz;
        return dense_->bits.emplace(key, std::move(b)).first->second;
    }
    // Bytes of the dense sets kept by all segments of the process.
    static std::atomic<std::size_t> &dense_bytes()
    {
        static std::atomic<std::size_t> n{0};
        return n;
    }

    const std::uint8_t *postings() const { return post_; }
    const std::uint8_t *positions() const { return pos_; }
    std::size_t positions_bytes() const { return h_->len[S_POSITIONS]; }

private:
    bool verify_postings(const TermInfo &ti, std::uint32_t t) const;
    const std::uint8_t *base_ = nullptr;
    std::size_t size_ = 0;
    const SegmentHeader *h_ = nullptr;
    const std::int64_t *doc_ids_ = nullptr;
    const std::uint32_t *order_ = nullptr;      // format 5: docnos by doc_id
    const std::uint32_t *doc_len_ = nullptr;
    const std::uint8_t *norms_ = nullptr;
    const std::uint32_t *term_off_ = nullptr;
    const char *pool_ = nullptr;
    const TermInfo *info_ = nullptr;
    const StemEntry *stems_ = nullptr;
    const char *stem_pool_ = nullptr;
    const std::uint32_t *stem_terms_ = nullptr;
    const std::uint8_t *post_ = nullptr;
    const std::uint8_t *pos_ = nullptr;
    const TermInfo *groups_ = nullptr;
    const BitmapEntry *bitmap_dir_ = nullptr;
    std::size_t n_bitmaps_ = 0;
    const std::uint8_t *bitmaps_ = nullptr;
    struct DenseStore {
        std::mutex mu;
        std::unordered_map<std::uint64_t, DenseBits> bits;
        std::size_t bytes = 0;
        ~DenseStore() { dense_bytes().fetch_sub(bytes); }
    };
    std::shared_ptr<DenseStore> dense_ = std::make_shared<DenseStore>();
};

// ---- small codecs shared by writer and readers

inline void put_varint(std::string &out, std::uint32_t v)
{
    while (v >= 0x80) { out.push_back(static_cast<char>(v | 0x80)); v >>= 7; }
    out.push_back(static_cast<char>(v));
}

inline std::uint32_t get_varint(const std::uint8_t *&p)
{
    std::uint32_t v = *p & 0x7F;
    if (*p++ < 0x80) return v;
    v |= static_cast<std::uint32_t>(*p & 0x7F) << 7;
    if (*p++ < 0x80) return v;
    v |= static_cast<std::uint32_t>(*p & 0x7F) << 14;
    if (*p++ < 0x80) return v;
    v |= static_cast<std::uint32_t>(*p & 0x7F) << 21;
    if (*p++ < 0x80) return v;
    v |= static_cast<std::uint32_t>(*p++) << 28;
    return v;
}

inline void skip_varints(const std::uint8_t *&p, std::uint32_t n)
{
    while (n) { if (*p++ < 0x80) --n; }
}

inline std::uint32_t bits_needed(std::uint32_t v) { return v ? 32 - static_cast<std::uint32_t>(__builtin_clz(v)) : 0; }

// Packs n values of `bits` bits each, LSB first, appending to out.
void pack_bits(std::string &out, const std::uint32_t *v, std::uint32_t n, std::uint32_t bits);
// Unpacks n values. Reads up to 8 bytes past the packed data (the segment pads S_POSTINGS).
void unpack_bits(const std::uint8_t *p, std::uint32_t n, std::uint32_t bits, std::uint32_t *out);
inline std::size_t packed_bytes(std::uint32_t n, std::uint32_t bits) { return (static_cast<std::size_t>(n) * bits + 7) / 8; }

} // namespace vfts

#endif
