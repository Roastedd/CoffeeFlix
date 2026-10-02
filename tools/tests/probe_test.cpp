// What the probe says about real files, made with ffmpeg: codec, size, rotation, verdict, and the
// cache on disk (a second run reads it back without opening the files).
//   make -f desktop.mk -f tools/tests/probe.mk BUILD=<dir> probe-tests && <dir>/probe-test <empty dir>
#include <sys/stat.h>
#include <utime.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "core/util.hpp"
#include "platform/platform.hpp"
#include "player/probe.hpp"

using player::Verdict;

namespace {

std::string g_dir, g_ffmpeg = "/opt/homebrew/bin/ffmpeg";
int g_failed = 0;

struct Fixture {
    const char* file;
    const char* args;  // ffmpeg arguments, "" for files written by hand
    Verdict verdict;
    const char* reason;  // English
    const char* vcodec;
    int w, h, rotation;
    double duration;  // 0: don't check
};

const std::vector<Fixture>& fixtures() {
    static const std::vector<Fixture> list = {
        {"ready_720p.mp4",
         "-f lavfi -i testsrc2=size=1280x720:rate=30:duration=2 -f lavfi -i sine=duration=2 "
         "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -shortest",
         Verdict::READY, "", "h264", 1280, 720, 0, 2.0},
        {"limited_1080p60.mp4",
         "-f lavfi -i testsrc2=size=1920x1080:rate=60:duration=1 -c:v libx264 -preset ultrafast -pix_fmt yuv420p",
         Verdict::LIMITED, "1080p at 60 fps shows about 45 pictures a second", "h264", 1920, 1080, 0, 1.0},
        {"ready_720p60.mp4",
         "-f lavfi -i testsrc2=size=1280x720:rate=60:duration=1 -c:v libx264 -preset ultrafast -pix_fmt yuv420p",
         Verdict::READY, "", "h264", 1280, 720, 0, 0},
        {"convert_hevc.mp4",
         "-f lavfi -i testsrc2=size=640x360:rate=25:duration=1 -c:v libx265 -preset ultrafast -pix_fmt yuv420p "
         "-x265-params log-level=error",
         Verdict::CONVERT, "HEVC (H.265) video needs conversion", "hevc", 640, 360, 0, 0},
        {"convert_vp9.webm",
         "-f lavfi -i testsrc2=size=640x360:rate=25:duration=1 -c:v libvpx-vp9 -deadline realtime -cpu-used 8",
         Verdict::CONVERT, "Video format needs conversion", "vp9", 640, 360, 0, 0},
        {"convert_mpeg4_720p.avi",
         "-f lavfi -i testsrc2=size=1280x720:rate=25:duration=1 -c:v mpeg4",
         Verdict::CONVERT, "Video format needs conversion", "mpeg4", 1280, 720, 0, 0},
        {"limited_mpeg4_sd.avi",
         "-f lavfi -i testsrc2=size=640x480:rate=25:duration=1 -f lavfi -i sine=duration=1 -c:v mpeg4 -c:a libmp3lame -shortest",
         Verdict::LIMITED, "Plays with software decoding, may be slow", "mpeg4", 640, 480, 0, 0},
        {"limited_mpeg2_dvd.mpg",
         "-f lavfi -i testsrc2=size=720x576:rate=25:duration=1 -f lavfi -i sine=duration=1 -c:v mpeg2video -c:a mp2 -shortest",
         Verdict::LIMITED, "Plays with software decoding, may be slow", "mpeg2video", 720, 576, 0, 0},
        {"convert_10bit.mp4",
         "-f lavfi -i testsrc2=size=640x360:rate=25:duration=1 -c:v libx264 -preset ultrafast -pix_fmt yuv420p10le",
         Verdict::CONVERT, "10-bit video needs conversion", "h264", 640, 360, 0, 0},
        {"convert_422.mp4",
         "-f lavfi -i testsrc2=size=640x360:rate=25:duration=1 -c:v libx264 -preset ultrafast -pix_fmt yuv422p",
         Verdict::CONVERT, "10-bit video needs conversion", "h264", 640, 360, 0, 0},
        {"convert_1440p.mp4",
         "-f lavfi -i testsrc2=size=2560x1440:rate=25:duration=0.2 -c:v libx264 -preset ultrafast -pix_fmt yuv420p",
         Verdict::CONVERT, "Larger than 1080p, needs conversion", "h264", 2560, 1440, 0, 0},
        {"convert_hdr.mp4",
         "-f lavfi -i testsrc2=size=640x360:rate=25:duration=1 "
         "-vf setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc "
         "-c:v libx264 -preset ultrafast -pix_fmt yuv420p",
         Verdict::CONVERT, "HDR video needs conversion", "h264", 640, 360, 0, 0},
        // HDR comes before the codec when both apply.
        {"convert_hdr_hevc.mp4",
         "-f lavfi -i testsrc2=size=640x360:rate=25:duration=1 "
         "-vf setparams=color_primaries=bt2020:color_trc=arib-std-b67:colorspace=bt2020nc "
         "-c:v libx265 -preset ultrafast -pix_fmt yuv420p10le -x265-params log-level=error",
         Verdict::CONVERT, "HDR video needs conversion", "hevc", 640, 360, 0, 0},
        // Portrait as coded: its long side is 1920, like a 1080p video's.
        {"ready_portrait_coded.mp4",
         "-f lavfi -i testsrc2=size=1080x1920:rate=30:duration=0.3 -c:v libx264 -preset ultrafast -pix_fmt yuv420p",
         Verdict::READY, "", "h264", 1080, 1920, 0, 0},
        // A phone video held upright: stored landscape, turned a quarter clockwise to show.
        {"ready_rotated_cw.mp4",
         "-display_rotation -90 -i {dir}/ready_720p.mp4 -c copy",
         Verdict::READY, "", "h264", 1280, 720, 1, 0},
        {"ready_rotated_ccw.mp4",
         "-display_rotation 90 -i {dir}/ready_720p.mp4 -c copy",
         Verdict::READY, "", "h264", 1280, 720, 3, 0},
        {"ready_rotated_180.mp4",
         "-display_rotation 180 -i {dir}/ready_720p.mp4 -c copy",
         Verdict::READY, "", "h264", 1280, 720, 2, 0},
        {"limited_no_sound.mkv",
         "-f lavfi -i testsrc2=size=640x360:rate=25:duration=1 -f lavfi -i sine=duration=1 "
         "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a wmav2 -shortest",
         Verdict::LIMITED, "No sound: unsupported audio", "h264", 640, 360, 0, 0},
        {"ready_ac3.mkv",
         "-f lavfi -i testsrc2=size=640x360:rate=25:duration=1 -f lavfi -i sine=duration=1 "
         "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a ac3 -shortest",
         Verdict::READY, "", "h264", 640, 360, 0, 0},
        {"ready_song.mp3", "-f lavfi -i sine=duration=3 -c:a libmp3lame", Verdict::READY, "", "", 0, 0, 0, 3.0},
        // Cover art isn't video.
        {"ready_song_cover.mp3",
         "-f lavfi -i sine=duration=1 -f lavfi -i color=red:size=300x300:duration=0.04 -map 0 -map 1 -c:a libmp3lame "
         "-c:v mjpeg -frames:v 1 -disposition:v attached_pic -id3v2_version 3",
         Verdict::READY, "", "", 0, 0, 0, 0},
        {"convert_song.wma", "-f lavfi -i sine=duration=1 -c:a wmav2", Verdict::CONVERT, "Audio format needs conversion", "", 0,
         0, 0, 0},
        {"unknown_garbage.mp4", "", Verdict::UNKNOWN, "Couldn't check this file", "", 0, 0, 0, 0},
        {"unknown_missing.mp4", "", Verdict::UNKNOWN, "Couldn't check this file", "", 0, 0, 0, 0},
    };
    return list;
}

std::string path_of(const Fixture& f) { return g_dir + "/" + f.file; }

void make_fixtures() {
    for (const Fixture& f : fixtures()) {
        std::string path = path_of(f);
        if (std::string(f.file) == "unknown_garbage.mp4") {
            std::string junk;
            for (int i = 0; i < 70000; i++) junk += (char)((i * 7919 + 13) & 0xFF);
            assert(util::write_file_atomic(path, junk));
            continue;
        }
        if (!*f.args) continue;
        std::string args = f.args;
        for (size_t p; (p = args.find("{dir}")) != std::string::npos;) args.replace(p, 5, g_dir);
        std::string cmd = g_ffmpeg + " -y -hide_banner -loglevel error " + args + " '" + path + "'";
        if (std::system(cmd.c_str()) != 0) {
            std::cerr << "ffmpeg failed: " << cmd << "\n";
            std::exit(2);
        }
    }
}

#define CHECK(cond, f, what)                                                                  \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            std::cerr << "FAIL " << (f).file << ": " << what << "\n";                        \
            g_failed++;                                                                       \
        }                                                                                     \
    } while (0)

