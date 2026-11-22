#include "search.h"
#include "expand.h"
#include "parallel.h"
#include "postings.h"
#include "stemmer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <queue>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <unordered_map>

namespace vfts {

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

struct Span { std::uint32_t start, end; };      // a match, both ends inclusive (snippets)

// A span as Lucene's Spans enumerate it: positions [start, end), and the width of the match, which
// gives its sloppy frequency 1 / (1 + width).
struct LSpan { std::int32_t start, end, width; };
inline bool lspan_less(const LSpan &a, const LSpan &b) { return a.start != b.start ? a.start < b.start : a.end < b.end; }

// Lucene's NearSpansOrdered over the spans of the clauses in one document (each list sorted by
// start, then end): for every span of the first clause, the next clauses move forward (never back)
// to their first span starting at or after the end of the previous one; a match when the gaps
// together are at most slop. The first clause that runs out ends the document.
void near_ordered(const std::vector<const std::vector<LSpan> *> &L, int slop, std::vector<LSpan> &out)
{
    const std::size_t n = L.size();
    std::vector<std::ptrdiff_t> cur(n, -1);
    for (;;) {
        if (++cur[0] >= static_cast<std::ptrdiff_t>(L[0]->size())) return;
        const LSpan *prev = &(*L[0])[static_cast<std::size_t>(cur[0])];
        std::int64_t width = 0;
        for (std::size_t i = 1; i < n; ++i) {
            const std::vector<LSpan> &v = *L[i];
            const auto size = static_cast<std::ptrdiff_t>(v.size());
            while (cur[i] < 0 || (cur[i] < size && v[static_cast<std::size_t>(cur[i])].start < prev->end)) ++cur[i];
            if (cur[i] >= size) return;
            width += v[static_cast<std::size_t>(cur[i])].start - prev->end;
            prev = &v[static_cast<std::size_t>(cur[i])];
        }
        if (width <= slop) out.push_back({(*L[0])[static_cast<std::size_t>(cur[0])].start, prev->end, static_cast<std::int32_t>(width)});
    }
}

// Lucene's NearSpansUnordered for two clauses: a window of one current span per clause; a match
// when (largest end - smallest start - the lengths of the spans) is at most slop, its width is
// (largest end - smallest start); then the clause
// with the smallest span (start, then end; on a tie the current one) moves to its next span. The
// clause that holds the largest end changes only when another end is larger (as in Lucene).
void near_unordered(const std::vector<LSpan> &a, const std::vector<LSpan> &b, int slop, std::vector<LSpan> &out)
{
    if (a.empty() || b.empty()) return;
    const std::vector<LSpan> *L[2] = {&a, &b};
    std::size_t cur[2] = {0, 0};
    auto sp = [&](int c) -> const LSpan & { return (*L[c])[cur[c]]; };
    std::int64_t total = (a[0].end - a[0].start) + (b[0].end - b[0].start);
    int maxc = b[0].end > a[0].end ? 1 : 0;
    int top = lspan_less(b[0], a[0]) ? 1 : 0;
    for (;;) {
        // the match condition subtracts the lengths of the spans; the width of the match, which
        // gives its frequency, does not (Lucene's NearSpansUnordered.width(): end - start)
        if (sp(maxc).end - sp(top).start - total <= slop)
            out.push_back({sp(top).start, sp(maxc).end, sp(maxc).end - sp(top).start});
        const std::int64_t old_len = sp(top).end - sp(top).start;
        if (++cur[top] >= L[top]->size()) return;
        total += (sp(top).end - sp(top).start) - old_len;
        if (top != maxc && sp(top).end > sp(maxc).end) maxc = top;
        const int other = 1 - top;
        if (lspan_less(sp(other), sp(top))) top = other;
    }
}

// One leaf of the query (a word, or one slot of a phrase, or a pattern), resolved for every
// segment.
struct Leaf {
    enum Kind { Word, Pattern } kind = Word;
    bool constant = false;                          // prefix / wildcard: score 1, like Lucene's constant-score rewrite
    std::vector<std::vector<std::uint32_t>> ids;    // per segment
    std::vector<std::int64_t> stem_entry;           // per segment: the stem group when ids is exactly that group
    std::uint64_t df = 0;                           // global live document frequency
    float idf = 0;
    // the terms of the leaf as Lucene's span queries count them in the weight of a w/N or pre/N:
    // (key, idf); a word is one term, a pattern every term it expands to
    std::vector<std::pair<std::string, float>> span_terms;
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
    // the spans of the current document as a clause of w/N or pre/N (Lucene's span queries), sorted
    // by start, then end
    virtual void lspans(std::vector<LSpan> &) {}
    virtual std::uint64_t cost() const = 0;
    // At most how many spans (lspans) the current document gives; ~0u: not known. Bounds the
    // frequency of a phrase or w/N, whose matches each count at most 1.
    virtual std::uint32_t max_spans() { return ~0u; }
    // Top level only (set by the collector): the score a document must reach to enter the top k.
    // Phrases and w/N skip reading positions of documents whose bound stays below it.
    double threshold = -1e300;
};

// The documents of one leaf: one term, or the union of several (stem group, expansion).
struct LeafIt : It {
    std::vector<PostingIter> its;
    // A word of a stem group (several terms): the documents and summed tfs come from the group's
    // own list (grp, one list instead of one per form); the forms' lists in its are moved only to
    // read the positions of a document.
    std::vector<PostingIter> grp;
    float weight = 0;
    bool constant = false;
    Scoring sc;
    std::vector<std::uint32_t> tmp;

