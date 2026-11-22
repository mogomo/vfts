#include "expand.h"

#include <algorithm>
#include <cstdlib>

namespace vfts {

void to_code_points(std::string_view s, std::vector<std::uint32_t> &out)
{
    out.clear();
    const unsigned char *p = reinterpret_cast<const unsigned char *>(s.data());
    const unsigned char *e = p + s.size();
    while (p < e) {
        std::uint32_t c = *p;
        int n = 0;
        if (c < 0x80) n = 0;
        else if ((c >> 5) == 6) { c &= 0x1F; n = 1; }
        else if ((c >> 4) == 14) { c &= 0x0F; n = 2; }
        else if ((c >> 3) == 30) { c &= 0x07; n = 3; }
        else { out.push_back(0xFFFD); ++p; continue; }
        ++p;
        bool ok = true;
        for (int k = 0; k < n; ++k) {
            if (p >= e || (*p >> 6) != 2) { ok = false; break; }
            c = (c << 6) | (*p++ & 0x3F);
        }
        out.push_back(ok ? c : 0xFFFD);
    }
}

bool wildcard_match(const std::vector<std::uint32_t> &pat, const std::vector<std::uint32_t> &txt)
{
    std::size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < txt.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == txt[t])) { ++p; ++t; }
        else if (p < pat.size() && pat[p] == '*') { star = p++; mark = t; }
        else if (star != std::string::npos) { p = star + 1; t = ++mark; }
        else return false;
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

int bounded_osa_distance(const std::vector<std::uint32_t> &a, const std::vector<std::uint32_t> &b, int max_edits)
{
    const int n = static_cast<int>(a.size()), m = static_cast<int>(b.size());
    if (std::abs(n - m) > max_edits) return max_edits + 1;
    // three rows of the DP table
    std::vector<int> prev2(m + 1), prev(m + 1), cur(m + 1);
    for (int j = 0; j <= m; ++j) prev[j] = j;
    for (int i = 1; i <= n; ++i) {
        cur[0] = i;
        int row_min = cur[0];
        for (int j = 1; j <= m; ++j) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            int v = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) v = std::min(v, prev2[j - 2] + 1);
            cur[j] = v;
            row_min = std::min(row_min, v);
        }
        if (row_min > max_edits) return max_edits + 1;
        prev2.swap(prev);
        prev.swap(cur);
    }
    return std::min(prev[m], max_edits + 1);
}

