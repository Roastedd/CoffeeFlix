#include "core/md5.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace md5 {

namespace {

inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

inline uint32_t F(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (~x & z); }
inline uint32_t G(uint32_t x, uint32_t y, uint32_t z) { return (x & z) | (y & ~z); }
inline uint32_t H(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
inline uint32_t I(uint32_t x, uint32_t y, uint32_t z) { return y ^ (x | ~z); }

inline void FF(uint32_t& a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, int s, uint32_t ac) {
    a = rotl(a + F(b, c, d) + x + ac, s) + b;
}
inline void GG(uint32_t& a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, int s, uint32_t ac) {
    a = rotl(a + G(b, c, d) + x + ac, s) + b;
}
inline void HH(uint32_t& a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, int s, uint32_t ac) {
    a = rotl(a + H(b, c, d) + x + ac, s) + b;
}
inline void II(uint32_t& a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, int s, uint32_t ac) {
    a = rotl(a + I(b, c, d) + x + ac, s) + b;
}

}  // namespace

Hasher::Hasher() {
    state_[0] = 0x67452301;
    state_[1] = 0xefcdab89;
    state_[2] = 0x98badcfe;
    state_[3] = 0x10325476;
    used_ = 0;
    total_ = 0;
}

void Hasher::block(const uint8_t* p) {
    uint32_t w[16];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) |
               ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
    }

    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];

    // Round 1
    FF(a, b, c, d, w[0], 7, 0xd76aa478);
    FF(d, a, b, c, w[1], 12, 0xe8c7b756);
    FF(c, d, a, b, w[2], 17, 0x242070db);
    FF(b, c, d, a, w[3], 22, 0xc1bdceee);
    FF(a, b, c, d, w[4], 7, 0xf57c0faf);
    FF(d, a, b, c, w[5], 12, 0x4787c62a);
    FF(c, d, a, b, w[6], 17, 0xa8304613);
    FF(b, c, d, a, w[7], 22, 0xfd469501);
    FF(a, b, c, d, w[8], 7, 0x698098d8);
    FF(d, a, b, c, w[9], 12, 0x8b44f7af);
    FF(c, d, a, b, w[10], 17, 0xffff5bb1);
    FF(b, c, d, a, w[11], 22, 0x895cd7be);
    FF(a, b, c, d, w[12], 7, 0x6b901122);
    FF(d, a, b, c, w[13], 12, 0xfd987193);
    FF(c, d, a, b, w[14], 17, 0xa679438e);
    FF(b, c, d, a, w[15], 22, 0x49b40821);

    // Round 2
    GG(a, b, c, d, w[1], 5, 0xf61e2562);
    GG(d, a, b, c, w[6], 9, 0xc040b340);
    GG(c, d, a, b, w[11], 14, 0x265e5a51);
    GG(b, c, d, a, w[0], 20, 0xe9b6c7aa);
    GG(a, b, c, d, w[5], 5, 0xd62f105d);
    GG(d, a, b, c, w[10], 9, 0x02441453);
    GG(c, d, a, b, w[15], 14, 0xd8a1e681);
    GG(b, c, d, a, w[4], 20, 0xe7d3fbc8);
    GG(a, b, c, d, w[9], 5, 0x21e1cde6);
    GG(d, a, b, c, w[14], 9, 0xc33707d6);
    GG(c, d, a, b, w[3], 14, 0xf4d50d87);
    GG(b, c, d, a, w[8], 20, 0x455a14ed);
    GG(a, b, c, d, w[13], 5, 0xa9e3e905);
    GG(d, a, b, c, w[2], 9, 0xfcefa3f8);
    GG(c, d, a, b, w[7], 14, 0x676f02d9);
    GG(b, c, d, a, w[12], 20, 0x8d2a4c8a);

    // Round 3
    HH(a, b, c, d, w[5], 4, 0xfffa3942);
    HH(d, a, b, c, w[8], 11, 0x8771f681);
    HH(c, d, a, b, w[11], 16, 0x6d9d6122);
    HH(b, c, d, a, w[14], 23, 0xfde5380c);
    HH(a, b, c, d, w[1], 4, 0xa4beea44);
    HH(d, a, b, c, w[4], 11, 0x4bdecfa9);
    HH(c, d, a, b, w[7], 16, 0xf6bb4b60);
    HH(b, c, d, a, w[10], 23, 0xbebfbc70);
    HH(a, b, c, d, w[13], 4, 0x289b7ec6);
    HH(d, a, b, c, w[0], 11, 0xeaa127fa);
    HH(c, d, a, b, w[3], 16, 0xd4ef3085);
    HH(b, c, d, a, w[6], 23, 0x04881d05);
    HH(a, b, c, d, w[9], 4, 0xd9d4d039);
    HH(d, a, b, c, w[12], 11, 0xe6db99e5);
    HH(c, d, a, b, w[15], 16, 0x1fa27cf8);
    HH(b, c, d, a, w[2], 23, 0xc4ac5665);

    // Round 4
    II(a, b, c, d, w[0], 6, 0xf4292244);
    II(d, a, b, c, w[7], 10, 0x432aff97);
    II(c, d, a, b, w[14], 15, 0xab9423a7);
    II(b, c, d, a, w[5], 21, 0xfc93a039);
    II(a, b, c, d, w[12], 6, 0x655b59c3);
    II(d, a, b, c, w[3], 10, 0x8f0ccc92);
    II(c, d, a, b, w[10], 15, 0xffeff47d);
    II(b, c, d, a, w[1], 21, 0x85845dd1);
    II(a, b, c, d, w[8], 6, 0x6fa87e4f);
    II(d, a, b, c, w[15], 10, 0xfe2ce6e0);
    II(c, d, a, b, w[6], 15, 0xa3014314);
    II(b, c, d, a, w[13], 21, 0x4e0811a1);
    II(a, b, c, d, w[4], 6, 0xf7537e82);
    II(d, a, b, c, w[11], 10, 0xbd3af235);
    II(c, d, a, b, w[2], 15, 0x2ad7d2bb);
    II(b, c, d, a, w[9], 21, 0xeb86d391);

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
}

void Hasher::update(const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    total_ += size;
    while (size) {
        size_t n = std::min(size, sizeof(buf_) - used_);
        std::memcpy(buf_ + used_, p, n);
        used_ += n;
        p += n;
        size -= n;
        if (used_ == sizeof(buf_)) {
            block(buf_);
            used_ = 0;
        }
    }
}

std::string Hasher::hex() {
    uint64_t bits = total_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (used_ != 56) update(&zero, 1);
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; i++) len_bytes[i] = static_cast<uint8_t>((bits >> (i * 8)) & 0xff);
    update(len_bytes, 8);

    uint8_t digest[16];
    for (int i = 0; i < 4; i++) {
        digest[i * 4] = static_cast<uint8_t>(state_[i] & 0xff);
        digest[i * 4 + 1] = static_cast<uint8_t>((state_[i] >> 8) & 0xff);
        digest[i * 4 + 2] = static_cast<uint8_t>((state_[i] >> 16) & 0xff);
        digest[i * 4 + 3] = static_cast<uint8_t>((state_[i] >> 24) & 0xff);
    }

    char out[33];
    for (int i = 0; i < 16; i++) {
        std::snprintf(out + i * 2, 3, "%02x", digest[i]);
    }
    return std::string(out, 32);
}

std::string hash(std::string_view s) {
    Hasher h;
    h.update(s.data(), s.size());
    return h.hex();
}

}  // namespace md5
