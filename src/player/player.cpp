#include "player/player.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
}

#include <SDL2/SDL_image.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "audio/mixer.hpp"
#include "audio/tempo.hpp"
#include "core/cpu.hpp"
#include "core/http.hpp"
#include "core/i18n.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/gfx.hpp"
#include "gfx/images.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"
#include "player/convert.hpp"
#include "player/http_io.hpp"
#include "player/smb_io.hpp"
#include "player/subtitles.hpp"
#include "services/smb.hpp"

// The Wii U build uses FFmpeg-wiiu (FFmpeg 4.3). The desktop preview uses the system FFmpeg, which
// describes audio channels with AVChannelLayout from 5.1 on and dropped the old fields in 7.0.
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 24, 100)
#define HAVE_CH_LAYOUT 1
#endif
#ifndef AV_PROFILE_H264_HIGH_10  // FFmpeg < 6.1 only has the FF_ names, 8.0 only the AV_ ones
#define AV_PROFILE_H264_HIGH_10 FF_PROFILE_H264_HIGH_10
#define AV_PROFILE_H264_HIGH_422 FF_PROFILE_H264_HIGH_422
#define AV_PROFILE_H264_HIGH_444_PREDICTIVE FF_PROFILE_H264_HIGH_444_PREDICTIVE
#endif

namespace player {

namespace {

constexpr size_t VIDEO_QUEUE_BYTES_WIIU = 14u << 20;
constexpr size_t VIDEO_QUEUE_BYTES_DESKTOP = 48u << 20;
constexpr size_t AUDIO_QUEUE_BYTES = 3u << 20;
// Demuxed video kept ahead (unless the bytes above run out first): 8 s at 60 fps. The downloads
// keep more ahead of that (video_ready()); without them, more is demuxed (video_packet_limit()).
constexpr size_t VIDEO_QUEUE_PACKETS = 480;
constexpr double VIDEO_CUSHION = 3.0;  // seconds, more each time it ran dry (refill_target())
// Seconds of video from the network to have before a start or a seek plays: with just the first
// picture it ran out again a second later, and with 1.5 s it still did now and then on the Wii U
// (googlevideo's first burst is soon over), then waited for a whole cushion.
constexpr double START_CUSHION = 2.5;
// About the longest refill_target() waits (seconds on the clock) the first time the downloads
// can't keep up with the stream; twice that the second time, three times from then on.
constexpr double PLAY_THROUGH_WAIT = 20.0;
// The downloads keep what a wait is for and this much more (keep_ahead()): a stretch of the file
// can take more bytes a second than its average.
constexpr double KEEP_AHEAD_MARGIN = 1.25;

size_t video_queue_bytes() { return platform::is_wiiu() ? VIDEO_QUEUE_BYTES_WIIU : VIDEO_QUEUE_BYTES_DESKTOP; }

double now() { return util::now_seconds(); }

// --- packet queue --------------------------------------------------------------

struct QPacket {
    AVPacket* pkt;
    uint32_t gen;
};

struct PacketQueue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<QPacket> q;
    size_t bytes = 0;
    bool eof = false;
    double last_pts = -1;  // media time of the newest queued packet

    void push(AVPacket* p, uint32_t gen, double pts) {
        std::lock_guard<std::mutex> lk(m);
        q.push_back(QPacket{p, gen});
        bytes += p->size;
        eof = false;
        if (pts >= 0) last_pts = pts;
        cv.notify_one();
    }
    bool pop(QPacket& out, const std::atomic<bool>& abort, int timeout_ms) {
        std::unique_lock<std::mutex> lk(m);
        if (!cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return abort.load() || !q.empty(); }))
            return false;
        if (abort || q.empty()) return false;
        out = q.front();
        q.pop_front();
        bytes -= out.pkt->size;
        return true;
    }
    void flush() {
        std::lock_guard<std::mutex> lk(m);
        for (auto& p : q) av_packet_free(&p.pkt);
        q.clear();
        bytes = 0;
        eof = false;
        last_pts = -1;
    }
    void set_eof() {
        std::lock_guard<std::mutex> lk(m);
        eof = true;
        cv.notify_all();
    }
    size_t count() {
        std::lock_guard<std::mutex> lk(m);
        return q.size();
    }
    bool drained() {
        std::lock_guard<std::mutex> lk(m);
        return eof && q.empty();
    }
    // The first keyframe waiting of generation `gen`: false when there's none yet, else its pts and
    // whether `check` accepts it (under the lock: a seek frees the packets).
    template <class F>
    bool next_key(uint32_t gen, int64_t& pts, bool& ok, F check) {
        std::lock_guard<std::mutex> lk(m);
        for (auto& p : q) {
            if (p.gen != gen) return false;
            if (p.pkt->flags & AV_PKT_FLAG_KEY) {
                pts = p.pkt->pts;
                ok = pts != AV_NOPTS_VALUE && check(p.pkt);
                return true;
            }
        }
        return false;
    }
    void wake() { cv.notify_all(); }
};

// Seconds stored as 32-bit milliseconds: 64-bit atomics aren't lock-free on
// the Wii U's 32-bit PowerPC (and there is no libatomic).
struct AtomicSeconds {
    std::atomic<int32_t> ms{0};
    AtomicSeconds& operator=(double s) {
        ms.store((int32_t)std::lround(s * 1000.0));
        return *this;
    }
    operator double() const { return ms.load() / 1000.0; }
};

struct VFrame {
    double pts = 0;
    int w = 0, h = 0;
    uint32_t gen = 0;
    // 4:2:0 pictures keep the decoded frame and the GPU converts them (NV12 or IYUV
    // textures); anything else is converted to RGBA on the CPU.
    Uint32 format = SDL_PIXELFORMAT_RGBA32;
    SDL_YUV_CONVERSION_MODE yuv_mode = SDL_YUV_CONVERSION_BT601;
    AVFrame* yuv = av_frame_alloc();
    std::vector<uint8_t> rgba;

    VFrame() = default;
    VFrame(const VFrame&) = delete;
    VFrame& operator=(const VFrame&) = delete;
    ~VFrame() { av_frame_free(&yuv); }
};

// The texture format and conversion for a picture the renderer can take as it is.
// SDL has no full range BT.709, so those pictures stay on the CPU converter.
bool yuv_texture_format(const AVFrame* f, Uint32& format, SDL_YUV_CONVERSION_MODE& mode) {
    bool full = yuv_full_range(f), bt709 = yuv_bt709(f);
    if (full && bt709) return false;
    switch (f->format) {
        case AV_PIX_FMT_NV12: format = SDL_PIXELFORMAT_NV12; break;
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P: format = SDL_PIXELFORMAT_IYUV; break;
        default: return false;
    }
    mode = full ? SDL_YUV_CONVERSION_JPEG : bt709 ? SDL_YUV_CONVERSION_BT709 : SDL_YUV_CONVERSION_BT601;
    return true;
}

struct Input {
    AVFormatContext* fmt = nullptr;
    std::thread thread;
    std::atomic<bool> seek_req{false};
    std::atomic<bool> eof{false};
    int video = -1, audio = -1;
    bool network = false;  // http(s)
    bool chunked = false;  // through http_io, which keeps downloads ahead; else FFmpeg reads it as it goes
    bool remote = false;   // over the network: http(s) or SMB
};

struct Session {
    uint32_t id = 0;
    Source src;
    Source original;  // as requested, for retry()
    std::atomic<bool> abort{false};
    std::atomic<uint32_t> gen{0};
    std::atomic<int> state{OPENING};
    std::mutex err_m;
    std::string error;

    Input in[2];
    int inputs = 0;
    int audio_input = 0;
    int pref_audio = -1;

    AVCodecContext* vdec = nullptr;
    AVCodecContext* adec = nullptr;
    AVCodecContext* sdec = nullptr;
    AVRational vtb{1, 1}, atb{1, 1};
    bool hw = false;
    PacketQueue vq, aq;
    std::thread opener, vthread, athread;
    std::atomic<bool> video_eof{false}, audio_eof{false};

    // decoded video frames
    std::mutex fm;
    std::condition_variable fcv;
    std::deque<std::unique_ptr<VFrame>> frames;
    std::vector<std::unique_ptr<VFrame>> spare;
    size_t max_frames = 4;

    AtomicSeconds seek_target;
    std::atomic<bool> seeking{false};
    // The viewer's seeks play from the keyframe before the spot asked for (seek_asked; from
    // seek_from): input 0 finds it and moves seek_target there (while `snapping`) before the
    // other inputs seek.
    AtomicSeconds seek_asked, seek_from;
    std::atomic<bool> snapping{false};
    std::mutex seek_m;  // seek_at() against input 0 moving seek_target
    double duration = 0;
    bool seekable = true;
    bool live = false;
    bool has_video = false, has_audio = false;
    int vw = 0, vh = 0;
    float fps = 0;
    std::string codec;  // for the log: "H.264 · 1920×1080 · 60 fps · HW · AAC · 44 kHz"
    // The same without the frame rate (codec_info() puts in the one it plays at), under meta_m.
    std::string codec_head, codec_tail;

    std::mutex meta_m;
    std::string icy, art_key;
    std::vector<Track> audio_tracks;
    std::vector<Track> sub_tracks;
    int sub_stream = -1;       // embedded stream index in input 0, or -1
    int sub_external = -1;     // index into src.external_subs
    Subtitles subs;

    // Playback clock as of the last update(), for the video thread.
    AtomicSeconds clock_now;
    std::atomic<bool> clock_running{false};

    // Loading diagnostics and watchdog.
    double created = now();
    std::atomic<int> step{0};                // what the opener is doing (STEP_TIMEOUTS)
    double opened_at = 0;                    // streams open, decoders ready
    std::atomic<int> packets_read{0};        // by the demuxers
    std::atomic<int> video_in{0}, video_out{0};  // packets given to the video decoder (from a keyframe on), pictures back
    int decoding = 0;                        // Wii U hardware decoding level used (video_decoding setting)
    double progress_at = 0, last_status = 0;
    int64_t progress_mark = -1;
    std::string startup;                     // how long each opening step took ("Started in")
    bool started = false;                    // playing (or ready, paused) since opening
    bool starved = false;                    // buffering because playback ran out
    bool video_starved = false;              // ...of pictures (or both, from one file over the network): refill_target()
    double video_dry_since = 0;              // no pictures or packets waiting since (with sound)
    double underrun = 0, underrun_since = 0; // seconds the sound missed, in the moment since
    double playing_since = 0;                // last time buffering ended

    // playback clock when there is no audio
    double wall_base_pts = 0, wall_base_time = 0;
    bool wall_running = false;
    double last_clock = 0;
    double buffering_since = 0;
    double last_progress_report = 0;
    std::vector<char> skipped;  // per src.skip_segments: already jumped over
    std::string skip_notice;

    // Auto quality: what the downloads got during this session (measure_network).
    int64_t meter_bytes = -1;
    double meter_busy = 0, meter_at = 0;
    double net_rate = 0;        // bytes/s, smoothed
    // Input 0 when FFmpeg reads it from the network itself (not through http_io): KB it read, and
    // milliseconds the reads took (the demuxer waiting on the network, or a live stream's next
    // piece). By its demuxer.
    std::atomic<int> read_kb{0}, read_ms{0};
    // MB more the demuxed packets may hold (keep_ahead()): all there is ahead of the playback for
    // an input without http_io's downloads.
    std::atomic<int> room_mb{0};
    int stalls = 0;             // times the picture ran out while playing (after a refill or seconds of it)
    bool refilled = false;      // playing on after running out, rather than after a start or a seek
    double last_stall = 0;
    bool user_paused = false;
    std::atomic<float> speed{1};  // playback speed (always 1 for live streams)

    // Pictures faster than the decoder can go (1080p60 on the Wii U): the share of them it has
    // time for, the rest left out (video_loop). When that means a long freeze before each keyframe,
    // the video plays at 720p instead (decoder_step_down), `freeze` seconds (estimated or seen).
    std::atomic<float> keep_share{1}, unreferenced_share{0}, freeze{0}, gop{0};
    std::atomic<bool> decoder_too_slow{false};

    // Playback statistics, logged every 10 s of playing (playback_stats).
    std::atomic<int> decode_us{0}, decoded{0}, dropped{0}, left_out{0};  // by the video thread
    std::atomic<int> tail_out{0}, tails{0}, tail_ms_max{0};  // of left_out: before keyframes
    std::atomic<int> bytes_read{0};                           // by the demuxers
    HttpIoStats io_mark[2];  // the inputs' downloads as of stats.since (download_report)
    struct {
        double since = 0, last_update = 0;
        double show_time = 0, show_max = 0, gap_max = 0;
        int shown = 0, bound = 0, late = 0, updates = 0, slow_updates = 0;
    } stats;

    // For the developer's stats panel (player::stats()), on the main thread.
    int64_t rate_bytes = -1;  // http_io_meter() (or read_kb) as of rate_at
    double rate_at = 0;
    float rate = 0;           // KB/s, smoothed
    int waits = 0;            // times it ran out while playing
    double waited = 0;        // seconds, till it played again
    struct {
        float fps = 0, decode_ms = 0;
        int left_out = 0, dropped = 0, late = 0;
    } last;                   // the last playback_stats() window
};

std::shared_ptr<Session> g_s;
uint32_t g_next_id = 1;
float g_speed = 1;  // the playback speed picked, kept for the next videos until the app closes
std::atomic<int> g_closers{0};
// The Wii U has one hardware H.264 decoder: a new session waits until the previous one has
// closed it (sessions close in the background).
std::atomic<int> g_hw_busy{0};
// Safest decoding level the watchdog has needed this run, never gone back on so it can't retry
// the same level over and over. Not saved: one stream the hardware decoder can't take says
// little about the next.
int g_decoding_floor = 0;
// 1080p60 was too much for the Wii U's decoder in this video, even leaving out pictures: its
// 720p60 until another video plays (see max_fps).
std::atomic<bool> g_hd60_too_slow{false};
// auto_cap(): what Auto stepped down to last (bits/s), and when (now(), in whole seconds: the Wii U
// has no 8-byte atomics).
constexpr int AUTO_CAP_SECONDS = 20 * 60;
std::atomic<int> g_auto_cap{0};
std::atomic<int> g_auto_cap_at{-AUTO_CAP_SECONDS};
std::string g_empty;
Source g_empty_src;

SDL_Texture* g_tex = nullptr;
int g_tex_w = 0, g_tex_h = 0;
Uint32 g_tex_format = 0;
SDL_YUV_CONVERSION_MODE g_tex_mode = SDL_YUV_CONVERSION_BT601;
uint32_t g_shown_gen = 0;
double g_shown_pts = -1;
// Frames the video texture draws from or drew from last (the GPU can still be reading those until
// the screen has been updated twice since), newest first.
AVFrame* g_bound[3] = {};

std::vector<Source> g_queue;
int g_queue_index = -1;

// What video downloads get (bytes/s), smoothed over the last minute or so of them, kept between
// runs as "net_rate" (KB/s): Auto quality's starting point.
double g_net_rate = -1;
// The same in KB/s for other threads (auto_budget, from a service's resolve); -1: not measured yet.
std::atomic<int> g_net_kbs{-1};
std::string g_quality_notice;  // Auto or the decoder stepped down: for a toast

void set_error(Session& s, const std::string& e) {
    {
        std::lock_guard<std::mutex> lk(s.err_m);
        s.error = e;
    }
    s.state = FAILED;
    log_message(LOG_ERROR, "Player", "%s", e.c_str());
}

// Why opening gave up, by step.
const char* const STEP_TIMEOUTS[] = {N_("Timed out while finding the stream"),
                                     N_("Timed out while connecting to the video"),
                                     N_("Timed out while connecting to the audio"),
                                     N_("Timed out while setting up decoding")};

