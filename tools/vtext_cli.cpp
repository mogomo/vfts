// vtext_cli: the engine without Vertica, for measurements and debugging.
//
//   vtext_cli build  <docs.dat> <cache_dir> <index> [--threads N] [--lang en] [--max-segment-mb M]
//   vtext_cli merge  <cache_dir> <index> <max_segments> [--no-reorder] [--force]   --force: rewrite even
//                    when there are max_segments or fewer (one segment: reorder it)
//   vtext_cli explain <queries.tsv> [--lang en] [--plain]   the parsed queries as JSON lines (for the
//                    Elasticsearch / OpenSearch translation in bench/run_engines.py)
//   vtext_cli search <cache_dir> <index> <queries.tsv> [--k 10] [--threads N] [--mode stem|exact]
//                    [--repeat R] [--run out.tsv] [--no-pruning] [--count] [--plain] [--and]
//                    [--max-expansions 50] [--prefix-length 0] [--classic] [--stats]
//                    [--clients C]   C concurrent searchers, prints one JSON line with q/s
//                    [--batch B]     each searcher sends B queries at a time (search_many)
//                    [--times out.tsv]  per query: qid, best ms, and (with --stats) plan us, execute us
//
// docs.dat: records "doc_id \x01 lang \x01 body \x02" (bench/data/prepare.py). queries.tsv: qid \t query.
// search prints the latency of every query over R runs (best run per query) as percentiles, and
// with --run writes "qid doc_id rank score" lines (TREC-like) for relevance evaluation.
#include "../src/engine/builder.h"
#include "../src/engine/cache.h"
#include "../src/engine/merge.h"
#include "../src/engine/query.h"
#include "../src/engine/search.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace vtext;
using Clock = std::chrono::steady_clock;

static double secs(Clock::time_point a) { return std::chrono::duration<double>(Clock::now() - a).count(); }

static const char *opt(int argc, char **argv, const char *name, const char *dflt)
{
    for (int i = 1; i + 1 < argc; ++i) if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    return dflt;
}
static bool flag(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], name) == 0) return true;
    return false;
}

static std::string options_string(const AnalyzerOptions &o)
{
    return std::string("lang=") + lang_info(o.default_lang).code + ";accent_fold=latin;on_unsupported=" + (o.unsupported_plain ? "plain" : "error");
}

static int build(int argc, char **argv)
{
    const std::string path = argv[2], cache = argv[3], index = argv[4];
    const int threads = std::atoi(opt(argc, argv, "--threads", "8"));
    const std::size_t max_bytes = static_cast<std::size_t>(std::atol(opt(argc, argv, "--max-segment-mb", "4096"))) << 20;
    AnalyzerOptions ao;
    ao.default_lang = parse_lang(opt(argc, argv, "--lang", "en"));
    ao.unsupported_plain = true;
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) { std::perror(path.c_str()); return 1; }
    struct stat st;
    ::fstat(fd, &st);
    const char *data = static_cast<const char *>(::mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
    const std::size_t size = static_cast<std::size_t>(st.st_size);
    // record boundaries
    std::vector<std::size_t> starts;
    starts.push_back(0);
    for (const char *p = data; (p = static_cast<const char *>(std::memchr(p, '\x02', data + size - p))) != nullptr; ++p)
        starts.push_back(static_cast<std::size_t>(p - data) + 1);
    const std::size_t n = starts.size() - 1;
    std::printf("%zu documents, %.0f MB\n", n, size / 1e6);
    CacheWriter w(cache, index);
    const auto t0 = Clock::now();
    std::atomic<std::size_t> next{0};
    std::mutex mu;
    std::vector<GenSegment> gsegs;
    std::atomic<std::int64_t> seg_id{1};
    auto emit = [&](SegmentBuilder &b) {
        const std::string bytes = b.finish(seg_id++);
        Segment seg(bytes.data(), bytes.size());
        w.write_segment(bytes);
        std::lock_guard<std::mutex> l(mu);
        gsegs.push_back({seg.id(), seg.header().checksum, {}});
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            SegmentBuilder b(ao);
            for (;;) {
                const std::size_t from = next.fetch_add(1000);
                if (from >= n) break;
                for (std::size_t i = from; i < std::min(n, from + 1000); ++i) {
                    const char *r = data + starts[i];
                    const char *e = data + starts[i + 1] - 1;
                    const char *a = static_cast<const char *>(std::memchr(r, '\x01', e - r));
                    const char *l = static_cast<const char *>(std::memchr(a + 1, '\x01', e - a - 1));
                    const std::int64_t id = std::strtoll(std::string(r, a).c_str(), nullptr, 10);
                    Lang lang = LANG_NONE;
                    find_lang(std::string(a + 1, l), lang);
                    b.add(id, std::string_view(l + 1, e - l - 1), lang);
                    if (b.memory() > max_bytes) emit(b);
                }
            }
            if (b.docs()) emit(b);
        });
    for (auto &t : pool) t.join();
    std::sort(gsegs.begin(), gsegs.end(), [](const GenSegment &a, const GenSegment &b) { return a.segment_id < b.segment_id; });
    Generation g;
    g.gen = 1;
    g.options = options_string(ao);
    g.segs = gsegs;
    w.activate(g);
    std::printf("built %zu segments in %.1f s\n", gsegs.size(), secs(t0));
    return 0;
}

