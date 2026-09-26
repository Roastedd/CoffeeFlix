#include "player/http_io.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "core/http.hpp"
#include "core/i18n.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"

namespace player {

namespace {

// The file is fetched as 1 MB ranges, two at a time and ahead of the reader. googlevideo
// paces each connection (to 150-250 KB/s once several are open, about what 720p plays at), the
// Wii U's sockets are slower still, and fetching only when FFmpeg asks leaves the demuxer idle
// for as long as a range takes: the video runs dry.
constexpr int BUFFER_SIZE = 64 << 10;
constexpr int64_t CHUNK = 1 << 20;
constexpr int64_t AHEAD = 8;        // chunks kept downloaded ahead of the reader
constexpr int CONNECTIONS = 2;
// A separate audio track is a tenth of the video's rate: 2 MB is minutes of it, and it
// shouldn't take bandwidth from the video.
constexpr int64_t AHEAD_AUDIO = 2;
constexpr int CONNECTIONS_AUDIO = 1;
constexpr int ATTEMPTS = 4;
constexpr int64_t SPEED_SAMPLE = 4 * CHUNK;

struct Chunk {
    int64_t start = 0;
    int64_t length = CHUNK;  // shorter at the end of the file
    std::vector<char> data;  // valid up to `filled`
    int64_t filled = 0;
    bool done = false, failed = false;
    std::string error;
    std::atomic<bool> cancel{false};  // evicted, or the stream is closing
};

struct HttpStream {
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    const std::atomic<bool>* abort = nullptr;
    int64_t pos = 0;  // the reader's (FFmpeg's thread only)
    int64_t ahead = AHEAD;
    int connections = CONNECTIONS;

    std::mutex m;
    std::condition_variable data_cv;  // bytes arrived, a chunk finished or failed
    std::condition_variable work_cv;  // a chunk became wanted, or stop
    std::map<int64_t, std::shared_ptr<Chunk>> chunks;  // by index
    int64_t cursor = 0;  // chunk index the reader is in
    int64_t size = -1;   // from the first response
    bool stop = false;
    std::vector<std::thread> workers;