    std::uint32_t advance(std::uint32_t target) override
    {
        if (positioned && (doc == NO_DOC || doc >= target)) return doc;
        positioned = true;
        if (!grp.empty()) return doc = grp[0].advance(target);
        std::uint32_t m = NO_DOC;
        for (PostingIter &p : its) m = std::min(m, p.advance(target));
        return doc = m;
    }
    std::uint32_t tf() const
    {
        if (!grp.empty()) return grp[0].doc() == doc ? grp[0].tf() : 0;
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
    std::uint32_t max_spans() override { return tf(); }
    void positions(std::vector<std::uint32_t> &out)
    {
        out.clear();
        std::size_t parts = 0;
        for (PostingIter &p : its) {
            if (!grp.empty()) p.advance(doc);     // never backwards; a no-op when already there
            if (p.doc() == doc) { p.positions(out); ++parts; }
        }
        if (parts > 1) std::sort(out.begin(), out.end());
    }
    void spans(std::vector<Span> &out) override
    {
        positions(tmp);
        for (std::uint32_t p : tmp) out.push_back({p, p});
    }
    void lspans(std::vector<LSpan> &out) override
    {
        positions(tmp);
        for (std::uint32_t p : tmp) out.push_back({static_cast<std::int32_t>(p), static_cast<std::int32_t>(p) + 1, 0});
    }
    std::uint64_t cost() const override
    {
        if (!grp.empty()) return grp[0].df();
        std::uint64_t c = 0;
        for (const PostingIter &p : its) c += p.df();
        return c;
    }
};

// The documents of a constant-score pattern with many terms (prefix / wildcard / fuzzy outside
// w/N), collected once into a bit set: one cheap list instead of advancing every term's list at
// every step (Lucene rewrites such patterns into a bit set as well). Score 1, like the leaf.
struct SetIt : It {
    std::vector<std::uint64_t> bits;
    std::uint32_t n = 0;
    std::uint64_t count = 0;
    std::uint32_t advance(std::uint32_t target) override
    {
        if (positioned && (doc == NO_DOC || doc >= target)) return doc;
        positioned = true;
        if (target >= n) return doc = NO_DOC;
        std::size_t w = target >> 6;
        std::uint64_t x = bits[w] & (~0ull << (target & 63));
        while (!x) { if (++w >= bits.size()) return doc = NO_DOC; x = bits[w]; }
        return doc = static_cast<std::uint32_t>(w * 64 + static_cast<std::size_t>(__builtin_ctzll(x)));
    }
    float score() override { return 1.0f; }
    void spans(std::vector<Span> &) override {}
    std::uint64_t cost() const override { return count; }
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
        for (std::uint32_t d = conjunction(conj, target); d != NO_DOC; d = conjunction(conj, d + 1)) {
            if (threshold > -1e299) {
                // the phrase occurs at most min(tf) times: when even that cannot reach the
                // threshold, the positions are not read (the top k is the same)
                std::uint32_t f = ~0u;
                for (auto &s : slots) f = std::min(f, s->tf());
                const float b = weight - weight / (1.0f + static_cast<float>(f) * sc.cache[sc.norms[d]]);
                if (static_cast<double>(b) * (1.0 + 1e-6) < threshold) continue;
            }
            if (verify()) return doc = d;
        }
        return doc = NO_DOC;
    }
    std::uint32_t max_spans() override
    {
        std::uint64_t f = 0;
        for (auto &x : slots) f += x->tf();
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(f, ~0u - 1));
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
    // As a clause of w/N: an ordered span query of the words with the phrase's slop (Lucene's
    // SpanNearQuery, in order), the form the phrase takes inside a span query.
    void lspans(std::vector<LSpan> &out) override
    {
        if (slop == 0) {
            for (const Span &m : matches) out.push_back({static_cast<std::int32_t>(m.start), static_cast<std::int32_t>(m.end) + 1, 0});
            return;
        }
        std::vector<std::vector<LSpan>> lists(slots.size());
        std::vector<const std::vector<LSpan> *> L;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            for (std::uint32_t p : pos[i]) lists[i].push_back({static_cast<std::int32_t>(p), static_cast<std::int32_t>(p) + 1, 0});
            L.push_back(&lists[i]);
        }
        const std::size_t before = out.size();
        near_ordered(L, slop, out);
        std::stable_sort(out.begin() + static_cast<std::ptrdiff_t>(before), out.end(), lspan_less);
    }
    std::uint64_t cost() const override { return conj.empty() ? 0 : conj[0]->cost(); }
};

// w/N and pre/N: both operands in the document, then Lucene's span matching (SpanNearQuery with
// slop N - 1, in order for pre/N): the frequency is the sum of 1 / (1 + width) over the matches.
struct NearIt : It {
    std::unique_ptr<It> a, b;
    std::vector<It *> conj;
    int dist = 0;
    bool ordered = false;
    float weight = 0;
    Scoring sc;
    float freq = 0;
    std::vector<LSpan> la, lb, matches;

