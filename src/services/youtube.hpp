// YouTube via the InnerTube API (no account). Browsing uses the WEB client;
// playback uses clients whose stream URLs don't need the JS player.
#pragma once

#include <string>
#include <vector>

namespace player { struct Source; }

namespace youtube {

struct Video {
    std::string id, title, channel, channel_id;
    std::string duration, views, published;
    bool live = false;
};

struct Results {
    std::vector<Video> items;
    std::string continuation;
    std::string error;
    bool ok = false;
};

struct Topic {
    const char* name;
    const char* query;
    int icon;
};
const std::vector<Topic>& topics();

// Search params: videos only / popular this week.
extern const char* PARAMS_VIDEOS;
extern const char* PARAMS_POPULAR_WEEK;

Results search(const std::string& query, const std::string& params = PARAMS_VIDEOS,
               const std::string& continuation = "");
Results trending();
Results channel_videos(const std::string& channel_id, const std::string& continuation = "");

std::string thumbnail(const std::string& id);     // 320x180
std::string thumbnail_hq(const std::string& id);  // 480x360 (backdrops)

// Fills src.url / src.audio_url / headers for playback at up to max_height.
bool resolve(const std::string& video_id, int max_height, player::Source& src, std::string& error);

// Builds a player source for a video (resolved lazily on the player thread).
player::Source make_source(const Video& v);

}  // namespace youtube
