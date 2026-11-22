// The search engine against a brute-force reference: random corpora, random queries (boolean,
// phrases, w/N, pre/N; exact and stem match), one segment, several segments with tombstones and a
// live tail. Same matches, same order, same scores (correctness rules 1, 2 and 4).
#include "check.h"
#include "../../src/engine/builder.h"
#include "../../src/engine/expand.h"
#include "../../src/engine/postings.h"
#include "../../src/engine/search.h"
#include "../../src/engine/stemmer.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>

using namespace vfts;
using Doc = std::vector<std::string>;
using Corpus = std::map<std::int64_t, Doc>;

static const char *VOCAB[] = {"run", "runs", "running", "contract", "contracts", "indemnity", "holmes", "watson",
                              "the", "of", "a", "b", "c", "d", "e", "f", "g", "h"};
constexpr int NV = sizeof(VOCAB) / sizeof(VOCAB[0]);

static AnalyzerOptions aopts()
{
    AnalyzerOptions o;
    o.default_lang = parse_lang("en");
    return o;
}

// ------------------------------------------------------------------ reference

struct Ref {
    const Corpus &docs;
    MatchMode match;
    double N = 0, sum = 0;
    float cache[256];
    Stemmer st;

    Ref(const Corpus &c, MatchMode m) : docs(c), match(m)
    {
        for (auto &kv : docs) if (!kv.second.empty()) { N += 1; sum += kv.second.size(); }
        const float avgdl = N ? static_cast<float>(sum / N) : 1.0f;
        for (int i = 0; i < 256; ++i)
            cache[i] = 1.0f / (1.2f * ((1.0f - 0.75f) + 0.75f * static_cast<float>(norm_to_length(static_cast<std::uint8_t>(i))) / avgdl));
    }

    std::set<std::string> terms_of(const std::string &w)
    {
        std::set<std::string> s{w};
        if (match == MatchMode::Stem) {
            const std::string sw(st.stem(w, parse_lang("en")));
            for (int i = 0; i < NV; ++i) if (std::string(st.stem(VOCAB[i], parse_lang("en"))) == sw) s.insert(VOCAB[i]);
        }
        return s;
    }
    float idf(const std::set<std::string> &ts)
    {
        double df = 0;
        for (auto &kv : docs) {
            for (auto &w : kv.second) if (ts.count(w)) { df += 1; break; }
        }
        return static_cast<float>(std::log(1.0 + (N - df + 0.5) / (df + 0.5)));
    }
    float bm25(float idf, float freq, std::size_t len)
    {
        const float w = idf;            // Lucene's BM25Similarity (OpenSearch)
        return w - w / (1.0f + freq * cache[length_to_norm(static_cast<std::uint32_t>(len))]);
    }
    static std::vector<std::uint32_t> positions(const Doc &d, const std::set<std::string> &ts)
    {
        std::vector<std::uint32_t> p;
        for (std::uint32_t i = 0; i < d.size(); ++i) if (ts.count(d[i])) p.push_back(i);
        return p;
    }
    struct Sp { std::uint32_t s, e; };

    // spans of a phrase (both ends inclusive)
    std::vector<Sp> phrase_spans(const QNode &q, const Doc &d)
    {
        std::vector<Sp> out;
        for (std::uint32_t s = 0; s < d.size(); ++s) {
            bool ok = true;
            for (std::size_t i = 0; i < q.words.size() && ok; ++i) {
                if (q.words[i].empty()) continue;
                ok = s + i < d.size() && terms_of(q.words[i]).count(d[s + i]);
            }
            if (ok) out.push_back({s, static_cast<std::uint32_t>(s + q.words.size() - 1)});
        }
        return out;
    }

