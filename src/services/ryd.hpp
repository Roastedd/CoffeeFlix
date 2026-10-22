// Return YouTube Dislike: the dislike count YouTube stopped showing, as an estimate from
// returnyoutubedislike.com (read only: nothing is sent but the video's ID).
#pragma once

#include <cstdint>
#include <string>

namespace ryd {

struct Votes {
    bool ok = false;
    int64_t likes = 0, dislikes = 0;
};

// Settings > "Dislike counts" (on by default).
bool enabled();

// From the service's answer ({"likes":..,"dislikes":..}); ok is false for anything else.
Votes parse(const std::string& body);

// Asks (a worker thread; it blocks on the network). Answers are kept for a few minutes, so
// going back to a video doesn't ask again, and the service's limit (100 a minute) is kept to: after
// a "too many requests", nothing is asked for a minute.
Votes fetch(const std::string& video_id, std::string& error);

}  // namespace ryd
