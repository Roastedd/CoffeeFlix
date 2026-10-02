#include "player/probe.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
}

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/i18n.hpp"
#include "core/json.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"

// The Wii U build has FFmpeg 4.3; the desktop preview the system's. FFmpeg 6.1 moved a stream's
// side data (the display matrix) into its codec parameters, and 7.0 dropped the old call.
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 31, 102)
#define HAVE_CODED_SIDE_DATA 1
#endif
#ifndef AV_PROFILE_H264_HIGH_10  // FFmpeg < 6.1 only has the FF_ names, 8.0 only the AV_ ones
#define AV_PROFILE_H264_HIGH_10 FF_PROFILE_H264_HIGH_10
#define AV_PROFILE_H264_HIGH_422 FF_PROFILE_H264_HIGH_422
#define AV_PROFILE_H264_HIGH_444_PREDICTIVE FF_PROFILE_H264_HIGH_444_PREDICTIVE
#define AV_PROFILE_H264_CAVLC_444 FF_PROFILE_H264_CAVLC_444
#endif

namespace player {

namespace {

// --- reading a file ----------------------------------------------------------------------------

const int32_t* display_matrix(const AVStream* st) {
#ifdef HAVE_CODED_SIDE_DATA
    const AVPacketSideData* sd = av_packet_side_data_get(st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data,
                                                         AV_PKT_DATA_DISPLAYMATRIX);
    return sd && sd->size >= 9 * sizeof(int32_t) ? (const int32_t*)sd->data : nullptr;
#elif LIBAVFORMAT_VERSION_MAJOR >= 59
    size_t size = 0;
    const uint8_t* data = av_stream_get_side_data(st, AV_PKT_DATA_DISPLAYMATRIX, &size);
    return data && size >= 9 * sizeof(int32_t) ? (const int32_t*)data : nullptr;
#else
    int size = 0;
    const uint8_t* data = av_stream_get_side_data(st, AV_PKT_DATA_DISPLAYMATRIX, &size);
    return data && size >= (int)(9 * sizeof(int32_t)) ? (const int32_t*)data : nullptr;
#endif
}

// The sound decoders the Wii U build has.
bool audio_decodable(const std::string& codec) {
    static const char* const names[] = {"aac", "ac3", "eac3", "mp3", "mp2", "flac", "vorbis", "opus", "alac", "wavpack"};
    for (const char* n : names)
        if (codec == n) return true;
    return util::starts_with(codec, "pcm_");
}

MediaInfo read_info(const std::string& path) {
    MediaInfo m;
    AVFormatContext* fmt = nullptr;
    // A small look is enough for what's in the file (the thumbnailer reads the same way).
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "probesize", "2097152", 0);
    av_dict_set(&opts, "analyzeduration", "2000000", 0);
    int r = avformat_open_input(&fmt, path.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (r < 0) return m;
    // The decoders it opens to fill in the details: one thread each, and no loop filter.
    std::vector<AVDictionary*> stream_opts(fmt->nb_streams, nullptr);
    for (AVDictionary*& d : stream_opts) {
        av_dict_set(&d, "threads", "1", 0);
        av_dict_set(&d, "skip_loop_filter", "all", 0);
    }
    r = avformat_find_stream_info(fmt, stream_opts.empty() ? nullptr : stream_opts.data());
    for (AVDictionary*& d : stream_opts) av_dict_free(&d);
    if (r < 0) {
        avformat_close_input(&fmt);
        return m;
    }
    m.ok = true;

    int v = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (v >= 0 && (fmt->streams[v]->disposition & AV_DISPOSITION_ATTACHED_PIC)) v = -1;  // cover art
    if (v >= 0) {
        const AVStream* st = fmt->streams[v];
        const AVCodecParameters* p = st->codecpar;
        m.video_codec = avcodec_get_name(p->codec_id);
        m.width = p->width;
        m.height = p->height;
        AVRational fr = st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0 ? st->avg_frame_rate : st->r_frame_rate;
        if (fr.num > 0 && fr.den > 0) {
            double f = av_q2d(fr);
            if (f > 0 && f <= 300) m.fps = (float)f;
        }
        const AVPixFmtDescriptor* d = p->format >= 0 ? av_pix_fmt_desc_get((AVPixelFormat)p->format) : nullptr;
        if (d) {
            m.bit_depth = d->comp[0].depth;
            m.chroma_420 = d->log2_chroma_w == 1 && d->log2_chroma_h == 1 && !(d->flags & AV_PIX_FMT_FLAG_RGB);
        } else if (p->bits_per_raw_sample > 8) {
            m.bit_depth = p->bits_per_raw_sample;
        }
        if (p->codec_id == AV_CODEC_ID_H264 && p->profile >= 0) {
            int idc = p->profile & 0xFF;  // without the constrained / intra flags
            m.pro_profile = idc == AV_PROFILE_H264_HIGH_10 || idc == AV_PROFILE_H264_HIGH_422 ||
                            idc == AV_PROFILE_H264_HIGH_444_PREDICTIVE || idc == AV_PROFILE_H264_CAVLC_444;
        }
        m.hdr = p->color_trc == AVCOL_TRC_SMPTE2084 || p->color_trc == AVCOL_TRC_ARIB_STD_B67;
        if (const int32_t* matrix = display_matrix(st)) {
            double cw = -av_display_rotation_get(matrix);  // it gives the angle counterclockwise
            if (!std::isnan(cw)) m.rotation = (int)(((std::lround(cw / 90.0) % 4) + 4) % 4);
        }
        if (fmt->duration <= 0 && st->duration > 0) m.duration = st->duration * av_q2d(st->time_base);
    }

    int best_audio = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, v, nullptr, 0);
    std::string played, first;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        const AVCodecParameters* p = fmt->streams[i]->codecpar;
        if (p->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        m.audio_tracks++;
        std::string name = avcodec_get_name(p->codec_id);
        if (first.empty() || (int)i == best_audio) first = name;
        if (audio_decodable(name)) {
            m.audio_supported = true;
            if (played.empty() || (int)i == best_audio) played = name;
        }
    }
    m.audio_codec = !played.empty() ? played : first;
    if (fmt->duration > 0) m.duration = fmt->duration / (double)AV_TIME_BASE;
    avformat_close_input(&fmt);
    return m;
}

// --- remembering results ---------------------------------------------------------------------
//
// media_info.json: {"version": 1, "files": {"<path>": [size, mtime, seq, ok, duration, video codec,
// audio codec, width, height, fps, bit depth, flags, rotation, audio tracks]}}. Only the facts:
// verdicts and reasons are worked out again when read (in the language showing then).

constexpr int CACHE_VERSION = 1;
constexpr size_t MAX_CACHED = 2000;  // the oldest tenth goes beyond that
constexpr int SAVE_EVERY = 24;       // new results written out together

enum Flags { F_HDR = 1, F_420 = 2, F_PRO = 4, F_AUDIO_OK = 8 };

struct Cached {
    uint64_t size = 0;
    int64_t mtime = 0;
    uint64_t seq = 0;  // order of arrival, for dropping the oldest
    MediaInfo info;    // without verdict and reason
};

std::mutex g_m;
std::mutex g_write_m;  // one writer at a time, in order
bool g_loaded = false;
std::unordered_map<std::string, Cached> g_cache;
uint64_t g_seq = 0;
int g_unsaved = 0;

std::string cache_path() { return util::join_path(platform::data_dir(), "media_info.json"); }

void load_locked() {
    if (g_loaded) return;
    g_loaded = true;
    std::string data;
    if (!util::read_file(cache_path(), data)) return;
    json::Doc doc = json::Doc::parse(data);
    if (json::num(doc.get(), {"version"}) != CACHE_VERSION) return;
    const char* key;
    json_t* a;
    json_object_foreach(json::at(doc.get(), {"files"}), key, a) {
        if (json::size(a) < 14) continue;
        auto at = [a](int i) { return json_array_get(a, (size_t)i); };
        Cached c;
        c.size = (uint64_t)json::num(at(0));
        c.mtime = json::num(at(1));
        c.seq = (uint64_t)json::num(at(2));
        MediaInfo& m = c.info;
        m.ok = json::boolean(at(3));
        m.duration = json::real(at(4));
        m.video_codec = json::str(at(5));
        m.audio_codec = json::str(at(6));
        m.width = (int)json::num(at(7));
        m.height = (int)json::num(at(8));
        m.fps = (float)json::real(at(9));
        m.bit_depth = (int)json::num(at(10), 8);
        int flags = (int)json::num(at(11));
        m.hdr = flags & F_HDR;
        m.chroma_420 = flags & F_420;
        m.pro_profile = flags & F_PRO;
        m.audio_supported = flags & F_AUDIO_OK;
        m.rotation = (int)json::num(at(12)) & 3;
        m.audio_tracks = (int)json::num(at(13));
        g_seq = std::max(g_seq, c.seq);
        g_cache[key] = std::move(c);
    }
}

std::string serialize_locked() {
    json_t* files = json_object();
    for (const auto& [path, c] : g_cache) {
        const MediaInfo& m = c.info;
        int flags = (m.hdr ? F_HDR : 0) | (m.chroma_420 ? F_420 : 0) | (m.pro_profile ? F_PRO : 0) |
                    (m.audio_supported ? F_AUDIO_OK : 0);
        json_t* a = json_pack("[IIIbfssiifiiii]", (json_int_t)c.size, (json_int_t)c.mtime, (json_int_t)c.seq, (int)m.ok,
                              std::round(m.duration * 100) / 100, m.video_codec.c_str(), m.audio_codec.c_str(), m.width,
                              m.height, std::round(m.fps * 1000) / 1000.0, m.bit_depth, flags, m.rotation, m.audio_tracks);
        if (a) json_object_set_new(files, path.c_str(), a);
    }
    json_t* root = json_pack("{s:i, s:o}", "version", CACHE_VERSION, "files", files);
    char* s = root ? json_dumps(root, JSON_COMPACT) : nullptr;
    std::string out = s ? s : "";
    free(s);
    if (root) json_decref(root);
    return out;
}

void evict_locked() {
    if (g_cache.size() <= MAX_CACHED) return;
    std::vector<uint64_t> seqs;
    seqs.reserve(g_cache.size());
    for (const auto& kv : g_cache) seqs.push_back(kv.second.seq);
    size_t drop = g_cache.size() - MAX_CACHED + MAX_CACHED / 10;
    std::nth_element(seqs.begin(), seqs.begin() + (drop - 1), seqs.end());
    uint64_t cutoff = seqs[drop - 1];
    for (auto it = g_cache.begin(); it != g_cache.end();) it = it->second.seq <= cutoff ? g_cache.erase(it) : std::next(it);
}

bool file_stamp(const std::string& path, uint64_t& size, int64_t& mtime) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    size = (uint64_t)st.st_size;
    mtime = (int64_t)st.st_mtime;
    return true;
}

}  // namespace

