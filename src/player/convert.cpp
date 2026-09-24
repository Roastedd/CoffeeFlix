#include "player/convert.hpp"

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace player {

namespace {

// Fixed-point YCbCr -> RGB coefficients (x256).
struct Coeffs { int y, rv, gu, gv, bu, yoff; };
constexpr Coeffs BT601_LIMITED{298, 409, -100, -208, 516, 16};
constexpr Coeffs BT709_LIMITED{298, 459, -55, -136, 541, 16};
constexpr Coeffs BT601_FULL{256, 359, -88, -183, 454, 0};
constexpr Coeffs BT709_FULL{256, 403, -48, -120, 475, 0};

uint8_t g_clip[1024 + 256];
bool g_clip_init = false;

inline uint8_t clip(int v) { return g_clip[(v >> 8) + 512]; }

void init_clip() {
    if (g_clip_init) return;
    for (int i = 0; i < 1024 + 256; i++) g_clip[i] = (uint8_t)std::clamp(i - 512, 0, 255);
    g_clip_init = true;
}

// One RGBA32 pixel (bytes R, G, B, A in memory) as a single word store.
inline uint32_t rgba(int yy, int r, int g, int b) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return (uint32_t)clip(yy + r) << 24 | (uint32_t)clip(yy + g) << 16 | (uint32_t)clip(yy + b) << 8 | 0xFFu;
#else
    return (uint32_t)clip(yy + r) | (uint32_t)clip(yy + g) << 8 | (uint32_t)clip(yy + b) << 16 | 0xFF000000u;
#endif
}

// Two output rows sharing one chroma row. `UVStep` is 1 for planar U/V,
// 2 for interleaved NV12 (with v = u + 1).
template <int UVStep, bool Second>
void convert_pair(const uint8_t* y0, const uint8_t* y1, const uint8_t* u, const uint8_t* v, uint8_t* d0, uint8_t* d1,
                  int w, const Coeffs& c) {
    const int cy = c.y, yoff = c.yoff;
    int x = 0;
    for (; x + 1 < w; x += 2) {
        int cu = u[(x >> 1) * UVStep] - 128, cv = v[(x >> 1) * UVStep] - 128;
        int r = c.rv * cv + 128, g = c.gu * cu + c.gv * cv + 128, b = c.bu * cu + 128;
        uint32_t p[2] = {rgba((y0[x] - yoff) * cy, r, g, b), rgba((y0[x + 1] - yoff) * cy, r, g, b)};
        std::memcpy(d0 + x * 4, p, sizeof(p));
        if (Second) {
            p[0] = rgba((y1[x] - yoff) * cy, r, g, b);
            p[1] = rgba((y1[x + 1] - yoff) * cy, r, g, b);
            std::memcpy(d1 + x * 4, p, sizeof(p));
        }
    }
    if (x < w) {  // odd width
        int cu = u[(x >> 1) * UVStep] - 128, cv = v[(x >> 1) * UVStep] - 128;
        int r = c.rv * cv + 128, g = c.gu * cu + c.gv * cv + 128, b = c.bu * cu + 128;
        uint32_t p = rgba((y0[x] - yoff) * cy, r, g, b);
        std::memcpy(d0 + x * 4, &p, sizeof(p));
        if (Second) {
            p = rgba((y1[x] - yoff) * cy, r, g, b);
            std::memcpy(d1 + x * 4, &p, sizeof(p));
        }
    }
}

}  // namespace

bool yuv_bt709(const AVFrame* f) {
    return f->colorspace == AVCOL_SPC_BT709 || (f->colorspace == AVCOL_SPC_UNSPECIFIED && f->height >= 700);
}

bool yuv_full_range(const AVFrame* f) {
    return f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P;
}

FrameConverter::FrameConverter() {
    init_clip();
    helper_ = std::thread(&FrameConverter::helper_loop, this);
}

FrameConverter::~FrameConverter() {
    {
        std::lock_guard<std::mutex> lk(m_);
        quit_ = true;
    }
    cv_.notify_all();
    if (helper_.joinable()) helper_.join();
    if (sws_) sws_freeContext(sws_);
}

void FrameConverter::helper_loop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return quit_ || (job_ && !job_done_); });
            if (quit_) return;
            job = job_;
        }
        job();
        {
            std::lock_guard<std::mutex> lk(m_);
            job_done_ = true;
            job_ = nullptr;
        }
        cv_.notify_all();
    }
}

void FrameConverter::convert_rows(const AVFrame* f, uint8_t* dst, int pitch, int y0, int y1) {
    const Coeffs& c = full_range_ ? (bt709_ ? BT709_FULL : BT601_FULL) : (bt709_ ? BT709_LIMITED : BT601_LIMITED);
    const int w = f->width;
    bool nv12 = f->format == AV_PIX_FMT_NV12;
    for (int y = y0; y < y1; y += 2) {
        const uint8_t* yr0 = f->data[0] + (size_t)y * f->linesize[0];
        const uint8_t* yr1 = yr0 + f->linesize[0];
        bool second = y + 1 < y1;
        const uint8_t *u, *v;
        if (nv12) {
            u = f->data[1] + (size_t)(y >> 1) * f->linesize[1];
            v = u + 1;
        } else {
            u = f->data[1] + (size_t)(y >> 1) * f->linesize[1];
            v = f->data[2] + (size_t)(y >> 1) * f->linesize[2];
        }
        uint8_t* d0 = dst + (size_t)y * pitch;
        uint8_t* d1 = d0 + pitch;
        if (nv12) {
            if (second) convert_pair<2, true>(yr0, yr1, u, v, d0, d1, w, c);
            else convert_pair<2, false>(yr0, yr1, u, v, d0, d1, w, c);
        } else {
            if (second) convert_pair<1, true>(yr0, yr1, u, v, d0, d1, w, c);
            else convert_pair<1, false>(yr0, yr1, u, v, d0, d1, w, c);
        }
    }
}

bool FrameConverter::convert(const AVFrame* f, uint8_t* dst, int pitch) {
    const int fmt = f->format;
    bool fast = fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVJ420P || fmt == AV_PIX_FMT_NV12;
    if (fast) {
        bt709_ = yuv_bt709(f);
        full_range_ = yuv_full_range(f);
        int h = f->height;
        if (h >= 360) {
            // Bottom half on the helper thread, top half here.
            int mid = (h / 2) & ~1;
            {
                std::lock_guard<std::mutex> lk(m_);
                job_ = [this, f, dst, pitch, mid, h] { convert_rows(f, dst, pitch, mid, h); };
                job_done_ = false;
            }
            cv_.notify_all();
            convert_rows(f, dst, pitch, 0, mid);
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return job_done_; });
        } else {
            convert_rows(f, dst, pitch, 0, h);
        }
        return true;
    }
    // Anything else (10-bit, 4:2:2, RGB...) goes through swscale.
    sws_ = sws_getCachedContext(sws_, f->width, f->height, (AVPixelFormat)fmt, f->width, f->height, AV_PIX_FMT_RGBA,
                                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) return false;
    uint8_t* dst_planes[4] = {dst, nullptr, nullptr, nullptr};
    int dst_lines[4] = {pitch, 0, 0, 0};
    sws_scale(sws_, f->data, f->linesize, 0, f->height, dst_planes, dst_lines);
    return true;
}

}  // namespace player