    // Lucene's span model, written after its Java classes: a Spans is a list of [start, end) with a
    // width, walked forward with next_start(); start() is -1 before the first, NO_MORE after the last.
    struct LS { int s, e, w; };
    static constexpr int NO_MORE = 1 << 30;
    struct Spans {
        std::vector<LS> v;
        long at = -1;
        int start() const { return at < 0 ? -1 : (at >= static_cast<long>(v.size()) ? NO_MORE : v[at].s); }
        int end() const { return at < 0 ? -1 : (at >= static_cast<long>(v.size()) ? NO_MORE : v[at].e); }
        int next_start() { ++at; return start(); }
    };
    static bool ordered_pos(const Spans &x, const Spans &y) { return x.start() == y.start() ? x.end() < y.end() : x.start() < y.start(); }

    std::vector<LS> lspans(const QNode &q, const Doc &d)
    {
        std::vector<LS> out;
        if (q.kind == QKind::Term) {
            for (auto p : positions(d, terms_of(q.words[0]))) out.push_back({static_cast<int>(p), static_cast<int>(p) + 1, 0});
        } else if (q.kind == QKind::Phrase) {
            for (auto &m : phrase_spans(q, d)) out.push_back({static_cast<int>(m.s), static_cast<int>(m.e) + 1, 0});
        } else if (q.kind == QKind::Bool) {          // SpanOrQuery
            for (auto &k : q.should) { auto v = lspans(k, d); out.insert(out.end(), v.begin(), v.end()); }
            std::stable_sort(out.begin(), out.end(), [](const LS &a, const LS &b) { return a.s != b.s ? a.s < b.s : a.e < b.e; });
        } else if (q.kind == QKind::Near) {
            Spans a{lspans(q.kids[0], d)}, b{lspans(q.kids[1], d)};
            const int slop = q.dist - 1;
            if (q.ordered) {                          // NearSpansOrdered.stretchToOrder
                while (a.next_start() != NO_MORE) {
                    while (b.start() < a.end()) b.next_start();
                    if (b.start() == NO_MORE) break;
                    const int width = b.start() - a.end();
                    if (width <= slop) out.push_back({a.start(), b.end(), width});
                }
            } else {                                  // NearSpansUnordered.SpanTotalLengthEndPositionWindow
                if (a.v.empty() || b.v.empty()) return out;
                a.next_start();
                b.next_start();
                Spans *cells[2] = {&a, &b};
                int total = (a.end() - a.start()) + (b.end() - b.start());
                Spans *maxc = &a;
                if (b.end() > maxc->end()) maxc = &b;
                Spans *top = ordered_pos(b, a) ? &b : &a;
                for (;;) {
                    if (maxc->end() - top->start() - total <= slop)            // atMatch()
                        out.push_back({top->start(), maxc->end(), maxc->end() - top->start()});   // width()
                    const int old = top->end() - top->start();
                    if (top->next_start() == NO_MORE) break;
                    total += (top->end() - top->start()) - old;
                    if (top->end() > maxc->end()) maxc = top;
                    Spans *other = top == cells[0] ? cells[1] : cells[0];
                    if (ordered_pos(*other, *top)) top = other;
                }
            }
            std::stable_sort(out.begin(), out.end(), [](const LS &x, const LS &y) { return x.s != y.s ? x.s < y.s : x.e < y.e; });
        }
        return out;
    }
    // the weight of a span query: the idf of its distinct terms
    void span_idf(const QNode &q, std::map<std::string, float> &m)
    {
        auto add = [&](const std::string &w) {
            const std::string key = match == MatchMode::Stem ? std::string(st.stem(w, parse_lang("en"))) : w;
            m.emplace(key, idf(terms_of(w)));
        };
        if (q.kind == QKind::Term) add(q.words[0]);
        else if (q.kind == QKind::Phrase) { for (auto &x : q.words) if (!x.empty()) add(x); }
        for (auto *v : {&q.should, &q.must, &q.kids}) for (auto &k : *v) span_idf(k, m);
    }
    // score if the document matches
    std::optional<float> eval(const QNode &q, const Doc &d)
    {
        switch (q.kind) {
        case QKind::Term: {
            auto ts = terms_of(q.words[0]);
            const auto p = positions(d, ts);
            if (p.empty()) return std::nullopt;
            return bm25(idf(ts), static_cast<float>(p.size()), d.size());
        }
        case QKind::Phrase: {
            const auto s = phrase_spans(q, d);
            if (s.empty()) return std::nullopt;
            double w = 0;
            for (auto &x : q.words) if (!x.empty()) w += idf(terms_of(x));
            return bm25(static_cast<float>(w), static_cast<float>(s.size()), d.size());
        }
        case QKind::Near: {
            float freq = 0;
            const auto m = lspans(q, d);
            for (auto &x : m) freq += 1.0f / (1.0f + static_cast<float>(x.w));
            if (m.empty()) return std::nullopt;
            std::map<std::string, float> terms;
            span_idf(q, terms);
            double w = 0;
            for (auto &t : terms) w += t.second;
            return bm25(static_cast<float>(w), freq, d.size());
        }
        case QKind::Bool: {
            double s = 0;
            for (auto &k : q.must_not) if (eval(k, d)) return std::nullopt;
            for (auto &k : q.must) { auto v = eval(k, d); if (!v) return std::nullopt; s += *v; }
            bool any = false;
            for (auto &k : q.should) { auto v = eval(k, d); if (v) { any = true; s += *v; } }
            if (q.must.empty() && !q.should.empty() && !any) return std::nullopt;
            return static_cast<float>(s);
        }
        default: return std::nullopt;
        }
    }
};

