#include "attribute.h"
#include "cache.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <list>
#include <stdexcept>
#include <unordered_map>

namespace vfts {

namespace {

constexpr char ATTR_MAGIC[8] = {'V', 'F', 'T', 'S', 'A', 'T', 'R', '1'};

struct AttrHeader {
    char magic[8];
    std::uint32_t format;           // 1
    std::uint32_t type;             // AttrType
    std::int64_t version;
    std::uint64_t n_rows;
    std::uint32_t n_codes;
    std::uint32_t reserved0;
    std::uint64_t pool_bytes;       // text: bytes of the values
    std::uint64_t checksum;         // words after the header (word_sum)
    std::uint64_t total_bytes;
};
static_assert(sizeof(AttrHeader) == 64, "AttrHeader size");

// A checksum over 8-byte words (the tail zero-padded): fast enough for 100 MB at every open.
std::uint64_t word_sum(const char *p, std::size_t n)
{
    std::uint64_t h = 0x9E3779B97F4A7C15ull, i = 0;
    std::size_t at = 0;
    for (; at + 8 <= n; at += 8, ++i) {
        std::uint64_t w;
        std::memcpy(&w, p + at, 8);
        h ^= (w + i) * 0xBF58476D1CE4E5B9ull;
        h = (h << 27) | (h >> 37);
    }
    if (at < n) {
        std::uint64_t w = 0;
        std::memcpy(&w, p + at, n - at);
        h ^= (w + i) * 0xBF58476D1CE4E5B9ull;
        h = (h << 27) | (h >> 37);
    }
    return h ^ n;
}

void pad8(std::string &s) { while (s.size() % 8) s.push_back('\0'); }

template <class T> void put(std::string &s, const T &v) { s.append(reinterpret_cast<const char *>(&v), sizeof(T)); }

std::string finish_file(AttrType type, std::int64_t version, const std::vector<std::int64_t> &ids, const std::vector<std::uint32_t> &codes,
                        std::uint32_t n_codes, const std::string &dict, std::uint64_t pool_bytes)
{
    std::string body;
    body.reserve(ids.size() * 12 + dict.size() + 16);
    body.append(reinterpret_cast<const char *>(ids.data()), ids.size() * 8);
    body.append(reinterpret_cast<const char *>(codes.data()), codes.size() * 4);
    pad8(body);
    body += dict;
    pad8(body);
    AttrHeader h{};
    std::memcpy(h.magic, ATTR_MAGIC, 8);
    h.format = 1;
    h.type = static_cast<std::uint32_t>(type);
    h.version = version;
    h.n_rows = ids.size();
    h.n_codes = n_codes;
    h.pool_bytes = pool_bytes;
    h.checksum = word_sum(body.data(), body.size());
    h.total_bytes = sizeof(AttrHeader) + body.size();
    std::string out;
    out.reserve(h.total_bytes);
    put(out, h);
    out += body;
    return out;
}

void check_unique(const std::vector<std::int64_t> &ids)
{
    for (std::size_t i = 1; i < ids.size(); ++i)
        if (ids[i] == ids[i - 1]) throw std::runtime_error("doc_id " + std::to_string(ids[i]) + " has two values (the key column must be unique)");
}

// days since 1970-01-01 of a civil date (H. Hinnant's algorithm)
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civil_from_days(std::int64_t z, std::int64_t &y, unsigned &m, unsigned &d)
{
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}

const std::int64_t J2000 = days_from_civil(2000, 1, 1);

struct FileMap {
    void *p = MAP_FAILED;
    std::size_t n = 0;
    ~FileMap() { if (p != MAP_FAILED && n) ::munmap(p, n); }
};

} // namespace

const char *attr_type_name(AttrType t)
{
    switch (t) {
    case AttrType::Int: return "int";
    case AttrType::Bool: return "bool";
    case AttrType::Date: return "date";
    case AttrType::Text: return "text";
    }
    return "?";
}

bool parse_attr_type(const std::string &s, AttrType &t)
{
    if (s == "int") t = AttrType::Int;
    else if (s == "bool") t = AttrType::Bool;
    else if (s == "date") t = AttrType::Date;
    else if (s == "text") t = AttrType::Text;
    else return false;
    return true;
}

std::string attr_file_name(const std::string &name) { return "attr_" + name + ".vfa"; }

bool parse_date(std::string_view s, std::int64_t &days)
{
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (int i : {0, 1, 2, 3, 5, 6, 8, 9}) if (s[static_cast<std::size_t>(i)] < '0' || s[static_cast<std::size_t>(i)] > '9') return false;
    const int y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    const unsigned m = static_cast<unsigned>((s[5] - '0') * 10 + (s[6] - '0')), d = static_cast<unsigned>((s[8] - '0') * 10 + (s[9] - '0'));
    if (m < 1 || m > 12 || d < 1) return false;
    static const unsigned mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (d > mdays[m - 1] + (m == 2 && leap ? 1 : 0)) return false;
    days = days_from_civil(y, m, d) - J2000;
    return true;
}

std::string format_date(std::int64_t days)
{
    std::int64_t y;
    unsigned m, d;
    civil_from_days(days + J2000, y, m, d);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04lld-%02u-%02u", static_cast<long long>(y), m, d);
    return buf;
}

std::string encode_attribute(AttrType type, std::int64_t version, std::vector<std::pair<std::int64_t, std::int64_t>> rows)
{
    if (type == AttrType::Text) throw std::runtime_error("encode_attribute: text values go to encode_attribute_text");
    std::sort(rows.begin(), rows.end());
    std::vector<std::int64_t> ids(rows.size()), dict;
    dict.reserve(1024);
    for (std::size_t i = 0; i < rows.size(); ++i) { ids[i] = rows[i].first; dict.push_back(rows[i].second); }
    check_unique(ids);
    std::sort(dict.begin(), dict.end());
    dict.erase(std::unique(dict.begin(), dict.end()), dict.end());
    std::vector<std::uint32_t> codes(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i)
        codes[i] = static_cast<std::uint32_t>(std::lower_bound(dict.begin(), dict.end(), rows[i].second) - dict.begin());
    std::string d(reinterpret_cast<const char *>(dict.data()), dict.size() * 8);
    return finish_file(type, version, ids, codes, static_cast<std::uint32_t>(dict.size()), d, 0);
}

std::string encode_attribute_text(std::int64_t version, std::vector<std::pair<std::int64_t, std::string>> rows)
{
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<std::int64_t> ids(rows.size());
    std::vector<std::string_view> dict;
    for (std::size_t i = 0; i < rows.size(); ++i) { ids[i] = rows[i].first; dict.push_back(rows[i].second); }
    check_unique(ids);
    std::sort(dict.begin(), dict.end());
    dict.erase(std::unique(dict.begin(), dict.end()), dict.end());
    if (dict.size() >= ATTR_NONE) throw std::runtime_error("too many distinct values");
    std::vector<std::uint32_t> codes(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i)
        codes[i] = static_cast<std::uint32_t>(std::lower_bound(dict.begin(), dict.end(), std::string_view(rows[i].second)) - dict.begin());
    std::string d;
    std::uint64_t off = 0;
    for (const auto &v : dict) { put(d, off); off += v.size(); }
    put(d, off);
    for (const auto &v : dict) d.append(v.data(), v.size());
    return finish_file(AttrType::Text, version, ids, codes, static_cast<std::uint32_t>(dict.size()), d, off);
}

Attribute::Attribute(const char *p, std::size_t n, std::shared_ptr<const void> keep) : keep_(std::move(keep))
{
    static std::atomic<std::uint64_t> serials{0};
    serial_ = ++serials;
    if (n < sizeof(AttrHeader)) throw std::runtime_error("attribute file too short");
    AttrHeader h;
    std::memcpy(&h, p, sizeof h);
    if (std::memcmp(h.magic, ATTR_MAGIC, 8) != 0 || h.format != 1) throw std::runtime_error("not an attribute file of this vfts version");
    if (h.total_bytes != n) throw std::runtime_error("attribute file has the wrong size");
    if (h.type > 3) throw std::runtime_error("attribute file: unknown type");
    if (word_sum(p + sizeof h, n - sizeof h) != h.checksum) throw std::runtime_error("attribute file is damaged (checksum)");
    type_ = static_cast<AttrType>(h.type);
    version_ = h.version;
    n_rows_ = h.n_rows;
    n_codes_ = h.n_codes;
    std::size_t at = sizeof h;
    auto need = [&](std::size_t bytes) { if (at + bytes > n) throw std::runtime_error("attribute file: sections out of bounds"); };
    need(n_rows_ * 12);
    ids_ = reinterpret_cast<const std::int64_t *>(p + at);
    at += n_rows_ * 8;
    codes_ = reinterpret_cast<const std::uint32_t *>(p + at);
    at += n_rows_ * 4;
    at = (at + 7) / 8 * 8;
    if (type_ == AttrType::Text) {
        need((static_cast<std::size_t>(n_codes_) + 1) * 8);
        offs_ = reinterpret_cast<const std::uint64_t *>(p + at);
        at += (static_cast<std::size_t>(n_codes_) + 1) * 8;
        need(h.pool_bytes);
        if (offs_[n_codes_] != h.pool_bytes) throw std::runtime_error("attribute file: text offsets");
        pool_ = p + at;
    } else {
        need(static_cast<std::size_t>(n_codes_) * 8);
        ints_ = reinterpret_cast<const std::int64_t *>(p + at);
    }
    for (std::uint64_t i = 0; i < n_rows_; ++i)
        if (codes_[i] >= n_codes_) throw std::runtime_error("attribute file: code out of range");
    if (n_rows_ > 0) {
        const std::int64_t lo = ids_[0], hi = ids_[n_rows_ - 1];
        if (hi >= lo && static_cast<std::uint64_t>(hi - lo) < 2 * n_rows_ + 4096) {
            dense_lo_ = lo;
            dense_.assign(static_cast<std::size_t>(hi - lo) + 1, ATTR_NONE);
            for (std::uint64_t i = 0; i < n_rows_; ++i) dense_[static_cast<std::size_t>(ids_[i] - lo)] = codes_[i];
        }
    }
}

std::uint32_t Attribute::code_of(std::int64_t doc_id) const
{
    if (!dense_.empty()) {
        if (doc_id < dense_lo_ || doc_id - dense_lo_ >= static_cast<std::int64_t>(dense_.size())) return ATTR_NONE;
        return dense_[static_cast<std::size_t>(doc_id - dense_lo_)];
    }
    const std::int64_t *e = ids_ + n_rows_;
    const std::int64_t *f = std::lower_bound(ids_, e, doc_id);
    return f != e && *f == doc_id ? codes_[f - ids_] : ATTR_NONE;
}

std::string Attribute::value_string(std::uint32_t code) const
{
    switch (type_) {
    case AttrType::Int: return std::to_string(ints_[code]);
    case AttrType::Bool: return ints_[code] ? "t" : "f";
    case AttrType::Date: return format_date(ints_[code]);
    case AttrType::Text: return std::string(text_value(code));
    }
    return "";
}

std::uint32_t Attribute::lower_int(std::int64_t v) const
{
    return static_cast<std::uint32_t>(std::lower_bound(ints_, ints_ + n_codes_, v) - ints_);
}

std::uint32_t Attribute::lower_text(std::string_view v) const
{
    std::uint32_t lo = 0, hi = n_codes_;
    while (lo < hi) {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        if (text_value(mid) < v) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

std::shared_ptr<const std::vector<std::uint32_t>> Attribute::codes_for(const Segment &seg) const
{
    const bool keepit = seg.id() >= 0;
    const std::pair<std::int64_t, std::uint64_t> key{seg.id(), seg.header().checksum};
    if (keepit) {
        std::lock_guard<std::mutex> l(mu_);
        auto it = per_seg_.find(key);
        if (it != per_seg_.end()) return it->second;
    }
    auto v = std::make_shared<std::vector<std::uint32_t>>(seg.n_docs());
    for (std::uint32_t d = 0; d < seg.n_docs(); ++d) (*v)[d] = code_of(seg.doc_id(d));
    if (keepit) {
        std::lock_guard<std::mutex> l(mu_);
        if (per_seg_.size() >= 64) per_seg_.clear();        // segments merged away over time
        per_seg_[key] = v;
    }
    return v;
}

// ---------------------------------------------------------------- the files in the cache

namespace {

struct AttrRegistry {
    std::mutex mu;
    struct Entry {
        std::shared_ptr<const Attribute> a;
        dev_t dev = 0;
        ino_t ino = 0;
        off_t size = 0;
        std::chrono::steady_clock::time_point checked;
    };
    std::map<std::string, Entry> files;     // key: path
};

AttrRegistry &attr_registry()
{
    static AttrRegistry *r = new AttrRegistry();
    return *r;
}

} // namespace

std::shared_ptr<const Attribute> open_attribute(const std::string &cache_dir, const std::string &index, const std::string &name)
{
    if (!valid_index_name(index)) throw std::runtime_error("invalid index name");
    if (!valid_index_name(name)) throw std::runtime_error("invalid attribute name '" + name + "'");
    const std::string path = cache_dir + "/" + index + "/" + attr_file_name(name);
    AttrRegistry &r = attr_registry();
    std::lock_guard<std::mutex> lock(r.mu);
    const auto now = std::chrono::steady_clock::now();
    auto it = r.files.find(path);
    if (it != r.files.end() && now - it->second.checked < std::chrono::milliseconds(ACTIVE_CHECK_MS)) return it->second.a;
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        if (it != r.files.end()) r.files.erase(it);
        throw std::runtime_error("index " + index + " has no attribute " + name + " on this node: CALL vfts.add_attribute (or vfts.load_all('" + index + "'))");
    }
    if (it != r.files.end() && it->second.dev == st.st_dev && it->second.ino == st.st_ino && it->second.size == st.st_size) {
        it->second.checked = now;
        return it->second.a;
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("cannot open " + path + ": " + std::strerror(errno));
    auto m = std::make_shared<FileMap>();
    m->n = static_cast<std::size_t>(st.st_size);
    m->p = m->n ? ::mmap(nullptr, m->n, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
    ::close(fd);
    if (m->p == MAP_FAILED) throw std::runtime_error("cannot map " + path);
    std::shared_ptr<const Attribute> a;
    try {
        a = std::make_shared<Attribute>(static_cast<const char *>(m->p), m->n, m);
    } catch (const std::exception &e) {
        throw std::runtime_error("attribute " + name + " of index " + index + ": " + e.what() + ": CALL vfts.refresh_attributes('" + index + "')");
    }
    r.files[path] = {a, st.st_dev, st.st_ino, st.st_size, now};
    return a;
}

void forget_attributes(const std::string &cache_dir, const std::string &index)
{
    const std::string pre = cache_dir + "/" + index + "/";
    AttrRegistry &r = attr_registry();
    std::lock_guard<std::mutex> lock(r.mu);
    for (auto it = r.files.begin(); it != r.files.end();) it = it->first.rfind(pre, 0) == 0 ? r.files.erase(it) : std::next(it);
}

std::vector<std::string> attribute_names(const std::string &cache_dir, const std::string &index)
{
    std::vector<std::string> out;
    if (!valid_index_name(index)) return out;
    DIR *d = ::opendir((cache_dir + "/" + index).c_str());
    if (!d) return out;
    while (dirent *e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() > 9 && n.rfind("attr_", 0) == 0 && n.compare(n.size() - 4, 4, ".vfa") == 0) out.push_back(n.substr(5, n.size() - 9));
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------- filters

struct Filter::Node {
    enum Kind { Leaf, And, Or, Not } kind = Leaf;
    int attr = -1;
    bool null_test = false, want_null = false;     // IS [NOT] NULL
    std::vector<std::uint64_t> allowed;            // leaf: a bit per code
    std::vector<std::unique_ptr<Node>> kids;
};

Filter::~Filter() = default;

namespace {

enum class Tok { End, Name, Number, String, LParen, RParen, Comma, Op };

struct Lexer {
    const std::string &s;
    std::size_t at = 0;
    Tok tok = Tok::End;
    std::string text;           // name (upper case copy in upper), number, string value, operator
    std::string upper;
    std::size_t start = 0;

    explicit Lexer(const std::string &src) : s(src) { next(); }

    [[noreturn]] void error(const std::string &why) const
    {
        throw std::runtime_error("filter: " + why + " at position " + std::to_string(start + 1) + " of '" + s + "'");
    }

    void next()
    {
        while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\n' || s[at] == '\r')) ++at;
        start = at;
        text.clear();
        upper.clear();
        if (at >= s.size()) { tok = Tok::End; return; }
        const char c = s[at];
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            while (at < s.size() && (std::isalnum(static_cast<unsigned char>(s[at])) || s[at] == '_')) text += s[at++];
            for (char x : text) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(x)));
            tok = Tok::Name;
        } else if (std::isdigit(static_cast<unsigned char>(c)) || (c == '-' && at + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[at + 1])))) {
            text += s[at++];
            while (at < s.size() && std::isdigit(static_cast<unsigned char>(s[at]))) text += s[at++];
            tok = Tok::Number;
        } else if (c == '\'') {
            ++at;
            for (;;) {
                if (at >= s.size()) error("unterminated string");
                if (s[at] == '\'') {
                    if (at + 1 < s.size() && s[at + 1] == '\'') { text += '\''; at += 2; continue; }
                    ++at;
                    break;
                }
                text += s[at++];
            }
            tok = Tok::String;
        } else if (c == '(') { ++at; tok = Tok::LParen; }
        else if (c == ')') { ++at; tok = Tok::RParen; }
        else if (c == ',') { ++at; tok = Tok::Comma; }
        else if (c == '=' ) { ++at; text = "="; tok = Tok::Op; }
        else if (c == '!' && at + 1 < s.size() && s[at + 1] == '=') { at += 2; text = "!="; tok = Tok::Op; }
        else if (c == '<') {
            ++at;
            if (at < s.size() && s[at] == '=') { ++at; text = "<="; }
            else if (at < s.size() && s[at] == '>') { ++at; text = "!="; }
            else text = "<";
            tok = Tok::Op;
        } else if (c == '>') {
            ++at;
            if (at < s.size() && s[at] == '=') { ++at; text = ">="; } else text = ">";
            tok = Tok::Op;
        } else error(std::string("unexpected character '") + c + "'");
    }
    bool keyword(const char *k) const { return tok == Tok::Name && upper == k; }
};

struct Value {
    bool is_text = false;
    std::int64_t i = 0;
    std::string t;
};

class Parser {
public:
    Parser(Lexer &lx, const Filter::Opener &open, std::vector<std::shared_ptr<const Attribute>> &attrs, std::vector<std::string> &names)
        : lx_(lx), open_(open), attrs_(attrs), names_(names) {}