int interrupt_cb(void* opaque) { return ((std::atomic<bool>*)opaque)->load() ? 1 : 0; }

std::string av_err(int e) {
    char buf[128];
    av_strerror(e, buf, sizeof(buf));
    return buf;
}

double ts_to_sec(int64_t ts, AVRational tb) { return ts == AV_NOPTS_VALUE ? -1 : ts * av_q2d(tb); }

// Developer updates are on (updater::developer()): developer settings apply.
bool developer() { return store::get_bool("update_dev_unlocked", false) && store::get_bool("update_dev", false); }

// Frees custom I/O (smb://, chunked http) that avformat_close_input() leaves.
void free_custom_io(AVIOContext* pb) {
    smb_io_free(pb);
    http_io_free(pb);
}

void close_input(AVFormatContext** fmt) {
    AVIOContext* pb = *fmt && ((*fmt)->flags & AVFMT_FLAG_CUSTOM_IO) ? (*fmt)->pb : nullptr;
    avformat_close_input(fmt);
    free_custom_io(pb);
}

AVFormatContext* open_input(Session& s, const std::string& url, Input& in) {
    AVFormatContext* fmt = avformat_alloc_context();
    fmt->interrupt_callback.callback = interrupt_cb;
    fmt->interrupt_callback.opaque = &s.abort;
    const bool network = util::starts_with(url, "http://") || util::starts_with(url, "https://");
    in.network = network;
    in.remote = network || smb::is_url(url);
    if (smb::is_url(url)) {  // FFmpeg has no SMB protocol: custom I/O
        std::string err;
        if (!(fmt->pb = smb_io_open(url, &s.abort, err))) {
            avformat_free_context(fmt);
            if (!s.abort) set_error(s, err);
            return nullptr;
        }
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    } else if (network && s.src.chunked_http) {  // through libcurl: see http_io.hpp
        std::vector<std::pair<std::string, std::string>> headers = s.src.headers;
        headers.emplace_back("User-Agent", s.src.user_agent.empty() ? http::user_agent() : s.src.user_agent);
        std::string err;
        bool audio_track = !s.src.audio_url.empty() && url == s.src.audio_url;
        // Developers can try another number of downloads at once (Settings > Playback).
        http_io_set_connections(developer() ? (int)store::get_int("dev_connections", 0) : 0);
        bool no_ranges = false;
        if ((fmt->pb = http_io_open(url, headers, &s.abort, audio_track, err, &no_ranges))) {
            fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
            in.chunked = true;
        } else if (no_ranges && !s.abort) {  // a media server that sends whole files: FFmpeg's client below
            log_message(LOG_WARNING, "Player", "The server sends only whole files: reading it as it plays");
        } else {
            avformat_free_context(fmt);
            if (!s.abort) set_error(s, err);
            return nullptr;
        }
    }
    AVIOContext* custom_pb = fmt->pb;  // not freed by avformat_open_input() on failure

    AVDictionary* opts = nullptr;
    if (network && custom_pb) {
        fmt->probesize = 1 << 20;
        fmt->max_analyze_duration = 2 * AV_TIME_BASE;
    } else if (network) {
        std::string hdrs;
        for (auto& [k, v] : s.src.headers) hdrs += k + ": " + v + "\r\n";
        if (!hdrs.empty()) av_dict_set(&opts, "headers", hdrs.c_str(), 0);
        av_dict_set(&opts, "user_agent", s.src.user_agent.empty() ? http::user_agent() : s.src.user_agent.c_str(), 0);
        av_dict_set(&opts, "reconnect", "1", 0);
        av_dict_set(&opts, "reconnect_streamed", "1", 0);
        av_dict_set(&opts, "reconnect_delay_max", "4", 0);
        av_dict_set(&opts, "timeout", "15000000", 0);  // socket I/O, microseconds
        av_dict_set(&opts, "icy", "1", 0);
        av_dict_set(&opts, "tls_verify", http::verify_tls() ? "1" : "0", 0);
        if (http::verify_tls() && util::file_exists(http::ca_bundle()))
            av_dict_set(&opts, "ca_file", http::ca_bundle().c_str(), 0);
        av_dict_set(&opts, "http_persistent", "1", 0);
        fmt->probesize = 1 << 20;
        fmt->max_analyze_duration = 2 * AV_TIME_BASE;
    } else {
        fmt->probesize = 4 << 20;
    }

    std::string path = util::starts_with(url, "file://") ? url.substr(7) : url;
    int r = avformat_open_input(&fmt, path.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (r < 0) {
        free_custom_io(custom_pb);
        if (!s.abort) set_error(s, util::fmt(tr("Couldn't open stream (%s)"), av_err(r).c_str()));
        return nullptr;
    }
    r = avformat_find_stream_info(fmt, nullptr);
    if (r < 0 && !s.abort) log_message(LOG_WARNING, "Player", "find_stream_info: %s", av_err(r).c_str());
    return fmt;
}

bool reserve_hw_decoder(Session& s) {
    for (int i = 0; i < 300 && !s.abort; i++) {
        int free_ = 0;
        if (g_hw_busy.compare_exchange_strong(free_, 1)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!s.abort) log_message(LOG_WARNING, "Player", "Hardware decoder still in use: decoding in software");
    return false;
}

AVCodecContext* open_decoder(AVStream* st, bool allow_hw, bool* used_hw) {
    const AVCodec* codec = nullptr;
    if (used_hw) *used_hw = false;
    AVCodecParameters* p = st->codecpar;
    if (allow_hw && p->codec_id == AV_CODEC_ID_H264 && p->width <= 1920 && p->height <= 1088 &&
        p->format != AV_PIX_FMT_YUV420P10LE && p->profile != AV_PROFILE_H264_HIGH_10 &&
        p->profile != AV_PROFILE_H264_HIGH_422 && p->profile != AV_PROFILE_H264_HIGH_444_PREDICTIVE) {
        codec = avcodec_find_decoder_by_name("h264_wiiu");
        if (codec && used_hw) *used_hw = true;
    }
    // By name for H.264 so a software fallback never picks h264_wiiu again.
    if (!codec) codec = p->codec_id == AV_CODEC_ID_H264 ? avcodec_find_decoder_by_name("h264") : avcodec_find_decoder(p->codec_id);
    if (!codec) return nullptr;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, p);
    ctx->pkt_timebase = st->time_base;
    if (!(used_hw && *used_hw) && p->codec_type == AVMEDIA_TYPE_VIDEO) {
        ctx->thread_count = platform::is_wiiu() ? 3 : 0;
        ctx->thread_type = FF_THREAD_SLICE | FF_THREAD_FRAME;
    }
    if (used_hw && *used_hw) platform::attach_video_frames(ctx);  // decoded where the GPU draws them
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        platform::detach_video_frames(ctx);
        avcodec_free_context(&ctx);
        if (used_hw && *used_hw) {
            // Hardware refused it (unsupported profile/level): software fallback.
            *used_hw = false;
            return open_decoder(st, false, nullptr);
        }
        return nullptr;
    }
    return ctx;
}

int stream_channels(const AVCodecParameters* p) {
#ifdef HAVE_CH_LAYOUT
    return p->ch_layout.nb_channels;
#else
    return p->channels;
#endif
}

// Channel layout of a decoded audio frame as a mask, falling back to the default for its channel count.
int64_t frame_layout(const AVFrame* f) {
#ifdef HAVE_CH_LAYOUT
    if (f->ch_layout.order == AV_CHANNEL_ORDER_NATIVE) return (int64_t)f->ch_layout.u.mask;
    AVChannelLayout def = {};
    av_channel_layout_default(&def, f->ch_layout.nb_channels);
    return def.order == AV_CHANNEL_ORDER_NATIVE ? (int64_t)def.u.mask : 0;
#else
    return f->channel_layout ? (int64_t)f->channel_layout : av_get_default_channel_layout(f->channels);
#endif
}

// Resampler from the given input to interleaved S16 stereo at the mixer rate.
SwrContext* alloc_resampler(int64_t in_layout, AVSampleFormat in_fmt, int in_rate) {
#ifdef HAVE_CH_LAYOUT
    AVChannelLayout in = {}, out = AV_CHANNEL_LAYOUT_STEREO;
    if (av_channel_layout_from_mask(&in, (uint64_t)in_layout) < 0) return nullptr;
    SwrContext* swr = nullptr;
    if (swr_alloc_set_opts2(&swr, &out, AV_SAMPLE_FMT_S16, audio::RATE, &in, in_fmt, in_rate, 0, nullptr) < 0)
        return nullptr;
    return swr;
#else
    return swr_alloc_set_opts(nullptr, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, audio::RATE, in_layout, in_fmt, in_rate,
                              0, nullptr);
#endif
}

std::string stream_label(AVStream* st, int n) {
    std::string lang, title;
    if (AVDictionaryEntry* e = av_dict_get(st->metadata, "language", nullptr, 0)) lang = e->value;
    if (AVDictionaryEntry* e = av_dict_get(st->metadata, "title", nullptr, 0)) title = e->value;
    std::string label = !title.empty() ? title : !lang.empty() ? util::lower(lang) : util::fmt(tr("Track %d"), n);
    if (!title.empty() && !lang.empty() && title.find(lang) == std::string::npos) label += " (" + lang + ")";
    const char* cname = avcodec_get_name(st->codecpar->codec_id);
    if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        int ch = stream_channels(st->codecpar);
        label += util::fmt(" \xC2\xB7 %s %s", util::lower(cname).c_str(),
                           ch >= 6 ? "5.1" : ch == 2 ? tr("stereo") : ch == 1 ? tr("mono") : "");
    }
    return label;
}

const char* codec_display_name(AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_H264: return "H.264";
        case AV_CODEC_ID_HEVC: return "HEVC";
        case AV_CODEC_ID_VP8: return "VP8";
        case AV_CODEC_ID_VP9: return "VP9";
        case AV_CODEC_ID_MPEG4: return "MPEG-4";
        case AV_CODEC_ID_MPEG2VIDEO: return "MPEG-2";
        case AV_CODEC_ID_AAC: return "AAC";
        case AV_CODEC_ID_MP3: return "MP3";
        case AV_CODEC_ID_AC3: return "AC-3";
        case AV_CODEC_ID_EAC3: return "E-AC-3";
        case AV_CODEC_ID_FLAC: return "FLAC";
        case AV_CODEC_ID_OPUS: return "Opus";
        case AV_CODEC_ID_VORBIS: return "Vorbis";
        case AV_CODEC_ID_ALAC: return "ALAC";
        default: return avcodec_get_name(id);
    }
}

void extract_artwork(Session& s, AVFormatContext* fmt) {
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        AVStream* st = fmt->streams[i];
        if (!(st->disposition & AV_DISPOSITION_ATTACHED_PIC) || st->attached_pic.size <= 0) continue;
        SDL_RWops* rw = SDL_RWFromConstMem(st->attached_pic.data, st->attached_pic.size);
        SDL_Surface* surf = IMG_Load_RW(rw, 1);
        if (!surf) return;
        std::string key = util::fmt("player-art-%u", s.id);
        SDL_Surface* copy = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_RGBA32, 0);
        SDL_FreeSurface(surf);
        if (!copy) return;
        // Texture upload must happen on the main thread.
        tasks::on_main([key, copy]() {
            SDL_Surface* blur = SDL_ConvertSurfaceFormat(copy, SDL_PIXELFORMAT_RGBA32, 0);
            images::put(key, copy);
            if (blur) images::put(key, blur, images::BLUR);
        });
        std::lock_guard<std::mutex> lk(s.meta_m);
        s.art_key = key;
        return;
    }
}

void fill_metadata(Session& s, AVFormatContext* fmt) {
    std::lock_guard<std::mutex> lk(s.meta_m);
    auto get = [&](const char* k) -> std::string {
        AVDictionaryEntry* e = av_dict_get(fmt->metadata, k, nullptr, 0);
        return e ? e->value : "";
    };
    if (s.src.title.empty()) s.src.title = get("title");
    if (s.src.subtitle.empty()) {
        std::string artist = get("artist"), album = get("album");
        s.src.subtitle = artist.empty() ? album : album.empty() ? artist : artist + " \xC2\xB7 " + album;
    }
    if (s.src.title.empty() && (s.src.service == "local" || s.src.service == "smb"))
        s.src.title = util::file_name(s.src.url);
}

void poll_icy(Session& s, AVFormatContext* fmt) {
    if (!fmt->pb || (fmt->flags & AVFMT_FLAG_CUSTOM_IO)) return;  // ICY comes from FFmpeg's HTTP client
    uint8_t* meta = nullptr;
    if (av_opt_get(fmt->pb, "icy_metadata_packet", AV_OPT_SEARCH_CHILDREN, &meta) < 0 || !meta) return;
    std::string m = (const char*)meta;
    av_free(meta);
    size_t p = m.find("StreamTitle='");
    if (p == std::string::npos) return;
    p += 13;
    size_t e = m.find("';", p);
    std::string title = m.substr(p, e == std::string::npos ? std::string::npos : e - p);
    // iHeart stations append key="value" tags: Artist - text="Song" song_spot="M" amgArtworkURL="..."
    size_t t = title.find(" text=\"");
    if (t != std::string::npos) {
        size_t v = t + 7, ve = title.find('"', v);
        std::string song = title.substr(v, ve == std::string::npos ? std::string::npos : ve - v);
        std::string artist = title.substr(0, t);
        if (artist.size() >= 2 && artist.compare(artist.size() - 2, 2, " -") == 0) artist.resize(artist.size() - 2);
        title = artist.empty() ? song : song.empty() ? artist : artist + " - " + song;
    }
    std::lock_guard<std::mutex> lk(s.meta_m);
    if (title != s.icy) {
        s.icy = title;
        log_message(LOG_OK, "Player", "Now playing: %s", title.c_str());
    }
}

// Seconds of the first video packet from where input 0 stands (after a seek: the keyframe it
// landed on), or -1. What it reads is dropped.
double first_video_time(Session& s) {
    Input& in = s.in[0];
    AVStream* st = in.fmt->streams[in.video];
    AVPacket* pkt = av_packet_alloc();
    double t = -1;
    for (int i = 0; pkt && i < 200 && !s.abort && av_read_frame(in.fmt, pkt) >= 0; i++) {
        bool video = pkt->stream_index == in.video;
        if (video) {
            t = ts_to_sec(pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts, st->time_base);
            if (t >= 0 && in.fmt->start_time != AV_NOPTS_VALUE) t -= in.fmt->start_time / (double)AV_TIME_BASE;
        }
        av_packet_unref(pkt);
        if (video) break;
    }
    av_packet_free(&pkt);
    return t;
}

// How much the demuxed packets may hold ahead of the playback. When FFmpeg reads the video itself
// (no http_io downloads ahead of it), they are all there is ahead: four times the packets, so
// that the bytes set the limit (8 s of 60 fps and 20 s of 24 fps weren't much for a slow
// connection), twice the sound (AC-3 is 40 s of 3 MB), and more of all once a long wait needs
// room (room_mb, keep_ahead()).
double room_scale(const Session& s) { return 1 + ((size_t)s.room_mb << 20) / (double)video_queue_bytes(); }
size_t video_queue_limit(const Session& s) { return video_queue_bytes() + ((size_t)s.room_mb << 20); }
size_t video_packet_limit(const Session& s) {
    return (size_t)(VIDEO_QUEUE_PACKETS * (s.in[0].chunked ? 1 : 4) * room_scale(s));
}
size_t audio_queue_limit(const Session& s) {
    return (size_t)(AUDIO_QUEUE_BYTES * (s.in[0].chunked ? 1 : 2) * room_scale(s));
}

