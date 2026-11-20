#include "search.h"
#include "expand.h"
#include "postings.h"
#include "stemmer.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace vtext {

// ---------------------------------------------------------------- dead counts

std::uint32_t DeadCounts::term(std::uint32_t t) const
{
    auto it = std::lower_bound(terms.begin(), terms.end(), std::make_pair(t, 0u));
    return it != terms.end() && it->first == t ? it->second : 0;
}

std::uint32_t DeadCounts::stem(std::uint32_t s) const
{
    auto it = std::lower_bound(stems.begin(), stems.end(), std::make_pair(s, 0u));
    return it != stems.end() && it->first == s ? it->second : 0;
}

DeadCounts compute_dead_counts(const Segment &seg, const DocSet &dead)
{
    DeadCounts dc;
    if (dead.count == 0) return dc;
    std::vector<std::uint32_t> per_term(seg.n_terms(), 0);
    for (std::uint32_t t = 0; t < seg.n_terms(); ++t) {
        PostingIter it(seg, t);
        std::uint32_t n = 0;
        for (std::uint32_t d = it.next(); d != NO_DOC; d = it.next()) n += dead.test(d);
        per_term[t] = n;
        if (n) dc.terms.emplace_back(t, n);
    }
    std::vector<std::uint32_t> stamp(seg.n_docs(), 0);
    for (std::uint32_t s = 0; s < seg.n_stems(); ++s) {
        std::uint32_t count;
        const std::uint32_t *ids = seg.stem_terms_at(s, count);
        std::uint32_t n = 0;
        if (count == 1) n = per_term[ids[0]];
        else {
            bool any = false;
            for (std::uint32_t k = 0; k < count; ++k) any |= per_term[ids[k]] > 0;
            if (any)
                for (std::uint32_t k = 0; k < count; ++k) {
                    if (!per_term[ids[k]]) continue;
                    PostingIter it(seg, ids[k]);
                    for (std::uint32_t d = it.next(); d != NO_DOC; d = it.next())
                        if (dead.test(d) && stamp[d] != s + 1) { stamp[d] = s + 1; ++n; }
                }
        }
        if (n) dc.stems.emplace_back(s, n);
    }
    return dc;
}

namespace {

// ---------------------------------------------------------------- plan

constexpr std::size_t MAX_NEAR_PAIRS = 100000;

struct Span { std::uint32_t start, end; };

// One leaf of the query (a word, or one slot of a phrase, or a pattern), resolved for every
// segment.
struct Leaf {
    enum Kind { Word, Pattern } kind = Word;
    bool constant = false;                          // prefix / wildcard: score 1, like Lucene's constant-score rewrite
    std::vector<std::vector<std::uint32_t>> ids;    // per segment
    std::vector<std::int64_t> stem_entry;           // per segment: the stem group when ids is exactly that group
    std::uint64_t df = 0;                           // global live document frequency
    float idf = 0;
};

struct PNode {
    QKind kind = QKind::None;
    std::vector<int> leaves;        // Term: 1; Phrase: per slot (-1 = gap); patterns: 1
    int slop = 0, dist = 0;
    bool ordered = false;
    float weight = 0;
    std::vector<PNode> must, should, must_not, kids;
};

// ---------------------------------------------------------------- iterators

struct Scoring {
    const float *cache = nullptr;           // 256 entries: 1 / (k1 * (1 - b + b * len / avgdl))
    const std::uint8_t *norms = nullptr;
};

struct It {
    std::uint32_t doc = NO_DOC;
    bool positioned = false;
    virtual ~It() = default;
    virtual std::uint32_t advance(std::uint32_t target) = 0;    // first doc >= target, never backwards
    std::uint32_t next() { return positioned ? (doc == NO_DOC ? NO_DOC : advance(doc + 1)) : advance(0); }
    virtual float score() = 0;
    virtual void spans(std::vector<Span> &out) = 0;
    virtual std::uint64_t cost() const = 0;
};

// The documents of one leaf: one term, or the union of several (stem group, expansion).
struct LeafIt : It {
    std::vector<PostingIter> its;
    float weight = 0;
    bool constant = false;
    Scoring sc;
    std::vector<std::uint32_t> tmp;