    std::unique_ptr<Filter::Node> expr()
    {
        auto left = conj();
        while (lx_.keyword("OR")) {
            lx_.next();
            auto n = std::make_unique<Filter::Node>();
            n->kind = Filter::Node::Or;
            n->kids.push_back(std::move(left));
            n->kids.push_back(conj());
            left = std::move(n);
        }
        return left;
    }

private:
    Lexer &lx_;
    const Filter::Opener &open_;
    std::vector<std::shared_ptr<const Attribute>> &attrs_;
    std::vector<std::string> &names_;

    std::unique_ptr<Filter::Node> conj()
    {
        auto left = neg();
        while (lx_.keyword("AND")) {
            lx_.next();
            auto n = std::make_unique<Filter::Node>();
            n->kind = Filter::Node::And;
            n->kids.push_back(std::move(left));
            n->kids.push_back(neg());
            left = std::move(n);
        }
        return left;
    }

    std::unique_ptr<Filter::Node> neg()
    {
        if (lx_.keyword("NOT")) {
            lx_.next();
            auto n = std::make_unique<Filter::Node>();
            n->kind = Filter::Node::Not;
            n->kids.push_back(neg());
            return n;
        }
        if (lx_.tok == Tok::LParen) {
            lx_.next();
            auto e = expr();
            if (lx_.tok != Tok::RParen) lx_.error("')' expected");
            lx_.next();
            return e;
        }
        return predicate();
    }

