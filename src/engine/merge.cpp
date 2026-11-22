#include "merge.h"
#include "postings.h"
#include "segment_writer.h"

#include <algorithm>
#include <queue>
#include <stdexcept>

namespace vtext {

std::string merge_segments(const std::vector<SegmentRef> &in, std::int64_t new_id, const MergeOptions &o)
{
    const std::size_t n = in.size();
    // 1. live documents of all inputs, in doc_id order (pre: their numbers in that order)
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
    for (std::uint32_t i = 0; i < docs.size(); ++i) {
        if (i > 0 && docs[i].id == docs[i - 1].id)
            throw std::runtime_error("merge: doc_id " + std::to_string(docs[i].id) + " is live in two segments");
        remap[docs[i].seg][docs[i].docno] = i;
    }
    const std::uint32_t nd = static_cast<std::uint32_t>(docs.size());

    // 2. terms in byte order over all inputs (k-way merge of the sorted dictionaries): each merged
    //    term is a run of (segment, term id) entries
    std::vector<std::pair<std::uint32_t, std::uint32_t>> entries;
    std::vector<std::uint32_t> term_start;
    std::vector<std::uint64_t> term_df;
    {
        std::vector<std::uint32_t> next(n, 0);
        auto term_greater = [&](std::uint32_t a, std::uint32_t b) {
            const std::string_view x = in[a].seg->term(next[a]), y = in[b].seg->term(next[b]);
            return x != y ? x > y : a > b;
        };
        std::priority_queue<std::uint32_t, std::vector<std::uint32_t>, decltype(term_greater)> pq(term_greater);
        for (std::uint32_t s = 0; s < n; ++s) if (in[s].seg->n_terms() > 0) pq.push(s);
        std::vector<std::uint32_t> group;
        while (!pq.empty()) {
            group.clear();
            const std::uint32_t first = pq.top();
            const std::string_view term = in[first].seg->term(next[first]);
            while (!pq.empty() && in[pq.top()].seg->term(next[pq.top()]) == term) { group.push_back(pq.top()); pq.pop(); }
            term_start.push_back(static_cast<std::uint32_t>(entries.size()));
            std::uint64_t df = 0;
            for (std::uint32_t s : group) { entries.emplace_back(s, next[s]); df += in[s].seg->info(next[s]).df; }
            term_df.push_back(df);
            for (std::uint32_t s : group) if (++next[s] < in[s].seg->n_terms()) pq.push(s);
        }
        term_start.push_back(static_cast<std::uint32_t>(entries.size()));
    }
    const std::size_t n_terms = term_df.size();

    // 3. the docno of every document: doc_id order, or recursive graph bisection over the terms
    //    with at least reorder_min_df documents
    std::vector<std::uint32_t> newno(nd);
    for (std::uint32_t i = 0; i < nd; ++i) newno[i] = i;
    if (o.reorder && nd >= 4 * std::max<std::uint32_t>(2, o.bisection.min_partition)) {
        const std::uint32_t min_df = o.bisection.min_df ? o.bisection.min_df : reorder_min_df(nd);
        ForwardIndex f;
        f.n_docs = nd;
        f.off.assign(static_cast<std::size_t>(nd) + 1, 0);
        std::vector<std::uint32_t> selected;
        for (std::size_t t = 0; t < n_terms; ++t) if (term_df[t] >= min_df) selected.push_back(static_cast<std::uint32_t>(t));
        f.n_terms = static_cast<std::uint32_t>(selected.size());
        auto each = [&](auto fn) {
            for (std::uint32_t k = 0; k < selected.size(); ++k) {
                const std::uint32_t t = selected[k];
                for (std::uint32_t e = term_start[t]; e < term_start[t + 1]; ++e) {
                    const std::uint32_t s = entries[e].first;
                    PostingIter it(*in[s].seg, entries[e].second);
                    for (std::uint32_t d = it.next(); d != NO_DOC; d = it.next()) {
                        const std::uint32_t i = remap[s][d];
                        if (i != NO_DOC) fn(i, k);
                    }
                }
            }
        };
        each([&](std::uint32_t i, std::uint32_t) { ++f.off[i + 1]; });
        for (std::uint32_t i = 0; i < nd; ++i) f.off[i + 1] += f.off[i];
        f.terms.resize(f.off[nd]);
        {
            std::vector<std::uint64_t> at(f.off.begin(), f.off.end() - 1);
            each([&](std::uint32_t i, std::uint32_t k) { f.terms[at[i]++] = k; });
        }
        const std::vector<std::uint32_t> order = bisect_order(f, o.bisection, o.stats);
        for (std::uint32_t p = 0; p < nd; ++p) newno[order[p]] = p;
    }
    std::vector<std::int64_t> ids(nd);
    std::vector<std::uint32_t> lens(nd);
    for (std::uint32_t i = 0; i < nd; ++i) {
        ids[newno[i]] = docs[i].id;
        lens[newno[i]] = in[docs[i].seg].seg->doc_len(docs[i].docno);
    }
    for (std::uint32_t s = 0; s < n; ++s)
        for (std::uint32_t &x : remap[s]) if (x != NO_DOC) x = newno[x];
    std::vector<D>().swap(docs);
    std::vector<std::uint32_t>().swap(newno);
    SegmentWriter w(new_id, 0);
    w.set_docs(std::move(ids), std::move(lens));

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

    // 4. postings and positions, term by term, in the new docnos
    struct Post { std::uint32_t docno, tf, at; };
    std::vector<Post> posts;
    std::vector<std::uint32_t> pos, tmp;
    for (std::size_t t = 0; t < n_terms; ++t) {
        posts.clear();
        pos.clear();
        bool ascending = true;
        for (std::uint32_t e = term_start[t]; e < term_start[t + 1]; ++e) {
            const std::uint32_t s = entries[e].first;
            PostingIter it(*in[s].seg, entries[e].second);
            for (std::uint32_t d = it.next(); d != NO_DOC; d = it.next()) {
                const std::uint32_t nd2 = remap[s][d];
                if (nd2 == NO_DOC) continue;
                tmp.clear();
                it.positions(tmp);
                if (!posts.empty() && posts.back().docno > nd2) ascending = false;
                posts.push_back({nd2, it.tf(), static_cast<std::uint32_t>(pos.size())});
                pos.insert(pos.end(), tmp.begin(), tmp.end());
            }
        }
        if (posts.empty()) continue;
        // the new docnos follow the input order only when neither side is reordered
        if (!ascending) std::sort(posts.begin(), posts.end(), [](const Post &a, const Post &b) { return a.docno < b.docno; });
        const std::uint32_t first = entries[term_start[t]].first;
        w.begin_term(in[first].seg->term(entries[term_start[t]].second));
        for (const Post &p : posts) w.add_doc(p.docno, pos.data() + p.at, p.tf);
        const std::uint32_t id = w.end_term();
        for (std::uint32_t e = term_start[t]; e < term_start[t + 1]; ++e) {
            const std::uint32_t s = entries[e].first;
            for (std::uint32_t se : term_stems[s][entries[e].second]) w.add_stem(std::string(in[s].seg->stem_key_at(se)), id);
        }
    }
    return w.finish();
}

} // namespace vtext
