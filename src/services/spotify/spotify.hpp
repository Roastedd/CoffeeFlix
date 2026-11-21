// Spotify (optional, Premium accounts): songs and podcasts played on the console itself. Spotify's own
// protocol, as librespot speaks it: a login on its access point, which also hands out each file's key; the
// files come encrypted from Spotify's CDN and are decrypted as they play (player/spotify_io).
// Unofficial: not affiliated with or endorsed by Spotify.
//
// Everything that goes over the network blocks: call it from worker threads (tasks::submit).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "player/player.hpp"

namespace podcasts { struct Show; }

namespace spotify {

bool signed_in();
std::string user_name();  // the account's Spotify username
void sign_out();

// Signing in with a phone: a code to scan (or to type on spotify.com/pair) and confirm there.
struct DeviceLogin {
    std::string device_code, user_code, url;  // url: what the QR code holds
    int interval = 5;                         // seconds between checks
    double expires_at = 0;                    // util::now_seconds()
};
bool begin_login(DeviceLogin& out, std::string& error);
enum class LoginState { WAITING, SIGNED_IN, FAILED };
// One check whether the phone confirmed it; once it has, signs in on the access point and keeps the login.
LoginState check_login(DeviceLogin& login, std::string& error);

// A token for Spotify's web services, renewed when it runs out. Empty with `error` set when there is none.
std::string access_token(std::string& error);

// --- the library (spotify_library.cpp) -----------------------------------------------------------------

// A song, episode, album, artist or playlist as lists show it.
struct Item {
    std::string uri;       // "spotify:track:...", "spotify:episode:...", "spotify:album:..."
    std::string name, subtitle;  // subtitle: the artists, the show, the owner
    std::string image;     // about 300 px; empty when there's none
    int64_t duration_ms = 0;
    int count = 0;         // a playlist's songs
    int year = 0;          // an album's
    int64_t released = 0;  // an episode's, as yyyymmddhhmm (0: not known)
};

std::vector<Item> playlists(std::string& error);        // the account's, in its order (folders flattened)
std::vector<Item> liked_songs(std::string& error);      // newest first
std::vector<Item> saved_albums(std::string& error);
std::vector<Item> followed_artists(std::string& error);
std::vector<Item> saved_episodes(std::string& error);   // "Your Episodes"
std::vector<Item> followed_shows(std::string& error);   // podcasts
// A show's newest episode, from Spotify's public embed page (what websites embed). Spotify lists a show's
// episodes only to its own apps (November 2026), so this is what there is of a show only on Spotify: the
// others' episodes come from their RSS feeds. Empty uri when there's none.
Item latest_episode(const std::string& show_uri);
// The podcast's own feed, found by its name in the Podcasts section's search (the publisher telling apart shows
// named alike): false when there's none that surely is it (a show only on Spotify, or no clear match).
// `searched` (optional) is set false when the search itself failed: then it may yet be found.
bool find_feed(const Item& show, podcasts::Show& out, bool* searched = nullptr);
std::vector<Item> playlist_items(const std::string& uri, std::string& error);  // its songs and episodes
// The playlists with each one's revision, which changes whenever the playlist does (for the search index).
struct Listed {
    Item playlist;
    std::string revision;
};
std::vector<Listed> playlists_with_revisions(std::string& error);

// A long list in parts, for showing the first of it at once: each call brings the next (on a worker, one at a
// time); `more` false with the last. Up to 10,000 items (Spotify's limit for a playlist).
struct Part {
    std::vector<Item> items;
    bool more = false;
};
using Parts = std::function<Part(std::string& error)>;
Parts playlist_parts(const std::string& uri);
Parts liked_parts();
// The picture Spotify's apps show for a playlist without one of its own: the covers of its first four albums
// (one cover when there are fewer). Empty when there's nothing to show.
std::string playlist_cover(const std::string& uri);

struct Album {
    Item info;
    std::vector<Item> tracks;
};
bool album(const std::string& uri, Album& out, std::string& error);

struct Artist {
    Item info;
    std::vector<Item> top_tracks, albums, singles;
};
bool artist(const std::string& uri, Artist& out, std::string& error);

// The library searched by name: Spotify's own search isn't open to this client (it answers 400), so as Music
// Assistant does, what's searched is what the account has: liked songs, saved episodes, albums, artists,
// playlists and podcasts. Every word of `query` has to be in a name or what's under it (any case).
struct Found {
    std::vector<Item> songs, episodes, albums, artists, playlists, podcasts;
    std::vector<Item> in_playlists;  // songs of the playlists (spotify_index), not among `songs`
    bool empty() const {
        return songs.empty() && episodes.empty() && albums.empty() && artists.empty() && playlists.empty() &&
               podcasts.empty() && in_playlists.empty();
    }
};
Found search_library(const std::string& query, std::string& error);

// What the player opens for a song or an episode.
player::Source source(const Item& item);

// A song to play: the player resolves it (metadata, file, key) when it opens it.
player::Source track_source(const std::string& uri, const std::string& title = "", const std::string& artist = "",
                            const std::string& artwork = "");

}  // namespace spotify
