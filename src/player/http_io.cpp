#include "player/http_io.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "core/cpu.hpp"
#include "core/heartbeat.hpp"
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
constexpr long STALL_SECONDS = 20; // outer low-speed guard; buffer-aware quiet policy recovers sooner
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
// A request for a chunk waits for the server's first byte (0.5-0.8 s measured on a Wii U, on a
// connection already open) before it moves any: half of what a 256 KB request took there. One
// request for several chunks in a row pays that wait once (http_io_span_chunks()).
constexpr int SPAN_MAX = 8;  // 2 MB
// After the server said it was busy, the log gets a line for each request and a pulse while one
// waits, for this long (see trace()): what the network was doing when the console froze.
constexpr double TRACE_SECONDS = 30.0;
// ... and the heartbeat (core/heartbeat.hpp) for this long: a line a second that says whether the
// whole app stopped or only the main loop.
constexpr double HEARTBEAT_SECONDS = 60.0;
// Requests for several chunks wait this long after a busy answer.
constexpr double BUSY_SPAN_HOLD = 10.0;
// What a busy server's answer (503, 429) does. false: only the download that was refused waits (for
// its Retry-After, else 0.5 s and up) and the others go on: how the build of Sep 28 handled it, which
// took 59 of them and played on. true: every download waits, one connection is left until 16 chunks
// have come in, and the refusal's body is read out so its connection serves the retry (the
// development builds of Sep 29-30). Every run of those that got one (6 of 6, logs of Sep 29-30)
// froze the whole console within 3 s of it; build .3 is to tell whether this is why.
constexpr bool BACK_OFF_ALL = false;

struct Chunk {
    int64_t start = 0;
    int64_t length = CHUNK;  // less at the end of the file, or from where the reader jumped to
    std::vector<char> data;  // valid up to `filled`
    int64_t filled = 0;
    bool done = false, failed = false;
    bool fatal = false;      // failed for good (the server refused: an expired link), not for trouble on the way
    double failed_at = 0;
    std::string error;
    std::atomic<bool> cancel{false};  // evicted, or the stream is closing
    double asked_at = 0;     // when the current request started (0: none running)
    double data_at = 0;      // when bytes last came in for it (0: none yet)
    int quiet = 0;           // times it was asked for again for going quiet
};

// The chunks one request covers, in order: a request is made for all of them, and each is done
// as its bytes have come in.
using Span = std::vector<std::shared_ptr<Chunk>>;

