// Still frames for video files (cached on disk), for thumbnails in browsers.
#pragma once

#include <SDL2/SDL.h>
#include <string>

namespace player {

// Blocking; call from a worker thread. Returns an RGBA surface or nullptr.
SDL_Surface* video_thumbnail(const std::string& path, int max_w);

// Deletes the video's cached stills (it was deleted or renamed). Blocking, like the above.
void forget_thumbnail(const std::string& path);

}  // namespace player