static int merge(int argc, char **argv)
{
    const std::string cache = argv[2], index = argv[3];
    const std::size_t want = static_cast<std::size_t>(std::atoi(argv[4]));
    auto li = open_index(cache, index, 0);
    std::vector<SegmentRef> refs = li->refs;
    std::sort(refs.begin(), refs.end(), [](const SegmentRef &a, const SegmentRef &b) { return a.seg->bytes() < b.seg->bytes(); });
    const bool force = flag(argc, argv, "--force");
    if (refs.size() <= want && !force) { std::printf("%zu segments, nothing to merge\n", refs.size()); return 0; }
    const auto t0 = Clock::now();
    const std::size_t k = refs.size() <= want ? refs.size() : refs.size() - want + 1;
    std::vector<SegmentRef> in(refs.begin(), refs.begin() + k);
    std::int64_t new_id = 0;
    for (auto &r : li->refs) new_id = std::max(new_id, r.seg->id());
    MergeOptions mo;
    mo.reorder = !flag(argc, argv, "--no-reorder");
    mo.bisection.min_df = static_cast<std::uint32_t>(std::atoi(opt(argc, argv, "--min-df", "0")));
    mo.bisection.max_iters = static_cast<std::uint32_t>(std::atoi(opt(argc, argv, "--iters", "20")));
    mo.bisection.min_partition = static_cast<std::uint32_t>(std::atoi(opt(argc, argv, "--min-partition", "32")));
    ReorderStats rs;
    mo.stats = &rs;
    const std::string bytes = merge_segments(in, new_id + 1, mo);
    CacheWriter w(cache, index);
    w.write_segment(bytes);
    Segment s(bytes.data(), bytes.size());
    Generation g;
    g.gen = li->gen + 1;
    g.options = li->options;
    for (auto it = refs.begin() + k; it != refs.end(); ++it) g.segs.push_back({it->seg->id(), it->seg->header().checksum, {}});
    g.segs.push_back({s.id(), s.header().checksum, {}});
    w.activate(g);
    std::printf("merged %zu segments into one (%.0f MB, postings %.0f MB, positions %.0f MB) in %.1f s; %zu segments now\n", k, bytes.size() / 1e6,
                s.header().len[S_POSTINGS] / 1e6, s.header().len[S_POSITIONS] / 1e6, secs(t0), g.segs.size());
    if (mo.reorder) std::printf("reordering: %.1f s, %u levels, %llu rounds, %llu swaps\n", rs.seconds, rs.levels,
                                static_cast<unsigned long long>(rs.rounds), static_cast<unsigned long long>(rs.swaps));
    return 0;
}

static AnalyzerOptions parse_opts(const std::string &s)
{
    AnalyzerOptions o;
    const std::size_t a = s.find("lang=");
    if (a != std::string::npos) o.default_lang = parse_lang(s.substr(a + 5, s.find(';', a) - a - 5));
    o.unsupported_plain = s.find("on_unsupported=plain") != std::string::npos;
    return o;
}

