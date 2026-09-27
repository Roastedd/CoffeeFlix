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

#include "core/cpu.hpp"
#include "core/http.hpp"
#include "core/i18n.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"

namespace player {

namespace {

// The file is fetched as 256 KB ranges, three at a time and ahead of the reader. googlevideo paces
// each connection to 150-250 KB/s after a first burst, whatever the format's bitrate (measured
// September 2026 on a 22 MB/s line: one connection 180 KB/s, three 420-640, six 500-1330), and
// fetching only when FFmpeg asks leaves the demuxer idle for as long as a range takes: the video
// runs dry. Small ranges: the one the reader waits for (starting, after a seek) comes in a few
// times sooner than 1 MB ones did. Not more at once: a Wii U's Wi-Fi got no more with six (about
// 370 KB/s either way), and each download, getting a sixth of it, went quiet for QUIET_SECONDS
// twenty times as often (a new connection each time): 13 stops in 12 minutes against 2 in 37.
constexpr int BUFFER_SIZE = 64 << 10;
constexpr int64_t CHUNK = 256 << 10;
// Chunks kept downloaded ahead of the reader: 16 MB, 40 s of 720p60 and a minute of most 1080p30,
// to ride out the Wi-Fi's slow spells.
constexpr int64_t AHEAD = 64;
// The most http_io_keep_ahead() lets a file keep ahead, in bytes: 48 MB on the Wii U, two minutes
// of 720p60 and over a minute of 1080p60.
int64_t max_ahead() { return platform::is_wiiu() ? 48 << 20 : 256 << 20; }
constexpr int CONNECTIONS = 3;
std::atomic<int> g_connections{0};  // http_io_set_connections()
// A separate audio track is a tenth of the video's rate or less: 1 MB is over a minute of it, and
// it shouldn't take bandwidth from the video.
constexpr int64_t AHEAD_AUDIO = 4;
constexpr int CONNECTIONS_AUDIO = 1;
// Chunks kept behind the reader: demuxers step back a little (to a fragment's header after
// reading on into its first picture, say).
constexpr int64_t BEHIND = 4;
constexpr int ATTEMPTS = 4;
constexpr long STALL_SECONDS = 6;
// googlevideo answers a request on an open connection within 0.7 s, and its pacing pauses an
// answer for at most 1.6 s (measured: 600 requests on three connections). On the Wii U a request
// now and then got no answer, or its answer stopped, while the other connections went on; the
// reader reads in order, so the picture waited for STALL_SECONDS and a retry: 6-11 s. A request
// quiet this long while other downloads got bytes is made again at once, on a new connection:
constexpr double QUIET_SECONDS = 2.0;        // what the reader needs next (within URGENT chunks)
constexpr double QUIET_SECONDS_AHEAD = 3.0;  // further ahead
constexpr int64_t URGENT = 8;                // 2 MB: 5 s of 720p60, 3 s of 1080p60
// Twice that when nothing else came in either (the Wi-Fi itself may be pausing), and twice as
// long again once the same chunk needed it before. Connecting (a TLS handshake on the Wii U) gets
// this, once per chunk (then curl's connect timeout ends it, as a failure):
constexpr double CONNECT_SECONDS = 5.0;
constexpr int64_t SPEED_SAMPLE = 4 << 20;

struct Chunk {
    int64_t start = 0;
    int64_t length = CHUNK;  // less at the end of the file, or from where the reader jumped to
    std::vector<char> data;  // valid up to `filled`
    int64_t filled = 0;
    bool done = false, failed = false;
    std::string error;
    std::atomic<bool> cancel{false};  // evicted, or the stream is closing
    double asked_at = 0;     // when the current request started (0: none running)
    double data_at = 0;      // when bytes last came in for it (0: none yet)
    int quiet = 0;           // times it was asked for again for going quiet
    bool went_quiet = false;  // the current request was given up on for that
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
    int64_t reader_at = 0;  // `pos` as of the last read
    // The reader didn't come to `cursor` from the chunk before (opening, a seek, a demuxer
    // looking something up): only its chunk is fetched until it's in or the reader reads on,
    // and from where the reader is rather than from the chunk's start. The connections share
    // the bandwidth, so reading ahead of a spot that may be left again at once would slow down
    // the bytes wanted now.
    bool jumped = true;
    int64_t size = -1;   // from the first response
    bool whole = false;  // the first answer was the whole file (200): the server ignores ranges
    // Where `url`'s redirects led, asked directly from then on: podcasts' go through a chain of
    // trackers (up to 8 servers), each a new connection and TLS handshake on the Wii U. Given up
    // (`url` again) when it gets refused: a link that expires, or is good for one request only.
    std::string direct;
    bool no_direct = false;
    bool stop = false;
    std::vector<std::thread> workers;

