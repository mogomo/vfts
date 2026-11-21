#include "query.h"

#include <cctype>
#include <cstdlib>
#include <stdexcept>

namespace vtext {

namespace {

enum class Tok { End, LParen, RParen, And, Or, Not, Plus, Minus, Near, Quoted, Word };

struct Token {
    Tok kind = Tok::End;
    std::string text;
    int num = 0;            // Near: distance; Quoted: slop (-1 none); Word: fuzzy edits (-1 none)
    bool ordered = false;   // Near: pre/N
    bool wild = false;      // Word: has an unescaped * or ?
    std::size_t at = 0;     // byte offset in the query, for messages
};

[[noreturn]] void syntax(const std::string &why, std::size_t at)
{
    throw std::runtime_error("query syntax: " + why + " (at character " + std::to_string(at + 1) + ")");
}

class Lexer {
public:
    explicit Lexer(std::string_view q) : q_(q) {}

    Token next()
    {
        while (i_ < q_.size() && std::isspace(static_cast<unsigned char>(q_[i_]))) ++i_;
        Token t;
        t.at = i_;
        if (i_ >= q_.size()) return t;
        const char c = q_[i_];
        if (c == '(') { ++i_; t.kind = Tok::LParen; return t; }
        if (c == ')') { ++i_; t.kind = Tok::RParen; return t; }
        if (c == '"') return quoted(t);
        if ((c == '+' || c == '-') && i_ + 1 < q_.size() && !std::isspace(static_cast<unsigned char>(q_[i_ + 1])) &&
            q_[i_ + 1] != ')') {
            ++i_;
            t.kind = c == '+' ? Tok::Plus : Tok::Minus;
            return t;
        }
        return word(t);
    }

private:
    Token quoted(Token t)
    {
        ++i_;
        std::string s;
        while (i_ < q_.size() && q_[i_] != '"') {
            if (q_[i_] == '\\' && i_ + 1 < q_.size()) ++i_;
            s += q_[i_++];
        }
        if (i_ >= q_.size()) syntax("missing closing quote", t.at);
        ++i_;
        t.kind = Tok::Quoted;
        t.text = s;
        t.num = -1;
        if (i_ < q_.size() && q_[i_] == '~') {
            ++i_;
            std::size_t j = i_;
            while (j < q_.size() && std::isdigit(static_cast<unsigned char>(q_[j]))) ++j;
            if (j == i_) syntax("a number must follow ~ after a phrase", i_);
            t.num = std::atoi(std::string(q_.substr(i_, j - i_)).c_str());
            if (j - i_ > 6) syntax("phrase slop too large", i_);
            i_ = j;
        }
        return t;
    }

    Token word(Token t)
    {
        std::string s;
        bool escaped_any = false;
        while (i_ < q_.size()) {
            const char c = q_[i_];
            if (std::isspace(static_cast<unsigned char>(c)) || c == '(' || c == ')' || c == '"') break;
            if (c == '\\' && i_ + 1 < q_.size()) { s += q_[i_ + 1]; i_ += 2; escaped_any = true; continue; }
            if (c == '*' || c == '?') t.wild = true;
            s += c;
            ++i_;
        }
        t.num = -1;
        if (!escaped_any) {
            if (s == "AND" || s == "&&") { t.kind = Tok::And; return t; }
            if (s == "OR" || s == "||") { t.kind = Tok::Or; return t; }
            if (s == "NOT") { t.kind = Tok::Not; return t; }
            // w/N and pre/N, any case
            std::string low;
            for (char ch : s) low += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            const std::size_t slash = low.find('/');
            if (slash != std::string::npos && (low.compare(0, slash, "w") == 0 || low.compare(0, slash, "pre") == 0)) {
                const std::string n = low.substr(slash + 1);
                if (!n.empty() && n.size() <= 6 && n.find_first_not_of("0123456789") == std::string::npos) {
                    t.kind = Tok::Near;
                    t.num = std::atoi(n.c_str());
                    t.ordered = low[0] == 'p';
                    if (t.num < 1) syntax("w/N and pre/N need N >= 1", t.at);
                    return t;
                }
            }
            // fuzzy: word~ or word~N
            const std::size_t tilde = s.rfind('~');
            if (tilde != std::string::npos && tilde > 0) {
                const std::string n = s.substr(tilde + 1);
                if (n.empty() || n.find_first_not_of("0123456789") == std::string::npos) {
                    t.num = n.empty() ? 2 : std::atoi(n.c_str());
                    if (n.size() > 1 || t.num > 2) syntax("fuzzy terms allow at most 2 edits (word~, word~1, word~2)", t.at);
                    s.resize(tilde);
                }
            }
        }
        t.kind = Tok::Word;
        t.text = s;
        return t;
    }