    std::uint32_t advance(std::uint32_t target) override
    {
        if (positioned && (doc == NO_DOC || doc >= target)) return doc;
        positioned = true;
        std::uint32_t m = NO_DOC;
        for (PostingIter &p : its) m = std::min(m, p.advance(target));
        return doc = m;
    }
    std::uint32_t tf() const
    {
        std::uint32_t f = 0;
        for (const PostingIter &p : its) if (p.doc() == doc) f += p.tf();
        return f;
    }
    float score() override
    {
        if (constant) return 1.0f;
        const float freq = static_cast<float>(tf());
        const float norm_inverse = sc.cache[sc.norms[doc]];
        return weight - weight / (1.0f + freq * norm_inverse);
    }
    void positions(std::vector<std::uint32_t> &out)
    {
        out.clear();
        std::size_t parts = 0;
        for (PostingIter &p : its)
            if (p.doc() == doc) { p.positions(out); ++parts; }
        if (parts > 1) std::sort(out.begin(), out.end());
    }
    void spans(std::vector<Span> &out) override
    {
        positions(tmp);
        for (std::uint32_t p : tmp) out.push_back({p, p});
    }
    std::uint64_t cost() const override
    {
        std::uint64_t c = 0;
        for (const PostingIter &p : its) c += p.df();
        return c;
    }
};

struct AllIt : It {
    std::uint32_t n;
    explicit AllIt(std::uint32_t n_docs) : n(n_docs) {}
    std::uint32_t advance(std::uint32_t target) override
    {
        if (positioned && (doc == NO_DOC || doc >= target)) return doc;
        positioned = true;
        return doc = target < n ? target : NO_DOC;
    }
    float score() override { return 0.0f; }
    void spans(std::vector<Span> &) override {}
    std::uint64_t cost() const override { return n; }
};

struct EmptyIt : It {
    std::uint32_t advance(std::uint32_t) override { positioned = true; return doc = NO_DOC; }
    float score() override { return 0.0f; }
    void spans(std::vector<Span> &) override {}
    std::uint64_t cost() const override { return 0; }
};

// Moves all iterators to the same document >= target (leapfrog); NO_DOC when there is none.
std::uint32_t conjunction(std::vector<It *> &v, std::uint32_t target)
{
    std::uint32_t d = v[0]->advance(target);
    for (;;) {
        if (d == NO_DOC) return NO_DOC;
        bool all = true;
        for (std::size_t i = 1; i < v.size(); ++i) {
            const std::uint32_t e = v[i]->advance(d);
            if (e != d) {
                if (e == NO_DOC) return NO_DOC;
                d = v[0]->advance(e);
                all = false;
                break;
            }
        }
        if (all) return d;
    }
}

// Phrase: the slots in a conjunction, then the positions.
struct PhraseIt : It {
    std::vector<std::unique_ptr<LeafIt>> slots;
    std::vector<std::uint32_t> offsets;     // position of each slot inside the phrase
    std::vector<It *> conj;
    int slop = 0;
    float weight = 0;
    Scoring sc;
    float freq = 0;
    std::vector<Span> matches;
    std::vector<std::vector<std::uint32_t>> pos;

