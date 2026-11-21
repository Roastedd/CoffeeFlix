// FFmpeg I/O for Spotify's files: downloaded like any file over http (http_io), decrypted as they're read
// (AES-128 in counter mode), and without the header Spotify puts in front of the Ogg stream.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct AVIOContext;

namespace player {

// Returns a context to set as AVFormatContext::pb (with AVFMT_FLAG_CUSTOM_IO), or nullptr with `error` set.
AVIOContext* spotify_io_open(const std::string& url, const std::vector<uint8_t>& key, const std::atomic<bool>* abort,
                             std::string& error);
// Frees a context from spotify_io_open(); anything else (and null) is ignored.
void spotify_io_free(AVIOContext* pb);

}  // namespace player
