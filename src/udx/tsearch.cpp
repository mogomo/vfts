// vfts.tsearch(* USING PARAMETERS index, query, k, ...) OVER() FROM <schema>.<index>_live
// vfts.tcount (* USING PARAMETERS index, query, ...)    OVER() FROM <schema>.<index>_live
// vfts.tmatch (* USING PARAMETERS index, query, ...)    OVER() FROM <schema>.<index>_live
//
// Input: the columns of the views made by vfts.create_index (qid, query, doc_id, body, lang, op,
// gen), read by view_input.h, which also checks the view's access token. Query rows (qid, query)
// may be added to the view's rows; the parameter query adds qid 0.
// Output: tsearch (qid, doc_id, score, rank), best first; tcount (qid, count); tmatch (qid, doc_id),
// every match, no scores, in no order (for joins and GROUP BY over all matches).
#include "udx_common.h"
#include "search_options.h"
#include "view_input.h"

#include <chrono>
#include <thread>

using namespace Vertica;
using namespace vfts_udx;

namespace {

class TSearch : public TransformFunction {
    bool counting_, matching_;

public:
    TSearch(bool counting, bool matching) : counting_(counting), matching_(matching) {}

    void processPartition(ServerInterface &srv, PartitionReader &in, PartitionWriter &out) override
    {
        try {
            run(srv, in, out);
        } catch (const std::exception &e) {
            vt_report_error(0, "%s: %s", matching_ ? "tmatch" : counting_ ? "tcount" : "tsearch", e.what());
        }
    }

private:
    void run(ServerInterface &srv, PartitionReader &in, PartitionWriter &out)
    {
        using clk = std::chrono::steady_clock;
        const auto te = clk::now();
        Settings s(srv);
        const std::string index = read_index_name(s);
        std::vector<std::pair<vint, std::string>> queries;
        if (s.has("query")) queries.emplace_back(0, s.text("query", ""));
        if (s.has("queries")) {
            // many queries in one parameter: one per line, "qid<TAB>query" (or only the query: qid = line number)
            const std::string all = s.text("queries", "");
            std::size_t at = 0;
            vint line = 0;
            while (at < all.size()) {
                std::size_t e = all.find('\n', at);
                if (e == std::string::npos) e = all.size();
                const std::string l = all.substr(at, e - at);
                ++line;
                if (!l.empty()) {
                    const std::size_t tab = l.find('\t');
                    if (tab == std::string::npos) queries.emplace_back(line, l);
                    else queries.emplace_back(std::stoll(l.substr(0, tab)), l.substr(tab + 1));
                }
                at = e + 1;
            }
        }
        const auto t0 = clk::now();
        ViewInput v;
        v.read(in, true);
        for (auto &q : v.queries) queries.push_back(std::move(q));
        if (queries.empty()) return;
        const auto t1 = clk::now();
        auto li = v.open(resolve_cache_dir(s), index, srv.getUserName());
        const auto t2 = clk::now();
        std::unique_ptr<vfts::Searcher> sp = v.searcher(*li);
        vfts::Searcher &searcher = *sp;
        searcher.ready();
        const auto t3 = clk::now();
        struct Timing {
            ServerInterface &srv; bool on; clk::time_point te, t0, t1, t2, t3; std::size_t hidden;
            ~Timing()
            {
                if (!on) return;
                auto ms = [](clk::time_point a, clk::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
                srv.log("vfts timing: parameters %.3f ms, read %.3f ms, open+token %.3f ms, searcher %.3f ms, search %.3f ms (%zu hidden)",
                        ms(te, t0), ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, clk::now()), hidden);
            }
        } timing{srv, s.text("debug_timing", "") == "1", te, t0, t1, t2, t3, v.hidden.size()};

        vfts::SearchOptions o = parse_search_options(s);

        if (matching_) {
            // every match, no scores: (qid, doc_id)
            for (auto &q : queries) {
                if (isCanceled()) return;
                for (std::int64_t id : searcher.matches(q.second, o)) {
                    out.setInt(0, q.first);
                    out.setInt(1, id);
                    out.next();
                }
            }
            return;
        }
        // A batch runs its queries in parallel (one thread each, results kept in query order), in
        // chunks so that a cancel is seen between them.
        const std::size_t chunk = std::max<std::size_t>(64, static_cast<std::size_t>(o.threads) * 4);
        for (std::size_t at = 0; at < queries.size(); at += chunk) {
            if (isCanceled()) return;
            const std::size_t end = std::min(queries.size(), at + chunk);
            std::vector<std::string> texts;
            texts.reserve(end - at);
            for (std::size_t i = at; i < end; ++i) texts.push_back(queries[i].second);
            if (counting_) {
                const std::vector<std::uint64_t> n = searcher.count_many(texts, o);
                for (std::size_t i = at; i < end; ++i) {
                    out.setInt(0, queries[i].first);
                    out.setInt(1, static_cast<vint>(n[i - at]));
                    out.next();
                }
            } else {
                const std::vector<std::vector<vfts::Hit>> hits = searcher.search_many(texts, o);
                for (std::size_t i = at; i < end; ++i) {
                    vint rank = 0;
                    for (const vfts::Hit &h : hits[i - at]) {
                        out.setInt(0, queries[i].first);
                        out.setInt(1, h.doc_id);
                        out.setFloat(2, h.score);
                        out.setInt(3, ++rank);
                        out.next();
                    }
                }
            }
        }
    }
};

class TSearchFactory : public TransformFunctionFactory {
    bool counting_, matching_;

public:
    TSearchFactory(bool counting, bool matching = false) : counting_(counting || matching), matching_(matching) {}
    void getPrototype(ServerInterface &, ColumnTypes &a, ColumnTypes &r) override
    {
        a.addInt();             // qid
        a.addVarchar();         // query
        a.addInt();             // doc_id
        a.addLongVarchar();     // body
        a.addVarchar();         // lang
        a.addVarchar();         // op
        a.addInt();             // gen
        r.addInt();
        r.addInt();
        if (!counting_) { r.addFloat(); r.addInt(); }
    }
    void getReturnType(ServerInterface &, const SizedColumnTypes &, SizedColumnTypes &out) override
    {
        out.addInt("qid");
        if (matching_) { out.addInt("doc_id"); return; }
        if (counting_) { out.addInt("count"); return; }
        out.addInt("doc_id");
        out.addFloat("score");
        out.addInt("rank");
    }
    void getParameterType(ServerInterface &, SizedColumnTypes &p) override
    {
        p.addVarchar(128, "index");
        p.addVarchar(65000, "query");
        p.addLongVarchar(32000000, "queries");
        add_search_parameters(p);
    }
    TransformFunction *createTransformFunction(ServerInterface &srv) override
    {
        return vt_createFuncObject<TSearch>(srv.allocator, counting_ && !matching_, matching_);
    }
};

class TSearchFactoryS : public TSearchFactory { public: TSearchFactoryS() : TSearchFactory(false) {} };
class TCountFactory : public TSearchFactory { public: TCountFactory() : TSearchFactory(true) {} };
class TMatchFactory : public TSearchFactory { public: TMatchFactory() : TSearchFactory(false, true) {} };

} // namespace

RegisterFactory(TSearchFactoryS);
RegisterFactory(TCountFactory);
RegisterFactory(TMatchFactory);
