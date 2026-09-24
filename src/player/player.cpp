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
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "audio/mixer.hpp"
#include "core/http.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/gfx.hpp"
#include "gfx/images.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"
#include "player/convert.hpp"
#include "player/smb_io.hpp"
#include "player/subtitles.hpp"
#include "services/smb.hpp"

namespace player {

namespace {

constexpr size_t VIDEO_QUEUE_BYTES_WIIU = 14u << 20;
constexpr size_t VIDEO_QUEUE_BYTES_DESKTOP = 48u << 20;
constexpr size_t AUDIO_QUEUE_BYTES = 3u << 20;

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
    std::vector<uint8_t> rgba;
};

struct Input {
    AVFormatContext* fmt = nullptr;
    std::thread thread;
    std::atomic<bool> seek_req{false};
    std::atomic<bool> eof{false};
    int video = -1, audio = -1;
    bool network = false;
};

struct Session {
    uint32_t id = 0;
    Source src;
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
    double duration = 0;
    bool seekable = true;
    bool live = false;
    bool has_video = false, has_audio = false;
    int vw = 0, vh = 0;
    std::string codec;

    std::mutex meta_m;
    std::string icy, art_key;
    std::vector<Track> audio_tracks;
    std::vector<Track> sub_tracks;
    int sub_stream = -1;       // embedded stream index in input 0, or -1
    int sub_external = -1;     // index into src.external_subs
    Subtitles subs;