// ------------------------------------------------------------------ random corpora and queries

static Corpus corpus(std::uint64_t seed, int n, int maxlen)
{
    Rng r(seed);
    Corpus c;
    while (static_cast<int>(c.size()) < n) {
        Doc d;
        const int len = static_cast<int>(r.below(maxlen + 1));
        for (int i = 0; i < len; ++i) d.push_back(VOCAB[r.below(r.below(3) == 0 ? 8 : NV)]);
        c[r.below(n * 10) + 1] = d;
    }
    return c;
}

static std::string rand_word(Rng &r) { return VOCAB[r.below(NV)]; }

static std::string rand_pos(Rng &r, int depth)
{
    const int k = static_cast<int>(r.below(depth > 0 ? 5 : 2));
    switch (k) {
    case 0: return rand_word(r);
    case 1: return "\"" + rand_word(r) + " " + rand_word(r) + (r.below(2) ? " " + rand_word(r) : "") + "\"";
    case 2: return "(" + rand_word(r) + " OR " + rand_word(r) + ")";
    default: return "(" + rand_pos(r, depth - 1) + (r.below(2) ? " w/" : " pre/") + std::to_string(1 + r.below(4)) + " " + rand_pos(r, depth - 1) + ")";
    }
}

static std::string rand_query(Rng &r, int depth)
{
    const int k = static_cast<int>(r.below(depth > 0 ? 7 : 3));
    switch (k) {
    case 0: case 1: return rand_word(r);
    case 2: return rand_pos(r, 1);
    case 3: return "(" + rand_query(r, depth - 1) + " AND " + rand_query(r, depth - 1) + ")";
    case 4: return "(" + rand_query(r, depth - 1) + " OR " + rand_query(r, depth - 1) + ")";
    case 5: return "(" + rand_query(r, depth - 1) + " NOT " + rand_query(r, depth - 1) + ")";
    default: return rand_query(r, depth - 1) + " " + rand_query(r, depth - 1) + " -" + rand_word(r);
    }
}

// ------------------------------------------------------------------ comparison

static std::string text_of(const Doc &d)
{
    std::string s;
    for (auto &w : d) s += w + " ";
    return s;
}

static std::shared_ptr<const std::string> build_seg(const Corpus &c, std::int64_t id)
{
    SegmentBuilder b(aopts());
    for (auto &kv : c) b.add(kv.first, text_of(kv.second));
    return std::make_shared<const std::string>(b.finish(id));
}

static int compared = 0;

