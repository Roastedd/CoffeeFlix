// What a photo needs from its JPEG that SDL_image does not give: decoding at a fraction of the size (the picture is made
// from the file's 1/2, 1/4 or 1/8 scale directly, several times quicker than decoding it whole and shrinking it), the
// camera's own tiny preview and the way the camera was held (EXIF), and writing a small JPEG for the thumbnail cache.
#pragma once

#include <SDL2/SDL.h>

#include <string>

namespace jpeg_tools {

bool is_jpeg(const std::string& data);

struct Info {
    int width = 0, height = 0;
    int orientation = 1;            // EXIF: 1 as it is; 3 upside down; 6 turned a quarter to the right (a phone held upright); 8 to the left...
    size_t thumb_offset = 0, thumb_length = 0;   // the camera's preview inside `data` (0: none)
    std::string taken;              // "2026:10:16 14:03:22", when the camera says
    std::string camera;             // "Pixel 8", when it says
};
// The size, the orientation and the rest of the EXIF, from the start of the file: a prefix of the first 64 KB or so
// is enough for all but the size of a file with a very big header. False when it is no JPEG.
bool read_info(const std::string& data, Info& out);

// Decodes at the largest reduction (1, 1/2, 1/4, 1/8) that still has `want_w` x `want_h` or more (0: whole size). RGBA32,
// as the file holds it (not turned). nullptr when it can't (CMYK, damaged): the caller falls back to SDL_image.
// `min_denom` (1, 2, 4, 8): at least this much smaller, for a picture too big to hold whole.
SDL_Surface* decode_scaled(const std::string& data, int want_w, int want_h, int min_denom = 1);

// The camera's preview, decoded (as the file holds it). nullptr when there is none.
SDL_Surface* decode_thumbnail(const std::string& data, const Info& info);

// The picture turned as the EXIF orientation says, so it is upright. Takes the surface, gives the new one.
SDL_Surface* orient(SDL_Surface* s, int orientation);
// Turned by quarter turns clockwise (0-3).
SDL_Surface* rotate_quarters(SDL_Surface* s, int turns);

// A JPEG of the surface (quality 1-100), for the thumbnail cache. Empty on failure.
std::string encode(SDL_Surface* s, int quality);

}  // namespace jpeg_tools