    void init()
    {
        for (auto &s : slots) conj.push_back(s.get());
        std::sort(conj.begin(), conj.end(), [](It *a, It *b) { return a->cost() < b->cost(); });
        pos.resize(slots.size());
    }
    std::uint32_t advance(std::uint32_t target) override
    {
        if (positioned && (doc == NO_DOC || doc >= target)) return doc;
        positioned = true;
        for (std::uint32_t d = conjunction(conj, target); d != NO_DOC; d = conjunction(conj, d + 1))
            if (verify()) return doc = d;
        return doc = NO_DOC;
    }
    bool verify()
    {
        for (std::size_t i = 0; i < slots.size(); ++i) slots[i]->positions(pos[i]);
        matches.clear();
        freq = 0;
        if (slop == 0) exact();
        else sloppy();
        return freq > 0;
    }
    void exact()
    {
        const std::uint32_t o0 = offsets[0], last = offsets.back();
        for (std::uint32_t p : pos[0]) {
            if (p < o0) continue;
            const std::uint32_t s = p - o0;
            bool ok = true;
            for (std::size_t i = 1; i < slots.size() && ok; ++i)
                ok = std::binary_search(pos[i].begin(), pos[i].end(), s + offsets[i]);
            if (ok) { freq += 1.0f; matches.push_back({s, s + last}); }
        }
    }
    // Lucene's SloppyPhraseMatcher (without the special handling of repeated terms).
    void sloppy()
    {
        const std::size_t n = slots.size();
        std::vector<std::size_t> idx(n, 0);
        std::vector<std::int64_t> q(n);
        std::int64_t end = INT64_MIN;
        for (std::size_t i = 0; i < n; ++i) {
            q[i] = static_cast<std::int64_t>(pos[i][0]) - offsets[i];
            end = std::max(end, q[i]);
        }
        // queue order: position, then offset, then slot (Lucene's PhraseQueue)
        auto greater = [&](std::size_t a, std::size_t b) {
            if (q[a] != q[b]) return q[a] > q[b];
            if (offsets[a] != offsets[b]) return offsets[a] > offsets[b];
            return a > b;
        };
        std::priority_queue<std::size_t, std::vector<std::size_t>, decltype(greater)> pq(greater);
        for (std::size_t i = 0; i < n; ++i) pq.push(i);
        auto advance_pp = [&](std::size_t i) {
            if (idx[i] + 1 >= pos[i].size()) return false;
            ++idx[i];
            q[i] = static_cast<std::int64_t>(pos[i][idx[i]]) - offsets[i];
            end = std::max(end, q[i]);
            return true;
        };
        auto record = [&](std::int64_t match_length, std::size_t moved, std::int64_t moved_q) {
            freq += 1.0f / (1.0f + static_cast<float>(match_length));
            std::int64_t lo = INT64_MAX, hi = INT64_MIN;
            for (std::size_t i = 0; i < n; ++i) {
                const std::int64_t p = (i == moved ? moved_q : q[i]) + offsets[i];
                lo = std::min(lo, p);
                hi = std::max(hi, p);
            }
            matches.push_back({static_cast<std::uint32_t>(lo), static_cast<std::uint32_t>(hi)});
        };
        for (;;) {
            std::size_t pp = pq.top();
            pq.pop();
            std::int64_t match_length = end - q[pp];
            std::int64_t next = q[pq.top()];
            bool matched = false;
            for (;;) {
                const std::int64_t before = q[pp];
                if (!advance_pp(pp)) {
                    if (match_length <= slop) record(match_length, n, 0);
                    return;
                }
                if (q[pp] > next) {
                    pq.push(pp);
                    if (match_length <= slop) { record(match_length, pp, before); matched = true; break; }
                    pp = pq.top();
                    pq.pop();
                    next = q[pq.top()];
                    match_length = end - q[pp];
                } else {
                    match_length = std::min(match_length, end - q[pp]);
                }
            }
            if (!matched) return;
        }
    }
    float score() override
    {
        const float norm_inverse = sc.cache[sc.norms[doc]];
        return weight - weight / (1.0f + freq * norm_inverse);
    }
    void spans(std::vector<Span> &out) override { out.insert(out.end(), matches.begin(), matches.end()); }
    std::uint64_t cost() const override { return conj.empty() ? 0 : conj[0]->cost(); }
};

// w/N and pre/N: both operands in the document, then pairs of their spans within the distance.
struct NearIt : It {
    std::unique_ptr<It> a, b;
    std::vector<It *> conj;
    int dist = 0;
    bool ordered = false;
    float weight = 0;
    Scoring sc;
    float freq = 0;
    std::vector<Span> sa, sb, matches;

    void init()
    {
        conj = {a.get(), b.get()};
        std::sort(conj.begin(), conj.end(), [](It *x, It *y) { return x->cost() < y->cost(); });
    }
    std::uint32_t advance(std::uint32_t target) override
    {
        if (positioned && (doc == NO_DOC || doc >= target)) return doc;
        positioned = true;
        for (std::uint32_t d = conjunction(conj, target); d != NO_DOC; d = conjunction(conj, d + 1))
            if (verify()) return doc = d;
        return doc = NO_DOC;
    }
    bool verify()
    {
        sa.clear(); sb.clear(); matches.clear();
        a->spans(sa);
        b->spans(sb);
        auto by_start = [](const Span &x, const Span &y) { return x.start != y.start ? x.start < y.start : x.end < y.end; };
        std::sort(sb.begin(), sb.end(), by_start);
        freq = 0;
        std::size_t pairs = 0;
        for (const Span &x : sa) {
            for (const Span &y : sb) {
                if (y.start > x.end + static_cast<std::uint32_t>(dist)) break;
                std::uint32_t gap = 0;
                if (y.start > x.end) gap = y.start - x.end;
                else if (!ordered && x.start > y.end) gap = x.start - y.end;
                else continue;
                if (gap == 0 || gap > static_cast<std::uint32_t>(dist)) continue;
                freq += 1.0f / static_cast<float>(gap);
                matches.push_back({std::min(x.start, y.start), std::max(x.end, y.end)});
                if (++pairs >= MAX_NEAR_PAIRS) return freq > 0;
            }
        }
        return freq > 0;
    }
    float score() override
    {
        const float norm_inverse = sc.cache[sc.norms[doc]];
        return weight - weight / (1.0f + freq * norm_inverse);
    }
    void spans(std::vector<Span> &out) override { out.insert(out.end(), matches.begin(), matches.end()); }
    std::uint64_t cost() const override { return std::min(a->cost(), b->cost()); }
};

// Lucene's BooleanQuery: every must clause, at least one should clause when there is no must
// clause, no must_not clause. Score: the sum over the must clauses and the matching should clauses.
struct BoolIt : It {
    std::vector<std::unique_ptr<It>> must, should, must_not;
    std::vector<It *> conj;
    std::unique_ptr<It> all;
    std::uint32_t n_docs = 0;