bool probe_cached(const std::string& path, MediaInfo& out) {
    uint64_t size;
    int64_t mtime;
    if (!file_stamp(path, size, mtime)) {
        out = MediaInfo();
        judge(out);
        return true;  // nothing to open
    }
    {
        std::lock_guard<std::mutex> lk(g_m);
        load_locked();
        auto it = g_cache.find(path);
        if (it == g_cache.end() || it->second.size != size || it->second.mtime != mtime) return false;
        out = it->second.info;
    }
    judge(out);
    return true;
}

MediaInfo probe_media(const std::string& path) {
    MediaInfo m;
    if (probe_cached(path, m)) return m;
    uint64_t size = 0;
    int64_t mtime = 0;
    file_stamp(path, size, mtime);
    double t0 = util::now_seconds();
    m = read_info(path);
    double took = util::now_seconds() - t0;
    if (took > 1.5) log_message(LOG_WARNING, "Probe", "Reading %s took %.1f s", util::file_name(path).c_str(), took);
    bool save = false;
    {
        std::lock_guard<std::mutex> lk(g_m);
        Cached& c = g_cache[path];
        c.size = size;
        c.mtime = mtime;
        c.seq = ++g_seq;
        c.info = m;
        evict_locked();
        save = ++g_unsaved >= SAVE_EVERY;
    }
    if (save) flush_probe_cache();
    judge(m);
    return m;
}

