// The search parameters shared by tsearch, tcount and tserve.
#ifndef VFTS_UDX_SEARCH_OPTIONS_H
#define VFTS_UDX_SEARCH_OPTIONS_H

#include "udx_common.h"
#include "../engine/attribute.h"
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
    p.addVarchar(65000, "filter");
    p.addBool("filter_cache");
    p.addInt("tail_warn");
}

// A filter over the index's attributes (parameter filter, attribute.h) and the attribute to count
// by (tcount's parameter by), opened from this node's cache and fitted to a searcher's segments.
struct AttrSearch {
    std::shared_ptr<const vfts::Filter> filter;
    std::vector<std::shared_ptr<const vfts::DocSet>> exclude;
    std::unique_ptr<vfts::DocFilter> view;
    std::shared_ptr<const vfts::Attribute> by;
    std::vector<std::shared_ptr<const std::vector<std::uint32_t>>> by_keep;
    std::vector<const std::uint32_t *> by_codes;

    // filter_text / by_name may be empty. Sets o.exclude (or o.doc_filter) when there is a filter.
    void setup(const std::string &cache_dir, const std::string &index, const std::string &filter_text, const std::string &by_name,
               bool use_cache, const vfts::Searcher &searcher, vfts::SearchOptions &o)
    {
        open(cache_dir, index, filter_text, by_name);
        apply(use_cache, searcher, o);
    }

    // The two steps of setup: open parses the filter and opens the attributes (cheap: what a cache
    // key needs); apply computes the filter's sets and the codes of the segments.
    void open(const std::string &cache_dir, const std::string &index, const std::string &filter_text, const std::string &by_name)
    {
        if (!filter_text.empty())
            filter = vfts::Filter::parse(filter_text, [&](const std::string &n) { return vfts::open_attribute(cache_dir, index, n); });
        if (!by_name.empty()) by = vfts::open_attribute(cache_dir, index, by_name);
    }

    // The attribute files the answer depends on (empty without filter and by).
    std::string key() const
    {
        std::string k = filter ? filter->key() : std::string();
        if (by) k += "\x1e" + std::to_string(by->version()) + "#" + std::to_string(by->serial());
        return k;
    }

    void apply(bool use_cache, const vfts::Searcher &searcher, vfts::SearchOptions &o)
    {
        const std::vector<const vfts::Segment *> segs = searcher.all_segments();
        if (filter) {
            if (use_cache) {
                // a set of left-out documents per segment, kept in the filter cache
                exclude = vfts::filter_segments(*filter, segs, true);
                o.exclude = &exclude;
            } else {
                // asked per candidate: costs per match, not per document of the collection
                view = filter->view(segs);
                o.doc_filter = view.get();
            }
        }
        if (by)
            for (const vfts::Segment *g : segs) { by_keep.push_back(by->codes_for(*g)); by_codes.push_back(by_keep.back()->data()); }
    }
};

} // namespace vfts_udx

#endif
