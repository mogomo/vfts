// One index segment in its binary form (format "VTEXTSG1", docs/format.md).
//
// The same bytes are the per-node cache file (mmap'ed, read in place) and the in-memory form of
// the live tail. They are made by SegmentWriter (segment_writer.h) from a SegmentBuilder (new
// documents), from the rows of the index tables (tload), or from other segments (merge).
//
// Inside a segment documents are numbered 0 .. n_docs-1 ("docno") in doc_id order, so a docno
// comparison is a doc_id comparison and ties in score are broken the same way in every segment.
#ifndef VTEXT_ENGINE_SEGMENT_H
#define VTEXT_ENGINE_SEGMENT_H

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "vtext segments are little-endian only"
#endif

namespace vtext {

constexpr char SEGMENT_MAGIC[8] = {'V', 'T', 'E', 'X', 'T', 'S', 'G', '1'};
constexpr std::uint32_t SEGMENT_FORMAT = 1;
constexpr std::uint32_t BLOCK = 128;                 // documents per postings block
constexpr std::size_t HEADER_BYTES = 512;
constexpr std::size_t TAIL_PAD = 16;                 // zero bytes after the postings for unaligned 8-byte reads

// Section order in the file; header.off[] holds their byte offsets.
enum Section {
    S_DOC_IDS = 0,      // int64[n_docs], ascending, unique
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
    std::uint8_t reserved[HEADER_BYTES - 72 - 16 * S_COUNT];
};
static_assert(sizeof(SegmentHeader) == HEADER_BYTES, "header size");

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

// Block b of a term holds documents b*128 .. b*128+127 of its list. Data at
// post_off + n_blocks*sizeof(BlockMeta) + data_off: one byte doc_bits, one byte tf_bits, then n
// values gap-1 packed in doc_bits each (gap = docno - previous docno; the previous docno of a
// block's first document is the last docno of the block before, or -1), then n values tf-1 packed
// in tf_bits each. Bits are packed LSB first. A run of consecutive docnos with tf 1 takes 0 bits.
struct BlockMeta {
    std::uint32_t last_doc;     // docno of the block's last document
    std::uint32_t data_off;     // byte offset of the block data after the BlockMeta array
    std::uint32_t max_tf;       // within the block
    std::uint32_t min_len;      // within the block
    std::uint64_t pos_off;      // offset of the block's first document's positions, from TermInfo::pos_off
};
static_assert(sizeof(BlockMeta) == 24, "BlockMeta size");

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
    const std::int64_t *doc_ids() const { return doc_ids_; }
    std::uint32_t doc_len(std::uint32_t docno) const { return doc_len_[docno]; }
    std::uint8_t norm(std::uint32_t docno) const { return norms_[docno]; }
    const std::uint8_t *norms() const { return norms_; }
    // The docno of a doc_id, or -1.
    std::int64_t find_doc(std::int64_t doc_id) const;

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
    // Index of a stem key in the stem table, or -1.
    std::int64_t find_stem(std::string_view key) const;

    // The stored bitmap of a term (CRoaring portable bytes), or nullptr.
    const std::uint8_t *bitmap(std::uint32_t term, std::size_t &len) const;

    const std::uint8_t *postings() const { return post_; }
    const std::uint8_t *positions() const { return pos_; }
    std::size_t positions_bytes() const { return h_->len[S_POSITIONS]; }

private:
    const std::uint8_t *base_ = nullptr;
    std::size_t size_ = 0;
    const SegmentHeader *h_ = nullptr;
    const std::int64_t *doc_ids_ = nullptr;
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
    const BitmapEntry *bitmap_dir_ = nullptr;
    std::size_t n_bitmaps_ = 0;
    const std::uint8_t *bitmaps_ = nullptr;
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

} // namespace vtext

#endif
