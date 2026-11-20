// Helpers shared by the vtext UDx adapters. The adapters are thin: they read rows and parameters,
// call the engine and write rows. Errors: the adapters throw std::runtime_error and report it
// once, in their catch block, as "<function>: <cause>" (vt_report_error throws through a function
// pointer of the server; its exception type is not visible to the library).
#ifndef VTEXT_UDX_COMMON_H
#define VTEXT_UDX_COMMON_H

#include "Vertica.h"
#include "../engine/analyzer.h"
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

} // namespace vtext_udx

#endif
