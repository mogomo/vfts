// The search parameters shared by tsearch, tcount and tserve.
#ifndef VFTS_UDX_SEARCH_OPTIONS_H
#define VFTS_UDX_SEARCH_OPTIONS_H

#include "udx_common.h"
#include "../engine/search.h"

#include <thread>

namespace vfts_udx {

inline vfts::SearchOptions parse_search_options(Settings &s, std::int64_t default_threads = 0)
{
    vfts::SearchOptions o;
    // mode= is the parameter to use; MATCH is a reserved word in Vertica, so "match"= (quoted)
    // is accepted as well.
    if (s.has("mode") && s.has("match")) fail("give mode or \"match\", not both");
    const std::string match = s.has("mode") ? s.text("mode", "stem") : s.text("match", "stem");
    if (match == "stem") o.match = vfts::MatchMode::Stem;
    else if (match == "exact") o.match = vfts::MatchMode::Exact;
    else fail("mode must be 'stem' or 'exact', not '" + match + "'");
    if (s.has("lang")) o.query.lang = vfts::parse_lang(s.text("lang", ""));
    const std::string op = s.text("operator", "or");
    if (op == "and" || op == "AND") o.query.op_and = true;
    else if (op != "or" && op != "OR") fail("operator must be 'or' or 'and', not '" + op + "'");
    o.query.stopwords = s.boolean("stopwords", false);
    const std::string bm25 = s.text("bm25", "lucene");
    if (bm25 == "classic") o.classic = true;
    else if (bm25 != "lucene") fail("bm25 must be 'lucene' (the scores of OpenSearch) or 'classic' (times k1 + 1: the scores of Elasticsearch)");
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
    std::int64_t th = s.integer("threads", default_threads);
    if (th < 0 || th > 256) fail("threads must be 0 (automatic) to 256");
    // 0: one thread per core; large segments are cut into slices of documents (search.h)
    if (th == 0) th = std::max<std::int64_t>(1, std::thread::hardware_concurrency());
    o.threads = static_cast<int>(th);
    return o;
}

inline void add_search_parameters(Vertica::SizedColumnTypes &p)
{
    p.addInt("k");
    p.addVarchar(16, "mode");
    p.addVarchar(16, "match");
    p.addVarchar(16, "lang");
    p.addVarchar(8, "operator");
    p.addBool("stopwords");
    p.addVarchar(16, "syntax");
    p.addVarchar(16, "bm25");
    p.addInt("prefix_length");
    p.addInt("max_expansions");
    p.addInt("threads");
    p.addVarchar(1024, "cache_dir");
}

} // namespace vfts_udx

#endif
