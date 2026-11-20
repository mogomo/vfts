#include "merge.h"
#include "postings.h"
#include "segment_writer.h"

#include <algorithm>
#include <queue>
#include <stdexcept>

namespace vtext {

std::string merge_segments(const std::vector<SegmentRef> &in, std::int64_t new_id)
{
    const std::size_t n = in.size();
    // 1. live documents of all inputs, in doc_id order: new docnos
    struct D { std::int64_t id; std::uint32_t seg, docno; };
    std::vector<D> docs;
    for (std::uint32_t s = 0; s < n; ++s) {
        const Segment &seg = *in[s].seg;
        for (std::uint32_t d = 0; d < seg.n_docs(); ++d)
            if (!(in[s].dead && in[s].dead->test(d))) docs.push_back({seg.doc_id(d), s, d});
    }
    std::sort(docs.begin(), docs.end(), [](const D &a, const D &b) { return a.id < b.id; });
    std::vector<std::vector<std::uint32_t>> remap(n);
    for (std::uint32_t s = 0; s < n; ++s) remap[s].assign(in[s].seg->n_docs(), NO_DOC);
    std::vector<std::int64_t> ids(docs.size());
    std::vector<std::uint32_t> lens(docs.size());
    for (std::uint32_t i = 0; i < docs.size(); ++i) {
        if (i > 0 && docs[i].id == docs[i - 1].id)
            throw std::runtime_error("merge: doc_id " + std::to_string(docs[i].id) + " is live in two segments");
        remap[docs[i].seg][docs[i].docno] = i;
        ids[i] = docs[i].id;
        lens[i] = in[docs[i].seg].seg->doc_len(docs[i].docno);
    }
    std::vector<D>().swap(docs);
    SegmentWriter w(new_id, 0);
    w.set_docs(std::move(ids), std::move(lens));

    // 2. terms in byte order over all inputs (k-way merge of the sorted dictionaries)
    std::vector<std::uint32_t> next(n, 0);
    auto term_greater = [&](std::uint32_t a, std::uint32_t b) {
        const std::string_view x = in[a].seg->term(next[a]), y = in[b].seg->term(next[b]);
        return x != y ? x > y : a > b;
    };
    std::priority_queue<std::uint32_t, std::vector<std::uint32_t>, decltype(term_greater)> pq(term_greater);
    for (std::uint32_t s = 0; s < n; ++s) if (in[s].seg->n_terms() > 0) pq.push(s);

    // stem entries of every input term, to carry over: per segment, term id -> stem entry keys
    std::vector<std::vector<std::vector<std::uint32_t>>> term_stems(n);
    for (std::uint32_t s = 0; s < n; ++s) {
        const Segment &seg = *in[s].seg;
        term_stems[s].resize(seg.n_terms());
        for (std::uint32_t e = 0; e < seg.n_stems(); ++e) {
            std::uint32_t c;
            const std::uint32_t *t = seg.stem_terms_at(e, c);
            for (std::uint32_t k = 0; k < c; ++k) term_stems[s][t[k]].push_back(e);
        }
    }

    struct Post { std::uint32_t docno, tf, at; };
    std::vector<Post> posts;
    std::vector<std::uint32_t> pos, tmp;
    std::vector<std::uint32_t> group;
    while (!pq.empty()) {
        group.clear();
        const std::uint32_t first = pq.top();
        const std::string term(in[first].seg->term(next[first]));
        while (!pq.empty() && in[pq.top()].seg->term(next[pq.top()]) == term) { group.push_back(pq.top()); pq.pop(); }
        posts.clear();
        pos.clear();
        for (std::uint32_t s : group) {
            PostingIter it(*in[s].seg, next[s]);
            for (std::uint32_t d = it.next(); d != NO_DOC; d = it.next()) {
                const std::uint32_t nd = remap[s][d];
                if (nd == NO_DOC) continue;
                tmp.clear();
                it.positions(tmp);
                posts.push_back({nd, it.tf(), static_cast<std::uint32_t>(pos.size())});
                pos.insert(pos.end(), tmp.begin(), tmp.end());
            }
        }
        if (!posts.empty()) {
            if (group.size() > 1) std::sort(posts.begin(), posts.end(), [](const Post &a, const Post &b) { return a.docno < b.docno; });
            w.begin_term(term);
            for (const Post &p : posts) w.add_doc(p.docno, pos.data() + p.at, p.tf);
            const std::uint32_t id = w.end_term();
            for (std::uint32_t s : group)
                for (std::uint32_t e : term_stems[s][next[s]]) w.add_stem(std::string(in[s].seg->stem_key_at(e)), id);
        }
        for (std::uint32_t s : group) if (++next[s] < in[s].seg->n_terms()) pq.push(s);
    }
    return w.finish();
}

} // namespace vtext
