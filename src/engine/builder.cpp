#include "builder.h"
#include "segment.h"
#include "segment_writer.h"

#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace vtext {

std::uint64_t term_hash(std::string_view s)
{
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ull; }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return h;
}

SegmentBuilder::SegmentBuilder(const AnalyzerOptions &opts) : analyzer_(opts), slots_(1 << 12, 0)
{
    key_off_.push_back(0);
    if (lang_count() > 64) throw std::runtime_error("builder: more than 64 languages");
}

void SegmentBuilder::grow()
{
    std::vector<std::uint32_t> s(slots_.size() * 2, 0);
    const std::size_t mask = s.size() - 1;
    for (std::uint32_t t = 0; t < terms_.size(); ++t) {
        std::size_t i = hashes_[t] & mask;
        while (s[i]) i = (i + 1) & mask;
        s[i] = t + 1;
    }
    slots_.swap(s);
}

std::uint32_t SegmentBuilder::intern(std::string_view term)
{
    const std::uint64_t h = term_hash(term);
    const std::size_t mask = slots_.size() - 1;
    std::size_t i = h & mask;
    while (slots_[i]) {
        const std::uint32_t t = slots_[i] - 1;
        if (hashes_[t] == h && std::string_view(keys_).substr(key_off_[t], key_off_[t + 1] - key_off_[t]) == term) return t;
        i = (i + 1) & mask;
    }
    const std::uint32_t t = static_cast<std::uint32_t>(terms_.size());
    slots_[i] = t + 1;
    hashes_.push_back(h);
    keys_.append(term);
    key_off_.push_back(static_cast<std::uint32_t>(keys_.size()));
    terms_.emplace_back();
    memory_ += term.size() + 64;
    if (terms_.size() * 2 > slots_.size()) grow();
    return t;
}

void SegmentBuilder::add(std::int64_t doc_id, std::string_view text, Lang lang, std::int64_t ver)
{
    analyzer_.analyze(text, lang);
    const std::uint32_t docidx = static_cast<std::uint32_t>(doc_ids_.size());
    if (docidx >= 0xFFFFFFF0u) throw std::runtime_error("builder: too many documents for one segment");
    const std::size_t n = analyzer_.size();
    scratch_.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t t = intern(analyzer_.term(i));
        terms_[t].langs |= 1ull << analyzer_.token(i).lang;
        scratch_[i] = {t, analyzer_.token(i).pos};
    }
    std::sort(scratch_.begin(), scratch_.end());
    for (std::size_t i = 0; i < n;) {
        std::size_t j = i;
        while (j < n && scratch_[j].first == scratch_[i].first) ++j;
        TermAcc &acc = terms_[scratch_[i].first];
        const std::size_t before = acc.buf.capacity();
        put_varint(acc.buf, docidx + 1 - acc.last);
        acc.last = docidx + 1;
        put_varint(acc.buf, static_cast<std::uint32_t>(j - i));
        std::uint32_t prev = 0;
        for (std::size_t k = i; k < j; ++k) { put_varint(acc.buf, scratch_[k].second - prev); prev = scratch_[k].second; }
        memory_ += acc.buf.capacity() - before;
        i = j;
    }
    doc_ids_.push_back(doc_id);
    doc_len_.push_back(static_cast<std::uint32_t>(n));
    max_ver_ = std::max(max_ver_, ver);
    memory_ += 12;
}

std::string SegmentBuilder::finish(std::int64_t segment_id)
{
    const std::uint32_t nd = static_cast<std::uint32_t>(doc_ids_.size());
    // docno = rank of the doc_id
    std::vector<std::uint32_t> order(nd);
    std::iota(order.begin(), order.end(), 0u);
    bool sorted = std::is_sorted(doc_ids_.begin(), doc_ids_.end());
    if (!sorted) std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return doc_ids_[a] < doc_ids_[b]; });
    std::vector<std::uint32_t> docno(nd);
    std::vector<std::int64_t> ids(nd);
    std::vector<std::uint32_t> lens(nd);
    for (std::uint32_t r = 0; r < nd; ++r) {
        docno[order[r]] = r;
        ids[r] = doc_ids_[order[r]];
        lens[r] = doc_len_[order[r]];
        if (r > 0 && ids[r] == ids[r - 1]) throw std::runtime_error("duplicate doc_id " + std::to_string(ids[r]));
    }
    SegmentWriter w(segment_id, max_ver_);
    w.set_docs(std::move(ids), std::move(lens));

    std::vector<std::uint32_t> term_order(terms_.size());
    std::iota(term_order.begin(), term_order.end(), 0u);
    auto key = [&](std::uint32_t t) { return std::string_view(keys_).substr(key_off_[t], key_off_[t + 1] - key_off_[t]); };
    std::sort(term_order.begin(), term_order.end(), [&](std::uint32_t a, std::uint32_t b) { return key(a) < key(b); });

    struct Entry { std::uint32_t docno, tf, at; };
    std::vector<Entry> entries;
    std::vector<std::uint32_t> pos;
    for (std::uint32_t t : term_order) {
        TermAcc &acc = terms_[t];
        entries.clear();
        pos.clear();
        const std::uint8_t *p = reinterpret_cast<const std::uint8_t *>(acc.buf.data());
        const std::uint8_t *end = p + acc.buf.size();
        std::uint32_t docidx = 0;
        while (p < end) {
            docidx += get_varint(p);
            const std::uint32_t tf = get_varint(p);
            entries.push_back({docno[docidx - 1], tf, static_cast<std::uint32_t>(pos.size())});
            std::uint32_t q = 0;
            for (std::uint32_t k = 0; k < tf; ++k) { q += get_varint(p); pos.push_back(q); }
        }
        if (!sorted) std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.docno < b.docno; });
        const std::string_view term = key(t);
        w.begin_term(term);
        for (const Entry &e : entries) w.add_doc(e.docno, pos.data() + e.at, e.tf);
        const std::uint32_t id = w.end_term();
        for (std::uint32_t l = 0; l < 64; ++l) {
            if (!(acc.langs >> l & 1)) continue;
            const Lang lang = static_cast<Lang>(l);
            w.add_stem(stem_key(lang_info(lang).code, stemmer_.stem(term, lang)), id);
        }
        std::string().swap(acc.buf);
    }
    std::string out = w.finish();
    // reset
    slots_.assign(1 << 12, 0);
    hashes_.clear();
    keys_.clear();
    key_off_.assign(1, 0);
    terms_.clear();
    doc_ids_.clear();
    doc_len_.clear();
    max_ver_ = 0;
    memory_ = 0;
    return out;
}

} // namespace vtext
