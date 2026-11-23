// Attributes (attribute.h): the file, dates, the filter language with SQL's three-valued logic,
// and searching with a filter and counting by an attribute against a reference computed from the
// unfiltered search: the same documents, in the same order, with the same scores.
#include "check.h"
#include "../../src/engine/attribute.h"
#include "../../src/engine/builder.h"
#include "../../src/engine/cache.h"

#include <cstdlib>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <unistd.h>

using namespace vfts;

static AnalyzerOptions aopts()
{
    AnalyzerOptions o;
    o.default_lang = parse_lang("en");
    return o;
}

static const char *VOCAB[] = {"contract", "contracts", "indemnity", "fraud", "audit", "committee", "the", "of", "a", "b", "c"};

int main()
{
    // ---- dates: Vertica's DateADT counts days from 2000-01-01
    std::int64_t d = 99;
    CHECK(parse_date("2000-01-01", d) && d == 0);
    CHECK(parse_date("2001-06-01", d) && d == 517);
    CHECK(parse_date("1999-12-31", d) && d == -1);
    CHECK(parse_date("2024-02-29", d) && format_date(d) == "2024-02-29");
    CHECK(!parse_date("2023-02-29", d));
    CHECK(!parse_date("2001-13-01", d));
    CHECK(!parse_date("2001-6-1", d));
    CHECK_EQ(format_date(517), std::string("2001-06-01"));
    CHECK_EQ(format_date(-1), std::string("1999-12-31"));

    // ---- files
    {
        const std::string f = encode_attribute(AttrType::Int, 7, {{30, 5}, {10, -2}, {20, 5}, {40, 9}});
        Attribute a(f.data(), f.size());
        CHECK(a.type() == AttrType::Int);
        CHECK_EQ(a.version(), static_cast<std::int64_t>(7));
        CHECK_EQ(a.rows(), static_cast<std::uint64_t>(4));
        CHECK_EQ(a.n_codes(), 3u);
        CHECK_EQ(a.value_string(a.code_of(10)), std::string("-2"));
        CHECK_EQ(a.value_string(a.code_of(20)), std::string("5"));
        CHECK_EQ(a.code_of(20), a.code_of(30));
        CHECK(a.code_of(25) == ATTR_NONE);
        CHECK(a.code_of(-5) == ATTR_NONE);
        CHECK(a.code_of(1000000) == ATTR_NONE);
        CHECK(throws([] { encode_attribute(AttrType::Int, 1, {{1, 1}, {1, 2}}); }));
        std::string bad = f;
        bad[bad.size() - 3] ^= 1;
        CHECK(throws([&] { Attribute x(bad.data(), bad.size()); }));
        // sparse doc ids: binary search instead of a table
        const std::string g = encode_attribute(AttrType::Bool, 1, {{5, 1}, {5000000000LL, 0}, {-7, 1}});
        Attribute b(g.data(), g.size());
        CHECK_EQ(b.value_string(b.code_of(5000000000LL)), std::string("f"));
        CHECK_EQ(b.value_string(b.code_of(-7)), std::string("t"));
        CHECK(b.code_of(6) == ATTR_NONE);
        const std::string t = encode_attribute_text(1, {{1, "open"}, {2, "closed"}, {3, ""}, {4, "open"}});
        Attribute c(t.data(), t.size());
        CHECK_EQ(c.n_codes(), 3u);
        CHECK_EQ(c.value_string(c.code_of(1)), std::string("open"));
        CHECK_EQ(c.value_string(c.code_of(3)), std::string(""));
        CHECK(c.code_of(2) < c.code_of(1));    // codes in value order
    }

    // ---- a corpus: docs 0..1999 in two segments and a live tail of 50; custodian = doc_id % 7
    // for most documents, none for every 13th; status text; sent date
    std::mt19937 rng(42);
    std::vector<std::pair<std::int64_t, std::string>> docs;
    for (int i = 0; i < 2050; ++i) {
        std::string s;
        const int n = 1 + static_cast<int>(rng() % 12);
        for (int w = 0; w < n; ++w) s += std::string(w ? " " : "") + VOCAB[rng() % (sizeof(VOCAB) / sizeof(VOCAB[0]))];
        docs.emplace_back(i, s);
    }
    SegmentBuilder b1(aopts()), b2(aopts()), bt(aopts());
    for (auto &x : docs) (x.first < 1000 ? b1 : x.first < 2000 ? b2 : bt).add(x.first, x.second);
    const std::string s1 = b1.finish(101), s2 = b2.finish(102), st = bt.finish(-1);
    Segment g1(s1.data(), s1.size()), g2(s2.data(), s2.size());
    std::vector<std::pair<std::int64_t, std::int64_t>> cust, sent;
    std::vector<std::pair<std::int64_t, std::string>> status;
    std::map<std::int64_t, std::int64_t> cust_of;
    for (auto &x : docs) {
        if (x.first % 13 == 0) continue;
        cust.emplace_back(x.first, x.first % 7);
        cust_of[x.first] = x.first % 7;
        status.emplace_back(x.first, x.first % 3 == 0 ? "closed" : "open");
        sent.emplace_back(x.first, 500 + x.first % 40);   // 2001-05-15 .. 2001-06-23
    }

    char tmpl[] = "/tmp/vfts_test_attr_XXXXXX";
    const std::string root = ::mkdtemp(tmpl);
    {
        CacheWriter w(root, "idx");
        w.write_attribute("custodian", encode_attribute(AttrType::Int, 1, cust));
        w.write_attribute("status", encode_attribute_text(1, status));
        w.write_attribute("sent", encode_attribute(AttrType::Date, 1, sent));
        CHECK(throws([&] { w.write_attribute("bad name", encode_attribute(AttrType::Int, 1, cust)); }));
        CHECK(throws([&] { w.write_attribute("x", "garbage"); }));
    }
    CHECK_EQ(attribute_names(root, "idx"), (std::vector<std::string>{"custodian", "sent", "status"}));
    Filter::Opener open = [&](const std::string &n) { return open_attribute(root, "idx", n); };

    Searcher s(aopts(), {SegmentRef{&g1, nullptr, nullptr}, SegmentRef{&g2, nullptr, nullptr}});
    s.add_tail(std::make_shared<const std::string>(st));
    const std::vector<const Segment *> segs = s.all_segments();
    CHECK_EQ(segs.size(), static_cast<std::size_t>(3));

    auto keep_of = [&](const std::string &filter) {
        // reference: evaluate the filter by hand for every doc_id
        std::set<std::int64_t> keep;
        for (auto &x : docs) {
            const std::int64_t id = x.first;
            const bool has = cust_of.count(id) > 0;
            const std::int64_t c = has ? cust_of[id] : -1;
            const bool closed = id % 3 == 0;
            const std::int64_t day = 500 + id % 40;
            int v = -1;     // 1 true, 0 false, 2 unknown
            if (filter == "custodian IN (1, 2)") v = has ? (c == 1 || c == 2) : 2;
            else if (filter == "custodian = 3") v = has ? c == 3 : 2;
            else if (filter == "custodian <> 3") v = has ? c != 3 : 2;
            else if (filter == "custodian BETWEEN 2 AND 4") v = has ? (c >= 2 && c <= 4) : 2;
            else if (filter == "NOT custodian IN (1)") v = has ? c != 1 : 2;
            else if (filter == "custodian NOT IN (1, 5)") v = has ? (c != 1 && c != 5) : 2;
            else if (filter == "custodian IS NULL") v = !has;
            else if (filter == "custodian IS NOT NULL OR custodian = 1") v = has;
            else if (filter == "custodian < 2 OR status = 'closed'") v = has ? (c < 2 || closed) : 2;
            else if (filter == "NOT (custodian >= 5 AND status = 'open')") v = has ? !(c >= 5 && !closed) : 2;
            else if (filter == "sent >= '2001-06-01' AND custodian > 0") v = has ? (day >= 517 && c > 0) : 2;
            else if (filter == "sent < DATE '2001-05-20'") v = has ? day < 505 : 2;
            else if (filter == "status IN ('closed', 'nothing')") v = has ? closed : 2;
            else if (filter == "status > 'ok'") v = has ? !closed : 2;      // "open" > "ok" > "closed"
            if (v == -1) throw std::runtime_error("no reference for " + filter);
            if (v == 1) keep.insert(id);
        }
        return keep;
    };

    SearchOptions all;
    all.k = 1000000;
    const std::vector<std::string> queries = {"contract", "contract OR indemnity", "fraud AND audit", "audit w/3 committee", "NOT the", "\"the contract\""};
    const std::vector<std::string> filters = {"custodian IN (1, 2)", "custodian = 3", "custodian <> 3", "custodian BETWEEN 2 AND 4",
                                              "NOT custodian IN (1)", "custodian NOT IN (1, 5)", "custodian IS NULL",
                                              "custodian IS NOT NULL OR custodian = 1", "custodian < 2 OR status = 'closed'",
                                              "NOT (custodian >= 5 AND status = 'open')", "sent >= '2001-06-01' AND custodian > 0",
                                              "sent < DATE '2001-05-20'", "status IN ('closed', 'nothing')", "status > 'ok'"};
    for (const std::string &fs : filters) {
        auto f = Filter::parse(fs, open);
        const std::set<std::int64_t> keep = keep_of(fs);
        for (int mode : {0, 1, 2}) {        // sets from the cache, sets made anew, asked per candidate
            const auto ex = mode < 2 ? filter_segments(*f, segs, mode == 0) : std::vector<std::shared_ptr<const DocSet>>();
            const std::unique_ptr<DocFilter> fv = mode == 2 ? f->view(segs) : nullptr;
            auto use = [&](SearchOptions &x) { if (mode < 2) x.exclude = &ex; else x.doc_filter = fv.get(); };
            for (const std::string &q : queries) {
                const std::vector<Hit> full = s.search(q, all);
                for (std::uint32_t k : {1u, 10u, 100u}) {
                    std::vector<Hit> want;
                    for (const Hit &h : full) if (keep.count(h.doc_id) && want.size() < k) want.push_back(h);
                    SearchOptions o;
                    o.k = k;
                    use(o);
                    for (int th : {1, 4}) {
                        o.threads = th;
                        const std::vector<Hit> got = s.search(q, o);
                        bool same = got.size() == want.size();
                        for (std::size_t i = 0; same && i < got.size(); ++i) same = got[i].doc_id == want[i].doc_id && got[i].score == want[i].score;
                        if (!same) std::printf("filter '%s' query '%s' k=%u threads=%d mode=%d: %zu hits, want %zu\n", fs.c_str(), q.c_str(), k, th, mode, got.size(), want.size());
                        CHECK(same);
                        // the batch path
                        const auto many = s.search_many({q, q}, o);
                        CHECK(many[1].size() == got.size() && (got.empty() || many[1][0].doc_id == got[0].doc_id));
                    }
                }
                // count, matches and count by custodian
                std::uint64_t n = 0;
                std::map<std::int64_t, std::uint64_t> by;
                for (std::int64_t id : s.matches(q, all))
                    if (keep.count(id)) { ++n; if (cust_of.count(id)) ++by[cust_of[id]]; }
                SearchOptions o;
                use(o);
                o.threads = 3;
                CHECK_EQ(s.count(q, o), n);
                CHECK_EQ(s.matches(q, o).size(), static_cast<std::size_t>(n));
                auto ca = open("custodian");
                std::vector<std::shared_ptr<const std::vector<std::uint32_t>>> cv;
                std::vector<const std::uint32_t *> cp;
                for (const Segment *g : segs) { cv.push_back(ca->codes_for(*g)); cp.push_back(cv.back()->data()); }
                const std::vector<std::uint64_t> got = s.count_by(q, o, cp, ca->n_codes());
                for (std::uint32_t c = 0; c < ca->n_codes(); ++c) {
                    const std::int64_t v = ca->int_value(c);
                    CHECK_EQ(got[c], by.count(v) ? by[v] : 0);
                }
            }
        }
    }
    // a filter never changes a score
    {
        auto f = Filter::parse("custodian = 2", open);
        const auto ex = filter_segments(*f, segs);
        SearchOptions o;
        o.k = 5;
        o.exclude = &ex;
        const std::vector<Hit> got = s.search("contract", o);
        std::map<std::int64_t, float> full;
        for (const Hit &h : s.search("contract", all)) full[h.doc_id] = h.score;
        CHECK(!got.empty());
        for (const Hit &h : got) CHECK(full[h.doc_id] == h.score);
    }

    // ---- errors
    auto bad = [&](const std::string &fs, const std::string &part) {
        try {
            Filter::parse(fs, open);
            std::printf("no error for '%s'\n", fs.c_str());
            CHECK(false);
        } catch (const std::exception &e) {
            if (std::string(e.what()).find(part) == std::string::npos) std::printf("message for '%s': %s\n", fs.c_str(), e.what());
            CHECK(std::string(e.what()).find(part) != std::string::npos);
        }
    };
    bad("", "empty filter");
    bad("owner = 1", "no attribute owner");
    bad("custodian = 'x'", "is an integer");
    bad("status = 3", "is text");
    bad("sent = '2001-02-30'", "is a date");
    bad("custodian IN (1, 2", "',' or ')' expected");
    bad("custodian = 1 AND", "attribute name expected");
    bad("custodian = 1 extra", "unexpected");
    bad("custodian 1", "comparison, IN, BETWEEN or IS expected");
    bad("custodian = 1; DROP TABLE x", "unexpected character");
    bad("status = 'open", "unterminated string");

    // ---- a refresh replaces the file: the next open (after ACTIVE_CHECK_MS) reads the new one
    {
        auto a1 = open("custodian");
        CacheWriter w(root, "idx");
        w.write_attribute("custodian", encode_attribute(AttrType::Int, 2, {{1, 100}}));
        std::this_thread::sleep_for(std::chrono::milliseconds(ACTIVE_CHECK_MS + 50));
        auto a2 = open("custodian");
        CHECK_EQ(a2->version(), static_cast<std::int64_t>(2));
        CHECK_EQ(a1->version(), static_cast<std::int64_t>(1));     // a search that holds the old one keeps it
        CHECK(a2->serial() != a1->serial());
        w.remove_attribute("status");
        std::this_thread::sleep_for(std::chrono::milliseconds(ACTIVE_CHECK_MS + 50));
        CHECK(throws([&] { open("status"); }));
    }
    drop_cache(root, "idx");
    ::rmdir(root.c_str());
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("test_attribute: OK\n");
    return 0;
}