    void init()
    {
        for (auto &m : must) conj.push_back(m.get());
        std::sort(conj.begin(), conj.end(), [](It *x, It *y) { return x->cost() < y->cost(); });
        if (must.empty() && should.empty()) { all.reset(new AllIt(n_docs)); conj.push_back(all.get()); }
    }
    std::uint32_t candidate(std::uint32_t target)
    {
        if (!conj.empty()) return conjunction(conj, target);
        std::uint32_t m = NO_DOC;
        for (auto &s : should) m = std::min(m, s->advance(target));
        return m;
    }
    std::uint32_t advance(std::uint32_t target) override
    {
        if (positioned && (doc == NO_DOC || doc >= target)) return doc;
        positioned = true;
        for (std::uint32_t d = candidate(target); d != NO_DOC; d = candidate(d + 1)) {
            bool excluded = false;
            for (auto &x : must_not) if (x->advance(d) == d) { excluded = true; break; }
            if (!excluded) return doc = d;
        }
        return doc = NO_DOC;
    }
    float score() override
    {
        double s = 0;
        for (auto &m : must) s += m->score();
        for (auto &x : should) if (x->advance(doc) == doc) s += x->score();
        return static_cast<float>(s);
    }
    void spans(std::vector<Span> &out) override
    {
        for (auto &x : should) if (x->advance(doc) == doc) x->spans(out);
    }
    std::uint64_t cost() const override
    {
        if (!conj.empty()) return conj[0]->cost();
        std::uint64_t c = 0;
        for (auto &s : should) c += s->cost();
        return c;
    }
};

// ---------------------------------------------------------------- top k

struct Cand {
    float score;
    std::int64_t doc_id;
};
// "a is a better hit than b": higher score, then lower doc id
inline bool better(const Cand &a, const Cand &b) { return a.score != b.score ? a.score > b.score : a.doc_id < b.doc_id; }

struct TopK {
    std::size_t k;
    std::vector<Cand> heap;     // the worst hit on top
    explicit TopK(std::size_t k_) : k(k_) {}
    void offer(float s, std::int64_t id)
    {
        Cand c{s, id};
        if (heap.size() < k) { heap.push_back(c); std::push_heap(heap.begin(), heap.end(), better); }
        else if (better(c, heap.front())) {
            std::pop_heap(heap.begin(), heap.end(), better);
            heap.back() = c;
            std::push_heap(heap.begin(), heap.end(), better);
        }
    }
};

} // namespace

// ---------------------------------------------------------------- searcher

struct Searcher::Impl {
    Searcher &s;
    const SearchOptions &o;
    std::vector<const Segment *> segs;
    std::vector<const DocSet *> dead;
    std::vector<const DeadCounts *> dcs;
    std::vector<const std::vector<std::uint32_t> *> masked;
    std::vector<Leaf> leaves;
    float cache[256];
    Stemmer stemmer;

    Impl(Searcher &searcher, const SearchOptions &opts) : s(searcher), o(opts)
    {
        const std::size_t n = s.segs_.size() + s.tail_segs_.size();
        static const std::vector<std::uint32_t> none;
        for (std::size_t i = 0; i < s.segs_.size(); ++i) {
            segs.push_back(s.segs_[i].seg);
            dead.push_back(s.combined_dead_[i].get());
            dcs.push_back(s.segs_[i].dead_counts.get());
            masked.push_back(&s.masked_[i]);
        }
        for (auto &t : s.tail_segs_) { segs.push_back(t.get()); dead.push_back(nullptr); dcs.push_back(nullptr); masked.push_back(&none); }
        (void)n;
        const float avgdl = s.stats_.avgdl();
        for (int i = 0; i < 256; ++i)
            cache[i] = 1.0f / (o.k1 * ((1.0f - o.b) + o.b * static_cast<float>(norm_to_length(static_cast<std::uint8_t>(i))) / avgdl));
    }

    float idf(std::uint64_t df) const
    {
        const double N = static_cast<double>(s.stats_.n_docs);
        return static_cast<float>(std::log(1.0 + (N - static_cast<double>(df) + 0.5) / (static_cast<double>(df) + 0.5)));
    }

    bool is_dead(std::size_t si, std::uint32_t d) const { return dead[si] && dead[si]->test(d); }

