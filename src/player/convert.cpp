#include "player/convert.hpp"

#include <algorithm>

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

// Two output rows sharing one chroma row. `uv_step` is 1 for planar U/V,
// 2 for interleaved NV12 (with v = u + 1).
inline void convert_pair(const uint8_t* y0, const uint8_t* y1, const uint8_t* u, const uint8_t* v, int uv_step,
                         uint8_t* d0, uint8_t* d1, int w, const Coeffs& c, bool has_second) {
    for (int x = 0; x < w; x += 2) {
        int cu = u[(x >> 1) * uv_step] - 128, cv = v[(x >> 1) * uv_step] - 128;
        int r = c.rv * cv + 128, g = c.gu * cu + c.gv * cv + 128, b = c.bu * cu + 128;
        int n = (x + 1 < w) ? 2 : 1;
        for (int k = 0; k < n; k++) {
            int yy = (y0[x + k] - c.yoff) * c.y;
            uint8_t* p = d0 + (x + k) * 4;
            p[0] = clip(yy + r); p[1] = clip(yy + g); p[2] = clip(yy + b); p[3] = 255;
            if (has_second) {
                yy = (y1[x + k] - c.yoff) * c.y;
                p = d1 + (x + k) * 4;
                p[0] = clip(yy + r); p[1] = clip(yy + g); p[2] = clip(yy + b); p[3] = 255;
            }
        }
    }
}

}  // namespace

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
        convert_pair(yr0, yr1, u, v, nv12 ? 2 : 1, dst + (size_t)y * pitch, dst + (size_t)(y + 1) * pitch, w, c, second);
    }
}

bool FrameConverter::convert(const AVFrame* f, uint8_t* dst, int pitch) {
    const int fmt = f->format;
    bool fast = fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVJ420P || fmt == AV_PIX_FMT_NV12;
    if (fast) {
        bt709_ = f->colorspace == AVCOL_SPC_BT709 || (f->colorspace == AVCOL_SPC_UNSPECIFIED && f->height >= 700);
        full_range_ = f->color_range == AVCOL_RANGE_JPEG || fmt == AV_PIX_FMT_YUVJ420P;
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
