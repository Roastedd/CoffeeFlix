// FFmpeg I/O for smb:// URLs (FFmpeg has no SMB protocol): an AVIOContext
// backed by a libsmb2 file handle with its own connection.
#pragma once

#include <atomic>
#include <string>

struct AVFormatContext;
struct AVIOContext;

namespace player {

// Returns a context to set as AVFormatContext::pb (with AVFMT_FLAG_CUSTOM_IO),
// or nullptr with `error` set. Reads fail with AVERROR_EXIT once *abort is set.
AVIOContext* smb_io_open(const std::string& url, const std::atomic<bool>* abort, std::string& error);
// Frees a context from smb_io_open(); anything else (and null) is ignored.
void smb_io_free(AVIOContext* pb);
// avformat_close_input() that also frees smb_io custom I/O.
void close_input(AVFormatContext** fmt);

}  // namespace player
