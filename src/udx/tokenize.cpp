// vfts.tokenize(text [, lang] USING PARAMETERS lang, accent_fold, on_unsupported)
// Shows what the analyzer makes of a text: one row per word with its position, the indexed term,
// its stem, the stemmer language and the byte offsets in the text. An exploder: call it without
// OVER(), beside other columns (SELECT doc_id, vfts.tokenize(body) FROM t).
#include "udx_common.h"
#include "../engine/stemmer.h"

using namespace Vertica;
using namespace vfts_udx;

namespace {

class Tokenize : public TransformFunction {
    std::unique_ptr<vfts::Analyzer> analyzer_;
    std::unique_ptr<vfts::Stemmer> stemmer_;
    bool lang_column_ = false;

public:
    void setup(ServerInterface &srv, const SizedColumnTypes &argTypes) override
    {
        try {
            Settings s(srv);
            analyzer_.reset(new vfts::Analyzer(analyzer_options(s)));
            stemmer_.reset(new vfts::Stemmer());
            lang_column_ = argTypes.getColumnCount() == 2;
        } catch (const std::exception &e) {
            vt_report_error(0, "tokenize: %s", e.what());
        }
    }

    void processPartition(ServerInterface &srv, PartitionReader &in, PartitionWriter &out) override
    {
        try {
            do {
                if (in.isNull(0)) continue;
                const VString &text = in.getStringRef(0);
                vfts::Lang lang = vfts::LANG_NONE;
                if (lang_column_ && !in.isNull(1)) lang = vfts::parse_lang(in.getStringRef(1).str());
                analyzer_->analyze(std::string_view(text.data(), text.length()), lang);
                for (std::size_t i = 0; i < analyzer_->size(); ++i) {
                    const vfts::Token &t = analyzer_->token(i);
                    const std::string_view term = analyzer_->term(i);
                    const std::string_view st = stemmer_->stem(term, t.lang);
                    out.setInt(0, t.pos);
                    out.getStringRef(1).copy(term.data(), term.size());
                    out.getStringRef(2).copy(st.data(), st.size());
                    const char *code = vfts::lang_info(t.lang).code;
                    out.getStringRef(3).copy(code);
                    out.setInt(4, t.start);
                    out.setInt(5, t.end);
                    out.next();
                }
            } while (in.next());
        } catch (const std::exception &e) {
            vt_report_error(0, "tokenize: %s", e.what());
        }
    }
};

class TokenizeFactory : public TransformFunctionFactory {
    bool with_lang_;

public:
    explicit TokenizeFactory(bool with_lang) : with_lang_(with_lang) {}

    void getPrototype(ServerInterface &, ColumnTypes &argTypes, ColumnTypes &returnType) override
    {
        argTypes.addLongVarchar();
        if (with_lang_) argTypes.addVarchar();
        returnType.addInt();
        returnType.addVarchar();
        returnType.addVarchar();
        returnType.addVarchar();
        returnType.addInt();
        returnType.addInt();
    }

    void getReturnType(ServerInterface &, const SizedColumnTypes &, SizedColumnTypes &out) override
    {
        out.addInt("pos");
        out.addVarchar(1024, "term");
        out.addVarchar(1024, "stem");
        out.addVarchar(8, "lang");
        out.addInt("start_byte");
        out.addInt("end_byte");
    }

    void getParameterType(ServerInterface &, SizedColumnTypes &p) override { add_analyzer_parameters(p); }

    void getFunctionProperties(ServerInterface &, const SizedColumnTypes &, Properties &properties) override
    {
        properties.isExploder = true;
    }

    TransformFunction *createTransformFunction(ServerInterface &srv) override
    {
        return vt_createFuncObject<Tokenize>(srv.allocator);
    }
};

class TokenizeFactory1 : public TokenizeFactory { public: TokenizeFactory1() : TokenizeFactory(false) {} };
class TokenizeFactory2 : public TokenizeFactory { public: TokenizeFactory2() : TokenizeFactory(true) {} };

} // namespace

RegisterFactory(TokenizeFactory1);
RegisterFactory(TokenizeFactory2);