static int search(int argc, char **argv)
{
    const std::string cache = argv[2], index = argv[3], qpath = argv[4];
    SearchOptions o;
    o.k = static_cast<std::uint32_t>(std::atoi(opt(argc, argv, "--k", "10")));
    o.threads = std::atoi(opt(argc, argv, "--threads", "1"));
    o.match = std::string(opt(argc, argv, "--mode", "stem")) == "exact" ? MatchMode::Exact : MatchMode::Stem;
    o.pruning = !flag(argc, argv, "--no-pruning");
    o.query.plain = flag(argc, argv, "--plain");
    o.classic = flag(argc, argv, "--classic");
    o.window = static_cast<std::uint32_t>(std::atoi(opt(argc, argv, "--window", "0")));
    o.max_expansions = static_cast<std::uint32_t>(std::atoi(opt(argc, argv, "--max-expansions", "50")));
    o.prefix_length = std::atoi(opt(argc, argv, "--prefix-length", "0"));
    o.query.op_and = flag(argc, argv, "--and");
    const bool counting = flag(argc, argv, "--count");
    const int repeat = std::atoi(opt(argc, argv, "--repeat", "3"));
    SearchOptions::Stats st;
    if (flag(argc, argv, "--stats")) o.stats = &st;
    SearchOptions::Stats last;
    const double slow = std::atof(opt(argc, argv, "--slow", "0"));
    const char *run = opt(argc, argv, "--run", nullptr);
    auto li = open_index(cache, index, 0);
    const AnalyzerOptions ao = parse_opts(li->options);
    std::vector<std::pair<std::string, std::string>> qs;
    std::ifstream in(qpath);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        qs.emplace_back(line.substr(0, tab), line.substr(tab + 1));
    }
    std::vector<double> best(qs.size(), 1e9);
    const int clients = std::atoi(opt(argc, argv, "--clients", "1"));
    if (clients > 1 || std::atoi(opt(argc, argv, "--batch", "1")) > 1) {
        // C concurrent searchers, one query at a time each (no SQL, no network): the ceiling of
        // the engine for C clients. One warm-up run, then R measured runs; best q/s is printed.
        o.stats = nullptr;
        std::vector<std::atomic<double>> bq(qs.size());
        for (auto &b : bq) b.store(1e9);
        double best_qps = 0;
        std::atomic<std::size_t> errs{0};
        for (int r = 0; r <= repeat; ++r) {
            std::atomic<std::size_t> next{0};
            const auto t0 = Clock::now();
            std::vector<std::thread> th;
            const std::size_t batch = static_cast<std::size_t>(std::max(1, std::atoi(opt(argc, argv, "--batch", "1"))));
            for (int c = 0; c < clients && batch > 1; ++c)
                th.emplace_back([&] {
                    // B queries per call of search_many (the tsearch batch path)
                    for (std::size_t i; (i = next.fetch_add(batch)) < qs.size();) {
                        const std::size_t e = std::min(qs.size(), i + batch);
                        std::vector<std::string> texts;
                        for (std::size_t j = i; j < e; ++j) texts.push_back(qs[j].second);
                        const auto q0 = Clock::now();
                        try {
                            Searcher s(ao, li->refs);
                            if (counting) (void)s.count_many(texts, o);
                            else (void)s.search_many(texts, o);
                        } catch (const std::exception &) { ++errs; }
                        const double ms = secs(q0) * 1000.0 / static_cast<double>(e - i);
                        for (std::size_t j = i; j < e; ++j) if (r > 0 && ms < bq[j].load()) bq[j].store(ms);
                    }
                });
            for (int c = 0; c < clients && batch == 1; ++c)
                th.emplace_back([&] {
                    for (std::size_t i; (i = next++) < qs.size();) {
                        const auto q0 = Clock::now();
                        try {
                            Searcher s(ao, li->refs);
                            if (counting) (void)s.count(qs[i].second, o);
                            else (void)s.search(qs[i].second, o);
                        } catch (const std::exception &) { ++errs; }
                        const double ms = secs(q0) * 1000.0;
                        if (r > 0 && ms < bq[i].load()) bq[i].store(ms);
                    }
                });
            for (auto &t : th) t.join();
            if (r > 0) best_qps = std::max(best_qps, qs.size() / secs(t0));
        }
        std::vector<double> v;
        for (auto &b : bq) v.push_back(b.load());
        std::sort(v.begin(), v.end());
        double sum = 0;
        for (double x : v) sum += x;
        auto pct = [&](double p) { return v.empty() ? 0.0 : v[std::min(v.size() - 1, static_cast<std::size_t>(p * v.size()))]; };
        std::printf("{\"engine\": \"vtext_cli\", \"index\": \"%s\", \"queries\": %zu, \"clients\": %d, \"threads\": %d, \"errors\": %zu, "
                    "\"ms\": {\"mean\": %.3f, \"p50\": %.3f, \"p90\": %.3f, \"p99\": %.3f, \"max\": %.3f}, \"qps_best\": %.1f}\n",
                    index.c_str(), qs.size(), clients, o.threads, errs.load() / static_cast<std::size_t>(repeat + 1),
                    v.empty() ? 0 : sum / v.size(), pct(0.5), pct(0.9), pct(0.99), v.empty() ? 0 : v.back(), best_qps);
        return 0;
    }
    std::FILE *out = run ? std::fopen(run, "w") : nullptr;
    const char *times = opt(argc, argv, "--times", nullptr);
    std::vector<double> bplan(qs.size(), 0), bexec(qs.size(), 0);     // of the best run of each query
    std::size_t errors = 0;
    for (int r = 0; r < repeat; ++r) {
        for (std::size_t i = 0; i < qs.size(); ++i) {
            const auto t0 = Clock::now();
            try {
                Searcher s(ao, li->refs);
                if (counting) {
                    const std::uint64_t c = s.count(qs[i].second, o);
                    if (out && r == 0) std::fprintf(out, "%s\t%llu\n", qs[i].first.c_str(), static_cast<unsigned long long>(c));
                } else {
                    const auto hits = s.search(qs[i].second, o);
                    if (out && r == 0)
                        for (std::size_t j = 0; j < hits.size(); ++j)
                            std::fprintf(out, "%s\t%lld\t%zu\t%.7f\n", qs[i].first.c_str(), static_cast<long long>(hits[j].doc_id), j + 1, hits[j].score);
                }
            } catch (const std::exception &e) {
                if (r == 0 && errors++ < 5) std::fprintf(stderr, "query %s: %s\n", qs[i].first.c_str(), e.what());
            }
            const double ms = secs(t0) * 1000.0;
            if (ms < best[i]) {
                best[i] = ms;
                if (o.stats) { bplan[i] = st.plan_us - last.plan_us; bexec[i] = st.exec_us - last.exec_us; }
            }
            if (slow > 0 && r == 0 && secs(t0) * 1000.0 > slow && o.stats)
                std::printf("slow %.1f ms: %s | cand %llu scored %llu skip %llu probes %llu\n", secs(t0) * 1000.0, qs[i].second.c_str(),
                            (unsigned long long)(st.candidates - last.candidates), (unsigned long long)(st.scored - last.scored),
                            (unsigned long long)(st.windows_skipped - last.windows_skipped), (unsigned long long)(st.probes - last.probes));
            if (o.stats) last = st;
        }
    }
    if (out) std::fclose(out);
    if (times) {
        std::FILE *tf = std::fopen(times, "w");
        for (std::size_t i = 0; tf && i < qs.size(); ++i)
            std::fprintf(tf, "%s\t%.4f\t%.1f\t%.1f\n", qs[i].first.c_str(), best[i], bplan[i], bexec[i]);
        if (tf) std::fclose(tf);
    }
    std::vector<double> v = best;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    auto pct = [&](double p) { return v.empty() ? 0.0 : v[std::min(v.size() - 1, static_cast<std::size_t>(p * v.size()))]; };
    if (o.stats) {
        const double q = static_cast<double>(qs.size() * repeat);
        std::printf("per query: plan %.1f us, exec %.1f us, %.0f candidates, %.0f scored, %.0f windows skipped, %.0f probes, %.1f lists\n",
                    st.plan_us / q, st.exec_us / q, st.candidates / q, st.scored / q, st.windows_skipped / q, st.probes / q, st.terms / q);
    }
    std::printf("%zu queries, %zu segments, threads %d, k %u%s: mean %.3f ms, p50 %.3f, p90 %.3f, p99 %.3f, max %.3f\n",
                qs.size(), li->refs.size(), o.threads, o.k, o.pruning ? "" : " (no pruning)", v.empty() ? 0 : sum / v.size(),
                pct(0.5), pct(0.9), pct(0.99), v.empty() ? 0 : v.back());
    return 0;
}

