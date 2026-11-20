#include "table_codec.h"
#include "postings.h"
#include "segment_writer.h"

#include <roaring.h>

#include <algorithm>
#include <stdexcept>

namespace vtext {

namespace {

std::string serialize(roaring_bitmap_t *r)
{
    roaring_bitmap_run_optimize(r);
    std::string out(roaring_bitmap_portable_size_in_bytes(r), '\0');
    roaring_bitmap_portable_serialize(r, &out[0]);
    return out;
}

} // namespace

void segment_to_rows(const Segment &seg, const RowSink &sink)
{
    if (sink.doc)
        for (std::uint32_t d = 0; d < seg.n_docs(); ++d) sink.doc({d, seg.doc_id(d), seg.doc_len(d)});
    if (sink.term) {
        for (std::uint32_t s = 0; s < seg.n_stems(); ++s) {
            const std::string_view key = seg.stem_key_at(s);
            const std::size_t cut = key.find('\x01');
            std::uint32_t count;
            const std::uint32_t *ids = seg.stem_terms_at(s, count);
            for (std::uint32_t k = 0; k < count; ++k) {
                TermDictRow r;
                r.term_id = ids[k];
                r.term.assign(seg.term(ids[k]));
                r.lang.assign(key.substr(0, cut));
                r.stem.assign(key.substr(cut + 1));
                r.doc_freq = seg.info(ids[k]).df;
                sink.term(r);
            }
        }
    }
    if (sink.posting) {
        std::vector<std::uint32_t> pos;
        for (std::uint32_t t = 0; t < seg.n_terms(); ++t) {
            PostingIter it(seg, t);
            std::uint32_t d = it.next();
            while (d != NO_DOC) {
                const std::uint32_t chunk = d >> CHUNK_BITS;
                roaring_bitmap_t *r = roaring_bitmap_create();
                PostingRow row;
                row.term_id = t;
                row.chunk_id = chunk;
                while (d != NO_DOC && (d >> CHUNK_BITS) == chunk) {
                    roaring_bitmap_add(r, d & 0xFFFFu);
                    const std::uint32_t tf = it.tf();
                    put_varint(row.payload, tf);
                    pos.clear();
                    it.positions(pos);
                    std::uint32_t prev = 0;
                    for (std::uint32_t p : pos) { put_varint(row.payload, p - prev); prev = p; }
                    d = it.next();
                }
                row.docs = serialize(r);
                roaring_bitmap_free(r);
                sink.posting(row);
            }
        }
    }
}

void SegmentAssembler::add(const TermDictRow &r)
{
    Term &t = terms_[r.term_id];
    if (t.term.empty()) { t.term = r.term; memory_ += r.term.size() + 64; }
    else if (t.term != r.term) throw std::runtime_error("term_dict: term id " + std::to_string(r.term_id) + " has two terms");
    t.df = r.doc_freq;
    t.stems.emplace_back(r.lang, r.stem);
    memory_ += r.stem.size() + r.lang.size() + 64;
}

void SegmentAssembler::add(PostingRow &&r)
{
    auto key = std::make_pair(r.term_id, r.chunk_id);
    auto it = postings_.find(key);
    if (it == postings_.end()) {
        memory_ += r.docs.size() + r.payload.size() + 96;
        postings_.emplace(key, std::move(r));
        return;
    }
    // postings and positions arrive as separate rows: merge them
    if (it->second.docs.empty()) it->second.docs = std::move(r.docs);
    else if (!r.docs.empty()) throw std::runtime_error("postings: two rows for one term and chunk");
    if (it->second.payload.empty()) it->second.payload = std::move(r.payload);
    else if (!r.payload.empty()) throw std::runtime_error("positions: two rows for one term and chunk");
    memory_ += it->second.docs.size() + it->second.payload.size();
}

void SegmentAssembler::add(const DocRow &r) { docs_.push_back(r); memory_ += sizeof(DocRow); }

std::string SegmentAssembler::finish(std::int64_t segment_id, std::int64_t max_ver)
{
    std::sort(docs_.begin(), docs_.end(), [](const DocRow &a, const DocRow &b) { return a.docno < b.docno; });
    std::vector<std::int64_t> ids(docs_.size());
    std::vector<std::uint32_t> lens(docs_.size());
    for (std::size_t i = 0; i < docs_.size(); ++i) {
        if (docs_[i].docno != i) throw std::runtime_error("doc_stats: docnos are not 0.." + std::to_string(docs_.size() - 1));
        ids[i] = docs_[i].doc_id;
        lens[i] = docs_[i].length;
    }
    const std::uint32_t nd = static_cast<std::uint32_t>(docs_.size());
    std::vector<DocRow>().swap(docs_);
    SegmentWriter w(segment_id, max_ver);
    w.set_docs(std::move(ids), std::move(lens));

    std::uint32_t expect = 0;
    std::vector<std::uint32_t> pos;
    auto p = postings_.begin();
    for (auto &kv : terms_) {
        if (kv.first != expect) throw std::runtime_error("term_dict: term ids are not consecutive at " + std::to_string(expect));
        ++expect;
        Term &t = kv.second;
        w.begin_term(t.term);
        std::uint32_t df = 0;
        for (; p != postings_.end() && p->first.first == kv.first; ++p) {
            const PostingRow &row = p->second;
            if (row.docs.empty() || row.payload.empty())
                throw std::runtime_error("postings or positions row missing for term id " + std::to_string(kv.first));
            roaring_bitmap_t *r = roaring_bitmap_portable_deserialize_safe(row.docs.data(), row.docs.size());
            if (!r) throw std::runtime_error("postings: damaged bitmap for term id " + std::to_string(kv.first));
            const std::uint8_t *q = reinterpret_cast<const std::uint8_t *>(row.payload.data());
            const std::uint8_t *end = q + row.payload.size();
            roaring_uint32_iterator_t *it = roaring_iterator_create(r);
            while (it->has_value) {
                const std::uint32_t docno = (row.chunk_id << CHUNK_BITS) | it->current_value;
                if (docno >= nd) throw std::runtime_error("postings: docno out of range");
                if (q >= end) throw std::runtime_error("positions: payload too short");
                const std::uint32_t tf = get_varint(q);
                pos.clear();
                std::uint32_t v = 0;
                for (std::uint32_t k = 0; k < tf; ++k) {
                    if (q >= end) throw std::runtime_error("positions: payload too short");
                    v += get_varint(q);
                    pos.push_back(v);
                }
                w.add_doc(docno, pos.data(), tf);
                ++df;
                roaring_uint32_iterator_advance(it);
            }
            roaring_uint32_iterator_free(it);
            roaring_bitmap_free(r);
            if (q != end) throw std::runtime_error("positions: payload longer than its documents");
        }
        if (df != t.df) throw std::runtime_error("term_dict: doc_freq of term id " + std::to_string(kv.first) + " differs from its postings");
        const std::uint32_t id = w.end_term();
        for (auto &s : t.stems) w.add_stem(stem_key(s.first, s.second), id);
    }
    if (p != postings_.end()) throw std::runtime_error("postings: rows for a term id that is not in term_dict");
    terms_.clear();
    postings_.clear();
    (void)nd;
    return w.finish();
}

} // namespace vtext