    // Live documents of segment si that contain at least one of ids.
    std::uint64_t live_df(std::size_t si, const std::vector<std::uint32_t> &ids, std::int64_t stem_entry)
    {
        const Segment &seg = *segs[si];
        if (ids.empty()) return 0;
        std::uint64_t df;
        if (ids.size() == 1) {
            df = seg.info(ids[0]).df - (dcs[si] ? dcs[si]->term(ids[0]) : 0);
        } else if (stem_entry >= 0) {
            df = seg.stem_union_df(static_cast<std::uint32_t>(stem_entry)) - (dcs[si] ? dcs[si]->stem(static_cast<std::uint32_t>(stem_entry)) : 0);
        } else {
            // union counted directly (fuzzy expansions, or a word outside its stem group)
            std::vector<PostingIter> its;
            for (std::uint32_t t : ids) its.emplace_back(seg, t);
            std::uint64_t n = 0;
            std::uint32_t d = 0;
            for (;;) {
                std::uint32_t m = NO_DOC;
                for (PostingIter &p : its) m = std::min(m, p.advance(d));
                if (m == NO_DOC) break;
                if (!is_dead(si, m)) ++n;
                d = m + 1;
            }
            return n;
        }
        // documents masked by the live tail
        const auto &mk = *masked[si];
        if (!mk.empty()) {
            std::vector<PostingIter> its;
            for (std::uint32_t t : ids) its.emplace_back(seg, t);
            for (std::uint32_t m : mk)
                for (PostingIter &p : its) if (p.advance(m) == m) { --df; break; }
        }
        return df;
    }

    int add_word_leaf(const std::string &word, Lang lang)
    {
        Leaf L;
        L.kind = Leaf::Word;
        const std::size_t n = segs.size();
        L.ids.resize(n);
        L.stem_entry.assign(n, -1);
        std::string key;
        if (o.match == MatchMode::Stem) key = stem_key(lang_info(lang).code, stemmer.stem(word, lang));
        for (std::size_t si = 0; si < n; ++si) {
            const Segment &seg = *segs[si];
            const std::int64_t exact = seg.find_term(word);
            if (o.match == MatchMode::Stem) {
                const std::int64_t e = seg.find_stem(key);
                if (e >= 0) {
                    std::uint32_t c;
                    const std::uint32_t *ids = seg.stem_terms_at(static_cast<std::uint32_t>(e), c);
                    L.ids[si].assign(ids, ids + c);
                    L.stem_entry[si] = e;
                    if (exact >= 0 && !std::binary_search(ids, ids + c, static_cast<std::uint32_t>(exact))) {
                        L.ids[si].push_back(static_cast<std::uint32_t>(exact));
                        std::sort(L.ids[si].begin(), L.ids[si].end());
                        L.stem_entry[si] = -1;
                    }
                    continue;
                }
            }
            if (exact >= 0) L.ids[si].push_back(static_cast<std::uint32_t>(exact));
        }
        for (std::size_t si = 0; si < n; ++si) L.df += live_df(si, L.ids[si], L.stem_entry[si]);
        L.idf = idf(L.df);
        leaves.push_back(std::move(L));
        return static_cast<int>(leaves.size() - 1);
    }