static void compare(Searcher &s, const Corpus &live, const std::string &q, MatchMode m)
{
    SearchOptions o;
    o.match = m;
    o.k = 1000000;
    Analyzer a(aopts());
    const QNode node = parse_query(q, a, o.query);
    Ref ref(live, m);
    std::vector<std::pair<float, std::int64_t>> want;
    for (auto &kv : live) { auto v = ref.eval(node, kv.second); if (v) want.push_back({*v, kv.first}); }
    // pure negative queries match every document with score 0
    std::sort(want.begin(), want.end(), [](auto &x, auto &y) { return x.first != y.first ? x.first > y.first : x.second < y.second; });
    const auto got = s.search(q, o);
    const std::uint64_t n = s.count(q, o);
    // top 10 with pruning (MaxScore) equals the first 10 of the full result
    {
        SearchOptions o10 = o;
        o10.k = 10;
        o10.threads = 2;
        const auto top = s.search(q, o10);
        bool same = top.size() == std::min<std::size_t>(10, got.size());
        for (std::size_t i = 0; same && i < top.size(); ++i) same = top[i].doc_id == got[i].doc_id && top[i].score == got[i].score;
        if (!same) { ++failures; std::printf("FAIL pruned top 10 differs for %s\n", q.c_str()); }
    }
    ++compared;
    bool ok = got.size() == want.size() && n == want.size();
    for (std::size_t i = 0; ok && i < got.size(); ++i) {
        ok = got[i].doc_id == want[i].second && std::fabs(got[i].score - want[i].first) <= 1e-5f * std::max(1.0f, want[i].first);
        if (!ok && std::fabs(got[i].score - want[i].first) <= 1e-5f * std::max(1.0f, want[i].first)) {
            // equal scores within rounding may order differently only if they are equal in float
            ok = false;
        }
    }
    if (!ok) {
        ++failures;
        std::printf("FAIL query %s [%s] (%s): got %zu (count %llu), want %zu\n", q.c_str(), to_string(node).c_str(),
                    m == MatchMode::Stem ? "stem" : "exact", got.size(), static_cast<unsigned long long>(n), want.size());
        for (std::size_t i = 0; i < std::min<std::size_t>(5, std::max(got.size(), want.size())); ++i)
            std::printf("   %zu: got %lld %.7f  want %lld %.7f\n", i, i < got.size() ? static_cast<long long>(got[i].doc_id) : -1LL,
                        i < got.size() ? got[i].score : 0.0f, i < want.size() ? static_cast<long long>(want[i].second) : -1LL,
                        i < want.size() ? want[i].first : 0.0f);
    }
}

static void single_segment()
{
    const Corpus c = corpus(1, 400, 25);
    auto bytes = build_seg(c, 1);
    Segment seg(bytes->data(), bytes->size());
    Rng r(2);
    for (int i = 0; i < 300; ++i) {
        const std::string q = rand_query(r, 3);
        for (MatchMode m : {MatchMode::Exact, MatchMode::Stem}) {
            Searcher s(aopts(), {SegmentRef{&seg, nullptr, nullptr}});
            compare(s, c, q, m);
        }
    }
}