    int attribute(const std::string &name)
    {
        for (std::size_t i = 0; i < names_.size(); ++i) if (names_[i] == name) return static_cast<int>(i);
        if (!valid_index_name(name)) lx_.error("invalid attribute name '" + name + "'");
        attrs_.push_back(open_(name));
        names_.push_back(name);
        return static_cast<int>(names_.size() - 1);
    }

    Value literal(const Attribute &a, const std::string &name)
    {
        Value v;
        const AttrType t = a.type();
        if (lx_.keyword("DATE")) {
            lx_.next();
            if (lx_.tok != Tok::String) lx_.error("DATE 'YYYY-MM-DD' expected");
            if (t != AttrType::Date) lx_.error("attribute " + name + " is not a date");
        }
        if (t == AttrType::Text) {
            if (lx_.tok != Tok::String) lx_.error("attribute " + name + " is text: a value in quotes expected");
            v.is_text = true;
            v.t = lx_.text;
        } else if (t == AttrType::Date) {
            if (lx_.tok != Tok::String || !parse_date(lx_.text, v.i)) lx_.error("attribute " + name + " is a date: 'YYYY-MM-DD' expected");
        } else if (t == AttrType::Bool) {
            const std::string u = lx_.upper.empty() ? lx_.text : lx_.upper;
            if ((lx_.tok == Tok::Name && u == "TRUE") || (lx_.tok == Tok::String && (lx_.text == "t" || lx_.text == "true")) ||
                (lx_.tok == Tok::Number && lx_.text == "1")) v.i = 1;
            else if ((lx_.tok == Tok::Name && u == "FALSE") || (lx_.tok == Tok::String && (lx_.text == "f" || lx_.text == "false")) ||
                     (lx_.tok == Tok::Number && lx_.text == "0")) v.i = 0;
            else lx_.error("attribute " + name + " is a boolean: TRUE or FALSE expected");
        } else {
            if (lx_.tok != Tok::Number) lx_.error("attribute " + name + " is an integer: a number expected");
            try { v.i = std::stoll(lx_.text); } catch (...) { lx_.error("number out of range"); }
        }
        lx_.next();
        return v;
    }

