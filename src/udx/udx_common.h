// Helpers shared by the vtext UDx adapters. The adapters are thin: they read rows and parameters,
// call the engine and write rows. Errors: the adapters throw std::runtime_error and report it
// once, in their catch block, as "<function>: <cause>" (vt_report_error throws through a function
// pointer of the server; its exception type is not visible to the library).
#ifndef VTEXT_UDX_COMMON_H
#define VTEXT_UDX_COMMON_H

#include "Vertica.h"
#include "../engine/analyzer.h"
#include "../engine/cache.h"
#include "../engine/table_codec.h"
#include "../engine/language.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <stdexcept>
#include <string>

namespace vtext_udx {

using Vertica::vint;

[[noreturn]] inline void fail(const std::string &why) { throw std::runtime_error(why); }

inline bool one_of(const std::string &v, std::initializer_list<const char *> allowed)
{
    for (const char *a : allowed) if (v == a) return true;
    return false;
}

// Values with the precedence: function parameter, then session parameter
// (ALTER SESSION SET UDPARAMETER FOR vtext <name> = '...'), then the built-in default.
// Session parameters arrive as strings whatever they look like.
class Settings {
public:
    explicit Settings(Vertica::ServerInterface &srv)
        : params_(srv.getParamReader()), session_(srv.getUDSessionParamReader("library")) {}

    bool has(const char *name) { return params_.containsParameter(name) || session_.containsParameter(name); }

    std::string text(const char *name, const std::string &builtin)
    {
        if (params_.containsParameter(name)) return params_.getStringRef(name).str();
        if (session_.containsParameter(name)) return session_.getStringRef(name).str();
        return builtin;
    }

    std::int64_t integer(const char *name, std::int64_t builtin)
    {
        if (params_.containsParameter(name)) return params_.getIntRef(name);
        if (!session_.containsParameter(name)) return builtin;
        const std::string v = session_.getStringRef(name).str();
        char *end = nullptr;
        errno = 0;
        const long long x = std::strtoll(v.c_str(), &end, 10);
        if (v.empty() || *end || errno) fail(std::string("session parameter ") + name + " = '" + v + "' is not an integer");
        return x;
    }

    double real(const char *name, double builtin)
    {
        if (params_.containsParameter(name)) return params_.getFloatRef(name);
        if (!session_.containsParameter(name)) return builtin;
        const std::string v = session_.getStringRef(name).str();
        char *end = nullptr;
        const double x = std::strtod(v.c_str(), &end);
        if (v.empty() || *end || !std::isfinite(x)) fail(std::string("session parameter ") + name + " = '" + v + "' is not a number");
        return x;
    }

    bool boolean(const char *name, bool builtin)
    {
        if (params_.containsParameter(name)) return params_.getBoolRef(name) == Vertica::vbool_true;
        if (!session_.containsParameter(name)) return builtin;
        const std::string v = session_.getStringRef(name).str();
        if (v == "true" || v == "t" || v == "1" || v == "yes") return true;
        if (v == "false" || v == "f" || v == "0" || v == "no") return false;
        fail(std::string("session parameter ") + name + " = '" + v + "' is not true or false");
    }

private:
    Vertica::ParamReader params_, session_;
};

// Analyzer options from the parameters lang, accent_fold and on_unsupported.
inline vtext::AnalyzerOptions analyzer_options(Settings &s, const std::string &default_lang = "en")
{
    vtext::AnalyzerOptions o;
    o.default_lang = vtext::parse_lang(s.text("lang", default_lang));
    const std::string fold = s.text("accent_fold", "latin");
    if (fold == "latin") o.fold = vtext::AccentFold::Latin;
    else if (fold == "none") o.fold = vtext::AccentFold::None;
    else if (fold == "all") o.fold = vtext::AccentFold::All;
    else fail("accent_fold must be 'latin', 'none' or 'all', not '" + fold + "'");
    const std::string un = s.text("on_unsupported", "error");
    if (un == "plain") o.unsupported_plain = true;
    else if (un != "error") fail("on_unsupported must be 'error' or 'plain', not '" + un + "'");
    return o;
}

inline void add_analyzer_parameters(Vertica::SizedColumnTypes &p)
{
    p.addVarchar(16, "lang");
    p.addVarchar(16, "accent_fold");
    p.addVarchar(16, "on_unsupported");
}

// The analyzer options of an index as stored in its generation file:
// "lang=en;accent_fold=latin;on_unsupported=error".
inline vtext::AnalyzerOptions parse_index_options(const std::string &s)
{
    vtext::AnalyzerOptions o;
    o.default_lang = vtext::parse_lang("en");
    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t j = s.find(';', i);
        if (j == std::string::npos) j = s.size();
        const std::string kv = s.substr(i, j - i);
        const std::size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if (k == "lang") o.default_lang = vtext::parse_lang(v);
            else if (k == "accent_fold") o.fold = v == "none" ? vtext::AccentFold::None : v == "all" ? vtext::AccentFold::All : vtext::AccentFold::Latin;
            else if (k == "on_unsupported") o.unsupported_plain = v == "plain";
        }
        i = j + 1;
    }
    return o;
}

inline std::string index_options_string(const vtext::AnalyzerOptions &o)
{
    return std::string("lang=") + vtext::lang_info(o.default_lang).code + ";accent_fold=" +
           (o.fold == vtext::AccentFold::None ? "none" : o.fold == vtext::AccentFold::All ? "all" : "latin") +
           ";on_unsupported=" + (o.unsupported_plain ? "plain" : "error");
}

