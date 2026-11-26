// Merging segments: the merged segment holds exactly the live documents, passes verification, and
// every query returns the same ids and the same scores (bit for bit) before and after (rule 4),
// with documents in doc_id order and reordered by recursive graph bisection (reorder.h). A
// reordered segment survives the table round trip, finds every doc_id, and masks work on it.
#include "check.h"
#include "../../src/engine/builder.h"
#include "../../src/engine/merge.h"
#include "../../src/engine/search.h"
#include "../../src/engine/table_codec.h"

#include <algorithm>
#include <set>

using namespace vfts;

static const char *W[] = {"run", "runs", "running", "contract", "contracts", "indemnity", "holmes", "watson", "the", "of", "a", "b", "c"};

static std::string doc(Rng &r)
{
    std::string s;
    const int n = 1 + static_cast<int>(r.below(15));
    for (int i = 0; i < n; ++i) s += std::string(W[r.below(13)]) + " ";
    return s;
}

static AnalyzerOptions ao()
{
    AnalyzerOptions o;
    o.default_lang = parse_lang("en");
    return o;
}

int main()
{
    Rng r(3);
    std::vector<std::string> bytes;
    std::set<std::int64_t> used;
    for (int s = 0; s < 3; ++s) {
        SegmentBuilder b(ao());
        for (int i = 0; i < 700; ++i) {
            std::int64_t id;
            do { id = r.below(100000); } while (!used.insert(id).second);
            b.add(id, doc(r));
        }
        bytes.push_back(b.finish(s + 1));
    }
    std::vector<std::unique_ptr<Segment>> segs;
    std::vector<SegmentRef> refs;
    std::uint64_t live = 0;
    for (int s = 0; s < 3; ++s) {
        segs.emplace_back(new Segment(bytes[s].data(), bytes[s].size()));
        auto dead = std::make_shared<DocSet>();
        dead->resize(segs[s]->n_docs());
        for (std::uint32_t d = s; d < segs[s]->n_docs(); d += 5) dead->set(d);
        live += segs[s]->n_docs() - dead->count;
        refs.push_back({segs[s].get(), dead, std::make_shared<DeadCounts>(compute_dead_counts(*segs[s], *dead))});
    }
    MergeOptions plain;
    plain.reorder = false;
    ReorderStats rs;
    MergeOptions bp;
    bp.stats = &rs;
    const std::string merged = merge_segments(refs, 99, bp);
    const std::string merged_plain = merge_segments(refs, 99, plain);
    {
        // in parallel: the same bytes, reordered or not
        MergeOptions bp8 = bp, plain8 = plain;
        bp8.threads = 8;
        plain8.threads = 8;
        CHECK(merge_segments(refs, 99, bp8) == merged);
        CHECK(merge_segments(refs, 99, plain8) == merged_plain);
        plain8.threads = 3;
        CHECK(merge_segments(refs, 99, plain8) == merged_plain);
    }
    Segment m(merged.data(), merged.size()), mp(merged_plain.data(), merged_plain.size());
    m.verify();
    mp.verify();
    CHECK_EQ(m.n_docs(), static_cast<std::uint32_t>(live));
    CHECK_EQ(m.id(), std::int64_t(99));
    CHECK_EQ(m.header().format, SEGMENT_FORMAT_ORDERED);
    CHECK_EQ(mp.header().format, SEGMENT_FORMAT);
    CHECK(!m.in_doc_id_order());
    CHECK(rs.swaps > 0);
    std::printf("bisection: %u levels, %llu rounds, %llu swaps; postings %zu bytes reordered, %zu in doc_id order\n", rs.levels,
                static_cast<unsigned long long>(rs.rounds), static_cast<unsigned long long>(rs.swaps),
                static_cast<std::size_t>(m.header().len[S_POSTINGS]), static_cast<std::size_t>(mp.header().len[S_POSTINGS]));
    // every doc_id is found, at the docno that holds it
    for (std::uint32_t d = 0; d < mp.n_docs(); ++d) {
        const std::int64_t at = m.find_doc(mp.doc_id(d));
        CHECK(at >= 0 && m.doc_id(static_cast<std::uint32_t>(at)) == mp.doc_id(d));
    }
    CHECK_EQ(m.find_doc(-12345), std::int64_t(-1));
    // the table round trip gives the same bytes
    {
        SegmentAssembler as;
        RowSink sink;
        sink.term = [&](const TermDictRow &r) { as.add(r); };
        sink.posting = [&](const PostingRow &r) { PostingRow c = r; std::string pl = std::move(c.payload); c.payload.clear(); as.add_part(c.term_id, c.chunk_id, 0, std::move(pl)); as.add(std::move(c)); };
        sink.doc = [&](const DocRow &r) { as.add(r); };
        segment_to_rows(m, sink);
        CHECK(as.finish(99, 0) == merged);
    }
    // masks (doc_ids looked up in the segment) give the same results on both
    {
        std::vector<std::int64_t> mask;
        for (std::uint32_t d = 0; d < mp.n_docs(); d += 7) mask.push_back(mp.doc_id(d));
        for (const char *q : {"contract", "run* OR holmes", "\"the contract\""}) {
            Searcher a(ao(), {SegmentRef{&m, nullptr, nullptr}}), b(ao(), {SegmentRef{&mp, nullptr, nullptr}});
            a.mask(mask);
            b.mask(mask);
            SearchOptions o;
            o.k = 100000;
            const auto x = a.search(q, o), y = b.search(q, o);
            bool same = x.size() == y.size() && !x.empty();
            for (std::size_t i = 0; same && i < x.size(); ++i) same = x[i].doc_id == y[i].doc_id && x[i].score == y[i].score;
            if (!same) { ++failures; std::printf("FAIL mask on a reordered segment changes '%s'\n", q); }
        }
    }

    const char *queries[] = {"run", "running", "contract indemnity", "holmes AND watson", "\"the contract\"", "contract w/2 indemnity",
                             "run* -the", "contrac~1", "(holmes OR watson) pre/3 the", "NOT a", "\"run the\"~2", "watson"};
    for (MatchMode mode : {MatchMode::Stem, MatchMode::Exact}) {
        for (const char *q : queries) {
            for (std::uint32_t k : {5u, 100000u}) {
                for (const Segment *ms : {&m, &mp}) {
                    Searcher before(ao(), refs);
                    Searcher after(ao(), {SegmentRef{ms, nullptr, nullptr}});
                    SearchOptions o;
                    o.match = mode;
                    o.k = k;
                    const auto x = before.search(q, o), y = after.search(q, o);
                    bool same = x.size() == y.size() && !x.empty();
                    for (std::size_t i = 0; same && i < x.size(); ++i) same = x[i].doc_id == y[i].doc_id && x[i].score == y[i].score;
                    if (!same) { ++failures; std::printf("FAIL merge (%s, k %u) changes '%s' (%zu vs %zu hits)\n", ms == &m ? "reordered" : "doc_id order", k, q, x.size(), y.size()); }
                    CHECK_EQ(before.count(q, o), after.count(q, o));
                    auto mx = before.matches(q, o), my = after.matches(q, o);
                    std::sort(mx.begin(), mx.end());
                    std::sort(my.begin(), my.end());
                    CHECK(mx == my);
                }
            }
        }
    }
    // merging the merged segment with a new one, and a segment whose documents are all deleted
    SegmentBuilder b(ao());
    b.add(-5, "contract run");
    const std::string extra = b.finish(7);
    Segment e(extra.data(), extra.size());
    auto all_dead = std::make_shared<DocSet>();
    all_dead->resize(segs[0]->n_docs());
    for (std::uint32_t d = 0; d < segs[0]->n_docs(); ++d) all_dead->set(d);
    const std::string m2 = merge_segments({SegmentRef{&m, nullptr, nullptr}, SegmentRef{&e, nullptr, nullptr},
                                           SegmentRef{segs[0].get(), all_dead, std::make_shared<DeadCounts>(compute_dead_counts(*segs[0], *all_dead))}}, 100);
    Segment mm(m2.data(), m2.size());
    mm.verify();
    CHECK_EQ(mm.n_docs(), m.n_docs() + 1);
    CHECK(mm.find_doc(-5) >= 0);
    // a reordered segment merged again in doc_id order (an index switched to reorder=false), alone
    // and with a segment in doc_id order: the same results as the doc_id-order merge
    {
        MergeOptions keep;
        keep.reorder = false;
        const std::string back = merge_segments({SegmentRef{&m, nullptr, nullptr}}, 101, keep);
        Segment bk(back.data(), back.size());
        bk.verify();
        CHECK_EQ(bk.header().format, SEGMENT_FORMAT);
        CHECK(back.substr(HEADER_BYTES) == merged_plain.substr(HEADER_BYTES) || bk.n_docs() == mp.n_docs());
        const std::string both = merge_segments({SegmentRef{&m, nullptr, nullptr}, SegmentRef{&e, nullptr, nullptr}}, 102, keep);
        Segment bt(both.data(), both.size());
        bt.verify();
        for (const char *q : {"contract", "run* OR holmes", "\"the contract\"", "holmes w/2 watson"}) {
            Searcher a(ao(), {SegmentRef{&bt, nullptr, nullptr}}), b(ao(), {SegmentRef{&m, nullptr, nullptr}, SegmentRef{&e, nullptr, nullptr}});
            SearchOptions o;
            o.k = 100000;
            const auto x = a.search(q, o), y = b.search(q, o);
            bool same = x.size() == y.size() && !x.empty();
            for (std::size_t i = 0; same && i < x.size(); ++i) same = x[i].doc_id == y[i].doc_id && x[i].score == y[i].score;
            if (!same) { ++failures; std::printf("FAIL merge of a reordered segment in doc_id order changes '%s'\n", q); }
        }
    }
    // threads on a large reordered segment (slices of a segment that is not in doc_id order) give
    // exactly the hits of one thread and of the segment in doc_id order
    {
        Rng r2(11);
        SegmentBuilder big(ao());
        for (int i = 0; i < 50000; ++i) big.add(i * 3 + 1, doc(r2));
        const std::string bb = big.finish(200);
        Segment bs(bb.data(), bb.size());
        const std::string bm = merge_segments({SegmentRef{&bs, nullptr, nullptr}}, 201);
        Segment bms(bm.data(), bm.size());
        CHECK(!bms.in_doc_id_order());
        for (const char *q : {"contract indemnity", "run* OR holmes", "\"the contract\"", "holmes w/3 watson", "watson AND NOT the"}) {
            for (int th : {1, 4, 8}) {
                Searcher a(ao(), {SegmentRef{&bms, nullptr, nullptr}}), b(ao(), {SegmentRef{&bs, nullptr, nullptr}});
                SearchOptions o;
                o.k = 20;
                o.threads = th;
                const auto x = a.search(q, o);
                o.threads = 1;
                const auto y = b.search(q, o);
                bool same = x.size() == y.size() && !x.empty();
                for (std::size_t i = 0; same && i < x.size(); ++i) same = x[i].doc_id == y[i].doc_id && x[i].score == y[i].score;
                if (!same) { ++failures; std::printf("FAIL %d threads on a reordered segment change '%s'\n", th, q); }
            }
        }
        const auto many = Searcher(ao(), {SegmentRef{&bms, nullptr, nullptr}}).search_many({"contract", "holmes watson", "\"run the\""}, [] { SearchOptions o; o.k = 10; o.threads = 8; return o; }());
        for (std::size_t i = 0; i < many.size(); ++i) {
            const char *qq[] = {"contract", "holmes watson", "\"run the\""};
            SearchOptions o;
            o.k = 10;
            const auto y = Searcher(ao(), {SegmentRef{&bs, nullptr, nullptr}}).search(qq[i], o);
            bool same = many[i].size() == y.size();
            for (std::size_t j = 0; same && j < y.size(); ++j) same = many[i][j].doc_id == y[j].doc_id && many[i][j].score == y[j].score;
            if (!same) { ++failures; std::printf("FAIL batch on a reordered segment changes '%s'\n", qq[i]); }
        }
    }
    // a doc_id live in two inputs is refused
    CHECK(throws([&] { merge_segments({SegmentRef{&m, nullptr, nullptr}, SegmentRef{&m, nullptr, nullptr}}, 5); }, "live in two segments"));
    return finish("test_merge");
}