namespace {

// One code point at s[i] (valid UTF-8; an invalid byte is U+FFFD and one byte long). Sets len.
inline std::uint32_t decode_one(std::string_view s, std::size_t i, std::size_t &len)
{
    const unsigned char c = static_cast<unsigned char>(s[i]);
    auto cont = [&](std::size_t k) { return i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80; };
    if (c < 0x80) { len = 1; return c; }
    if (c >= 0xC2 && c < 0xE0 && cont(1)) { len = 2; return ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu); }
    if (c >= 0xE0 && c < 0xF0 && cont(1) && cont(2)) {
        len = 3;
        return ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
    }
    if (c >= 0xF0 && c < 0xF5 && cont(1) && cont(2) && cont(3)) {
        len = 4;
        return ((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
               ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
    }
    len = 1;
    return 0xFFFD;
}

void append_utf8(std::string &out, std::uint32_t c)
{
    if (c < 0x80) out += static_cast<char>(c);
    else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// The successor of every string that starts with p (byte order); empty when there is none.
std::string successor(std::string p)
{
    while (!p.empty() && static_cast<unsigned char>(p.back()) == 0xFF) p.pop_back();
    if (!p.empty()) p.back() = static_cast<char>(static_cast<unsigned char>(p.back()) + 1);
    return p;
}

} // namespace

void fuzzy_terms(std::uint32_t n, const std::function<std::string_view(std::uint32_t)> &term,
                 const std::function<std::uint32_t(std::string_view)> &lower_bound,
                 const std::vector<std::uint32_t> &word, int max_edits, const std::string &prefix,
                 const std::function<void(std::uint32_t, std::string_view, int)> &hit)
{
    const std::size_t W = word.size() + 1;
    // The current path: its code points, the byte where each ends, and the rows of the distance
    // table after each of them (row 0: the empty prefix), in one flat buffer.
    std::vector<std::uint32_t> path;
    std::vector<std::size_t> path_end;      // path_end[d]: bytes of the first d code points (path_end[0] = 0)
    std::vector<int> rows(W), row_min(1, 0);
    std::string path_bytes;
    for (std::size_t j = 0; j < W; ++j) rows[j] = static_cast<int>(j);
    path_end.push_back(0);
    std::uint32_t t = lower_bound(prefix);
    while (t < n) {
        const std::string_view s = term(t);
        if (s.compare(0, prefix.size(), prefix) != 0) break;
        // the code points this term shares with the path: compare bytes, then back off to a
        // code point boundary of the path
        std::size_t b = 0;
        const std::size_t lim = std::min(s.size(), path_bytes.size());
        while (b < lim && s[b] == path_bytes[b]) ++b;
        std::size_t depth = static_cast<std::size_t>(std::upper_bound(path_end.begin(), path_end.end(), b) - path_end.begin()) - 1;
        path.resize(depth);
        path_end.resize(depth + 1);
        row_min.resize(depth + 1);
        rows.resize((depth + 1) * W);
        path_bytes.resize(path_end[depth]);
        std::size_t at = path_end[depth];
        bool dead = false;
        while (at < s.size()) {
            std::size_t len;
            const std::uint32_t cp = decode_one(s, at, len);
            const std::size_t i = depth;            // the new row is rows[i + 1]
            rows.resize((i + 2) * W);
            const int *p = &rows[i * W];
            const int *pp = i >= 1 ? &rows[(i - 1) * W] : nullptr;
            int *r = &rows[(i + 1) * W];
            r[0] = static_cast<int>(i) + 1;
            int mn = r[0];
            for (std::size_t j = 1; j < W; ++j) {
                const int cost = cp == word[j - 1] ? 0 : 1;
                int v = std::min({p[j] + 1, r[j - 1] + 1, p[j - 1] + cost});
                if (pp && j >= 2 && cp == word[j - 2] && path[i - 1] == word[j - 1]) v = std::min(v, pp[j - 2] + 1);
                r[j] = v;
                mn = std::min(mn, v);
            }
            path.push_back(cp);
            at += len;
            path_end.push_back(at);
            path_bytes.append(s.substr(at - len, len));
            row_min.push_back(mn);
            ++depth;
            // no extension can come back within max_edits: the next rows are at least the minimum
            // of this row and of the row before it plus one (a transposition reaches back two rows)
            if (mn > max_edits && row_min[i] + 1 > max_edits) { dead = true; break; }
        }
        if (dead) {
            // The node of the path above the dead code point (depth i) has every entry of its row
            // at max_edits or more (otherwise its children could not die). A child of it lives only
            // with a code point that matches the word where the row is exactly max_edits, or that
            // completes a transposition: jump to the next such sibling, or past the node.
            const std::size_t i = depth - 1;
            const std::uint32_t c = path[i];
            const int *p = &rows[i * W];
            const int *pp = i >= 1 ? &rows[(i - 1) * W] : nullptr;
            std::uint32_t best = 0xFFFFFFFFu;
            for (std::size_t j = 1; j < W; ++j) {
                if (p[j - 1] == max_edits && word[j - 1] > c) best = std::min(best, word[j - 1]);
                if (pp && j >= 2 && pp[j - 2] + 1 <= max_edits && path[i - 1] == word[j - 1] && word[j - 2] > c)
                    best = std::min(best, word[j - 2]);
            }
            std::string next = path_bytes.substr(0, path_end[i]);
            if (best != 0xFFFFFFFFu) append_utf8(next, best);
            else next = successor(next);
            if (next.empty()) break;
            // galloping search from t: the next sibling is usually near
            std::uint32_t lo = t, step = 1, hi = t + 1;
            while (hi < n && term(hi) < next) { lo = hi; step *= 2; hi = (n - hi > step) ? hi + step : n; }
            while (lo + 1 < hi) {
                const std::uint32_t mid = lo + (hi - lo) / 2;
                if (term(mid) < next) lo = mid; else hi = mid;
            }
            t = hi;
            continue;
        }
        const int d = rows[depth * W + W - 1];
        if (d <= max_edits) hit(t, s, d);
        ++t;
    }
}

std::string literal_prefix(std::string_view pattern)
{
    const std::size_t i = pattern.find_first_of("*?");
    return std::string(pattern.substr(0, i));
}

} // namespace vfts
