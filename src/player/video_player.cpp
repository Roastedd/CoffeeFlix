#include <cstdint>
#include <cstdio>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>
#include <new>
#include <cstring>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

#include "utils/utils.hpp"
#include "utils/media_info.hpp"
#include "utils/sdl.hpp"
#include "logger/logger.hpp"
#include "player/audio_player.hpp"
#include "player/video_player.hpp"

#include <SDL2/SDL.h>

#define FRAME_POOL_SIZE 16
#define MAX_FRAME_QUEUE_SIZE 12
#define PIX_FMT_TARGET AV_PIX_FMT_YUV420P

class FrameQueue {
public:
    explicit FrameQueue(size_t capacity) : cap(capacity) {
        buffer.resize(cap, nullptr);
        head = 0;
        tail = 0;
    }

    bool push(AVFrame* f) {
        std::lock_guard<std::mutex> lk(mtx);
        size_t next = (head + 1) % cap;
        if (next == tail) {
            return false; // full
        }
        buffer[head] = f;
        head = next;
        return true;
    }

    AVFrame* pop() {
        std::lock_guard<std::mutex> lk(mtx);
        if (tail == head) return nullptr; // empty
        AVFrame* f = buffer[tail];
        buffer[tail] = nullptr;
        tail = (tail + 1) % cap;
        return f;
    }

    void clear_and_free_all() {
        std::lock_guard<std::mutex> lk(mtx);
        while (tail != head) {
            AVFrame* f = buffer[tail];
            buffer[tail] = nullptr;
            tail = (tail + 1) % cap;
            if (f) av_frame_free(&f);
        }
    }

    AVFrame* pop_one_for_replace() {
        std::lock_guard<std::mutex> lk(mtx);
        if (tail == head) return nullptr;
        AVFrame* f = buffer[tail];
        buffer[tail] = nullptr;
        tail = (tail + 1) % cap;
        return f;
    }

    bool empty() {
        std::lock_guard<std::mutex> lk(mtx);
        return tail == head;
    }

    bool full() {
        std::lock_guard<std::mutex> lk(mtx);
        size_t next = (head + 1) % cap;
        return next == tail;
    }

private:
    size_t cap;
    std::vector<AVFrame*> buffer;
    size_t head;
    size_t tail;
    std::mutex mtx;
};

static FrameQueue frame_queue(MAX_FRAME_QUEUE_SIZE + 1);

static AVFormatContext* fmt_ctx = nullptr;
static AVCodecContext* video_codec_ctx = nullptr;
static SwsContext* sws_ctx = nullptr;
static int video_stream_index = -1;
static std::thread decode_thread;

static std::atomic<bool> thread_running{false};
static std::atomic<bool> playback_running{false};

static frame_info* current_frame_info = nullptr;
static int64_t start_time_us = 0;
static int64_t pause_start_us = 0;
static AVRational video_time_base;

SDL_Rect dest_rect = {0, 0, 0, 0};
bool dest_rect_initialised = false;
static int sws_width = 0;
static int sws_height = 0;

inline int64_t get_time_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(
        steady_clock::now().time_since_epoch()
    ).count();
}

static void clear_frame_queue() {
    frame_queue.clear_and_free_all();
}

static void free_current_frame_info() {
    if (current_frame_info) {
        if (current_frame_info->texture) {
            SDL_DestroyTexture(current_frame_info->texture);
            current_frame_info->texture = nullptr;
        }
        delete current_frame_info;
        current_frame_info = nullptr;
    }
}

double video_player_get_total_playback_time() {
    if (!fmt_ctx || video_stream_index < 0) return 0.0;
    AVStream* stream = fmt_ctx->streams[video_stream_index];
    if (stream->duration != AV_NOPTS_VALUE)
        return stream->duration * av_q2d(stream->time_base);
    else if (fmt_ctx->duration != AV_NOPTS_VALUE)
        return fmt_ctx->duration / (double)AV_TIME_BASE;
    return -1.0;
}

double video_player_get_current_playback_time() {
    if (!fmt_ctx || video_stream_index < 0) return -1.0;
    if (playback_running.load())
        return (get_time_us() - start_time_us) / 1'000'000.0;
    else
        return (pause_start_us - start_time_us) / 1'000'000.0;
}

