// MD5 (RFC 1321), fed in pieces: used for Subsonic / Navidrome authentication tokens.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace md5 {

class Hasher {
public:
    Hasher();
    void update(const void* data, size_t size);
    std::string hex();  // lower-case; ends the hash, so call it once

private:
    void block(const uint8_t* p);
    uint32_t state_[4];
    uint8_t buf_[64];
    size_t used_ = 0;
    uint64_t total_ = 0;
};

std::string hash(std::string_view s);

}  // namespace md5