    int add_pattern_leaf(const QNode &q)
    {
        Leaf L;
        L.kind = Leaf::Pattern;
        L.constant = q.kind != QKind::Fuzzy;
        const std::size_t n = segs.size();
        L.ids.resize(n);
        L.stem_entry.assign(n, -1);
        // candidates over all segments: term -> (edits, total df)
        std::map<std::string, std::pair<int, std::uint64_t>> cand;
        std::vector<std::uint32_t> pat, txt;
        if (q.kind == QKind::Prefix) {
            for (std::size_t si = 0; si < n; ++si) {
                const Segment &seg = *segs[si];
                for (std::uint32_t t = seg.lower_bound(q.pattern); t < seg.n_terms(); ++t) {
                    const std::string_view term = seg.term(t);
                    if (term.compare(0, q.pattern.size(), q.pattern) != 0) break;
                    auto &c = cand[std::string(term)];
                    c.second += seg.info(t).df;
                }
            }
        } else if (q.kind == QKind::Wildcard) {
            to_code_points(q.pattern, pat);
            const std::string pre = literal_prefix(q.pattern);
            for (std::size_t si = 0; si < n; ++si) {
                const Segment &seg = *segs[si];
                for (std::uint32_t t = seg.lower_bound(pre); t < seg.n_terms(); ++t) {
                    const std::string_view term = seg.term(t);
                    if (term.compare(0, pre.size(), pre) != 0) break;
                    to_code_points(term, txt);
                    if (wildcard_match(pat, txt)) cand[std::string(term)].second += seg.info(t).df;
                }
            }
        } else {    // fuzzy
            const std::string &w = q.words[0];
            to_code_points(w, pat);
            const std::size_t plen = std::min<std::size_t>(static_cast<std::size_t>(std::max(o.prefix_length, 0)), pat.size());
            // the byte prefix of the first plen code points
            std::size_t bytes = 0;
            {
                std::size_t cps = 0;
                while (bytes < w.size() && cps < plen) {
                    ++bytes;
                    while (bytes < w.size() && (static_cast<unsigned char>(w[bytes]) & 0xC0) == 0x80) ++bytes;
                    ++cps;
                }
            }
            const std::string pre = w.substr(0, bytes);
            for (std::size_t si = 0; si < n; ++si) {
                const Segment &seg = *segs[si];
                for (std::uint32_t t = seg.lower_bound(pre); t < seg.n_terms(); ++t) {
                    const std::string_view term = seg.term(t);
                    if (term.compare(0, pre.size(), pre) != 0) break;
                    // quick length filter on bytes (a code point is 1 to 4 bytes)
                    if (term.size() + 4 * static_cast<std::size_t>(q.edits) < w.size() && term.size() * 4 + 4 * q.edits < w.size()) continue;
                    to_code_points(term, txt);
                    const int dist = bounded_osa_distance(pat, txt, q.edits);
                    if (dist <= q.edits) {
                        auto &c = cand[std::string(term)];
                        c.first = dist;
                        c.second += seg.info(t).df;
                    }
                }
            }
        }
        // the best max_expansions terms, the same choice on every segment
        std::vector<std::pair<std::string, std::pair<int, std::uint64_t>>> v(cand.begin(), cand.end());
        std::sort(v.begin(), v.end(), [](const auto &x, const auto &y) {
            if (x.second.first != y.second.first) return x.second.first < y.second.first;      // fewer edits
            if (x.second.second != y.second.second) return x.second.second > y.second.second;  // more documents
            return x.first < y.first;
        });
        if (v.size() > o.max_expansions) v.resize(o.max_expansions);
        std::uint64_t max_df = 0;
        for (std::size_t si = 0; si < n; ++si) {
            for (auto &e : v) {
                const std::int64_t t = segs[si]->find_term(e.first);
                if (t >= 0) L.ids[si].push_back(static_cast<std::uint32_t>(t));
            }
            std::sort(L.ids[si].begin(), L.ids[si].end());
        }
        if (L.constant) {
            // only used as the weight of the pattern inside w/N or pre/N: the most frequent term
            for (auto &e : v) max_df = std::max(max_df, e.second.second);
            L.df = max_df;
        } else {
            for (std::size_t si = 0; si < n; ++si) L.df += live_df(si, L.ids[si], -1);
        }
        L.idf = idf(L.df);
        leaves.push_back(std::move(L));
        return static_cast<int>(leaves.size() - 1);
    }

    PNode plan(const QNode &q)
    {
        PNode p;
        p.kind = q.kind;
        switch (q.kind) {
        case QKind::None: break;
        case QKind::Term:
            p.leaves.push_back(add_word_leaf(q.words[0], q.langs[0]));
            p.weight = leaves[p.leaves[0]].idf;
            break;
        case QKind::Phrase: {
            double w = 0;
            for (std::size_t i = 0; i < q.words.size(); ++i) {
                if (q.words[i].empty()) { p.leaves.push_back(-1); continue; }
                const int l = add_word_leaf(q.words[i], q.langs[i]);
                p.leaves.push_back(l);
                w += leaves[l].idf;
            }
            p.weight = static_cast<float>(w);
            p.slop = q.slop;
            break;
        }
        case QKind::Prefix: case QKind::Wildcard: case QKind::Fuzzy:
            p.leaves.push_back(add_pattern_leaf(q));
            p.weight = leaves[p.leaves[0]].idf;
            break;
        case QKind::Near: {
            p.dist = q.dist;
            p.ordered = q.ordered;
            p.kids.push_back(plan(q.kids[0]));
            p.kids.push_back(plan(q.kids[1]));
            p.weight = leaf_weight(p.kids[0]) + leaf_weight(p.kids[1]);
            break;
        }
        case QKind::Bool:
            for (const QNode &k : q.must) p.must.push_back(plan(k));
            for (const QNode &k : q.should) p.should.push_back(plan(k));
            for (const QNode &k : q.must_not) p.must_not.push_back(plan(k));
            break;
        }
        return p;
    }

    // The sum of the idf of the words below a node (the weight of a w/N or pre/N).
    float leaf_weight(const PNode &p) const
    {
        double w = 0;
        for (int l : p.leaves) if (l >= 0) w += leaves[l].idf;
        for (auto *v : {&p.must, &p.should, &p.kids}) for (const PNode &k : *v) w += leaf_weight(k);
        return static_cast<float>(w);
    }

    std::unique_ptr<LeafIt> leaf_it(std::size_t si, int l, float weight, const Scoring &sc)
    {
        std::unique_ptr<LeafIt> it(new LeafIt());
        const Leaf &L = leaves[l];
        for (std::uint32_t t : L.ids[si]) it->its.emplace_back(*segs[si], t);
        it->weight = weight;
        it->constant = L.constant;
        it->sc = sc;
        return it;
    }