// The same live documents split into three segments, with tombstones, a live tail that masks
// changed documents, and deleted documents: results must be those of the live set.
static void many_segments()
{
    const Corpus all = corpus(7, 600, 20);
    Corpus parts[3];
    int i = 0;
    for (auto &kv : all) parts[i++ % 3][kv.first] = kv.second;
    std::vector<std::shared_ptr<const std::string>> bytes;
    std::vector<std::unique_ptr<Segment>> segs;
    for (int p = 0; p < 3; ++p) { bytes.push_back(build_seg(parts[p], 10 + p)); segs.emplace_back(new Segment(bytes.back()->data(), bytes.back()->size())); }
    Corpus live = all;
    Rng r(8);
    // tombstones: delete every 9th document of each segment
    std::vector<SegmentRef> refs;
    for (int p = 0; p < 3; ++p) {
        auto dead = std::make_shared<DocSet>();
        dead->resize(segs[p]->n_docs());
        for (std::uint32_t d = 0; d < segs[p]->n_docs(); d += 9) { dead->set(d); live.erase(segs[p]->doc_id(d)); }
        auto dc = std::make_shared<DeadCounts>(compute_dead_counts(*segs[p], *dead));
        refs.push_back({segs[p].get(), dead, dc});
    }
    // live tail: 30 changed documents (new text), 10 new documents, 10 deleted through the mask
    Corpus tail;
    std::vector<std::int64_t> mask;
    int changed = 0;
    for (auto &kv : live) {
        if (changed < 30 && r.below(10) == 0) {
            Doc d;
            for (int k = 0; k < 8; ++k) d.push_back(VOCAB[r.below(NV)]);
            tail[kv.first] = d;
            mask.push_back(kv.first);
            ++changed;
        }
    }
    for (int k = 0; k < 10; ++k) tail[100000 + k] = {"contract", "indemnity", VOCAB[r.below(NV)]};
    int deleted = 0;
    for (auto &kv : live) if (!tail.count(kv.first) && deleted < 10 && r.below(15) == 0) { mask.push_back(kv.first); ++deleted; }
    for (auto id : mask) live.erase(id);
    for (auto &kv : tail) live[kv.first] = kv.second;
    auto tail_bytes = build_seg(tail, -1);

    Rng rq(9);
    for (int k = 0; k < 200; ++k) {
        const std::string q = rand_query(rq, 3);
        for (MatchMode m : {MatchMode::Exact, MatchMode::Stem}) {
            Searcher s(aopts(), refs);
            s.mask(mask);
            s.add_tail(tail_bytes);
            compare(s, live, q, m);
        }
    }
    // statistics equal those of the live set
    Searcher s(aopts(), refs);
    s.mask(mask);
    s.add_tail(tail_bytes);
    s.search("a", SearchOptions());
    std::uint64_t n = 0, sum = 0;
    for (auto &kv : live) if (!kv.second.empty()) { ++n; sum += kv.second.size(); }
    CHECK_EQ(s.stats().n_docs, n);
    CHECK_EQ(s.stats().sum_len, sum);
    CHECK_EQ(s.stats().live_docs, static_cast<std::uint64_t>(live.size()));

    // one segment of the live set gives the same scores (rule 4)
    auto one = build_seg(live, 99);
    Segment seg(one->data(), one->size());
    Rng rq2(10);
    for (int k = 0; k < 50; ++k) {
        const std::string q = rand_query(rq2, 2);
        Searcher a(aopts(), refs);
        a.mask(mask);
        a.add_tail(tail_bytes);
        Searcher b(aopts(), {SegmentRef{&seg, nullptr, nullptr}});
        SearchOptions o;
        o.k = 20;
        const auto x = a.search(q, o), y = b.search(q, o);
        bool same = x.size() == y.size();
        for (std::size_t j = 0; same && j < x.size(); ++j) same = x[j].doc_id == y[j].doc_id && x[j].score == y[j].score;
        if (!same) { ++failures; std::printf("FAIL segmentation changes the result of %s\n", q.c_str()); }
    }
}

// One segment of 200,000 documents, searched with 1 thread and with 8 (slices of the docnos):
// the same hits and counts.
static void slices()
{
    Rng r(21);
    SegmentBuilder b(aopts());
    for (int i = 0; i < 200000; ++i) {
        std::string t;
        for (int k = 0; k < 6; ++k) t += std::string(VOCAB[r.below(NV)]) + " ";
        b.add(i * 3, t);
    }
    const std::string bytes = b.finish(1);
    Segment seg(bytes.data(), bytes.size());
    Rng rq(22);
    for (int k = 0; k < 60; ++k) {
        const std::string q = rand_query(rq, 2);
        Searcher s1(aopts(), {SegmentRef{&seg, nullptr, nullptr}}), s8(aopts(), {SegmentRef{&seg, nullptr, nullptr}});
        SearchOptions o1, o8;
        o1.k = o8.k = 20;
        o8.threads = 8;
        const auto x = s1.search(q, o1), y = s8.search(q, o8);
        bool same = x.size() == y.size();
        for (std::size_t j = 0; same && j < x.size(); ++j) same = x[j].doc_id == y[j].doc_id && x[j].score == y[j].score;
        if (!same || s1.count(q, o1) != s8.count(q, o8)) { ++failures; std::printf("FAIL slices change the result of %s\n", q.c_str()); }
    }
}