static std::string jstr(const std::string &s)
{
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
    return o + "\"";
}

static std::string to_json(const QNode &n)
{
    auto list = [](const std::vector<QNode> &v) {
        std::string s = "[";
        for (std::size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + to_json(v[i]);
        return s + "]";
    };
    switch (n.kind) {
    case QKind::None: return "{\"k\":\"none\"}";
    case QKind::Term: return "{\"k\":\"term\",\"w\":" + jstr(n.words[0]) + "}";
    case QKind::Fuzzy: return "{\"k\":\"fuzzy\",\"w\":" + jstr(n.words[0]) + ",\"edits\":" + std::to_string(n.edits) + "}";
    case QKind::Prefix: return "{\"k\":\"prefix\",\"p\":" + jstr(n.pattern) + "}";
    case QKind::Wildcard: return "{\"k\":\"wildcard\",\"p\":" + jstr(n.pattern) + "}";
    case QKind::Phrase: {
        std::string s = "{\"k\":\"phrase\",\"slop\":" + std::to_string(n.slop) + ",\"w\":[";
        for (std::size_t i = 0; i < n.words.size(); ++i) s += (i ? "," : "") + jstr(n.words[i]);
        return s + "]}";
    }
    case QKind::Near:
        return "{\"k\":\"near\",\"dist\":" + std::to_string(n.dist) + ",\"ordered\":" + (n.ordered ? "true" : "false") +
               ",\"a\":" + to_json(n.kids[0]) + ",\"b\":" + to_json(n.kids[1]) + "}";
    case QKind::Bool:
        return "{\"k\":\"bool\",\"must\":" + list(n.must) + ",\"should\":" + list(n.should) + ",\"must_not\":" + list(n.must_not) + "}";
    }
    return "{}";
}

// tokens <file.tsv> [--lang en]: per line "id<TAB>text", prints "id<TAB>count<TAB>terms" (debugging)
static int tokens(int argc, char **argv)
{
    AnalyzerOptions ao;
    ao.default_lang = parse_lang(opt(argc, argv, "--lang", "en"));
    ao.unsupported_plain = true;
    Analyzer a(ao);
    std::ifstream in(argv[2]);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        a.analyze(line.substr(tab + 1));
        std::printf("%s\t%zu\t", line.substr(0, tab).c_str(), a.size());
        for (std::size_t i = 0; i < a.size(); ++i) std::printf("%s%.*s", i ? " " : "", static_cast<int>(a.term(i).size()), a.term(i).data());
        std::printf("\n");
    }
    return 0;
}