    // first code >= v (lower) or > v (upper)
    static std::uint32_t bound(const Attribute &a, const Value &v, bool upper)
    {
        if (a.type() == AttrType::Text) {
            std::uint32_t c = a.lower_text(v.t);
            if (upper && c < a.n_codes() && a.text_value(c) == v.t) ++c;
            return c;
        }
        std::uint32_t c = a.lower_int(v.i);
        if (upper && c < a.n_codes() && a.int_value(c) == v.i) ++c;
        return c;
    }

    static void set_range(std::vector<std::uint64_t> &bits, std::uint32_t lo, std::uint32_t hi)
    {
        for (std::uint32_t c = lo; c < hi; ++c) bits[c >> 6] |= 1ull << (c & 63);
    }

    std::unique_ptr<Filter::Node> predicate()
    {
        if (lx_.tok != Tok::Name) lx_.error("attribute name expected");
        const std::string name = lx_.text;
        const int ai = attribute(name);
        const Attribute &a = *attrs_[static_cast<std::size_t>(ai)];
        lx_.next();
        auto n = std::make_unique<Filter::Node>();
        n->attr = ai;
        n->allowed.assign((static_cast<std::size_t>(a.n_codes()) + 63) / 64, 0);
        const std::uint32_t N = a.n_codes();
        if (lx_.keyword("IS")) {
            lx_.next();
            bool neg = false;
            if (lx_.keyword("NOT")) { neg = true; lx_.next(); }
            if (!lx_.keyword("NULL")) lx_.error("NULL expected");
            lx_.next();
            n->null_test = true;
            n->want_null = !neg;
            return n;
        }
        bool negate = false;
        if (lx_.keyword("NOT")) { negate = true; lx_.next(); if (!lx_.keyword("IN") && !lx_.keyword("BETWEEN")) lx_.error("IN or BETWEEN expected after NOT"); }
        if (lx_.keyword("IN")) {
            lx_.next();
            if (lx_.tok != Tok::LParen) lx_.error("'(' expected after IN");
            lx_.next();
            for (;;) {
                const Value v = literal(a, name);
                const std::uint32_t lo = bound(a, v, false), hi = bound(a, v, true);
                set_range(n->allowed, lo, hi);
                if (lx_.tok == Tok::Comma) { lx_.next(); continue; }
                if (lx_.tok != Tok::RParen) lx_.error("',' or ')' expected");
                lx_.next();
                break;
            }
        } else if (lx_.keyword("BETWEEN")) {
            lx_.next();
            const Value lo = literal(a, name);
            if (!lx_.keyword("AND")) lx_.error("AND expected in BETWEEN");
            lx_.next();
            const Value hi = literal(a, name);
            set_range(n->allowed, bound(a, lo, false), bound(a, hi, true));
        } else if (lx_.tok == Tok::Op) {
            const std::string op = lx_.text;
            lx_.next();
            const Value v = literal(a, name);
            const std::uint32_t lo = bound(a, v, false), hi = bound(a, v, true);
            if (op == "=") set_range(n->allowed, lo, hi);
            else if (op == "!=") { set_range(n->allowed, 0, lo); set_range(n->allowed, hi, N); }
            else if (op == "<") set_range(n->allowed, 0, lo);
            else if (op == "<=") set_range(n->allowed, 0, hi);
            else if (op == ">") set_range(n->allowed, hi, N);
            else if (op == ">=") set_range(n->allowed, lo, N);
        } else {
            lx_.error("comparison, IN, BETWEEN or IS expected after " + name);
        }
        if (negate) {
            for (auto &w : n->allowed) w = ~w;
            if (!n->allowed.empty() && N % 64) n->allowed.back() &= (1ull << (N % 64)) - 1;
        }
        return n;
    }
};

// three-valued logic: 0 false, 1 true, 2 unknown
inline int eval(const Filter::Node &n, const std::vector<const std::uint32_t *> &codes, std::uint32_t d)
{
    switch (n.kind) {
    case Filter::Node::Leaf: {
        const std::uint32_t c = codes[static_cast<std::size_t>(n.attr)][d];
        if (n.null_test) return (c == ATTR_NONE) == n.want_null ? 1 : 0;
        if (c == ATTR_NONE) return 2;
        return n.allowed[c >> 6] >> (c & 63) & 1 ? 1 : 0;
    }
    case Filter::Node::Not: {
        const int v = eval(*n.kids[0], codes, d);
        return v == 2 ? 2 : 1 - v;
    }
    case Filter::Node::And: {
        const int a = eval(*n.kids[0], codes, d);
        if (a == 0) return 0;
        const int b = eval(*n.kids[1], codes, d);
        if (b == 0) return 0;
        return a == 1 && b == 1 ? 1 : 2;
    }
    case Filter::Node::Or: {
        const int a = eval(*n.kids[0], codes, d);
        if (a == 1) return 1;
        const int b = eval(*n.kids[1], codes, d);
        if (b == 1) return 1;
        return a == 0 && b == 0 ? 0 : 2;
    }
    }
    return 2;
}

// ---- the filter cache: (filter, segment, attribute files) -> left-out documents
struct FilterCache {
    std::mutex mu;
    std::list<std::pair<std::string, std::shared_ptr<const DocSet>>> lru;     // most recent first
    std::unordered_map<std::string, decltype(lru)::iterator> map;
    std::size_t bytes = 0;
    static constexpr std::size_t CAP = 512ull << 20;

