// Snippets: hits marked at their byte offsets, stem matches, patterns, NOT ignored, windows.
#include "check.h"
#include "../../src/engine/snippet.h"

using namespace vtext;

static std::string S(const std::string &text, const std::string &q, int words = 20, int fragments = 2, MatchMode m = MatchMode::Stem)
{
    AnalyzerOptions ao;
    ao.default_lang = parse_lang("en");
    SnippetOptions o;
    o.words = words;
    o.fragments = fragments;
    o.match = m;
    Snippeter s(ao, q, o);
    return s.snippet(text);
}

int main()
{
    CHECK_EQ(S("The Adventures of Sherlock Holmes' Smarter Brother", "holmes"),
             std::string("The Adventures of Sherlock <b>Holmes</b>' Smarter Brother"));
    CHECK_EQ(S("He was running; she runs.", "run"), std::string("He was <b>running</b>; she <b>runs</b>."));
    CHECK_EQ(S("He was running; she runs.", "run", 20, 2, MatchMode::Exact), std::string("He was running; she runs."));
    CHECK_EQ(S("indemnity and indemnification", "indemn*"), std::string("<b>indemnity</b> and <b>indemnification</b>"));
    CHECK_EQ(S("test text tent", "te?t"), std::string("<b>test</b> <b>text</b> <b>tent</b>"));
    CHECK_EQ(S("Sherlock Holmes", "holmse~1"), std::string("Sherlock <b>Holmes</b>"));
    CHECK_EQ(S("contract and indemnity", "contract NOT indemnity"), std::string("<b>contract</b> and indemnity"));
    CHECK_EQ(S("a contract with an indemnity clause", "contract w/5 indemnity"),
             std::string("a <b>contract</b> with an <b>indemnity</b> clause"));
    CHECK_EQ(S("Café au lait", "cafe"), std::string("<b>Café</b> au lait"));
    // windows: the best window of 4 words, marked as a part of the text
    const std::string t = "one two three four five six seven eight contract nine ten indemnity eleven twelve thirteen";
    CHECK_EQ(S(t, "contract indemnity", 4, 1), std::string("... <b>contract</b> nine ten <b>indemnity</b> ..."));
    // a hit near the end: the window is filled from the left
    CHECK_EQ(S("The indemnity clause of the contract protects both parties.", "parties", 3, 1),
             std::string("... protects both <b>parties</b>."));
    // no hit: the beginning of the text
    CHECK_EQ(S("alpha beta gamma delta", "zulu", 2, 1), std::string("alpha beta ..."));
    // Hebrew prefix stem, Arabic article
    AnalyzerOptions ao;
    ao.default_lang = parse_lang("he");
    SnippetOptions o;
    Snippeter he(ao, "בית", o);
    CHECK_EQ(he.snippet("ראינו את הבית"), std::string("ראינו את <b>הבית</b>"));
    return finish("test_snippet");
}
