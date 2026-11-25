#include "player/spotify_io.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <cstring>
#include <memory>

#include "core/i18n.hpp"
#include "player/http_io.hpp"
#include "services/spotify/spotify_crypto.hpp"

namespace player {

namespace {

constexpr int BUFFER_SIZE = 64 << 10;
// Spotify's files start with a page of their own before the Ogg stream (librespot's SPOTIFY_OGG_HEADER_END).
constexpr int64_t HEADER = 0xa7;

struct SpotifyStream {
    AVIOContext* inner = nullptr;  // the encrypted file, from http_io
    std::unique_ptr<spotify::crypto::Aes128> aes;
    int64_t skip = 0;              // bytes in front of the Ogg stream
    const std::atomic<bool>* abort = nullptr;
    ~SpotifyStream() { http_io_free(inner); }
};

int read_packet(void* opaque, uint8_t* buf, int size) {
    auto* s = (SpotifyStream*)opaque;
    if (s->abort && s->abort->load()) return AVERROR_EXIT;
    const int64_t at = avio_tell(s->inner);
    const int n = avio_read(s->inner, buf, size);
    if (n > 0) spotify::crypto::audio_decrypt(*s->aes, (uint64_t)at, buf, (size_t)n);
    return n == 0 ? AVERROR_EOF : n;
}

int64_t seek(void* opaque, int64_t offset, int whence) {
    auto* s = (SpotifyStream*)opaque;
    if (whence & AVSEEK_SIZE) {
        const int64_t size = avio_size(s->inner);
        return size < 0 ? size : size - s->skip;
    }
    whence &= ~AVSEEK_FORCE;
    if (whence == SEEK_SET) offset += s->skip;
    else if (whence == SEEK_END) {
        const int64_t size = avio_size(s->inner);
        if (size < 0) return AVERROR(EINVAL);
        offset += size, whence = SEEK_SET;
    }
    const int64_t pos = avio_seek(s->inner, offset, whence);
    return pos < 0 ? pos : pos - s->skip;
}

}  // namespace

AVIOContext* spotify_io_open(const std::string& url, const std::vector<uint8_t>& key, const std::atomic<bool>* abort,
                             std::string& error) {
    if (key.size() != 16) {
        error = tr("Couldn't open stream (no key)");
        return nullptr;
    }
    auto s = std::make_unique<SpotifyStream>();
    s->abort = abort;
    s->aes = std::make_unique<spotify::crypto::Aes128>(key.data());
    if (!(s->inner = http_io_open(url, {}, abort, true, error))) return nullptr;

    // The Ogg stream starts after Spotify's header, when the file has one ("OggS" there).
    uint8_t head[HEADER + 4];
    if (avio_read(s->inner, head, sizeof(head)) == (int)sizeof(head)) {
        spotify::crypto::audio_decrypt(*s->aes, 0, head, sizeof(head));
        if (memcmp(head + HEADER, "OggS", 4) == 0) s->skip = HEADER;
    }
    if (avio_seek(s->inner, s->skip, SEEK_SET) < 0) {
        error = tr("Couldn't open stream (seek)");
        return nullptr;
    }

    uint8_t* buf = (uint8_t*)av_malloc(BUFFER_SIZE);
    AVIOContext* pb = buf ? avio_alloc_context(buf, BUFFER_SIZE, 0, s.get(), read_packet, nullptr, seek) : nullptr;
    if (!pb) {
        av_free(buf);
        error = tr("Out of memory");
        return nullptr;
    }
    s.release();  // owned by pb until spotify_io_free()
    return pb;
}

void spotify_io_free(AVIOContext* pb) {
    if (!pb || pb->read_packet != read_packet) return;
    delete (SpotifyStream*)pb->opaque;
    av_freep(&pb->buffer);
    avio_context_free(&pb);
}

}  // namespace player
