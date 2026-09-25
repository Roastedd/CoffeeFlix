// Video frames the GPU draws from where the hardware decoder wrote them: see platform.hpp.
#include "platform/platform.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
}

#include <SDL2/SDL_system.h>
#include <coreinit/cache.h>

#include <malloc.h>

#include "logger/logger.hpp"

namespace platform {

namespace {

// Frames of an attached decoder point here (AVFrame::opaque).
char g_tag;

// The frames of one decoder, all of one size.
struct VideoFrames {
    AVBufferPool* pool = nullptr;
    int size = 0;
};

void free_frame(void*, uint8_t* data) { free(data); }

// From the default heap (MEM2, which the GPU reads), aligned for the hardware decoder (1 KB,
// more than the GPU's 256 bytes).
AVBufferRef* alloc_frame(int size) {
    void* p = memalign(1024, size);
    if (!p) return nullptr;
    // Out of the CPU cache before anything else writes it: nothing the CPU had there can land on
    // top of a picture later.
    DCFlushRange(p, size);
    AVBufferRef* b = av_buffer_create((uint8_t*)p, size, free_frame, nullptr, 0);
    if (!b) free(p);
    return b;
}

// NV12 laid out like the hardware decoder's pictures and the GPU's textures: rows padded to 256
// bytes, the height to 16 lines, the chroma right after the luma.
int get_buffer(AVCodecContext* ctx, AVFrame* f, int flags) {
    auto* v = (VideoFrames*)ctx->opaque;
    if (!v || f->format != AV_PIX_FMT_NV12 || f->width <= 0 || f->height <= 0 || f->width > 4096 || f->height > 4096)
        return avcodec_default_get_buffer2(ctx, f, flags);
    int pitch = (f->width + 255) & ~255, rows = (f->height + 15) & ~15;
    int size = pitch * rows * 3 / 2;
    if (size != v->size) {  // the stream changed size
        av_buffer_pool_uninit(&v->pool);  // its frames stay valid
        v->pool = av_buffer_pool_init(size, alloc_frame);
        v->size = v->pool ? size : 0;
    }
    if (!v->pool || !(f->buf[0] = av_buffer_pool_get(v->pool))) return AVERROR(ENOMEM);
    f->data[0] = f->buf[0]->data;
    f->data[1] = f->data[0] + pitch * rows;
    f->linesize[0] = f->linesize[1] = pitch;
    f->extended_data = f->data;
    f->opaque = &g_tag;
    return 0;
}

}  // namespace

void attach_video_frames(AVCodecContext* ctx) {
    ctx->opaque = new VideoFrames;
    ctx->get_buffer2 = get_buffer;
}

void detach_video_frames(AVCodecContext* ctx) {
    if (!ctx || ctx->get_buffer2 != get_buffer) return;
    auto* v = (VideoFrames*)ctx->opaque;
    ctx->get_buffer2 = avcodec_default_get_buffer2;
    ctx->opaque = nullptr;
    av_buffer_pool_uninit(&v->pool);
    delete v;
}

bool show_video_frame(SDL_Texture* tex, const AVFrame* f) {
    static bool reported = false;
    if (!tex || !f || f->opaque != &g_tag || !f->data[0]) return false;
    if (SDL_WiiUSetNVTextureMemory(tex, f->data[0], f->data[1], f->linesize[0]) == 0) return true;
    if (!reported) {
        reported = true;
        log_message(LOG_WARNING, "Player", "Copying pictures to the GPU: %s", SDL_GetError());
    }
    return false;
}

void video_frame_cpu_read(const AVFrame* f) {
    if (!f || f->opaque != &g_tag || !f->buf[0]) return;
    DCInvalidateRange(f->buf[0]->data, f->buf[0]->size);
}

}  // namespace platform
