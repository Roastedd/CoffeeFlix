// A few lines of protobuf by hand: Spotify's messages are protobuf, and the parts read and written
// here are small enough not to need a generator. From spotify-wiiu (MIT, see spotify_ap.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace spotify {

inline void pb_vi(std::vector<uint8_t>& b, uint64_t v) {
    do {
        uint8_t byte = v & 0x7F;
        v >>= 7;
        if (v) byte |= 0x80;
        b.push_back(byte);
    } while (v);
}
inline void pb_tag(std::vector<uint8_t>& b, uint32_t f, uint8_t w) { pb_vi(b, ((uint64_t)f << 3) | w); }
inline void pb_u32(std::vector<uint8_t>& b, uint32_t f, uint32_t v) { pb_tag(b, f, 0); pb_vi(b, v); }
inline void pb_u64(std::vector<uint8_t>& b, uint32_t f, uint64_t v) { pb_tag(b, f, 0); pb_vi(b, v); }
inline void pb_bytes(std::vector<uint8_t>& b, uint32_t f, const uint8_t* p, size_t n) {
    pb_tag(b, f, 2);
    pb_vi(b, n);
    b.insert(b.end(), p, p + n);
}
inline void pb_str(std::vector<uint8_t>& b, uint32_t f, const std::string& s) {
    pb_bytes(b, f, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}
inline void pb_msg(std::vector<uint8_t>& b, uint32_t f, const std::vector<uint8_t>& m) { pb_bytes(b, f, m.data(), m.size()); }

// Reads a message field by field: while (rd.next(f, w)) { if (f == 2) rd.read_str(s); else rd.skip(w); }
struct PRd {
    const uint8_t *p = nullptr, *end = nullptr;
    bool ok = true;

    bool vi(uint64_t& out) {
        out = 0;
        int sh = 0;
        while (p < end && sh < 64) {
            uint8_t b = *p++;
            out |= (uint64_t)(b & 0x7F) << sh;
            sh += 7;
            if (!(b & 0x80)) return true;
        }
        return ok = false;
    }
    bool next(uint32_t& field, uint8_t& wire) {
        if (p >= end) return false;
        uint64_t v;
        if (!vi(v)) return false;
        field = (uint32_t)(v >> 3);
        wire = v & 7;
        return true;
    }
    bool skip(uint8_t w) {
        uint64_t n;
        switch (w) {
        case 0: return vi(n);
        case 1:
            if (end - p < 8) return ok = false;
            p += 8;
            return true;
        case 2:
            if (!vi(n) || n > (uint64_t)(end - p)) return ok = false;
            p += n;
            return true;
        case 5:
            if (end - p < 4) return ok = false;
            p += 4;
            return true;
        default: return ok = false;
        }
    }
    bool enter(PRd& sub) {
        uint64_t n;
        if (!vi(n) || n > (uint64_t)(end - p)) return ok = false;
        sub = {p, p + (size_t)n, true};
        p += (size_t)n;
        return true;
    }
    bool read_bytes(std::vector<uint8_t>& out) {
        uint64_t n;
        if (!vi(n) || n > (uint64_t)(end - p)) return ok = false;
        out.assign(p, p + (size_t)n);
        p += (size_t)n;
        return true;
    }
    bool read_str(std::string& out) {
        uint64_t n;
        if (!vi(n) || n > (uint64_t)(end - p)) return ok = false;
        out.assign(reinterpret_cast<const char*>(p), (size_t)n);
        p += (size_t)n;
        return true;
    }
};

inline PRd reader(const std::vector<uint8_t>& b) { return {b.data(), b.data() + b.size()}; }
inline PRd reader(const std::string& b) {
    auto* p = reinterpret_cast<const uint8_t*>(b.data());
    return {p, p + b.size()};
}

}  // namespace spotify
