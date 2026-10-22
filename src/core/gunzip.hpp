// Text that may come gzipped (playlists and listings saved as .gz files, which servers send as
// they are): plain text goes through as it is, gzip is unpacked as it arrives.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace gunzip {

class Stream {
public:
    explicit Stream(std::function<void(const char* data, size_t size)> out);
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    // False once the gzip turns out broken (what came before it was handed on).
    bool feed(const char* data, size_t size);
    bool gzipped() const { return mode_ == GZIP; }

private:
    enum Mode { UNKNOWN, PLAIN, GZIP, BROKEN };
    std::function<void(const char*, size_t)> out_;
    Mode mode_ = UNKNOWN;
    unsigned char first_ = 0;  // a lone first byte, until the second says what it is
    bool have_first_ = false;
    struct Z;
    std::unique_ptr<Z> z_;
    bool inflate(const unsigned char* data, size_t size);
};

}  // namespace gunzip
