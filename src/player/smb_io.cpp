#include "player/smb_io.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <memory>

#include "services/smb.hpp"

namespace player {

namespace {

// Large reads: one SMB round trip per 256 KB keeps a LAN link busy.
constexpr int BUFFER_SIZE = 256 << 10;

struct SmbStream {
    std::unique_ptr<smb::File> file;
    const std::atomic<bool>* abort = nullptr;
};

int read_packet(void* opaque, uint8_t* buf, int size) {
    auto* s = (SmbStream*)opaque;
    if (s->abort && s->abort->load()) return AVERROR_EXIT;
    int n = s->file->read(buf, size);
    if (n < 0) return AVERROR(EIO);
    return n == 0 ? AVERROR_EOF : n;
}

int64_t seek(void* opaque, int64_t offset, int whence) {
    auto* s = (SmbStream*)opaque;
    if (whence & AVSEEK_SIZE) return (int64_t)s->file->size();
    int64_t pos = s->file->seek(offset, whence & ~AVSEEK_FORCE);
    return pos < 0 ? AVERROR(EINVAL) : pos;
}

}  // namespace

AVIOContext* smb_io_open(const std::string& url, const std::atomic<bool>* abort, std::string& error) {
    auto s = std::make_unique<SmbStream>();
    s->abort = abort;
    s->file = smb::File::open(url, error);
    if (!s->file) return nullptr;
    uint8_t* buf = (uint8_t*)av_malloc(BUFFER_SIZE);
    AVIOContext* pb = buf ? avio_alloc_context(buf, BUFFER_SIZE, 0, s.get(), read_packet, nullptr, seek) : nullptr;
    if (!pb) {
        av_free(buf);
        error = "Out of memory";
        return nullptr;
    }
    s.release();  // owned by pb until smb_io_free()
    return pb;
}

void smb_io_free(AVIOContext* pb) {
    if (!pb || pb->read_packet != read_packet) return;
    delete (SmbStream*)pb->opaque;
    av_freep(&pb->buffer);  // FFmpeg may have replaced the original buffer
    avio_context_free(&pb);
}

void close_input(AVFormatContext** fmt) {
    AVIOContext* pb = *fmt && ((*fmt)->flags & AVFMT_FLAG_CUSTOM_IO) ? (*fmt)->pb : nullptr;
    avformat_close_input(fmt);
    smb_io_free(pb);
}

}  // namespace player
