#include "core/gunzip.hpp"

#include <zlib.h>

namespace gunzip {

struct Stream::Z {
    z_stream s{};
    bool done = false;
};

Stream::Stream(std::function<void(const char*, size_t)> out) : out_(std::move(out)) {}

Stream::~Stream() {
    if (z_) inflateEnd(&z_->s);
}

bool Stream::feed(const char* data, size_t size) {
    if (!size) return mode_ != BROKEN;
    if (mode_ == UNKNOWN) {
        // gzip starts 1f 8b.
        if (!have_first_) {
            first_ = (unsigned char)data[0];
            have_first_ = true;
            data++, size--;
            if (!size) return true;
        }
        if (first_ == 0x1f && (unsigned char)data[0] == 0x8b) {
            z_ = std::make_unique<Z>();
            if (inflateInit2(&z_->s, 16 + MAX_WBITS) != Z_OK) {
                mode_ = BROKEN;
                return false;
            }
            mode_ = GZIP;
            const unsigned char head = 0x1f;
            if (!inflate(&head, 1)) return false;
        } else {
            mode_ = PLAIN;
            out_((const char*)&first_, 1);
        }
    }
    if (mode_ == PLAIN) {
        out_(data, size);
        return true;
    }
    if (mode_ == BROKEN) return false;
    return inflate((const unsigned char*)data, size);
}

bool Stream::inflate(const unsigned char* data, size_t size) {
    if (z_->done) return true;  // what follows the end (padding) is left
    z_->s.next_in = const_cast<unsigned char*>(data);
    z_->s.avail_in = (uInt)size;
    unsigned char buf[32768];
    while (z_->s.avail_in > 0) {
        z_->s.next_out = buf;
        z_->s.avail_out = sizeof(buf);
        int r = ::inflate(&z_->s, Z_NO_FLUSH);
        size_t got = sizeof(buf) - z_->s.avail_out;
        if (got) out_((const char*)buf, got);
        if (r == Z_STREAM_END) {
            z_->done = true;
            return true;
        }
        if (r != Z_OK && r != Z_BUF_ERROR) {
            mode_ = BROKEN;
            return false;
        }
        if (r == Z_BUF_ERROR && got == 0) break;
    }
    return true;
}

}  // namespace gunzip