// cache_dir: function parameter, then session parameter, then /tmp/vtext.
inline std::string resolve_cache_dir(Settings &s) { return s.text("cache_dir", vtext::DEFAULT_CACHE_DIR); }

inline std::string read_index_name(Settings &s)
{
    const std::string name = s.text("index", "");
    if (name.empty()) fail("parameter index is required");
    if (!vtext::valid_index_name(name)) fail("index name '" + name + "' is not valid: use letters, digits and underscore");
    return name;
}

// "Index rows": the one row shape that tbuild writes and tload reads (docs/format.md):
//   kind  D doc_stats, T term_dict, P postings, Q positions (docno = part number), S segment
//         summary, G active segment of a generation, X tombstone
enum IndexCol { IC_KIND = 0, IC_SEGMENT, IC_TERM_ID, IC_CHUNK, IC_DOCNO, IC_DOC_ID, IC_N, IC_TERM, IC_STEM, IC_LANG, IC_BIN, IC_COUNT };

inline void add_index_row_types(Vertica::ColumnTypes &t)
{
    t.addVarchar(); t.addInt(); t.addInt(); t.addInt(); t.addInt(); t.addInt(); t.addInt();
    t.addVarchar(); t.addVarchar(); t.addVarchar(); t.addLongVarbinary();
}

// Declared widths matter: Vertica sizes buffers by them (a LONG VARBINARY(32000000) column made the
// build 20 times slower than LONG VARBINARY(65000), docs/VERTICA_NOTES.md). Terms are at most 255
// bytes; payloads are cut into parts of vtext::PART_BYTES.
inline void add_index_row_columns(Vertica::SizedColumnTypes &t, std::int64_t bin_bytes = 65000)
{
    t.addVarchar(1, "kind");
    t.addInt("segment_id");
    t.addInt("term_id");
    t.addInt("chunk_id");
    t.addInt("docno");
    t.addInt("doc_id");
    t.addInt("n");
    t.addVarchar(256, "term");
    t.addVarchar(256, "stem");
    t.addVarchar(8, "lang");
    t.addLongVarbinary(static_cast<int>(bin_bytes), "bin");
}

// Starts an index row: kind and segment set, every other column NULL.
inline void begin_index_row(Vertica::PartitionWriter &out, const char *kind, std::int64_t sid)
{
    out.getStringRef(IC_KIND).copy(kind);
    out.setInt(IC_SEGMENT, sid);
    for (int c : {IC_TERM_ID, IC_CHUNK, IC_DOCNO, IC_DOC_ID, IC_N}) out.setNull(c);
    for (int c : {IC_TERM, IC_STEM, IC_LANG, IC_BIN}) out.setNull(c);
}

// Writes one segment as index rows: D, T, P, Q rows and the S summary row (calling thread only).
inline void write_segment_rows(Vertica::PartitionWriter &out, const std::string &bytes)
{
    const vtext::Segment seg(bytes.data(), bytes.size());
    const std::int64_t sid = seg.id();
    vtext::RowSink sink;
    sink.doc = [&](const vtext::DocRow &r) {
        begin_index_row(out, "D", sid);
        out.setInt(IC_DOCNO, r.docno);
        out.setInt(IC_DOC_ID, r.doc_id);
        out.setInt(IC_N, r.length);
        out.next();
    };
    sink.term = [&](const vtext::TermDictRow &r) {
        begin_index_row(out, "T", sid);
        out.setInt(IC_TERM_ID, r.term_id);
        out.setInt(IC_N, r.doc_freq);
        out.getStringRef(IC_TERM).copy(r.term.data(), r.term.size());
        out.getStringRef(IC_STEM).copy(r.stem.data(), r.stem.size());
        out.getStringRef(IC_LANG).copy(r.lang.data(), r.lang.size());
        out.next();
    };
    sink.posting = [&](const vtext::PostingRow &r) {
        begin_index_row(out, "P", sid);
        out.setInt(IC_TERM_ID, r.term_id);
        out.setInt(IC_CHUNK, r.chunk_id);
        out.getStringRef(IC_BIN).copy(r.docs.data(), r.docs.size());
        out.next();
        for (std::size_t at = 0, part = 0; at < r.payload.size() || part == 0; at += vtext::PART_BYTES, ++part) {
            const std::size_t n = std::min(vtext::PART_BYTES, r.payload.size() - at);
            begin_index_row(out, "Q", sid);
            out.setInt(IC_TERM_ID, r.term_id);
            out.setInt(IC_CHUNK, r.chunk_id);
            out.setInt(IC_DOCNO, static_cast<vint>(part));
            out.getStringRef(IC_BIN).copy(r.payload.data() + at, n);
            out.next();
        }
    };
    vtext::segment_to_rows(seg, sink);
    const vtext::SegmentHeader &h = seg.header();
    begin_index_row(out, "S", sid);
    out.setInt(IC_N, h.n_docs);
    out.setInt(IC_TERM_ID, h.n_terms);
    out.setInt(IC_DOCNO, static_cast<vint>(h.sum_len));
    out.setInt(IC_CHUNK, static_cast<vint>(h.total_bytes));
    out.setInt(IC_DOC_ID, static_cast<vint>(h.checksum));
    out.next();
}

} // namespace vtext_udx

#endif
