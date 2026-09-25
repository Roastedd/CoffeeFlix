#include "core/qr.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdlib>

namespace qr {

namespace {

const int MAX_VERSION = 10;
// Error correction level M, by version (index 0 unused).
const int ECC_PER_BLOCK[MAX_VERSION + 1] = {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
const int BLOCKS[MAX_VERSION + 1] = {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5};
const int FORMAT_M = 0;  // the two format bits for level M

int raw_modules(int ver) {
    int n = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
        int align = ver / 7 + 2;
        n -= (25 * align - 10) * align - 55;
        if (ver >= 7) n -= 36;
    }
    return n;
}

int data_codewords(int ver) { return raw_modules(ver) / 8 - ECC_PER_BLOCK[ver] * BLOCKS[ver]; }

uint8_t gf_mul(uint8_t x, uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return (uint8_t)z;
}

std::vector<uint8_t> rs_divisor(int degree) {
    std::vector<uint8_t> d(degree);
    d[degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; i++) {
        for (size_t j = 0; j < d.size(); j++) {
            d[j] = gf_mul(d[j], root);
            if (j + 1 < d.size()) d[j] ^= d[j + 1];
        }
        root = gf_mul(root, 2);
    }
    return d;
}

std::vector<uint8_t> rs_remainder(const std::vector<uint8_t>& data, const std::vector<uint8_t>& divisor) {
    std::vector<uint8_t> r(divisor.size());
    for (uint8_t b : data) {
        uint8_t factor = b ^ r[0];
        r.erase(r.begin());
        r.push_back(0);
        for (size_t i = 0; i < r.size(); i++) r[i] ^= gf_mul(divisor[i], factor);
    }
    return r;
}

// Splits the data into blocks, adds each block's error correction and interleaves them.
std::vector<uint8_t> add_ecc(const std::vector<uint8_t>& data, int ver) {
    int blocks = BLOCKS[ver], ecc = ECC_PER_BLOCK[ver];
    int raw = raw_modules(ver) / 8;
    int short_blocks = blocks - raw % blocks;
    int short_len = raw / blocks;
    std::vector<uint8_t> div = rs_divisor(ecc);
    std::vector<std::vector<uint8_t>> all;
    for (int i = 0, k = 0; i < blocks; i++) {
        int len = short_len - ecc + (i < short_blocks ? 0 : 1);
        std::vector<uint8_t> b(data.begin() + k, data.begin() + k + len);
        k += len;
        std::vector<uint8_t> rem = rs_remainder(b, div);
        if (i < short_blocks) b.push_back(0);
        b.insert(b.end(), rem.begin(), rem.end());
        all.push_back(std::move(b));
    }
    std::vector<uint8_t> out;
    for (size_t i = 0; i < all[0].size(); i++)
        for (size_t j = 0; j < all.size(); j++)
            if (i != (size_t)(short_len - ecc) || (int)j >= short_blocks) out.push_back(all[j][i]);
    return out;
}

struct Grid {
    int size;
    std::vector<bool> dark, fixed;  // fixed: part of a function pattern
    explicit Grid(int s) : size(s), dark((size_t)s * s), fixed((size_t)s * s) {}
    bool get(int x, int y) const { return dark[(size_t)y * size + x]; }
    void set(int x, int y, bool d) { dark[(size_t)y * size + x] = d; }
    void fix(int x, int y, bool d) {
        set(x, y, d);
        fixed[(size_t)y * size + x] = true;
    }
    bool is_fixed(int x, int y) const { return fixed[(size_t)y * size + x]; }
};

std::vector<int> alignment_positions(int ver, int size) {
    if (ver == 1) return {};
    int n = ver / 7 + 2;
    int step = (ver * 4 + n * 2 + 1) / (n * 2 - 2) * 2;
    std::vector<int> out;
    for (int i = 0, pos = size - 7; i < n - 1; i++, pos -= step) out.insert(out.begin(), pos);
    out.insert(out.begin(), 6);
    return out;
}

void draw_format(Grid& g, int mask) {
    int data = FORMAT_M << 3 | mask;
    int rem = data;
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = (data << 10 | rem) ^ 0x5412;
    auto bit = [&](int i) { return ((bits >> i) & 1) != 0; };
    int s = g.size;
    for (int i = 0; i <= 5; i++) g.fix(8, i, bit(i));
    g.fix(8, 7, bit(6));
    g.fix(8, 8, bit(7));
    g.fix(7, 8, bit(8));
    for (int i = 9; i < 15; i++) g.fix(14 - i, 8, bit(i));
    for (int i = 0; i < 8; i++) g.fix(s - 1 - i, 8, bit(i));
    for (int i = 8; i < 15; i++) g.fix(8, s - 15 + i, bit(i));
    g.fix(8, s - 8, true);
}

void draw_patterns(Grid& g, int ver) {
    int s = g.size;
    for (int i = 0; i < s; i++) {
        g.fix(6, i, i % 2 == 0);
        g.fix(i, 6, i % 2 == 0);
    }
    const int finders[3][2] = {{3, 3}, {s - 4, 3}, {3, s - 4}};
    for (const auto& f : finders) {
        int cx = f[0], cy = f[1];
        for (int dy = -4; dy <= 4; dy++)
            for (int dx = -4; dx <= 4; dx++) {
                int x = cx + dx, y = cy + dy, d = std::max(std::abs(dx), std::abs(dy));
                if (x >= 0 && x < s && y >= 0 && y < s) g.fix(x, y, d != 2 && d != 4);
            }
    }
    std::vector<int> pos = alignment_positions(ver, s);
    size_t n = pos.size();
    for (size_t i = 0; i < n; i++)
        for (size_t j = 0; j < n; j++) {
            if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)) continue;
            for (int dy = -2; dy <= 2; dy++)
                for (int dx = -2; dx <= 2; dx++)
                    g.fix(pos[i] + dx, pos[j] + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
        }
    draw_format(g, 0);  // reserves the area; drawn for real once the mask is chosen
    if (ver >= 7) {
        int rem = ver;
        for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        long bits = (long)ver << 12 | rem;
        for (int i = 0; i < 18; i++) {
            bool b = ((bits >> i) & 1) != 0;
            int a = s - 11 + i % 3, c = i / 3;
            g.fix(a, c, b);
            g.fix(c, a, b);
        }
    }
}

void draw_codewords(Grid& g, const std::vector<uint8_t>& data) {
    size_t i = 0;
    int s = g.size;
    for (int right = s - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < s; vert++)
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                bool upward = ((right + 1) & 2) == 0;
                int y = upward ? s - 1 - vert : vert;
                if (!g.is_fixed(x, y) && i < data.size() * 8) {
                    g.set(x, y, ((data[i >> 3] >> (7 - (i & 7))) & 1) != 0);
                    i++;
                }
            }
    }
}

