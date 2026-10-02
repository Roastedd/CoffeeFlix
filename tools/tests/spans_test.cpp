// Ranged downloads covering several chunks per request, and a busy server's answer read out:
// the reader gets every byte, whatever the requests cover, and the connection stays good.
// Driven by tools/tests/spans.py (argv: base URL of its fixture server).
#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
extern "C" {
#include <libavformat/avio.h>
}
#include "core/http.hpp"
#include "player/http_io.hpp"

namespace {

constexpr int64_t CHUNK = 256 << 10;

// The fixture's file: byte i is i % 251, so a chunk put in the wrong place shows.
bool read_checked(AVIOContext* io, int64_t from, int64_t count) {
    unsigned char bytes[32768];
    int64_t pos = from;
    while (pos < from + count) {
        const int n = avio_read(io, bytes, (int)std::min<int64_t>(sizeof(bytes), from + count - pos));
        if (n <= 0) {
            std::cerr << "read ended at " << pos << " of " << from + count << " (" << n << ")" << std::endl;
            return false;
        }
        for (int j = 0; j < n; ++j)
            if (bytes[j] != (pos + j) % 251) {
                std::cerr << "wrong byte at " << pos + j << std::endl;
                return false;
            }
        pos += n;
    }
    return true;
}

double seconds_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

AVIOContext* open(const std::string& url, std::atomic<bool>& stop) {
    std::string error;
    auto* io = player::http_io_open(url, {}, &stop, false, error);
    if (!io) std::cerr << "open failed: " << error << std::endl;
    assert(io);
    return io;
}

}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string base = argv[1];

    // The policy: a wait of 0.75 s and 300 KB/s on a connection asks for 5 chunks at a time (the wait
    // a fifth of it) once the reader is far enough off; nothing known, a busy server, or a link where
    // the wait hardly matters asks for one; never more than 8.
    assert(player::http_io_span_chunks(0.75, 300 << 10, 100, true) == 5);
    assert(player::http_io_span_chunks(0.75, 300 << 10, 0, true) == 1);
    assert(player::http_io_span_chunks(0.75, 300 << 10, 4, true) == 3);
    assert(player::http_io_span_chunks(0.75, 300 << 10, 100, false) == 1);
    assert(player::http_io_span_chunks(0, 300 << 10, 100, true) == 1);
    assert(player::http_io_span_chunks(0.75, 0, 100, true) == 1);
    assert(player::http_io_span_chunks(0.3, 50 << 10, 100, true) == 1);
    assert(player::http_io_span_chunks(2.0, 5 << 20, 1000, true) == 8);
    assert(player::http_io_span_chunks(0.001, 100 << 20, 100, true) >= 1);

    http::init("content/cacert.pem");
    std::atomic<bool> stop{false};

    // A slow first byte (0.15 s) and 3 MB/s on each connection: requests for more chunks at once make it
    // up. All 16 MB arrive, in order, from fewer requests than there are chunks.
    {
        const auto began = std::chrono::steady_clock::now();
        auto* io = open(base + "/span.bin", stop);
        assert(read_checked(io, 0, 16 << 20));
        player::HttpIoStats stats;
        assert(player::http_io_stats(io, stats));
        std::printf("span.bin: 16 MB in %.1f s, %d requests, %d new connections, requests up to %d KB\n", seconds_since(began),
                    stats.requests, stats.connects, stats.request_kb);
        assert(stats.requests < 40 && stats.request_kb > 256);
        player::http_io_free(io);
    }

    // Jumping around while requests for several chunks are out: every read is right, none hangs.
    {
        auto* io = open(base + "/span.bin", stop);
        assert(read_checked(io, 0, 3 << 20));
        const auto began = std::chrono::steady_clock::now();
        assert(avio_seek(io, 12 << 20, SEEK_SET) == (12 << 20));
        assert(read_checked(io, 12 << 20, 1 << 20));
        assert(avio_seek(io, 5 * CHUNK + 1000, SEEK_SET) == 5 * CHUNK + 1000);
        assert(read_checked(io, 5 * CHUNK + 1000, 2 << 20));
        assert(avio_seek(io, (16 << 20) - 1000, SEEK_SET) == (16 << 20) - 1000);
        assert(read_checked(io, (16 << 20) - 1000, 1000));
        assert(avio_read(io, (unsigned char[16]){}, 16) <= 0);  // the end
        assert(seconds_since(began) < 30);
        player::http_io_free(io);
    }

    // Closing with requests out ends them: no waiting for what was asked.
    {
        auto* io = open(base + "/span.bin", stop);
        assert(read_checked(io, 0, 2 << 20));
        const auto began = std::chrono::steady_clock::now();
        player::http_io_free(io);
        std::printf("closed in %.2f s\n", seconds_since(began));
        assert(seconds_since(began) < 3);
    }

    // A connection cut in the middle of a request for several chunks: what came in is kept, the
    // rest is asked for again from where it stopped.
    {
        auto* io = open(base + "/drop.bin", stop);
        assert(read_checked(io, 0, 16 << 20));
        player::HttpIoStats stats;
        assert(player::http_io_stats(io, stats));
        std::printf("drop.bin: %d requests, %d failed\n", stats.requests, stats.retries);
        assert(stats.retries >= 1);
        player::http_io_free(io);
    }

    // A busy server (503 with a body): the file still arrives whole, each refusal retried. With
    // http_io_back_off_all() the answer is read out too and the retry goes on the same connection
    // (the fixture records the ports; spans.py looks).
    {
        auto* io = open(base + "/busy.bin", stop);
        assert(read_checked(io, 0, 4 << 20));
        player::HttpIoStats stats;
        assert(player::http_io_stats(io, stats));
        std::printf("busy.bin: %d requests, %d failed, %d new connections\n", stats.requests, stats.retries, stats.connects);
        std::printf("back_off_all: %d\n", player::http_io_back_off_all() ? 1 : 0);
        assert(stats.retries >= 3);
        player::http_io_free(io);
    }
    std::puts("Spans test passed");
    http::shutdown();
    return 0;
}
