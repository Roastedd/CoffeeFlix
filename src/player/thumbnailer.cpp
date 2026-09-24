#include "player/thumbnailer.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <SDL2/SDL_image.h>

#include <algorithm>

#include "core/util.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"

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

        int w = std::min(max_w, frame->width);
        int h = std::max(1, (int)((int64_t)frame->height * w / frame->width));
        sws = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format, w, h, AV_PIX_FMT_RGBA,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) break;
        out = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
        if (!out) break;
        uint8_t* dst[4] = {(uint8_t*)out->pixels, nullptr, nullptr, nullptr};
        int lines[4] = {out->pitch, 0, 0, 0};
        sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, lines);
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
    std::string cached = util::fmt("%s/%016llx-%d.jpg", dir.c_str(), (unsigned long long)util::hash64(path), max_w);
    if (util::file_exists(cached)) {
        if (SDL_Surface* s = IMG_Load(cached.c_str())) return s;
    }
    SDL_Surface* s = grab_frame(path, max_w);
    if (!s) return nullptr;
    util::make_dirs(dir);
    if (IMG_SaveJPG(s, cached.c_str(), 82) != 0) IMG_SavePNG(s, cached.c_str());
    return s;
}

}  // namespace player