    std::shared_ptr<const DocSet> get(const std::string &k)
    {
        std::lock_guard<std::mutex> l(mu);
        auto it = map.find(k);
        if (it == map.end()) return nullptr;
        lru.splice(lru.begin(), lru, it->second);
        return it->second->second;
    }
    void put(const std::string &k, std::shared_ptr<const DocSet> v)
    {
        std::lock_guard<std::mutex> l(mu);
        if (map.count(k)) return;
        const std::size_t b = v ? v->bits.size() * 8 + 64 : 64;
        lru.emplace_front(k, std::move(v));
        map[k] = lru.begin();
        bytes += b;
        while (bytes > CAP && lru.size() > 1) {
            auto &last = lru.back();
            bytes -= last.second ? last.second->bits.size() * 8 + 64 : 64;
            map.erase(last.first);
            lru.pop_back();
        }
    }
};

FilterCache &filter_cache()
{
    static FilterCache *c = new FilterCache();
    return *c;
}

} // namespace

std::shared_ptr<const Filter> Filter::parse(const std::string &text, const Opener &open)
{
    auto f = std::shared_ptr<Filter>(new Filter());
    f->text_ = text;
    Lexer lx(text);
    if (lx.tok == Tok::End) lx.error("empty filter");
    Parser p(lx, open, f->attrs_, f->attr_names_);
    f->root_ = p.expr();
    if (lx.tok != Tok::End) lx.error("unexpected '" + (lx.text.empty() ? std::string(1, text[lx.start]) : lx.text) + "'");
    // the cache key: the text and the identity of every attribute file it reads
    f->key_ = text;
    for (std::size_t i = 0; i < f->attrs_.size(); ++i)
        f->key_ += "\x1f" + f->attr_names_[i] + "@" + std::to_string(f->attrs_[i]->version()) + "#" +
                   std::to_string(f->attrs_[i]->serial());
    return f;
}