void demux_loop(std::shared_ptr<Session> sp, int idx) {
    cpu::ThreadTag tag("demux");
    Session& s = *sp;
    Input& in = s.in[idx];
    AVPacket* pkt = av_packet_alloc();
    double last_icy = 0;
    // Input 0 read by FFmpeg from the network: what its reads got, and how long they took
    // (read_kb, read_ms), handed on in whole KB and ms.
    const bool meter = idx == 0 && in.remote && !in.chunked;
    int64_t unmetered_bytes = 0;
    double unmetered_busy = 0;

    while (!s.abort) {
        if (in.seek_req) {
            std::unique_lock<std::mutex> lk(s.seek_m);
            if (idx > 0 && s.snapping) {  // input 0 is finding where the video plays from
                lk.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            in.seek_req = false;
            const uint32_t gen = s.gen;
            const double t = s.seek_target;
            const bool snap = idx == 0 && s.snapping;
            lk.unlock();
            auto seek_to = [&](double to) {
                int64_t ts = (int64_t)(to * AV_TIME_BASE);
                if (in.fmt->start_time != AV_NOPTS_VALUE) ts += in.fmt->start_time;
                int r = avformat_seek_file(in.fmt, -1, INT64_MIN, ts, ts, 0);
                if (r < 0) r = avformat_seek_file(in.fmt, -1, INT64_MIN, ts, INT64_MAX, AVSEEK_FLAG_BACKWARD);
                if (r < 0) log_message(LOG_WARNING, "Player", "Seek failed: %s", av_err(r).c_str());
            };
            seek_to(t);
            if (snap) {
                // From the keyframe the video landed on, as at a start (open_session): the pictures
                // up to the spot would be downloaded and decoded only to be dropped, and the
                // downloads alone take longer than the video in them on a Wii U's Wi-Fi. Going
                // forward, only while that keeps at least half the jump (it could be back where it
                // was, or before); then from the spot exactly. (FFmpeg can't go to the keyframe
                // after a spot in YouTube's fragmented files: it looks only in fragments read.)
                const double kf = in.video >= 0 ? first_video_time(s) : -1;
                seek_to(t);
                const double from = s.seek_from;
                std::lock_guard<std::mutex> lk(s.seek_m);
                if (gen == s.gen) {
                    if (kf >= 0 && kf < t && kf > t - 15 && (t < from || kf >= (from + t) / 2)) s.seek_target = kf;
                    s.snapping = false;
                }
            }
            if (in.video >= 0) s.vq.flush();
            if (in.audio >= 0) s.aq.flush();
            in.eof = false;
        }

        // Back-pressure: stop reading when enough is buffered.
        bool v_full = in.video >= 0 && s.vq.bytes > video_queue_limit(s);
        bool a_full = in.audio >= 0 && s.aq.bytes > audio_queue_limit(s);
        bool v_enough = in.video < 0 || s.vq.count() >= video_packet_limit(s) || v_full;
        bool a_enough = in.audio < 0 || s.aq.count() > 160 || a_full;
        if (v_full || a_full || (v_enough && a_enough) || in.eof) {
            std::this_thread::sleep_for(std::chrono::milliseconds(in.eof ? 30 : 8));
            if (in.network && now() - last_icy > 1.5) {
                poll_icy(s, in.fmt);
                last_icy = now();
            }
            continue;
        }

        const uint32_t gen = s.gen;
        const double read_at = meter ? now() : 0;
        int r = av_read_frame(in.fmt, pkt);
        if (meter && r >= 0) {
            unmetered_busy += now() - read_at;
            unmetered_bytes += pkt->size;
            if (unmetered_bytes >= 64 << 10) {
                const int kb = (int)(unmetered_bytes >> 10), ms = (int)(unmetered_busy * 1000);
                s.read_kb += kb;
                s.read_ms += ms;
                unmetered_bytes -= (int64_t)kb << 10;
                unmetered_busy -= ms / 1000.0;
            }
        }
        if (gen != s.gen || in.seek_req || s.abort) {
            // A seek came while this waited for the network: what it read is from before it
            // (a picture without the ones it refers to, decoded after the decoder was reset).
            // Closing, it's cut short.
            av_packet_unref(pkt);
            continue;
        }
        if (r == AVERROR(EAGAIN)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (r < 0) {
            if (s.abort) break;
            if (r != AVERROR_EOF) log_message(LOG_WARNING, "Player", "read_frame: %s", av_err(r).c_str());
            in.eof = true;
            if (in.video >= 0) s.vq.set_eof();
            if (in.audio >= 0) s.aq.set_eof();
            continue;
        }

        s.packets_read++;
        s.bytes_read += pkt->size;
        AVStream* st = in.fmt->streams[pkt->stream_index];
        // On the playback clock's scale (from the file's start time, as the decoders do).
        const int64_t pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
        const double at = pts == AV_NOPTS_VALUE ? -1
                          : ts_to_sec(pts, st->time_base) -
                                (in.fmt->start_time != AV_NOPTS_VALUE ? in.fmt->start_time / (double)AV_TIME_BASE : 0);
        if (pkt->stream_index == in.video) {
            s.vq.push(av_packet_clone(pkt), gen, at);
        } else if (pkt->stream_index == in.audio) {
            s.aq.push(av_packet_clone(pkt), gen, at);
        } else if (idx == 0 && pkt->stream_index == s.sub_stream && s.sdec) {
            AVSubtitle sub;
            int got = 0;
            if (avcodec_decode_subtitle2(s.sdec, &sub, &got, pkt) >= 0 && got) {
                double start = ts_to_sec(pkt->pts, st->time_base) + sub.start_display_time / 1000.0;
                double end = sub.end_display_time ? ts_to_sec(pkt->pts, st->time_base) + sub.end_display_time / 1000.0
                                                  : start + ts_to_sec(pkt->duration, st->time_base);
                if (end <= start) end = start + 4;
                for (unsigned i = 0; i < sub.num_rects; i++) {
                    AVSubtitleRect* rc = sub.rects[i];
                    std::string txt = rc->ass ? Subtitles::strip_ass(rc->ass) : rc->text ? rc->text : "";
                    if (!txt.empty()) s.subs.add(start, end, txt);
                }
                avsubtitle_free(&sub);
            }
        }
        av_packet_unref(pkt);

        if (in.network && now() - last_icy > 1.5) {
            poll_icy(s, in.fmt);
            last_icy = now();
        }
    }
    av_packet_free(&pkt);
}

// Calls `f` with the header byte of each NAL unit of an H.264 packet, while it returns true.
// `length_size`: bytes of each NAL unit's length (MP4), 0 for start codes (MPEG-TS). False when
// `f` stopped or the packet is malformed.
template <class F>
bool each_nal(const AVPacket* p, int length_size, F f) {
    const uint8_t* d = p->data;
    const uint8_t* end = d + p->size;
    if (length_size > 0) {
        while (end - d > length_size) {
            uint32_t n = 0;
            for (int i = 0; i < length_size; i++) n = n << 8 | d[i];
            d += length_size;
            if (n == 0 || n > (uint32_t)(end - d) || !f(d[0])) return false;
            d += n;
        }
    } else {
        for (; end - d > 3; d++)
            if (d[0] == 0 && d[1] == 0 && d[2] == 1) {
                if (!f(d[3])) return false;
                d += 2;
            }
    }
    return true;
}

// Whether an H.264 packet is a picture no other picture refers to (nal_ref_idc 0 in its slices:
// the B-frames of YouTube's streams, over half of them), so leaving it out harms no other.
bool unreferenced_picture(const AVPacket* p, int length_size) {
    bool slice = false;
    return each_nal(p, length_size, [&](uint8_t header) {  // false: a reference picture, or not a picture at all
        const int type = header & 0x1f;
        if (type == 1 || type == 5) slice = true;
        return !(type == 5 || type == 7 || type == 8 || (type == 1 && header >> 5));
    }) && slice;
}

// Whether an H.264 packet is an IDR picture: nothing after it refers to a picture before it.
bool idr_picture(const AVPacket* p, int length_size) {
    bool idr = false;
    each_nal(p, length_size, [&](uint8_t header) { return !(idr = (header & 0x1f) == 5); });
    return idr;
}

void video_loop(std::shared_ptr<Session> sp) {
    cpu::ThreadTag tag("video");
    Session& s = *sp;
    FrameConverter conv;
    AVFrame* frame = av_frame_alloc();
    uint32_t dec_gen = s.gen;
    bool drained = false;
    uint32_t drain_gen = 0;  // s.gen when it began draining: a seek since, and it isn't the end
    int64_t frame_count = 0;
    double fps = 0;
    int dropped_in_row = 0;
    double catch_up_until = 0;  // leaving out unreferenced pictures until then (at least)
    const AVDiscard base_skip = s.vdec->skip_frame;
    bool keyframe_seen = false;  // packets before the first keyframe can't give a picture
    AVStream* st = s.in[0].fmt->streams[s.in[0].video];
    if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) fps = av_q2d(st->avg_frame_rate);
    // The Wii U's decoder does its work as a packet goes in, about 20 ms for a 1080p picture: 50 a
    // second at most, short of 1080p60's 60. Rather than fall behind and then lose pictures in
    // bursts, it leaves out unreferenced ones evenly, as many as keep it busy for BUSY of the
    // time: about 45 pictures a second. Lighter streams (720p60: 8 ms) keep all of theirs.
    constexpr double BUSY = 0.9;
    const bool thin = s.hw;
    const AVCodecParameters* par = st->codecpar;
    const int length_size = par->extradata_size >= 7 && par->extradata[0] == 1 ? (par->extradata[4] & 3) + 1 : 0;
    double cost = 0;          // seconds a picture takes the decoder, smoothed
    double cost_slow = 0;     // ...over a few seconds (the mean of the first ones)
    int cost_n = 0;
    double credit = 0;        // pictures it has time for (decimation)
    double unref_share = -1;  // of the packets, smoothed
    int thin_packets = 0;
    double decode_took = 0;  // by the last packet: going in, and its pictures coming out
    bool timed = false;      // ...and it counts toward `cost`
    // A quality shown at 30 fps (quality_fps()) of a 60 fps stream: every other picture, the ones
    // on the 30 fps grid counted from the last keyframe, so the motion stays even. Unreferenced
    // pictures off it aren't decoded; the others are (later ones need them) but not shown.
    const int every = quality_fps(s.src.quality) == 30 && fps > 45 ? (int)std::lround(fps / 30) : 1;
    int64_t grid_from = AV_NOPTS_VALUE;  // pts of the last keyframe packet
    std::deque<int64_t> hidden;          // pts of pictures decoded only for the ones after them
    if (every > 1) s.keep_share = 1.0f / every;
    // When leaving out unreferenced pictures isn't enough (few B-frames, a busy scene), the decoder
    // falls behind. It catches up before a keyframe that starts afresh (an IDR: nothing after it
    // refers to a picture before it): the pictures up to it are left out, the last one shown stays
    // up a moment, and the decoder gets ahead again. It does so when that keyframe is due within
    // `lead`, so each run of pictures from one keyframe to the next starts about that far ahead
    // (Session::max_frames holds them), room for the busier parts of it.
    const bool tails_on = thin && fps > 31 && par->height > 720;
    const double lead = fps > 0 ? (s.max_frames - 1) / fps : 0;
    const double start_time = s.in[0].fmt->start_time != AV_NOPTS_VALUE ? s.in[0].fmt->start_time / (double)AV_TIME_BASE : 0;
    auto due_in = [&](int64_t pts) { return ts_to_sec(pts, s.vtb) - start_time - s.clock_now; };
    bool tail_skip = false;             // leaving out the pictures up to the next keyframe
    int tail = 0;                       // pictures left out before it
    int next_key = 0;                   // the next keyframe waiting: 0 not seen yet, 1 an IDR, 2 not
    int64_t next_key_pts = AV_NOPTS_VALUE;
    int64_t last_key = AV_NOPTS_VALUE;  // pts of the last IDR keyframe decoded
    double gop = 0;                     // seconds from IDR to IDR, smoothed
    double freeze = 0;                  // seconds left out before each IDR, smoothed (from the third)
    int gops = 0, idr_keys = 0;
    bool idr_rare = false;              // more than 10 s from one to the next
    double unref_slow = 0;              // unref_share over a few seconds (the mean of the first ones)
    int unref_n = 0;

    while (!s.abort) {
        if (timed && decode_took > 0) {
            cost = cost > 0 ? 0.95 * cost + 0.05 * decode_took : decode_took;
            cost_slow += (decode_took - cost_slow) / std::min(++cost_n, 200);
        }
        decode_took = 0;
        timed = false;
        QPacket qp{nullptr, 0};
        if (!s.vq.pop(qp, s.abort, 50)) {
            if (s.vq.drained() && !drained && !s.abort) {
                drain_gen = s.gen;
                avcodec_send_packet(s.vdec, nullptr);  // flush remaining frames
                drained = true;
            } else if (!drained) {
                continue;
            }
            qp.pkt = nullptr;
        } else {
            drained = false;
        }

        if (qp.pkt) {
            if (qp.gen != s.gen) {  // from before a seek
                av_packet_free(&qp.pkt);
                continue;
            }
            if (qp.gen != dec_gen) {
                avcodec_flush_buffers(s.vdec);
                dec_gen = qp.gen;
                hidden.clear();
                tail_skip = false;
                tail = next_key = gops = 0;
                last_key = AV_NOPTS_VALUE;
            }
            const bool key = qp.pkt->flags & AV_PKT_FLAG_KEY;
            if (key) keyframe_seen = true;
            // Of the decoder's time, all pictures but the unreferenced ones take.
            const double need = cost > 0 ? (1 - std::max(unref_share, 0.0)) * fps * cost : 0;
            if (key && tails_on) {
                // Some streams' keyframes are only sometimes IDRs (YouTube's): it catches up
                // before those, so runs are counted from one to the next.
                next_key = 0;
                const bool idr = idr_picture(qp.pkt, length_size);
                if (idr) idr_keys++;
                if (qp.pkt->pts != AV_NOPTS_VALUE && last_key != AV_NOPTS_VALUE && qp.pkt->pts > last_key &&
                    (qp.pkt->pts - last_key) * av_q2d(s.vtb) > 10)
                    idr_rare = true;
                if (idr && qp.pkt->pts != AV_NOPTS_VALUE) {
                    if (last_key != AV_NOPTS_VALUE && qp.pkt->pts > last_key && s.clock_running) {
                        const double g = (qp.pkt->pts - last_key) * av_q2d(s.vtb), f = tail / fps;
                        gop = gop > 0 ? 0.7 * gop + 0.3 * g : g;
                        if (++gops > 2) freeze = gops == 3 ? f : 0.7 * freeze + 0.3 * f;  // after the start's
                        if (tail) s.tail_ms_max = std::max(s.tail_ms_max.load(), (int)std::lround(f * 1e3));
                    }
                    last_key = qp.pkt->pts;
                }
                tail_skip = false;
                tail = 0;
            } else if (!key && !tail_skip && tails_on && need > 0.9 && s.clock_running && qp.pkt->pts != AV_NOPTS_VALUE &&
                       due_in(qp.pkt->pts) < lead) {
                if (!next_key) {
                    bool idr = false;
                    if (s.vq.next_key(dec_gen, next_key_pts, idr, [&](const AVPacket* k) { return idr_picture(k, length_size); }))
                        next_key = idr ? 1 : 2;
                }
                if (next_key == 1 && due_in(next_key_pts) < lead) {
                    tail_skip = true;
                    s.tails++;
                }
            }
            if (tail_skip) {
                tail++;
                s.left_out++;
                s.tail_out++;
                av_packet_free(&qp.pkt);
                continue;
            }
            const bool unref = (thin || every > 1) && unreferenced_picture(qp.pkt, length_size);
            if (thin) {
                unref_share = unref_share < 0 ? unref : 0.98 * unref_share + 0.02 * unref;
                unref_slow += (unref - unref_slow) / std::min(++unref_n, 200);
                s.unreferenced_share = (float)unref_share;
                // More than the decoder has time for even with every unreferenced picture left out
                // (a stream with few B-frames, a slower decoder): it catches up before keyframes
                // (above). Frozen much of the time (estimated from the start, then as seen) or for
                // long each time, and 720p60 (lighter) plays better, so only above 720p30. Streams
                // with few IDR keyframes can't catch up so.
                if (++thin_packets > 300 && cost > 0 && fps > 31 && par->height > 720 && !s.decoder_too_slow) {
                    const double steady = (1 - unref_slow) * fps * cost_slow;
                    const double share = steady > 1 ? 1 - 1 / steady : 0;  // of the time, frozen
                    const bool slow = idr_keys == 0 || idr_rare
                                          ? steady > 1
                                          : share > 0.1 || (gops >= 6 && (freeze > 0.3 || freeze > 0.08 * gop));
                    if (slow) {
                        s.freeze = (float)(gops >= 6 ? freeze : share * gop);
                        s.gop = (float)gop;
                        s.decoder_too_slow = true;
                    }
                }
            }
            if (every > 1 && qp.pkt->pts != AV_NOPTS_VALUE) {
                if ((qp.pkt->flags & AV_PKT_FLAG_KEY) || grid_from == AV_NOPTS_VALUE) grid_from = qp.pkt->pts;
                if (std::llround((qp.pkt->pts - grid_from) * av_q2d(s.vtb) * fps) % every != 0) {
                    if (unref) {
                        s.left_out++;
                        av_packet_free(&qp.pkt);
                        continue;
                    }
                    hidden.push_back(qp.pkt->pts);
                    if (hidden.size() > 64) hidden.pop_front();
                }
            }
            if (thin && cost > 0 && fps > 0) {
                // Of the packets that get here: shown at 30 fps, the unreferenced half off the
                // grid is gone already.
                const double rate = fps * (1 - std::max(unref_share, 0.0) * (every - 1) / every);
                const double keep = std::min(1.0, BUSY / (rate * cost));
                s.keep_share = (float)(std::max(0.0, 1.0 / every - rate / fps * (1 - keep)) *  // shown of all
                                       (1 - (gop > 0 ? std::min(1.0, freeze / gop) : 0.0)));
                credit += keep;
                if (credit < 1 && unref) {
                    s.left_out++;
                    av_packet_free(&qp.pkt);
                    continue;
                }
                credit = std::max(credit - 1, -1.0);
            }
            // Timed when the decoder decodes it (catching up, it leaves out unreferenced ones).
            timed = thin && !(unref && s.vdec->skip_frame >= AVDISCARD_NONREF);
            double start = now();
            int r = avcodec_send_packet(s.vdec, qp.pkt);
            decode_took = now() - start;
            s.decode_us += (int)(decode_took * 1e6);
            s.decoded++;
            av_packet_free(&qp.pkt);
            if (keyframe_seen) s.video_in++;
            if (r < 0 && r != AVERROR(EAGAIN)) continue;
        }

        for (;;) {
            double start = now();
            int r = avcodec_receive_frame(s.vdec, frame);
            decode_took += now() - start;
            s.decode_us += (int)((now() - start) * 1e6);
            if (r == AVERROR_EOF) {
                if (drain_gen == s.gen) s.video_eof = true;
                avcodec_flush_buffers(s.vdec);
                break;
            }
            if (r < 0) break;
            s.video_out++;
            int64_t ts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
            if (!hidden.empty()) {  // off the 30 fps grid
                auto h = std::find(hidden.begin(), hidden.end(), frame->pts != AV_NOPTS_VALUE ? frame->pts : ts);
                if (h != hidden.end()) {
                    hidden.erase(h);
                    av_frame_unref(frame);
                    continue;
                }
            }
            double pts = ts_to_sec(ts, s.vtb);
            if (pts < 0) pts = fps > 0 ? frame_count / fps : 0;
            frame_count++;
            if (s.in[0].fmt->start_time != AV_NOPTS_VALUE) pts -= s.in[0].fmt->start_time / (double)AV_TIME_BASE;
            s.video_eof = false;

            // Accurate seek: decode past frames silently.
            if (pts < s.seek_target - 0.02 && dec_gen == s.gen) {
                av_frame_unref(frame);
                continue;
            }

            {
                std::unique_lock<std::mutex> lk(s.fm);
                s.fcv.wait(lk, [&] { return s.abort || s.frames.size() < s.max_frames || dec_gen != s.gen; });
            }
            if (s.abort) break;
            if (dec_gen != s.gen) {
                av_frame_unref(frame);
                continue;
            }

            // Behind the clock (decoding or converting is too slow for this stream): skip the
            // conversion of late pictures, keeping one in four so the picture still moves, and
            // have the decoder leave out pictures nothing refers to (most B-frames) until it
            // is comfortably ahead again.
            if (s.clock_running) {
                double lag = s.clock_now - pts;
                double t = now();
                if (lag > 0.3 && s.vdec->skip_frame < AVDISCARD_NONREF) {
                    s.vdec->skip_frame = AVDISCARD_NONREF;
                    catch_up_until = t + 5;
                    log_message(LOG_WARNING, "Player", "Video %.2f s behind: skipping pictures to catch up", lag);
                } else if (lag < -0.05 && t > catch_up_until && s.vdec->skip_frame > base_skip) {
                    s.vdec->skip_frame = base_skip;
                    log_message(LOG_OK, "Player", "Video caught up");
                }
                // (Pictures of the Wii U's decoder cost nothing to show: those late while it catches
                // up before a keyframe show late rather than one in four.)
                if (lag > (tails_on ? 0.2 : 0.1) && dropped_in_row < 3) {
                    dropped_in_row++;
                    s.dropped++;
                    av_frame_unref(frame);
                    continue;
                }
            }
            dropped_in_row = 0;

            std::unique_ptr<VFrame> vf;
            {
                std::lock_guard<std::mutex> lk(s.fm);
                if (!s.spare.empty()) {
                    vf = std::move(s.spare.back());
                    s.spare.pop_back();
                }
            }
            if (!vf) vf = std::make_unique<VFrame>();
            vf->w = frame->width;
            vf->h = frame->height;
            vf->pts = pts;
            vf->gen = dec_gen;
            bool ok = true;
            if (yuv_texture_format(frame, vf->format, vf->yuv_mode)) {
                av_frame_move_ref(vf->yuv, frame);
            } else {
                vf->format = SDL_PIXELFORMAT_RGBA32;
                vf->yuv_mode = SDL_YUV_CONVERSION_BT601;
                vf->rgba.resize((size_t)vf->w * vf->h * 4);
                platform::video_frame_cpu_read(frame);
                ok = conv.convert(frame, vf->rgba.data(), vf->w * 4);
            }
            av_frame_unref(frame);
            if (!ok) continue;
            std::lock_guard<std::mutex> lk(s.fm);
            s.frames.push_back(std::move(vf));
        }
    }
    av_frame_free(&frame);
}

// The audio thread's way to the mixer, at the playback speed. What it sends is kept for a few
// seconds, so that when the speed changes, the part the mixer hasn't played yet can be taken
// back and sent again at the new speed (right away, not seconds later).
class AudioOut {
public:
    explicit AudioOut(Session& s) : s_(s) {}

    // Sends `n` decoded frames starting at media time `pts`, of decoder generation `gen`.
    void put(const int16_t* p, size_t n, double pts, uint32_t gen) {
        if (gen != gen_) {  // seeked: none of what was kept belongs anymore
            gen_ = gen;
            history_.clear();
            kept_ = 0;
            tempo_.reset();
        }
        Chunk c;
        while (kept_ > KEEP_FRAMES && !history_.empty()) {
            c = std::move(history_.front());  // reuse its memory
            kept_ -= c.frames.size() / 2;
            history_.pop_front();
        }
        c.pts = pts;
        c.frames.assign(p, p + n * 2);
        history_.push_back(std::move(c));
        kept_ += n;
        if (speed_ != s_.speed) resend();
        else if (!send(p, n, pts)) resend();
    }

    // Between packets (or while none arrive): picks up a new speed for what's queued.
    void check() {
        if (speed_ == s_.speed) return;
        if (gen_ != s_.gen) {  // nothing current was sent yet
            speed_ = s_.speed;
            tempo_.reset();
            tempo_.set_speed(speed_);
            return;
        }
        resend();
    }

private:
    static constexpr size_t KEEP_FRAMES = audio::RATE * 5;  // the mixer holds up to 2 s, 4 s of media at 2x
    static constexpr int FADE_FRAMES = audio::RATE / 400;   // 2.5 ms fade-in after a resend

    struct Chunk {
        double pts = 0;
        std::vector<int16_t> frames;
    };

    uint32_t tag() const { return (s_.id << 16) | (gen_ & 0xFFFF); }
    bool current() const { return !s_.abort && gen_ == s_.gen; }

    // To the mixer at speed_, waiting while it's full. False if the speed changed meanwhile.
    bool send(const int16_t* p, size_t n, double pts) {
        if (speed_ != 1) {
            stretched_.clear();
            pts = tempo_.process(p, n, pts, stretched_);
            if (pts < 0) return true;  // the stretcher is still filling up
            p = stretched_.data();
            n = stretched_.size() / 2;
        }
        if (fade_ < FADE_FRAMES) {
            if (p != stretched_.data()) {
                stretched_.assign(p, p + n * 2);
                p = stretched_.data();
            }
            for (size_t i = 0; i < n && fade_ < FADE_FRAMES; i++, fade_++) {
                float g = (fade_ + 1.0f) / (FADE_FRAMES + 1);
                stretched_[i * 2] = (int16_t)(stretched_[i * 2] * g);
                stretched_[i * 2 + 1] = (int16_t)(stretched_[i * 2 + 1] * g);
            }
        }
        size_t written = 0;
        while (written < n && current()) {
            if (speed_ != s_.speed) return false;
            size_t w = audio::stream_write(p + written * 2, n - written, pts + (double)written * speed_ / audio::RATE,
                                           tag(), speed_);
            written += w;
            if (w == 0) std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        return true;
    }

    // Takes back what the mixer hasn't played yet (all but a moment) and sends it again, from
    // what was kept, at the speed now asked for.
    void resend() {
        for (;;) {
            speed_ = s_.speed;
            tempo_.reset();
            tempo_.set_speed(speed_);
            double from = audio::stream_cut(tag(), 0.1);
            fade_ = from >= 0 ? 0 : FADE_FRAMES;
            bool changed = false;
            for (const Chunk& c : history_) {
                size_t n = c.frames.size() / 2, skip = 0;
                if (from > c.pts) skip = std::min(n, (size_t)std::lround((from - c.pts) * audio::RATE));
                if (skip >= n) continue;
                if (!send(c.frames.data() + skip * 2, n - skip, c.pts + (double)skip / audio::RATE)) {
                    changed = true;
                    break;
                }
                if (!current()) return;
            }
            if (!changed) return;
        }
    }

    Session& s_;
    uint32_t gen_ = 0;
    float speed_ = 1;
    audio::Tempo tempo_;
    std::vector<int16_t> stretched_;
    std::deque<Chunk> history_;
    size_t kept_ = 0;  // frames in history_
    int fade_ = FADE_FRAMES;
};

void audio_loop(std::shared_ptr<Session> sp) {
    cpu::ThreadTag tag("audio");
    Session& s = *sp;
    AVFrame* frame = av_frame_alloc();
    SwrContext* swr = nullptr;
    int64_t swr_layout = 0;
    int swr_rate = 0, swr_fmt = -1;
    std::vector<int16_t> buf;
    uint32_t dec_gen = s.gen;
    bool drained = false;
    uint32_t drain_gen = 0;  // as in video_loop
    double next_pts = 0;
    Input& in = s.in[s.audio_input];
    double start_offset = in.fmt->start_time != AV_NOPTS_VALUE ? in.fmt->start_time / (double)AV_TIME_BASE : 0;
    AudioOut sink(s);

    while (!s.abort) {
        sink.check();
        QPacket qp{nullptr, 0};
        if (!s.aq.pop(qp, s.abort, 50)) {
            if (s.aq.drained() && !drained && !s.abort) {
                drain_gen = s.gen;
                avcodec_send_packet(s.adec, nullptr);
                drained = true;
            } else if (!drained) {
                continue;
            }
        } else {
            drained = false;
        }
        if (qp.pkt) {
            if (qp.gen != s.gen) {
                av_packet_free(&qp.pkt);
                continue;
            }
            if (qp.gen != dec_gen) {
                avcodec_flush_buffers(s.adec);
                dec_gen = qp.gen;
            }
            int r = avcodec_send_packet(s.adec, qp.pkt);
            av_packet_free(&qp.pkt);
            if (r < 0 && r != AVERROR(EAGAIN)) continue;
        }
        for (;;) {
            int r = avcodec_receive_frame(s.adec, frame);
            if (r == AVERROR_EOF) {
                if (drain_gen == s.gen) s.audio_eof = true;
                avcodec_flush_buffers(s.adec);
                break;
            }
            if (r < 0) break;
            s.audio_eof = false;
            int64_t ts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
            double pts = ts != AV_NOPTS_VALUE ? ts_to_sec(ts, s.atb) - start_offset : next_pts;
            next_pts = pts + (double)frame->nb_samples / frame->sample_rate;

            int64_t layout = frame_layout(frame);
            if (!swr || layout != swr_layout || frame->sample_rate != swr_rate || frame->format != swr_fmt) {
                swr_free(&swr);
                swr = alloc_resampler(layout, (AVSampleFormat)frame->format, frame->sample_rate);
                if (!swr || swr_init(swr) < 0) {
                    swr_free(&swr);
                    av_frame_unref(frame);
                    continue;
                }
                swr_layout = layout;
                swr_rate = frame->sample_rate;
                swr_fmt = frame->format;
            }
            int max_out = (int)av_rescale_rnd(swr_get_delay(swr, swr_rate) + frame->nb_samples, audio::RATE, swr_rate,
                                              AV_ROUND_UP);
            buf.resize((size_t)max_out * 2);
            uint8_t* out = (uint8_t*)buf.data();
            int n = swr_convert(swr, &out, max_out, (const uint8_t**)frame->extended_data, frame->nb_samples);
            av_frame_unref(frame);
            if (n <= 0) continue;

            const int16_t* p = buf.data();
            // Accurate seek: trim samples before the target.
            double target = s.seek_target;
            if (pts < target - 0.005) {
                int skip = (int)((target - pts) * audio::RATE);
                if (skip >= n) continue;
                p += skip * 2;
                n -= skip;
                pts = target;
            }
            sink.put(p, n, pts, dec_gen);
        }
    }
    swr_free(&swr);
    av_frame_free(&frame);
}

void open_session(std::shared_ptr<Session> sp) {
    cpu::ThreadTag tag("opening");
    Session& s = *sp;
    double t = now();
    auto lap = [&](const std::string& step) {
        double t2 = now();
        if (t2 - t >= 0.05) s.startup += util::fmt("%s %.1f s, ", step.c_str(), t2 - t);
        t = t2;
    };
    if (s.src.resolve) {
        s.step = 0;
        std::string err;
        Source resolved = s.src;
        bool ok = resolved.resolve(resolved, err);
        if (s.abort) return;
        if (!ok) {
            set_error(s, err.empty() ? tr("Couldn't load this stream") : err);
            return;
        }
        resolved.resolve = nullptr;
        {
            std::lock_guard<std::mutex> lk(s.meta_m);
            s.src = std::move(resolved);
        }
        lap("finding the stream");
    }
    s.step = 1;
    // A separate audio track opens alongside the video: each takes a connection, a few round
    // trips and a first range, and one after the other added seconds to starting.
    AVFormatContext* audio_fmt = nullptr;
    std::thread audio_opener;
    if (!s.src.audio_url.empty()) {
        audio_opener = std::thread([&s, &audio_fmt] {
            cpu::ThreadTag tag("opening");
            audio_fmt = open_input(s, s.src.audio_url, s.in[1]);
        });
    }
    s.in[0].fmt = open_input(s, s.src.url, s.in[0]);
    lap(s.in[0].network ? "connecting" : "opening");
    if (audio_opener.joinable()) {
        if (!s.in[0].fmt) s.abort = true;  // failed: the audio isn't needed now (its error isn't either)
        s.step = 2;
        audio_opener.join();
        lap("waiting for the audio");
        if (audio_fmt && !s.in[0].fmt) close_input(&audio_fmt);
    }
    if (!s.in[0].fmt) return;
    s.inputs = 1;
    if (!s.src.audio_url.empty()) {
        if (!audio_fmt) return;
        s.in[1].fmt = audio_fmt;
        s.inputs = 2;
    }
    if (s.abort) return;
    s.step = 3;

    AVFormatContext* f0 = s.in[0].fmt;
    int v = av_find_best_stream(f0, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (v >= 0 && (f0->streams[v]->disposition & AV_DISPOSITION_ATTACHED_PIC)) v = -1;
    s.in[0].video = v;

    // Audio and subtitle tracks of the primary input (published at once: the screen reads them).
    std::vector<Track> audio_tracks, sub_tracks;
    int n_audio = 0;
    for (unsigned i = 0; i < f0->nb_streams; i++) {
        AVStream* st = f0->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) audio_tracks.push_back(Track{(int)i, stream_label(st, ++n_audio)});
        if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            AVCodecID id = st->codecpar->codec_id;
            if (id == AV_CODEC_ID_SUBRIP || id == AV_CODEC_ID_TEXT || id == AV_CODEC_ID_ASS || id == AV_CODEC_ID_SSA ||
                id == AV_CODEC_ID_MOV_TEXT || id == AV_CODEC_ID_WEBVTT)
                sub_tracks.push_back(Track{(int)i, stream_label(st, (int)sub_tracks.size() + 1)});
        }
    }
    for (size_t i = 0; i < s.src.external_subs.size(); i++)
        sub_tracks.push_back(Track{1000 + (int)i, s.src.external_subs[i].first});
    {
        std::lock_guard<std::mutex> lk(s.meta_m);
        s.audio_tracks = std::move(audio_tracks);
        s.sub_tracks = std::move(sub_tracks);
    }

    if (s.inputs == 2) {
        s.in[1].audio = av_find_best_stream(s.in[1].fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        s.audio_input = 1;
    } else {
        int a = s.pref_audio >= 0 ? s.pref_audio : av_find_best_stream(f0, AVMEDIA_TYPE_AUDIO, -1, v, nullptr, 0);
        s.in[0].audio = a;
        s.audio_input = 0;
    }

    Input& ain = s.in[s.audio_input];
    if (v < 0 && ain.audio < 0) {
        set_error(s, tr("No playable audio or video found"));
        return;
    }

    std::string vinfo, ainfo;
    if (v >= 0) {
        AVStream* st = f0->streams[v];
        // The video_decoding setting, or the safer level the player fell back to this run when the
        // hardware gave no pictures.
        s.decoding = platform::is_wiiu()
                         ? std::clamp(std::max((int)store::get_int("video_decoding", 0), g_decoding_floor), 0, 2)
                         : 2;
        bool hw_reserved = s.decoding < 2 && st->codecpar->codec_id == AV_CODEC_ID_H264 && reserve_hw_decoder(s);
        s.vdec = open_decoder(st, hw_reserved, &s.hw);
        if (hw_reserved && !s.hw) g_hw_busy = 0;
        if (s.vdec && s.hw && s.decoding == 1) s.vdec->skip_frame = AVDISCARD_NONREF;
        if (s.vdec)
            log_message(LOG_OK, "Player", "Video decoder: %s%s", s.vdec->codec->name,
                        s.hw && s.decoding == 1 ? " (without unreferenced pictures)" : "");
        if (!s.vdec) {
            if (ain.audio < 0) {
                set_error(s, util::fmt(tr("Unsupported video codec (%s)"), avcodec_get_name(st->codecpar->codec_id)));
                return;
            }
            s.in[0].video = -1;  // play the audio at least
        } else {
            s.vtb = st->time_base;
            s.has_video = true;
            s.vw = st->codecpar->width;
            s.vh = st->codecpar->height;
            AVRational fr = st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0 ? st->avg_frame_rate : st->r_frame_rate;
            s.fps = fr.num > 0 && fr.den > 0 && av_q2d(fr) < 400 ? (float)av_q2d(fr) : 0;
            // Pictures decoded ahead. The Wii U's decoder gets further ahead at 1080p60, for the
            // busier parts between keyframes (video_loop): 12 of its pictures are 37 MB.
            s.max_frames = (size_t)s.vw * s.vh <= 1280 * 720 ? 5 : s.hw && s.fps > 31 ? 12 : 3;
            vinfo = util::fmt("%s \xC2\xB7 %d\xC3\x97%d", codec_display_name(st->codecpar->codec_id), s.vw, s.vh);
        }
    }
    if (ain.audio >= 0) {
        AVStream* st = ain.fmt->streams[ain.audio];
        s.adec = open_decoder(st, false, nullptr);
        if (s.adec) {
            s.atb = st->time_base;
            s.has_audio = true;
            ainfo = util::fmt("%s \xC2\xB7 %d kHz", codec_display_name(st->codecpar->codec_id),
                              st->codecpar->sample_rate / 1000);
        } else {
            ain.audio = -1;
        }
    }
    if (!s.has_video && !s.has_audio) {
        set_error(s, tr("Unsupported codecs"));
        return;
    }
    {
        std::lock_guard<std::mutex> lk(s.meta_m);
        s.codec_head = vinfo;
        s.codec_tail = s.hw ? " \xC2\xB7 HW" : "";
        if (!ainfo.empty()) s.codec_tail += vinfo.empty() ? ainfo : " \xC2\xB7 " + ainfo;
        s.codec = s.codec_head + (s.fps > 0 ? util::fmt(" \xC2\xB7 %ld fps", std::lround(s.fps)) : "") + s.codec_tail;
    }
    lap("setting up decoding");

    if (f0->duration > 0) s.duration = f0->duration / (double)AV_TIME_BASE;
    else if (s.src.live) s.duration = 0;
    bool pb_seekable = f0->pb ? (f0->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0 : true;
    s.live = s.src.live || (s.duration <= 0 && !pb_seekable) || (s.duration <= 0 && s.in[0].network);
    s.seekable = !s.live && s.duration > 0;
    if (s.live) s.speed = 1;
    // What the stream takes when the service didn't say: FFmpeg's reckoning (a file's size over
    // its length), for refill_target(). Nothing lighter is known: Auto doesn't step down for it.
    if (s.src.bitrate <= 0 && !s.live && s.duration > 0 && f0->bit_rate > 0) {
        std::lock_guard<std::mutex> lk(s.meta_m);
        s.src.bitrate = (int)std::min<int64_t>(f0->bit_rate, INT_MAX);
        s.src.min_bitrate = s.src.bitrate;
    }

    fill_metadata(s, f0);
    extract_artwork(s, f0);

    // External subtitles: the first track is loaded now when it should be on from the start.
    if (!s.src.external_subs.empty() && s.src.subs_auto && store::get_bool("subs_default_on", true)) {
        std::string data;
        const std::string& u = s.src.external_subs[0].second;
        bool ok = util::starts_with(u, "http") ? [&] {
            http::Response r = http::get(u, {}, 15);
            data = std::move(r.body);
            return r.ok();
        }() : smb::is_url(u) ? smb::read_file(u, data) : util::read_file(u, data);
        if (ok) {
            s.subs.load(data);
            s.sub_external = 0;
        }
        lap("loading subtitles");
    }

    if (s.src.start > 1 && s.seekable) {
        auto seek_input = [&](int i, double to) {
            int64_t ts = (int64_t)(to * AV_TIME_BASE);
            if (s.in[i].fmt->start_time != AV_NOPTS_VALUE) ts += s.in[i].fmt->start_time;
            avformat_seek_file(s.in[i].fmt, -1, INT64_MIN, ts, ts, 0);
        };
        double start = s.src.start;
        seek_input(0, start);
        // Played from the keyframe the video landed on rather than the exact spot: the pictures
        // in between would be downloaded and decoded only to be dropped, seconds on the Wii U.
        // Finding it reads a packet, so the video goes back to it after.
        if (s.in[0].video >= 0) {
            double kf = first_video_time(s);
            if (kf >= 0 && kf > start - 15 && kf < start) start = kf;
            // The same seek again lands on the same keyframe. Seeking to the keyframe's own time
            // can look in the fragment before (the file's times are a little off from FFmpeg's),
            // downloading a piece of it for nothing.
            seek_input(0, s.src.start);
        }
        for (int i = 1; i < s.inputs; i++) seek_input(i, start);
        s.seek_target = start;
        lap("jumping to " + util::format_duration(s.src.start) +
            (start < s.src.start ? util::fmt(" (its keyframe %.1f s before)", s.src.start - start) : ""));
    } else {
        s.seek_target = 0;
    }

    log_message(LOG_OK, "Player", "Opened: %s (%s)%s", s.src.title.c_str(), s.codec.c_str(), s.live ? " [live]" : "");
    if (s.abort) return;
    s.opened_at = now();
    s.state = BUFFERING;
    s.buffering_since = now();
    for (int i = 0; i < s.inputs; i++) s.in[i].thread = std::thread(demux_loop, sp, i);
    if (s.has_video) s.vthread = std::thread(video_loop, sp);
    if (s.has_audio) s.athread = std::thread(audio_loop, sp);
}

void teardown(std::shared_ptr<Session> sp) {
    Session& s = *sp;
    s.abort = true;
    s.vq.wake();
    s.aq.wake();
    s.fcv.notify_all();
    if (s.opener.joinable()) s.opener.join();
    for (auto& in : s.in)
        if (in.thread.joinable()) in.thread.join();
    if (s.vthread.joinable()) s.vthread.join();
    if (s.athread.joinable()) s.athread.join();
    s.vq.flush();
    s.aq.flush();
    if (s.vdec) {
        platform::detach_video_frames(s.vdec);
        avcodec_free_context(&s.vdec);
    }
    if (s.hw) {
        s.hw = false;
        g_hw_busy = 0;
    }
    if (s.adec) avcodec_free_context(&s.adec);
    if (s.sdec) avcodec_free_context(&s.sdec);
    for (auto& in : s.in)
        if (in.fmt) close_input(&in.fmt);  // also frees custom (smb://, chunked http) I/O
}

double master_clock(Session& s) {
    if (s.has_audio && !(s.audio_eof && audio::stream_buffered_seconds() < 0.05)) {
        double c = audio::stream_clock();
        if (c >= 0) {
            s.last_clock = c;
            s.wall_base_pts = c;
            s.wall_base_time = now();
            return c;
        }
        return s.last_clock;
    }
    if (s.wall_running) {
        s.last_clock = s.wall_base_pts + (now() - s.wall_base_time) * s.speed;
        return s.last_clock;
    }
    return s.last_clock;
}

void start_wall(Session& s, double pts) {
    s.wall_base_pts = pts;
    s.wall_base_time = now();
    s.wall_running = true;
}

void report_progress(Session& s, bool force) {
    if (s.live) return;
    double t = now();
    if (!force && t - s.last_progress_report < 10) return;
    s.last_progress_report = t;
    double pos = s.last_clock;
    if (s.src.remember_position && !s.src.service.empty() && !s.src.id.empty()) {
        store::Resume r;
        r.service = s.src.service;
        r.id = s.src.id;
        r.title = s.src.title;
        r.subtitle = s.src.subtitle;
        r.image = s.src.artwork;
        r.extra = s.src.extra;
        r.position = pos;
        r.duration = s.duration;
        r.video = s.has_video;
        store::resume_save(r);
    }
    if (s.src.on_progress) s.src.on_progress(pos, s.state == PAUSED);
}

// The GPU is done with the frames the video texture drew from: it has been destroyed, or has
// just been uploaded to (which waits for the GPU).
void release_bound() {
    for (AVFrame* f : g_bound)
        if (f) av_frame_unref(f);
}

void destroy_texture() {
    if (g_tex) SDL_DestroyTexture(g_tex);
    g_tex = nullptr;
    release_bound();
}

// Puts the picture into the video texture: points the texture at it where the GPU can read the
// decoder's frame as it is (the frame is kept, f.yuv left empty), else uploads it. True for the
// former.
bool upload_frame(VFrame& f) {
    SDL_Renderer* r = gfx::renderer();
    if (!g_tex || g_tex_w != f.w || g_tex_h != f.h || g_tex_format != f.format || g_tex_mode != f.yuv_mode) {
        destroy_texture();
        SDL_SetYUVConversionMode(f.yuv_mode);  // renderers read it when they make the texture or draw it
        g_tex = SDL_CreateTexture(r, f.format, SDL_TEXTUREACCESS_STREAMING, f.w, f.h);
        if (!g_tex) return false;
        SDL_SetTextureBlendMode(g_tex, SDL_BLENDMODE_NONE);
        SDL_SetTextureScaleMode(g_tex, SDL_ScaleModeLinear);
        g_tex_w = f.w;
        g_tex_h = f.h;
        g_tex_format = f.format;
        g_tex_mode = f.yuv_mode;
    }
    g_shown_pts = f.pts;
    g_shown_gen = f.gen;
    const AVFrame* y = f.yuv;
    if (f.format == SDL_PIXELFORMAT_NV12 && platform::show_video_frame(g_tex, y)) {
        AVFrame* keep = g_bound[2] ? g_bound[2] : av_frame_alloc();
        av_frame_unref(keep);
        g_bound[2] = g_bound[1];
        g_bound[1] = g_bound[0];
        g_bound[0] = keep;
        av_frame_move_ref(keep, f.yuv);
        return true;
    }
    if (f.format == SDL_PIXELFORMAT_NV12)
        SDL_UpdateNVTexture(g_tex, nullptr, y->data[0], y->linesize[0], y->data[1], y->linesize[1]);
    else if (f.format == SDL_PIXELFORMAT_IYUV)
        SDL_UpdateYUVTexture(g_tex, nullptr, y->data[0], y->linesize[0], y->data[1], y->linesize[1], y->data[2],
                             y->linesize[2]);
    else
        SDL_UpdateTexture(g_tex, nullptr, f.rgba.data(), f.w * 4);
    release_bound();
    return false;
}

void recycle(Session& s, std::unique_ptr<VFrame> f) {
    av_frame_unref(f->yuv);  // hand the picture buffer back to the decoder
    if (s.spare.size() < 3) s.spare.push_back(std::move(f));
}

// Seconds of demuxed video waiting ahead of the playback clock.
double video_ahead(Session& s) {
    // From where it plays on: the clock, or where a start or a seek goes (a resume's keyframe).
    double from = !s.started || s.seeking ? (double)s.seek_target : s.last_clock;
    std::lock_guard<std::mutex> lk(s.vq.m);
    return s.vq.q.empty() || s.vq.last_pts < 0 ? 0 : std::max(0.0, s.vq.last_pts - from);
}

// Seconds of video in ahead of the playback clock: demuxed, and what the downloads have ahead of
// the demuxer (at the file's average rate).
double video_ready(Session& s) {
    double t = video_ahead(s);
    const Input& in = s.in[0];
    HttpIoStats io;
    if (in.video >= 0 && in.fmt && s.duration > 0 && http_io_stats(in.fmt->pb, io) && io.size > 0)
        t += io.ready * s.duration / io.size;
    return t;
}

// The same for the sound (a separate audio file's downloads; the video's file has both).
double audio_ready(Session& s) {
    double from = !s.started || s.seeking ? (double)s.seek_target : s.last_clock, t;
    {
        std::lock_guard<std::mutex> lk(s.aq.m);
        t = s.aq.q.empty() || s.aq.last_pts < 0 ? 0 : std::max(0.0, s.aq.last_pts - from);
    }
    const Input& in = s.in[s.audio_input];
    HttpIoStats io;
    if (s.audio_input > 0 && in.fmt && s.duration > 0 && http_io_stats(in.fmt->pb, io) && io.size > 0)
        t += io.ready * s.duration / io.size;
    return t;
}

// Seconds of the video the downloads can keep ahead of it (http_io_max_ahead()): what a wait can
// fill. For a file FFmpeg reads itself, what the demuxed packets can grow to (keep_ahead()).
double room_ahead(const Session& s) {
    const Input& in = s.in[0];
    HttpIoStats io;
    if (s.duration <= 0 || !in.fmt) return 1e9;
    if (http_io_stats(in.fmt->pb, io)) return io.size > 0 ? http_io_max_ahead() / KEEP_AHEAD_MARGIN * s.duration / io.size : 1e9;
    if (s.src.bitrate <= 0) return 1e9;
    return (video_queue_bytes() + http_io_max_ahead()) / KEEP_AHEAD_MARGIN * 8 / s.src.bitrate;
}

// Seconds of video to wait for after the pictures ran out: 3, 6, 9, 12, then 15. A connection
// slower than the stream runs dry again soon after a short wait; fewer, longer waits are less
// of a bother than a stop every few seconds. When the downloads get less than the stream takes
// (a little less than measured, to be safe), it waits for what the rest of the video lacks (a
// browser's "can play through"), as much as comes in within PLAY_THROUGH_WAIT (longer each time)
// and fits in room_ahead(): one longer wait instead of a stop every half minute.
double refill_target(const Session& s) {
    const int n = std::max(s.stalls, 1);
    double target = VIDEO_CUSHION * std::min(n, 5);
    const double got = s.net_rate * 8 * 0.9, takes = s.src.bitrate * (double)s.speed;  // bits/s
    if (got > 0 && got < takes && s.duration > 0) {
        const double left = std::max(0.0, s.duration - s.last_clock);
        target = std::max(target, std::min({left * (1 - got / takes), PLAY_THROUGH_WAIT * std::min(n, 3) * got / takes,
                                            room_ahead(s)}));
    }
    return target;
}

// Lets the downloads keep `seconds` of the video ahead, and of a separate sound file: room for
// what a long wait waits for. A file FFmpeg reads itself: the demuxed packets (room_mb), as far as
// http_io would go.
void keep_ahead(Session& s, double seconds) {
    if (s.duration <= 0) return;
    auto keep = [&](const Input& in) {
        HttpIoStats io;
        if (in.fmt && http_io_stats(in.fmt->pb, io) && io.size > 0)
            http_io_keep_ahead(in.fmt->pb, (int64_t)(seconds * KEEP_AHEAD_MARGIN * io.size / s.duration));
    };
    keep(s.in[0]);
    if (s.audio_input > 0) keep(s.in[s.audio_input]);
    if (!s.in[0].chunked && s.src.bitrate > 0) {
        const double more = seconds * KEEP_AHEAD_MARGIN * s.src.bitrate / 8 - (double)video_queue_bytes();
        const int mb = (int)std::clamp(std::ceil(more / (1 << 20)), 0.0, (double)(http_io_max_ahead() >> 20));
        if (mb > s.room_mb) {
            log_message(LOG_OK, "Player", "The video read ahead may take %d MB now (was %d MB)",
                        (int)(video_queue_limit(s) >> 20) + mb - s.room_mb, (int)(video_queue_limit(s) >> 20));
            s.room_mb = mb;
        }
    }
}

// Enough video came in to play on for a while (`seconds`), or all there is, or all that is kept
// (the demuxed packets and the downloads ahead of them: nothing more comes in until it plays).
bool video_refilled(Session& s, double seconds) {
    if (s.video_eof || s.vq.drained() || s.in[0].eof) return true;
    if (video_ready(s) >= seconds) return true;
    {
        std::lock_guard<std::mutex> lk(s.vq.m);
        if (s.vq.q.size() < video_packet_limit(s) && s.vq.bytes <= video_queue_limit(s)) return false;
    }
    HttpIoStats io;
    return !s.in[0].fmt || !http_io_stats(s.in[0].fmt->pb, io) || io.full;
}

// The Wii U hardware decoder taking packets without giving pictures, or not taking them at all:
// reopen with the next safer decoding level (without unreferenced pictures, then software).
// Returns true when it replaced the session.
bool decoder_watchdog(Session& s, double t) {
    if (!s.hw || s.video_out > 0 || s.opened_at <= 0) return false;
    int vin = s.video_in;
    double since_open = t - s.opened_at;
    if (vin < 45 && !(since_open > 15 && s.vq.count() > 0)) return false;
    int next = std::min(s.decoding + 1, 2);
    g_decoding_floor = next;
    log_message(LOG_WARNING, "Player", "No pictures from the hardware decoder (%d packets in %.0f s): trying %s", vin,
                since_open, next == 1 ? "it without unreferenced pictures" : "software decoding");
    retry();
    return true;
}

// Bytes the downloads of the inputs received so far (0 for inputs that aren't http_io's).
int64_t downloaded(Session& s) {
    int64_t n = 0;
    for (int i = 0; i < s.inputs; i++) {
        HttpIoStats io;
        if (s.in[i].fmt && http_io_stats(s.in[i].fmt->pb, io)) n += io.downloaded;
    }
    return n;
}

// While loading: logs what is (not) happening every 5 s, and gives up when nothing has arrived
// for 30 s: no packets, no pictures and no bytes (with the demuxed packets all kept, a wait for
// refill_target() goes on in the downloads only). Returns true when it ended the session.
bool buffering_watchdog(Session& s, double t) {
    // All of these only grow, so their sum changes whenever one does.
    int64_t mark = (int64_t)s.packets_read + s.video_out + downloaded(s);
    if (mark != s.progress_mark) {
        s.progress_mark = mark;
        s.progress_at = t;
    }
    int vin = s.video_in, vout = s.video_out;
    double loading = t - s.buffering_since;
    if (loading >= 5 && t - s.last_status >= 5) {
        s.last_status = t;
        log_message(LOG_WARNING, "Player",
                    "Loading for %.0f s: %.1f s of audio ready, %zu video packets waiting (%.1f s of video in), %d "
                    "decoded into %d pictures (%s)",
                    loading, audio::stream_buffered_seconds(), s.vq.count(), s.has_video ? video_ready(s) : 0.0, vin, vout,
                    s.vdec ? s.vdec->codec->name : "no video");
    }
    if (s.progress_at > 0 && t - s.progress_at > 30) {
        s.abort = true;
        set_error(s, tr("Stopped loading: nothing arrived for 30 seconds"));
        return true;
    }
    return false;
}

void start_session(const Source& src, int pref_audio) {
    const Source* was = g_s ? &g_s->original : nullptr;
    if (!was || was->service != src.service || was->id != src.id || was->url != src.url) g_hd60_too_slow = false;
    close();
    auto s = std::make_shared<Session>();
    s->id = g_next_id++ & 0xFFFF;
    s->src = src;
    s->original = src;
    s->pref_audio = pref_audio;
    s->gen = 1;
    s->user_paused = false;
    s->speed = src.live ? 1 : g_speed;
    s->last_clock = src.start;
    audio::stream_reset((s->id << 16) | 1);
    audio::stream_pause(true);
    g_shown_pts = -1;
    g_shown_gen = 0;
    g_s = s;
    s->opener = std::thread(open_session, s);
}

// FFmpeg's warnings and errors go to our log (and its file), each distinct message once: some
// streams repeat a harmless one ("Late SEI is not implemented") for every frame. Its chatter
// below warnings is left out.
void ffmpeg_log(void* avcl, int level, const char* fmt, va_list vl) {
    if (level > AV_LOG_WARNING) return;
    char line[1024];
    int prefix = 0;  // no "[h264 @ 0x...]": the address would make every message distinct
    av_log_format_line(nullptr, level, fmt, vl, line, sizeof(line), &prefix);
    size_t len = std::strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = 0;
    if (!len) return;

    static std::mutex m;
    static std::vector<uint32_t> seen;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) h = (h ^ (uint8_t)line[i]) * 16777619u;
    {
        std::lock_guard<std::mutex> lk(m);
        if (std::find(seen.begin(), seen.end(), h) != seen.end()) return;
        if (seen.size() >= 256) seen.erase(seen.begin());
        seen.push_back(h);
    }
    const AVClass* cls = avcl ? *(const AVClass**)avcl : nullptr;
    std::string who = cls ? cls->item_name(avcl) : "FFmpeg";
    const char* text = line;
    if (util::starts_with(text, who + ": ")) text += who.size() + 2;  // already named
    log_message(level <= AV_LOG_ERROR ? LOG_ERROR : LOG_WARNING, "FFmpeg", "%s: %s", who.c_str(), text);
}

// The language picked for this video (kept when reopening it), "" for the original.
std::string chosen_language(Session& s) {
    std::lock_guard<std::mutex> lk(s.meta_m);
    return s.src.audio_languages.empty() ? "" : s.src.audio_language;
}

// Same position and settings, another audio language (the service resolves it again).
void reopen_with_language(const std::string& language) {
    Source src = g_s->original;
    src.audio_language = language;
    src.quality = g_s->src.quality > 0 ? g_s->src.quality : src.quality;
    if (!src.live) src.start = std::max(src.start, g_s->last_clock);
    auto queue = std::move(g_queue);
    int qi = g_queue_index;
    if (qi >= 0 && qi < (int)queue.size()) queue[qi].audio_language = language;
    start_session(src, -1);
    g_queue = std::move(queue);
    g_queue_index = qi;
}

double net_rate() {
    if (g_net_rate < 0) g_net_rate = store::get_int("net_rate", 0) * 1024.0;
    return g_net_rate;
}

void save_net_rate() {
    if (g_net_rate > 0 && store::get_int("net_rate", 0) != (int64_t)(g_net_rate / 1024))
        store::set_int("net_rate", (int64_t)(g_net_rate / 1024));
}

// Every couple of seconds of a stream from the network: what the downloads got since goes into
// the estimates, once they ran long enough for it to mean something (a first burst is quicker
// than what follows). A file FFmpeg reads itself counts for this session only (it may be the
// server that's slow, over one connection), and a live stream not at all (its reads wait for
// the next piece of it more than for the network).
void measure_network(Session& s, double t) {
    const Input& in = s.in[0];
    const bool own = !in.chunked && in.remote && !s.live;
    if ((!in.chunked && !own) || t - s.meter_at < 2) return;
    s.meter_at = t;
    int64_t bytes;
    double busy;
    if (own) {
        bytes = (int64_t)s.read_kb << 10;
        busy = s.read_ms / 1000.0;
    } else {
        http_io_meter(bytes, busy);
    }
    if (s.meter_bytes < 0) {
        s.meter_bytes = bytes;
        s.meter_busy = busy;
        return;
    }
    double got = (double)(bytes - s.meter_bytes), took = busy - s.meter_busy;
    if (took < 8 || got < (1 << 20)) return;
    s.meter_bytes = bytes;
    s.meter_busy = busy;
    double r = got / took;
    s.net_rate = s.net_rate > 0 ? 0.7 * s.net_rate + 0.3 * r : r;
    if (own) return;
    double g = net_rate();
    g_net_rate = g > 0 ? 0.8 * g + 0.2 * r : r;
    g_net_kbs = (int)(g_net_rate / 1024);
}

// What all downloads got over the last second or so, for the stats panel.
void sample_download_rate(Session& s, double t) {
    if (t - s.rate_at < 1) return;
    int64_t bytes = (int64_t)s.read_kb << 10;
    double busy;
    if (s.in[0].chunked) http_io_meter(bytes, busy);
    if (s.rate_bytes >= 0) {
        const float r = (float)((bytes - s.rate_bytes) / 1024.0 / (t - s.rate_at));
        s.rate = s.rate > 0 ? 0.5f * s.rate + 0.5f * r : r;
    }
    s.rate_bytes = bytes;
    s.rate_at = t;
}

// Auto, when the picture ran out while playing: steps down (reopening at the same place) when
// the downloads can't carry this stream, or when it ran out twice in a minute and a half anyway.
// Returns true when it replaced the session.
bool auto_step_down(Session& s, double t) {
    const Source& o = s.original;
    if (!o.auto_quality || o.quality != 0 || s.src.bitrate <= 0 || s.src.bitrate <= s.src.min_bitrate) return false;
    bool again = s.last_stall > 0 && t - s.last_stall < 90;
    s.last_stall = t;
    double rate = s.net_rate > 0 ? s.net_rate : net_rate();
    bool behind = rate > 0 && rate * 8 < s.src.bitrate * 1.1;
    if (!behind && !again) return false;
    int cap = s.src.bitrate - 1;
    if (behind) cap = std::min(cap, (int)(rate * 8 * 0.8));
    log_message(LOG_WARNING, "Player", "Auto quality: downloads get %.0f KB/s, the stream takes %d kbps: at most %d kbps now",
                rate / 1024, s.src.bitrate / 1000, cap / 1000);
    g_auto_cap = cap;
    g_auto_cap_at = (int)t;
    Source src = o;
    src.bitrate_cap = cap;
    src.audio_language = chosen_language(s);
    if (!src.live) src.start = std::max(src.start, s.last_clock);
    auto queue = std::move(g_queue);
    int qi = g_queue_index;
    start_session(src, s.pref_audio);
    g_queue = std::move(queue);
    g_queue_index = qi;
    g_quality_notice = tr("Lower quality for now: the connection is slow");
    return true;
}

// 1080p60 on the Wii U that leaving out pictures only keeps up with by long freezes (video_loop):
// reopens at the same place, where the service picks 720p60 (max_fps). Returns true when it
// replaced the session.
bool decoder_step_down(Session& s) {
    if (!s.decoder_too_slow || g_hd60_too_slow || s.original.qualities.empty() || !s.original.resolve) return false;
    s.decoder_too_slow = false;
    g_hd60_too_slow = true;
    log_message(LOG_WARNING, "Player",
                "Too much for the decoder: %.2f s left out before keyframes %.1f s apart (%.0f%% of the pictures "
                "unreferenced): 720p60",
                s.freeze.load(), s.gop.load(), s.unreferenced_share * 100);
    Source src = s.original;
    src.audio_language = chosen_language(s);
    if (!src.live) src.start = std::max(src.start, s.last_clock);
    auto queue = std::move(g_queue);
    int qi = g_queue_index;
    start_session(src, s.pref_audio);
    g_queue = std::move(queue);
    g_queue_index = qi;
    g_quality_notice = tr("720p60 for this video: its 1080p60 is too much for the Wii U");
    return true;
}

}  // namespace

void init() {
    av_log_set_callback(ffmpeg_log);
    avformat_network_init();
    // Frame rates had switches of their own before ("Allow 60 fps streams", "1080p at 60 fps"), and
    // 1080p a 45 fps choice: YouTube's 720p and 1080p become the quality that plays the same, or
    // nearly (1080p at 30 fps).
    const int64_t q = store::get_int("yt_quality", -1);
    if (q == 720 || q == 1080 || q == 108045)
        store::set_int("yt_quality", q == 108045 ? 108060
                                     : q == 1080 ? 108030
                                     : store::get_bool("allow_60fps", true) ? 72060 : 72030);
}

void shutdown() {
    close();
    for (int i = 0; i < 300 && g_closers > 0; i++) SDL_Delay(10);
    destroy_texture();
    for (AVFrame*& f : g_bound) av_frame_free(&f);
    avformat_network_deinit();
}

void open(const Source& src) {
    g_queue.clear();
    g_queue_index = -1;
    start_session(src, -1);
}

void open_queue(std::vector<Source> queue, int index) {
    if (queue.empty()) return;
    index = std::clamp(index, 0, (int)queue.size() - 1);
    Source first = queue[index];
    start_session(first, -1);
    g_queue = std::move(queue);
    g_queue_index = index;
}

bool has_next() { return g_queue_index >= 0 && g_queue_index + 1 < (int)g_queue.size(); }
bool has_previous() { return g_queue_index > 0; }

bool next() {
    if (!has_next()) return false;
    auto q = std::move(g_queue);
    int i = g_queue_index + 1;
    start_session(q[i], -1);
    g_queue = std::move(q);
    g_queue_index = i;
    return true;
}

bool previous() {
    if (g_s && position() > 5 && g_s->seekable) {
        seek(0);
        return true;
    }
    if (!has_previous()) return false;
    auto q = std::move(g_queue);
    int i = g_queue_index - 1;
    start_session(q[i], -1);
    g_queue = std::move(q);
    g_queue_index = i;
    return true;
}

void close() {
    save_net_rate();
    auto s = std::move(g_s);
    g_s.reset();
    // Nothing draws the video texture from here on: without it, the decoder's frames it drew from
    // can go.
    if (g_bound[0] && g_bound[0]->buf[0]) destroy_texture();
    if (!s) return;
    if (s->state == PLAYING || s->state == PAUSED || s->state == BUFFERING) {
        report_progress(*s, true);
        if (s->src.on_stop) s->src.on_stop(s->last_clock, false);
    }
    s->abort = true;
    audio::stream_reset(0);
    audio::stream_pause(true);
    g_closers++;
    std::thread([s]() mutable {
        cpu::ThreadTag tag("closing");
        teardown(s);
        s.reset();
        g_closers--;
    }).detach();
}

void retry() {
    if (!g_s) return;
    Source src = g_s->original;
    src.start = std::max(src.start, g_s->last_clock);
    src.audio_language = chosen_language(*g_s);
    auto queue = std::move(g_queue);
    int qi = g_queue_index;
    start_session(src, -1);
    g_queue = std::move(queue);
    g_queue_index = qi;
}

void reset_decoding_fallback() { g_decoding_floor = 0; }

int auto_cap() {
    return now() - g_auto_cap_at < AUTO_CAP_SECONDS ? g_auto_cap.load() : 0;
}

int auto_budget() {
    int kbs = g_net_kbs >= 0 ? g_net_kbs.load() : (int)store::get_int("net_rate", 0);
    // Before any were measured, a Wii U's Wi-Fi: about 370 KB/s (September 2026).
    if (kbs <= 0 && platform::is_wiiu()) kbs = 370;
    return kbs > 0 ? (int)std::min(kbs * 1024.0 * 8 * 0.8, 1e9) : 0;
}

float max_fps(int height) { return !platform::is_wiiu() || height <= 720 ? 61 : 31; }

float max_fps(int height, int quality) {
    // 1080p60 leaving out pictures (video_loop): chosen with a frame rate, or Auto.
    if (height <= 720 || !platform::is_wiiu() || (quality != 0 && quality_fps(quality) <= 0)) return max_fps(height);
    return g_hd60_too_slow ? 31 : 61;
}

std::string quality_label(int quality, bool hfr) {
    const int fps = quality_fps(quality);
    return fps > 0 && hfr ? util::fmt("%dp %d fps", quality_height(quality), fps)
                          : util::fmt("%dp", quality_height(quality));
}

void set_quality(int quality) {
    if (!g_s || g_s->original.quality == quality) return;
    Source src = g_s->original;
    src.quality = quality;
    src.bitrate_cap = 0;
    src.audio_language = chosen_language(*g_s);
    if (!src.live) src.start = std::max(src.start, g_s->last_clock);
    auto queue = std::move(g_queue);
    int qi = g_queue_index;
    if (qi >= 0 && qi < (int)queue.size()) queue[qi].quality = quality;
    start_session(src, g_s->pref_audio);
    g_queue = std::move(queue);
    g_queue_index = qi;
}

void set_paused(bool paused) {
    if (!g_s) return;
    Session& s = *g_s;
    s.user_paused = paused;
    int st = s.state;
    if (paused && st == PLAYING) {
        s.state = PAUSED;
        audio::stream_pause(true);
        s.wall_running = false;
        report_progress(s, true);
    } else if (!paused && st == PAUSED) {
        s.state = PLAYING;
        audio::stream_pause(false);
        start_wall(s, s.last_clock);
    } else if (!paused && st == ENDED) {
        seek(0);
    }
}

void toggle_pause() {
    if (!g_s) return;
    set_paused(!(g_s->state == PAUSED || g_s->user_paused));
}

void set_speed(float speed) {
    g_speed = std::clamp(speed, 0.5f, 2.0f);
    if (!g_s || g_s->live) return;
    Session& s = *g_s;
    if (s.wall_running) start_wall(s, master_clock(s));  // the time so far went by at the old speed
    s.speed = g_speed;
}

float speed() { return g_s && g_s->live ? 1 : g_speed; }

// `snap`: from the keyframe before `t` (the viewer's seeks; see demux_loop). Otherwise from `t`
// exactly, the pictures before it decoded and dropped.
void seek_at(double t, bool snap) {
    if (!g_s || !g_s->seekable) return;
    Session& s = *g_s;
    if (s.state == OPENING || s.state == FAILED) return;
    t = std::clamp(t, 0.0, std::max(0.0, s.duration - 0.5));
    uint32_t gen = s.gen + 1;
    {
        std::lock_guard<std::mutex> lk(s.seek_m);
        const double from = s.seeking ? (double)s.seek_asked : s.last_clock;
        s.seek_target = t;
        s.seek_asked = t;
        s.seek_from = from;
        s.snapping = snap && s.has_video && s.in[0].video >= 0;
        // Before the new gen: a demuxer that reads it sees the request too (demux_loop).
        for (int i = 0; i < s.inputs; i++) s.in[i].seek_req = true;
        s.gen = gen;
    }
    // Not ended while the demuxers seek (input 0 can wait on the network for its keyframe first,
    // with the queues drained and the inputs at their end near a video's end).
    s.vq.flush();
    s.aq.flush();
    for (int i = 0; i < s.inputs; i++) s.in[i].eof = false;
    s.last_clock = t;
    audio::stream_reset((s.id << 16) | (gen & 0xFFFF));
    audio::stream_pause(true);
    {
        std::lock_guard<std::mutex> lk(s.fm);
        while (!s.frames.empty()) {
            recycle(s, std::move(s.frames.front()));
            s.frames.pop_front();
        }
    }
    s.fcv.notify_all();
    s.video_eof = false;
    s.audio_eof = false;
    s.wall_running = false;
    s.state = BUFFERING;
    s.buffering_since = now();
    s.seeking = true;
    s.starved = false;
    s.video_starved = false;
    s.video_dry_since = 0;
    s.underrun = 0;
}

void seek(double t) { seek_at(t, true); }

void seek_relative(double delta) {
    if (!g_s) return;
    double base = g_s->seeking ? (double)g_s->seek_asked : position();
    seek(base + delta);
}

State state() { return g_s ? (State)g_s->state.load() : IDLE; }

bool started() { return g_s && g_s->started; }

bool active() {
    State st = state();
    return st == OPENING || st == BUFFERING || st == PLAYING || st == PAUSED;
}

const std::string& error() {
    if (!g_s) return g_empty;
    std::lock_guard<std::mutex> lk(g_s->err_m);
    return g_s->error;
}

Source source() {
    if (!g_s) return g_empty_src;
    std::lock_guard<std::mutex> lk(g_s->meta_m);
    return g_s->src;
}

double position() {
    if (!g_s) return 0;
    if (g_s->seeking) return g_s->seek_target;
    return std::max(0.0, g_s->last_clock);
}

double duration() { return g_s ? g_s->duration : 0; }

double buffered_until() {
    if (!g_s) return 0;
    Session& s = *g_s;
    double a = s.has_audio ? s.aq.last_pts : -1, v = s.has_video ? s.vq.last_pts : -1;
    double b = s.has_video && s.has_audio ? std::min(a, v) : std::max(a, v);
    if (s.in[0].eof) b = s.duration;
    return std::max(b, position());
}

bool seekable() { return g_s && g_s->seekable; }
bool live() { return g_s && g_s->live; }
bool has_video() { return g_s && g_s->has_video; }
bool audio_only() { return g_s && !g_s->has_video && g_s->has_audio; }
int video_width() { return g_s ? g_s->vw : 0; }
int video_height() { return g_s ? g_s->vh : 0; }
float video_fps() { return g_s ? g_s->fps : 0; }
std::string codec_info(bool fps) {
    if (!g_s) return "";
    Session& s = *g_s;
    std::lock_guard<std::mutex> lk(s.meta_m);
    if (!fps || s.fps <= 0) return s.codec_head + s.codec_tail;
    const long all = std::lround(s.fps), shown = std::lround(s.fps * s.keep_share);
    return s.codec_head +
           (shown < all ? util::fmt(" \xC2\xB7 %ld/%ld fps", shown, all) : util::fmt(" \xC2\xB7 %ld fps", all)) +
           s.codec_tail;
}

bool stats(Stats& out) {
    if (!g_s) return false;
    Session& s = *g_s;
    const int st = s.state;
    if (st == IDLE || st == FAILED) return false;
    out = {};
    out.download = s.rate;
    out.average = (float)(s.net_rate / 1024);
    out.waits = s.waits;
    out.waited = (float)(s.waited + (st == BUFFERING && s.starved ? now() - s.buffering_since : 0));
    if (st == OPENING) return true;
    out.stream = s.src.bitrate > 0 ? s.src.bitrate / 8.0f / 1024 : 0;
    if (s.has_video) out.video_ahead = (float)video_ready(s);
    if (s.has_audio) out.audio_ahead = (float)audio_ready(s);
    for (int i = 0; i < s.inputs; i++) {
        HttpIoStats io;
        if (!s.in[i].fmt || !http_io_stats(s.in[i].fmt->pb, io)) continue;
        if (s.in[i].video >= 0) out.connections = io.connections;
        out.requests += io.requests;
        out.new_connections += io.connects;
        out.gone_quiet += io.quiet_restarts;
        out.failed += io.retries;
    }
    out.shown_fps = s.last.fps;
    out.decode_ms = s.last.decode_ms;
    out.left_out = s.last.left_out;
    out.dropped = s.last.dropped;
    out.late = s.last.late;
    return true;
}

float buffering_progress() {
    if (!g_s) return 0;
    Session& s = *g_s;
    if (s.video_starved) return (float)std::clamp(video_ready(s) / refill_target(s), 0.0, 1.0);
    double a = std::clamp(audio::stream_buffered_seconds() / 0.6, 0.0, 1.0);
    if (s.has_video && s.in[0].remote && !s.live) a = std::min(a, std::clamp(video_ready(s) / START_CUSHION, 0.0, 1.0));
    return (float)a;
}

std::string stream_title() {
    if (!g_s) return "";
    std::lock_guard<std::mutex> lk(g_s->meta_m);
    return g_s->icy;
}

std::string artwork_key() {
    if (!g_s) return "";
    std::lock_guard<std::mutex> lk(g_s->meta_m);
    return g_s->art_key;
}

// Tracks come from the file, or from the service (YouTube's dubs, one stream per language).
std::vector<Track> audio_tracks() {
    if (!g_s) return {};
    std::lock_guard<std::mutex> lk(g_s->meta_m);
    const auto& langs = g_s->src.audio_languages;
    if (langs.empty()) return g_s->audio_tracks;
    std::vector<Track> out;
    for (size_t i = 0; i < langs.size(); i++) out.push_back(Track{(int)i, langs[i].second});
    return out;
}

int audio_track() {
    if (!g_s) return -1;
    std::lock_guard<std::mutex> lk(g_s->meta_m);
    const auto& langs = g_s->src.audio_languages;
    for (size_t i = 0; i < langs.size(); i++)
        if (langs[i].first == g_s->src.audio_language) return (int)i;
    return langs.empty() ? g_s->in[0].audio : -1;
}

void set_audio_track(int index) {
    if (!g_s) return;
    std::string language;
    {
        std::lock_guard<std::mutex> lk(g_s->meta_m);
        const auto& langs = g_s->src.audio_languages;
        if (!langs.empty()) {
            if (index < 0 || index >= (int)langs.size() || langs[index].first == g_s->src.audio_language) return;
            language = langs[index].first;
        }
    }
    if (!language.empty()) {
        reopen_with_language(language);
        return;
    }
    if (g_s->inputs != 1 || index == g_s->in[0].audio) return;
    Source src = g_s->src;
    src.start = position();
    auto queue = std::move(g_queue);
    int qi = g_queue_index;
    start_session(src, index);
    g_queue = std::move(queue);
    g_queue_index = qi;
}

std::vector<Track> subtitle_tracks() {
    if (!g_s) return {};
    std::lock_guard<std::mutex> lk(g_s->meta_m);
    return g_s->sub_tracks;
}

int subtitle_track() {
    if (!g_s) return -1;
    if (g_s->sub_external >= 0) return 1000 + g_s->sub_external;
    return g_s->sub_stream;
}

void set_subtitle_track(int index) {
    if (!g_s) return;
    Session& s = *g_s;
    s.subs.clear();
    s.sub_external = -1;
    s.sub_stream = -1;
    if (index < 0) return;
    if (index >= 1000) {
        size_t i = index - 1000;
        if (i >= s.src.external_subs.size()) return;
        std::string u = s.src.external_subs[i].second;
        std::shared_ptr<Session> sp = g_s;
        s.sub_external = (int)i;
        std::thread([sp, u]() {
            cpu::ThreadTag tag("subtitles");
            std::string data;
            bool ok = util::starts_with(u, "http") ? [&] {
                http::Response r = http::get(u, {}, 15);
                data = std::move(r.body);
                return r.ok();
            }() : smb::is_url(u) ? smb::read_file(u, data) : util::read_file(u, data);
            if (ok && !sp->abort) sp->subs.load(data);
        }).detach();
        return;
    }
    // Embedded: open a decoder; cues arrive as packets are demuxed, so seek
    // back slightly to pick up the current line.
    AVFormatContext* f0 = s.in[0].fmt;
    if (!f0 || index >= (int)f0->nb_streams) return;
    if (s.sdec) avcodec_free_context(&s.sdec);
    AVStream* st = f0->streams[index];
    const AVCodec* c = avcodec_find_decoder(st->codecpar->codec_id);
    if (!c) return;
    s.sdec = avcodec_alloc_context3(c);
    avcodec_parameters_to_context(s.sdec, st->codecpar);
    s.sdec->pkt_timebase = st->time_base;
    if (avcodec_open2(s.sdec, c, nullptr) < 0) {
        avcodec_free_context(&s.sdec);
        return;
    }
    f0->streams[index]->discard = AVDISCARD_DEFAULT;
    s.sub_stream = index;
    if (s.seekable) seek_at(position(), false);
}

std::string subtitle_text() {
    if (!g_s || (g_s->sub_stream < 0 && g_s->sub_external < 0)) return "";
    return g_s->subs.at(position());
}

void mark_downloads(Session& s) {
    for (int i = 0; i < s.inputs; i++)
        if (s.in[i].fmt) http_io_stats(s.in[i].fmt->pb, s.io_mark[i]);
}

// What the downloads of chunked inputs did since mark_downloads() `window` seconds ago, for the
// log ("" without any): what came in, what is in ahead of the reader, and what held it up.
std::string download_report(Session& s, double window) {
    std::string out;
    for (int i = 0; i < s.inputs; i++) {
        HttpIoStats now;
        if (!s.in[i].fmt || !http_io_stats(s.in[i].fmt->pb, now)) continue;
        const HttpIoStats& was = s.io_mark[i];
        out += util::fmt("%s%s %.0f KB/s, %.1f MB in ahead", out.empty() ? "" : "; ", s.in[i].video >= 0 ? "video" : "audio",
                         (now.downloaded - was.downloaded) / 1024.0 / std::max(window, 0.1), now.ready / 1048576.0);
        std::string held;
        auto add = [&held](int n, const char* one, const char* more) {
            if (n > 0) held += util::fmt("%s%d %s", held.empty() ? "" : ", ", n, n == 1 ? one : more);
        };
        add(now.requests - was.requests, "request", "requests");
        add(now.connects - was.connects, "new connection", "new connections");
        add(now.quiet_restarts - was.quiet_restarts, "gone quiet", "gone quiet");
        add(now.retries - was.retries, "failed", "failed");
        if (!held.empty()) out += " (" + held + ")";
    }
    return out;
}

// Every 10 s of playing, logs what happened to the pictures and where the time went: decoding (on
// the video thread), showing (putting pictures into the texture), screen updates that took longer
// than a refresh, and how much media came in.
void playback_stats(Session& s, double t, int st) {
    auto& p = s.stats;
    if (st != PLAYING || s.seeking) {
        p = {};
        s.decode_us = s.decoded = s.dropped = s.left_out = s.bytes_read = 0;
        s.tail_out = s.tails = s.tail_ms_max = 0;
        return;
    }
    if (p.since <= 0) {
        p.since = t;
        mark_downloads(s);
    }
    if (p.last_update > 0) {
        double gap = t - p.last_update;
        p.updates++;
        if (gap > 0.025) p.slow_updates++;
        p.gap_max = std::max(p.gap_max, gap);
    }
    p.last_update = t;
    if (t - p.since < 10) return;
    int decoded = s.decoded.exchange(0), decode_us = s.decode_us.exchange(0), dropped = s.dropped.exchange(0);
    int left_out = s.left_out.exchange(0);
    int tail_out = s.tail_out.exchange(0), tails = s.tails.exchange(0), tail_ms = s.tail_ms_max.exchange(0);
    s.last = {(float)(p.shown / (t - p.since)), decoded ? (float)(decode_us / 1e3 / decoded) : 0.0f, left_out, dropped, p.late};
    std::string downloads = download_report(s, t - p.since);
    std::string rest = util::fmt("%d of %d screen updates late (longest %.0f ms); %zu packets waiting, %.1f s of audio, "
                                 "%.0f KB/s read",
                                 p.slow_updates, p.updates, p.gap_max * 1e3, s.has_video ? s.vq.count() : s.aq.count(),
                                 audio::stream_buffered_seconds(), s.bytes_read.exchange(0) / 1024.0 / (t - p.since));
    if (!downloads.empty()) rest += "; downloads: " + downloads;
    if (s.has_video)
        log_message(LOG_OK, "Player",
                    "Playback: %d pictures shown in %.0f s (%d without a copy), %d late, %d dropped after decoding%s; "
                    "decoding %.1f ms per packet; showing %.1f ms (longest %.1f); %s",
                    p.shown, t - p.since, p.bound, p.late, dropped,
                    left_out ? util::fmt(", %d left out (30 fps, or for the decoder%s)", left_out,
                                         tails ? util::fmt(": %d before %d keyframes, the longest %d ms", tail_out,
                                                           tails, tail_ms).c_str()
                                               : "").c_str()
                             : "",
                    decoded ? decode_us / 1e3 / decoded : 0.0,
                    p.shown ? p.show_time * 1e3 / p.shown : 0.0, p.show_max * 1e3, rest.c_str());
    else
        log_message(LOG_OK, "Player", "Playback: %.0f s of sound; %s", t - p.since, rest.c_str());
    p = {};
    p.since = p.last_update = t;
    mark_downloads(s);
}

void update() {
    // Screens' own downloads (a feed can be a couple of MB) wait while a video starts, so it
    // has the Wi-Fi to itself. At most 20 s, in case it never does.
    static bool holding = false;
    bool starting = g_s && !g_s->started && (g_s->state == OPENING || g_s->state == BUFFERING) &&
                    now() - g_s->created < 20;
    if (starting != holding) {
        holding = starting;
        tasks::hold(tasks::API, starting);
    }

    if (!g_s) return;
    std::shared_ptr<Session> sp = g_s;
    Session& s = *sp;
    int st = s.state;
    double t = now();
    if (st == OPENING && t - s.created > 60 && !s.abort) {
        s.abort = true;  // FFmpeg's interrupt callback ends whatever the opener is waiting for
        set_error(s, tr(STEP_TIMEOUTS[std::clamp((int)s.step, 0, 3)]));
        return;
    }
    if (st == OPENING || st == FAILED || st == IDLE) return;
    measure_network(s, t);
    sample_download_rate(s, t);
    if (decoder_step_down(s)) return;

    if (st == BUFFERING) {
        bool audio_ok = !s.has_audio || audio::stream_buffered_seconds() >= (s.in[0].remote ? 0.6 : 0.25) ||
                        s.audio_eof || s.aq.drained();
        bool video_ok = !s.has_video || s.video_eof || s.vq.drained();
        {
            std::lock_guard<std::mutex> lk(s.fm);
            if (!s.frames.empty()) video_ok = true;
        }
        // After the pictures ran out, one isn't enough: it would run out again at once. Nor
        // from the network at a start or a seek.
        if (s.video_starved) video_ok = video_refilled(s, refill_target(s));
        else if (video_ok && s.has_video && s.in[0].remote && !s.live) video_ok = video_refilled(s, START_CUSHION);
        if (audio_ok && video_ok) {
            // A start or a seek plays from where it went: a resume from the keyframe before the
            // spot asked for (the pictures would seem late until the sound's clock runs).
            if (!s.started || s.seeking) s.last_clock = s.seek_target;
            if (!s.started)
                log_message(LOG_OK, "Player", "Started in %.1f s: %sbuffering %.1f s", t - s.created,
                            s.startup.c_str(), t - s.buffering_since);
            else if (s.starved) {
                log_message(LOG_OK, "Player", "Playing again after %.1f s (%.1f s of video in)", t - s.buffering_since,
                            s.has_video ? video_ready(s) : 0.0);
                s.waited += t - s.buffering_since;
            } else if (s.seeking) {
                const double asked = s.seek_asked, from = s.seek_target;
                log_message(LOG_OK, "Player", "Jumped to %s%s in %.1f s", util::format_duration(asked).c_str(),
                            from < asked - 0.05 ? util::fmt(" (from its keyframe %.1f s before)", asked - from).c_str() : "",
                            t - s.buffering_since);
            }
            s.started = true;
            s.refilled = s.starved;
            s.starved = false;
            s.video_starved = false;
            s.video_dry_since = 0;
            s.underrun = 0;
            s.seeking = false;
            s.playing_since = t;
            if (s.user_paused) {
                s.state = PAUSED;
            } else {
                s.state = PLAYING;
                audio::stream_pause(false);
                start_wall(s, s.seek_target > 0 ? (double)s.seek_target : s.last_clock);
            }
            audio::stream_take_underrun();
            st = s.state;
        }
    } else if (st == PLAYING) {
        // Downloads slower than the stream feed the sound in dribs: gaps too short to notice one
        // by one, and the sound's clock goes at the download's pace. Adds them up for a moment.
        if (double under = audio::stream_take_underrun(); under > 0) {
            if (t - s.underrun_since > 2) s.underrun = 0, s.underrun_since = t;
            s.underrun += under;
        }
        bool audio_starved = s.has_audio && s.underrun > 0.15 && !s.audio_eof && !s.aq.drained() && s.aq.count() == 0;
        bool video_starved = false;
        if (s.has_video && !s.video_eof && !s.vq.drained()) {
            bool dry;
            {
                std::lock_guard<std::mutex> lk(s.fm);
                dry = s.frames.empty() && s.vq.count() == 0;
            }
            if (!s.has_audio) {
                video_starved = dry;
            } else if (s.in[0].remote) {
                // The sound keeps the clock going when the pictures stop coming in: the picture
                // froze while the sound played on, and then skipped seconds ahead to catch up.
                // Wait for it instead, as for the sound (after a moment: a dry frame or two
                // passes unseen).
                if (!dry) s.video_dry_since = 0;
                else if (s.video_dry_since <= 0) s.video_dry_since = t;
                else video_starved = t - s.video_dry_since >= 0.2;
            }
        }
        if (audio_starved || video_starved) {
            double window = s.stats.since > 0 ? t - s.stats.since : 0;
            std::string downloads = window > 0 ? download_report(s, window) : "";
            log_message(LOG_WARNING, "Player", "Ran out of %s after %.0f s%s: in the last %.0f s %s",
                        audio_starved ? "audio" : "video", t - s.playing_since,
                        s.has_video ? util::fmt(" (%zu video packets waiting)", s.vq.count()).c_str() : "", window,
                        !downloads.empty() ? downloads.c_str()
                                           : util::fmt("%.0f KB/s read", window > 0 ? s.bytes_read / 1024.0 / window : 0.0).c_str());
            s.starved = true;
            s.waits++;
            // Sound and pictures from one file over the network: both ran out, so it waits for the
            // video as when the pictures run out.
            s.video_starved = video_starved || (s.has_video && s.audio_input == 0 && s.in[0].remote);
            s.state = BUFFERING;
            s.buffering_since = t;
            audio::stream_pause(true);
            s.wall_running = false;
            st = BUFFERING;
            // Starts and seeks play on from the first picture, so running out just after one
            // is the download catching up rather than falling behind.
            if (s.refilled || t - s.playing_since >= 5) {
                s.stalls++;
                if (auto_step_down(s, t)) return;
                const double target = refill_target(s);
                if (s.video_starved) keep_ahead(s, target);
                if (s.video_starved && target > VIDEO_CUSHION * std::min(s.stalls, 5))
                    log_message(LOG_WARNING, "Player",
                                "Downloads get %.0f KB/s, the stream takes %d kbps: waiting for %.0f s of video to play on",
                                s.net_rate / 1024, s.src.bitrate / 1000, target);
            }
        }
    }

    if ((st == BUFFERING || st == PLAYING) && s.has_video && decoder_watchdog(s, t)) return;
    if (st == BUFFERING && buffering_watchdog(s, t)) return;

    double clock = master_clock(s);
    s.clock_now = clock;
    s.clock_running = st == PLAYING && !s.seeking;

    // Video: drop late frames, show the one that is due.
    if (s.has_video) {
        std::unique_ptr<VFrame> show;
        {
            std::lock_guard<std::mutex> lk(s.fm);
            while (!s.frames.empty() && s.frames.front()->gen != s.gen) {
                recycle(s, std::move(s.frames.front()));
                s.frames.pop_front();
            }
            bool first = g_shown_gen != s.gen;  // nothing shown since open/seek
            if (st == PLAYING) {
                while (s.frames.size() >= 2 && s.frames[1]->pts <= clock) {
                    recycle(s, std::move(s.frames.front()));
                    s.frames.pop_front();
                    s.stats.late++;
                }
                if (!s.frames.empty() && (s.frames.front()->pts <= clock + 0.004 || first)) {
                    show = std::move(s.frames.front());
                    s.frames.pop_front();
                }
            } else if (first && !s.frames.empty()) {
                // Paused/buffering after a seek: show the new position.
                show = std::move(s.frames.front());
                s.frames.pop_front();
            }
        }
        if (show) {
            if (!s.has_audio && g_shown_gen != s.gen && st == PLAYING) start_wall(s, show->pts);
            double start = now();
            bool bound = upload_frame(*show);
            double took = now() - start;
            s.stats.shown++;
            s.stats.bound += bound;
            s.stats.show_time += took;
            s.stats.show_max = std::max(s.stats.show_max, took);
            std::lock_guard<std::mutex> lk(s.fm);
            recycle(s, std::move(show));
        }
        s.fcv.notify_all();
    }
    playback_stats(s, t, st);

    // End of stream.
    if (st == PLAYING) {
        bool inputs_done = true;
        for (int i = 0; i < s.inputs; i++) inputs_done = inputs_done && s.in[i].eof;
        bool frames_empty;
        {
            std::lock_guard<std::mutex> lk(s.fm);
            frames_empty = s.frames.empty();
        }
        bool v_done = !s.has_video || (s.vq.drained() && frames_empty && s.video_eof);
        bool a_done = !s.has_audio || (s.aq.drained() && s.audio_eof && audio::stream_buffered_seconds() < 0.03);
        if (inputs_done && v_done && a_done) {
            s.state = ENDED;
            audio::stream_pause(true);
            if (s.src.remember_position) store::resume_remove(s.src.service, s.src.id);
            if (s.src.on_stop) s.src.on_stop(s.duration, true);
            log_message(LOG_OK, "Player", "Finished: %s", s.src.title.c_str());
            if (has_next()) next();
            return;
        }
    }
    if (st == PLAYING && s.seekable && !s.src.skip_segments.empty()) {
        s.skipped.resize(s.src.skip_segments.size(), 0);
        for (size_t i = 0; i < s.src.skip_segments.size(); i++) {
            const SkipSegment& seg = s.src.skip_segments[i];
            // Once per pass: seeking back into a skipped part plays it, going back before it re-arms it.
            if (clock < seg.start - 1) s.skipped[i] = 0;
            if (s.skipped[i] || clock < seg.start || clock >= seg.end - 0.5) continue;
            s.skipped[i] = 1;
            s.skip_notice = seg.label;
            log_message(LOG_OK, "Player", "%s (%.1f -> %.1f)", seg.label.c_str(), clock, seg.end);
            seek_at(seg.end, false);
            return;
        }
    }
    if (st == PLAYING) report_progress(s, false);
}

std::string take_skip_notice() {
    if (!g_s) return "";
    std::string n;
    n.swap(g_s->skip_notice);
    return n;
}

std::string take_quality_notice() {
    std::string n;
    n.swap(g_quality_notice);
    return n;
}

SDL_Texture* video_texture() { return g_s && g_shown_gen != 0 ? g_tex : nullptr; }

SDL_Rect fit_rect(int area_w, int area_h) {
    int w = g_tex_w, h = g_tex_h;
    if (w <= 0 || h <= 0) return SDL_Rect{0, 0, area_w, area_h};
    float s = std::min((float)area_w / w, (float)area_h / h);
    int dw = (int)(w * s), dh = (int)(h * s);
    return SDL_Rect{(area_w - dw) / 2, (area_h - dh) / 2, dw, dh};
}

}  // namespace player