    double opened_at = 0;
    int64_t downloaded = 0;
    bool first_logged = false, speed_logged = false;
    // For the player's log (http_io_stats).
    int requests = 0, connects = 0, retries = 0, quiet_restarts = 0;
};

bool aborted(const HttpStream& s) { return s.abort && s.abort->load(); }

// What downloads get while running, for http_io_meter().
struct Meter {
    std::mutex m;
    int running = 0;
    double busy_since = 0, busy = 0;
    int64_t bytes = 0;
    double data_at = 0;  // when any download last got bytes
};
Meter g_meter;

void meter_running(bool start) {
    std::lock_guard<std::mutex> lk(g_meter.m);
    double t = util::now_seconds();
    if (start && g_meter.running++ == 0) g_meter.busy_since = t;
    if (!start && --g_meter.running == 0) g_meter.busy += t - g_meter.busy_since;
}

void meter_add(int64_t n, double now) {
    std::lock_guard<std::mutex> lk(g_meter.m);
    g_meter.bytes += n;
    g_meter.data_at = now;
}

double meter_data_at() {
    std::lock_guard<std::mutex> lk(g_meter.m);
    return g_meter.data_at;
}

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

// Bytes of chunk `k` from `start` on, before the file's end when its size is known.
int64_t length_from(const HttpStream& s, int64_t k, int64_t start) {
    int64_t end = (k + 1) * CHUNK;
    if (s.size >= 0) end = std::min(end, s.size);
    return std::max<int64_t>(0, end - start);
}

// Under s.m: the size became known, so the last chunk is shorter than the rest.
void set_size(HttpStream& s, int64_t size) {
    if (s.size >= 0 || size < 0) return;
    s.size = size;
    for (auto& [k, c] : s.chunks) c->length = length_from(s, k, c->start);
    s.work_cv.notify_all();
}

// Under s.m: the first chunk in the reader's window that nobody fetches yet (made on the
// spot), or null. Until the size is known only the first chunk is wanted: it tells the size.
std::shared_ptr<Chunk> next_wanted(HttpStream& s) {
    auto here = s.chunks.find(s.cursor);
    const bool settled = !s.jumped || (here != s.chunks.end() && here->second->done);
    for (int64_t k = s.cursor; k < s.cursor + (settled ? s.ahead : 1); k++) {
        if (s.chunks.count(k)) continue;
        if (s.size < 0 && k > 0) return nullptr;
        if (s.size >= 0 && k * CHUNK >= s.size) return nullptr;
        auto c = std::make_shared<Chunk>();
        c->start = k == s.cursor ? std::max(k * CHUNK, s.reader_at) : k * CHUNK;
        c->length = length_from(s, k, c->start);
        s.chunks[k] = c;
        return c;
    }
    return nullptr;
}

// Under s.m: the reader moved to chunk `k`. Drops what fell out of the window (BEHIND and
// ahead of it) and wakes the workers for what came in. After a seek (further than a demuxer
// steps back) to a chunk that isn't in, the downloads beyond URGENT make way for it too: they
// would keep the connections busy for seconds (a chunk takes 2 s or more on a Wii U's share of
// the pacing), and for longer when one went quiet.
void move_cursor(HttpStream& s, int64_t k) {
    if (k == s.cursor) return;
    const bool far = k < s.cursor - BEHIND || k > s.cursor + 1;
    s.jumped = k != s.cursor + 1;
    s.cursor = k;
    auto here = s.chunks.find(k);
    const bool seek = far && (here == s.chunks.end() || !here->second->done);
    for (auto it = s.chunks.begin(); it != s.chunks.end();) {
        if (it->first < k - BEHIND || it->first >= k + s.ahead ||
            (seek && it->first >= k + URGENT && !it->second->done)) {
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

// Under s.m: whether the request for chunk c went quiet for long enough to give up on it (see
// QUIET_SECONDS). `sent_at`: when it went out (0: still connecting). Logs why when it did.
bool gone_quiet(HttpStream& s, const Chunk& c, double sent_at, double now) {
    if (sent_at <= 0 && c.quiet > 0) return false;
    const bool urgent = c.start / CHUNK < s.cursor + URGENT;
    const double since = c.data_at > 0 ? c.data_at : sent_at > 0 ? sent_at : c.asked_at;
    const bool others = now - meter_data_at() < 0.5 && now - since > 0.5;
    double limit = sent_at <= 0 ? CONNECT_SECONDS : urgent ? QUIET_SECONDS : QUIET_SECONDS_AHEAD;
    if (sent_at > 0 && !others) limit *= 2;
    if (c.quiet > 0) limit *= 2;
    if (now - since < limit) return false;
    const long long at = (long long)(c.start + c.filled) / 1024;
    const char* why = others ? " while others got bytes" : "";
    if (sent_at <= 0)
        log_message(LOG_WARNING, "Player", "Download at %lld KB still connecting after %.1f s%s: again", at, now - since, why);
    else if (c.data_at <= 0)
        log_message(LOG_WARNING, "Player", "Download at %lld KB: no answer %.1f s after asking%s: again", at, now - since, why);
    else
        log_message(LOG_WARNING, "Player", "Download at %lld KB quiet for %.1f s%s: again", at, now - since, why);
    return true;
}

void sleep_unless_cancelled(const Chunk& c, int ms) {
    for (int waited = 0; waited < ms && !c.cancel; waited += 50)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// "host.example" of "https://host.example/path?signed": for the log, without the link's keys.
std::string host_of(const std::string& url) {
    size_t from = url.find("://");
    from = from == std::string::npos ? 0 : from + 3;
    return url.substr(from, url.find_first_of("/?#", from) - from);
}

std::string status_error(long status) {
    return status == 401 || status == 403 ? util::fmt(tr("Access denied (HTTP %ld)"), status)
           : status == 404                ? std::string(tr("Not found (HTTP 404)"))
           : status == 200                ? std::string(tr("The server doesn't support partial downloads"))
                                          : util::fmt(tr("HTTP error %ld"), status);
}

// Fills one chunk, streaming it in so the reader can use the first bytes at once. A dropped
// connection resumes where it stopped; a refusal (403: an expired or IP-locked link) fails.
// `conn`: the worker's own connection, kept from one chunk to the next.
void download(HttpStream& s, const std::shared_ptr<Chunk>& c, http::Connection& conn) {
    {
        std::lock_guard<std::mutex> lk(s.m);
        c->data.resize((size_t)c->length);
    }
    const double t0 = util::now_seconds();
    bool fresh = false;  // on a new connection

    for (int failures = 0;;) {
        int64_t from, to;
        {
            std::lock_guard<std::mutex> lk(s.m);
            from = c->start + c->filled;
            to = c->start + c->length - 1;
            c->asked_at = util::now_seconds();
            c->data_at = 0;
            s.requests++;
        }
        long bad_status = 0;  // an answer we stopped reading
        bool checked = false;
        http::Request req;
        {
            std::lock_guard<std::mutex> lk(s.m);
            req.url = s.direct.empty() ? s.url : s.direct;
        }
        const bool via_direct = req.url != s.url;
        req.headers = s.headers;
        req.headers.emplace_back("Range", util::fmt("bytes=%lld-%lld", (long long)from, (long long)to));
        req.timeout = 120;  // a slow connection that keeps going is fine: STALL_SECONDS is for dead ones
        req.stall_seconds = STALL_SECONDS;
        req.cancel = &c->cancel;
        req.keep_going = [&](double sent_at) {
            std::lock_guard<std::mutex> lk(s.m);
            if (!c->went_quiet && gone_quiet(s, *c, sent_at, util::now_seconds())) c->went_quiet = true;
            return !c->went_quiet;
        };
        req.connection = &conn;
        req.fresh_connection = fresh;
        fresh = false;
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
                    s.whole = true;
                } else {
                    bad_status = r.status == 206 ? 502 : r.status;  // 206 for the wrong range
                    return false;
                }
            }
            int64_t take = std::min<int64_t>((int64_t)n, c->length - c->filled);
            if (take <= 0) return false;  // more than we asked for
            std::memcpy(c->data.data() + c->filled, data, (size_t)take);
            c->filled += take;
            c->data_at = util::now_seconds();
            s.downloaded += take;
            meter_add(take, c->data_at);
            s.data_cv.notify_all();
            return true;
        };
        meter_running(true);
        http::Response r = http::perform(req);
        meter_running(false);
        if (c->cancel || aborted(s)) return;

        const long status = bad_status ? bad_status : r.status;
        std::unique_lock<std::mutex> lk(s.m);
        c->asked_at = 0;
        s.connects += (int)r.connects;
        if (!via_direct && !s.no_direct && (status == 206 || status == 200) && !r.effective_url.empty() &&
            r.effective_url != s.url && s.direct.empty()) {
            s.direct = r.effective_url;
            log_message(LOG_OK, "Player", "Redirected to %s: asking there directly", host_of(s.direct).c_str());
        }
        const bool short_file = r.error.empty() && status == 200 && from == 0;  // no ranges, file under a chunk
        if (c->filled >= c->length || short_file) {
            if (short_file) {
                c->length = c->filled;
                set_size(s, c->filled);
            }
            c->done = true;
            s.data_cv.notify_all();
            s.work_cv.notify_all();  // the reader's chunk after a jump: the read-ahead can go on
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
        if (c->went_quiet) {
            // Again at once, on a new connection.
            fresh = true;
            c->went_quiet = false;
            c->quiet++;
            s.quiet_restarts++;
            continue;
        }
        const std::string error = bad_status || r.error.empty() ? status_error(status) : r.error;
        const bool fatal = (status >= 400 && status < 500 && status != 408 && status != 429) || status == 200;
        const bool progressed = c->start + c->filled > from;  // it got somewhere before it stopped
        if (via_direct && !progressed && !s.no_direct) {
            s.no_direct = true;
            s.direct.clear();
            lk.unlock();
            log_message(LOG_WARNING, "Player", "The address it was redirected to failed (%s): asking the first one again",
                        error.c_str());
            continue;
        }
        if (progressed) failures = 0;
        failures++;
        if (fatal || failures >= ATTEMPTS) {
            c->failed = true;
            c->error = error;
            s.data_cv.notify_all();
            return;
        }
        s.retries++;
        lk.unlock();
        log_message(LOG_WARNING, "Player", "Download at %lld KB stopped (%s), retrying", (long long)from / 1024,
                    error.c_str());
        // A connection that was working and stopped is replaced at once; one that got nothing
        // (refused, an error from the server) is asked again after a pause.
        if (!progressed) sleep_unless_cancelled(*c, 500 << (failures - 1));
        if (c->cancel || aborted(s)) return;
    }
}

void worker(HttpStream* s) {
    cpu::ThreadTag tag("download");
    http::Connection conn;
    for (;;) {
        std::shared_ptr<Chunk> c;
        {
            std::unique_lock<std::mutex> lk(s->m);
            s->work_cv.wait(lk, [&] { return s->stop || (c = next_wanted(*s)) != nullptr; });
            if (s->stop) return;
        }
        download(*s, c, conn);
    }
}

int read_packet(void* opaque, uint8_t* buf, int size) {
    auto* s = (HttpStream*)opaque;
    std::unique_lock<std::mutex> lk(s->m);
    const int64_t k = s->pos / CHUNK;
    s->reader_at = s->pos;
    move_cursor(*s, k);
    for (;;) {
        if (aborted(*s)) {
            cancel_all(*s);
            return AVERROR_EXIT;
        }
        if (s->size >= 0 && s->pos >= s->size) return AVERROR_EOF;
        auto it = s->chunks.find(k);
        if (it != s->chunks.end() && s->pos < it->second->start) {
            // Fetched from where the reader jumped to, and it went back before that: again
            // from here.
            it->second->cancel = true;
            s->chunks.erase(it);
            s->jumped = true;
            s->work_cv.notify_all();
            it = s->chunks.end();
        }
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
                          const std::atomic<bool>* abort, bool audio_track, std::string& error, bool* no_ranges) {
    auto* s = new HttpStream();
    s->url = url;
    s->headers = headers;
    s->abort = abort;
    s->ahead = audio_track ? AHEAD_AUDIO : AHEAD;
    s->opened_at = util::now_seconds();
    s->connections = audio_track ? CONNECTIONS_AUDIO : g_connections > 0 ? (int)g_connections : CONNECTIONS;
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
        // All of a file bigger than a chunk, from a server that doesn't send parts: the chunks
        // after the first would fail.
        if (error.empty() && s->whole && (s->size < 0 || s->size > CHUNK)) {
            error = status_error(200);
            if (no_ranges) *no_ranges = true;
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

bool http_io_stats(const AVIOContext* pb, HttpIoStats& out) {
    if (!pb || pb->read_packet != read_packet) return false;
    auto* s = (HttpStream*)pb->opaque;
    std::lock_guard<std::mutex> lk(s->m);
    out.size = s->size;
    out.downloaded = s->downloaded;
    out.connections = s->connections;
    out.requests = s->requests;
    out.connects = s->connects;
    out.retries = s->retries;
    out.quiet_restarts = s->quiet_restarts;
    int64_t at = s->reader_at;
    for (int64_t k = at / CHUNK;; k++) {
        auto it = s->chunks.find(k);
        if (it == s->chunks.end() || at < it->second->start) break;
        const Chunk& c = *it->second;
        at = std::max(at, c.start + c.filled);
        if (!c.done) break;
    }
    out.ready = at - s->reader_at;
    out.full = s->size >= 0 && (at >= s->size || at >= (s->cursor + s->ahead) * CHUNK);
    return true;
}

void http_io_meter(int64_t& bytes, double& busy_seconds) {
    std::lock_guard<std::mutex> lk(g_meter.m);
    bytes = g_meter.bytes;
    busy_seconds = g_meter.busy + (g_meter.running > 0 ? util::now_seconds() - g_meter.busy_since : 0);
}

void http_io_set_connections(int n) { g_connections = std::clamp(n, 0, 8); }

int64_t http_io_max_ahead() { return max_ahead(); }

void http_io_keep_ahead(AVIOContext* pb, int64_t bytes) {
    if (!pb || pb->read_packet != read_packet) return;
    auto* s = (HttpStream*)pb->opaque;
    std::lock_guard<std::mutex> lk(s->m);
    const int64_t chunks = std::clamp((bytes + CHUNK - 1) / CHUNK, s->ahead, max_ahead() / CHUNK);
    if (chunks == s->ahead) return;
    log_message(LOG_OK, "Player", "Downloads keep %lld MB ahead now (were %lld MB)", (long long)(chunks * CHUNK >> 20),
                (long long)(s->ahead * CHUNK >> 20));
    s->ahead = chunks;
    s->work_cv.notify_all();
}

}  // namespace player