    std::unique_ptr<It> build(std::size_t si, const PNode &p, const Scoring &sc)
    {
        switch (p.kind) {
        case QKind::None: return std::unique_ptr<It>(new EmptyIt());
        case QKind::Term: case QKind::Prefix: case QKind::Wildcard: case QKind::Fuzzy: {
            if (leaves[p.leaves[0]].ids[si].empty()) return std::unique_ptr<It>(new EmptyIt());
            return leaf_it(si, p.leaves[0], p.weight, sc);
        }
        case QKind::Phrase: {
            std::unique_ptr<PhraseIt> it(new PhraseIt());
            for (std::size_t i = 0; i < p.leaves.size(); ++i) {
                if (p.leaves[i] < 0) continue;
                if (leaves[p.leaves[i]].ids[si].empty()) return std::unique_ptr<It>(new EmptyIt());
                it->slots.push_back(leaf_it(si, p.leaves[i], 0, sc));
                it->offsets.push_back(static_cast<std::uint32_t>(i));
            }
            it->slop = p.slop;
            it->weight = p.weight;
            it->sc = sc;
            it->init();
            return std::unique_ptr<It>(it.release());
        }
        case QKind::Near: {
            std::unique_ptr<NearIt> it(new NearIt());
            it->a = build(si, p.kids[0], sc);
            it->b = build(si, p.kids[1], sc);
            it->dist = p.dist;
            it->ordered = p.ordered;
            it->weight = p.weight;
            it->sc = sc;
            it->init();
            return std::unique_ptr<It>(it.release());
        }
        case QKind::Bool: {
            std::unique_ptr<BoolIt> it(new BoolIt());
            it->n_docs = segs[si]->n_docs();
            for (const PNode &k : p.must) {
                std::unique_ptr<It> c = build(si, k, sc);
                if (c->cost() == 0 && dynamic_cast<EmptyIt *>(c.get())) return std::unique_ptr<It>(new EmptyIt());
                it->must.push_back(std::move(c));
            }
            for (const PNode &k : p.should) {
                std::unique_ptr<It> c = build(si, k, sc);
                if (!dynamic_cast<EmptyIt *>(c.get())) it->should.push_back(std::move(c));
            }
            for (const PNode &k : p.must_not) {
                std::unique_ptr<It> c = build(si, k, sc);
                if (!dynamic_cast<EmptyIt *>(c.get())) it->must_not.push_back(std::move(c));
            }
            if (it->must.empty() && it->should.empty() && !p.should.empty()) return std::unique_ptr<It>(new EmptyIt());
            it->init();
            return std::unique_ptr<It>(it.release());
        }
        }
        return std::unique_ptr<It>(new EmptyIt());
    }

    template <class F> void each_segment(F f)
    {
        const std::size_t n = segs.size();
        const std::size_t threads = std::min<std::size_t>(static_cast<std::size_t>(std::max(o.threads, 1)), n);
        if (threads <= 1) { for (std::size_t si = 0; si < n; ++si) f(si); return; }
        std::vector<std::thread> pool;
        std::vector<std::exception_ptr> errors(threads);
        for (std::size_t t = 0; t < threads; ++t)
            pool.emplace_back([&, t] {
                try { for (std::size_t si = t; si < n; si += threads) f(si); }
                catch (...) { errors[t] = std::current_exception(); }
            });
        for (auto &th : pool) th.join();
        for (auto &e : errors) if (e) std::rethrow_exception(e);
    }
};

Searcher::Searcher(const AnalyzerOptions &analyzer_options, std::vector<SegmentRef> segments)
    : aopts_(analyzer_options), segs_(std::move(segments)), analyzer_(new Analyzer(analyzer_options))
{
    masked_.resize(segs_.size());
    for (const SegmentRef &r : segs_) {
        if (!r.seg) throw std::runtime_error("searcher: segment missing");
        if (r.dead && !r.dead_counts) throw std::runtime_error("searcher: dead docs without dead counts");
    }
}

Searcher::~Searcher() = default;

void Searcher::mask(const std::vector<std::int64_t> &doc_ids)
{
    if (prepared_) throw std::runtime_error("searcher: mask after the first search");
    for (std::size_t si = 0; si < segs_.size(); ++si) {
        const Segment &seg = *segs_[si].seg;
        for (std::int64_t id : doc_ids) {
            const std::int64_t d = seg.find_doc(id);
            if (d < 0) continue;
            if (segs_[si].dead && segs_[si].dead->test(static_cast<std::uint32_t>(d))) continue;
            masked_[si].push_back(static_cast<std::uint32_t>(d));
        }
        std::sort(masked_[si].begin(), masked_[si].end());
        masked_[si].erase(std::unique(masked_[si].begin(), masked_[si].end()), masked_[si].end());
    }
}

