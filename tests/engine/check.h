// Small helpers shared by the engine unit tests (same style as vertica-vector-db).
#ifndef VFTS_TESTS_CHECK_H
#define VFTS_TESTS_CHECK_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

static int failures = 0;

#define CHECK(cond) \
    do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

#define CHECK_EQ(a, b) \
    do { const auto &va_ = (a); const auto &vb_ = (b); if (!(va_ == vb_)) { \
        std::printf("FAIL %s:%d: %s == %s\n   got: %s\n  want: %s\n", __FILE__, __LINE__, #a, #b, \
                    show(va_).c_str(), show(vb_).c_str()); ++failures; } } while (0)

inline std::string show(const std::string &s) { return "'" + s + "'"; }
inline std::string show(const char *s) { return std::string("'") + s + "'"; }
template <class T> std::string show(const T &v) { return std::to_string(v); }
inline std::string show(const std::vector<std::string> &v)
{
    std::string s = "[";
    for (std::size_t i = 0; i < v.size(); ++i) s += (i ? ", '" : "'") + v[i] + "'";
    return s + "]";
}

inline int finish(const char *name)
{
    if (failures == 0) std::printf("%s: OK\n", name);
    else std::printf("%s: %d FAILURES\n", name, failures);
    return failures == 0 ? 0 : 1;
}

// True if f throws a std::runtime_error (or subclass) whose message contains `containing`.
template <class F> bool throws(F f, const char *containing = "")
{
    try { f(); } catch (const std::runtime_error &e) { return std::strstr(e.what(), containing) != nullptr; }
    return false;
}

// Deterministic pseudo random numbers, same on every platform.
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    std::uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    std::int64_t below(std::int64_t n) { return static_cast<std::int64_t>(next() % static_cast<std::uint64_t>(n)); }
};

#endif