void flush_probe_cache() {
    std::lock_guard<std::mutex> wl(g_write_m);
    std::string data;
    {
        std::lock_guard<std::mutex> lk(g_m);
        if (!g_unsaved) return;
        data = serialize_locked();
        g_unsaved = 0;
    }
    if (data.empty() || !util::write_file_atomic(cache_path(), data))
        log_message(LOG_WARNING, "Probe", "Couldn't save %s", cache_path().c_str());
}

// --- verdicts ----------------------------------------------------------------------------------
//
// Keep in step with content/transfer.html, which tells the same before a file is sent. The Wii U
// build decodes H.264 in hardware (8-bit 4:2:0, up to 1920x1088), and has software decoders that
// are only quick enough for MPEG-1/2/4 at standard definition.

void judge(MediaInfo& m) {
    auto set = [&m](Verdict v, const char* reason) {
        m.verdict = v;
        m.reason = reason ? reason : "";
    };
    if (!m.ok || (m.video_codec.empty() && m.audio_tracks == 0)) return set(Verdict::UNKNOWN, tr("Couldn't check this file"));
    if (m.video_codec.empty()) {
        if (m.audio_supported) return set(Verdict::READY, nullptr);
        return set(Verdict::CONVERT, tr("Audio format needs conversion"));
    }
    const std::string& vc = m.video_codec;
    bool h264 = vc == "h264";
    bool software_sd = (vc == "mpeg4" || vc == "mpeg1video" || vc == "mpeg2video") && (int64_t)m.width * m.height <= 720 * 576;
    // Upright or on its side alike: a 1080x1920 phone video is as big as 1080p.
    int long_side = std::max(m.width, m.height), short_side = std::min(m.width, m.height);
    if (m.hdr) return set(Verdict::CONVERT, tr("HDR video needs conversion"));
    if (vc == "hevc") return set(Verdict::CONVERT, tr("HEVC (H.265) video needs conversion"));
    if (!h264 && !software_sd) return set(Verdict::CONVERT, tr("Video format needs conversion"));
    if (h264 && (m.bit_depth > 8 || !m.chroma_420 || m.pro_profile)) return set(Verdict::CONVERT, tr("10-bit video needs conversion"));
    if (long_side > 1920 || short_side > 1088) return set(Verdict::CONVERT, tr("Larger than 1080p, needs conversion"));
    if (software_sd) return set(Verdict::LIMITED, tr("Plays with software decoding, may be slow"));
    if (m.fps > 31 && short_side > 720) return set(Verdict::LIMITED, tr("1080p at 60 fps shows about 45 pictures a second"));
    if (m.audio_tracks > 0 && !m.audio_supported) return set(Verdict::LIMITED, tr("No sound: unsupported audio"));
    set(Verdict::READY, nullptr);
}

