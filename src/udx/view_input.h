// The input of the search functions: the rows of a view made by vfts.create_index
// (qid, query, doc_id, body, lang, op, gen), read once, and the access check (docs/design.md
// "Access").
//
// The role of a row is given by its columns:
//   qid and query set       a query (more than one: a batch)
//   doc_id set, op 'T'      a row of the live tail: newer than the segments; masks the doc_id in the
//                           segments and is analyzed and searched now
//   doc_id set, op 'D'      a doc_id changed or deleted since the segments (journal): masked
//   op 'H', query set       documents of the segments that the user may not see (row access
//                           policy of the source table), comma separated doc ids: masked
//   only gen and query set  the sentinel: the generation the view belongs to, and in query the
//                           view's access token
//
// The token proves that the rows come from the view: a statement that makes its own rows cannot
// know it. Mode "open": md5(key:gen:key), a literal in the views (readable by whoever may read the
// views, nobody else). Mode "rows": md5(key:gen:user:n:s1:s2:key) over the H rows, computed by the
// view per statement with the key from vfts.index_keys (which users cannot read), so leaving
// out an H row, or reusing another user's token, gives a wrong token. Mode "masked:<column>": a
// column access policy masks an indexed column; vfts refuses to search.
#ifndef VFTS_UDX_VIEW_INPUT_H
#define VFTS_UDX_VIEW_INPUT_H

#include "udx_common.h"
#include "../engine/builder.h"
#include "../engine/md5.h"
#include "../engine/parallel.h"
#include "../engine/search.h"

#include <cstdio>
#include <cstring>
#include <set>

namespace vfts_udx {

struct ViewInput {
    enum Col { C_QID = 0, C_QUERY, C_DOC_ID, C_BODY, C_LANG, C_OP, C_GEN };
    struct TailDoc {
        std::int64_t doc_id;
        std::string text;
        vfts::Lang lang;
    };
    std::vector<std::pair<vint, std::string>> queries;      // query rows
    std::vector<std::int64_t> mask;                         // T, D and H rows
    std::vector<std::int64_t> hidden;                       // H rows
    std::vector<TailDoc> tail;
    std::int64_t gen = 0;
    std::string token;
    bool tokens_differ = false;

    // Reads every row of the partition. take_queries: query rows are queries (tsearch); else they
    // are ignored (tserve gets its queries over its channel).
    void read(Vertica::PartitionReader &in, bool take_queries)
    {
        std::set<std::int64_t> in_tail;
        do {
            if (!in.isNull(C_GEN)) gen = std::max<std::int64_t>(gen, in.getIntRef(C_GEN));
            if (!in.isNull(C_QID)) {
                if (take_queries && !in.isNull(C_QUERY)) queries.emplace_back(in.getIntRef(C_QID), in.getStringRef(C_QUERY).str());
                continue;
            }
            if (in.isNull(C_DOC_ID)) {
                if (!in.isNull(C_OP) && !in.isNull(C_QUERY) && in.getStringRef(C_OP).length() == 1 && in.getStringRef(C_OP).data()[0] == 'H') {
                    // hidden documents, comma separated
                    const Vertica::VString &q = in.getStringRef(C_QUERY);
                    const char *p = q.data(), *e = p + q.length();
                    while (p < e) {
                        bool neg = false;
                        if (*p == '-') { neg = true; ++p; }
                        if (p >= e || *p < '0' || *p > '9') fail("H row: not a list of doc ids");
                        std::uint64_t v = 0;
                        while (p < e && *p >= '0' && *p <= '9') v = v * 10 + static_cast<std::uint64_t>(*p++ - '0');
                        const std::int64_t id = neg ? -static_cast<std::int64_t>(v) : static_cast<std::int64_t>(v);
                        hidden.push_back(id);
                        mask.push_back(id);
                        if (p < e) { if (*p != ',') fail("H row: not a list of doc ids"); ++p; }
                    }
                    continue;
                }
                if (!in.isNull(C_QUERY)) {
                    const std::string t = in.getStringRef(C_QUERY).str();
                    if (!token.empty() && t != token) tokens_differ = true;
                    token = t;
                }
                continue;
            }
            const std::int64_t id = in.getIntRef(C_DOC_ID);
            mask.push_back(id);
            char op = 0;
            if (!in.isNull(C_OP)) { const Vertica::VString &o = in.getStringRef(C_OP); if (o.length() == 1) op = o.data()[0]; }
            if (op == 'H') { hidden.push_back(id); continue; }
            if (op == 'D' || !in_tail.insert(id).second) continue;
            TailDoc d{id, std::string(), vfts::LANG_NONE};
            if (!in.isNull(C_BODY)) { const Vertica::VString &b = in.getStringRef(C_BODY); d.text.assign(b.data(), b.length()); }
            if (!in.isNull(C_LANG) && !vfts::find_lang(in.getStringRef(C_LANG).str(), d.lang)) d.lang = vfts::LANG_NONE;
            tail.push_back(std::move(d));
        } while (in.next());
    }