struct HttpStream {
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    const std::atomic<bool>* abort = nullptr;
    int64_t pos = 0;  // the reader's (FFmpeg's thread only)
    int64_t ahead = AHEAD;
    int connections = CONNECTIONS;
    bool audio_track = false;
    int parallel_limit = CONNECTIONS, recovered_chunks = 0;
    double cooldown_until = 0;

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
    double playback_buffer = 0, feedback_at = 0, data_at = 0;
    double busy_seconds = 0, busy_since = 0, first_byte_seconds = 0;
    int running = 0, completed_requests = 0;
    int64_t sample_bytes = 0;
    double sample_busy = 0;
    int64_t downloaded = 0;
    bool first_logged = false, speed_logged = false;
    // For the player's log (http_io_stats).
    int requests = 0, connects = 0, retries = 0, quiet_restarts = 0;
    // What a request on an open connection takes: the wait for its first byte (seconds) and then
    // one connection's speed (bytes/s), averaged over the last ones (0: not measured yet); and the
    // chunks the next far-ahead request covers as a result.
    double wait_avg = 0, rate_avg = 0;
    int span = 1;
    double trace_until = 0, trace_at = 0;
    double busy_until = 0;  // a busy answer came: no larger requests until then
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

// The heartbeat's words about the downloads (core/heartbeat.hpp), from its thread only: how many are
// running, what came in since its last line, and how long since any bytes did. `try_to_lock`: a
// meter that stays locked is worth a line of its own.
std::string downloads_report() {
    static int64_t bytes_before = -1;
    static double at_before = 0;
    std::unique_lock<std::mutex> lk(g_meter.m, std::try_to_lock);
    if (!lk.owns_lock()) return "downloads: their counters are locked";
    const double now = util::now_seconds();
    std::string out = util::fmt("downloads: %d running", g_meter.running);
    if (bytes_before >= 0 && now - at_before < 30)
        out += util::fmt(", %.0f KB/s in", (g_meter.bytes - bytes_before) / 1024.0 / std::max(now - at_before, 0.001));
    out += g_meter.data_at > 0 ? util::fmt(", last bytes %.1f s ago", now - g_meter.data_at) : std::string(", no bytes yet");
    bytes_before = g_meter.bytes;
    at_before = now;
    return out;
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

// Under s.m: chunk `k`, made on the spot (from where the reader is, when that's in it).
std::shared_ptr<Chunk> make_chunk(HttpStream& s, int64_t k) {
    auto c = std::make_shared<Chunk>();
    c->start = k == s.cursor ? std::max(k * CHUNK, s.reader_at) : k * CHUNK;
    c->length = length_from(s, k, c->start);
    s.chunks[k] = c;
    return c;
}

// Under s.m: the reader's chunk is in (or it came to it from the one before): the window is open.
bool settled(const HttpStream& s) {
    auto here = s.chunks.find(s.cursor);
    return !s.jumped || (here != s.chunks.end() && here->second->done);
}

// Under s.m: the first chunk in the reader's window that nobody fetches yet (made on the
// spot), or null. Until the size is known only the first chunk is wanted: it tells the size.
std::shared_ptr<Chunk> next_wanted(HttpStream& s) {
    for (int64_t k = s.cursor; k < s.cursor + (settled(s) ? s.ahead : 1); k++) {
        if (s.chunks.count(k)) continue;
        if (s.size < 0 && k > 0) return nullptr;
        if (s.size >= 0 && k * CHUNK >= s.size) return nullptr;
        return make_chunk(s, k);
    }
    return nullptr;
}

// Under s.m: what the next request covers: the first chunk nobody fetches yet and, when a request
// for more of them in a row makes up for its wait (http_io_span_chunks()), the ones after it.
// Empty when there's nothing to fetch. They are all made on the spot, so nobody else takes them.
Span next_span(HttpStream& s) {
    Span span;
    auto first = next_wanted(s);
    if (!first) return span;
    span.push_back(first);
    if (s.size < 0 || !settled(s)) return span;
    const int64_t k0 = first->start / CHUNK;
    const int want = http_io_span_chunks(s.wait_avg, s.rate_avg, k0 - s.cursor, s.parallel_limit >= s.connections && util::now_seconds() >= s.busy_until && !s.audio_track);
    for (int64_t k = k0 + 1; (int)span.size() < want && k < s.cursor + s.ahead && k * CHUNK < s.size && !s.chunks.count(k); k++)
        span.push_back(make_chunk(s, k));
    return span;
}

// Under s.m: the reader moved to chunk `k`. Drops what fell out of the window (BEHIND and
// ahead of it) and wakes the workers for what came in. After a seek (further than a demuxer
// steps back) to a chunk that isn't in, the downloads beyond URGENT make way for it too: they
// would keep the connections busy for seconds (a chunk takes 2 s or more on a Wii U's share of
// the pacing), and for longer when one went quiet.
void move_cursor(HttpStream& s, int64_t k) {
    if (k == s.cursor) return;
    const bool far = k < s.cursor - BEHIND || k > s.cursor + 1;
    if (far) s.playback_buffer = s.feedback_at = 0;
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
    const bool others = now - s.data_at < 0.5 && now - since > 0.5;
    const double buffered = now - s.feedback_at < 1.5 ? s.playback_buffer : 0;
    const double limit = http_io_quiet_limit(sent_at <= 0, urgent, others, c.quiet > 0, buffered);
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

struct HostCapacity { double rate = 0, measured = 0; };
std::mutex host_mutex;
std::map<std::string, HostCapacity> host_capacity;
void record_capacity(const std::string& url, double rate) {
    const auto host = host_of(url);
    if (host.empty() || host.find('@') != std::string::npos) return;
    std::lock_guard<std::mutex> lk(host_mutex);
    const double now = util::now_seconds();
    for (auto it = host_capacity.begin(); it != host_capacity.end();)
        if (now - it->second.measured > 1800) it = host_capacity.erase(it); else ++it;
    if (host_capacity.size() >= 32 && !host_capacity.count(host)) host_capacity.erase(host_capacity.begin());
    auto& p = host_capacity[host];
    // Respond promptly to slowdowns, gradually to apparent improvements.
    p.rate = p.rate <= 0 ? rate : std::min(rate, p.rate * 0.8 + rate * 0.2);
    p.measured = now;
}

std::string status_error(long status) {
    return status == 401 || status == 403 ? util::fmt(tr("Access denied (HTTP %ld)"), status)
           : status == 404                ? std::string(tr("Not found (HTTP 404)"))
           : status == 200                ? std::string(tr("The server doesn't support partial downloads"))
                                          : util::fmt(tr("HTTP error %ld"), status);
}

// Under s.m. After overload reduces concurrency, the next bytes required by the
// reader take precedence over already-assigned lookahead chunks.
bool earlier_pending(const HttpStream& s, const Chunk& chunk) {
    if (s.parallel_limit != 1) return false;
    for (const auto& entry : s.chunks) {
        const auto& other = *entry.second;
        if (entry.first >= s.cursor && other.start < chunk.start && !other.done && !other.failed && !other.cancel)
            return true;
    }
    return false;
}

// Under s.m: chunk c came in whole: the reader can count on it, and a host that had been
// overloaded gets a connection back after a stretch of them.
void finish_chunk(HttpStream& s, Chunk& c) {
    if (c.done) return;
    c.done = true;
    c.asked_at = 0;
    if (s.parallel_limit < s.connections && ++s.recovered_chunks >= 16) {
        ++s.parallel_limit;
        s.recovered_chunks = 0;
        log_message(LOG_OK, "Player", "Host recovered: allowing %d download connections", s.parallel_limit);
    }
    s.data_cv.notify_all();
    s.work_cv.notify_all();  // the reader's chunk after a jump: the read-ahead can go on
}

// Under s.m: finishes the chunks of the span from `at` on that came in whole; the index of the
// first that didn't (the span's size: all did).
size_t finish_whole(HttpStream& s, const Span& span, size_t at) {
    while (at < span.size() && span[at]->filled >= span[at]->length) finish_chunk(s, *span[at++]);
    return at;
}

// Under s.m: a line for the log while the trace is on (see TRACE_SECONDS); at most `every` seconds
// apart when that's asked (a pulse: the same line over and over shows the network went on, or
// where it stopped).
bool tracing(HttpStream& s, double now, double every = 0) {
    if (now >= s.trace_until || now - s.trace_at < every) return false;
    if (every > 0) s.trace_at = now;
    return true;
}

// Fills the chunks of a span, streaming them in so the reader can use the first bytes at once:
// one request for all of them, then each is done as it comes in whole. A dropped connection
// resumes where it stopped; a refusal (403: an expired or IP-locked link) fails the chunk the
// reader would have needed first. `conn`: the worker's own connection, kept from one request to
// the next.
void fetch(HttpStream& s, const Span& span, http::Connection& conn) {
    const double t0 = util::now_seconds();
    bool fresh = false;  // on a new connection
    size_t at = 0;       // the chunk being filled (under s.m); the ones before it are done

    for (int failures = 0;;) {
        int64_t from, to;
        {
            std::lock_guard<std::mutex> lk(s.m);
            at = finish_whole(s, span, at);
            if (at >= span.size()) return;
            Chunk& cur = *span[at];
            if (cur.data.size() < (size_t)cur.length) cur.data.resize((size_t)cur.length);
            from = cur.start + cur.filled;
            to = span.back()->start + span.back()->length - 1;
            cur.data_at = 0;
        }
        long bad_status = 0;  // an answer we stopped reading
        int64_t drain = 0;    // of a refusal's body (a busy server's) still to read: a connection with it unread can't be reused
        int64_t received = 0;  // bytes this request brought
        bool checked = false, went_quiet = false;
        http::Request req;
        {
            std::lock_guard<std::mutex> lk(s.m);
            req.url = s.direct.empty() ? s.url : s.direct;
        }
        const bool via_direct = req.url != s.url;
        req.private_url = true;  // signed media URLs and provider credentials stay out of logs
        req.headers = s.headers;
        req.headers.emplace_back("Range", util::fmt("bytes=%lld-%lld", (long long)from, (long long)to));
        req.timeout = 120;  // a slow connection that keeps going is fine: STALL_SECONDS is for dead ones
        req.stall_seconds = STALL_SECONDS;
        // Not `req.cancel`: that is one chunk's, and the request goes on when a later one of the span is dropped.
        req.keep_going = [&](double sent_at) {
            std::lock_guard<std::mutex> lk(s.m);
            Chunk& cur = *span[at];
            const double now = util::now_seconds();
            if (cur.cancel || s.stop) return false;
            if (!went_quiet && gone_quiet(s, cur, sent_at, now)) went_quiet = true;
            if (tracing(s, now, 1.0))
                log_message(LOG_DEBUG, "Net", "Request for %lld KB: %s, %lld KB in, %d running", (long long)(from / 1024),
                            sent_at <= 0 ? "connecting" : util::fmt("sent %.1f s ago", now - sent_at).c_str(),
                            (long long)(received / 1024), s.running);
            return !went_quiet;
        };
        req.connection = &conn;
        req.fresh_connection = fresh;
        fresh = false;
        req.big_buffers = true;
        req.on_data = [&](const http::Response& r, const char* data, size_t n) {
            if (aborted(s)) return false;
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
                    // A busy server's answer is small: read it out, and the connection stays good for the retry.
                    drain = BACK_OFF_ALL && (r.status == 429 || r.status == 503) ? 64 << 10 : 0;
                }
            }
            if (bad_status) {
                drain -= (int64_t)n;
                return drain >= 0;
            }
            while (n > 0) {
                Chunk& cur = *span[at];
                if (cur.cancel) return false;
                const int64_t take = std::min<int64_t>((int64_t)n, cur.length - cur.filled);
                if (take <= 0) return false;  // more than we asked for
                if (cur.data.size() < (size_t)cur.length) cur.data.resize((size_t)cur.length);
                std::memcpy(cur.data.data() + cur.filled, data, (size_t)take);
                cur.filled += take;
                data += take;
                n -= (size_t)take;
                received += take;
                const double now = util::now_seconds();
                cur.data_at = now;
                s.data_at = now;
                s.downloaded += take;
                meter_add(take, now);
                if (cur.filled >= cur.length && at + 1 < span.size()) {
                    finish_chunk(s, cur);
                    Chunk& next = *span[++at];  // its bytes come next, on the same answer
                    next.asked_at = next.data_at = now;
                }
            }
            s.data_cv.notify_all();
            return true;
        };
        {
            std::unique_lock<std::mutex> lk(s.m);
            while (!s.stop && !span[at]->cancel && !aborted(s) &&
                   (util::now_seconds() < s.cooldown_until || s.running >= s.parallel_limit || earlier_pending(s, *span[at])))
                s.work_cv.wait_for(lk, std::chrono::milliseconds(100));
            if (s.stop || span[at]->cancel || aborted(s)) return;
            const double now = util::now_seconds();
            span[at]->asked_at = now;
            ++s.requests;
            if (s.running++ == 0) s.busy_since = now;
            if (tracing(s, now))
                log_message(LOG_DEBUG, "Net", "Asking for %lld-%lld KB (%d chunk%s), %d running, %s", (long long)(from / 1024),
                            (long long)(to / 1024), (int)(span.size() - at), span.size() - at == 1 ? "" : "s", s.running,
                            req.fresh_connection ? "on a new connection" : "on the worker's own connection");
        }
        meter_running(true);
        http::Response r = http::perform(req);
        meter_running(false);
        const long status = bad_status ? bad_status : r.status;
        std::unique_lock<std::mutex> lk(s.m);
        if (--s.running == 0) s.busy_seconds += util::now_seconds() - s.busy_since;
        const bool overloaded = status == 429 || status == 503;
        const double retry_after = overloaded ? http::retry_after(r, util::unix_time()) : -1;
        double busy_wait = 0;  // seconds this download waits before it asks again (the others go on)
        if (overloaded) {
            const double now = util::now_seconds();
            s.trace_until = now + TRACE_SECONDS;
            s.busy_until = now + BUSY_SPAN_HOLD;
            heartbeat::arm(HEARTBEAT_SECONDS, downloads_report);
            if (BACK_OFF_ALL) {
                const double delay = retry_after >= 0 ? retry_after : std::min(8.0, (double)(1 << std::min(failures, 3))) + (span[at]->start / CHUNK % 5) * 0.1;
                s.cooldown_until = std::max(s.cooldown_until, now + delay);
                s.parallel_limit = 1; s.recovered_chunks = 0;
                log_message(LOG_WARNING, "Player", "HTTP %ld: all range workers back off %.1f s; one connection until recovery", status, delay);
            } else {
                busy_wait = retry_after >= 0 ? retry_after : 0.5 * (1 << std::min(failures, 3));  // 0.5, 1, 2, 4 s
                log_message(LOG_WARNING, "Player", "HTTP %ld: this download waits %.1f s, the others go on", status, busy_wait);
            }
        }
        if (tracing(s, util::now_seconds()))
            log_message(LOG_DEBUG, "Net", "Answer %ld after %.2f s (first byte %.3f, %ld new connection%s), %lld KB came in: %s", status,
                        r.total_seconds, r.first_byte_seconds, r.connects, r.connects == 1 ? "" : "s", (long long)(received / 1024),
                        r.error.empty() ? "ok" : r.error.c_str());
        s.work_cv.notify_all();
        if (span[at]->cancel || s.stop || aborted(s)) return;
        span[at]->asked_at = 0;
        s.connects += (int)r.connects;
        const double busy = s.busy_seconds + (s.running ? util::now_seconds() - s.busy_since : 0);
        if (!s.audio_track && busy - s.sample_busy >= 8 && s.downloaded - s.sample_bytes >= (4 << 20)) {
            record_capacity(s.url, (s.downloaded - s.sample_bytes) / (busy - s.sample_busy));
            s.sample_busy = busy;
            s.sample_bytes = s.downloaded;
        }
        if (r.first_byte_seconds > 0) {
            s.first_byte_seconds += r.first_byte_seconds;
            ++s.completed_requests;
        }
        // What a request costs on a connection that was open (a new one's first byte includes the
        // handshake), and one connection's speed once bytes flow: for how many chunks to ask at once.
        if (r.error.empty() && received >= CHUNK / 2) {
            const double moving = r.total_seconds - r.first_byte_seconds;
            const auto average = [](double& avg, double sample) { avg = avg <= 0 ? sample : avg * 0.7 + sample * 0.3; };
            if (r.connects == 0 && r.first_byte_seconds > 0) average(s.wait_avg, r.first_byte_seconds);
            if (moving > 0.02) average(s.rate_avg, received / moving);
            const int span_now = http_io_span_chunks(s.wait_avg, s.rate_avg, SPAN_MAX * 2, !s.audio_track);
            if ((std::abs(span_now - s.span) >= 2 || (span_now == 1) != (s.span == 1)) && s.wait_avg > 0 && s.rate_avg > 0) {
                log_message(LOG_OK, "Player", "Requests now cover up to %d KB (first byte %.2f s, %.0f KB/s on a connection)",
                            span_now * (int)(CHUNK >> 10), s.wait_avg, s.rate_avg / 1024);
                s.span = span_now;
            }
        }
        if (!s.first_logged && span[0]->start == 0)
            log_message(LOG_OK, "Player", "Network timing: DNS %.3f, connect %.3f, TLS %.3f, first byte %.3f, total %.3f s (cumulative, %ld new connections)",
                        r.dns_seconds, r.connect_seconds, r.tls_seconds, r.first_byte_seconds, r.total_seconds, r.connects);
        if (!via_direct && !s.no_direct && (status == 206 || status == 200) && !r.effective_url.empty() &&
            r.effective_url != s.url && s.direct.empty()) {
            s.direct = r.effective_url;
            log_message(LOG_OK, "Player", "Redirected to %s: asking there directly", host_of(s.direct).c_str());
        }
        const bool short_file = r.error.empty() && status == 200 && from == 0;  // no ranges, file under a chunk
        if (short_file) {
            Chunk& first = *span[0];
            first.length = first.filled;
            set_size(s, first.filled);
        }
        at = finish_whole(s, span, at);
        if (at >= span.size()) {
            const double now = util::now_seconds();
            Chunk& first = *span[0];
            if (first.start == 0 && !s.first_logged) {
                s.first_logged = true;
                log_message(LOG_OK, "Player", "First %lld KB of %lld KB in %.1f s (%.0f KB/s)",
                            (long long)first.filled / 1024, (long long)s.size / 1024, now - t0,
                            first.filled / 1024.0 / std::max(now - t0, 0.001));
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
        Chunk& cur = *span[at];
        if (went_quiet) {
            // Again at once, on a new connection.
            fresh = true;
            cur.quiet++;
            s.quiet_restarts++;
            continue;
        }
        const std::string error = bad_status || r.error.empty() ? status_error(status) : r.error;
        const bool fatal = retry_after > 30 || (status >= 400 && status < 500 && status != 408 && status != 429) || status == 200;
        const bool progressed = received > 0;  // it got somewhere before it stopped
        if (via_direct && !progressed && !s.no_direct && !overloaded) {
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
            cur.failed = true;
            cur.fatal = fatal;
            cur.failed_at = util::now_seconds();
            cur.error = error;
            s.data_cv.notify_all();
            return;
        }
        s.retries++;
        lk.unlock();
        log_message(LOG_WARNING, "Player", "Download at %lld KB stopped (%s), retrying", (long long)from / 1024,
                    error.c_str());
        // A connection that was working and stopped is replaced at once; one that got nothing
        // (refused, an error from the server) is asked again after a pause.
        if (busy_wait > 0) sleep_unless_cancelled(cur, (int)(busy_wait * 1000));
        else if (!progressed && !overloaded) sleep_unless_cancelled(cur, 500 << (failures - 1));
        if (cur.cancel || aborted(s)) return;
    }
}

// Fills a span's chunks (see fetch()); what it leaves unfinished without a failure (it was cut
// short: one of the chunks was dropped, the stream is closing) is let go, for the next free
// download to ask for again: nobody else would fill those chunks, and the reader would wait.
void download(HttpStream& s, const Span& span, http::Connection& conn) {
    fetch(s, span, conn);
    std::lock_guard<std::mutex> lk(s.m);
    bool any = false;
    for (const auto& c : span) {
        if (c->done || c->failed) continue;
        auto it = s.chunks.find(c->start / CHUNK);
        if (it != s.chunks.end() && it->second == c) {
            s.chunks.erase(it);
            any = true;
        }
    }
    if (any) s.work_cv.notify_all();
}

void worker(HttpStream* s) {
    cpu::ThreadTag tag("download");
    http::Connection conn;
    for (;;) {
        Span span;
        {
            std::unique_lock<std::mutex> lk(s->m);
            s->work_cv.wait(lk, [&] { return s->stop || !(span = next_span(*s)).empty(); });
            if (s->stop) return;
        }
        download(*s, span, conn);
    }
}

// Under s.m: chunks that gave up on trouble on the way (Wi-Fi dropping, a busy server) are asked for
// again once it's had a moment: left failed they would be found dead by the reader when it got to
// them, half a minute later, though the link was good again. A refusal stays (fatal).
void forgive_failures(HttpStream& s, double now) {
    bool any = false;
    for (auto it = s.chunks.begin(); it != s.chunks.end();) {
        const Chunk& c = *it->second;
        if (c.failed && !c.fatal && now - c.failed_at >= 3.0) {
            it->second->cancel = true;
            it = s.chunks.erase(it);
            any = true;
        } else {
            ++it;
        }
    }
    if (any) s.work_cv.notify_all();
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
        forgive_failures(*s, util::now_seconds());
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
                // The reader gets the error once (the player waits, then seeks back to ask again); the
                // next read starts the chunk over, with a whole new set of attempts.
                if (!c.fatal) {
                    it->second->cancel = true;
                    s->chunks.erase(it);
                    s->jumped = true;
                }
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
    s->audio_track = audio_track;
    s->ahead = audio_track ? AHEAD_AUDIO : AHEAD;
    s->opened_at = util::now_seconds();
    s->connections = audio_track ? CONNECTIONS_AUDIO : g_connections > 0 ? (int)g_connections : CONNECTIONS;
    s->parallel_limit = s->connections;
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

void http_io_discard(AVIOContext* pb) {
    if (!pb || pb->read_packet != read_packet) return;
    auto* s = (HttpStream*)pb->opaque;
    std::lock_guard<std::mutex> lock(s->m);
    cancel_all(*s); s->chunks.clear();
    s->cursor = s->pos / CHUNK; s->reader_at = s->pos; s->jumped = true;
    s->playback_buffer = s->feedback_at = 0;
    s->work_cv.notify_all(); s->data_cv.notify_all();
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
    out.ahead_limit = s->ahead * CHUNK;
    out.downloaded = s->downloaded;
    out.connections = s->parallel_limit;
    out.busy_seconds = s->busy_seconds + (s->running ? util::now_seconds() - s->busy_since : 0);
    out.first_byte_seconds = s->first_byte_seconds;
    out.completed_requests = s->completed_requests;
    out.requests = s->requests;
    out.connects = s->connects;
    out.retries = s->retries;
    out.quiet_restarts = s->quiet_restarts;
    out.request_kb = s->span * (int)(CHUNK >> 10);
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

int http_io_host_budget(const std::string& url) {
    std::lock_guard<std::mutex> lk(host_mutex);
    auto it = host_capacity.find(host_of(url));
    if (it == host_capacity.end() || util::now_seconds() - it->second.measured > 1800) return 0;
    return (int)std::min(1000000000.0, it->second.rate * 8 * 0.7);
}

double http_io_quiet_limit(bool connecting, bool urgent, bool others, bool retried, double buffered) {
    if (connecting) return CONNECT_SECONDS;
    double limit = urgent ? QUIET_SECONDS : QUIET_SECONDS_AHEAD;
    if (!others) limit *= 2;
    if (retried) limit *= 2;
    // Leave at least half the confirmed buffer for recovery; never wait indefinitely.
    if (buffered >= 8) limit = std::max(limit, std::min(12.0, buffered * 0.5));
    return limit;
}

bool http_io_back_off_all() { return BACK_OFF_ALL; }

std::string http_io_report() { return downloads_report(); }

int http_io_span_chunks(double wait, double rate, int64_t distance, bool healthy) {
    if (!healthy || wait <= 0 || rate <= 0) return 1;
    // Long enough that the wait is a fifth of the request: `rate` for 4 x `wait` seconds (more
    // the slower the answer comes, more the faster the bytes flow).
    const double enough = std::ceil(5.0 * wait * rate / CHUNK);
    // What the reader needs next comes from a single connection, at its speed, while the others
    // fetch beyond it: short requests there, the ones for what it will want later long.
    const double near = 1 + std::max<int64_t>(distance, 0) / 2;
    return (int)std::clamp(std::min(enough, near), 1.0, (double)SPAN_MAX);
}

void http_io_playback_buffer(AVIOContext* pb, double seconds) {
    if (!pb || pb->read_packet != read_packet) return;
    auto* s = (HttpStream*)pb->opaque;
    std::lock_guard<std::mutex> lk(s->m);
    s->playback_buffer = std::max(0.0, seconds);
    s->feedback_at = util::now_seconds();
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
