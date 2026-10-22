// Small pictures kept on the card for the photos in My Files: the first time a folder of photos is opened each one is
// decoded and a small JPEG of it written here; every time after, the card shows from those (a few KB each, not a 5 MB
// photo read and decoded again). Keyed by the file's path, size and date, so a changed photo is made afresh. Only local
// files; the cache is kept to a few thousand pictures, the oldest dropped first.
#pragma once

#include <SDL2/SDL.h>

#include <string>

namespace thumb_cache {

// The small JPEG for this file at this size (empty: none). Reads nothing of the photo but its size and date.
std::string lookup(const std::string& path, int max_w, int max_h);
// Keeps `s` (already upright, at the size shown) as the small picture for the file.
void store(const std::string& path, int max_w, int max_h, SDL_Surface* s);

// The first `bytes` of a file (empty if it can't be read): enough of a photo for its EXIF.
std::string read_prefix(const std::string& path, size_t bytes);

// Only for the tests.
std::string file_name_for(const std::string& path, int max_w, int max_h);
void set_limit(size_t files);

}  // namespace thumb_cache
