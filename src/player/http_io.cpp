#include "player/http_io.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

#include "core/http.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"

namespace player {

namespace {

constexpr int BUFFER_SIZE = 64 << 10;
// One request per megabyte: seconds of video at the qualities the Wii U plays, and each
// request is quick. http::perform keeps the connection (and its TLS session) between them.
constexpr int64_t CHUNK = 1 << 20;
constexpr int ATTEMPTS = 4;

struct HttpStream {
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    const std::atomic<bool>* abort = nullptr;
    int64_t size = -1;  // from Content-Range once the first chunk arrived
    int64_t pos = 0;
    std::string chunk;  // the bytes at [chunk_start, chunk_start + chunk.size())
    int64_t chunk_start = 0;
    int requests = 0;
};

bool aborted(const HttpStream& s) { return s.abort && s.abort->load(); }

// "bytes 0-1048575/42031050" -> 42031050 (-1 when missing or "*").
int64_t content_range_total(const http::Response& r) {
    auto it = r.headers.find("content-range");
    if (it == r.headers.end()) return -1;
    size_t slash = it->second.rfind('/');
    if (slash == std::string::npos) return -1;
    char* end = nullptr;
    long long total = std::strtoll(it->second.c_str() + slash + 1, &end, 10);
    return end && end != it->second.c_str() + slash + 1 && total > 0 ? (int64_t)total : -1;
}

void sleep_unless_aborted(const HttpStream& s, int ms) {
    for (int waited = 0; waited < ms && !aborted(s); waited += 50)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// Replaces the chunk with the one starting at `start`. Retries dropped connections and
// server errors; a refusal (403: an expired or IP-locked link) fails at once.
bool fetch(HttpStream& s, int64_t start, std::string& error) {
    int64_t end = start + CHUNK - 1;
    if (s.size > 0) end = std::min(end, s.size - 1);
    http::Request req;
    req.url = s.url;
    req.headers = s.headers;
    req.headers.emplace_back("Range", util::fmt("bytes=%lld-%lld", (long long)start, (long long)end));
    req.timeout = 30;
    req.max_bytes = (size_t)CHUNK;
    req.cancel = s.abort;

    for (int attempt = 1;; attempt++) {
        double t0 = util::now_seconds();
        http::Response r = http::perform(req);
        double took = util::now_seconds() - t0;
        if (aborted(s)) {
            error = "Cancelled";
            return false;
        }
        if (r.ok() && r.status == 206) {
            int64_t total = content_range_total(r);
            if (total > 0) s.size = total;
        } else if (r.ok() && r.status == 200 && start == 0) {
            s.size = (int64_t)r.body.size();  // no range support, but the whole file fit
        } else if (r.status == 200) {
            error = "The server doesn't support partial downloads";
            return false;
        } else if (r.status >= 400 && r.status < 500 && r.status != 408 && r.status != 429) {
            error = r.error;
            return false;
        } else {
            if (attempt >= ATTEMPTS) {
                error = r.error.empty() ? util::fmt("HTTP error %ld", r.status) : r.error;
                return false;
            }
            log_message(LOG_WARNING, "Player", "Download at %lld failed (%s), retrying", (long long)start,
                        r.error.empty() ? util::fmt("HTTP %ld", r.status).c_str() : r.error.c_str());
            sleep_unless_aborted(s, 500 << (attempt - 1));
            continue;
        }
        if (s.requests++ == 0)
            log_message(LOG_OK, "Player", "First %lld KB of %lld KB in %.1f s (%.0f KB/s)",
                        (long long)r.body.size() / 1024, (long long)s.size / 1024, took,
                        r.body.size() / 1024.0 / std::max(took, 0.001));
        s.chunk = std::move(r.body);
        s.chunk_start = start;
        return true;
    }
}

int read_packet(void* opaque, uint8_t* buf, int size) {
    auto* s = (HttpStream*)opaque;
    if (aborted(*s)) return AVERROR_EXIT;
    if (s->size >= 0 && s->pos >= s->size) return AVERROR_EOF;
    int64_t off = s->pos - s->chunk_start;
    if (off < 0 || off >= (int64_t)s->chunk.size()) {
        std::string err;
        if (!fetch(*s, s->pos, err)) {
            if (aborted(*s)) return AVERROR_EXIT;
            log_message(LOG_ERROR, "Player", "Download stopped: %s", err.c_str());
            return AVERROR(EIO);
        }
        if (s->chunk.empty()) return AVERROR_EOF;
        off = 0;
    }
    int n = (int)std::min<int64_t>(size, (int64_t)s->chunk.size() - off);
    std::memcpy(buf, s->chunk.data() + off, n);
    s->pos += n;
    return n;
}

int64_t seek(void* opaque, int64_t offset, int whence) {
    auto* s = (HttpStream*)opaque;
    if (whence & AVSEEK_SIZE) return s->size >= 0 ? s->size : AVERROR(ENOSYS);
    whence &= ~AVSEEK_FORCE;
    int64_t pos = whence == SEEK_SET                    ? offset
                  : whence == SEEK_CUR                  ? s->pos + offset
                  : whence == SEEK_END && s->size >= 0 ? s->size + offset
                                                        : -1;
    if (pos < 0) return AVERROR(EINVAL);
    s->pos = pos;  // fetched on the next read, unless it's in the current chunk
    return pos;
}

}  // namespace

AVIOContext* http_io_open(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                          const std::atomic<bool>* abort, std::string& error) {
    auto s = std::make_unique<HttpStream>();
    s->url = url;
    s->headers = headers;
    s->abort = abort;
    if (!fetch(*s, 0, error)) return nullptr;
    uint8_t* buf = (uint8_t*)av_malloc(BUFFER_SIZE);
    AVIOContext* pb = buf ? avio_alloc_context(buf, BUFFER_SIZE, 0, s.get(), read_packet, nullptr, seek) : nullptr;
    if (!pb) {
        av_free(buf);
        error = "Out of memory";
        return nullptr;
    }
    s.release();  // owned by pb until http_io_free()
    return pb;
}

void http_io_free(AVIOContext* pb) {
    if (!pb || pb->read_packet != read_packet) return;
    delete (HttpStream*)pb->opaque;
    av_freep(&pb->buffer);  // FFmpeg may have replaced the original buffer
    avio_context_free(&pb);
}

}  // namespace player
