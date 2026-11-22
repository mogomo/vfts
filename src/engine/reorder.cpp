#include "reorder.h"
#include "parallel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <numeric>
#include <stdexcept>

namespace vfts {

namespace {

// Per bisection: degrees of the terms in both halves and the gain of moving a document, per term.
struct Scratch {
    std::vector<std::int32_t> dl, dr;
    std::vector<float> ml, mr;
    std::vector<std::uint32_t> touched;
    std::vector<float> gain;
    std::vector<std::uint32_t> li, ri;
};

class Bisection {
public:
    Bisection(const ForwardIndex &f, const ReorderOptions &o) : f_(f), o_(o)
    {
        lg_.resize(static_cast<std::size_t>(f.n_docs) + 3);
        lg_[0] = 0.0f;
        for (std::size_t i = 1; i < lg_.size(); ++i) lg_[i] = static_cast<float>(std::log2(static_cast<double>(i)));
        docs_.resize(f.n_docs);
        std::iota(docs_.begin(), docs_.end(), 0u);
    }

    std::vector<std::uint32_t> run(std::size_t threads, ReorderStats &st)
    {
        const std::uint32_t minp = std::max<std::uint32_t>(2, o_.min_partition);
        std::vector<std::pair<std::uint32_t, std::uint32_t>> level;
        if (f_.n_docs >= 2 * minp) level.emplace_back(0, f_.n_docs);
        while (!level.empty()) {
            ++st.levels;
            const std::size_t inner = std::max<std::size_t>(1, threads / level.size());
            Pool::instance().run(level.size(), threads, [&](std::size_t i) { bisect(level[i].first, level[i].second, inner); });
            std::vector<std::pair<std::uint32_t, std::uint32_t>> next;
            for (const auto &p : level) {
                const std::uint32_t mid = p.first + (p.second - p.first) / 2;
                if (mid - p.first >= 2 * minp) next.emplace_back(p.first, mid);
                if (p.second - mid >= 2 * minp) next.emplace_back(mid, p.second);
            }
            level.swap(next);
        }
        st.swaps = swaps_.load();
        st.rounds = rounds_.load();
        return std::move(docs_);
    }

private:
    // d * log2(n / (d + 1)): the cost of a term with d documents in a half of n
    float cost(std::int32_t d, std::uint32_t n) const { return static_cast<float>(d) * (lg_[n] - lg_[static_cast<std::size_t>(d) + 1]); }

    const std::uint32_t *terms_of(std::uint32_t doc, std::uint32_t &n) const
    {
        const std::uint64_t a = f_.off[doc], b = f_.off[doc + 1];
        n = static_cast<std::uint32_t>(b - a);
        return f_.terms.data() + a;
    }

    Scratch *acquire()
    {
        {
            std::lock_guard<std::mutex> l(mu_);
            if (!free_.empty()) { Scratch *s = free_.back().release(); free_.pop_back(); return s; }
        }
        Scratch *s = new Scratch();
        s->dl.assign(f_.n_terms, 0);
        s->dr.assign(f_.n_terms, 0);
        s->ml.assign(f_.n_terms, 0.0f);
        s->mr.assign(f_.n_terms, 0.0f);
        return s;
    }
    void release(Scratch *s)
    {
        std::lock_guard<std::mutex> l(mu_);
        free_.emplace_back(s);
    }

    // Parallel sort of positions by (gain descending, position ascending).
    static void sort_desc(std::vector<std::uint32_t> &v, const std::vector<float> &gain, std::uint32_t lo, std::size_t par)
    {
        auto cmp = [&](std::uint32_t a, std::uint32_t b) {
            const float ga = gain[a - lo], gb = gain[b - lo];
            return ga != gb ? ga > gb : a < b;
        };
        const std::size_t n = v.size();
        if (par <= 1 || n < 200000) { std::sort(v.begin(), v.end(), cmp); return; }
        std::size_t parts = 1;
        while (parts < par && parts < 64) parts *= 2;
        std::vector<std::size_t> cut(parts + 1);
        for (std::size_t i = 0; i <= parts; ++i) cut[i] = n * i / parts;
        Pool::instance().run(parts, par, [&](std::size_t i) { std::sort(v.begin() + cut[i], v.begin() + cut[i + 1], cmp); });
        for (std::size_t w = 1; w < parts; w *= 2) {
            const std::size_t merges = parts / (2 * w);
            Pool::instance().run(merges, par, [&](std::size_t m) {
                const std::size_t a = cut[2 * w * m], b = cut[2 * w * m + w], c = cut[2 * w * m + 2 * w];
                std::inplace_merge(v.begin() + a, v.begin() + b, v.begin() + c, cmp);
            });
        }
    }