    // Opens the index for these rows and checks the token (throws when the rows are not the
    // index's view, or the index may not be searched).
    std::shared_ptr<const vfts::LoadedIndex> open(const std::string &cache_dir, const std::string &index, const std::string &user) const
    {
        if (gen <= 0 || token.empty())
            fail("the input is not a view of index " + index + ": search FROM <schema>." + index + "_live or _snap (the views made by vfts.create_index)");
        if (tokens_differ) fail("the input mixes views of different indexes or generations");
        // In mode "rows" the H rows belong to the view's generation: open exactly that one.
        auto li = vfts::open_index(cache_dir, index, gen, !hidden.empty());
        if (li->access == "rows" && li->gen != gen) li = vfts::open_index(cache_dir, index, gen, true);
        check(*li, index, user);
        return li;
    }

    // A searcher over the index, with the masks and the live tail of these rows.
    std::unique_ptr<vfts::Searcher> searcher(const vfts::LoadedIndex &li) const
    {
        const vfts::AnalyzerOptions aopts = parse_index_options(li.options);
        std::unique_ptr<vfts::Searcher> s(new vfts::Searcher(aopts, li.refs));
        if (!mask.empty()) s->mask(mask);
        if (!tail.empty()) {
            vfts::SegmentBuilder b(aopts);
            for (const TailDoc &d : tail) {
                try { b.add(d.doc_id, d.text, d.lang); }
                catch (const std::exception &e) { fail("live tail, doc_id " + std::to_string(d.doc_id) + ": " + e.what()); }
            }
            s->add_tail(std::make_shared<const std::string>(b.finish(-1)));
        }
        return s;
    }

    static std::string rows_digest(const std::string &key, std::int64_t gen, const std::string &user, const std::vector<std::int64_t> &hidden)
    {
        // as the view computes it: n, and the sums of the first and second 32 bits of
        // md5(key:doc_id) over the hidden documents (in parallel: one MD5 per hidden document)
        const std::size_t n = hidden.size(), parts = n < 4096 ? 1 : std::min<std::size_t>(64, n / 4096);
        std::vector<std::uint64_t> p1(parts, 0), p2(parts, 0);
        vfts::Pool::instance().run(parts, parts, [&](std::size_t c) {
            char buf[96];
            std::memcpy(buf, key.data(), key.size());
            buf[key.size()] = ':';
            for (std::size_t i = n * c / parts; i < n * (c + 1) / parts; ++i) {
                const int l = std::snprintf(buf + key.size() + 1, sizeof buf - key.size() - 1, "%lld", static_cast<long long>(hidden[i]));
                unsigned char d[16];
                vfts::md5_raw(buf, key.size() + 1 + static_cast<std::size_t>(l), d);
                p1[c] += static_cast<std::uint64_t>(d[0]) << 24 | static_cast<std::uint64_t>(d[1]) << 16 | static_cast<std::uint64_t>(d[2]) << 8 | d[3];
                p2[c] += static_cast<std::uint64_t>(d[4]) << 24 | static_cast<std::uint64_t>(d[5]) << 16 | static_cast<std::uint64_t>(d[6]) << 8 | d[7];
            }
        });
        std::uint64_t s1 = 0, s2 = 0;
        for (std::size_t c = 0; c < parts; ++c) { s1 += p1[c]; s2 += p2[c]; }
        return vfts::md5_hex(key + ":" + std::to_string(gen) + ":" + user + ":" + std::to_string(hidden.size()) + ":" +
                              std::to_string(s1) + ":" + std::to_string(s2) + ":" + key);
    }

    // Every search analyzes every row of the live tail: a warning when the tail has more than
    // tail_warn rows (parameter or session parameter; default 10,000; 0: never). Vertica writes a
    // function's warnings to vertica.log and the Data Collector (dc_errors), not to the client.
    void warn_long_tail(Vertica::ServerInterface &srv, Settings &s, const std::string &index) const
    {
        const std::int64_t lim = s.integer("tail_warn", 10000);
        if (lim <= 0 || static_cast<std::int64_t>(tail.size()) <= lim) return;
        srv.reportWarning(Vertica::ClientMessage(1, "vfts: the live tail of index " + index + " has " + std::to_string(tail.size()) +
                                                    " rows, which every search analyzes again: CALL vfts.refresh_index('" + index +
                                                    "'), or schedule its refresh (vfts.alter_index option refresh_every)"));
    }

private:
    void check(const vfts::LoadedIndex &li, const std::string &index, const std::string &user) const
    {
        if (li.access.empty() || li.keys.empty())
            fail("index " + index + " has no access key on this node (made by an older vfts): run CALL vfts.check_access('" + index + "')");
        if (li.access.rfind("masked:", 0) == 0)
            fail("index " + index + ": the column " + li.access.substr(7) +
                 " of the source table has a column access policy; vfts does not search masked text (README: Access policies)");
        for (const std::string &k : li.keys) {
            std::string want;
            if (li.access == "open") want = vfts::md5_hex(k + ":" + std::to_string(gen) + ":" + k);
            else if (li.access == "rows") want = rows_digest(k, gen, user, hidden);
            else fail("index " + index + ": unknown access mode '" + li.access + "' on this node");
            if (want == token) return;
        }
        fail("the input is not the current view of index " + index + " for this user (wrong access token): search FROM <schema>." + index +
             "_live or _snap; after CALL vfts.check_access, start the statement again");
    }
};

} // namespace vfts_udx

#endif
