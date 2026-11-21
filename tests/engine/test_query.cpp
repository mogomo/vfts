// Query parser: canonical forms and error messages.
#include "check.h"
#include "../../src/engine/query.h"

using namespace vtext;

static std::string P(const char *q, bool op_and = false, bool stop = false)
{
    AnalyzerOptions ao;
    ao.default_lang = parse_lang("en");
    Analyzer a(ao);
    QueryOptions o;
    o.op_and = op_and;
    o.stopwords = stop;
    return to_string(parse_query(q, a, o));
}

static std::string E(const char *q)
{
    try { P(q); } catch (const std::runtime_error &e) { return e.what(); }
    return "no error";
}

int main()
{
    CHECK_EQ(P("Contract"), std::string("t:contract"));
    CHECK_EQ(P("Holmes-style"), std::string("\"holmes style\""));
    CHECK_EQ(P("a b"), std::string("(t:a t:b)"));
    CHECK_EQ(P("a b", true), std::string("(+t:a +t:b)"));
    CHECK_EQ(P("a AND b"), std::string("(+t:a +t:b)"));
    CHECK_EQ(P("a && b || c"), std::string("((+t:a +t:b) t:c)"));
    CHECK_EQ(P("a OR b AND c"), std::string("(t:a (+t:b +t:c))"));
    CHECK_EQ(P("a NOT b"), std::string("(+t:a -t:b)"));
    CHECK_EQ(P("a AND NOT b"), std::string("(+t:a -t:b)"));
    CHECK_EQ(P("a -b +c"), std::string("(+t:c t:a -t:b)"));
    CHECK_EQ(P("NOT b"), std::string("(-t:b)"));
    CHECK_EQ(P("(a OR b) AND (c OR d)"), std::string("(+(t:a t:b) +(t:c t:d))"));
    CHECK_EQ(P("\"roman empire\"~10"), std::string("\"roman empire\"~10"));
    CHECK_EQ(P("\"Roman Empire\""), std::string("\"roman empire\""));
    CHECK_EQ(P("\"single\""), std::string("t:single"));
    CHECK_EQ(P("contract w/5 indemnity"), std::string("(t:contract w/5 t:indemnity)"));
    CHECK_EQ(P("contract W/5 indemnity"), std::string("(t:contract w/5 t:indemnity)"));
    CHECK_EQ(P("contract pre/5 indemn*"), std::string("(t:contract pre/5 p:indemn*)"));
    CHECK_EQ(P("(a OR b) w/3 c"), std::string("((t:a t:b) w/3 t:c)"));
    CHECK_EQ(P("a w/3 b w/2 c"), std::string("((t:a w/3 t:b) w/2 t:c)"));
    CHECK_EQ(P("\"a b\" w/3 c AND d"), std::string("(+(\"a b\" w/3 t:c) +t:d)"));
    CHECK_EQ(P("te?t"), std::string("w:te?t"));
    CHECK_EQ(P("*tract"), std::string("w:*tract"));
    CHECK_EQ(P("Café*"), std::string("p:cafe*"));
    CHECK_EQ(P("holmse~"), std::string("f:holmse~2"));
    CHECK_EQ(P("holmse~1"), std::string("f:holmse~1"));
    CHECK_EQ(P("the contract", false, true), std::string("t:contract"));
    CHECK_EQ(P("\"united of america\"", false, true), std::string("\"united ? america\""));
    CHECK_EQ(P("the", false, true), std::string("<none>"));
    CHECK_EQ(P("a \\AND b"), std::string("(t:a t:and t:b)"));
    CHECK_EQ(P("2024 3.14"), std::string("(t:2024 t:3.14)"));
    CHECK_EQ(P(""), std::string("<none>"));
    CHECK_EQ(P("  ,,, "), std::string("<none>"));
    {   // plain: no syntax, every word a clause, repeated words kept
        AnalyzerOptions ao;
        ao.default_lang = parse_lang("en");
        Analyzer a(ao);
        QueryOptions o;
        o.plain = true;
        CHECK_EQ(to_string(parse_query("what is (the) \"price\" of new-york, new AND old?", a, o)),
                 std::string("(t:what t:is t:the t:price t:of t:new t:york t:new t:and t:old)"));
        CHECK_EQ(to_string(parse_query("holmes", a, o)), std::string("t:holmes"));
        o.op_and = true;
        CHECK_EQ(to_string(parse_query("a b", a, o)), std::string("(+t:a +t:b)"));
    }
    // errors
    CHECK(E("a AND").find("AND without a right operand") != std::string::npos);
    CHECK(E("(a").find("missing )") != std::string::npos);
    CHECK(E("a)").find("unmatched )") != std::string::npos);
    CHECK(E("\"abc").find("missing closing quote") != std::string::npos);
    CHECK(E("a w/5 (b AND c)").find("operands of w/N") != std::string::npos);
    CHECK(E("e-mail~1").find("fuzzy term must be one word") != std::string::npos);
    CHECK(E("x~3").find("at most 2 edits") != std::string::npos);
    CHECK(E("a w/0 b").find("N >= 1") != std::string::npos);
    CHECK(E("*").find("at least one letter") != std::string::npos);
    CHECK(E("\"a b\"~").find("number must follow") != std::string::npos);
    return finish("test_query");
}