// A batch (search_many / count_many, queries in parallel) gives what the queries give one by one.
static void batches()
{
    Rng r(31);
    SegmentBuilder b(aopts());
    for (int i = 0; i < 150000; ++i) {
        std::string t;
        for (int k = 0; k < 6; ++k) t += std::string(VOCAB[r.below(NV)]) + " ";
        b.add(i * 7, t);
    }
    const std::string bytes = b.finish(1);
    Segment seg(bytes.data(), bytes.size());
    Rng rq(32);
    std::vector<std::string> qs;
    for (int k = 0; k < 90; ++k) qs.push_back(rand_query(rq, 1 + k % 3));
    for (int threads : {1, 4, 16}) {
        Searcher one(aopts(), {SegmentRef{&seg, nullptr, nullptr}}), many(aopts(), {SegmentRef{&seg, nullptr, nullptr}});
        SearchOptions o;
        o.k = 15;
        o.threads = threads;
        const auto got = many.search_many(qs, o);
        const auto cnt = many.count_many(qs, o);
        CHECK(got.size() == qs.size() && cnt.size() == qs.size());
        for (std::size_t i = 0; i < qs.size(); ++i) {
            const auto x = one.search(qs[i], o);
            bool same = x.size() == got[i].size();
            for (std::size_t j = 0; same && j < x.size(); ++j) same = x[j].doc_id == got[i][j].doc_id && x[j].score == got[i][j].score;
            if (!same || one.count(qs[i], o) != cnt[i]) { ++failures; std::printf("FAIL batch changes the result of %s\n", qs[i].c_str()); }
            ++compared;
        }
    }
}

static void sloppy_phrase()
{
    // Lucene's sloppy phrase: matchLength = |(p1 - 1) - p0| for two words; freq = sum 1/(1+len)
    Corpus c;
    c[1] = {"roman", "x", "empire"};              // in order, one word between: length 1
    c[2] = {"empire", "roman"};                   // reversed adjacent: length 2
    c[3] = {"roman", "x", "x", "x", "empire"};    // length 3
    c[4] = {"roman", "empire"};                   // exact: length 0
    auto b = build_seg(c, 1);
    Segment seg(b->data(), b->size());
    Searcher s(aopts(), {SegmentRef{&seg, nullptr, nullptr}});
    SearchOptions o;
    CHECK_EQ(s.count("\"roman empire\"~0", o), 1ull);
    CHECK_EQ(s.count("\"roman empire\"~1", o), 2ull);
    CHECK_EQ(s.count("\"roman empire\"~2", o), 3ull);
    CHECK_EQ(s.count("\"roman empire\"~3", o), 4ull);
    CHECK_EQ(s.count("roman w/1 empire", o), 2ull);         // adjacent, either order
    CHECK_EQ(s.count("roman pre/1 empire", o), 1ull);
    CHECK_EQ(s.count("roman pre/2 empire", o), 2ull);
    CHECK_EQ(s.count("roman w/4 empire", o), 4ull);
    CHECK_EQ(s.count("empire pre/4 roman", o), 1ull);
    // freq of doc 1 with slop 1 is 1/2: score = w - w / (1 + 0.5 * cache)
    o.k = 10;
    const auto h = s.search("\"roman empire\"~3", o);
    CHECK(h.size() == 4 && h[0].doc_id == 4);
}

