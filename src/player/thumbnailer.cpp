#include "player/thumbnailer.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <SDL2/SDL_image.h>

#include <algorithm>
#include <cstdio>

#include "core/util.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"
#include "player/orientation.hpp"

namespace player {

namespace {

SDL_Surface* grab_frame(const std::string& path, int max_w) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return nullptr;
    SDL_Surface* out = nullptr;
    AVCodecContext* dec = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    SwsContext* sws = nullptr;
    do {
        fmt->probesize = 2 << 20;
        if (avformat_find_stream_info(fmt, nullptr) < 0) break;
        int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vi < 0) break;
        AVStream* st = fmt->streams[vi];
        // Software decoders only: the hardware one may be busy with playback.
        const AVCodec* codec = st->codecpar->codec_id == AV_CODEC_ID_H264 ? avcodec_find_decoder_by_name("h264")
                                                                          : avcodec_find_decoder(st->codecpar->codec_id);
        if (!codec) break;
        dec = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(dec, st->codecpar);
        dec->thread_count = 1;
        dec->skip_loop_filter = AVDISCARD_ALL;  // speed over quality for a still
        if (avcodec_open2(dec, codec, nullptr) < 0) break;

        // A frame ~10% in (capped at 3 minutes) avoids black intros.
        double dur = fmt->duration > 0 ? fmt->duration / (double)AV_TIME_BASE : 0;
        double t = std::min(dur * 0.1, 180.0);
        if (t > 1) avformat_seek_file(fmt, -1, INT64_MIN, (int64_t)(t * AV_TIME_BASE), INT64_MAX, AVSEEK_FLAG_BACKWARD);

        bool got = false;
        int packets = 0;
        while (!got && packets < 400 && av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == vi) {
                packets++;
                if (avcodec_send_packet(dec, pkt) >= 0 && avcodec_receive_frame(dec, frame) >= 0) got = true;
            }
            av_packet_unref(pkt);
        }
        if (!got) break;

        // Upright, with square pixels: the size it shows at, scaled down to max_w wide. Scaled in
        // the stored orientation (sw×sh), then turned.
        Orientation o = stream_orientation(st);
        int dw = 0, dh = 0;
        display_size(frame->width, frame->height, o, dw, dh);
        if (dw <= 0 || dh <= 0) break;
        int w = std::min(max_w, dw);
        int h = std::max(1, (int)((int64_t)dh * w / dw));
        int sw = o.turns & 1 ? h : w, sh = o.turns & 1 ? w : h;
        sws = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format, sw, sh, AV_PIX_FMT_RGBA,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) break;
        SDL_Surface* scaled = SDL_CreateRGBSurfaceWithFormat(0, sw, sh, 32, SDL_PIXELFORMAT_RGBA32);
        if (!scaled) break;
        uint8_t* dst[4] = {(uint8_t*)scaled->pixels, nullptr, nullptr, nullptr};
        int lines[4] = {scaled->pitch, 0, 0, 0};
        sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, lines);
        if (o.turns == 0) {
            out = scaled;
            break;
        }
        out = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
        if (out)
            rotate_pixels32((const uint8_t*)scaled->pixels, sw, sh, scaled->pitch, o.turns, (uint8_t*)out->pixels,
                            out->pitch);
        SDL_FreeSurface(scaled);
    } while (false);
    if (sws) sws_freeContext(sws);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    if (dec) avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return out;
}

}  // namespace

SDL_Surface* video_thumbnail(const std::string& path, int max_w) {
    std::string dir = platform::data_dir() + "/thumbs";
    // "-o1": stills made upright and with square pixels (older ones, without the tag, weren't).
    const unsigned long long key = util::hash64(path);
    std::string cached = util::fmt("%s/%016llx-%d-o1.jpg", dir.c_str(), key, max_w);
    if (util::file_exists(cached)) {
        if (SDL_Surface* s = IMG_Load(cached.c_str())) return s;
    }
    SDL_Surface* s = grab_frame(path, max_w);
    if (!s) return nullptr;
    std::remove(util::fmt("%s/%016llx-%d.jpg", dir.c_str(), key, max_w).c_str());
    util::make_dirs(dir);
    if (IMG_SaveJPG(s, cached.c_str(), 82) != 0) IMG_SavePNG(s, cached.c_str());
    return s;
}

}  // namespace player

// --- forgetting a video's stills ------------------------------------------------------------
#include <dirent.h>

#include <cstring>
#include <vector>

namespace player {

void forget_thumbnail(const std::string& path) {
    std::string dir = platform::data_dir() + "/thumbs";
    // Every size and version of it: "<hash of the path>-<width>[-tag].jpg".
    std::string prefix = util::fmt("%016llx-", (unsigned long long)util::hash64(path));
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> doomed;
    while (dirent* de = readdir(d))
        if (strncmp(de->d_name, prefix.c_str(), prefix.size()) == 0) doomed.push_back(dir + "/" + de->d_name);
    closedir(d);
    for (const std::string& f : doomed) std::remove(f.c_str());
}

}  // namespace player