    std::string_view q_;
    std::size_t i_ = 0;
};

enum class Occur { Default, Must, MustNot };
struct Clause { QNode node; Occur occur = Occur::Default; };

QNode none() { return QNode(); }

class Parser {
public:
    Parser(std::string_view q, Analyzer &a, const QueryOptions &o) : lex_(q), a_(a), o_(o)
    {
        lang_ = o.lang != LANG_NONE ? o.lang : a.options().default_lang;
        if (o.stopwords) stop_.reset(new Stopwords(lang_, a));
        advance();
    }

    QNode parse()
    {
        QNode n = seq();
        if (cur_.kind == Tok::RParen) syntax("unmatched )", cur_.at);
        if (cur_.kind != Tok::End) syntax("unexpected input", cur_.at);
        return n;
    }

private:
    void advance() { cur_ = lex_.next(); }

    void count_clause()
    {
        if (++clauses_ > o_.max_clauses) throw std::runtime_error("query has more than " + std::to_string(o_.max_clauses) + " clauses");
    }

    static bool starts_operand(Tok k)
    {
        return k == Tok::Word || k == Tok::Quoted || k == Tok::LParen || k == Tok::Plus || k == Tok::Minus || k == Tok::Not;
    }

    // seq := or_expr+   (implicit operator)
    QNode seq()
    {
        std::vector<Clause> cs;
        while (starts_operand(cur_.kind)) cs.push_back(or_expr());
        if (cs.empty()) {
            if (cur_.kind == Tok::End) return none();
            syntax(cur_.kind == Tok::RParen ? "empty ( )" : "operator without an operand", cur_.at);
        }
        if (cs.size() == 1 && cs[0].occur == Occur::Default) return std::move(cs[0].node);
        QNode b;
        b.kind = QKind::Bool;
        for (Clause &c : cs) add(b, std::move(c), o_.op_and ? Occur::Must : Occur::Default);
        return finish_bool(std::move(b));
    }

    // or_expr := and_expr (OR and_expr)*
    Clause or_expr()
    {
        Clause first = and_expr();
        if (cur_.kind != Tok::Or) return first;
        QNode b;
        b.kind = QKind::Bool;
        add(b, std::move(first), Occur::Default);
        while (cur_.kind == Tok::Or) {
            const std::size_t at = cur_.at;
            advance();
            if (!starts_operand(cur_.kind)) syntax("OR without a right operand", at);
            add(b, and_expr(), Occur::Default);
        }
        return {finish_bool(std::move(b)), Occur::Default};
    }

    // and_expr := unary ((AND | NOT | AND NOT) unary)*
    Clause and_expr()
    {
        Clause first = unary();
        if (cur_.kind != Tok::And && cur_.kind != Tok::Not) return first;
        QNode b;
        b.kind = QKind::Bool;
        add(b, std::move(first), Occur::Must);
        while (cur_.kind == Tok::And || cur_.kind == Tok::Not) {
            const std::size_t at = cur_.at;
            bool neg = cur_.kind == Tok::Not;
            advance();
            if (!neg && cur_.kind == Tok::Not) { neg = true; advance(); }
            if (!starts_operand(cur_.kind)) syntax(neg ? "NOT without a right operand" : "AND without a right operand", at);
            Clause c = unary();
            if (neg) c.occur = c.occur == Occur::MustNot ? Occur::Must : Occur::MustNot;
            add(b, std::move(c), Occur::Must);
        }
        return {finish_bool(std::move(b)), Occur::Default};
    }

    // unary := (+ | - | NOT) unary | near
    Clause unary()
    {
        if (cur_.kind == Tok::Plus || cur_.kind == Tok::Minus || cur_.kind == Tok::Not) {
            const Tok k = cur_.kind;
            const std::size_t at = cur_.at;
            advance();
            if (!starts_operand(cur_.kind)) syntax("+, - or NOT without an operand", at);
            Clause c = unary();
            if (k == Tok::Plus) { if (c.occur == Occur::Default) c.occur = Occur::Must; }
            else c.occur = c.occur == Occur::MustNot ? Occur::Must : Occur::MustNot;
            return c;
        }
        return {near(), Occur::Default};
    }