void apply_mask(Grid& g, int mask) {
    for (int y = 0; y < g.size; y++)
        for (int x = 0; x < g.size; x++) {
            bool flip = false;
            switch (mask) {
                case 0: flip = (x + y) % 2 == 0; break;
                case 1: flip = y % 2 == 0; break;
                case 2: flip = x % 3 == 0; break;
                case 3: flip = (x + y) % 3 == 0; break;
                case 4: flip = (x / 3 + y / 2) % 2 == 0; break;
                case 5: flip = x * y % 2 + x * y % 3 == 0; break;
                case 6: flip = (x * y % 2 + x * y % 3) % 2 == 0; break;
                default: flip = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
            }
            if (flip && !g.is_fixed(x, y)) g.set(x, y, !g.get(x, y));
        }
}

// The standard's penalty score: lower scans more reliably.
long penalty(const Grid& g) {
    int s = g.size;
    long score = 0;
    for (int pass = 0; pass < 2; pass++) {  // rows, then columns
        for (int a = 0; a < s; a++) {
            auto m = [&](int b) { return pass == 0 ? g.get(b, a) : g.get(a, b); };
            int run = 1;
            for (int b = 1; b <= s; b++) {
                if (b < s && m(b) == m(b - 1)) {
                    run++;
                    continue;
                }
                if (run >= 5) score += 3 + (run - 5);
                run = 1;
            }
            // 1:1:3:1:1 finder look-alikes with four light modules on one side.
            static const bool P1[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0}, P2[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
            for (int b = 0; b + 11 <= s; b++) {
                bool e1 = true, e2 = true;
                for (int k = 0; k < 11; k++) {
                    e1 = e1 && m(b + k) == P1[k];
                    e2 = e2 && m(b + k) == P2[k];
                }
                if (e1) score += 40;
                if (e2) score += 40;
            }
        }
    }
    for (int y = 0; y + 1 < s; y++)
        for (int x = 0; x + 1 < s; x++) {
            bool c = g.get(x, y);
            if (c == g.get(x + 1, y) && c == g.get(x, y + 1) && c == g.get(x + 1, y + 1)) score += 3;
        }
    long dark = 0;
    for (bool d : g.dark) dark += d;
    long total = (long)s * s;
    long k = (std::labs(dark * 20 - total * 10) + total - 1) / total - 1;
    return score + std::max(0L, k) * 10;
}

}  // namespace

Code encode(const std::string& text) {
    Code out;
    int ver = 1;
    for (; ver <= MAX_VERSION; ver++) {
        int count_bits = ver <= 9 ? 8 : 16;
        if (4 + count_bits + 8 * (long)text.size() <= data_codewords(ver) * 8) break;
    }
    if (ver > MAX_VERSION) return out;

    // Byte mode: 0100, the length, the bytes, a terminator, then padding.
    std::vector<bool> bits;
    auto put = [&](uint32_t v, int n) {
        for (int i = n - 1; i >= 0; i--) bits.push_back(((v >> i) & 1) != 0);
    };
    put(4, 4);
    put((uint32_t)text.size(), ver <= 9 ? 8 : 16);
    for (unsigned char c : text) put(c, 8);
    size_t capacity = (size_t)data_codewords(ver) * 8;
    put(0, (int)std::min<size_t>(4, capacity - bits.size()));
    put(0, (int)((8 - bits.size() % 8) % 8));
    for (uint8_t pad = 0xEC; bits.size() < capacity; pad ^= 0xEC ^ 0x11) put(pad, 8);
    std::vector<uint8_t> data(bits.size() / 8);
    for (size_t i = 0; i < bits.size(); i++) data[i >> 3] |= (uint8_t)(bits[i] << (7 - (i & 7)));

    int size = ver * 4 + 17;
    Grid g(size);
    draw_patterns(g, ver);
    draw_codewords(g, add_ecc(data, ver));
    int best = 0;
    long best_score = LONG_MAX;
    for (int mask = 0; mask < 8; mask++) {
        apply_mask(g, mask);
        draw_format(g, mask);
        long p = penalty(g);
        if (p < best_score) {
            best_score = p;
            best = mask;
        }
        apply_mask(g, mask);  // undo
    }
    apply_mask(g, best);
    draw_format(g, best);
    out.size = size;
    out.dark = g.dark;
    return out;
}

}  // namespace qr
