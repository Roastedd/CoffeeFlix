// Navidrome (Subsonic API) client: authentication, browsing music (artists, albums,
// playlists, songs), search, cover art, scrobbling, favorites, and playback streams.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace player { struct Source; }

namespace navidrome {

struct Account {
    std::string server;        // e.g. "http://192.168.1.50:4533"
    std::string server_name;   // server title or version
    std::string user_name;
    std::string token;         // md5(password + salt)
    std::string salt;
    bool valid() const { return !server.empty() && !user_name.empty() && !token.empty() && !salt.empty(); }
};

Account account();
bool signed_in();
void sign_out();
std::vector<Account> others();
void switch_to(size_t i);
void add_account();
int version();
std::string normalize_url(const std::string& input);

struct ServerInfo {
    bool ok = false;
    std::string name;
    std::string version;
    std::string error;
};
ServerInfo server_info(const std::string& url);

struct AuthResult {
    bool ok = false;
    std::string error;
};
AuthResult sign_in(const std::string& url, const std::string& user, const std::string& password);

struct Artist {
    std::string id;
    std::string name;
    int album_count = 0;
    std::string cover_art;
    std::string artist_image_url;
    bool starred = false;
};

struct Album {
    std::string id;
    std::string name;
    std::string artist;
    std::string artist_id;
    std::string cover_art;
    int song_count = 0;
    double duration = 0;    // seconds
    int year = 0;
    std::string genre;
    bool starred = false;
};

struct Song {
    std::string id;
    std::string parent;
    std::string title;
    std::string album;
    std::string album_id;
    std::string artist;
    std::string artist_id;
    int track = 0;
    int disc_number = 0;
    int year = 0;
    std::string genre;
    std::string cover_art;
    int64_t size = 0;
    std::string content_type;
    std::string suffix;
    double duration = 0;    // seconds
    int bit_rate = 0;       // kbps
    std::string path;
    bool starred = false;
};

struct Playlist {
    std::string id;
    std::string name;
    std::string comment;
    std::string owner;
    bool is_public = false;
    int song_count = 0;
    double duration = 0;    // seconds
    std::string cover_art;
};

struct List {
    std::vector<Artist> artists;
    std::vector<Album> albums;
    std::vector<Song> songs;
    std::vector<Playlist> playlists;
    int total = 0;
    bool ok = false;
    std::string error;
};

List get_artists();
List get_artist(const std::string& artist_id);
List get_album_list(const std::string& type, int size = 50, int offset = 0);
List get_album(const std::string& album_id);
List get_playlists();
List get_playlist(const std::string& playlist_id);
List search(const std::string& term, int count = 20);

// Interaction & State
void scrobble(const std::string& song_id, bool submission = true);
void star(const std::string& id, bool is_album = false, bool is_artist = false);
void unstar(const std::string& id, bool is_album = false, bool is_artist = false);

// URLs
std::string cover_art_url(const std::string& cover_id, int size = 300);
std::string stream_url(const std::string& song_id);

// Convert Song to player::Source for playback
player::Source make_source(const Song& song);

}  // namespace navidrome
