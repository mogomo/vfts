#include "snippet.h"
#include "expand.h"
#include "stemmer.h"

#include <algorithm>
#include <set>

namespace vfts {

namespace {

struct Pattern {
    QKind kind;
    std::string word;           // Term, Fuzzy
    std::string key;            // Term in stem mode: stem key of the query word
    std::vector<std::uint32_t> cps;
    std::string prefix;         // Prefix; Wildcard literal prefix
    int edits = 0;
    int id = 0;                 // query word number (for "different words")
};

void collect(const QNode &q, std::vector<const QNode *> &out)
{
    switch (q.kind) {
    case QKind::Bool:
        for (auto &k : q.must) collect(k, out);
        for (auto &k : q.should) collect(k, out);
        break;
    case QKind::Near:
        collect(q.kids[0], out);
        collect(q.kids[1], out);
        break;
    case QKind::None:
        break;
    default:
        out.push_back(&q);
    }
}

} // namespace

struct Snippeter::Impl {
    Analyzer analyzer;
    Stemmer stemmer;
    SnippetOptions o;
    std::vector<Pattern> pats;
    std::vector<std::uint32_t> cps;
    Impl(const AnalyzerOptions &ao, const SnippetOptions &so) : analyzer(ao), o(so) {}

    int match(std::string_view term, Lang lang)
    {
        bool have_cps = false;
        for (const Pattern &p : pats) {
            switch (p.kind) {
            case QKind::Term:
                if (term == p.word) return p.id;
                if (o.match == MatchMode::Stem && stem_key(lang_info(lang).code, stemmer.stem(term, lang)) == p.key) return p.id;
                break;
            case QKind::Prefix:
                if (term.compare(0, p.prefix.size(), p.prefix) == 0) return p.id;
                break;
            case QKind::Wildcard:
                if (term.compare(0, p.prefix.size(), p.prefix) != 0) break;
                if (!have_cps) { to_code_points(term, cps); have_cps = true; }
                if (wildcard_match(p.cps, cps)) return p.id;
                break;
            case QKind::Fuzzy: {
                if (!have_cps) { to_code_points(term, cps); have_cps = true; }
                const std::size_t pl = std::min<std::size_t>(static_cast<std::size_t>(std::max(o.prefix_length, 0)), p.cps.size());
                if (cps.size() < pl || !std::equal(p.cps.begin(), p.cps.begin() + pl, cps.begin())) break;
                if (bounded_osa_distance(p.cps, cps, p.edits) <= p.edits) return p.id;
                break;
            }
            default:
                break;
            }
        }
        return -1;
    }
};

Snippeter::Snippeter(const AnalyzerOptions &aopts, const std::string &query, const SnippetOptions &opts)
    : impl_(new Impl(aopts, opts))
{
    const QNode q = parse_query(query, impl_->analyzer, opts.query);
    std::vector<const QNode *> leaves;
    collect(q, leaves);
    int id = 0;
    auto add_word = [&](const std::string &w, Lang lang) {
        if (w.empty()) return;
        Pattern p;
        p.kind = QKind::Term;
        p.word = w;
        p.key = stem_key(lang_info(lang).code, impl_->stemmer.stem(w, lang));
        p.id = id++;
        impl_->pats.push_back(p);
    };
    for (const QNode *n : leaves) {
        if (n->kind == QKind::Term || n->kind == QKind::Phrase) {
            for (std::size_t i = 0; i < n->words.size(); ++i) add_word(n->words[i], n->langs[i]);
            continue;
        }
        Pattern p;
        p.kind = n->kind;
        p.id = id++;
        if (n->kind == QKind::Prefix) p.prefix = n->pattern;
        else if (n->kind == QKind::Wildcard) { p.prefix = literal_prefix(n->pattern); to_code_points(n->pattern, p.cps); }
        else { p.word = n->words[0]; p.edits = n->edits; to_code_points(p.word, p.cps); }
        impl_->pats.push_back(p);
    }
}

Snippeter::~Snippeter() = default;

std::string Snippeter::snippet(std::string_view text, Lang lang)
{
    Impl &im = *impl_;
    im.analyzer.analyze(text, lang);
    const std::size_t n = im.analyzer.size();
    std::vector<int> hit(n, -1);
    hits_ = 0;
    for (std::size_t i = 0; i < n; ++i) {
        hit[i] = im.match(im.analyzer.term(i), im.analyzer.token(i).lang);
        hits_ += hit[i] >= 0;
    }
    if (n == 0) return std::string();
    const std::size_t w = static_cast<std::size_t>(std::max(1, im.o.words));
    // windows starting at every word: (different query words, hits)
    struct Win { std::size_t start; int distinct, count; };
    std::vector<Win> wins;
    for (std::size_t s = 0; s < n; ++s) {
        if (hits_ > 0 && hit[s] < 0) continue;      // a window starts at a hit (or at word 0 without hits)
        std::set<int> d;
        int c = 0;
        for (std::size_t i = s; i < std::min(n, s + w); ++i) if (hit[i] >= 0) { d.insert(hit[i]); ++c; }
        wins.push_back({s, static_cast<int>(d.size()), c});
        if (hits_ == 0) break;
    }
    std::sort(wins.begin(), wins.end(), [](const Win &a, const Win &b) {
        if (a.distinct != b.distinct) return a.distinct > b.distinct;
        if (a.count != b.count) return a.count > b.count;
        return a.start < b.start;
    });
    std::vector<std::size_t> chosen;
    for (const Win &x : wins) {
        if (static_cast<int>(chosen.size()) >= std::max(1, im.o.fragments)) break;
        bool overlap = false;
        for (std::size_t c : chosen) if (x.start < c + w && c < x.start + w) { overlap = true; break; }
        if (!overlap) chosen.push_back(x.start);
    }
    std::sort(chosen.begin(), chosen.end());
    // start each window a few words before its first hit, for context
    std::string out;
    for (std::size_t k = 0; k < chosen.size(); ++k) {
        std::size_t s = chosen[k];
        // move back for context, but keep every hit of the window inside it
        std::size_t last = s;
        for (std::size_t i = s; i < std::min(n, s + w); ++i) if (hit[i] >= 0) last = i;
        // a quarter of the window, or more where the window would run past the end of the text
        const std::size_t want = std::max(w / 4, s + w > n ? s + w - n : std::size_t(0));
        const std::size_t back = std::min({s, want, s + w - 1 - last});
        s -= back;
        if (k > 0 && s < chosen[k - 1] + w) s = chosen[k - 1] + w;
        const std::size_t e = std::min(n, s + w);
        if (s >= e) continue;
        if (!out.empty()) out += im.o.separator;
        if (s > 0 && k == 0) out += im.o.separator.substr(im.o.separator.find_first_not_of(' '));
        std::size_t at = im.analyzer.token(s).start;
        for (std::size_t i = s; i < e; ++i) {
            const Token &t = im.analyzer.token(i);
            out.append(text.substr(at, t.start - at));
            if (hit[i] >= 0) { out += im.o.pre; out.append(text.substr(t.start, t.end - t.start)); out += im.o.post; }
            else out.append(text.substr(t.start, t.end - t.start));
            at = t.end;
        }
        // keep closing punctuation of the last word ("Holmes." , "Holmes,")
        std::size_t tail = at;
        while (tail < text.size() && tail < at + 3 && std::ispunct(static_cast<unsigned char>(text[tail]))) ++tail;
        out.append(text.substr(at, tail - at));
        if (e < n && k + 1 == chosen.size()) out += im.o.separator.substr(0, im.o.separator.find_last_not_of(' ') + 1);
    }
    return out;
}

} // namespace vfts