void check(const Fixture& f, const player::MediaInfo& m) {
    CHECK(m.verdict == f.verdict, f, "verdict " << player::verdict_label(m.verdict) << " (" << m.reason << ")");
    CHECK(m.reason == f.reason, f, "reason '" << m.reason << "'");
    CHECK(m.video_codec == f.vcodec, f, "video codec '" << m.video_codec << "'");
    if (*f.vcodec) {
        CHECK(m.width == f.w && m.height == f.h, f, "size " << m.width << "x" << m.height);
        CHECK(m.rotation == f.rotation, f, "rotation " << m.rotation);
    }
    if (f.duration > 0) CHECK(std::fabs(m.duration - f.duration) < 0.2, f, "duration " << m.duration);
}

// First run: every file read, the results saved.
int probe_all() {
    make_fixtures();
    for (const Fixture& f : fixtures()) {
        double t0 = util::now_seconds();
        player::MediaInfo m = player::probe_media(path_of(f));
        double took = util::now_seconds() - t0;
        check(f, m);
        std::cout << "  " << f.file << ": " << player::verdict_label(m.verdict) << (m.reason.empty() ? "" : " - ")
                  << m.reason << " [" << m.video_codec << " " << m.width << "x" << m.height << " r" << m.rotation << " "
                  << m.fps << "fps " << m.bit_depth << "bit " << m.audio_codec << " x" << m.audio_tracks << " "
                  << m.duration << "s, " << (int)(took * 1000) << " ms]\n";
        player::MediaInfo again;
        CHECK(player::probe_cached(path_of(f), again) && again.verdict == m.verdict, f, "not kept in memory");
    }
    // Labels for the cards and Info.
    assert(std::string(player::verdict_label(Verdict::READY)) == "Ready to play");
    assert(std::string(player::verdict_label(Verdict::LIMITED)) == "Plays with limits");
    assert(std::string(player::verdict_label(Verdict::CONVERT)) == "Needs conversion");
    assert(std::string(player::verdict_label(Verdict::UNKNOWN)) == "Not checked");
    assert(player::codec_label("h264") == "H.264" && player::codec_label("hevc") == "HEVC (H.265)");
    assert(player::codec_label("pcm_s16le") == "PCM" && player::codec_label("vp9") == "VP9");
    player::flush_probe_cache();
    assert(util::file_exists(platform::data_dir() + "/media_info.json"));
    return 0;
}