std::shared_ptr<const DocSet> Filter::compute(const Segment &seg) const
{
    std::vector<std::shared_ptr<const std::vector<std::uint32_t>>> keep;
    std::vector<const std::uint32_t *> codes;
    for (const auto &a : attrs_) { keep.push_back(a->codes_for(seg)); codes.push_back(keep.back()->data()); }
    const std::uint32_t n = seg.n_docs();
    auto out = std::make_shared<DocSet>();
    out->resize(n);
    const Node &r = *root_;
    if (r.kind == Node::Leaf && !r.null_test) {
        // one comparison: a code lookup per document
        const std::uint32_t *c = codes[static_cast<std::size_t>(r.attr)];
        const std::uint64_t *al = r.allowed.data();
        for (std::uint32_t d = 0; d < n; ++d) {
            const std::uint32_t x = c[d];
            if (x == ATTR_NONE || !(al[x >> 6] >> (x & 63) & 1)) { out->bits[d >> 6] |= 1ull << (d & 63); ++out->count; }
        }
    } else {
        for (std::uint32_t d = 0; d < n; ++d)
            if (eval(r, codes, d) != 1) { out->bits[d >> 6] |= 1ull << (d & 63); ++out->count; }
    }
    if (out->count == 0) return nullptr;
    return out;
}

