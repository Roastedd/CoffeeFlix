#include "audio/tempo.hpp"

#include <algorithm>
#include <cmath>

#include "audio/mixer.hpp"

namespace audio {

namespace {

constexpr long HOP = 960;          // 20 ms of output per step, and half a window
constexpr long WINDOW = HOP * 2;
constexpr long SEEK = 360;         // how far a window may shift to line up, either way (7.5 ms)

// The fade-in over one hop; the fade-out is 1 minus it (sin² and cos² add up to 1).
struct Fade {
    float v[HOP];
    Fade() {
        for (long i = 0; i < HOP; i++) {
            float s = std::sin(1.5707963f * (i + 0.5f) / HOP);
            v[i] = s * s;
        }
    }
};

const Fade& fade() {
    static const Fade f;
    return f;
}

}  // namespace

void Tempo::reset() {
    buf_.clear();
    mono_.clear();
    buf_pts_ = 0;
    pos_ = 0;
    prev_ = -1;
}

// How alike the sound at `a` and at `b` is over a hop, looking at every `step`th frame
// (normalized by b's loudness, so a loud spot doesn't win just for being loud).
float Tempo::corr(long a, long b, int step) const {
    const float* x = mono_.data() + a;
    const float* y = mono_.data() + b;
    float xy = 0, yy = 1.0f;
    for (long i = 0; i < HOP; i += step) {
        xy += x[i] * y[i];
        yy += y[i] * y[i];
    }
    return xy / std::sqrt(yy);
}

// Where to start the window meant for `nominal`: the spot nearby that best continues the last
// window's sound. A coarse look first, then a closer one around the best.
long Tempo::best_start(long nominal) const {
    long target = prev_ + HOP;
    long lo = std::max(0L, nominal - SEEK), hi = nominal + SEEK;
    long best = nominal;
    float best_score = -1e30f;
    for (long k = lo; k <= hi; k += 4) {
        float s = corr(target, k, 4);
        if (s > best_score) {
            best_score = s;
            best = k;
        }
    }
    long coarse = best;
    best_score = -1e30f;
    for (long k = std::max(lo, coarse - 3); k <= std::min(hi, coarse + 3); k++) {
        float s = corr(target, k, 2);
        if (s > best_score) {
            best_score = s;
            best = k;
        }
    }
    return best;
}

double Tempo::process(const int16_t* in, size_t count, double pts, std::vector<int16_t>& out) {
    size_t old = mono_.size();
    // Media time follows the frames taken in, unless the input jumped (a gap in the stream).
    if (old == 0 || std::fabs(buf_pts_ + (double)old / RATE - pts) > 0.1) buf_pts_ = pts - (double)old / RATE;
    buf_.resize((old + count) * 2);
    mono_.resize(old + count);
    for (size_t i = 0; i < count; i++) {
        float l = in[i * 2], r = in[i * 2 + 1];
        buf_[(old + i) * 2] = l;
        buf_[(old + i) * 2 + 1] = r;
        mono_[old + i] = l + r;
    }

    const float* f = fade().v;
    const long frames = (long)mono_.size();
    double first = -1;
    for (;;) {
        long nominal = std::lround(pos_);
        if (nominal + SEEK + WINDOW > frames) break;
        long start = prev_ < 0 ? nominal : best_start(nominal);
        if (first < 0) first = buf_pts_ + pos_ / RATE;
        size_t o = out.size();
        out.resize(o + HOP * 2);
        for (long i = 0; i < HOP; i++) {
            for (int c = 0; c < 2; c++) {
                float v = buf_[(start + i) * 2 + c];
                if (prev_ >= 0) v = buf_[(prev_ + HOP + i) * 2 + c] * (1 - f[i]) + v * f[i];
                out[o + i * 2 + c] = (int16_t)std::clamp(std::lround(v), -32768L, 32767L);
            }
        }
        prev_ = start;
        pos_ += HOP * speed_;
    }

    // Let go of the input nothing will look at again: the next window lines up somewhere after
    // pos_ - SEEK, and fades in against the last window's second half. (The whole last window
    // stays, so prev_ can't go below 0 and look like there wasn't one.)
    long keep = std::lround(pos_) - SEEK;
    if (prev_ >= 0) keep = std::min(keep, prev_);
    if (keep > 4 * HOP) {
        buf_.erase(buf_.begin(), buf_.begin() + keep * 2);
        mono_.erase(mono_.begin(), mono_.begin() + keep);
        buf_pts_ += (double)keep / RATE;
        pos_ -= keep;
        if (prev_ >= 0) prev_ -= keep;
    }
    return first;
}

}  // namespace audio