    void init()
    {
        conj = {a.get(), b.get()};
        std::sort(conj.begin(), conj.end(), [](It *x, It *y) { return x->cost() < y->cost(); });
    }
    std::uint32_t advance(std::uint32_t target) override
    {
        if (positioned && (doc == NO_DOC || doc >= target)) return doc;
        positioned = true;
        for (std::uint32_t d = conjunction(conj, target); d != NO_DOC; d = conjunction(conj, d + 1)) {
            if (threshold > -1e299) {
                // every match counts at most 1, and there are at most spans(a) + spans(b) matches
                const std::uint64_t f = static_cast<std::uint64_t>(a->max_spans()) + b->max_spans();
                if (f < ~0u) {
                    const float bd = weight - weight / (1.0f + static_cast<float>(f) * sc.cache[sc.norms[d]]);
                    if (static_cast<double>(bd) * (1.0 + 1e-6) < threshold) continue;
                }
            }
            if (verify()) return doc = d;
        }
        return doc = NO_DOC;
    }
    std::uint32_t max_spans() override
    {
        const std::uint64_t f = static_cast<std::uint64_t>(a->max_spans()) + b->max_spans();
        return f >= ~0u ? ~0u : static_cast<std::uint32_t>(f);
    }
    bool verify()
    {
        la.clear(); lb.clear(); matches.clear();
        a->lspans(la);
        b->lspans(lb);
        if (ordered) near_ordered({&la, &lb}, dist - 1, matches);
        else near_unordered(la, lb, dist - 1, matches);
        freq = 0;
        for (const LSpan &m : matches) freq += 1.0f / (1.0f + static_cast<float>(m.width));
        return !matches.empty();
    }
    float score() override
    {
        const float norm_inverse = sc.cache[sc.norms[doc]];
        return weight - weight / (1.0f + freq * norm_inverse);
    }
    void spans(std::vector<Span> &out) override
    {
        for (const LSpan &m : matches) out.push_back({static_cast<std::uint32_t>(m.start), static_cast<std::uint32_t>(m.end - 1)});
    }
    void lspans(std::vector<LSpan> &out) override
    {
        const std::size_t before = out.size();
        out.insert(out.end(), matches.begin(), matches.end());
        std::stable_sort(out.begin() + static_cast<std::ptrdiff_t>(before), out.end(), lspan_less);
    }
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
    // as a clause of w/N: the spans of the matching should clauses merged (Lucene's SpanOrQuery)
    std::uint32_t max_spans() override
    {
        if (!must.empty() || !must_not.empty()) return ~0u;
        std::uint64_t f = 0;
        for (auto &x : should) {
            if (x->advance(doc) != doc) continue;
            const std::uint32_t m = x->max_spans();
            if (m == ~0u) return ~0u;
            f += m;
        }
        return f >= ~0u ? ~0u : static_cast<std::uint32_t>(f);
    }
    void lspans(std::vector<LSpan> &out) override
    {
        const std::size_t before = out.size();
        for (auto &x : should) if (x->advance(doc) == doc) x->lspans(out);
        std::stable_sort(out.begin() + static_cast<std::ptrdiff_t>(before), out.end(), lspan_less);
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
    float boost;                // 1, or 1 + k1 with bm25='classic' (Elasticsearch's LegacyBM25Similarity)
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
        boost = o.classic ? 1.0f + o.k1 : 1.0f;
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
        L.span_terms.emplace_back((o.match == MatchMode::Stem ? "s\x01" + key : "e\x01" + word), L.idf);
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
                fuzzy_terms(seg.n_terms(), [&](std::uint32_t t) { return seg.term(t); },
                            [&](std::string_view x) { return seg.lower_bound(x); }, pat, q.edits, pre,
                            [&](std::uint32_t t, std::string_view term, int dist) {
                                auto &c = cand[std::string(term)];
                                c.first = dist;
                                c.second += seg.info(t).df;
                            });
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
        std::vector<std::uint64_t> term_df(v.size(), 0);
        for (std::size_t si = 0; si < n; ++si) {
            for (std::size_t j = 0; j < v.size(); ++j) {
                const std::int64_t t = segs[si]->find_term(v[j].first);
                if (t < 0) continue;
                L.ids[si].push_back(static_cast<std::uint32_t>(t));
                term_df[j] += live_df(si, {static_cast<std::uint32_t>(t)}, -1);
            }
            std::sort(L.ids[si].begin(), L.ids[si].end());
        }
        // inside w/N or pre/N every expansion adds its idf to the weight (Lucene's span_multi)
        for (std::size_t j = 0; j < v.size(); ++j)
            if (term_df[j]) L.span_terms.emplace_back("e\x01" + v[j].first, idf(term_df[j]));
        if (L.constant) {
            std::uint64_t max_df = 0;
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
            p.weight = boost * leaves[p.leaves[0]].idf;
            break;
        case QKind::Phrase: {
            double w = 0;
            for (std::size_t i = 0; i < q.words.size(); ++i) {
                if (q.words[i].empty()) { p.leaves.push_back(-1); continue; }
                const int l = add_word_leaf(q.words[i], q.langs[i]);
                p.leaves.push_back(l);
                w += leaves[l].idf;
            }
            p.weight = boost * static_cast<float>(w);
            p.slop = q.slop;
            break;
        }
        case QKind::Prefix: case QKind::Wildcard: case QKind::Fuzzy:
            p.leaves.push_back(add_pattern_leaf(q));
            p.weight = boost * leaves[p.leaves[0]].idf;
            break;
        case QKind::Near: {
            p.dist = q.dist;
            p.ordered = q.ordered;
            p.kids.push_back(plan(q.kids[0]));
            p.kids.push_back(plan(q.kids[1]));
            p.weight = boost * span_weight(p);
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

    // The weight of a w/N or pre/N: the sum of the idf of the distinct terms below it (Lucene's
    // SpanWeight: one statistic per term, the expansions of patterns included).
    void span_terms(const PNode &p, std::map<std::string, float> &m) const
    {
        for (int l : p.leaves) if (l >= 0) for (const auto &t : leaves[l].span_terms) m.emplace(t.first, t.second);
        for (auto *v : {&p.must, &p.should, &p.kids}) for (const PNode &k : *v) span_terms(k, m);
    }
    float span_weight(const PNode &p) const
    {
        std::map<std::string, float> m;
        span_terms(p, m);
        double w = 0;
        for (const auto &t : m) w += t.second;
        return static_cast<float>(w);
    }

    std::unique_ptr<LeafIt> leaf_it(std::size_t si, int l, float weight, const Scoring &sc)
    {
        std::unique_ptr<LeafIt> it(new LeafIt());
        const Leaf &L = leaves[l];
        for (std::uint32_t t : L.ids[si]) it->its.emplace_back(*segs[si], t);
        if (L.stem_entry[si] >= 0 && L.ids[si].size() > 1)
            it->grp.emplace_back(*segs[si], segs[si]->group_info(static_cast<std::uint32_t>(L.stem_entry[si])));
        it->weight = weight;
        it->constant = L.constant;
        it->sc = sc;
        return it;
    }

    std::unique_ptr<It> build(std::size_t si, const PNode &p, const Scoring &sc, bool spans = false)
    {
        switch (p.kind) {
        case QKind::None: return std::unique_ptr<It>(new EmptyIt());
        case QKind::Term: case QKind::Prefix: case QKind::Wildcard: case QKind::Fuzzy: {
            const Leaf &L = leaves[p.leaves[0]];
            if (L.ids[si].empty()) return std::unique_ptr<It>(new EmptyIt());
            if (!spans && L.constant && L.ids[si].size() > 8) {
                std::unique_ptr<SetIt> it(new SetIt());
                it->n = segs[si]->n_docs();
                it->bits.assign((it->n + 63) / 64, 0);
                for (std::uint32_t t : L.ids[si]) {
                    PostingIter pi(*segs[si], t);
                    for (std::uint32_t d = pi.advance(0); d != NO_DOC; d = pi.next()) it->bits[d >> 6] |= 1ull << (d & 63);
                }
                for (std::uint64_t w : it->bits) it->count += static_cast<std::uint64_t>(__builtin_popcountll(w));
                return std::unique_ptr<It>(it.release());
            }
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
            it->a = build(si, p.kids[0], sc, true);
            it->b = build(si, p.kids[1], sc, true);
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
                std::unique_ptr<It> c = build(si, k, sc, spans);
                if (c->cost() == 0 && dynamic_cast<EmptyIt *>(c.get())) return std::unique_ptr<It>(new EmptyIt());
                it->must.push_back(std::move(c));
            }
            for (const PNode &k : p.should) {
                std::unique_ptr<It> c = build(si, k, sc, spans);
                if (!dynamic_cast<EmptyIt *>(c.get())) it->should.push_back(std::move(c));
            }
            for (const PNode &k : p.must_not) {
                std::unique_ptr<It> c = build(si, k, sc, spans);
                if (!dynamic_cast<EmptyIt *>(c.get())) it->must_not.push_back(std::move(c));
            }
            if (it->must.empty() && it->should.empty() && !p.should.empty()) return std::unique_ptr<It>(new EmptyIt());
            it->init();
            return std::unique_ptr<It>(it.release());
        }
        }
        return std::unique_ptr<It>(new EmptyIt());
    }

    // ---- top k of a disjunction of scored words: MaxScore with block-max windows
    //
    // The query is a single word, or words joined by OR (should clauses only). Lists are sorted by
    // their upper bound; the lists whose bounds together stay below the current k-th score
    // ("non-essential") cannot produce a hit alone, so candidates come from the other lists only,
    // and the non-essential lists are probed for those candidates only while the score can still
    // reach the threshold. Whole windows of blocks whose block-max bounds stay below the threshold
    // are skipped. The threshold is shared by the segments searched in parallel. Pruning is strict
    // (a document whose bound equals the threshold is still scored: it may win the tie by doc id),
    // and the final score is summed in clause order, so the result is exactly that of the
    // exhaustive search.
    struct MLeaf {
        std::vector<PostingIter> its;
        float w = 0, ub = 0;
        bool constant = false;
        int clause = 0;
        std::uint32_t doc = NO_DOC;
        std::uint32_t advance(std::uint32_t t)
        {
            if (doc != NO_DOC && doc >= t) return doc;
            std::uint32_t m = NO_DOC;
            for (PostingIter &p : its) m = std::min(m, p.advance(t));
            return doc = m;
        }
        std::uint32_t next() { return doc == NO_DOC ? NO_DOC : advance(doc + 1); }
        float score(const float *cache, const std::uint8_t *norms) const
        {
            if (constant) return 1.0f;
            std::uint32_t tf = 0;
            for (const PostingIter &p : its) if (p.doc() == doc) tf += p.tf();
            const float ni = cache[norms[doc]];
            return w - w / (1.0f + static_cast<float>(tf) * ni);
        }
        // The best score of impacts (exact max over the pairs for one term; for a union of terms
        // the sum of the largest tfs with the smallest norm, which is still an upper bound).
        float bound(const Impacts *const *imps, std::size_t k, const float *cache) const
        {
            if (constant) return 1.0f;
            if (k == 1) {
                const Impacts &im = *imps[0];
                float best = 0.0f;
                for (int i = 0; i < im.n; ++i) best = std::max(best, w - w / (1.0f + static_cast<float>(im.tf[i]) * cache[im.norm[i]]));
                return best;
            }
            std::uint32_t sum_tf = 0;
            std::uint8_t min_norm = 255;
            for (std::size_t j = 0; j < k; ++j) {
                std::uint32_t mt = 0;
                for (int i = 0; i < imps[j]->n; ++i) { mt = std::max(mt, imps[j]->tf[i]); min_norm = std::min(min_norm, imps[j]->norm[i]); }
                sum_tf += mt;
            }
            return w - w / (1.0f + static_cast<float>(sum_tf) * cache[min_norm]);
        }
        float term_bound(const float *cache) const
        {
            std::vector<const Impacts *> v;
            for (const PostingIter &p : its) v.push_back(&p.term_impacts());
            return bound(v.data(), v.size(), cache);
        }
        // bound of the score of every document in [t, upto] (the term bound when that needs too many entries)
        float range_bound(std::uint32_t t, std::uint32_t upto, const float *cache) const
        {
            if (constant) return 1.0f;
            if (its.size() == 1) {
                float best = 0.0f;
                const bool ok = its[0].cover(t, upto, 8, [&](const Impacts &im) {
                    for (int i = 0; i < im.n; ++i) best = std::max(best, w - w / (1.0f + static_cast<float>(im.tf[i]) * cache[im.norm[i]]));
                });
                return ok ? best : ub;
            }
            std::uint32_t sum_tf = 0;
            std::uint8_t min_norm = 255;
            for (const PostingIter &p : its) {
                std::uint32_t mt = 0;
                const bool ok = p.cover(t, upto, 8, [&](const Impacts &im) {
                    for (int i = 0; i < im.n; ++i) { mt = std::max(mt, im.tf[i]); min_norm = std::min(min_norm, im.norm[i]); }
                });
                if (!ok) return ub;
                sum_tf += mt;
            }
            if (sum_tf == 0) return 0.0f;
            return w - w / (1.0f + static_cast<float>(sum_tf) * cache[min_norm]);
        }
        // bound of the score of every document in [t, *end]
        float block_bound(std::uint32_t t, std::uint32_t &end, const float *cache) const
        {
            const Impacts *v[64];
            std::size_t k = 0;
            end = NO_DOC;
            for (const PostingIter &p : its) {
                std::uint32_t last;
                const Impacts *imp;
                if (!p.shallow(t, last, imp)) continue;
                if (k < 64) v[k++] = imp;
                end = std::min(end, last);
            }
            if (k == 0) return 0.0f;
            if (k == 64) return ub;
            return bound(v, k, cache);
        }
    };

    static bool disjunction_of_leaves(const PNode &p)
    {
        auto leafy = [](const PNode &x) {
            return x.kind == QKind::Term || x.kind == QKind::Fuzzy || x.kind == QKind::Prefix || x.kind == QKind::Wildcard;
        };
        if (leafy(p)) return true;
        if (p.kind != QKind::Bool || !p.must.empty() || !p.must_not.empty() || p.should.empty()) return false;
        for (const PNode &k : p.should) if (!leafy(k)) return false;
        return true;
    }

    void maxscore(std::size_t si, const PNode &p, TopK &top, std::atomic<float> &shared, std::uint32_t lo, std::uint32_t hi)
    {
        const Segment &seg = *segs[si];
        const std::uint8_t *norms = seg.norms();
        std::vector<const PNode *> clauses;
        if (p.kind == QKind::Bool) for (const PNode &k : p.should) clauses.push_back(&k);
        else clauses.push_back(&p);
        std::vector<MLeaf> m;
        for (std::size_t c = 0; c < clauses.size(); ++c) {
            const Leaf &L = leaves[clauses[c]->leaves[0]];
            if (L.ids[si].empty()) continue;
            MLeaf x;
            x.clause = static_cast<int>(c);
            x.w = clauses[c]->weight;
            x.constant = L.constant;
            if (L.stem_entry[si] >= 0 && L.ids[si].size() > 1) {
                // the stem group's own postings: one list with the summed tf (same scores)
                x.its.emplace_back(seg, seg.group_info(static_cast<std::uint32_t>(L.stem_entry[si])));
                x.its.back().prefetch_norms(norms);
            } else {
                for (std::uint32_t t : L.ids[si]) { x.its.emplace_back(seg, t); x.its.back().prefetch_norms(norms); }
            }
            x.ub = x.term_bound(cache);
            x.advance(lo);
            m.push_back(std::move(x));
        }
        if (m.empty()) return;
        std::sort(m.begin(), m.end(), [](const MLeaf &a, const MLeaf &b) { return a.ub < b.ub; });
        if (std::getenv("VFTS_DEBUG")) {
            for (const MLeaf &x : m) std::fprintf(stderr, "  list clause %d df %u w %.3f ub %.3f\n", x.clause, x.its[0].df(), x.w, x.ub);
        }
        const std::size_t n = m.size();
        std::vector<double> prefix(n + 1, 0.0);
        for (std::size_t i = 0; i < n; ++i) prefix[i + 1] = prefix[i] + m[i].ub;
        std::vector<double> contrib(clauses.size(), 0.0);
        const double slack = 1.0 + 1e-6;
        auto theta = [&]() -> double {
            const double sh = shared.load(std::memory_order_relaxed);
            const double lo = top.heap.size() >= top.k ? top.heap.front().score : -1e300;
            return std::max(sh, lo);
        };
        double th = theta();
        // Windows of o.window docnos (default 8192). In each window every list has a bound from its impacts; sorted by
        // those bounds, the lists whose bounds together stay below the threshold are probed only
        // ("non-essential" in the window), and candidates come from the others. A window whose lists
        // together stay below the threshold is skipped. Before the top k is full, every list is
        // essential.
        std::uint64_t c_cand = 0, c_scored = 0, c_skip = 0, c_probe = 0;
        struct Flush {
            SearchOptions::Stats *st; std::uint64_t &a, &b, &c, &d; std::size_t terms;
            ~Flush() { if (st) { st->candidates += a; st->scored += b; st->windows_skipped += c; st->probes += d; st->terms += terms; } }
        } flush{o.stats, c_cand, c_scored, c_skip, c_probe, n};
        std::vector<int> touched;
        touched.reserve(n);
        constexpr std::uint32_t TW = 8192;      // term-at-a-time windows: scores and a bit per document
        std::vector<double> acc;
        std::vector<std::uint64_t> bits;
        if (n >= 2) { acc.assign(TW, 0.0); bits.assign(TW / 64, 0); }
        const DocSet *dset = dead[si];
        std::vector<double> wb(n), wp(n + 1, 0.0);
        std::vector<std::size_t> perm(n);       // lists by window bound, ascending
        for (std::size_t i = 0; i < n; ++i) perm[i] = i;
        std::size_t ne = 0;                     // perm[0 .. ne) are non-essential in this window
        auto set_ne = [&]() { while (ne < n && wp[ne + 1] * slack < th) ++ne; };
        auto offer = [&](std::uint32_t d, double sc) {
            // probes the non-essential lists, largest bound first, while the score can still reach the threshold
            for (std::size_t j = ne; j-- > 0;) {
                if ((sc + wp[j + 1]) * slack < th) { for (int c : touched) contrib[c] = 0.0; touched.clear(); return; }
                MLeaf &x = m[perm[j]];
                ++c_probe;
                if (x.advance(d) == d) { const float c = x.score(cache, norms); contrib[x.clause] = c; touched.push_back(x.clause); sc += c; }
            }
            double sum = 0;
            if (touched.size() == 1) sum = contrib[touched[0]];
            else for (double c : contrib) sum += c;
            for (int c : touched) contrib[c] = 0.0;
            touched.clear();
            top.offer(static_cast<float>(sum), seg.doc_id(d));
            if (top.heap.size() >= top.k) {
                const float kth = top.heap.front().score;
                float cur = shared.load(std::memory_order_relaxed);
                while (kth > cur && !shared.compare_exchange_weak(cur, kth)) {}
                if (kth > th) { th = kth; set_ne(); }
            }
        };
        const std::uint32_t nd = hi;            // this task's docnos: [lo, hi)
        const std::uint32_t W = o.window ? o.window : 8192;
        for (std::uint32_t ws = lo; ws < nd;) {
            th = std::max(th, theta());
            const std::uint32_t we = nd - ws > W ? ws + W - 1 : nd - 1;
            if (th > -1e299) {
                // bounds from the impacts: no list is decoded or moved for this
                for (std::size_t i = 0; i < n; ++i) wb[i] = m[i].range_bound(ws, we, cache);
                std::sort(perm.begin(), perm.end(), [&](std::size_t a, std::size_t b) { return wb[a] < wb[b]; });
                for (std::size_t j = 0; j < n; ++j) wp[j + 1] = wp[j] + wb[perm[j]];
                ne = 0;
                set_ne();
                if (ne >= n) { ++c_skip; ws = we + 1; continue; }
            } else {
                for (std::size_t j = 0; j < n; ++j) { perm[j] = j; wp[j + 1] = wp[j] + m[j].ub; }
                ne = 0;
            }
            for (std::size_t j = ne; j < n; ++j) m[perm[j]].advance(ws);
            if (ne == n - 1 && m[perm[ne]].its.size() == 1 && !m[perm[ne]].constant) {
                // one essential list in the window (the usual state once the threshold has risen)
                MLeaf &x = m[perm[ne]];
                PostingIter &it = x.its[0];
                const float w = x.w;
                std::uint32_t d = x.doc;
                while (d <= we) {
                    ++c_cand;
                    if (!(dset && dset->test(d))) {
                        const float c = w - w / (1.0f + static_cast<float>(it.tf()) * cache[norms[d]]);
                        if ((c + wp[ne]) * slack >= th) {
                            ++c_scored;
                            contrib[x.clause] = c;
                            touched.push_back(x.clause);
                            x.doc = d;
                            offer(d, c);
                            if (ne >= n) break;
                            if (ne != n - 1) { d = it.next(); x.doc = d; goto general; }
                        }
                    }
                    d = it.next();
                }
                x.doc = d;
                ws = we + 1;
                continue;
            }
            if (n - ne >= 2 && we - ws < TW) {
                // several essential lists: term at a time over the window (Lucene's MaxScore does
                // this too). Each list adds its contributions into acc; then every document with a
                // bit set is a candidate. The sums are exact in double (a few float terms), so the
                // scores are the same as document at a time. The window keeps its essential set:
                // a later rise of the threshold only makes the check below stricter.
                bool simple = true;
                for (std::size_t j = ne; j < n && simple; ++j) simple = m[perm[j]].its.size() == 1;
                if (simple) {
                    const std::size_t nw = ne;
                    for (std::size_t j = nw; j < n; ++j) {
                        MLeaf &x = m[perm[j]];
                        PostingIter &it = x.its[0];
                        const float w = x.w;
                        std::uint32_t d = x.doc;
                        while (d <= we) {
                            const std::uint32_t o = d - ws;
                            const float c = x.constant ? 1.0f : w - w / (1.0f + static_cast<float>(it.tf()) * cache[norms[d]]);
                            acc[o] += c;
                            bits[o >> 6] |= 1ull << (o & 63);
                            d = it.next();
                        }
                        x.doc = d;
                    }
                    const std::size_t nwords = ((we - ws) >> 6) + 1;
                    for (std::size_t wi = 0; wi < nwords; ++wi) {
                        std::uint64_t b = bits[wi];
                        bits[wi] = 0;
                        while (b) {
                            const std::uint32_t o = static_cast<std::uint32_t>(wi * 64 + static_cast<std::size_t>(__builtin_ctzll(b)));
                            b &= b - 1;
                            double sc = acc[o];
                            acc[o] = 0.0;
                            const std::uint32_t d = ws + o;
                            ++c_cand;
                            if (dset && dset->test(d)) continue;
                            if ((sc + wp[nw]) * slack < th) continue;
                            bool out = false;
                            for (std::size_t j = nw; j-- > 0;) {
                                if ((sc + wp[j + 1]) * slack < th) { out = true; break; }
                                MLeaf &x = m[perm[j]];
                                ++c_probe;
                                if (x.advance(d) == d) sc += x.score(cache, norms);
                            }
                            if (out) continue;
                            ++c_scored;
                            top.offer(static_cast<float>(sc), seg.doc_id(d));
                            if (top.heap.size() >= top.k) {
                                const float kth = top.heap.front().score;
                                float cur = shared.load(std::memory_order_relaxed);
                                while (kth > cur && !shared.compare_exchange_weak(cur, kth)) {}
                                if (kth > th) th = kth;
                            }
                        }
                    }
                    ws = we + 1;
                    continue;
                }
            }
        general:
            // candidates in [ws, we] from the essential lists
            for (;;) {
                std::uint32_t d = NO_DOC;
                for (std::size_t j = ne; j < n; ++j) d = std::min(d, m[perm[j]].doc);
                if (d > we) break;
                ++c_cand;
                if (!(dset && dset->test(d))) {
                    double sc = 0;
                    for (std::size_t j = ne; j < n; ++j) {
                        MLeaf &x = m[perm[j]];
                        if (x.doc == d) { const float c = x.score(cache, norms); contrib[x.clause] = c; touched.push_back(x.clause); sc += c; }
                    }
                    if ((sc + wp[ne]) * slack >= th) { ++c_scored; offer(d, sc); }
                    else { for (int c : touched) contrib[c] = 0.0; touched.clear(); }
                    if (ne >= n) break;     // the threshold rose so far that nothing in the window can reach it
                }
                for (std::size_t j = ne; j < n; ++j) { MLeaf &x = m[perm[j]]; if (x.doc == d) x.next(); }
            }
            ws = we + 1;
        }
    }

    // Bound of a phrase's score in [ws, we]: for every length level (norm) n, a document of norm n
    // has tf_t <= the largest tf among word t's impacts with norm <= n, and the phrase's frequency
    // is at most the smallest of these over the words (Lucene merges phrase impacts the same way).
    float phrase_bound(const std::vector<MLeaf> &b, float w, std::uint32_t ws, std::uint32_t we) const
    {
        struct Pair { std::uint8_t norm; std::uint32_t tf; };
        std::vector<std::vector<Pair>> per(b.size());
        for (std::size_t t = 0; t < b.size(); ++t) {
            if (b[t].its.size() != 1) {     // a union of terms: the simpler bound
                float x = std::numeric_limits<float>::infinity();
                for (const MLeaf &m : b) x = std::min(x, m.range_bound(ws, we, cache));
                return x;
            }
            std::vector<Pair> &v = per[t];
            const bool ok = b[t].its[0].cover(ws, we, 8, [&](const Impacts &im) {
                for (int i = 0; i < im.n; ++i) v.push_back({im.norm[i], im.tf[i]});
            });
            if (!ok) return b[t].ub;
            if (v.empty()) return 0.0f;     // the word has no document here
            std::sort(v.begin(), v.end(), [](const Pair &x, const Pair &y) { return x.norm < y.norm; });
            for (std::size_t i = 1; i < v.size(); ++i) v[i].tf = std::max(v[i].tf, v[i - 1].tf);   // largest tf with norm <= n
        }
        float best = 0.0f;
        for (std::size_t t = 0; t < per.size(); ++t)
            for (const Pair &c : per[t]) {
                std::uint32_t f = c.tf;
                for (std::size_t u = 0; u < per.size() && f; ++u) {
                    if (u == t) continue;
                    const std::vector<Pair> &v = per[u];
                    // largest tf of word u with norm <= c.norm
                    std::size_t lo = 0, hi = v.size();
                    while (lo < hi) { const std::size_t mid = (lo + hi) / 2; if (v[mid].norm <= c.norm) lo = mid + 1; else hi = mid; }
                    f = std::min<std::uint32_t>(f, lo ? v[lo - 1].tf : 0);
                }
                if (f) best = std::max(best, w - w / (1.0f + static_cast<float>(f) * cache[c.norm]));
            }
        return best;
    }

    // Top k of a query that is not a disjunction of leaves, from slice [lo, hi) of segment si. A
    // top-level phrase skips windows of documents whose bound is below the threshold: a phrase
    // occurs in a document at most as often as each of its words, so with the phrase's weight the
    // best score a word allows in the window (its impacts) bounds the phrase's score there, and the
    // smallest of these bounds is the window's bound (Lucene skips phrase blocks with impacts too).
    void scored(std::size_t si, const PNode &p, TopK &top, std::atomic<float> &shared, std::uint32_t lo, std::uint32_t hi)
    {
        const Segment &seg = *segs[si];
        Scoring sc{cache, seg.norms()};
        std::unique_ptr<It> root = build(si, p, sc);
        std::vector<MLeaf> bounds;
        if (o.pruning && (p.kind == QKind::Phrase || p.kind == QKind::Near) && !dynamic_cast<EmptyIt *>(root.get())) {
            for (int l : p.leaves) {
                if (l < 0) continue;
                const Leaf &L = leaves[static_cast<std::size_t>(l)];
                MLeaf x;
                x.w = p.weight;
                if (L.stem_entry[si] >= 0 && L.ids[si].size() > 1) x.its.emplace_back(seg, seg.group_info(static_cast<std::uint32_t>(L.stem_entry[si])));
                else for (std::uint32_t t : L.ids[si]) x.its.emplace_back(seg, t);
                x.ub = x.term_bound(cache);
                bounds.push_back(std::move(x));
            }
        }
        It *ph = o.pruning && (p.kind == QKind::Phrase || p.kind == QKind::Near) ? root.get() : nullptr;
        if (!ph) {
            for (std::uint32_t d = root->advance(lo); d < hi; d = root->next()) {
                if (is_dead(si, d)) continue;
                top.offer(root->score(), seg.doc_id(d));
            }
            return;
        }
        const double slack = 1.0 + 1e-6;
        const std::uint32_t W = o.window ? o.window : 4096;
        double th = -1e300;
        for (std::uint32_t ws = lo; ws < hi;) {
            const std::uint32_t we = hi - ws > W ? ws + W - 1 : hi - 1;
            th = std::max<double>(th, shared.load(std::memory_order_relaxed));
            if (th > -1e299 && !bounds.empty() && phrase_bound(bounds, p.weight, ws, we) * slack < th) { ws = we + 1; continue; }
            if (ph) ph->threshold = th;
            for (std::uint32_t d = root->advance(ws); d <= we; d = root->next()) {
                if (is_dead(si, d)) continue;
                top.offer(root->score(), seg.doc_id(d));
                if (top.heap.size() >= top.k) {
                    const float kth = top.heap.front().score;
                    if (kth > th) { th = kth; if (ph) ph->threshold = th; }
                    float cur = shared.load(std::memory_order_relaxed);
                    while (kth > cur && !shared.compare_exchange_weak(cur, kth)) {}
                }
            }
            ws = we + 1;
        }
    }

    // Estimated work of a planned query: the postings it reads (positions count more).
    double cost(const PNode &p) const
    {
        double c = 0;
        for (int l : p.leaves) if (l >= 0) c += static_cast<double>(leaves[static_cast<std::size_t>(l)].df);
        if (p.kind == QKind::Phrase || p.kind == QKind::Near) c *= 4;
        for (const auto *v : {&p.must, &p.should, &p.must_not, &p.kids}) for (const PNode &k : *v) c += cost(k);
        return c;
    }

    // Work items: every segment, cut into slices of docnos so that one large segment is searched by
    // several threads too (each slice has its own top k; they share the threshold).
    struct Task { std::size_t si; std::uint32_t lo, hi; };
    std::vector<Task> tasks() const
    {
        std::vector<Task> t;
        std::uint64_t total = 0;
        for (const Segment *s : segs) total += s->n_docs();
        const std::size_t threads = static_cast<std::size_t>(std::max(o.threads, 1));
        constexpr std::uint32_t MIN_SLICE = 16384, ALIGN = 8192;
        for (std::size_t si = 0; si < segs.size(); ++si) {
            const std::uint32_t nd = segs[si]->n_docs();
            std::uint64_t k = total ? (threads * static_cast<std::uint64_t>(nd) + total - 1) / total : 1;
            k = std::max<std::uint64_t>(1, std::min<std::uint64_t>(k, nd / MIN_SLICE));
            std::uint32_t lo = 0;
            for (std::uint64_t j = 1; j <= k; ++j) {
                std::uint32_t hi = j == k ? nd : static_cast<std::uint32_t>((static_cast<std::uint64_t>(nd) * j / k) / ALIGN * ALIGN);
                if (hi <= lo) continue;
                t.push_back({si, lo, hi});
                lo = hi;
            }
            if (nd == 0) t.push_back({si, 0, 0});
        }
        return t;
    }

    template <class F> void each_task(const std::vector<Task> &t, F f)
    {
        const std::function<void(std::size_t)> fn = f;
        Pool::instance().run(t.size(), static_cast<std::size_t>(std::max(o.threads, 1)), fn);
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
    std::vector<std::int64_t> ids(doc_ids);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    std::vector<std::uint32_t> found;
    for (std::size_t si = 0; si < segs_.size(); ++si) {
        const Segment &seg = *segs_[si].seg;
        found.clear();
        seg.find_docs(ids.data(), ids.size(), found);
        for (std::uint32_t d : found) {
            if (segs_[si].dead && segs_[si].dead->test(d)) continue;
            masked_[si].push_back(d);
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
    return run_search(q, opts);
}

std::vector<Hit> Searcher::search(const std::string &query, const SearchOptions &opts, Analyzer &analyzer)
{
    if (!prepared_) throw std::runtime_error("searcher: ready() before searching from several threads");
    if (opts.k == 0) return {};
    return run_search(parse_query(query, analyzer, opts.query), opts);
}

std::vector<Hit> Searcher::run_search(const QNode &q, const SearchOptions &opts_in)
{
    if (q.kind == QKind::None || opts_in.k == 0) return {};
    // the threads this query gets now (the pool's fair share), so that the documents are cut into
    // that many slices only
    SearchOptions opts = opts_in;
    opts.threads = static_cast<int>(Pool::instance().share(static_cast<std::size_t>(std::max(opts.threads, 1))));
    const auto t_plan = std::chrono::steady_clock::now();
    Impl im(*this, opts);
    const PNode p = im.plan(q);
    const auto t_exec = std::chrono::steady_clock::now();
    if (opts.stats) opts.stats->plan_us += std::chrono::duration<double, std::micro>(t_exec - t_plan).count();
    const std::vector<Impl::Task> tasks = im.tasks();
    std::vector<std::vector<Cand>> per(tasks.size());
    const bool fast = opts.pruning && Impl::disjunction_of_leaves(p);
    std::atomic<float> shared(-std::numeric_limits<float>::infinity());
    im.each_task(tasks, [&](std::size_t ti) {
        const Impl::Task &t = tasks[ti];
        const std::size_t si = t.si;
        TopK top(opts.k);
        if (t.hi > t.lo) {
            if (fast) im.maxscore(si, p, top, shared, t.lo, t.hi);
            else im.scored(si, p, top, shared, t.lo, t.hi);
        }
        per[ti] = std::move(top.heap);
    });
    if (std::getenv("VFTS_DEBUG")) std::fprintf(stderr, "  threshold at the end: %.3f\n", shared.load());
    if (opts.stats) opts.stats->exec_us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t_exec).count();
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
    return run_count(q, opts);
}

std::vector<std::int64_t> Searcher::matches(const std::string &query, const SearchOptions &opts_in)
{
    prepare();
    const QNode q = parse_query(query, *analyzer_, opts_in.query);
    if (q.kind == QKind::None) return {};
    SearchOptions opts = opts_in;
    opts.threads = static_cast<int>(Pool::instance().share(static_cast<std::size_t>(std::max(opts.threads, 1))));
    Impl im(*this, opts);
    const PNode p = im.plan(q);
    const std::vector<Impl::Task> tasks = im.tasks();
    std::vector<std::vector<std::int64_t>> per(tasks.size());
    im.each_task(tasks, [&](std::size_t ti) {
        const Impl::Task &t = tasks[ti];
        if (t.hi <= t.lo) return;
        Scoring sc{im.cache, im.segs[t.si]->norms()};
        std::unique_ptr<It> root = im.build(t.si, p, sc);
        const Segment &seg = *im.segs[t.si];
        for (std::uint32_t d = root->advance(t.lo); d < t.hi; d = root->next())
            if (!im.is_dead(t.si, d)) per[ti].push_back(seg.doc_id(d));
    });
    std::vector<std::int64_t> out;
    for (auto &v : per) out.insert(out.end(), v.begin(), v.end());
    return out;
}

std::uint64_t Searcher::run_count(const QNode &q, const SearchOptions &opts_in)
{
    if (q.kind == QKind::None) return 0;
    SearchOptions opts = opts_in;
    opts.threads = static_cast<int>(Pool::instance().share(static_cast<std::size_t>(std::max(opts.threads, 1))));
    Impl im(*this, opts);
    const PNode p = im.plan(q);
    const std::vector<Impl::Task> tasks = im.tasks();
    std::vector<std::uint64_t> per(tasks.size(), 0);
    im.each_task(tasks, [&](std::size_t ti) {
        const Impl::Task &t = tasks[ti];
        if (t.hi <= t.lo) return;
        Scoring sc{im.cache, im.segs[t.si]->norms()};
        std::unique_ptr<It> root = im.build(t.si, p, sc);
        std::uint64_t c = 0;
        for (std::uint32_t d = root->advance(t.lo); d < t.hi; d = root->next()) c += !im.is_dead(t.si, d);
        per[ti] = c;
    });
    std::uint64_t total = 0;
    for (std::uint64_t c : per) total += c;
    return total;
}

// A batch: the queries are parsed here, one after another (the analyzer is not shared), planned in
// parallel, and then every (query, slice of documents) piece of the whole batch is one work item
// of the pool. A slow query is cut into slices like a query alone, so the batch takes about its
// total work divided by the threads, not the time of its slowest query. Results in query order.
std::vector<std::vector<Hit>> Searcher::search_many(const std::vector<std::string> &queries, const SearchOptions &opts)
{
    prepare();
    std::vector<QNode> qs;
    qs.reserve(queries.size());
    for (const std::string &q : queries) qs.push_back(parse_query(q, *analyzer_, opts.query));
    std::vector<std::vector<Hit>> out(qs.size());
    if (qs.size() == 1) { out[0] = run_search(qs[0], opts); return out; }
    if (opts.k == 0) return out;
    const std::size_t threads = Pool::instance().share(static_cast<std::size_t>(std::max(opts.threads, 1)));
    struct Prep {
        SearchOptions o;
        std::unique_ptr<Impl> im;
        PNode p;
        std::vector<Impl::Task> tasks;
        std::vector<std::vector<Cand>> per;
        std::atomic<float> shared{-std::numeric_limits<float>::infinity()};
        bool fast = false, none = false;
        double cost = 0;
    };
    std::vector<std::unique_ptr<Prep>> prep(qs.size());
    for (std::size_t i = 0; i < qs.size(); ++i) {
        prep[i].reset(new Prep);
        prep[i]->o = opts;
        prep[i]->o.threads = static_cast<int>(threads);     // slices as for a query alone
        prep[i]->o.stats = nullptr;
        prep[i]->none = qs[i].kind == QKind::None;
    }
    const std::function<void(std::size_t)> plan_fn = [&](std::size_t i) {
        Prep &x = *prep[i];
        if (x.none) return;
        x.im.reset(new Impl(*this, x.o));
        x.p = x.im->plan(qs[i]);
        x.fast = x.o.pruning && Impl::disjunction_of_leaves(x.p);
        x.cost = x.im->cost(x.p);
    };
    Pool::instance().run(qs.size(), threads, plan_fn);
    // Slices only for the expensive queries: a query whose estimated work is more than a share of
    // the batch's is cut into as many slices (more slices cost more work in all, so cheap queries
    // stay whole). The largest pieces go first, so that no long piece starts last.
    double total = 0;
    for (auto &x : prep) total += x->cost;
    const double target = std::max(1.0, total / static_cast<double>(threads * 2));
    std::vector<std::pair<double, std::pair<std::uint32_t, std::uint32_t>>> order;     // (cost of the piece, (query, task))
    for (std::size_t i = 0; i < qs.size(); ++i) {
        Prep &x = *prep[i];
        if (x.none) continue;
        // matches are often bunched in a few ranges of documents: an expensive query gets twice the
        // slices it needs, so that the pool can balance them
        double sl = std::max(1.0, std::ceil(x.cost / target));
        if (sl > 1) sl = std::min<double>(static_cast<double>(threads) * 2, sl * 2);
        x.o.threads = static_cast<int>(sl);
        x.tasks = x.im->tasks();
        x.per.resize(x.tasks.size());
        for (std::size_t t = 0; t < x.tasks.size(); ++t)
            order.push_back({x.cost / static_cast<double>(x.tasks.size()), {static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(t)}});
    }
    std::stable_sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    std::vector<std::pair<std::uint32_t, std::uint32_t>> items;     // (query, task)
    items.reserve(order.size());
    for (auto &o2 : order) items.push_back(o2.second);
    const std::function<void(std::size_t)> run_fn = [&](std::size_t it) {
        Prep &x = *prep[items[it].first];
        const std::size_t ti = items[it].second;
        const Impl::Task &t = x.tasks[ti];
        Impl &im = *x.im;
        TopK top(x.o.k);
        if (t.hi > t.lo) {
            if (x.fast) im.maxscore(t.si, x.p, top, x.shared, t.lo, t.hi);
            else im.scored(t.si, x.p, top, x.shared, t.lo, t.hi);
        }
        x.per[ti] = std::move(top.heap);
    };
    Pool::instance().run(items.size(), threads, run_fn);
    for (std::size_t i = 0; i < qs.size(); ++i) {
        TopK all(opts.k);
        for (auto &v : prep[i]->per) for (const Cand &c : v) all.offer(c.score, c.doc_id);
        std::sort(all.heap.begin(), all.heap.end(), better);
        out[i].reserve(all.heap.size());
        for (const Cand &c : all.heap) out[i].push_back({c.doc_id, c.score});
    }
    return out;
}

std::vector<std::uint64_t> Searcher::count_many(const std::vector<std::string> &queries, const SearchOptions &opts)
{
    prepare();
    std::vector<QNode> qs;
    qs.reserve(queries.size());
    for (const std::string &q : queries) qs.push_back(parse_query(q, *analyzer_, opts.query));
    std::vector<std::uint64_t> out(qs.size(), 0);
    if (qs.size() == 1) { out[0] = run_count(qs[0], opts); return out; }
    SearchOptions one = opts;
    one.threads = 1;
    one.stats = nullptr;
    const std::function<void(std::size_t)> fn = [&](std::size_t i) { out[i] = run_count(qs[i], one); };
    Pool::instance().run(qs.size(), static_cast<std::size_t>(std::max(opts.threads, 1)), fn);
    return out;
}

std::string Searcher::explain(const std::string &query, const SearchOptions &opts)
{
    return to_string(parse_query(query, *analyzer_, opts.query));
}

} // namespace vfts
