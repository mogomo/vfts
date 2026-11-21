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

        vtext::SearchOptions o;
        // mode= is the parameter to use; MATCH is a reserved word in Vertica, so "match"= (quoted)
        // is accepted as well.
        if (s.has("mode") && s.has("match")) fail("give mode or \"match\", not both");
        const std::string match = s.has("mode") ? s.text("mode", "stem") : s.text("match", "stem");
        if (match == "stem") o.match = vtext::MatchMode::Stem;
        else if (match == "exact") o.match = vtext::MatchMode::Exact;
        else fail("mode must be 'stem' or 'exact', not '" + match + "'");
        if (s.has("lang")) o.query.lang = vtext::parse_lang(s.text("lang", ""));
        const std::string op = s.text("operator", "or");
        if (op == "and" || op == "AND") o.query.op_and = true;
        else if (op != "or" && op != "OR") fail("operator must be 'or' or 'and', not '" + op + "'");
        o.query.stopwords = s.boolean("stopwords", false);
        const std::string syntax = s.text("syntax", "query");
        if (syntax == "plain") o.query.plain = true;
        else if (syntax != "query") fail("syntax must be 'query' or 'plain', not '" + syntax + "'");
        const std::int64_t k = s.integer("k", 10);
        if (k < 1 || k > 1000000) fail("k must be 1 to 1000000");
        o.k = static_cast<std::uint32_t>(k);
        const std::int64_t pl = s.integer("prefix_length", 0);
        if (pl < 0 || pl > 100) fail("prefix_length must be 0 to 100");
        o.prefix_length = static_cast<int>(pl);
        const std::int64_t me = s.integer("max_expansions", 50);
        if (me < 1 || me > 100000) fail("max_expansions must be 1 to 100000");
        o.max_expansions = static_cast<std::uint32_t>(me);
        std::int64_t th = s.integer("threads", 0);
        if (th < 0 || th > 256) fail("threads must be 0 (automatic) to 256");
        if (th == 0) th = std::max<std::int64_t>(1, std::min<std::int64_t>(static_cast<std::int64_t>(li->refs.size()) + (tail.empty() ? 0 : 1),
                                                                         std::thread::hardware_concurrency()));
        o.threads = static_cast<int>(th);

        for (auto &q : queries) {
            if (isCanceled()) return;
            if (counting_) {
                const std::uint64_t n = searcher.count(q.second, o);
                out.setInt(0, q.first);
                out.setInt(1, static_cast<vint>(n));
                out.next();
            } else {
                const auto hits = searcher.search(q.second, o);
                vint rank = 0;
                for (const vtext::Hit &h : hits) {
                    out.setInt(0, q.first);
                    out.setInt(1, h.doc_id);
                    out.setFloat(2, h.score);
                    out.setInt(3, ++rank);
                    out.next();
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
        p.addInt("k");
        p.addVarchar(16, "mode");
        p.addVarchar(16, "match");
        p.addVarchar(16, "lang");
        p.addVarchar(8, "operator");
        p.addBool("stopwords");
        p.addVarchar(16, "syntax");
        p.addInt("prefix_length");
        p.addInt("max_expansions");
        p.addInt("threads");
        p.addVarchar(1024, "cache_dir");
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
