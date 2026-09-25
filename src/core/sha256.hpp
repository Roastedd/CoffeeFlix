// SHA-256 (FIPS 180-4), fed in pieces: checks downloads against the digest their source publishes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sha256 {

class Hasher {
public:
    Hasher();
    void update(const void* data, size_t size);
    std::string hex();  // lower-case; ends the hash, so call it once

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t used_ = 0;
    uint64_t total_ = 0;
};

}  // namespace sha256
