// Faster or slower sound at the same pitch, for the playback speed setting.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace audio {

// Changes the tempo of 48 kHz stereo sound without changing its pitch (WSOLA). The output is
// built from overlapping windows of the input, cross-faded one hop apart; the input moves on by
// `speed` hops for each hop of output. Each window is shifted a little to where its waveform
// best continues the one before, so the joins don't warble.
class Tempo {
public:
    void set_speed(float speed) { speed_ = speed; }
    float speed() const { return speed_; }
    void reset();
    // Takes `count` frames (2 x int16) that start at media time `pts`, and appends the frames
    // that are ready to `out`. Returns the media time the first appended frame stands for, or
    // -1 when nothing was appended (the first ~50 ms only fill the window).
    double process(const int16_t* in, size_t count, double pts, std::vector<int16_t>& out);

private:
    long best_start(long nominal) const;
    float corr(long a, long b, int step) const;

    float speed_ = 1;
    std::vector<float> buf_;   // input still needed, interleaved stereo
    std::vector<float> mono_;  // the same, both channels added, for lining windows up
    double buf_pts_ = 0;       // media time of buf_'s first frame
    double pos_ = 0;           // where the next window would start without shifting, in frames
    long prev_ = -1;           // where the last window started, or -1 before the first
};

}  // namespace audio
