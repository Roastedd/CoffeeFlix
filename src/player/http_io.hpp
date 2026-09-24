// FFmpeg I/O for http(s) files through libcurl, in ranged chunks: what YouTube's own
// players do. FFmpeg's HTTP client asks for the whole file in one request, which
// googlevideo.com never delivers to the Wii U (HLS segments, being small, are fine).
#pragma once

#include <atomic>
#include <string>
#include <utility>
#include <vector>

struct AVIOContext;

namespace player {

// Fetches the first chunk before returning, so a refused URL (HTTP 403, an expired
// link) fails here with `error` set. Returns a context to set as AVFormatContext::pb
// (with AVFMT_FLAG_CUSTOM_IO). Reads fail with AVERROR_EXIT once *abort is set.
AVIOContext* http_io_open(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                          const std::atomic<bool>* abort, std::string& error);
// Frees a context from http_io_open(); anything else (and null) is ignored.
void http_io_free(AVIOContext* pb);

}  // namespace player
