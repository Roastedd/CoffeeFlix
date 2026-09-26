// YouTube via the InnerTube API. Browsing uses the WEB client, or the VR client for the optional
// signed-in account; playback uses clients whose stream URLs don't need the JS player.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace player { struct Source; }

namespace youtube {

struct Video {
    std::string id, title, channel, channel_id;
    std::string duration, views, published;
    bool live = false;
};
// v.duration as shown ("Short" in the current language).
std::string duration_label(const Video& v);

struct Results {
    std::vector<Video> items;
    std::string continuation;
    std::string error;
    bool ok = false;
};

struct Channel {
    std::string id, name, handle, avatar, banner;
    std::string subscribers, videos, description;
};

struct Playlist {
    std::string id, title, channel, thumbnail, count;
};

struct ChannelResults {
    std::vector<Channel> items;
    std::string error;
    bool ok = false;
};

struct PlaylistResults {
    std::vector<Playlist> items;
    std::string continuation;
    std::string error;
    bool ok = false;
};

enum class Tab { VIDEOS, SHORTS, LIVE };

struct Topic {
    const char* name;  // English (N_): tr() it where it's shown
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
// One tab of a channel page. The first page also fills `header` (name, avatar, subscribers...).
Results channel_tab(const std::string& channel_id, Tab tab, const std::string& continuation = "",
                    Channel* header = nullptr);
PlaylistResults channel_playlists(const std::string& channel_id, const std::string& continuation = "");
// A playlist's videos; the first page also fills `info` (title, channel, count).
Results playlist_videos(const std::string& playlist_id, const std::string& continuation = "", Playlist* info = nullptr);
ChannelResults search_channels(const std::string& query);
// The newest uploads of these channels, newest first. `channels` (optional) receives each
// channel's header, e.g. to refresh stored avatars.
Results subscription_feed(const std::vector<std::string>& channel_ids, size_t per_channel = 8,
                          std::vector<Channel>* channels = nullptr);
// Rough age of a "3 days ago" / "Streamed 2 weeks ago" label in seconds (unknown: very old).
int64_t age_seconds(const std::string& published);
// Videos YouTube suggests next to this one (the watch page's sidebar).
Results related(const std::string& video_id);

// --- signed in (services/yt_account); these fail when nobody is --------------------------------
// YouTube's own recommendations and the account's subscriptions feed.
Results account_home(const std::string& continuation = "");
Results account_subscriptions(const std::string& continuation = "");
ChannelResults account_channels();  // every page of them
// Subscribes the account to a channel, or unsubscribes it.
bool account_subscribe(const std::string& channel_id, bool on, std::string& error);
struct AccountInfo {
    std::string name, photo;
};
bool account_info(AccountInfo& out, std::string& error);

std::string thumbnail(const std::string& id);     // 320x180
std::string thumbnail_hq(const std::string& id);  // 480x360 (backdrops)

// SponsorBlock segments (sponsor, self-promotion, interaction reminders) for a video.
struct Segment {
    double start = 0, end = 0;
    std::string category;
};
std::vector<Segment> sponsor_segments(const std::string& video_id);

// Fills src.url / src.audio_url / headers for playback at up to max_height.
bool resolve(const std::string& video_id, int max_height, player::Source& src, std::string& error);

// Builds a player source for a video (resolved lazily on the player thread).
player::Source make_source(const Video& v);

}  // namespace youtube
