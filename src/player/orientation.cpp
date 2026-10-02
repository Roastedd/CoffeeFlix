#include "player/orientation.hpp"

extern "C" {
#include <libavcodec/version.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/display.h>
}

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <utility>

namespace player {

namespace {

// The stream's display matrix, if it has one. FFmpeg 6.1 moved stream side data into the codec
// parameters (the Wii U's FFmpeg 4.3 only has AVStream.side_data).
const int32_t* display_matrix(const AVStream* st) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 30, 100)
    const AVPacketSideData* sd = st->codecpar->coded_side_data;
    const int n = st->codecpar->nb_coded_side_data;
#else
    const AVPacketSideData* sd = st->side_data;
    const int n = st->nb_side_data;
#endif
    for (int i = 0; i < n; i++)
        if (sd[i].type == AV_PKT_DATA_DISPLAYMATRIX && sd[i].data && (size_t)sd[i].size >= 9 * sizeof(int32_t))
            return (const int32_t*)sd[i].data;
    return nullptr;
}

// A pixel shape worth believing: both parts set, not wider or taller than 10:1.
bool usable_sar(AVRational r) {
    return r.num > 0 && r.den > 0 && (int64_t)r.num <= (int64_t)r.den * 10 && (int64_t)r.den <= (int64_t)r.num * 10;
}

}  // namespace

int quarter_turns_cw(double ccw_degrees) {
    if (!std::isfinite(ccw_degrees)) return 0;
    long q = std::lround(-ccw_degrees / 90.0) % 4;
    return (int)(q < 0 ? q + 4 : q);
}

int matrix_quarter_turns(const int32_t matrix[9]) {
    return matrix ? quarter_turns_cw(av_display_rotation_get(matrix)) : 0;
}

Orientation stream_orientation(const AVStream* st) {
    Orientation o;
    if (!st || !st->codecpar) return o;
    if (const int32_t* m = display_matrix(st)) {
        o.turns = matrix_quarter_turns(m);
    } else if (const AVDictionaryEntry* e = av_dict_get(st->metadata, "rotate", nullptr, 0)) {
        // Older FFmpeg's tag for the same thing, in degrees clockwise.
        char* end = nullptr;
        double cw = std::strtod(e->value, &end);
        if (end != e->value) o.turns = quarter_turns_cw(-cw);
    }
    AVRational sar = st->sample_aspect_ratio;
    if (!usable_sar(sar)) sar = st->codecpar->sample_aspect_ratio;
    if (usable_sar(sar)) {
        int g = std::gcd(sar.num, sar.den);
        o.sar_num = sar.num / g;
        o.sar_den = sar.den / g;
    }
    return o;
}

void display_size(int coded_w, int coded_h, const Orientation& o, int& w, int& h) {
    w = coded_w;
    h = coded_h;
    if (coded_w <= 0 || coded_h <= 0) return;
    if (o.sar_num > 0 && o.sar_den > 0 && o.sar_num != o.sar_den) {
        if (o.sar_num > o.sar_den)
            w = (int)(((int64_t)coded_w * o.sar_num + o.sar_den / 2) / o.sar_den);
        else
            h = (int)(((int64_t)coded_h * o.sar_den + o.sar_num / 2) / o.sar_num);
    }
    if (o.turns & 1) std::swap(w, h);
}

double display_aspect(int coded_w, int coded_h, const Orientation& o) {
    if (coded_w <= 0 || coded_h <= 0) return 0;
    double sar = o.sar_num > 0 && o.sar_den > 0 ? (double)o.sar_num / o.sar_den : 1.0;
    double a = coded_w * sar / coded_h;
    return o.turns & 1 ? 1.0 / a : a;
}

void rotate_pixels32(const uint8_t* src, int w, int h, int src_pitch, int turns, uint8_t* dst, int dst_pitch) {
    turns &= 3;
    const int dw = turns & 1 ? h : w, dh = turns & 1 ? w : h;
    for (int y = 0; y < dh; y++) {
        uint8_t* out = dst + (size_t)y * dst_pitch;
        for (int x = 0; x < dw; x++) {
            int sx = x, sy = y;
            if (turns == 1) sx = y, sy = h - 1 - x;
            else if (turns == 2) sx = w - 1 - x, sy = h - 1 - y;
            else if (turns == 3) sx = w - 1 - y, sy = x;
            std::memcpy(out + (size_t)x * 4, src + (size_t)sy * src_pitch + (size_t)sx * 4, 4);
        }
    }
}

}  // namespace player
