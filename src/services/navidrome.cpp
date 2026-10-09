#include "services/navidrome.hpp"

#include <atomic>
#include <mutex>

#include "core/http.hpp"
#include "core/i18n.hpp"
#include "core/json.hpp"
#include "core/md5.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "player/player.hpp"

namespace navidrome {

namespace {

Account g_acc;
bool g_loaded = false;
std::mutex g_m;
std::atomic<int> g_version{0};

void load_account() {
    if (g_loaded) return;
    g_acc.server = store::get_str("nd_server");
    g_acc.server_name = store::get_str("nd_server_name");
    g_acc.user_name = store::get_str("nd_user");
    g_acc.token = store::get_str("nd_token");
    g_acc.salt = store::get_str("nd_salt");
    g_loaded = true;
}

void save_account_fields(const Account& a) {
    store::set_str("nd_server", a.server);
    store::set_str("nd_server_name", a.server_name);
    store::set_str("nd_user", a.user_name);
    store::set_str("nd_token", a.token);
    store::set_str("nd_salt", a.salt);
    store::save_now();
}

Account snapshot() {
    std::lock_guard<std::mutex> lk(g_m);
    load_account();
    return g_acc;
}

std::string base_rest_url(const std::string& server) {
    std::string s = server;
    while (!s.empty() && s.back() == '/') s.pop_back();
    if (util::ends_with(s, "/rest")) return s;
    return s + "/rest";
}

std::string auth_query(const Account& a) {
    return util::fmt("u=%s&t=%s&s=%s&v=1.16.1&c=CoffeeFlix&f=json",
                     util::url_encode(a.user_name).c_str(),
                     util::url_encode(a.token).c_str(),
                     util::url_encode(a.salt).c_str());
}

http::Response api_call(const std::string& endpoint, const std::string& extra_params = "") {
    Account a = snapshot();
    if (!a.valid()) return http::Response();
    std::string url = base_rest_url(a.server) + "/" + endpoint + ".view?" + auth_query(a);
    if (!extra_params.empty()) {
        url += "&" + extra_params;
    }
    return http::get(url, {}, 20, true);
}

Artist parse_artist(json_t* a) {
    Artist art;
    art.id = json::str(a, {"id"});
    art.name = json::str(a, {"name"});
    art.album_count = (int)json::num(a, {"albumCount"});
    art.cover_art = json::str(a, {"coverArt"});
    art.artist_image_url = json::str(a, {"artistImageUrl"});
    art.starred = json::boolean(a, {"starred"}) || !json::str(a, {"starred"}).empty();
    return art;
}

Album parse_album(json_t* al) {
    Album a;
    a.id = json::str(al, {"id"});
    a.name = json::str(al, {"name"});
    if (a.name.empty()) a.name = json::str(al, {"title"});
    a.artist = json::str(al, {"artist"});
    a.artist_id = json::str(al, {"artistId"});
    a.cover_art = json::str(al, {"coverArt"});
    a.song_count = (int)json::num(al, {"songCount"});
    a.duration = json::real(al, {"duration"});
    a.year = (int)json::num(al, {"year"});
    a.genre = json::str(al, {"genre"});
    a.starred = json::boolean(al, {"starred"}) || !json::str(al, {"starred"}).empty();
    return a;
}

Song parse_song(json_t* s) {
    Song sg;
    sg.id = json::str(s, {"id"});
    sg.parent = json::str(s, {"parent"});
    sg.title = json::str(s, {"title"});
    sg.album = json::str(s, {"album"});
    sg.album_id = json::str(s, {"albumId"});
    sg.artist = json::str(s, {"artist"});
    sg.artist_id = json::str(s, {"artistId"});
    sg.track = (int)json::num(s, {"track"});
    sg.disc_number = (int)json::num(s, {"discNumber"});
    sg.year = (int)json::num(s, {"year"});
    sg.genre = json::str(s, {"genre"});
    sg.cover_art = json::str(s, {"coverArt"});
    sg.size = json::num(s, {"size"});
    sg.content_type = json::str(s, {"contentType"});
    sg.suffix = json::str(s, {"suffix"});
    sg.duration = json::real(s, {"duration"});
    sg.bit_rate = (int)json::num(s, {"bitRate"});
    sg.path = json::str(s, {"path"});
    sg.starred = json::boolean(s, {"starred"}) || !json::str(s, {"starred"}).empty();
    return sg;
}

Playlist parse_playlist(json_t* p) {
    Playlist pl;
    pl.id = json::str(p, {"id"});
    pl.name = json::str(p, {"name"});
    pl.comment = json::str(p, {"comment"});
    pl.owner = json::str(p, {"owner"});
    pl.is_public = json::boolean(p, {"public"});
    pl.song_count = (int)json::num(p, {"songCount"});
    pl.duration = json::real(p, {"duration"});
    pl.cover_art = json::str(p, {"coverArt"});
    return pl;
}

}  // namespace

Account account() { return snapshot(); }

bool signed_in() { return account().valid(); }

void sign_out() {
    {
        std::lock_guard<std::mutex> lk(g_m);
        g_acc = Account();
        save_account_fields(g_acc);
    }
    g_version++;
}

std::vector<Account> others() {
    std::vector<Account> out;
    for (const auto& m : store::saved_accounts("navidrome")) {
        Account a;
        auto it = m.find("nd_server");
        if (it != m.end()) a.server = it->second;
        it = m.find("nd_server_name");
        if (it != m.end()) a.server_name = it->second;
        it = m.find("nd_user");
        if (it != m.end()) a.user_name = it->second;
        it = m.find("nd_token");
        if (it != m.end()) a.token = it->second;
        it = m.find("nd_salt");
        if (it != m.end()) a.salt = it->second;
        if (a.valid()) out.push_back(std::move(a));
    }
    return out;
}

void switch_to(size_t i) {
    {
        std::lock_guard<std::mutex> lk(g_m);
        store::use_account("navidrome", i, true);
        g_loaded = false;
        load_account();
    }
    g_version++;
}

void add_account() {
    {
        std::lock_guard<std::mutex> lk(g_m);
        if (g_acc.valid()) store::save_account("navidrome");
        std::string s = g_acc.server;
        g_acc = Account();
        g_acc.server = s;
        save_account_fields(g_acc);
    }
    g_version++;
}

int version() { return g_version.load(); }

std::string normalize_url(const std::string& input) {
    std::string u = util::trim(input);
    while (!u.empty() && u.back() == '/') u.pop_back();
    if (u.empty()) return u;
    if (!util::starts_with(u, "http://") && !util::starts_with(u, "https://")) {
        u = "http://" + u;
        size_t host_start = 7;
        if (u.find(':', host_start) == std::string::npos) {
            size_t slash = u.find('/', host_start);
            u.insert(slash == std::string::npos ? u.size() : slash, ":4533");
        }
    }
    return u;
}

ServerInfo server_info(const std::string& url) {
    ServerInfo info;
    std::string norm = normalize_url(url);
    if (norm.empty()) {
        info.error = tr("Server address cannot be empty");
        return info;
    }
    std::string test_url = base_rest_url(norm) + "/ping.view?v=1.16.1&c=CoffeeFlix&f=json";
    http::Response r = http::get(test_url, {}, 10, true);
    if (!r.ok()) {
        info.error = r.error.empty() ? util::fmt(tr("HTTP status %ld"), r.status) : r.error;
        return info;
    }

    json::Doc doc = json::Doc::parse(r.body);
    json_t* sub = json::at(doc.get(), {"subsonic-response"});
    if (!sub) {
        info.error = tr("Not a valid Subsonic/Navidrome server");
        return info;
    }

    info.ok = true;
    info.version = json::str(sub, {"version"});
    info.name = json::str(sub, {"type"});
    if (info.name.empty()) info.name = json::str(sub, {"serverVersion"});
    if (info.name.empty()) info.name = "Navidrome";
    return info;
}

AuthResult sign_in(const std::string& url, const std::string& user, const std::string& password) {
    AuthResult res;
    std::string norm = normalize_url(url);
    if (norm.empty() || user.empty()) {
        res.error = tr("Server URL and username are required");
        return res;
    }

    std::string salt = util::random_hex(6);
    std::string token = md5::hash(password + salt);

    std::string ping_url = base_rest_url(norm) + "/ping.view?" +
                           util::fmt("u=%s&t=%s&s=%s&v=1.16.1&c=CoffeeFlix&f=json",
                                     util::url_encode(user).c_str(),
                                     util::url_encode(token).c_str(),
                                     util::url_encode(salt).c_str());

    http::Response r = http::get(ping_url, {}, 15, true);
    if (!r.ok()) {
        res.error = r.error.empty() ? util::fmt(tr("Connection failed: status %ld"), r.status) : r.error;
        return res;
    }

    json::Doc doc = json::Doc::parse(r.body);
    json_t* sub = json::at(doc.get(), {"subsonic-response"});
    if (!sub) {
        res.error = tr("Invalid response from server");
        return res;
    }

    std::string status = json::str(sub, {"status"});
    if (status != "ok") {
        std::string msg = json::str(sub, {"error", "message"});
        res.error = msg.empty() ? tr("Incorrect username or password") : msg;
        return res;
    }

    Account acc;
    acc.server = norm;
    acc.user_name = user;
    acc.token = token;
    acc.salt = salt;
    acc.server_name = json::str(sub, {"type"});
    if (acc.server_name.empty()) acc.server_name = json::str(sub, {"serverVersion"});
    if (acc.server_name.empty()) acc.server_name = "Navidrome";

    {
        std::lock_guard<std::mutex> lk(g_m);
        g_acc = acc;
        save_account_fields(g_acc);
    }
    g_version++;
    res.ok = true;
    return res;
}

List get_artists() {
    List res;
    http::Response r = api_call("getArtists");
    if (!r.ok()) {
        res.error = r.error.empty() ? util::fmt(tr("HTTP %ld"), r.status) : r.error;
        return res;
    }

    json::Doc doc = json::Doc::parse(r.body);
    json_t* sub = json::at(doc.get(), {"subsonic-response"});
    if (!sub || json::str(sub, {"status"}) != "ok") {
        res.error = json::str(sub, {"error", "message"}, tr("Error fetching artists"));
        return res;
    }

    json_t* artists_obj = json::at(sub, {"artists"});
    if (!artists_obj) return res;

    // Check artists.index[] or artists.artist[]
    json_t* index_arr = json_object_get(artists_obj, "index");
    if (json_is_array(index_arr)) {
        for (size_t i = 0; i < json::size(index_arr); i++) {
            json_t* idx = json_array_get(index_arr, i);
            json_t* list = json_object_get(idx, "artist");
            if (json_is_array(list)) {
                for (size_t j = 0; j < json::size(list); j++) {
                    res.artists.push_back(parse_artist(json_array_get(list, j)));
                }
            } else if (json_is_object(list)) {
                res.artists.push_back(parse_artist(list));
            }
        }
    } else {
        json_t* list = json_object_get(artists_obj, "artist");
        if (json_is_array(list)) {
            for (size_t j = 0; j < json::size(list); j++) {
                res.artists.push_back(parse_artist(json_array_get(list, j)));
            }
        }
    }

    res.total = (int)res.artists.size();
    res.ok = true;
    return res;
}

List get_artist(const std::string& artist_id) {
    List res;
    http::Response r = api_call("getArtist", "id=" + util::url_encode(artist_id));
    if (!r.ok()) {
        res.error = r.error.empty() ? util::fmt(tr("HTTP %ld"), r.status) : r.error;
        return res;
    }

    json::Doc doc = json::Doc::parse(r.body);
    json_t* sub = json::at(doc.get(), {"subsonic-response"});
    if (!sub || json::str(sub, {"status"}) != "ok") {
        res.error = json::str(sub, {"error", "message"}, tr("Error fetching artist"));
        return res;
    }

    json_t* art_obj = json::at(sub, {"artist"});
    if (art_obj) {
        res.artists.push_back(parse_artist(art_obj));
        json_t* al_arr = json_object_get(art_obj, "album");
        if (json_is_array(al_arr)) {
            for (size_t i = 0; i < json::size(al_arr); i++) {
                res.albums.push_back(parse_album(json_array_get(al_arr, i)));
            }
        } else if (json_is_object(al_arr)) {
            res.albums.push_back(parse_album(al_arr));
        }
    }

    res.total = (int)res.albums.size();
    res.ok = true;
    return res;
}

List get_album_list(const std::string& type, int size, int offset) {
    List res;
    std::string params = util::fmt("type=%s&size=%d&offset=%d",
                                   util::url_encode(type).c_str(), size, offset);
    http::Response r = api_call("getAlbumList2", params);
    if (!r.ok()) {
        res.error = r.error.empty() ? util::fmt(tr("HTTP %ld"), r.status) : r.error;
        return res;
    }

    json::Doc doc = json::Doc::parse(r.body);
    json_t* sub = json::at(doc.get(), {"subsonic-response"});
    if (!sub || json::str(sub, {"status"}) != "ok") {
        res.error = json::str(sub, {"error", "message"}, tr("Error fetching album list"));
        return res;
    }

    json_t* list = json::at(sub, {"albumList2", "album"});
    if (!list) list = json::at(sub, {"albumList", "album"});
    if (json_is_array(list)) {
        for (size_t i = 0; i < json::size(list); i++) {
            res.albums.push_back(parse_album(json_array_get(list, i)));
        }
    } else if (json_is_object(list)) {
        res.albums.push_back(parse_album(list));
    }

    res.total = (int)res.albums.size();
    res.ok = true;
    return res;
}

List get_album(const std::string& album_id) {
    List res;
    http::Response r = api_call("getAlbum", "id=" + util::url_encode(album_id));
    if (!r.ok()) {
        res.error = r.error.empty() ? util::fmt(tr("HTTP %ld"), r.status) : r.error;
        return res;
    }

    json::Doc doc = json::Doc::parse(r.body);
    json_t* sub = json::at(doc.get(), {"subsonic-response"});
    if (!sub || json::str(sub, {"status"}) != "ok") {
        res.error = json::str(sub, {"error", "message"}, tr("Error fetching album"));
        return res;
    }

    json_t* al_obj = json::at(sub, {"album"});
    if (al_obj) {
        res.albums.push_back(parse_album(al_obj));
        json_t* song_arr = json_object_get(al_obj, "song");
        if (json_is_array(song_arr)) {
            for (size_t i = 0; i < json::size(song_arr); i++) {
                res.songs.push_back(parse_song(json_array_get(song_arr, i)));
            }
        } else if (json_is_object(song_arr)) {
            res.songs.push_back(parse_song(song_arr));
        }
    }

    res.total = (int)res.songs.size();
    res.ok = true;
    return res;
}

List get_playlists() {
    List res;
    http::Response r = api_call("getPlaylists");
    if (!r.ok()) {
        res.error = r.error.empty() ? util::fmt(tr("HTTP %ld"), r.status) : r.error;
        return res;
    }

    json::Doc doc = json::Doc::parse(r.body);
    json_t* sub = json::at(doc.get(), {"subsonic-response"});
    if (!sub || json::str(sub, {"status"}) != "ok") {
        res.error = json::str(sub, {"error", "message"}, tr("Error fetching playlists"));
        return res;
    }

    json_t* pl_arr = json::at(sub, {"playlists", "playlist"});
    if (json_is_array(pl_arr)) {
        for (size_t i = 0; i < json::size(pl_arr); i++) {
            res.playlists.push_back(parse_playlist(json_array_get(pl_arr, i)));
        }
    } else if (json_is_object(pl_arr)) {
        res.playlists.push_back(parse_playlist(pl_arr));
    }

    res.total = (int)res.playlists.size();
    res.ok = true;
    return res;
}

List get_playlist(const std::string& playlist_id) {
    List res;
    http::Response r = api_call("getPlaylist", "id=" + util::url_encode(playlist_id));
    if (!r.ok()) {
        res.error = r.error.empty() ? util::fmt(tr("HTTP %ld"), r.status) : r.error;
        return res;
    }

    json::Doc doc = json::Doc::parse(r.body);
    json_t* sub = json::at(doc.get(), {"subsonic-response"});
    if (!sub || json::str(sub, {"status"}) != "ok") {
        res.error = json::str(sub, {"error", "message"}, tr("Error fetching playlist"));
        return res;
    }

    json_t* pl_obj = json::at(sub, {"playlist"});
    if (pl_obj) {
        res.playlists.push_back(parse_playlist(pl_obj));
        json_t* entry_arr = json_object_get(pl_obj, "entry");
        if (json_is_array(entry_arr)) {
            for (size_t i = 0; i < json::size(entry_arr); i++) {
                res.songs.push_back(parse_song(json_array_get(entry_arr, i)));
            }
        } else if (json_is_object(entry_arr)) {
            res.songs.push_back(parse_song(entry_arr));
        }
    }

    res.total = (int)res.songs.size();
    res.ok = true;
    return res;
}

List search(const std::string& term, int count) {
    List res;
    std::string params = util::fmt("query=%s&artistCount=%d&albumCount=%d&songCount=%d",
                                   util::url_encode(term).c_str(), count, count, count);
    http::Response r = api_call("search3", params);
    if (!r.ok()) {
        res.error = r.error.empty() ? util::fmt(tr("HTTP %ld"), r.status) : r.error;
        return res;
    }

    json::Doc doc = json::Doc::parse(r.body);
    json_t* sub = json::at(doc.get(), {"subsonic-response"});
    if (!sub || json::str(sub, {"status"}) != "ok") {
        res.error = json::str(sub, {"error", "message"}, tr("Search error"));
        return res;
    }

    json_t* s3 = json::at(sub, {"searchResult3"});
    if (s3) {
        json_t* art = json_object_get(s3, "artist");
        if (json_is_array(art)) {
            for (size_t i = 0; i < json::size(art); i++) res.artists.push_back(parse_artist(json_array_get(art, i)));
        } else if (json_is_object(art)) res.artists.push_back(parse_artist(art));

        json_t* alb = json_object_get(s3, "album");
        if (json_is_array(alb)) {
            for (size_t i = 0; i < json::size(alb); i++) res.albums.push_back(parse_album(json_array_get(alb, i)));
        } else if (json_is_object(alb)) res.albums.push_back(parse_album(alb));

        json_t* sng = json_object_get(s3, "song");
        if (json_is_array(sng)) {
            for (size_t i = 0; i < json::size(sng); i++) res.songs.push_back(parse_song(json_array_get(sng, i)));
        } else if (json_is_object(sng)) res.songs.push_back(parse_song(sng));
    }

    res.total = (int)(res.artists.size() + res.albums.size() + res.songs.size());
    res.ok = true;
    return res;
}

void scrobble(const std::string& song_id, bool submission) {
    if (song_id.empty()) return;
    tasks::submit(tasks::API, [song_id, submission]() -> std::function<void()> {
        std::string p = util::fmt("id=%s&submission=%s",
                                  util::url_encode(song_id).c_str(),
                                  submission ? "true" : "false");
        api_call("scrobble", p);
        return nullptr;
    });
}

void star(const std::string& id, bool is_album, bool is_artist) {
    if (id.empty()) return;
    tasks::submit(tasks::API, [id, is_album, is_artist]() -> std::function<void()> {
        std::string key = is_artist ? "artistId=" : is_album ? "albumId=" : "id=";
        api_call("star", key + util::url_encode(id));
        return nullptr;
    });
}

void unstar(const std::string& id, bool is_album, bool is_artist) {
    if (id.empty()) return;
    tasks::submit(tasks::API, [id, is_album, is_artist]() -> std::function<void()> {
        std::string key = is_artist ? "artistId=" : is_album ? "albumId=" : "id=";
        api_call("unstar", key + util::url_encode(id));
        return nullptr;
    });
}

std::string cover_art_url(const std::string& cover_id, int size) {
    Account a = snapshot();
    if (!a.valid() || cover_id.empty()) return "";
    return base_rest_url(a.server) + "/getCoverArt.view?" + auth_query(a) +
           util::fmt("&id=%s&size=%d", util::url_encode(cover_id).c_str(), size);
}

std::string stream_url(const std::string& song_id) {
    Account a = snapshot();
    if (!a.valid() || song_id.empty()) return "";
    return base_rest_url(a.server) + "/stream.view?" + auth_query(a) +
           "&id=" + util::url_encode(song_id);
}

player::Source make_source(const Song& song) {
    player::Source s;
    s.title = song.title.empty() ? tr("Unknown Track") : song.title;
    s.subtitle = song.artist.empty() ? song.album : song.artist;
    if (!song.album.empty() && !song.artist.empty()) {
        s.subtitle = song.artist + " \xC2\xB7 " + song.album;
    }
    if (!song.cover_art.empty()) {
        s.artwork = cover_art_url(song.cover_art, 600);
    } else if (!song.album_id.empty()) {
        s.artwork = cover_art_url(song.album_id, 600);
    }
    s.service = "navidrome";
    s.id = song.id;
    s.extra = song.album_id;
    s.remember_position = false;
    s.start = 0;
    s.chunked_http = false;
    s.url = stream_url(song.id);

    // Scrobble now playing notification in background
    scrobble(song.id, false);

    return s;
}

}  // namespace navidrome