    double opened_at = 0;
    int64_t downloaded = 0;
    bool first_logged = false, speed_logged = false;
};

bool aborted(const HttpStream& s) { return s.abort && s.abort->load(); }

// "bytes 0-1048575/42031050" -> first 0, total 42031050 (-1 when "*").
bool content_range(const http::Response& r, int64_t& first, int64_t& total) {
    auto it = r.headers.find("content-range");
    if (it == r.headers.end()) return false;
    const char* p = std::strchr(it->second.c_str(), ' ');
    if (!p) return false;
    char* end = nullptr;
    first = std::strtoll(p + 1, &end, 10);
    if (end == p + 1) return false;
    const char* slash = std::strrchr(it->second.c_str(), '/');
    total = slash ? std::strtoll(slash + 1, &end, 10) : -1;
    if (!slash || end == slash + 1 || total <= 0) total = -1;
    return true;
}

int64_t header_int(const http::Response& r, const char* name) {
    auto it = r.headers.find(name);
    return it == r.headers.end() ? -1 : std::strtoll(it->second.c_str(), nullptr, 10);
}

// Under s.m: the size became known, so the last chunk is shorter than the rest.
void set_size(HttpStream& s, int64_t size) {
    if (s.size >= 0 || size < 0) return;
    s.size = size;
    for (auto& [k, c] : s.chunks) c->length = std::max<int64_t>(0, std::min(CHUNK, size - c->start));
    s.work_cv.notify_all();
}

// Under s.m: the first chunk in the reader's window that nobody fetches yet (made on the
// spot), or null. Until the size is known only the first chunk is wanted: it tells the size.
std::shared_ptr<Chunk> next_wanted(HttpStream& s) {
    for (int64_t k = s.cursor; k < s.cursor + s.ahead; k++) {
        if (s.chunks.count(k)) continue;
        if (s.size < 0 && k > 0) return nullptr;
        if (s.size >= 0 && k * CHUNK >= s.size) return nullptr;
        auto c = std::make_shared<Chunk>();
        c->start = k * CHUNK;
        if (s.size >= 0) c->length = std::min(CHUNK, s.size - c->start);
        s.chunks[k] = c;
        return c;
    }
    return nullptr;
}

// Under s.m: the reader moved to chunk `k`. Drops what fell out of the window (one chunk
// back is kept: demuxers step back a little) and wakes the workers for what came in.
void move_cursor(HttpStream& s, int64_t k) {
    if (k == s.cursor) return;
    s.cursor = k;
    for (auto it = s.chunks.begin(); it != s.chunks.end();) {
        if (it->first < k - 1 || it->first >= k + s.ahead) {
            it->second->cancel = true;
            it = s.chunks.erase(it);
        } else {
            ++it;
        }
    }
    s.work_cv.notify_all();
}

// Under s.m: every download stops (closing, or the player gave up on this stream).
void cancel_all(HttpStream& s) {
    for (auto& [k, c] : s.chunks) c->cancel = true;
}

void sleep_unless_cancelled(const Chunk& c, int ms) {
    for (int waited = 0; waited < ms && !c.cancel; waited += 50)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

std::string status_error(long status) {
    return status == 401 || status == 403 ? util::fmt(tr("Access denied (HTTP %ld)"), status)
           : status == 404                ? std::string(tr("Not found (HTTP 404)"))
           : status == 200                ? std::string(tr("The server doesn't support partial downloads"))
                                          : util::fmt(tr("HTTP error %ld"), status);
}

// Fills one chunk, streaming it in so the reader can use the first bytes at once. A dropped
// connection resumes where it stopped; a refusal (403: an expired or IP-locked link) fails.
void download(HttpStream& s, const std::shared_ptr<Chunk>& c) {
    {
        std::lock_guard<std::mutex> lk(s.m);
        c->data.resize((size_t)c->length);
    }
    const double t0 = util::now_seconds();

    for (int attempt = 1;; attempt++) {
        int64_t from, to;
        {
            std::lock_guard<std::mutex> lk(s.m);
            from = c->start + c->filled;
            to = c->start + c->length - 1;
        }
        long bad_status = 0;  // an answer we stopped reading
        bool checked = false;
        http::Request req;
        req.url = s.url;
        req.headers = s.headers;
        req.headers.emplace_back("Range", util::fmt("bytes=%lld-%lld", (long long)from, (long long)to));
        req.timeout = 30;
        req.cancel = &c->cancel;
        req.big_buffers = true;
        req.on_data = [&](const http::Response& r, const char* data, size_t n) {
            if (c->cancel || aborted(s)) return false;
            std::lock_guard<std::mutex> lk(s.m);
            if (!checked) {
                checked = true;
                int64_t first = -1, total = -1;
                if (r.status == 206 && content_range(r, first, total) && first == from) {
                    set_size(s, total);
                } else if (r.status == 200 && from == 0) {
                    set_size(s, header_int(r, "content-length"));  // the whole file: fine if it fits
                } else {
                    bad_status = r.status == 206 ? 502 : r.status;  // 206 for the wrong range
                    return false;
                }
            }
            int64_t take = std::min<int64_t>((int64_t)n, c->length - c->filled);
            if (take <= 0) return false;  // more than we asked for
            std::memcpy(c->data.data() + c->filled, data, (size_t)take);
            c->filled += take;
            s.downloaded += take;
            s.data_cv.notify_all();
            return true;
        };
        http::Response r = http::perform(req);
        if (c->cancel || aborted(s)) return;

        const long status = bad_status ? bad_status : r.status;
        std::unique_lock<std::mutex> lk(s.m);
        const bool short_file = r.error.empty() && status == 200 && from == 0;  // no ranges, file under a chunk
        if (c->filled >= c->length || short_file) {
            if (short_file) {
                c->length = c->filled;
                set_size(s, c->filled);
            }
            c->done = true;
            s.data_cv.notify_all();
            const double now = util::now_seconds();
            if (c->start == 0 && !s.first_logged) {
                s.first_logged = true;
                log_message(LOG_OK, "Player", "First %lld KB of %lld KB in %.1f s (%.0f KB/s)",
                            (long long)c->filled / 1024, (long long)s.size / 1024, now - t0,
                            c->filled / 1024.0 / std::max(now - t0, 0.001));
            }
            // Measured over the first read-ahead, while nothing holds the downloads back.
            if (s.downloaded >= std::min(SPEED_SAMPLE, s.ahead * CHUNK) && !s.speed_logged) {
                s.speed_logged = true;
                log_message(LOG_OK, "Player", "Downloading at %.0f KB/s (%d connection%s)",
                            s.downloaded / 1024.0 / std::max(now - s.opened_at, 0.001), s.connections,
                            s.connections == 1 ? "" : "s");
            }
            return;
        }
        const std::string error = bad_status || r.error.empty() ? status_error(status) : r.error;
        const bool fatal = (status >= 400 && status < 500 && status != 408 && status != 429) || status == 200;
        if (fatal || attempt >= ATTEMPTS) {
            c->failed = true;
            c->error = error;
            s.data_cv.notify_all();
            return;
        }
        lk.unlock();
        log_message(LOG_WARNING, "Player", "Download at %lld KB stopped (%s), retrying", (long long)from / 1024,
                    error.c_str());
        sleep_unless_cancelled(*c, 500 << (attempt - 1));
        if (c->cancel || aborted(s)) return;
    }
}

void worker(HttpStream* s) {
    for (;;) {
        std::shared_ptr<Chunk> c;
        {
            std::unique_lock<std::mutex> lk(s->m);
            s->work_cv.wait(lk, [&] { return s->stop || (c = next_wanted(*s)) != nullptr; });
            if (s->stop) return;
        }
        download(*s, c);
    }
}

int read_packet(void* opaque, uint8_t* buf, int size) {
    auto* s = (HttpStream*)opaque;
    std::unique_lock<std::mutex> lk(s->m);
    const int64_t k = s->pos / CHUNK;
    move_cursor(*s, k);
    for (;;) {
        if (aborted(*s)) {
            cancel_all(*s);
            return AVERROR_EXIT;
        }
        if (s->size >= 0 && s->pos >= s->size) return AVERROR_EOF;
        auto it = s->chunks.find(k);
        if (it != s->chunks.end()) {
            const Chunk& c = *it->second;
            const int64_t off = s->pos - c.start;
            if (c.filled > off) {
                int n = (int)std::min<int64_t>(size, c.filled - off);
                std::memcpy(buf, c.data.data() + off, (size_t)n);
                s->pos += n;
                return n;
            }
            if (c.failed) {
                log_message(LOG_ERROR, "Player", "Download stopped: %s", c.error.c_str());
                return AVERROR(EIO);
            }
            if (c.done) return AVERROR_EOF;  // the file ended inside this chunk
        }
        s->data_cv.wait_for(lk, std::chrono::milliseconds(100));
    }
}

int64_t seek(void* opaque, int64_t offset, int whence) {
    auto* s = (HttpStream*)opaque;
    int64_t size;
    {
        std::lock_guard<std::mutex> lk(s->m);
        size = s->size;
    }
    if (whence & AVSEEK_SIZE) return size >= 0 ? size : AVERROR(ENOSYS);
    whence &= ~AVSEEK_FORCE;
    int64_t pos = whence == SEEK_SET                 ? offset
                  : whence == SEEK_CUR               ? s->pos + offset
                  : whence == SEEK_END && size >= 0 ? size + offset
                                                     : -1;
    if (pos < 0) return AVERROR(EINVAL);
    s->pos = pos;  // the window follows on the next read
    return pos;
}

void close_stream(HttpStream* s) {
    {
        std::lock_guard<std::mutex> lk(s->m);
        s->stop = true;
        cancel_all(*s);
    }
    s->work_cv.notify_all();
    for (auto& t : s->workers) t.join();
    delete s;
}

}  // namespace

AVIOContext* http_io_open(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                          const std::atomic<bool>* abort, bool audio_track, std::string& error) {
    auto* s = new HttpStream();
    s->url = url;
    s->headers = headers;
    s->abort = abort;
    s->ahead = audio_track ? AHEAD_AUDIO : AHEAD;
    s->opened_at = util::now_seconds();
    s->connections = audio_track ? CONNECTIONS_AUDIO : CONNECTIONS;
    for (int i = 0; i < s->connections; i++) s->workers.emplace_back(worker, s);

    // Wait for the first answer: it tells the size, or why the file can't be had.
    {
        std::unique_lock<std::mutex> lk(s->m);
        for (;;) {
            auto it = s->chunks.find(0);
            if (it != s->chunks.end() && it->second->failed) {
                error = it->second->error;
                break;
            }
            if (s->size >= 0 || (it != s->chunks.end() && it->second->done)) break;
            if (aborted(*s)) {
                error = "Cancelled";
                break;
            }
            s->data_cv.wait_for(lk, std::chrono::milliseconds(100));
        }
    }
    if (!error.empty()) {
        close_stream(s);
        return nullptr;
    }

    uint8_t* buf = (uint8_t*)av_malloc(BUFFER_SIZE);
    AVIOContext* pb = buf ? avio_alloc_context(buf, BUFFER_SIZE, 0, s, read_packet, nullptr, seek) : nullptr;
    if (!pb) {
        av_free(buf);
        close_stream(s);
        error = tr("Out of memory");
        return nullptr;
    }
    return pb;  // owns s until http_io_free()
}

void http_io_free(AVIOContext* pb) {
    if (!pb || pb->read_packet != read_packet) return;
    close_stream((HttpStream*)pb->opaque);
    av_freep(&pb->buffer);  // FFmpeg may have replaced the original buffer
    avio_context_free(&pb);
}

}  // namespace player