    // near := primary (NEAR primary)*
    QNode near()
    {
        QNode left = primary();
        while (cur_.kind == Tok::Near) {
            const Token op = cur_;
            advance();
            if (cur_.kind != Tok::Word && cur_.kind != Tok::Quoted && cur_.kind != Tok::LParen)
                syntax(std::string(op.ordered ? "pre/" : "w/") + std::to_string(op.num) + " without a right operand", op.at);
            QNode right = primary();
            if (left.kind == QKind::None || right.kind == QKind::None) { left = none(); continue; }
            if (!positional(left) || !positional(right))
                syntax("operands of w/N and pre/N must be words, phrases, wildcards or OR groups of them", op.at);
            QNode n;
            n.kind = QKind::Near;
            n.dist = op.num;
            n.ordered = op.ordered;
            n.kids.push_back(std::move(left));
            n.kids.push_back(std::move(right));
            left = std::move(n);
        }
        return left;
    }

    QNode primary()
    {
        if (cur_.kind == Tok::LParen) {
            const std::size_t at = cur_.at;
            advance();
            QNode n = seq();
            if (cur_.kind != Tok::RParen) syntax("missing )", at);
            advance();
            return n;
        }
        if (cur_.kind == Tok::Quoted) {
            Token t = cur_;
            advance();
            return phrase(t.text, t.num < 0 ? 0 : t.num, t.at);
        }
        if (cur_.kind == Tok::Word) {
            Token t = cur_;
            advance();
            return word(t);
        }
        syntax("expected a word, a phrase or (", cur_.at);
    }

    QNode word(const Token &t)
    {
        count_clause();
        if (t.wild) return pattern(t);
        a_.analyze(t.text, lang_);
        if (t.num >= 0) {
            if (a_.size() != 1) syntax("a fuzzy term must be one word: '" + t.text + "'", t.at);
            QNode n;
            n.kind = QKind::Fuzzy;
            n.words.emplace_back(a_.term(0));
            n.langs.push_back(a_.token(0).lang);
            n.edits = t.num;
            return n;
        }
        return from_tokens(0);
    }

    QNode phrase(const std::string &text, int slop, std::size_t at)
    {
        count_clause();
        (void)at;
        a_.analyze(text, lang_);
        return from_tokens(slop);
    }

    // The analyzed tokens as a term (one word) or a phrase (several: "e-mail" -> "e mail").
    QNode from_tokens(int slop)
    {
        QNode n;
        std::size_t real = 0;
        for (std::size_t i = 0; i < a_.size(); ++i) {
            const std::string_view w = a_.term(i);
            const bool stop = stop_ && stop_->contains(w);
            n.words.emplace_back(stop ? std::string() : std::string(w));
            n.langs.push_back(a_.token(i).lang);
            real += !stop;
        }
        if (real == 0) return none();
        // trim gaps at both ends
        while (n.words.front().empty()) { n.words.erase(n.words.begin()); n.langs.erase(n.langs.begin()); }
        while (n.words.back().empty()) { n.words.pop_back(); n.langs.pop_back(); }
        if (n.words.size() == 1) { n.kind = QKind::Term; return n; }
        n.kind = QKind::Phrase;
        n.slop = slop;
        return n;
    }

    QNode pattern(const Token &t)
    {
        // normalize the literal pieces between * and ?
        std::string pat, piece, norm;
        auto flush = [&] {
            if (piece.empty()) return;
            a_.normalize_piece(piece, lang_, norm);
            pat += norm;
            piece.clear();
        };
        for (char c : t.text) {
            if (c == '*' || c == '?') { flush(); pat += c; }
            else piece += c;
        }
        flush();
        if (pat.find_first_not_of("*?") == std::string::npos && pat.find('?') == std::string::npos)
            syntax("a wildcard needs at least one letter: '" + t.text + "'", t.at);
        QNode n;
        const std::size_t first = pat.find_first_of("*?");
        if (first == pat.size() - 1 && pat.back() == '*') {
            n.kind = QKind::Prefix;
            n.pattern = pat.substr(0, pat.size() - 1);
        } else {
            n.kind = QKind::Wildcard;
            n.pattern = pat;
        }
        return n;
    }

    void add(QNode &b, Clause c, Occur dflt)
    {
        if (c.node.kind == QKind::None) return;
        const Occur o = c.occur == Occur::Default ? dflt : c.occur;
        if (o == Occur::Must) b.must.push_back(std::move(c.node));
        else if (o == Occur::MustNot) b.must_not.push_back(std::move(c.node));
        else b.should.push_back(std::move(c.node));
    }

