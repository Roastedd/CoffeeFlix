// Asynchronous image cache for thumbnails, posters and artwork.
//
// get() is called every frame for whatever is on screen; the first call queues
// a download+decode on the image worker pool (newest requests first), and the
// decoded surface is uploaded to a texture on the main thread under a per-frame
// budget so scrolling stays smooth. Unused textures are evicted LRU.
#pragma once

#include <SDL2/SDL.h>
#include <cstdint>
#include <string>

#include "gfx/focus.hpp"

namespace images {

enum Flags {
    BLUR = 1,       // tiny, blurred copy for ambient backdrops
    NO_EVICT = 2,   // keep resident (app icons)
    FOCUS = 4,      // once decoded, find where the people are in it (focus_of)
};

struct Image {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
    bool ready = false;
    bool failed = false;
    bool preview = false;   // the camera's own tiny preview of a photo, until the picture itself is decoded
    double ready_time = 0;
};

void init(size_t budget_bytes);
void shutdown();
void begin_frame();

// Returns nullptr for an empty url. The returned pointer is valid for this frame.
const Image* get(const std::string& url, int max_w = 0, int max_h = 0, int flags = 0);
// Where the people are in a photo asked for with FOCUS, once it has loaded. False before then.
bool focus_of(const std::string& url, focus::Focus& out);
// Whatever is loaded of this picture, at any size (not the blurred one), the largest: to show while the size wanted loads.
// nullptr when none is. Cheap to call, but not for every thumbnail in a grid.
const Image* best_loaded(const std::string& url);
// 0..1 fade-in progress for a freshly loaded image.
float fade(const Image* img, float duration = 0.25f);
// Insert an image produced locally (e.g. embedded album art). Takes the surface.
void put(const std::string& key, SDL_Surface* surface, int flags = 0);
// Decodes jpg/png/webp/gif bytes to an RGBA32 surface that fits max_w x max_h
// (0 = unlimited). Any thread; nullptr on failure, and for a picture that states more than 16
// million pixels: a small file can (flat colour compresses to nothing), and decoding one takes
// the memory the player needs.
SDL_Surface* decode(const std::string& data, int max_w = 0, int max_h = 0, int flags = 0);
// The size a jpg/png/webp/gif header states, without decoding. False for other formats and for
// a header cut short.
bool stated_size(const std::string& data, int64_t& w, int64_t& h);
// Whether decode() refuses the picture for its size.
bool too_big(const std::string& data);
void clear();
size_t bytes_used();
// Whether put() has a ready picture under this key.
bool has(const std::string& key, int flags = 0);
// Lets go of every loaded size of one picture at once (a screensaver's pictures are not looked at again): its textures go through the
// same delayed destruction as an eviction. A picture still being loaded is left to finish.
void release(const std::string& url, int min_width = 0);   // min_width: leave the smaller sizes (the grid's thumbnails)

}  // namespace images
