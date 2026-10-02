// Which way up a video is and the shape of its pixels: phone videos are often stored sideways with
// a display matrix saying how to turn them, and anamorphic ones (DVD rips: 720×480 at 32:27, HDV:
// 1440×1080 at 4:3) have wide pixels. Flips in the matrix are ignored.
#pragma once

#include <cstdint>

struct AVStream;

namespace player {

struct Orientation {
    int turns = 0;              // quarter turns clockwise that show the picture upright, 0-3
    int sar_num = 1, sar_den = 1;  // pixel width : height (1:1 when unknown)
};

// FFmpeg's rotation (av_display_rotation_get: degrees counter-clockwise) as quarter turns
// clockwise, 0-3 (to the nearest quarter).
int quarter_turns_cw(double ccw_degrees);
// From a display matrix (9 native int32, 16.16 / 2.30 fixed point); 0 if it has none.
int matrix_quarter_turns(const int32_t matrix[9]);
// A video stream's rotation (display matrix, else the old "rotate" tag) and pixel shape (the
// stream's, else the codec's; nonsense values count as square).
Orientation stream_orientation(const AVStream* st);
// The size the picture shows at: the coded size with its pixels made square (widened or made
// taller, never smaller), turned. Falls back to the coded size for bad input.
void display_size(int coded_w, int coded_h, const Orientation& o, int& w, int& h);
// Width over height as shown (0 for bad input).
double display_aspect(int coded_w, int coded_h, const Orientation& o);
// Copies a 32-bit-per-pixel image turned clockwise by `turns`: `dst` is h×w for odd turns.
// Pitches in bytes.
void rotate_pixels32(const uint8_t* src, int w, int h, int src_pitch, int turns, uint8_t* dst, int dst_pitch);

}  // namespace player