const char* verdict_label(Verdict v) {
    switch (v) {
        case Verdict::READY: return tr("Ready to play");
        case Verdict::LIMITED: return tr("Plays with limits");
        case Verdict::CONVERT: return tr("Needs conversion");
        default: return tr("Not checked");
    }
}

std::string codec_label(const std::string& name) {
    static const char* const labels[][2] = {
        {"h264", "H.264"}, {"hevc", "HEVC (H.265)"}, {"av1", "AV1"}, {"vp9", "VP9"}, {"vp8", "VP8"},
        {"mpeg4", "MPEG-4"}, {"mpeg2video", "MPEG-2"}, {"mpeg1video", "MPEG-1"}, {"mjpeg", "Motion JPEG"},
        {"vc1", "VC-1"}, {"wmv1", "WMV"}, {"wmv2", "WMV"}, {"wmv3", "WMV"}, {"theora", "Theora"},
        {"prores", "ProRes"}, {"h263", "H.263"}, {"msmpeg4v3", "DivX 3"}, {"aac", "AAC"}, {"ac3", "AC-3"},
        {"eac3", "E-AC-3"}, {"mp3", "MP3"}, {"mp2", "MP2"}, {"flac", "FLAC"}, {"vorbis", "Vorbis"},
        {"opus", "Opus"}, {"alac", "ALAC"}, {"wavpack", "WavPack"}, {"dts", "DTS"}, {"truehd", "TrueHD"},
        {"mlp", "MLP"}, {"wmav1", "WMA"}, {"wmav2", "WMA"}, {"wmapro", "WMA Pro"}, {"amr_nb", "AMR"},
        {"amr_wb", "AMR-WB"},
    };
    for (const auto& l : labels)
        if (name == l[0]) return l[1];
    if (util::starts_with(name, "pcm_")) return "PCM";
    std::string up = name;
    for (char& c : up) c = (char)toupper((unsigned char)c);
    return up;
}

}  // namespace player
