#include "expand.h"

#include <algorithm>
#include <cstdlib>

namespace vtext {

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

std::string literal_prefix(std::string_view pattern)
{
    const std::size_t i = pattern.find_first_of("*?");
    return std::string(pattern.substr(0, i));
}

} // namespace vtext