void Searcher::add_tail(std::shared_ptr<const std::string> bytes)
{
    if (prepared_) throw std::runtime_error("searcher: add_tail after the first search");
    tail_bytes_.push_back(bytes);
    tail_segs_.emplace_back(new Segment(bytes->data(), bytes->size()));
}

void Searcher::prepare()
{
    if (prepared_) return;
    prepared_ = true;
    combined_dead_.resize(segs_.size());
    stats_ = CollectionStats();
    for (std::size_t si = 0; si < segs_.size(); ++si) {
        const Segment &seg = *segs_[si].seg;
        stats_.n_docs += seg.header().n_nonempty;
        stats_.sum_len += seg.sum_len();
        stats_.live_docs += seg.n_docs();
        std::shared_ptr<DocSet> d;
        if (segs_[si].dead && segs_[si].dead->count) {
            if (masked_[si].empty()) d = std::const_pointer_cast<DocSet>(segs_[si].dead);
            else d = std::make_shared<DocSet>(*segs_[si].dead);
        } else if (!masked_[si].empty()) {
            d = std::make_shared<DocSet>();
            d->resize(seg.n_docs());
        }
        // tombstones
        if (segs_[si].dead && segs_[si].dead->count) {
            const DocSet &t = *segs_[si].dead;
            for (std::size_t w = 0; w < t.bits.size(); ++w) {
                std::uint64_t bits = t.bits[w];
                while (bits) {
                    const std::uint32_t doc = static_cast<std::uint32_t>(w * 64 + __builtin_ctzll(bits));
                    bits &= bits - 1;
                    stats_.live_docs -= 1;
                    const std::uint32_t len = seg.doc_len(doc);
                    stats_.sum_len -= len;
                    stats_.n_docs -= len > 0;
                }
            }
        }
        // masked by the live tail
        for (std::uint32_t doc : masked_[si]) {
            d->set(doc);
            stats_.live_docs -= 1;
            const std::uint32_t len = seg.doc_len(doc);
            stats_.sum_len -= len;
            stats_.n_docs -= len > 0;
        }
        combined_dead_[si] = d;
    }
    for (auto &t : tail_segs_) {
        stats_.n_docs += t->header().n_nonempty;
        stats_.sum_len += t->sum_len();
        stats_.live_docs += t->n_docs();
    }
}

std::vector<Hit> Searcher::search(const std::string &query, const SearchOptions &opts)
{
    prepare();
    if (opts.k == 0) return {};
    const QNode q = parse_query(query, *analyzer_, opts.query);
    if (q.kind == QKind::None) return {};
    Impl im(*this, opts);
    const PNode p = im.plan(q);
    const std::size_t n = im.segs.size();
    std::vector<std::vector<Cand>> per(n);
    im.each_segment([&](std::size_t si) {
        const Segment &seg = *im.segs[si];
        Scoring sc{im.cache, seg.norms()};
        std::unique_ptr<It> root = im.build(si, p, sc);
        TopK top(opts.k);
        for (std::uint32_t d = root->next(); d != NO_DOC; d = root->next()) {
            if (im.is_dead(si, d)) continue;
            top.offer(root->score(), seg.doc_id(d));
        }
        per[si] = std::move(top.heap);
    });
    TopK all(opts.k);
    for (auto &v : per) for (const Cand &c : v) all.offer(c.score, c.doc_id);
    std::sort(all.heap.begin(), all.heap.end(), better);
    std::vector<Hit> out;
    out.reserve(all.heap.size());
    for (const Cand &c : all.heap) out.push_back({c.doc_id, c.score});
    return out;
}

std::uint64_t Searcher::count(const std::string &query, const SearchOptions &opts)
{
    prepare();
    const QNode q = parse_query(query, *analyzer_, opts.query);
    if (q.kind == QKind::None) return 0;
    Impl im(*this, opts);
    const PNode p = im.plan(q);
    const std::size_t n = im.segs.size();
    std::vector<std::uint64_t> per(n, 0);
    im.each_segment([&](std::size_t si) {
        Scoring sc{im.cache, im.segs[si]->norms()};
        std::unique_ptr<It> root = im.build(si, p, sc);
        std::uint64_t c = 0;
        for (std::uint32_t d = root->next(); d != NO_DOC; d = root->next()) c += !im.is_dead(si, d);
        per[si] = c;
    });
    std::uint64_t total = 0;
    for (std::uint64_t c : per) total += c;
    return total;
}

std::string Searcher::explain(const std::string &query, const SearchOptions &opts)
{
    return to_string(parse_query(query, *analyzer_, opts.query));
}

} // namespace vtext