static int explain(int argc, char **argv)
{
    AnalyzerOptions ao;
    ao.default_lang = parse_lang(opt(argc, argv, "--lang", "en"));
    ao.unsupported_plain = true;
    Analyzer a(ao);
    QueryOptions qo;
    qo.plain = flag(argc, argv, "--plain");
    std::ifstream in(argv[2]);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string tree;
        try { tree = to_json(parse_query(line.substr(tab + 1), a, qo)); }
        catch (const std::exception &e) { tree = "{\"k\":\"error\",\"error\":" + jstr(e.what()) + "}"; }
        std::printf("{\"qid\":%s,\"tree\":%s}\n", jstr(line.substr(0, tab)).c_str(), tree.c_str());
    }
    return 0;
}

int main(int argc, char **argv)
{
    try {
        if (argc >= 5 && std::strcmp(argv[1], "build") == 0) return build(argc, argv);
        if (argc >= 5 && std::strcmp(argv[1], "merge") == 0) return merge(argc, argv);
        if (argc >= 5 && std::strcmp(argv[1], "search") == 0) return search(argc, argv);
        if (argc >= 3 && std::strcmp(argv[1], "explain") == 0) return explain(argc, argv);
        if (argc >= 3 && std::strcmp(argv[1], "tokens") == 0) return tokens(argc, argv);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "vtext_cli: %s\n", e.what());
        return 1;
    }
    std::fprintf(stderr, "usage: see the comment at the top of tools/vtext_cli.cpp\n");
    return 2;
}