static AVFrame* convert_frame_to_yuv420p(AVFrame* src) {
    if (!src) return nullptr;

    AVFrame* dst = av_frame_alloc();
    if (!dst) return nullptr;

    dst->format = PIX_FMT_TARGET;
    dst->width = src->width;
    dst->height = src->height;

    if (av_frame_get_buffer(dst, 32) < 0) {
        av_frame_free(&dst);
        return nullptr;
    }

    if (!sws_ctx || sws_width != src->width || sws_height != src->height) {
        if (sws_ctx) sws_freeContext(sws_ctx);
        sws_ctx = sws_getContext(src->width, src->height, (AVPixelFormat)src->format,
                                 dst->width, dst->height, PIX_FMT_TARGET,
                                 SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        sws_width = src->width;
        sws_height = src->height;
        if (!sws_ctx) {
            av_frame_free(&dst);
            return nullptr;
        }
    }

    sws_scale(sws_ctx, src->data, src->linesize, 0, src->height, dst->data, dst->linesize);
    return dst;
}

static void decode_loop() {
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        log_message(LOG_ERROR, "Video Player", "Failed to allocate packet");
        return;
    }

    while (thread_running.load()) {
        while (!playback_running.load() && thread_running.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(2));

        if (!thread_running.load()) break;

        int ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0) {
            av_packet_unref(pkt);

            int send_ret = avcodec_send_packet(video_codec_ctx, nullptr);
            if (send_ret < 0) {
                char errbuf[128];
                av_strerror(send_ret, errbuf, sizeof(errbuf));
                log_message(LOG_ERROR, "Video Player", "avcodec_send_packet(nullptr) failed: %s", errbuf);
            } else {
                // Drain frames
                while (thread_running.load()) {
                    AVFrame* out_frame = av_frame_alloc();
                    if (!out_frame) break;
                    int r = avcodec_receive_frame(video_codec_ctx, out_frame);
                    if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
                        av_frame_free(&out_frame);
                        break;
                    } else if (r < 0) {
                        char errbuf[128];
                        av_strerror(r, errbuf, sizeof(errbuf));
                        log_message(LOG_ERROR, "Video Player", "avcodec_receive_frame flush failed: %s", errbuf);
                        av_frame_free(&out_frame);
                        break;
                    }

                    AVFrame* frame_to_queue = nullptr;
                    if (out_frame->format == PIX_FMT_TARGET) {
                        frame_to_queue = out_frame;
                    } else if (out_frame->format == AV_PIX_FMT_NV12) {
                        frame_to_queue = convert_frame_to_yuv420p(out_frame);
                        av_frame_free(&out_frame);
                        if (!frame_to_queue) continue;
                    } else {
                        static std::atomic<bool> warned{false};
                        if (!warned.exchange(true))
                            log_message(LOG_WARNING, "Video Player", "Unsupported pixel format: %d", out_frame->format);
                        av_frame_free(&out_frame);
                        continue;
                    }

                    if (!frame_queue.push(frame_to_queue)) {
                        AVFrame* old = frame_queue.pop_one_for_replace();
                        if (old) av_frame_free(&old);
                        if (!frame_queue.push(frame_to_queue)) {
                            av_frame_free(&frame_to_queue);
                        }
                    }
                }
            }

            thread_running = false;
            break;
        }

        if (pkt->stream_index == video_stream_index) {
            int send_ret = avcodec_send_packet(video_codec_ctx, pkt);
            if (send_ret < 0) {
                char errbuf[128];
                av_strerror(send_ret, errbuf, sizeof(errbuf));
                log_message(LOG_ERROR, "Video Player", "avcodec_send_packet failed: %s", errbuf);
                av_packet_unref(pkt);
                continue;
            }

            while (send_ret >= 0) {
                AVFrame* out_frame = av_frame_alloc();
                if (!out_frame) {
                    log_message(LOG_ERROR, "Video Player", "av_frame_alloc failed");
                    break;
                }
                int r = avcodec_receive_frame(video_codec_ctx, out_frame);
                if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
                    av_frame_free(&out_frame);
                    break;
                } else if (r < 0) {
                    char errbuf[128];
                    av_strerror(r, errbuf, sizeof(errbuf));
                    log_message(LOG_ERROR, "Video Player", "avcodec_receive_frame failed: %s", errbuf);
                    av_frame_free(&out_frame);
                    break;
                }

                AVFrame* frame_to_queue = nullptr;
                if (out_frame->format == PIX_FMT_TARGET) {
                    frame_to_queue = out_frame;
                } else if (out_frame->format == AV_PIX_FMT_NV12) {
                    frame_to_queue = convert_frame_to_yuv420p(out_frame);
                    av_frame_free(&out_frame);
                    if (!frame_to_queue) continue;
                } else {
                    static std::atomic<bool> warned{false};
                    if (!warned.exchange(true))
                        log_message(LOG_WARNING, "Video Player", "Unsupported pixel format: %d", out_frame->format);
                    av_frame_free(&out_frame);
                    continue;
                }

                if (!frame_queue.push(frame_to_queue)) {
                    AVFrame* old = frame_queue.pop_one_for_replace();
                    if (old) av_frame_free(&old);
                    if (!frame_queue.push(frame_to_queue)) {
                        av_frame_free(&frame_to_queue);
                    }
                }
            }
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
}

