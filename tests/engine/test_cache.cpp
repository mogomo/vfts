// Per-node cache: segment files, generations with tombstones, ACTIVE, stale checks, cleanup,
// and searching a loaded generation.
#include "check.h"
#include "../../src/engine/builder.h"
#include "../../src/engine/cache.h"

#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

using namespace vtext;

static std::string make_seg(std::int64_t id, int first, int n)
{
    AnalyzerOptions o;
    o.default_lang = parse_lang("en");
    SegmentBuilder b(o);
    for (int i = 0; i < n; ++i) b.add(first + i, (i % 2 ? "contract indemnity " : "contract ") + std::to_string(i));
    return b.finish(id);
}

static bool exists(const std::string &p) { struct stat st; return ::stat(p.c_str(), &st) == 0; }

int main()
{
    char tmpl[] = "/tmp/vtext_test_cache_XXXXXX";
    const std::string root = ::mkdtemp(tmpl);
    const std::string dir = root + "/idx";

    CHECK(valid_index_name("abc_12"));
    CHECK(!valid_index_name("../x"));
    CHECK(!valid_index_name(""));
    CHECK(throws([&] { CacheWriter w(root, "a/b"); }, "not valid"));
    CHECK(throws([&] { open_index(root, "idx", 0); }, "no cache for index idx"));

    CacheWriter w(root, "idx");
    const std::string s1 = make_seg(1, 0, 100), s2 = make_seg(2, 1000, 50);
    Segment h1(s1.data(), s1.size()), h2(s2.data(), s2.size());
    CHECK(!w.has_segment(1, h1.header().checksum));
    w.write_segment(s1);
    CHECK(w.has_segment(1, h1.header().checksum));
    CHECK(!w.has_segment(1, h1.header().checksum + 1));
    std::string bad = s2;
    bad[bad.size() / 2] ^= 1;
    CHECK(throws([&] { w.write_segment(bad); }, "damaged"));

    // generation 1: segment 1
    Generation g1;
    g1.gen = 1;
    g1.segs.push_back({1, h1.header().checksum, {}});
    w.activate(g1);
    auto li = open_index(root, "idx", 1);
    CHECK_EQ(li->gen, std::int64_t(1));
    CHECK_EQ(li->refs.size(), std::size_t(1));
    {
        Searcher s(AnalyzerOptions(), li->refs);
        CHECK_EQ(s.count("contract", SearchOptions()), 100ull);
        CHECK_EQ(s.count("indemnity", SearchOptions()), 50ull);
    }
    CHECK(throws([&] { open_index(root, "idx", 2); }, "stale"));

    // generation 2: segment 1 with tombstones, segment 2 new
    w.write_segment(s2);
    Generation g2;
    g2.gen = 2;
    g2.segs.push_back({1, h1.header().checksum, {1, 3, 5, 0}});   // docnos 1, 3, 5 have indemnity; 0 not
    g2.segs.push_back({2, h2.header().checksum, {}});
    w.activate(g2);
    li = open_index(root, "idx", 2);
    CHECK_EQ(li->gen, std::int64_t(2));
    CHECK_EQ(li->dead, std::uint64_t(4));
    {
        Searcher s(AnalyzerOptions(), li->refs);
        CHECK_EQ(s.count("contract", SearchOptions()), 146ull);
        CHECK_EQ(s.count("indemnity", SearchOptions()), 72ull);       // 50 - 3 + 25
        CHECK_EQ(li->refs[0].dead_counts->term(static_cast<std::uint32_t>(h1.find_term("indemnity"))), 3u);
        CHECK_EQ(s.stats().n_docs, 146ull);
    }
    CHECK(exists(dir + "/gen_1.vtg") && exists(dir + "/gen_2.vtg"));

    // generation 3 drops segment 1: generation 1 and its files that gen 2 does not use go away
    Generation g3;
    g3.gen = 3;
    g3.segs.push_back({2, h2.header().checksum, {}});
    w.activate(g3);
    CHECK(!exists(dir + "/gen_1.vtg"));
    CHECK(exists(dir + "/gen_2.vtg") && exists(dir + "/seg_1.vts"));   // kept for queries on gen 2
    Generation g4 = g3;
    g4.gen = 4;
    w.activate(g4);
    CHECK(!exists(dir + "/seg_1.vts") && !exists(dir + "/gen_2.vtg"));
    // the process cache trusts ACTIVE for a moment; a newer min_gen forces a read
    li = open_index(root, "idx", 4);
    CHECK_EQ(li->gen, std::int64_t(4));
    // a segment that is missing on this node
    Generation g5;
    g5.gen = 5;
    g5.segs.push_back({9, 123, {}});
    CHECK(throws([&] { w.activate(g5); }, "not in the cache"));
    CacheInfo ci = cache_info(root, "idx");
    CHECK(ci.present && ci.gen == 4 && ci.segments == 1 && ci.docs == 50);
    drop_cache(root, "idx");
    CHECK(!exists(dir));
    CHECK(!cache_info(root, "idx").present);
    ::rmdir(root.c_str());
    return finish("test_cache");
}
