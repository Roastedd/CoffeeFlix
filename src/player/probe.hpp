// What's in a media file and whether the Wii U plays it well, for file browsers (and the same
// rules as the browser upload page in content/transfer.html). Results are kept in memory and in
// a small file in platform::data_dir(), by path, size and modification time, so a folder listed
// again is known at once.
#pragma once

#include <string>

namespace player {

enum class Verdict { UNKNOWN, READY, LIMITED, CONVERT };

struct MediaInfo {
    bool ok = false;           // the file could be opened and read
    double duration = 0;       // seconds, 0 when unknown
    std::string video_codec;   // FFmpeg's names ("h264", "hevc"); empty when there is no video
    std::string audio_codec;   // the sound track played ("aac"), else the first one; empty: none
    int width = 0, height = 0; // as coded (see rotation)
    float fps = 0;             // 0 when unknown
    int bit_depth = 8;
    bool hdr = false;          // PQ or HLG transfer
    int rotation = 0;          // quarter turns clockwise that show the picture upright
    Verdict verdict = Verdict::UNKNOWN;
    std::string reason;        // short, in the current language; empty when READY

    // The rest of what the verdict is made from.
    bool chroma_420 = true;        // 4:2:0 colour
    bool pro_profile = false;      // H.264 High 10, 4:2:2 or 4:4:4 profile
    int audio_tracks = 0;          // sound tracks in the file
    bool audio_supported = false;  // at least one of them can be decoded
};

// Blocking (opens the file): call it from the task pool.
MediaInfo probe_media(const std::string& local_path);
// The result without opening the file, when it's already known (the file unchanged since).
// Still stats the file: not on the main thread either.
bool probe_cached(const std::string& local_path, MediaInfo& out);
// Writes what was learnt since the last time to the cache file, if anything. Blocking; probes
// write it by themselves every few dozen files.
void flush_probe_cache();

// Fills verdict and reason from the facts above.
void judge(MediaInfo& m);
// "Ready to play", "Plays with limits", "Needs conversion", "Not checked".
const char* verdict_label(Verdict v);
// A codec's usual name: "h264" -> "H.264", "hevc" -> "HEVC (H.265)", "aac" -> "AAC".
std::string codec_label(const std::string& ffmpeg_name);

}  // namespace player
