// vtext.tsnippet(body, query [USING PARAMETERS index, mode, lang, words, fragments, pre_tag,
//                post_tag, separator, prefix_length])
//
// A scalar function: the parts of `body` that match `query`, with the hits marked (snippet.h).
// With `index`, the text is analyzed with that index's settings (read from the node's cache), so
// the marks agree with what tsearch matched. Join it to the search result, for the top hits only:
//   SELECT r.rank, d.doc_id, vtext.tsnippet(d.body, 'contract w/5 indemnity' USING PARAMETERS index='emails')
//   FROM (SELECT vtext.tsearch(...) OVER() FROM app.emails_live) r JOIN app.emails d USING (doc_id);
#include "udx_common.h"
#include "../engine/snippet.h"

using namespace Vertica;
using namespace vtext_udx;

namespace {

class TSnippet : public ScalarFunction {
    vtext::AnalyzerOptions aopts_;
    vtext::SnippetOptions sopts_;
    std::unique_ptr<vtext::Snippeter> sn_;
    std::string last_query_;
    bool lang_column_ = false;

public:
    void setup(ServerInterface &srv, const SizedColumnTypes &argTypes) override
    {
        try {
            Settings s(srv);
            if (s.has("index")) {
                auto li = vtext::open_index(resolve_cache_dir(s), read_index_name(s), 0);
                aopts_ = parse_index_options(li->options);
            } else {
                aopts_ = analyzer_options(s);
            }
            const std::string mode = s.has("mode") ? s.text("mode", "stem") : s.text("match", "stem");
            if (mode == "exact") sopts_.match = vtext::MatchMode::Exact;
            else if (mode != "stem") fail("mode must be 'stem' or 'exact'");
            if (s.has("lang")) sopts_.query.lang = vtext::parse_lang(s.text("lang", ""));
            sopts_.words = static_cast<int>(s.integer("words", 20));
            sopts_.fragments = static_cast<int>(s.integer("fragments", 2));
            if (sopts_.words < 1 || sopts_.words > 10000 || sopts_.fragments < 1 || sopts_.fragments > 100)
                fail("words must be 1 to 10000 and fragments 1 to 100");
            sopts_.prefix_length = static_cast<int>(s.integer("prefix_length", 0));
            sopts_.pre = s.text("pre_tag", "<b>");
            sopts_.post = s.text("post_tag", "</b>");
            sopts_.separator = s.text("separator", " ... ");
            lang_column_ = argTypes.getColumnCount() == 3;
        } catch (const std::exception &e) {
            vt_report_error(0, "tsnippet: %s", e.what());
        }
    }

    void processBlock(ServerInterface &, BlockReader &in, BlockWriter &out) override
    {
        try {
            do {
                if (in.isNull(0) || in.isNull(1)) { out.getStringRef().setNull(); out.next(); continue; }
                const std::string q = in.getStringRef(1).str();
                if (!sn_ || q != last_query_) {
                    sn_.reset(new vtext::Snippeter(aopts_, q, sopts_));
                    last_query_ = q;
                }
                vtext::Lang lang = vtext::LANG_NONE;
                if (lang_column_ && !in.isNull(2)) vtext::find_lang(in.getStringRef(2).str(), lang);
                const VString &body = in.getStringRef(0);
                std::string r = sn_->snippet(std::string_view(body.data(), body.length()), lang);
                if (r.size() > 65000) r.resize(65000);
                out.getStringRef().copy(r.data(), r.size());
                out.next();
            } while (in.next());
        } catch (const std::exception &e) {
            vt_report_error(0, "tsnippet: %s", e.what());
        }
    }
};

class TSnippetFactory : public ScalarFunctionFactory {
    bool with_lang_;

public:
    explicit TSnippetFactory(bool with_lang) : with_lang_(with_lang) {}
    void getPrototype(ServerInterface &, ColumnTypes &a, ColumnTypes &r) override
    {
        a.addLongVarchar();
        a.addVarchar();
        if (with_lang_) a.addVarchar();
        r.addVarchar();
    }
    void getReturnType(ServerInterface &, const SizedColumnTypes &, SizedColumnTypes &out) override { out.addVarchar(65000, "snippet"); }
    void getParameterType(ServerInterface &, SizedColumnTypes &p) override
    {
        add_analyzer_parameters(p);
        p.addVarchar(128, "index");
        p.addVarchar(1024, "cache_dir");
        p.addVarchar(16, "mode");
        p.addVarchar(16, "match");
        p.addInt("words");
        p.addInt("fragments");
        p.addInt("prefix_length");
        p.addVarchar(256, "pre_tag");
        p.addVarchar(256, "post_tag");
        p.addVarchar(256, "separator");
    }
    ScalarFunction *createScalarFunction(ServerInterface &srv) override { return vt_createFuncObject<TSnippet>(srv.allocator); }
};

class TSnippetFactory2 : public TSnippetFactory { public: TSnippetFactory2() : TSnippetFactory(false) {} };
class TSnippetFactory3 : public TSnippetFactory { public: TSnippetFactory3() : TSnippetFactory(true) {} };

} // namespace

RegisterFactory(TSnippetFactory2);
RegisterFactory(TSnippetFactory3);
