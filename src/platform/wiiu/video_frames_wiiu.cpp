// Reference-counted hardware pictures; decoder targets survive delayed output,
// and SDL waits for GPU completion before retiring a texture's old memory.
#include "platform/platform.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
}

#include <SDL2/SDL_system.h>
#include <coreinit/cache.h>
#include <h264/decode.h>

#include <malloc.h>
#include <mutex>
#include <vector>

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

// Blocks the hardware decoder refused (H264DECCheckMemSegmentation), kept so the heap does not offer
// the same ones again. Freed with the last decoder.
std::mutex g_refused_lock;  // decoders open and close on other threads
std::vector<void*> g_refused;
int g_attached = 0, g_allocated = 0, g_refusals = 0;
constexpr size_t MAX_REFUSED_BYTES = 24u << 20;   // about seven 1080p blocks: enough that the heap does not offer them again soon
constexpr int MAX_TRIES = 12;

// From the default heap (MEM2, which the GPU reads), aligned for the hardware decoder (1 KB,
// more than the GPU's 256 bytes). The decoder only writes into memory its own check accepts
// (h264_wiiu): a picture in any other block is decoded elsewhere and copied by the CPU, 30-40 ms
// of each picture, which on 1080p60 left about 30 a second and a busy thread (Oct 17 log: the
// same video at 20 ms a picture, then at 32 when the pool's blocks had landed in other memory).
// So a refused block is put aside and another asked for.
AVBufferRef* alloc_frame(int size) {
    void* p = nullptr;
    int refused = 0;
    for (int i = 0; i < MAX_TRIES; i++) {
        p = memalign(1024, size);
        if (!p) {  // the set-aside blocks may be what is in the way: let them go and try once more
            std::vector<void*> let_go;
            {
                std::lock_guard<std::mutex> lk(g_refused_lock);
                let_go.swap(g_refused);
            }
            for (void* q : let_go) free(q);
            if (let_go.empty()) break;
            p = memalign(1024, size);
            if (!p) break;
        }
        // Out of the CPU cache before anything else writes it: nothing the CPU had there can land on
        // top of a picture later.
        DCFlushRange(p, size);
        if (H264DECCheckMemSegmentation(p, size) == 0) break;
        refused++;
        if (i == MAX_TRIES - 1) break;  // every try refused: it is used anyway (copied, as before)
        std::lock_guard<std::mutex> lk(g_refused_lock);
        if (g_refused.size() * (size_t)size >= MAX_REFUSED_BYTES) {
            free(g_refused.front());
            g_refused.erase(g_refused.begin());
        }
        g_refused.push_back(p);
        p = nullptr;
    }
    bool say = false;
    int total_refused = 0, total_frames = 0;
    {
        std::lock_guard<std::mutex> lk(g_refused_lock);
        g_allocated++;
        g_refusals += refused;
        total_refused = g_refusals;
        total_frames = g_allocated;
        say = refused && (g_refusals <= 8 || g_refusals % 25 == 0);
    }
    if (say)
        log_message(LOG_WARNING, "Player", "Decoder memory: %d block%s refused before one fit (%d refused of %d frames so far)",
                    refused, refused == 1 ? "" : "s", total_refused, total_frames);
    if (!p) return nullptr;
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
    {
        std::lock_guard<std::mutex> lk(g_refused_lock);
        g_attached++;
    }
    ctx->opaque = new VideoFrames;
    ctx->get_buffer2 = get_buffer;
    log_message(LOG_OK, "Player", "Direct hardware pictures with retained decoder targets and GPU completion waits");
}

void detach_video_frames(AVCodecContext* ctx) {
    if (!ctx || ctx->get_buffer2 != get_buffer) return;
    auto* v = (VideoFrames*)ctx->opaque;
    ctx->get_buffer2 = avcodec_default_get_buffer2;
    ctx->opaque = nullptr;
    av_buffer_pool_uninit(&v->pool);
    delete v;
    std::lock_guard<std::mutex> lk(g_refused_lock);
    if (--g_attached <= 0) {
        for (void* p : g_refused) free(p);
        g_refused.clear();
    }
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
