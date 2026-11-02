// "Surprise me": something picked at random from the SD card, without reading the whole card. It goes down from
// a folder by chance, one listing at a time (a folder of many subfolders is far likelier to be gone into than a
// single file is to be taken), and does it again if it came to nothing. Nothing here draws.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "player/player.hpp"
#include "screens/media_files.hpp"

namespace screens::media {

enum SurpriseScope { SURPRISE_ALL, SURPRISE_VIDEOS, SURPRISE_MUSIC, SURPRISE_PHOTOS };

bool surprise_matches(SurpriseScope scope, Kind kind);

struct SurpriseOptions {
    std::vector<std::string> roots;      // tried in this order: the next one when one has nothing
    std::vector<std::string> skip_top;   // folder names not gone into directly under a root (the console's own files)
    SurpriseScope scope = SURPRISE_ALL;
    uint64_t seed = 1;
    double seconds = 4;                  // how long to look
    int max_depth = 8;
    std::vector<std::string> avoid;      // paths picked lately: not again, if there's another
    const std::atomic<bool>* cancel = nullptr;
};

struct Surprise {
    bool ok = false;
    std::string path;
    Kind kind = K_OTHER;
};

Surprise pick_surprise(const SurpriseOptions& options);

// The ones picked lately, kept for the profile in use (the last 20), so a pick is not what came just before.
std::vector<std::string> recent_surprises();
void remember_surprise(const std::string& path);

// Opens it the way My Media opens a file: a song with the songs of its folder after it, a photo among its folder's.
void open_surprise(const std::string& path);

// The songs of the folder of `path` as a queue, `path` first and the rest shuffled: for music under a slideshow.
std::vector<player::Source> music_queue_from(const std::string& path, uint64_t seed);

}  // namespace screens::media