    static QNode finish_bool(QNode b)
    {
        if (b.must.empty() && b.should.empty() && b.must_not.empty()) return none();
        if (b.must_not.empty() && b.must.size() + b.should.size() == 1)
            return std::move(b.must.empty() ? b.should[0] : b.must[0]);
        return b;
    }

    Lexer lex_;
    Analyzer &a_;
    QueryOptions o_;
    Lang lang_;
    std::unique_ptr<Stopwords> stop_;
    Token cur_;
    std::size_t clauses_ = 0;
};

void append(std::string &s, const QNode &n);

void append_list(std::string &s, const std::vector<QNode> &v, const char *prefix, bool &first)
{
    for (const QNode &k : v) {
        if (!first) s += ' ';
        first = false;
        s += prefix;
        append(s, k);
    }
}

void append(std::string &s, const QNode &n)
{
    switch (n.kind) {
    case QKind::None: s += "<none>"; break;
    case QKind::Term: s += "t:" + n.words[0]; break;
    case QKind::Fuzzy: s += "f:" + n.words[0] + "~" + std::to_string(n.edits); break;
    case QKind::Prefix: s += "p:" + n.pattern + "*"; break;
    case QKind::Wildcard: s += "w:" + n.pattern; break;
    case QKind::Phrase:
        s += '"';
        for (std::size_t i = 0; i < n.words.size(); ++i) s += (i ? " " : "") + (n.words[i].empty() ? std::string("?") : n.words[i]);
        s += '"';
        if (n.slop) s += "~" + std::to_string(n.slop);
        break;
    case QKind::Near:
        s += '(';
        append(s, n.kids[0]);
        s += std::string(n.ordered ? " pre/" : " w/") + std::to_string(n.dist) + " ";
        append(s, n.kids[1]);
        s += ')';
        break;
    case QKind::Bool: {
        s += '(';
        bool first = true;
        append_list(s, n.must, "+", first);
        append_list(s, n.should, "", first);
        append_list(s, n.must_not, "-", first);
        s += ')';
        break;
    }
    }
}

} // namespace

QNode parse_query(std::string_view query, Analyzer &analyzer, const QueryOptions &opts)
{
    if (opts.plain) {
        // Every word is a clause, repeated words too (as Lucene's QueryBuilder does for a match query).
        const Lang lang = opts.lang != LANG_NONE ? opts.lang : analyzer.options().default_lang;
        std::unique_ptr<Stopwords> stop;
        if (opts.stopwords) stop.reset(new Stopwords(lang, analyzer));
        analyzer.analyze(query, lang);
        if (analyzer.size() > opts.max_clauses) throw std::runtime_error("query has more than " + std::to_string(opts.max_clauses) + " words");
        QNode b;
        b.kind = QKind::Bool;
        for (std::size_t i = 0; i < analyzer.size(); ++i) {
            if (stop && stop->contains(analyzer.term(i))) continue;
            QNode t;
            t.kind = QKind::Term;
            t.words.emplace_back(analyzer.term(i));
            t.langs.push_back(analyzer.token(i).lang);
            (opts.op_and ? b.must : b.should).push_back(std::move(t));
        }
        if (b.must.empty() && b.should.empty()) return QNode();
        if (b.must.size() + b.should.size() == 1) return std::move(b.must.empty() ? b.should[0] : b.must[0]);
        return b;
    }
    Parser p(query, analyzer, opts);
    return p.parse();
}

std::string to_string(const QNode &n)
{
    std::string s;
    append(s, n);
    return s;
}

bool positional(const QNode &n)
{
    switch (n.kind) {
    case QKind::Term: case QKind::Phrase: case QKind::Prefix: case QKind::Wildcard: case QKind::Fuzzy: case QKind::Near:
        return true;
    case QKind::Bool:
        if (!n.must.empty() || !n.must_not.empty() || n.should.empty()) return false;
        for (const QNode &k : n.should) if (!positional(k)) return false;
        return true;
    default:
        return false;
    }
}

std::size_t count_leaves(const QNode &n)
{
    switch (n.kind) {
    case QKind::None: return 0;
    case QKind::Bool: {
        std::size_t c = 0;
        for (auto *v : {&n.must, &n.should, &n.must_not}) for (const QNode &k : *v) c += count_leaves(k);
        return c;
    }
    case QKind::Near: return count_leaves(n.kids[0]) + count_leaves(n.kids[1]);
    default: return 1;
    }
}

} // namespace vtext
