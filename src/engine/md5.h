// MD5 (RFC 1321), as Vertica's MD5() computes it: the hex digest of the bytes. Used for the access
// tokens of the views (docs/design.md "Access"): the SQL side and the functions must compute the
// same value, and MD5 is the digest both have.
#ifndef VFTS_ENGINE_MD5_H
#define VFTS_ENGINE_MD5_H

#include <cstdint>
#include <cstring>
#include <string>

namespace vfts {

// The 16 bytes of MD5(msg). No allocation for messages shorter than 120 bytes.
inline void md5_raw(const void *msg, std::size_t len, unsigned char out[16])
{
    static const std::uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    const std::size_t total = ((len + 8) / 64 + 1) * 64;
    unsigned char small[128];
    std::string big;
    unsigned char *m = small;
    if (total > sizeof small) { big.assign(total, '\0'); m = reinterpret_cast<unsigned char *>(&big[0]); }
    std::memcpy(m, msg, len);
    m[len] = 0x80;
    std::memset(m + len + 1, 0, total - len - 1);
    const std::uint64_t bits = static_cast<std::uint64_t>(len) * 8;
    for (int i = 0; i < 8; ++i) m[total - 8 + i] = static_cast<unsigned char>((bits >> (8 * i)) & 0xff);
    std::uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    for (std::size_t at = 0; at < total; at += 64) {
        std::uint32_t w[16];
        for (int i = 0; i < 16; ++i) {
            const unsigned char *p = m + at + 4 * i;
            w[i] = static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
                   static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t f;
            int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            const std::uint32_t t = d;
            d = c;
            c = b;
            const std::uint32_t x = a + f + K[i] + w[g];
            b = b + ((x << R[i]) | (x >> (32 - R[i])));
            a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) out[4 * j + i] = static_cast<unsigned char>((h[j] >> (8 * i)) & 0xff);
}

// The hex digest, as Vertica's MD5() returns it.
inline std::string md5_hex(const std::string &msg)
{
    unsigned char d[16];
    md5_raw(msg.data(), msg.size(), d);
    static const char hex[] = "0123456789abcdef";
    std::string out(32, '0');
    for (int i = 0; i < 16; ++i) { out[2 * i] = hex[d[i] >> 4]; out[2 * i + 1] = hex[d[i] & 15]; }
    return out;
}

} // namespace vfts

#endif