int video_player_init(const char* filepath) {
    if (thread_running.load()) {
        log_message(LOG_WARNING, "Video Player", "Already initialized, cleaning up first");
        video_player_cleanup();
    }

    avformat_network_init();
    
    // Ensure clean state before initialization
    fmt_ctx = nullptr;
    video_codec_ctx = nullptr;
    sws_ctx = nullptr;

    if (avformat_open_input(&fmt_ctx, filepath, nullptr, nullptr) < 0) {
        log_message(LOG_ERROR, "Video Player", "Failed to open input file");
        return -1;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        log_message(LOG_ERROR, "Video Player", "Failed to find stream info");
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    video_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index < 0) {
        log_message(LOG_ERROR, "Video Player", "No video stream found");
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    AVStream* stream = fmt_ctx->streams[video_stream_index];
    video_time_base = stream->time_base;

    // Log container and codec information
    const char* container_name = fmt_ctx->iformat ? fmt_ctx->iformat->name : "unknown";
    const char* codec_name = avcodec_get_name(stream->codecpar->codec_id);
    log_message(LOG_OK, "Video Player", "Container: %s, Codec: %s, Resolution: %dx%d", 
                container_name, codec_name, 
                stream->codecpar->width, stream->codecpar->height);

    const AVCodec* codec = nullptr;
    bool using_hardware = false;
    
    // Codec selection with hardware acceleration support
    switch (stream->codecpar->codec_id) {
        case AV_CODEC_ID_H264:
            if (stream->codecpar->height == 720) {
                codec = avcodec_find_decoder_by_name("h264_wiiu");
                if (codec) {
                    using_hardware = true;
                    log_message(LOG_OK, "Video Player", "Using H.264 hardware decoding");
                }
            }
            if (!codec) {
                codec = avcodec_find_decoder_by_name("h264");
                log_message(LOG_OK, "Video Player", "Using H.264 software decoding");
            }
            break;
            
        case AV_CODEC_ID_VP8:
            codec = avcodec_find_decoder(stream->codecpar->codec_id);
            if (codec) {
                log_message(LOG_OK, "Video Player", "Using VP8 software decoding");
            }
            break;
            
        case AV_CODEC_ID_VP9:
            codec = avcodec_find_decoder(stream->codecpar->codec_id);
            if (codec) {
                log_message(LOG_OK, "Video Player", "Using VP9 software decoding");
                // VP9 is computationally expensive, warn about performance
                if (stream->codecpar->width > 854 || stream->codecpar->height > 480) {
                    log_message(LOG_ERROR, "Video Player", "Warning: VP9 at this resolution may not play smoothly");
                }
            }
            break;
            
        case AV_CODEC_ID_HEVC:
            // Try hardware decoder first, then software
            codec = avcodec_find_decoder_by_name("hevc_wiiu");
            if (codec) {
                using_hardware = true;
                log_message(LOG_OK, "Video Player", "Using HEVC/H.265 hardware decoding");
            } else {
                codec = avcodec_find_decoder(stream->codecpar->codec_id);
                if (codec) {
                    log_message(LOG_OK, "Video Player", "Using HEVC/H.265 software decoding");
                    // HEVC software decoding is very slow
                    if (stream->codecpar->width > 640 || stream->codecpar->height > 480) {
                        log_message(LOG_ERROR, "Video Player", "Warning: HEVC software decoding may be too slow");
                    }
                }
            }
            break;
            
        case AV_CODEC_ID_MPEG4:
        case AV_CODEC_ID_MSMPEG4V3:
        case AV_CODEC_ID_MPEG2VIDEO:
        case AV_CODEC_ID_MPEG1VIDEO:
            codec = avcodec_find_decoder(stream->codecpar->codec_id);
            if (codec) {
                log_message(LOG_OK, "Video Player", "Using MPEG software decoding");
            }
            break;
            
        default:
            // Try to find any decoder for this codec
            codec = avcodec_find_decoder(stream->codecpar->codec_id);
            if (codec) {
                log_message(LOG_OK, "Video Player", "Using generic decoder for codec: %s", codec_name);
            }
            break;
    }
    
    if (!codec) {
        log_message(LOG_ERROR, "Video Player", "Unsupported video codec: %s (0x%x)", 
                    codec_name, stream->codecpar->codec_id);
        log_message(LOG_ERROR, "Video Player", "Supported codecs: H.264, VP8, VP9, HEVC (experimental), MPEG1/2/4");
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    video_codec_ctx = avcodec_alloc_context3(codec);
    if (!video_codec_ctx) {
        log_message(LOG_ERROR, "Video Player", "Failed to allocate codec context");
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    if (avcodec_parameters_to_context(video_codec_ctx, stream->codecpar) < 0) {
        log_message(LOG_ERROR, "Video Player", "Failed to copy codec parameters");
        avcodec_free_context(&video_codec_ctx);
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    video_codec_ctx->get_format = [](AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) -> enum AVPixelFormat {
        for (int i = 0; pix_fmts[i] != -1; ++i)
            if (pix_fmts[i] == PIX_FMT_TARGET) return pix_fmts[i];
        for (int i = 0; pix_fmts[i] != -1; ++i)
            if (pix_fmts[i] == AV_PIX_FMT_NV12) return AV_PIX_FMT_NV12;
        return pix_fmts[0];
    };

    if (avcodec_open2(video_codec_ctx, codec, nullptr) < 0) {
        log_message(LOG_ERROR, "Video Player", "Failed to open codec");
        avcodec_free_context(&video_codec_ctx);
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    free_current_frame_info();
    current_frame_info = new frame_info();
    current_frame_info->width = video_codec_ctx->width;
    current_frame_info->height = video_codec_ctx->height;
    current_frame_info->texture = SDL_CreateTexture(
        sdl_get()->sdl_renderer,
        SDL_PIXELFORMAT_IYUV,
        SDL_TEXTUREACCESS_STREAMING,
        video_codec_ctx->width, video_codec_ctx->height
    );

    if (!current_frame_info->texture) {
        log_message(LOG_ERROR, "Video Player", "SDL_CreateTexture failed");
        video_player_cleanup();
        return -1;
    }

    clear_frame_queue();
    // Free existing SwsContext before creating new one
    if (sws_ctx) {
        sws_freeContext(sws_ctx);
        sws_ctx = nullptr;
        log_message(LOG_OK, "Video Player", "Freed previous SwsContext");
    }
    sws_width = 0;
    sws_height = 0;

    start_time_us = get_time_us();
    pause_start_us = start_time_us;

    thread_running = true;
    playback_running = false;

    media_info_get()->playback_status = true;
    media_info_get()->total_video_playback_time = video_player_get_total_playback_time();

    audio_player_init(filepath);
    decode_thread = std::thread(decode_loop);

    return 0;
}

void video_player_play(bool play) {
    if (play && pause_start_us != 0) {
        int64_t paused_duration = get_time_us() - pause_start_us;
        start_time_us += paused_duration;
        pause_start_us = 0;
    } else if (!play) {
        pause_start_us = get_time_us();
    }
    playback_running = play;
    media_info_get()->playback_status = play;
    audio_player_play(play);
}

void video_player_seek(double seconds) {
    if (!fmt_ctx || video_stream_index < 0) return;

    video_player_play(false);

    int64_t seek_target = static_cast<int64_t>(seconds * AV_TIME_BASE);
    int64_t seek_ts = av_rescale_q(seek_target, AVRational{1, AV_TIME_BASE}, fmt_ctx->streams[video_stream_index]->time_base);

    if (av_seek_frame(fmt_ctx, video_stream_index, seek_ts, AVSEEK_FLAG_BACKWARD) < 0)
        log_message(LOG_ERROR, "Video Player", "Seek failed");
    else {
        avcodec_flush_buffers(video_codec_ctx);
        clear_frame_queue();
        start_time_us = get_time_us() - static_cast<int64_t>(seconds * 1'000'000);
    }

    video_player_play(true);
}

void video_player_update() {
    media_info_get()->current_video_playback_time = video_player_get_current_playback_time();
    if (!playback_running.load()) return;

    AVFrame* frame = frame_queue.pop();
    if (!frame) return;

    if (!dest_rect_initialised) {
        dest_rect = calculate_aspect_fit_rect(frame->width, frame->height);
        dest_rect_initialised = true;
    }

    if (current_frame_info && current_frame_info->texture) {
        int tex_w = 0, tex_h = 0;
        SDL_QueryTexture(current_frame_info->texture, nullptr, nullptr, &tex_w, &tex_h);
        if (tex_w != frame->width || tex_h != frame->height) {
            SDL_DestroyTexture(current_frame_info->texture);
            current_frame_info->texture = SDL_CreateTexture(
                sdl_get()->sdl_renderer,
                SDL_PIXELFORMAT_IYUV,
                SDL_TEXTUREACCESS_STREAMING,
                frame->width, frame->height
            );
            if (!current_frame_info->texture) {
                log_message(LOG_ERROR, "Video Player", "SDL_CreateTexture (recreate) failed");
                av_frame_free(&frame);
                return;
            }
        }
    }

    SDL_UpdateYUVTexture(current_frame_info->texture, nullptr,
                         frame->data[0], frame->linesize[0],
                         frame->data[1], frame->linesize[1],
                         frame->data[2], frame->linesize[2]);
    SDL_RenderCopy(sdl_get()->sdl_renderer, current_frame_info->texture, nullptr, &dest_rect);

    av_frame_free(&frame);
}

void video_player_cleanup() {
    log_message(LOG_OK, "Video Player", "Starting cleanup");
    
    // Stop audio first to prevent access to video context
    audio_player_cleanup();

    // Signal threads to stop
    thread_running = false;
    playback_running = false;
    dest_rect_initialised = false;

    // Wait for decode thread to finish
    if (decode_thread.joinable()) {
        log_message(LOG_OK, "Video Player", "Waiting for decode thread");
        decode_thread.join();
    }

    // Clear frame queue before freeing contexts
    clear_frame_queue();

    // Free codec context (also closes codec internally)
    if (video_codec_ctx) {
        avcodec_free_context(&video_codec_ctx);
        video_codec_ctx = nullptr;
        log_message(LOG_OK, "Video Player", "Freed codec context");
    }
    
    // Close input format (frees all streams and I/O)
    if (fmt_ctx) {
        avformat_close_input(&fmt_ctx);
        fmt_ctx = nullptr;
        log_message(LOG_OK, "Video Player", "Closed format context");
    }

    // Free current frame textures
    free_current_frame_info();

    // Free SwsContext with proper cleanup
    if (sws_ctx) {
        sws_freeContext(sws_ctx);
        sws_ctx = nullptr;
        sws_width = 0;
        sws_height = 0;
        log_message(LOG_OK, "Video Player", "Freed SwsContext");
    }

    // Deinitialize network (safe to call multiple times)
    avformat_network_deinit();

    log_message(LOG_OK, "Video Player", "Cleanup complete");
}
