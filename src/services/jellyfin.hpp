// Jellyfin client: sign-in (password or Quick Connect), browsing, and playback
// with a device profile describing what the Wii U can decode.
#pragma once

#include <string>
#include <vector>

namespace player { struct Source; }

namespace jellyfin {

struct Account {
    std::string server, server_name, user_id, user_name, token;
    bool valid() const { return !server.empty() && !token.empty() && !user_id.empty(); }
};

const Account& account();
void sign_out();
std::string normalize_url(const std::string& input);

struct ServerInfo {
    bool ok = false;
    std::string name, version, error;
};
ServerInfo server_info(const std::string& url);

struct AuthResult {
    bool ok = false;
    std::string error;
};
AuthResult sign_in(const std::string& url, const std::string& user, const std::string& password);

struct QuickConnect {
    bool ok = false;
    std::string code, secret, error;
};
QuickConnect quick_connect_start(const std::string& url);
// 1 = approved, 0 = waiting, -1 = failed/expired
int quick_connect_poll(const std::string& url, const std::string& secret);
AuthResult quick_connect_finish(const std::string& url, const std::string& secret);

struct Item {
    std::string id, name, type, collection_type;
    std::string overview, year, rating, series_name, series_id, season_id, album_artist;
    std::string primary_tag, backdrop_tag, thumb_tag, series_primary_tag, parent_backdrop_id, parent_backdrop_tag,
        parent_thumb_id, parent_thumb_tag, album_id, album_primary_tag;
    double runtime = 0, position = 0, community_rating = 0;
    int index = 0, parent_index = 0, child_count = 0, unplayed = 0;
    bool played = false, favorite = false;
    float aspect = 0;  // primary image aspect ratio

    bool is_folderish() const {
        return type == "Series" || type == "Season" || type == "BoxSet" || type == "MusicAlbum" ||
               type == "MusicArtist" || type == "Folder" || type == "CollectionFolder" || type == "Playlist" ||
               type == "UserView";
    }
    bool is_audio() const { return type == "Audio"; }
};

struct List {
    std::vector<Item> items;
    int total = 0;
    bool ok = false;
    std::string error;
};

List views();
List resume();
List next_up(const std::string& series_id = "");
List latest(const std::string& parent_id);
List items(const std::string& parent_id, const std::string& types, const std::string& sort, int start, int limit,
           bool recursive = true);
List seasons(const std::string& series_id);
List episodes(const std::string& series_id, const std::string& season_id);
List children(const std::string& parent_id);  // album tracks, folders
List similar(const std::string& id);
List search(const std::string& term);
bool item(const std::string& id, Item& out, std::string& error);
void set_favorite(const std::string& id, bool fav);
void set_played(const std::string& id, bool played);

// Image URLs ("" if the item has no such image).
std::string poster(const Item& it, int max_w = 400);
std::string backdrop(const Item& it, int max_w = 1280);
std::string thumb(const Item& it, int max_w = 480);  // 16:9 still (episodes, continue watching)

player::Source make_source(const Item& it, bool from_start = false);

}  // namespace jellyfin
