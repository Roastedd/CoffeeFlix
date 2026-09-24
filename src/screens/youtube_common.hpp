// Pieces shared by the YouTube screens: feeds, cards, the "More" menu, and the local library
// (subscriptions, Watch later, history, saved playlists). Nothing needs an account.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/app.hpp"
#include "core/tasks.hpp"
#include "screens/widgets.hpp"
#include "services/youtube.hpp"

namespace screens {
namespace yt {

// A lazily loaded list of videos with pagination.
struct Feed {
    youtube::Results res;
    bool loading = false, loaded = false;
    tasks::Scope scope;
    std::function<youtube::Results(const std::string& continuation)> fetch;

    void load();
    void more();
    void reload();
    void remove(const std::string& video_id);
};

CardInfo video_card(const youtube::Video& v);
CardInfo playlist_card(const youtube::Playlist& p);

// Plays a video (and remembers it in the history).
void play(const youtube::Video& v);
// Plays a list in order from `index`, e.g. a playlist.
void play_all(const std::vector<youtube::Video>& list, int index);

// --- subscriptions ---------------------------------------------------------------------
bool subscribed(const std::string& channel_id);
// Subscribes or unsubscribes, with a toast. `avatar` may be empty.
void set_subscribed(const std::string& channel_id, const std::string& name, const std::string& avatar, bool on);
std::vector<youtube::Channel> subscriptions();
// Refreshes stored names and avatars from channel headers.
void update_channels(const std::vector<youtube::Channel>& channels);

// Imports subscriptions from a Google Takeout CSV or NewPipe/LibreTube JSON export in the app's
// folder; returns a message for a toast.
std::string import_subscriptions();
std::string export_subscriptions();

// --- library ------------------------------------------------------------------------------
bool in_watch_later(const std::string& video_id);
void set_watch_later(const youtube::Video& v, bool on);
std::vector<youtube::Video> watch_later();
std::vector<youtube::Video> history();
void remove_from_history(const std::string& video_id);
void clear_history();
bool playlist_saved(const std::string& playlist_id);
void set_playlist_saved(const youtube::Playlist& p, bool on);
std::vector<youtube::Playlist> saved_playlists();

// The X menu for a video. `removed` runs after "Not interested" so the caller can drop the video
// from what it shows; `extra` items go at the end (e.g. "Remove from history").
struct MenuOptions {
    bool show_channel = true;
    std::function<void()> removed;
    std::vector<MenuItem> extra;
};
void video_menu(const youtube::Video& v, MenuOptions opts = MenuOptions());

// --- screens -------------------------------------------------------------------------------
std::unique_ptr<app::Screen> make_channel(const std::string& id, const std::string& name, const std::string& avatar = "");
std::unique_ptr<app::Screen> make_playlist(const std::string& id, const std::string& title);
std::unique_ptr<app::Screen> make_subscriptions();
std::unique_ptr<app::Screen> make_library();

}  // namespace yt
}  // namespace screens
