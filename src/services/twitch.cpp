#include "services/twitch.hpp"

#include <algorithm>
#include <cstdlib>

#include "core/http.hpp"
#include "core/json.hpp"
#include "core/store.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"
#include "player/player.hpp"
#include "services/hls.hpp"

namespace twitch {

namespace {

// Twitch's own web client id (public; used by every third-party player).
const char* CLIENT_ID = "kimne78kx3ncx6brgo4mv6wki5h1ko";
const char* STREAM_FIELDS =
    "title viewersCount previewImageURL(width: 440, height: 248) "
    "broadcaster { login displayName profileImageURL(width: 150) } game { name }";
const char* USER_FIELDS =
    "login displayName profileImageURL(width: 150) "
    "stream { title viewersCount previewImageURL(width: 440, height: 248) game { name } }";

std::string device_id() {
    std::string id = store::get_str("twitch_device_id");
    if (id.empty()) {
        id = util::random_hex(16);
        store::set_str("twitch_device_id", id);
    }
    return id;
}

json::Doc gql(const std::string& query, const std::string& variables_json, std::string& error) {
    json_t* body = json_object();
    json_object_set_new(body, "query", json_string(query.c_str()));
    if (!variables_json.empty()) {
        json_error_t e;
        json_t* vars = json_loads(variables_json.c_str(), 0, &e);
        if (vars) json_object_set_new(body, "variables", vars);
    }
    std::string payload = json::dump(body);
    json_decref(body);
    http::Response r = http::post_json(util::env_or("COFFEEFLIX_TWITCH_GQL", "https://gql.twitch.tv/gql"), payload,
                                       {{"Client-ID", CLIENT_ID}, {"X-Device-Id", device_id()}}, 15);
    if (!r.ok()) {
        error = r.error;
        return json::Doc();
    }
    json::Doc doc = json::Doc::parse(r.body);
    if (!doc) {
        error = "Unexpected response from Twitch";
    } else if (json_t* errs = json_object_get(doc.get(), "errors"); json::size(errs) > 0 && !json_object_get(doc.get(), "data")) {
        error = json::str(errs, {0, "message"}, "Twitch request failed");
        return json::Doc();
    }
    return doc;
}

std::string gql_string(const std::string& s) {
    json_t* j = json_string(s.c_str());
    std::string out = json::dump(j);
    json_decref(j);
    return out;
}

Stream parse_stream(json_t* node) {
    Stream s;
    s.title = json::str(node, {"title"});
    s.viewers = (int)json::num(node, {"viewersCount"});
    s.preview = json::str(node, {"previewImageURL"});
    s.login = json::str(node, {"broadcaster", "login"});
    s.name = json::str(node, {"broadcaster", "displayName"});
    s.avatar = json::str(node, {"broadcaster", "profileImageURL"});
    s.game = json::str(node, {"game", "name"});
    s.live = true;
    return s;
}

Stream parse_user(json_t* u) {
    Stream s;
    s.login = json::str(u, {"login"});
    s.name = json::str(u, {"displayName"});
    s.avatar = json::str(u, {"profileImageURL"});
    json_t* st = json_object_get(u, "stream");
    if (json_is_object(st)) {
        s.live = true;
        s.title = json::str(st, {"title"});
        s.viewers = (int)json::num(st, {"viewersCount"});
        s.preview = json::str(st, {"previewImageURL"});
        s.game = json::str(st, {"game", "name"});
    }
    return s;
}

Streams edges_to_streams(json_t* edges) {
    Streams out;
    for (size_t i = 0; i < json::size(edges); i++) {
        Stream s = parse_stream(json::at(json_array_get(edges, i), {"node"}));
        if (!s.login.empty()) out.items.push_back(std::move(s));
    }
    out.ok = true;
    return out;
}

bool resolve(const std::string& login, player::Source& src, std::string& error) {
    const char* q =
        "query PlaybackAccessToken($login: String!, $playerType: String!) {"
        " streamPlaybackAccessToken(channelName: $login, params: {platform: \"web\", playerBackend: \"mediaplayer\", playerType: $playerType})"
        " { value signature } }";
    std::string vars = "{\"login\":" + gql_string(login) + ",\"playerType\":\"site\"}";
    json::Doc doc = gql(q, vars, error);
    if (!doc) return false;
    json_t* tok = json::at(doc.get(), {"data", "streamPlaybackAccessToken"});
    std::string value = json::str(tok, {"value"}), sig = json::str(tok, {"signature"});
    if (value.empty() || sig.empty()) {
        error = "This channel is offline";
        return false;
    }
    std::string usher = util::fmt(
        "%s/api/channel/hls/%s.m3u8?sig=%s&token=%s&allow_source=true&allow_audio_only=true"
        "&fast_bread=true&playlist_include_framerate=true&player_backend=mediaplayer&supported_codecs=avc1&p=%d",
        util::env_or("COFFEEFLIX_TWITCH_USHER", "https://usher.ttvnw.net").c_str(), util::lower(login).c_str(), sig.c_str(), util::url_encode(value).c_str(), rand() % 999999);
    http::Response r = http::get(usher, {}, 15);
    if (r.status == 404) {
        error = "This channel is offline";
        return false;
    }
    if (!r.ok()) {
        error = r.error;
        return false;
    }
    hls::Master m = hls::parse(r.body, usher);
    int max_h = (int)store::get_int("twitch_quality", 720);
    float max_fps = store::get_bool("allow_60fps", false) ? 61.0f : 31.0f;
    const hls::Variant* v = hls::pick(m, max_h, true, max_fps);
    if (!v) {
        error = "No compatible stream quality";
        return false;
    }
    log_message(LOG_OK, "Twitch", "%s: %dp%.0f (%d kbps)", login.c_str(), v->height, v->fps, v->bandwidth / 1000);
    src.url = v->url;
    return true;
}

}  // namespace

Streams top_streams(int count) {
    std::string err;
    json::Doc doc = gql(util::fmt("query { streams(first: %d) { edges { node { %s } } } }", count, STREAM_FIELDS), "", err);
    if (!doc) {
        Streams s;
        s.error = err;
        return s;
    }
    return edges_to_streams(json::at(doc.get(), {"data", "streams", "edges"}));
}

Streams game_streams(const std::string& game, int count) {
    std::string err;
    json::Doc doc = gql(util::fmt("query { game(name: %s) { streams(first: %d) { edges { node { %s } } } } }",
                                  gql_string(game).c_str(), count, STREAM_FIELDS), "", err);
    if (!doc) {
        Streams s;
        s.error = err;
        return s;
    }
    return edges_to_streams(json::at(doc.get(), {"data", "game", "streams", "edges"}));
}

Categories top_categories(int count) {
    Categories out;
    std::string err;
    json::Doc doc = gql(util::fmt("query { games(first: %d) { edges { node { name viewersCount boxArtURL(width: 285, height: 380) } } } }",
                                  count), "", err);
    if (!doc) {
        out.error = err;
        return out;
    }
    json_t* edges = json::at(doc.get(), {"data", "games", "edges"});
    for (size_t i = 0; i < json::size(edges); i++) {
        json_t* n = json::at(json_array_get(edges, i), {"node"});
        Category c;
        c.name = json::str(n, {"name"});
        c.viewers = (int)json::num(n, {"viewersCount"});
        c.box_art = json::str(n, {"boxArtURL"});
        if (!c.name.empty()) out.items.push_back(std::move(c));
    }
    out.ok = true;
    return out;
}

Streams search(const std::string& query) {
    Streams out;
    std::string err;
    // Exact channel name first, then fuzzy matches.
    std::string login;
    for (char c : query)
        if (isalnum((unsigned char)c) || c == '_') login += (char)tolower((unsigned char)c);
    if (!login.empty()) {
        json::Doc doc = gql(util::fmt("query { user(login: %s) { %s } }", gql_string(login).c_str(), USER_FIELDS), "", err);
        json_t* u = json::at(doc.get(), {"data", "user"});
        if (json_is_object(u)) out.items.push_back(parse_user(u));
    }
    json::Doc doc = gql(util::fmt("query { searchUsers(userQuery: %s, first: 25) { edges { node { %s } } } }",
                                  gql_string(query).c_str(), USER_FIELDS), "", err);
    json_t* edges = json::at(doc.get(), {"data", "searchUsers", "edges"});
    for (size_t i = 0; i < json::size(edges); i++) {
        Stream s = parse_user(json::at(json_array_get(edges, i), {"node"}));
        bool dup = std::any_of(out.items.begin(), out.items.end(), [&](const Stream& o) { return o.login == s.login; });
        if (!s.login.empty() && !dup) out.items.push_back(std::move(s));
    }
    // Live channels first.
    std::stable_sort(out.items.begin(), out.items.end(), [](const Stream& a, const Stream& b) { return a.live > b.live; });
    out.ok = !out.items.empty() || err.empty();
    if (!out.ok) out.error = err;
    return out;
}

Streams followed() {
    Streams out;
    auto favs = store::favs("twitch");
    if (favs.empty()) {
        out.ok = true;
        return out;
    }
    std::string logins;
    for (auto& f : favs) logins += (logins.empty() ? "" : ",") + gql_string(f.id);
    std::string err;
    json::Doc doc = gql(util::fmt("query { users(logins: [%s]) { %s } }", logins.c_str(), USER_FIELDS), "", err);
    if (!doc) {
        out.error = err;
        return out;
    }
    json_t* users = json::at(doc.get(), {"data", "users"});
    for (size_t i = 0; i < json::size(users); i++) {
        json_t* u = json_array_get(users, i);
        if (json_is_object(u)) out.items.push_back(parse_user(u));
    }
    std::stable_sort(out.items.begin(), out.items.end(), [](const Stream& a, const Stream& b) {
        return a.live != b.live ? a.live > b.live : a.viewers > b.viewers;
    });
    out.ok = true;
    return out;
}

bool is_followed(const std::string& login) { return store::fav_has("twitch", login); }

bool toggle_follow(const Stream& s) { return store::fav_toggle("twitch", store::Fav{s.login, s.name, s.game, s.avatar, ""}); }

player::Source make_source(const Stream& s) {
    player::Source src;
    src.title = s.title.empty() ? s.name : s.title;
    src.subtitle = s.game.empty() ? s.name : s.name + " \xC2\xB7 " + s.game;
    src.artwork = s.preview;
    src.live = true;
    src.service = "twitch";
    src.id = s.login;
    src.remember_position = false;
    std::string login = s.login;
    src.resolve = [login](player::Source& out, std::string& err) { return resolve(login, out, err); };
    return src;
}

}  // namespace twitch