    // playback clock when there is no audio
    double wall_base_pts = 0, wall_base_time = 0;
    bool wall_running = false;
    double last_clock = 0;
    double buffering_since = 0;
    double last_progress_report = 0;
    bool user_paused = false;
};

std::shared_ptr<Session> g_s;
uint32_t g_next_id = 1;
std::atomic<int> g_closers{0};
std::string g_empty;
Source g_empty_src;

SDL_Texture* g_tex = nullptr;
int g_tex_w = 0, g_tex_h = 0;
uint32_t g_shown_gen = 0;
double g_shown_pts = -1;

std::vector<Source> g_queue;
int g_queue_index = -1;

void set_error(Session& s, const std::string& e) {
    {
        std::lock_guard<std::mutex> lk(s.err_m);
        s.error = e;
    }
    s.state = FAILED;
    log_message(LOG_ERROR, "Player", "%s", e.c_str());
}

int interrupt_cb(void* opaque) { return ((std::atomic<bool>*)opaque)->load() ? 1 : 0; }

std::string av_err(int e) {
    char buf[128];
    av_strerror(e, buf, sizeof(buf));
    return buf;
}

double ts_to_sec(int64_t ts, AVRational tb) { return ts == AV_NOPTS_VALUE ? -1 : ts * av_q2d(tb); }

AVFormatContext* open_input(Session& s, const std::string& url, bool& network) {
    AVFormatContext* fmt = avformat_alloc_context();
    fmt->interrupt_callback.callback = interrupt_cb;
    fmt->interrupt_callback.opaque = &s.abort;
    network = util::starts_with(url, "http://") || util::starts_with(url, "https://");
    if (smb::is_url(url)) {  // FFmpeg has no SMB protocol: custom I/O
        std::string err;
        if (!(fmt->pb = smb_io_open(url, &s.abort, err))) {
            avformat_free_context(fmt);
            if (!s.abort) set_error(s, err);
            return nullptr;
        }
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    }
    AVIOContext* custom_pb = fmt->pb;  // not freed by avformat_open_input() on failure

    AVDictionary* opts = nullptr;
    if (network) {
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
        smb_io_free(custom_pb);
        if (!s.abort) set_error(s, "Couldn't open stream (" + av_err(r) + ")");
        return nullptr;
    }
    r = avformat_find_stream_info(fmt, nullptr);
    if (r < 0 && !s.abort) log_message(LOG_WARNING, "Player", "find_stream_info: %s", av_err(r).c_str());
    return fmt;
}

AVCodecContext* open_decoder(AVStream* st, bool allow_hw, bool* used_hw) {
    const AVCodec* codec = nullptr;
    if (used_hw) *used_hw = false;
    AVCodecParameters* p = st->codecpar;
    if (allow_hw && p->codec_id == AV_CODEC_ID_H264 && p->width <= 1920 && p->height <= 1088 &&
        p->format != AV_PIX_FMT_YUV420P10LE && p->profile != FF_PROFILE_H264_HIGH_10 &&
        p->profile != FF_PROFILE_H264_HIGH_422 && p->profile != FF_PROFILE_H264_HIGH_444_PREDICTIVE) {
        codec = avcodec_find_decoder_by_name("h264_wiiu");
        if (codec && used_hw) *used_hw = true;
    }
    if (!codec) codec = avcodec_find_decoder(p->codec_id);
    if (!codec) return nullptr;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, p);
    ctx->pkt_timebase = st->time_base;
    if (!(used_hw && *used_hw) && p->codec_type == AVMEDIA_TYPE_VIDEO) {
        ctx->thread_count = platform::is_wiiu() ? 3 : 0;
        ctx->thread_type = FF_THREAD_SLICE | FF_THREAD_FRAME;
    }
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
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

std::string stream_label(AVStream* st, int n) {
    std::string lang, title;
    if (AVDictionaryEntry* e = av_dict_get(st->metadata, "language", nullptr, 0)) lang = e->value;
    if (AVDictionaryEntry* e = av_dict_get(st->metadata, "title", nullptr, 0)) title = e->value;
    std::string label = !title.empty() ? title : !lang.empty() ? util::lower(lang) : util::fmt("Track %d", n);
    if (!title.empty() && !lang.empty() && title.find(lang) == std::string::npos) label += " (" + lang + ")";
    const char* cname = avcodec_get_name(st->codecpar->codec_id);
    if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        int ch = st->codecpar->channels;
        label += util::fmt(" \xC2\xB7 %s %s", util::lower(cname).c_str(), ch >= 6 ? "5.1" : ch == 2 ? "stereo" : ch == 1 ? "mono" : "");
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
    if (!fmt->pb) return;
    uint8_t* meta = nullptr;
    if (av_opt_get(fmt->pb, "icy_metadata_packet", AV_OPT_SEARCH_CHILDREN, &meta) < 0 || !meta) return;
    std::string m = (const char*)meta;
    av_free(meta);
    size_t p = m.find("StreamTitle='");
    if (p == std::string::npos) return;
    p += 13;
    size_t e = m.find("';", p);
    std::string title = m.substr(p, e == std::string::npos ? std::string::npos : e - p);
    std::lock_guard<std::mutex> lk(s.meta_m);
    if (title != s.icy) {
        s.icy = title;
        log_message(LOG_OK, "Player", "Now playing: %s", title.c_str());
    }
}

void demux_loop(std::shared_ptr<Session> sp, int idx) {
    Session& s = *sp;
    Input& in = s.in[idx];
    AVPacket* pkt = av_packet_alloc();
    size_t vmax = platform::is_wiiu() ? VIDEO_QUEUE_BYTES_WIIU : VIDEO_QUEUE_BYTES_DESKTOP;
    double last_icy = 0;

    while (!s.abort) {
        if (in.seek_req.exchange(false)) {
            double t = s.seek_target;
            int64_t ts = (int64_t)(t * AV_TIME_BASE);
            if (in.fmt->start_time != AV_NOPTS_VALUE) ts += in.fmt->start_time;
            int r = avformat_seek_file(in.fmt, -1, INT64_MIN, ts, ts, 0);
            if (r < 0) r = avformat_seek_file(in.fmt, -1, INT64_MIN, ts, INT64_MAX, AVSEEK_FLAG_BACKWARD);
            if (r < 0) log_message(LOG_WARNING, "Player", "Seek failed: %s", av_err(r).c_str());
            if (in.video >= 0) s.vq.flush();
            if (in.audio >= 0) s.aq.flush();
            in.eof = false;
        }

        // Back-pressure: stop reading when enough is buffered.
        bool v_full = in.video >= 0 && s.vq.bytes > vmax;
        bool a_full = in.audio >= 0 && s.aq.bytes > AUDIO_QUEUE_BYTES;
        bool v_enough = in.video < 0 || s.vq.count() > 90 || v_full;
        bool a_enough = in.audio < 0 || s.aq.count() > 160 || a_full;
        if (v_full || a_full || (v_enough && a_enough) || in.eof) {
            std::this_thread::sleep_for(std::chrono::milliseconds(in.eof ? 30 : 8));
            if (in.network && now() - last_icy > 1.5) {
                poll_icy(s, in.fmt);
                last_icy = now();
            }
            continue;
        }

        int r = av_read_frame(in.fmt, pkt);
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

        uint32_t gen = s.gen;
        AVStream* st = in.fmt->streams[pkt->stream_index];
        if (pkt->stream_index == in.video) {
            s.vq.push(av_packet_clone(pkt), gen, ts_to_sec(pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts, st->time_base));
        } else if (pkt->stream_index == in.audio) {
            s.aq.push(av_packet_clone(pkt), gen, ts_to_sec(pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts, st->time_base));
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

void video_loop(std::shared_ptr<Session> sp) {
    Session& s = *sp;
    FrameConverter conv;
    AVFrame* frame = av_frame_alloc();
    uint32_t dec_gen = s.gen;
    bool drained = false;
    int64_t frame_count = 0;
    double fps = 0;
    AVStream* st = s.in[0].fmt->streams[s.in[0].video];
    if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) fps = av_q2d(st->avg_frame_rate);

    while (!s.abort) {
        QPacket qp{nullptr, 0};
        if (!s.vq.pop(qp, s.abort, 50)) {
            if (s.vq.drained() && !drained && !s.abort) {
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
            }
            int r = avcodec_send_packet(s.vdec, qp.pkt);
            av_packet_free(&qp.pkt);
            if (r < 0 && r != AVERROR(EAGAIN)) continue;
        }

        for (;;) {
            int r = avcodec_receive_frame(s.vdec, frame);
            if (r == AVERROR_EOF) {
                s.video_eof = true;
                avcodec_flush_buffers(s.vdec);
                break;
            }
            if (r < 0) break;
            int64_t ts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
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

            std::unique_ptr<VFrame> vf;
            {
                std::unique_lock<std::mutex> lk(s.fm);
                s.fcv.wait(lk, [&] { return s.abort || s.frames.size() < s.max_frames || dec_gen != s.gen; });
                if (s.abort) break;
                if (!s.spare.empty()) {
                    vf = std::move(s.spare.back());
                    s.spare.pop_back();
                }
            }
            if (dec_gen != s.gen) {
                av_frame_unref(frame);
                continue;
            }
            if (!vf) vf = std::make_unique<VFrame>();
            vf->w = frame->width;
            vf->h = frame->height;
            vf->pts = pts;
            vf->gen = dec_gen;
            vf->rgba.resize((size_t)vf->w * vf->h * 4);
            bool ok = conv.convert(frame, vf->rgba.data(), vf->w * 4);
            av_frame_unref(frame);
            if (!ok) continue;
            std::lock_guard<std::mutex> lk(s.fm);
            s.frames.push_back(std::move(vf));
        }
    }
    av_frame_free(&frame);
}

void audio_loop(std::shared_ptr<Session> sp) {
    Session& s = *sp;
    AVFrame* frame = av_frame_alloc();
    SwrContext* swr = nullptr;
    int64_t swr_layout = 0;
    int swr_rate = 0, swr_fmt = -1;
    std::vector<int16_t> buf;
    uint32_t dec_gen = s.gen;
    bool drained = false;
    double next_pts = 0;
    Input& in = s.in[s.audio_input];
    double start_offset = in.fmt->start_time != AV_NOPTS_VALUE ? in.fmt->start_time / (double)AV_TIME_BASE : 0;

    while (!s.abort) {
        QPacket qp{nullptr, 0};
        if (!s.aq.pop(qp, s.abort, 50)) {
            if (s.aq.drained() && !drained && !s.abort) {
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
                s.audio_eof = true;
                avcodec_flush_buffers(s.adec);
                break;
            }
            if (r < 0) break;
            s.audio_eof = false;
            int64_t ts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
            double pts = ts != AV_NOPTS_VALUE ? ts_to_sec(ts, s.atb) - start_offset : next_pts;
            next_pts = pts + (double)frame->nb_samples / frame->sample_rate;

            int64_t layout = frame->channel_layout ? (int64_t)frame->channel_layout
                                                   : av_get_default_channel_layout(frame->channels);
            if (!swr || layout != swr_layout || frame->sample_rate != swr_rate || frame->format != swr_fmt) {
                swr_free(&swr);
                swr = swr_alloc_set_opts(nullptr, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, audio::RATE, layout,
                                         (AVSampleFormat)frame->format, frame->sample_rate, 0, nullptr);
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
            size_t written = 0;
            while (written < (size_t)n && !s.abort && dec_gen == s.gen) {
                size_t w = audio::stream_write(p + written * 2, n - written, pts + (double)written / audio::RATE,
                                               (s.id << 16) | (dec_gen & 0xFFFF));
                written += w;
                if (w == 0) std::this_thread::sleep_for(std::chrono::milliseconds(8));
            }
        }
    }
    swr_free(&swr);
    av_frame_free(&frame);
}

void open_session(std::shared_ptr<Session> sp) {
    Session& s = *sp;
    bool net0 = false, net1 = false;
    s.in[0].fmt = open_input(s, s.src.url, net0);
    if (!s.in[0].fmt) return;
    s.in[0].network = net0;
    s.inputs = 1;
    if (!s.src.audio_url.empty()) {
        s.in[1].fmt = open_input(s, s.src.audio_url, net1);
        if (!s.in[1].fmt) return;
        s.in[1].network = net1;
        s.inputs = 2;
    }
    if (s.abort) return;

    AVFormatContext* f0 = s.in[0].fmt;
    int v = av_find_best_stream(f0, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (v >= 0 && (f0->streams[v]->disposition & AV_DISPOSITION_ATTACHED_PIC)) v = -1;
    s.in[0].video = v;

    // Audio tracks of the primary input.
    int n_audio = 0;
    for (unsigned i = 0; i < f0->nb_streams; i++) {
        AVStream* st = f0->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) s.audio_tracks.push_back(Track{(int)i, stream_label(st, ++n_audio)});
        if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            AVCodecID id = st->codecpar->codec_id;
            if (id == AV_CODEC_ID_SUBRIP || id == AV_CODEC_ID_TEXT || id == AV_CODEC_ID_ASS || id == AV_CODEC_ID_SSA ||
                id == AV_CODEC_ID_MOV_TEXT || id == AV_CODEC_ID_WEBVTT)
                s.sub_tracks.push_back(Track{(int)i, stream_label(st, (int)s.sub_tracks.size() + 1)});
        }
    }
    for (size_t i = 0; i < s.src.external_subs.size(); i++)
        s.sub_tracks.push_back(Track{1000 + (int)i, s.src.external_subs[i].first});

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
        set_error(s, "No playable audio or video found");
        return;
    }

    std::string vinfo, ainfo;
    if (v >= 0) {
        AVStream* st = f0->streams[v];
        s.vdec = open_decoder(st, platform::is_wiiu(), &s.hw);
        if (!s.vdec) {
            if (ain.audio < 0) {
                set_error(s, util::fmt("Unsupported video codec (%s)", avcodec_get_name(st->codecpar->codec_id)));
                return;
            }
            s.in[0].video = -1;  // play the audio at least
        } else {
            s.vtb = st->time_base;
            s.has_video = true;
            s.vw = st->codecpar->width;
            s.vh = st->codecpar->height;
            s.max_frames = (size_t)s.vw * s.vh > 1280 * 720 ? 3 : 5;
            vinfo = util::fmt("%s \xC2\xB7 %d\xC3\x97%d%s", codec_display_name(st->codecpar->codec_id), s.vw, s.vh,
                              s.hw ? " \xC2\xB7 HW" : "");
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
        set_error(s, "Unsupported codecs");
        return;
    }
    s.codec = vinfo.empty() ? ainfo : ainfo.empty() ? vinfo : vinfo + " \xC2\xB7 " + ainfo;

    if (f0->duration > 0) s.duration = f0->duration / (double)AV_TIME_BASE;
    else if (s.src.live) s.duration = 0;
    bool pb_seekable = f0->pb ? (f0->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0 : true;
    s.live = s.src.live || (s.duration <= 0 && !pb_seekable) || (s.duration <= 0 && s.in[0].network);
    s.seekable = !s.live && s.duration > 0;

    fill_metadata(s, f0);
    extract_artwork(s, f0);

    // External subtitles (downloaded or read now so switching is instant).
    if (!s.src.external_subs.empty()) {
        std::string data;
        const std::string& u = s.src.external_subs[0].second;
        bool ok = util::starts_with(u, "http") ? [&] {
            http::Response r = http::get(u, {}, 15);
            data = std::move(r.body);
            return r.ok();
        }() : smb::is_url(u) ? smb::read_file(u, data) : util::read_file(u, data);
        if (ok && store::get_bool("subs_default_on", true)) {
            s.subs.load(data);
            s.sub_external = 0;
        }
    }

    if (s.src.start > 1 && s.seekable) {
        s.seek_target = s.src.start;
        for (int i = 0; i < s.inputs; i++) {
            int64_t ts = (int64_t)(s.src.start * AV_TIME_BASE);
            if (s.in[i].fmt->start_time != AV_NOPTS_VALUE) ts += s.in[i].fmt->start_time;
            avformat_seek_file(s.in[i].fmt, -1, INT64_MIN, ts, ts, 0);
        }
    } else {
        s.seek_target = 0;
    }

    log_message(LOG_OK, "Player", "Opened: %s (%s)%s", s.src.title.c_str(), s.codec.c_str(), s.live ? " [live]" : "");
    if (s.abort) return;
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
    if (s.vdec) avcodec_free_context(&s.vdec);
    if (s.adec) avcodec_free_context(&s.adec);
    if (s.sdec) avcodec_free_context(&s.sdec);
    for (auto& in : s.in)
        if (in.fmt) close_input(&in.fmt);  // also frees custom (smb://) I/O
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
        s.last_clock = s.wall_base_pts + (now() - s.wall_base_time);
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

void upload_frame(VFrame& f) {
    SDL_Renderer* r = gfx::renderer();
    if (!g_tex || g_tex_w != f.w || g_tex_h != f.h) {
        if (g_tex) SDL_DestroyTexture(g_tex);
        g_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, f.w, f.h);
        if (!g_tex) return;
        SDL_SetTextureBlendMode(g_tex, SDL_BLENDMODE_NONE);
        SDL_SetTextureScaleMode(g_tex, SDL_ScaleModeLinear);
        g_tex_w = f.w;
        g_tex_h = f.h;
    }
    SDL_UpdateTexture(g_tex, nullptr, f.rgba.data(), f.w * 4);
    g_shown_pts = f.pts;
    g_shown_gen = f.gen;
}

void recycle(Session& s, std::unique_ptr<VFrame> f) {
    if (s.spare.size() < 3) s.spare.push_back(std::move(f));
}

void start_session(const Source& src, int pref_audio) {
    close();
    auto s = std::make_shared<Session>();
    s->id = g_next_id++ & 0xFFFF;
    s->src = src;
    s->pref_audio = pref_audio;
    s->gen = 1;
    s->user_paused = false;
    s->last_clock = src.start;
    audio::stream_reset((s->id << 16) | 1);
    audio::stream_pause(true);
    g_shown_pts = -1;
    g_shown_gen = 0;
    g_s = s;
    s->opener = std::thread(open_session, s);
}

}  // namespace

void init() { avformat_network_init(); }

void shutdown() {
    close();
    for (int i = 0; i < 300 && g_closers > 0; i++) SDL_Delay(10);
    if (g_tex) SDL_DestroyTexture(g_tex);
    g_tex = nullptr;
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
    auto s = std::move(g_s);
    g_s.reset();
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
        teardown(s);
        s.reset();
        g_closers--;
    }).detach();
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

void seek(double t) {
    if (!g_s || !g_s->seekable) return;
    Session& s = *g_s;
    if (s.state == OPENING || s.state == FAILED) return;
    t = std::clamp(t, 0.0, std::max(0.0, s.duration - 0.5));
    uint32_t gen = s.gen + 1;
    s.seek_target = t;
    s.gen = gen;
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
    for (int i = 0; i < s.inputs; i++) s.in[i].seek_req = true;
    s.wall_running = false;
    s.state = BUFFERING;
    s.buffering_since = now();
    s.seeking = true;
}

void seek_relative(double delta) {
    if (!g_s) return;
    double base = g_s->seeking ? (double)g_s->seek_target : position();
    seek(base + delta);
}

State state() { return g_s ? (State)g_s->state.load() : IDLE; }

bool active() {
    State st = state();
    return st == OPENING || st == BUFFERING || st == PLAYING || st == PAUSED;
}

const std::string& error() {
    if (!g_s) return g_empty;
    std::lock_guard<std::mutex> lk(g_s->err_m);
    return g_s->error;
}

const Source& source() { return g_s ? g_s->src : g_empty_src; }

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
std::string codec_info() { return g_s ? g_s->codec : ""; }

float buffering_progress() {
    if (!g_s) return 0;
    double a = audio::stream_buffered_seconds();
    return (float)std::clamp(a / 0.6, 0.0, 1.0);
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

std::vector<Track> audio_tracks() { return g_s ? g_s->audio_tracks : std::vector<Track>{}; }

int audio_track() { return g_s ? g_s->in[0].audio : -1; }

void set_audio_track(int index) {
    if (!g_s || g_s->inputs != 1 || index == g_s->in[0].audio) return;
    Source src = g_s->src;
    src.start = position();
    auto queue = std::move(g_queue);
    int qi = g_queue_index;
    start_session(src, index);
    g_queue = std::move(queue);
    g_queue_index = qi;
}

std::vector<Track> subtitle_tracks() { return g_s ? g_s->sub_tracks : std::vector<Track>{}; }

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
    if (s.seekable) seek(position());
}

std::string subtitle_text() {
    if (!g_s || (g_s->sub_stream < 0 && g_s->sub_external < 0)) return "";
    return g_s->subs.at(position());
}

void update() {
    if (!g_s) return;
    std::shared_ptr<Session> sp = g_s;
    Session& s = *sp;
    int st = s.state;
    if (st == OPENING || st == FAILED || st == IDLE) return;

    double t = now();
    if (st == BUFFERING) {
        bool audio_ok = !s.has_audio || audio::stream_buffered_seconds() >= (s.in[0].network ? 0.6 : 0.25) ||
                        s.audio_eof || s.aq.drained();
        bool video_ok = !s.has_video || s.video_eof || s.vq.drained();
        {
            std::lock_guard<std::mutex> lk(s.fm);
            if (!s.frames.empty()) video_ok = true;
        }
        if (audio_ok && video_ok) {
            s.seeking = false;
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
        double under = audio::stream_take_underrun();
        bool audio_starved = s.has_audio && under > 0.15 && !s.audio_eof && !s.aq.drained() && s.aq.count() == 0;
        bool video_starved = false;
        if (s.has_video && !s.has_audio && !s.video_eof && !s.vq.drained()) {
            std::lock_guard<std::mutex> lk(s.fm);
            video_starved = s.frames.empty() && s.vq.count() == 0;
        }
        if (audio_starved || video_starved) {
            s.state = BUFFERING;
            s.buffering_since = t;
            audio::stream_pause(true);
            s.wall_running = false;
            st = BUFFERING;
        }
    }

    double clock = master_clock(s);

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
            upload_frame(*show);
            std::lock_guard<std::mutex> lk(s.fm);
            recycle(s, std::move(show));
        }
        s.fcv.notify_all();
    }

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
    if (st == PLAYING) report_progress(s, false);
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
