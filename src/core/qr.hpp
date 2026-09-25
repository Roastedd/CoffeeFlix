// QR codes for short texts such as sign-in links: byte mode, medium error correction, versions
// 1 to 10 (up to 213 bytes). Follows ISO/IEC 18004 the way Project Nayuki's generator lays it out.
#pragma once

#include <string>
#include <vector>

namespace qr {

struct Code {
    int size = 0;             // modules per side; 0 when the text doesn't fit
    std::vector<bool> dark;   // size * size, row by row
    bool at(int x, int y) const { return dark[(size_t)y * size + x]; }
};

Code encode(const std::string& text);

}  // namespace qr
