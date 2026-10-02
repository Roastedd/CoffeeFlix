// FFmpeg I/O for http(s) files through libcurl, in ranged chunks: what YouTube's own
// players do. FFmpeg's HTTP client asks for the whole file in one request, which
// googlevideo.com never delivers to the Wii U (HLS segments, being small, are fine). Media
// servers' files come this way too: downloads kept ahead ride out a slow spell.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct AVIOContext;

namespace player {

// Downloads ahead of the reader on background connections. Waits for the first answer
// before returning, so a refused URL (HTTP 403, an expired link) fails here with `error`
// set. Returns a context to set as AVFormatContext::pb (with AVFMT_FLAG_CUSTOM_IO). Reads
// fail with AVERROR_EXIT once *abort is set. `audio_track`: a separate audio stream, given
// a smaller share of the bandwidth. `no_ranges` (optional) is set when it failed because the
// server sends only whole files: FFmpeg's own HTTP client can still play those.
AVIOContext* http_io_open(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                          const std::atomic<bool>* abort, bool audio_track, std::string& error,
                          bool* no_ranges = nullptr);
// Frees a context from http_io_open(); anything else (and null) is ignored.
void http_io_free(AVIOContext* pb);
// Demux-thread only: discard bytes suspected of corruption so a retry fetches fresh data.
void http_io_discard(AVIOContext* pb);
// Bytes all downloads received so far, and for how long at least one was running: a read-ahead
// that is full isn't the network being slow. For Auto quality.
void http_io_meter(int64_t& bytes, double& busy_seconds);
// Downloads at once for a video file opened from now on (a developer setting; 0: the default).
void http_io_set_connections(int n);
// Lets a context from http_io_open() keep `bytes` downloaded ahead of the reader, when that's more
// than it keeps (up to http_io_max_ahead()): a long wait for the network needs room for what it
// waits for. It keeps that until it's freed.
void http_io_keep_ahead(AVIOContext* pb, int64_t bytes);
int64_t http_io_max_ahead();
// Confirmed queued media at the playback clock, excluding speculative downloaded bytes.
// Zero during startup, seeking or starvation; feedback expires if updates stop.
void http_io_playback_buffer(AVIOContext* pb, double seconds);
// Recent conservative capacity for this exact URL authority, in bits/s. Zero: unknown.
// In-memory only, expires after 30 minutes; never inferred from a different host.
int http_io_host_budget(const std::string& url);
// Pure retry policy, also exercised by the transport test.
double http_io_quiet_limit(bool connecting, bool urgent, bool others, bool retried, double buffered);
// How many 256 KB chunks in a row one request covers (1 to 8), for what a request on an open
// connection takes: `wait` seconds for the first byte, then `rate` bytes/s (0: not measured). `distance`:
// chunks between the reader and the first of them. `healthy`: the server hasn't been saying it's busy.
// A request pays its wait once however much it asks for. Pure, also exercised by the transport test.
int http_io_span_chunks(double wait, double rate, int64_t distance, bool healthy);

// What a busy server's answer (503, 429) does: true, every download waits and one connection is
// left until the host has recovered; false, only the download that was refused waits. For the tests.
bool http_io_back_off_all();

// The heartbeat's words about all downloads (core/heartbeat.hpp), from its thread only.
std::string http_io_report();

// What a context from http_io_open() did so far, for the log.
struct HttpIoStats {
    int64_t size = -1;       // of the file (-1: not known yet)
    int64_t downloaded = 0;  // bytes
    int64_t ahead_limit = 0; // configured download window, for diagnostics
    int64_t ready = 0;       // bytes in from the reader's position on, without a gap
    bool full = false;       // all it keeps ahead is in: nothing more comes until the reader reads on
    int connections = 0;     // downloads at once
    double busy_seconds = 0, first_byte_seconds = 0;
    int completed_requests = 0;
    int requests = 0;
    int connects = 0;        // new connections the requests needed
    int retries = 0;         // requests that failed and were made again
    int quiet_restarts = 0;  // requests made again because their connection went quiet
    int request_kb = 256;    // what a request far ahead of the reader covers now
};
// False for a context that isn't one of http_io_open()'s (or null).
bool http_io_stats(const AVIOContext* pb, HttpIoStats& out);

}  // namespace player