static void prefix_wildcard_fuzzy()
{
    Corpus c;
    c[1] = {"indemnity", "contract"};
    c[2] = {"indemnify", "x"};
    c[3] = {"indemnification"};
    c[4] = {"test", "text"};
    c[5] = {"holmes"};
    c[6] = {"holms", "homes"};
    c[7] = {"ohlmes"};          // transposition
    auto b = build_seg(c, 1);
    Segment seg(b->data(), b->size());
    Searcher s(aopts(), {SegmentRef{&seg, nullptr, nullptr}});
    SearchOptions o;
    CHECK_EQ(s.count("indemn*", o), 3ull);
    CHECK_EQ(s.count("indemnif*", o), 2ull);
    CHECK_EQ(s.count("te?t", o), 1ull);
    CHECK_EQ(s.count("*emnity", o), 1ull);
    // counts are documents: holms and homes are both in document 6
    CHECK_EQ(s.count("holmse~1", o), 2ull);      // holmes (transposition), holms (deletion)
    CHECK_EQ(s.count("holmes~1", o), 3ull);      // holmes; holms, homes; ohlmes (transposition)
    CHECK_EQ(s.count("holmes~2", o), 3ull);
    CHECK_EQ(s.count("holmes~0", o), 1ull);
    CHECK_EQ(s.count("indemn* w/1 contract", o), 1ull);
    o.max_expansions = 1;
    CHECK_EQ(s.count("indemn*", o), 1ull);
    o.max_expansions = 50;
    o.prefix_length = 2;
    CHECK_EQ(s.count("holmes~2", o), 2ull);      // ohlmes does not start with "ho"
    // constant score for prefix and wildcard, like Lucene's default
    o.k = 10;
    const auto h = s.search("indemn*", o);
    CHECK(h.size() == 3 && h[0].score == 1.0f && h[2].score == 1.0f && h[0].doc_id == 1);
}

// fuzzy_terms (the trie walk) against the distance of every term
static void test_fuzzy_terms()
{
    static const char *ALPHA[] = {"a", "b", "c", "d", "e", "\xc3\xa9", "\xd7\xa9", "\xd7\x94"};
    Rng r(77);
    int checked = 0;
    for (int round = 0; round < 40; ++round) {
        std::set<std::string> dict_set;
        const int n = 200 + static_cast<int>(r.below(2000));
        while (static_cast<int>(dict_set.size()) < n) {
            std::string w;
            const int len = 1 + static_cast<int>(r.below(9));
            for (int i = 0; i < len; ++i) w += ALPHA[r.below(r.below(4) == 0 ? 8 : 5)];
            dict_set.insert(w);
        }
        std::vector<std::string> dict(dict_set.begin(), dict_set.end());
        auto term = [&](std::uint32_t t) { return std::string_view(dict[t]); };
        auto lb = [&](std::string_view x) {
            return static_cast<std::uint32_t>(std::lower_bound(dict.begin(), dict.end(), x, [](const std::string &a, std::string_view b) { return std::string_view(a) < b; }) - dict.begin());
        };
        for (int qn = 0; qn < 25; ++qn) {
            std::string w;
            const int len = 1 + static_cast<int>(r.below(8));
            for (int i = 0; i < len; ++i) w += ALPHA[r.below(8)];
            const int edits = 1 + static_cast<int>(r.below(2));
            std::vector<std::uint32_t> wc, tc;
            to_code_points(w, wc);
            const std::size_t plen = r.below(3);
            std::string pre;
            {
                std::size_t b = 0, cps = 0;
                while (b < w.size() && cps < plen) { ++b; while (b < w.size() && (static_cast<unsigned char>(w[b]) & 0xC0) == 0x80) ++b; ++cps; }
                pre = w.substr(0, b);
            }
            std::map<std::string, int> want, got;
            for (auto &t : dict) {
                if (t.compare(0, pre.size(), pre) != 0) continue;
                to_code_points(t, tc);
                const int d = bounded_osa_distance(wc, tc, edits);
                if (d <= edits) want[t] = d;
            }
            fuzzy_terms(static_cast<std::uint32_t>(dict.size()), term, lb, wc, edits, pre,
                        [&](std::uint32_t, std::string_view t, int d) { got[std::string(t)] = d; });
            CHECK(want == got);
            ++checked;
        }
    }
    std::printf("%d fuzzy expansions equal to the brute force\n", checked);
}

int main()
{
    single_segment();
    many_segments();
    slices();
    batches();
    sloppy_phrase();
    prefix_wildcard_fuzzy();
    test_fuzzy_terms();
    std::printf("%d query comparisons\n", compared);
    return finish("test_search");
}
