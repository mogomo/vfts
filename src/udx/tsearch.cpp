// vtext.tsearch(* USING PARAMETERS index, query, k, ...) OVER() FROM <schema>.<index>_live
// vtext.tcount (* USING PARAMETERS index, query, ...)    OVER() FROM <schema>.<index>_live
//
// Input: the columns of the views made by vtext.create_index (qid, query, doc_id, body, lang, op,
// gen). The role of a row is given by its columns:
//   qid and query set          a query (more than one: a batch; the parameter query adds qid 0)
//   doc_id set, op 'T'         a row of the live tail: newer than the segments; masks the doc_id in
//                              the segments and is analyzed and searched now
//   doc_id set, op 'D'         a doc_id changed or deleted since the segments (journal): masked
//   only gen set               the sentinel: the generation the view belongs to (stale check)
// Output: tsearch (qid, doc_id, score, rank), best first; tcount (qid, count).
#include "udx_common.h"
#include "search_options.h"
#include "../engine/builder.h"
#include "../engine/search.h"

#include <set>
#include <thread>

using namespace Vertica;
using namespace vtext_udx;

namespace {

enum InCol { C_QID = 0, C_QUERY, C_DOC_ID, C_BODY, C_LANG, C_OP, C_GEN };

struct TailDoc {
    std::int64_t doc_id;
    std::string text;
    vtext::Lang lang;
};

class TSearch : public TransformFunction {
    bool counting_;

public:
    explicit TSearch(bool counting) : counting_(counting) {}

    void processPartition(ServerInterface &srv, PartitionReader &in, PartitionWriter &out) override
    {
        try {
            run(srv, in, out);
        } catch (const std::exception &e) {
            vt_report_error(0, "%s: %s", counting_ ? "tcount" : "tsearch", e.what());
        }
    }

private:
    void run(ServerInterface &srv, PartitionReader &in, PartitionWriter &out)
    {
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
        std::vector<std::int64_t> mask;
        std::vector<TailDoc> tail;
        std::set<std::int64_t> in_tail;
        std::int64_t gen = 0;
        do {
            if (!in.isNull(C_GEN)) gen = std::max<std::int64_t>(gen, in.getIntRef(C_GEN));
            if (!in.isNull(C_QID) && !in.isNull(C_QUERY)) {
                queries.emplace_back(in.getIntRef(C_QID), in.getStringRef(C_QUERY).str());
                continue;
            }
            if (in.isNull(C_DOC_ID)) continue;
            const std::int64_t id = in.getIntRef(C_DOC_ID);
            mask.push_back(id);
            const bool del = !in.isNull(C_OP) && in.getStringRef(C_OP).str() == "D";
            if (del || !in_tail.insert(id).second) continue;
            TailDoc d;
            d.doc_id = id;
            if (!in.isNull(C_BODY)) { const VString &b = in.getStringRef(C_BODY); d.text.assign(b.data(), b.length()); }
            d.lang = vtext::LANG_NONE;
            if (!in.isNull(C_LANG)) {
                if (!vtext::find_lang(in.getStringRef(C_LANG).str(), d.lang)) d.lang = vtext::LANG_NONE;
            }
            tail.push_back(std::move(d));
        } while (in.next());
        if (queries.empty()) return;

        auto li = vtext::open_index(resolve_cache_dir(s), index, gen);
        const vtext::AnalyzerOptions aopts = parse_index_options(li->options);
        vtext::Searcher searcher(aopts, li->refs);
        if (!mask.empty()) searcher.mask(mask);
        if (!tail.empty()) {
            vtext::SegmentBuilder b(aopts);
            for (TailDoc &d : tail) {
                try { b.add(d.doc_id, d.text, d.lang); }
                catch (const std::exception &e) { fail("live tail, doc_id " + std::to_string(d.doc_id) + ": " + e.what()); }
            }
            searcher.add_tail(std::make_shared<const std::string>(b.finish(-1)));
        }

        vtext::SearchOptions o = parse_search_options(s);

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
                const std::vector<std::vector<vtext::Hit>> hits = searcher.search_many(texts, o);
                for (std::size_t i = at; i < end; ++i) {
                    vint rank = 0;
                    for (const vtext::Hit &h : hits[i - at]) {
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
    bool counting_;

public:
    explicit TSearchFactory(bool counting) : counting_(counting) {}
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
        return counting_ ? vt_createFuncObject<TSearch>(srv.allocator, true) : vt_createFuncObject<TSearch>(srv.allocator, false);
    }
};

class TSearchFactoryS : public TSearchFactory { public: TSearchFactoryS() : TSearchFactory(false) {} };
class TCountFactory : public TSearchFactory { public: TCountFactory() : TSearchFactory(true) {} };

} // namespace

RegisterFactory(TSearchFactoryS);
RegisterFactory(TCountFactory);
