// Merging segments: the merged segment holds exactly the live documents, passes verification, and
// every query returns the same ids and the same scores (bit for bit) before and after (rule 4).
#include "check.h"
#include "../../src/engine/builder.h"
#include "../../src/engine/merge.h"
#include "../../src/engine/search.h"

#include <set>

using namespace vtext;

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
    const std::string merged = merge_segments(refs, 99);
    Segment m(merged.data(), merged.size());
    m.verify();
    CHECK_EQ(m.n_docs(), static_cast<std::uint32_t>(live));
    CHECK_EQ(m.id(), std::int64_t(99));

    const char *queries[] = {"run", "running", "contract indemnity", "holmes AND watson", "\"the contract\"", "contract w/2 indemnity",
                             "run* -the", "contrac~1", "(holmes OR watson) pre/3 the", "NOT a", "\"run the\"~2", "watson"};
    for (MatchMode mode : {MatchMode::Stem, MatchMode::Exact}) {
        for (const char *q : queries) {
            Searcher before(ao(), refs);
            Searcher after(ao(), {SegmentRef{&m, nullptr, nullptr}});
            SearchOptions o;
            o.match = mode;
            o.k = 100000;
            const auto x = before.search(q, o), y = after.search(q, o);
            bool same = x.size() == y.size() && !x.empty();
            for (std::size_t i = 0; same && i < x.size(); ++i) same = x[i].doc_id == y[i].doc_id && x[i].score == y[i].score;
            if (!same) { ++failures; std::printf("FAIL merge changes '%s' (%zu vs %zu hits)\n", q, x.size(), y.size()); }
            CHECK_EQ(before.count(q, o), after.count(q, o));
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
    CHECK_EQ(mm.doc_id(0), std::int64_t(-5));
    // a doc_id live in two inputs is refused
    CHECK(throws([&] { merge_segments({SegmentRef{&m, nullptr, nullptr}, SegmentRef{&m, nullptr, nullptr}}, 5); }, "live in two segments"));
    return finish("test_merge");
}