// Second run, a new process: known from the file on disk, until a file changes.
int from_disk() {
    for (const Fixture& f : fixtures()) {
        player::MediaInfo m;
        bool known = player::probe_cached(path_of(f), m);
        CHECK(known, f, "not in the cache on disk");
        if (known) check(f, m);
    }
    // Changed since (another file sent with the same name): read again.
    std::string changed = g_dir + "/ready_720p.mp4";
    struct utimbuf times = {1000000000, 1000000000};
    assert(utime(changed.c_str(), &times) == 0);
    player::MediaInfo m;
    assert(!player::probe_cached(changed, m));
    m = player::probe_media(changed);
    assert(m.verdict == Verdict::READY);
    assert(player::probe_cached(changed, m));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: probe-test <empty folder> [--from-disk]\n";
        return 2;
    }
    g_dir = argv[1];
    if (const char* f = getenv("FFMPEG")) g_ffmpeg = f;
    util::make_dirs(g_dir);
    setenv("COFFEEFLIX_DATA", (g_dir + "/data").c_str(), 1);
    platform::init();
    bool second = argc > 2 && std::string(argv[2]) == "--from-disk";
    if (!second) {
        std::cout << "probing:\n";
        probe_all();
        if (g_failed == 0) {
            // The cache as a fresh start sees it.
            std::string cmd = std::string("'") + argv[0] + "' '" + g_dir + "' --from-disk";
            if (std::system(cmd.c_str()) != 0) g_failed++;
        }
    } else {
        from_disk();
        std::cout << (g_failed ? "cache on disk: FAILED\n" : "cache on disk: ok\n");
    }
    if (!second) std::cout << (g_failed ? "probe tests FAILED\n" : "probe tests passed\n");
    return g_failed ? 1 : 0;
}
