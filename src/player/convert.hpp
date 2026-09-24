// YUV -> RGBA conversion for video frames, split across two threads.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

extern "C" {
#include <libavutil/frame.h>
}

struct SwsContext;

namespace player {

// Colour space of a YUV picture: BT.709 when tagged so or when HD and untagged, else BT.601.
bool yuv_bt709(const AVFrame* f);
// Full range (0-255) rather than limited (16-235) YUV.
bool yuv_full_range(const AVFrame* f);

class FrameConverter {
public:
    FrameConverter();
    ~FrameConverter();
    // Converts `src` into tightly packed RGBA (R,G,B,A byte order) at `dst`.
    bool convert(const AVFrame* src, uint8_t* dst, int dst_pitch);

private:
    void helper_loop();
    void convert_rows(const AVFrame* src, uint8_t* dst, int pitch, int y0, int y1);

    std::thread helper_;
    std::mutex m_;
    std::condition_variable cv_;
    std::function<void()> job_;
    bool job_done_ = true;
    bool quit_ = false;
    SwsContext* sws_ = nullptr;
    bool bt709_ = false;
    bool full_range_ = false;
};

}  // namespace player