    void bisect(std::uint32_t lo, std::uint32_t hi, std::size_t par)
    {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        const std::uint32_t n1 = mid - lo, n2 = hi - mid, n = hi - lo;
        Scratch &s = *acquire();
        // degrees
        s.touched.clear();
        if (par > 1 && n >= 100000) {
            const std::size_t parts = par;
            std::vector<std::vector<std::int32_t>> cl(parts), cr(parts);
            Pool::instance().run(parts, par, [&](std::size_t c) {
                cl[c].assign(f_.n_terms, 0);
                cr[c].assign(f_.n_terms, 0);
                const std::uint32_t a = lo + static_cast<std::uint32_t>(static_cast<std::uint64_t>(n) * c / parts);
                const std::uint32_t b = lo + static_cast<std::uint32_t>(static_cast<std::uint64_t>(n) * (c + 1) / parts);
                for (std::uint32_t p = a; p < b; ++p) {
                    std::uint32_t k;
                    const std::uint32_t *t = terms_of(docs_[p], k);
                    std::vector<std::int32_t> &d = p < mid ? cl[c] : cr[c];
                    for (std::uint32_t j = 0; j < k; ++j) ++d[t[j]];
                }
            });
            for (std::uint32_t t = 0; t < f_.n_terms; ++t) {
                std::int32_t a = 0, b = 0;
                for (std::size_t c = 0; c < parts; ++c) { a += cl[c][t]; b += cr[c][t]; }
                s.dl[t] = a;
                s.dr[t] = b;
                if (a || b) s.touched.push_back(t);
            }
        } else {
            for (std::uint32_t p = lo; p < hi; ++p) {
                std::uint32_t k;
                const std::uint32_t *t = terms_of(docs_[p], k);
                std::vector<std::int32_t> &d = p < mid ? s.dl : s.dr;
                for (std::uint32_t j = 0; j < k; ++j) {
                    if (s.dl[t[j]] == 0 && s.dr[t[j]] == 0) s.touched.push_back(t[j]);
                    ++d[t[j]];
                }
            }
        }
        s.gain.resize(n);
        for (std::uint32_t it = 0; it < o_.max_iters; ++it) {
            ++rounds_;
            for (std::uint32_t t : s.touched) {
                const std::int32_t a = s.dl[t], b = s.dr[t];
                const float base = cost(a, n1) + cost(b, n2);
                s.ml[t] = a > 0 ? base - cost(a - 1, n1) - cost(b + 1, n2) : 0.0f;
                s.mr[t] = b > 0 ? base - cost(a + 1, n1) - cost(b - 1, n2) : 0.0f;
            }
            auto gains = [&](std::uint32_t a, std::uint32_t b) {
                for (std::uint32_t p = a; p < b; ++p) {
                    std::uint32_t k;
                    const std::uint32_t *t = terms_of(docs_[p], k);
                    const std::vector<float> &m = p < mid ? s.ml : s.mr;
                    double g = 0;
                    for (std::uint32_t j = 0; j < k; ++j) g += m[t[j]];
                    s.gain[p - lo] = static_cast<float>(g);
                }
            };
            if (par > 1 && n >= 100000) {
                const std::size_t parts = par * 4;
                Pool::instance().run(parts, par, [&](std::size_t c) {
                    gains(lo + static_cast<std::uint32_t>(static_cast<std::uint64_t>(n) * c / parts),
                          lo + static_cast<std::uint32_t>(static_cast<std::uint64_t>(n) * (c + 1) / parts));
                });
            } else {
                gains(lo, hi);
            }
            // only documents that can be part of a swap: gain + the best gain of the other side > 0
            float best_l = -1e30f, best_r = -1e30f;
            for (std::uint32_t p = lo; p < mid; ++p) best_l = std::max(best_l, s.gain[p - lo]);
            for (std::uint32_t p = mid; p < hi; ++p) best_r = std::max(best_r, s.gain[p - lo]);
            if (best_l + best_r <= 0) break;
            s.li.clear();
            s.ri.clear();
            for (std::uint32_t p = lo; p < mid; ++p) if (s.gain[p - lo] + best_r > 0) s.li.push_back(p);
            for (std::uint32_t p = mid; p < hi; ++p) if (s.gain[p - lo] + best_l > 0) s.ri.push_back(p);
            sort_desc(s.li, s.gain, lo, par);
            sort_desc(s.ri, s.gain, lo, par);
            std::uint64_t swaps = 0;
            const std::size_t m = std::min(s.li.size(), s.ri.size());
            for (std::size_t i = 0; i < m; ++i) {
                const std::uint32_t pl = s.li[i], pr = s.ri[i];
                if (s.gain[pl - lo] + s.gain[pr - lo] <= 0) break;
                std::uint32_t k;
                const std::uint32_t *t = terms_of(docs_[pl], k);
                for (std::uint32_t j = 0; j < k; ++j) { --s.dl[t[j]]; ++s.dr[t[j]]; }
                t = terms_of(docs_[pr], k);
                for (std::uint32_t j = 0; j < k; ++j) { --s.dr[t[j]]; ++s.dl[t[j]]; }
                std::swap(docs_[pl], docs_[pr]);
                ++swaps;
            }
            swaps_ += swaps;
            if (swaps == 0) break;
        }
        for (std::uint32_t t : s.touched) { s.dl[t] = 0; s.dr[t] = 0; }
        release(&s);
    }

    const ForwardIndex &f_;
    const ReorderOptions &o_;
    std::vector<float> lg_;
    std::vector<std::uint32_t> docs_;
    std::mutex mu_;
    std::vector<std::unique_ptr<Scratch>> free_;
    std::atomic<std::uint64_t> swaps_{0}, rounds_{0};
};

} // namespace

std::vector<std::uint32_t> bisect_order(const ForwardIndex &f, const ReorderOptions &o, ReorderStats *stats)
{
    const auto t0 = std::chrono::steady_clock::now();
    if (f.off.size() != static_cast<std::size_t>(f.n_docs) + 1) throw std::runtime_error("reorder: forward index offsets");
    ReorderStats st;
    std::size_t threads = o.threads > 0 ? static_cast<std::size_t>(o.threads) : std::max(1u, std::thread::hardware_concurrency());
    Bisection b(f, o);
    std::vector<std::uint32_t> order = b.run(threads, st);
    st.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (stats) *stats = st;
    return order;
}

} // namespace vfts