std::shared_ptr<const DocSet> Filter::excluded(const Segment &seg, bool cache) const
{
    if (!cache || seg.id() < 0) return compute(seg);
    const std::string k = key_ + "\x1e" + std::to_string(seg.id()) + ":" + std::to_string(seg.header().checksum);
    FilterCache &fc = filter_cache();
    {
        std::lock_guard<std::mutex> l(fc.mu);
        auto it = fc.map.find(k);
        if (it != fc.map.end()) { fc.lru.splice(fc.lru.begin(), fc.lru, it->second); return it->second->second; }
    }
    auto v = compute(seg);
    fc.put(k, v);
    return v;
}

namespace {

// The filter asked per candidate: the codes of the documents of each segment, the tree evaluated
// for one docno at a time (one code lookup and one bit for a single comparison).
class FilterView : public DocFilter {
public:
    const Filter::Node *root = nullptr;
    std::vector<std::vector<const std::uint32_t *>> codes;     // [segment][attribute]
    std::vector<std::shared_ptr<const std::vector<std::uint32_t>>> keepalive;
    bool keep(std::size_t seg, std::uint32_t d) const override
    {
        const Filter::Node &n = *root;
        if (n.kind == Filter::Node::Leaf && !n.null_test) {
            const std::uint32_t c = codes[seg][static_cast<std::size_t>(n.attr)][d];
            return c != ATTR_NONE && (n.allowed[c >> 6] >> (c & 63) & 1);
        }
        return eval(n, codes[seg], d) == 1;
    }
};

} // namespace

std::unique_ptr<DocFilter> Filter::view(const std::vector<const Segment *> &segs) const
{
    auto v = std::make_unique<FilterView>();
    v->root = root_.get();
    for (const Segment *g : segs) {
        std::vector<const std::uint32_t *> row;
        for (const auto &a : attrs_) { v->keepalive.push_back(a->codes_for(*g)); row.push_back(v->keepalive.back()->data()); }
        v->codes.push_back(std::move(row));
    }
    return v;
}

std::vector<std::shared_ptr<const DocSet>> filter_segments(const Filter &f, const std::vector<const Segment *> &segs, bool cache)
{
    std::vector<std::shared_ptr<const DocSet>> out(segs.size());
    for (std::size_t i = 0; i < segs.size(); ++i) out[i] = f.excluded(*segs[i], cache);
    return out;
}

} // namespace vfts
